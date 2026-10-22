/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifdef NETPLAY

#include "DiscordLogin.hpp"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

using namespace Utilities;

namespace
{
// how long the browser has to come back
constexpr int kSignInTimeoutMs = 3 * 60 * 1000;

QString randomToken(int bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(data.data()), bytes / 4);
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
} // namespace

DiscordLogin::DiscordLogin(QObject* parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        finish();
        emit failed(tr("Discord sign-in timed out. Try again."));
    });
}

DiscordLogin::~DiscordLogin() = default;

QUrl DiscordLogin::HttpBase(const QString& wsUrl)
{
    QUrl url(wsUrl);
    url.setScheme(url.scheme() == "wss" ? "https" : "http");
    url.setPath(QString());
    url.setQuery(QString());
    return url;
}

void DiscordLogin::fetchConfig(const QString& wsUrl, std::function<void(bool, const QJsonObject&)> done)
{
    QUrl url = HttpBase(wsUrl);
    url.setPath("/api/auth/discord");
    QNetworkRequest request(url);
    request.setTransferTimeout(8000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject config = QJsonDocument::fromJson(reply->readAll()).object();
        done(reply->error() == QNetworkReply::NoError && status == 200 && !config.value("clientId").toString().isEmpty(), config);
    });
}

void DiscordLogin::checkAvailable(const QString& wsUrl)
{
    fetchConfig(wsUrl, [this](bool ok, const QJsonObject&) { emit availabilityKnown(ok); });
}

void DiscordLogin::start(const QString& wsUrl)
{
    if (m_running)
    {
        return;
    }
    m_running = true;
    fetchConfig(wsUrl, [this](bool ok, const QJsonObject& config) {
        if (!m_running)
        {
            return; // cancelled meanwhile
        }
        if (!ok)
        {
            m_running = false;
            emit failed(tr("This lobby doesn't support Discord sign-in."));
            return;
        }
        begin(config);
    });
}

void DiscordLogin::begin(const QJsonObject& config)
{
    // listen on the first registered loopback redirect that's free
    m_server = new QTcpServer(this);
    for (const QJsonValue& value : config.value("redirectUris").toArray())
    {
        const QUrl uri(value.toString());
        if (uri.port() > 0 && m_server->listen(QHostAddress::LocalHost, quint16(uri.port())))
        {
            m_redirectUri  = value.toString();
            m_redirectPath = uri.path().isEmpty() ? QStringLiteral("/") : uri.path();
            break;
        }
    }
    if (!m_server->isListening())
    {
        finish();
        emit failed(tr("Discord sign-in couldn't start: its local ports are in use."));
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &DiscordLogin::onConnection);

    // PKCE: the lobby proves it holds the verifier when it exchanges the code
    m_state    = randomToken(24);
    m_verifier = randomToken(48);
    m_challenge = QString::fromLatin1(QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    m_config      = config;
    m_skipConsent = true;

    m_timeout.start(kSignInTimeoutMs);
    if (!QDesktopServices::openUrl(authorizeUrl(true)))
    {
        finish();
        emit failed(tr("Couldn't open your browser for Discord sign-in."));
    }
}

QUrl DiscordLogin::authorizeUrl(bool skipConsent) const
{
    QUrl authorize(m_config.value("authorizeUrl").toString());
    QUrlQuery query;
    query.addQueryItem("client_id", m_config.value("clientId").toString());
    query.addQueryItem("response_type", "code");
    query.addQueryItem("redirect_uri", m_redirectUri);
    query.addQueryItem("scope", m_config.value("scope").toString("identify"));
    query.addQueryItem("state", m_state);
    query.addQueryItem("code_challenge", m_challenge);
    query.addQueryItem("code_challenge_method", "S256");
    // Skips Discord's consent screen once this app has been authorized.
    // Discord shows the screen anyway the first time; should it answer with
    // an error instead, the callback retries without it.
    if (skipConsent)
        query.addQueryItem("prompt", "none");
    authorize.setQuery(query);
    return authorize;
}

void DiscordLogin::onConnection(void)
{
    while (m_server != nullptr && m_server->hasPendingConnections())
    {
        QTcpSocket* socket = m_server->nextPendingConnection();
        // outlives the server, which closes once the code arrives, so the
        // browser still gets its page
        socket->setParent(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray request = socket->property("request").toByteArray() + socket->readAll();
            if (request.size() > 16384)
            {
                socket->abort();
                return;
            }
            socket->setProperty("request", request);
            if (!request.contains("\r\n\r\n") && !request.contains("\n\n"))
            {
                return; // wait for the whole request head
            }
            // "GET /callback?code=...&state=... HTTP/1.1"
            const QList<QByteArray> line = request.left(request.indexOf('\n')).trimmed().split(' ');
            const QUrl target(QString::fromLatin1(line.value(1)));
            if (line.value(0) != "GET" || target.path() != m_redirectPath)
            {
                answer(socket, 404, tr("Not found"), QString());
                return;
            }
            const QUrlQuery query(target);
            if (query.queryItemValue("state") != m_state)
            {
                answer(socket, 400, tr("Sign-in failed"), tr("This sign-in doesn't match the one RMG-K started. Try again from RMG-K."));
                return;
            }
            const QString error = query.queryItemValue("error");
            if (!error.isEmpty() && error != "access_denied" && m_skipConsent)
            {
                // the consent skip was refused: ask again with the screen
                m_skipConsent = false;
                const QByteArray location = authorizeUrl(false).toEncoded();
                socket->write("HTTP/1.1 302 Found\r\nLocation: " + location +
                              "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (query.hasQueryItem("error") || !query.hasQueryItem("code"))
            {
                answer(socket, 200, tr("Sign-in cancelled"), tr("Discord sign-in was cancelled. You can close this tab."));
                finish();
                emit failed(tr("Discord sign-in was cancelled."));
                return;
            }
            const QString code = query.queryItemValue("code", QUrl::FullyDecoded);
            answer(socket, 200, tr("Signed in with Discord"), tr("You can close this tab and return to RMG-K."));
            const QString verifier = m_verifier, redirect = m_redirectUri;
            finish();
            emit codeReceived(code, verifier, redirect);
        });
    }
}

void DiscordLogin::answer(QTcpSocket* socket, int status, const QString& title, const QString& text)
{
    const QString body = QString("<!doctype html><html><head><meta charset=\"utf-8\"><title>%1</title>"
        "<style>body{font:16px system-ui,sans-serif;background:#16131f;color:#f4efe3;display:grid;place-items:center;"
        "min-height:90vh;margin:0}main{text-align:center}h1{color:#f6c22d}</style></head>"
        "<body><main><h1>%1</h1><p>%2</p></main></body></html>").arg(title.toHtmlEscaped(), text.toHtmlEscaped());
    const QByteArray bytes = body.toUtf8();
    const QByteArray reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found";
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n"
                  "Content-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    socket->disconnectFromHost();
}

void DiscordLogin::cancel(void)
{
    finish();
}

void DiscordLogin::finish(void)
{
    m_running = false;
    m_timeout.stop();
    if (m_server != nullptr)
    {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_state.clear();
}

#endif // NETPLAY
