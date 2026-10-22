/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ReplayLink.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrlQuery>

static constexpr int kMaxLinkLength = 2048;

// one server per user, so links reach this user's RMG-K
static QString serverName(void)
{
    const QByteArray user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")).toUtf8();
    return "RMG-K-replay-links-" +
        QString::fromLatin1(QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex().left(12));
}

bool ReplayLink::IsLink(const QString& argument)
{
    return argument.startsWith("rmgk:", Qt::CaseInsensitive);
}

bool ReplayLink::Parse(const QString& link, QUrl& replayUrl, qint64& setId, int& record, QString& error)
{
    const QUrl url(link, QUrl::StrictMode);
    if (link.size() > kMaxLinkLength || !url.isValid() || url.scheme().compare("rmgk", Qt::CaseInsensitive) != 0 ||
        url.host().compare("replay", Qt::CaseInsensitive) != 0)
    {
        error = QCoreApplication::translate("ReplayLink", "This isn't an RMG-K replay link.");
        return false;
    }

    const QUrlQuery query(url);
    record = 0;
    if (query.hasQueryItem("record"))
    {
        static const QRegularExpression digits("^[0-9]{1,8}$");
        const QString value = query.queryItemValue("record");
        if (!digits.match(value).hasMatch())
        {
            error = QCoreApplication::translate("ReplayLink", "The replay link's start position isn't valid.");
            return false;
        }
        record = value.toInt();
    }
    replayUrl = QUrl(query.queryItemValue("url", QUrl::FullyDecoded), QUrl::StrictMode);
    static const QRegularExpression replayPath("^/api/ranked/sets/([0-9]{1,15})/replay$");
    const QRegularExpressionMatch match = replayPath.match(replayUrl.path());
    if (!replayUrl.isValid() || (replayUrl.scheme() != "https" && replayUrl.scheme() != "http") ||
        replayUrl.host().isEmpty() || !replayUrl.userInfo().isEmpty() || replayUrl.hasQuery() ||
        replayUrl.hasFragment() || !match.hasMatch())
    {
        error = QCoreApplication::translate("ReplayLink", "The replay link doesn't point at a ranked replay.");
        return false;
    }
    setId = match.captured(1).toLongLong();
    return setId > 0;
}

bool ReplayLink::ForwardToRunningInstance(const QString& link)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(500))
    {
        return false;
    }
    socket.write(link.left(kMaxLinkLength).toUtf8());
    socket.write("\n");
    const bool written = socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return written;
}

// one Exec= argument: quoted, with the characters the desktop entry spec
// reserves inside quotes escaped, then escaped again as a key file string
static QString desktopExecArgument(const QString& argument)
{
    QString quoted = "\"";
    for (const QChar c : argument)
    {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
        {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    quoted.replace("\\", "\\\\");
    quoted.replace("%", "%%");
    return quoted;
}

QString ReplayLink::DesktopEntry(const QString& executable)
{
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=RMG-K (replay links)\n"
           "Comment=Opens rmgk:// ranked replay links from the rankings site\n"
           "Exec=" + desktopExecArgument(executable) + " %u\n"
           "Terminal=false\n"
           "NoDisplay=true\n"
           "MimeType=x-scheme-handler/rmgk;\n";
}

void ReplayLink::RegisterHandler(void)
{
#if defined(__linux__)
#ifndef PORTABLE_INSTALL
    // system packages install the desktop entry; only AppImages register here
    if (qEnvironmentVariableIsEmpty("APPIMAGE"))
    {
        return;
    }
#endif
    // an AppImage runs from a temporary mount: link to the image itself
    const QString executable = qEnvironmentVariable("APPIMAGE", QCoreApplication::applicationFilePath());
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    const QString name = "rmg-k-replay-link.desktop";
    const QByteArray entry = DesktopEntry(executable).toUtf8();
    QFile file(directory + "/" + name);
    if (file.open(QIODevice::ReadOnly) && file.readAll() == entry)
    {
        return;
    }
    file.close();
    if (directory.isEmpty() || !QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(entry) != entry.size())
    {
        return;
    }
    file.close();
    // make it the default for the scheme (both tools are optional)
    QProcess::startDetached("xdg-mime", {"default", name, "x-scheme-handler/rmgk"});
    QProcess::startDetached("update-desktop-database", {directory});
#endif
#ifdef _WIN32
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString command = QString("\"%1\" \"%2\"").arg(exe, "%1");
    QSettings classes("HKEY_CURRENT_USER\\Software\\Classes\\rmgk", QSettings::NativeFormat);
    if (classes.value("shell/open/command/Default").toString() == command)
    {
        return;
    }
    classes.setValue("Default", "URL:RMG-K replay");
    classes.setValue("URL Protocol", "");
    classes.setValue("shell/open/command/Default", command);
#endif
}

ReplayLink::Server::Server(QObject* parent) : QObject(parent)
{
}

bool ReplayLink::Server::Listen(void)
{
    this->server = new QLocalServer(this);
    this->server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!this->server->listen(serverName()))
    {
        // a crashed instance can leave a stale socket behind (not on Windows)
        QLocalServer::removeServer(serverName());
        if (!this->server->listen(serverName()))
        {
            return false;
        }
    }

    connect(this->server, &QLocalServer::newConnection, this, [this]()
    {
        while (QLocalSocket* socket = this->server->nextPendingConnection())
        {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]()
            {
                if (!socket->canReadLine())
                {
                    if (socket->bytesAvailable() > kMaxLinkLength)
                    {
                        socket->abort();
                    }
                    return;
                }
                const QString link = QString::fromUtf8(socket->readLine(kMaxLinkLength + 2)).trimmed();
                socket->disconnectFromServer();
                if (ReplayLink::IsLink(link))
                {
                    emit this->linkReceived(link);
                }
            });
        }
    });
    return true;
}
