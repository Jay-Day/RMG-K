/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef PASSWORDRESETEMAIL_HPP
#define PASSWORDRESETEMAIL_HPP

#ifdef NETPLAY

#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace Utilities
{

// Emailed password reset codes for the rollback lobby. A lobby with email
// configured can email an account's owner a one-time reset code; without
// email it answers 404, and resets need a moderator's code.
class PasswordResetEmail : public QObject
{
    Q_OBJECT

public:
    explicit PasswordResetEmail(QObject* parent = nullptr);

    // Asks the lobby whether it can email reset codes (availabilityKnown).
    void checkAvailable(const QString& wsUrl);
    // Asks the lobby to email username's owner a code (requestFinished). The
    // lobby only sends it when email is the account's confirmed address.
    void request(const QString& wsUrl, const QString& username, const QString& email);
    // How long an emailed code works, from the lobby.
    int codeMinutes(void) const { return m_codeMinutes; }

signals:
    void availabilityKnown(bool available);
    // ok: the lobby took the request. It answers the same whether or not the
    // account exists or the address matches. Otherwise message says why not.
    void requestFinished(bool ok, const QString& message);

private:
    QNetworkAccessManager* m_network = nullptr;
    int m_codeMinutes = 60;
};

} // namespace Utilities

#endif // NETPLAY
#endif // PASSWORDRESETEMAIL_HPP
