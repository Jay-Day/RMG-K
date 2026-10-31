/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ACCOUNTEMAILDIALOG_HPP
#define ACCOUNTEMAILDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace UserInterface
{
namespace Dialog
{

class LobbyClient;

// Adds, changes or removes the lobby account's email address, the address a
// forgotten-password code can be emailed to. A new address takes effect once
// the code emailed to it is entered here.
class AccountEmailDialog : public QDialog
{
    Q_OBJECT

public:
    AccountEmailDialog(LobbyClient* client, QWidget* parent = nullptr);

private:
    void sendCode(void);
    void confirmCode(void);
    void removeEmail(void);
    void showCurrent(void);
    void setStatus(const QString& text, bool error);

    LobbyClient* m_client = nullptr;
    QLabel*      m_currentLabel  = nullptr;
    QPushButton* m_removeButton  = nullptr;
    QLabel*      m_addressLabel  = nullptr;
    QLineEdit*   m_addressEdit   = nullptr;
    QPushButton* m_sendButton    = nullptr;
    QWidget*     m_codeRow       = nullptr;
    QLabel*      m_sentLabel     = nullptr;
    QLineEdit*   m_codeEdit      = nullptr;
    QPushButton* m_confirmButton = nullptr;
    QLabel*      m_statusLabel   = nullptr;
    QString      m_pendingAddress;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // ACCOUNTEMAILDIALOG_HPP
