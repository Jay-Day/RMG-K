/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef LOBBYCONNECTDIALOG_HPP
#define LOBBYCONNECTDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QLabel;
class QRegularExpression;

namespace UserInterface
{
namespace Dialog
{

class LobbyButton;

// Small modal asking for a username before opening the main lobby. Server URL
// is hardcoded to the production endpoint; persists username via QSettings.
class LobbyConnectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LobbyConnectDialog(QWidget* parent = nullptr);
    ~LobbyConnectDialog() override = default;

    QString serverUrl() const { return m_serverUrl; }
    QString username()  const { return m_username; }
    void setUsername(const QString& username);
    void setStatusMessage(const QString& message);

    // Production lobby endpoint every client connects to. Exposed so callers
    // that bypass this dialog (e.g. the netplay launcher's rollback tab) can
    // connect with the same URL without duplicating the constant.
    static QString defaultServerUrl();

    // The lobby decides what's a valid name. These keep typing to its characters,
    // and catch what's too long for a replay's 32-byte field before it's refused.
    static QRegularExpression usernameCharacters();
    static bool usernameFits(const QString& username);
    static QString fitUsername(QString username);

private slots:
    void onConnect();
    void validateInput();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();

    QLineEdit*   m_usernameEdit  = nullptr;
    LobbyButton* m_connectButton = nullptr;
    QLabel*      m_validationLbl = nullptr;

    QString m_serverUrl;
    QString m_username;
    QString m_statusMessage;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYCONNECTDIALOG_HPP
