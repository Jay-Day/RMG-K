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

#include "PasswordResetEmail.hpp"
#include "DiscordLogin.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

using namespace Utilities;

PasswordResetEmail::PasswordResetEmail(QObject* parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

void PasswordResetEmail::checkAvailable(const QString& wsUrl)
{
    QUrl url = DiscordLogin::HttpBase(wsUrl);
    url.setPath("/api/auth/email");
    QNetworkRequest request(url);
    request.setTransferTimeout(8000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject config = QJsonDocument::fromJson(reply->readAll()).object();
        const bool available = reply->error() == QNetworkReply::NoError && status == 200 &&
                               config.value("passwordReset").toBool();
        if (available && config.value("codeMinutes").toInt() > 0)
        {
            m_codeMinutes = config.value("codeMinutes").toInt();
        }
        emit availabilityKnown(available);
    });
}

void PasswordResetEmail::request(const QString& wsUrl, const QString& username, const QString& email)
{
    QUrl url = DiscordLogin::HttpBase(wsUrl);
    url.setPath("/api/auth/password-reset");
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject body;
    body["username"] = username;
    body["email"]    = email;
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 202)
        {
            emit requestFinished(true, QString());
            return;
        }
        if (status == 429)
        {
            const int seconds = reply->rawHeader("Retry-After").toInt();
            const int minutes = qMax(1, (seconds + 59) / 60);
            emit requestFinished(false, minutes == 1
                ? tr("Too many reset requests from your network. Try again in 1 minute.")
                : tr("Too many reset requests from your network. Try again in %1 minutes.").arg(minutes));
            return;
        }
        if (status == 404)
        {
            emit requestFinished(false, tr("This lobby can't send email. Ask a moderator for a reset code."));
            return;
        }
        if (status == 0 || (reply->error() != QNetworkReply::NoError && status < 400))
        {
            emit requestFinished(false, tr("Couldn't reach the lobby: %1").arg(reply->errorString()));
            return;
        }
        const QString error = QJsonDocument::fromJson(reply->readAll()).object().value("error").toString();
        emit requestFinished(false, error.isEmpty() ? tr("The lobby couldn't send a reset code.") : error);
    });
}

#endif // NETPLAY
