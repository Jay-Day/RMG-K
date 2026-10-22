/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef DISCORDLOGIN_HPP
#define DISCORDLOGIN_HPP

#ifdef NETPLAY

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

namespace Utilities
{

// Discord sign-in for the rollback lobby: an easier way into an account that
// keeps its password. Opens Discord's authorize page in the browser and
// receives the redirect on a loopback port (OAuth2 authorization code with
// PKCE); the lobby exchanges the code with its client secret.
class DiscordLogin : public QObject
{
    Q_OBJECT

public:
    explicit DiscordLogin(QObject* parent = nullptr);
    ~DiscordLogin() override;

    // the lobby's web address for its ws:// or wss:// URL
    static QUrl HttpBase(const QString& wsUrl);

    // Asks the lobby whether it supports Discord sign-in (availabilityKnown).
    void checkAvailable(const QString& wsUrl);
    // Starts a sign-in in the browser; codeReceived or failed follows.
    void start(const QString& wsUrl);
    void cancel(void);
    bool isRunning(void) const { return m_running; }

signals:
    void availabilityKnown(bool available);
    void codeReceived(const QString& code, const QString& codeVerifier, const QString& redirectUri);
    void failed(const QString& message);

private:
    void fetchConfig(const QString& wsUrl, std::function<void(bool ok, const QJsonObject& config)> done);
    void begin(const QJsonObject& config);
    QUrl authorizeUrl(bool skipConsent) const;
    void onConnection(void);
    void answer(QTcpSocket* socket, int status, const QString& title, const QString& text);
    void finish(void);

    QNetworkAccessManager* m_network = nullptr;
    QTcpServer* m_server  = nullptr;
    QTimer      m_timeout;
    bool        m_running = false;
    QString     m_state;
    QString     m_verifier;
    QString     m_redirectUri;
    QString     m_redirectPath;
    QJsonObject m_config;
    QString     m_challenge;
    bool        m_skipConsent = true;
};

} // namespace Utilities

#endif // NETPLAY
#endif // DISCORDLOGIN_HPP
