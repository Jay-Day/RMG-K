/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef LOBBYCONNECTDIALOG_HPP
#define LOBBYCONNECTDIALOG_HPP

#ifdef NETPLAY

#include "LobbyClient.hpp"

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QLabel;
class QCheckBox;
class QFormLayout;
class QWidget;

namespace Utilities
{
class DiscordLogin;
class PasswordResetEmail;
}

namespace UserInterface
{
namespace Dialog
{

// Small modal asking how to join the lobby: log in, create an account, play
// as a guest, or reset a forgotten password with a one-time code from a
// moderator or, when the lobby can send email, from the account's email. Server
// URL is hardcoded to the production endpoint; persists the username and a
// remember-me token via QSettings.
class LobbyConnectDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode
    {
        Login,
        Register,
        Guest,
        Reset, // "Forgot password?": a one-time code from a moderator or by email
    };

    explicit LobbyConnectDialog(QWidget* parent = nullptr);
    ~LobbyConnectDialog() override = default;

    QString serverUrl() const { return m_serverUrl; }
    QString username()  const { return m_username; }
    // Account authentication for HELLO; empty mode = guest.
    LobbyClient::LobbyAuth auth() const { return m_auth; }
    // Whether to keep the session token returned by the server.
    bool rememberLogin() const { return m_rememberLogin; }
    void setUsername(const QString& username);
    void setStatusMessage(const QString& message);
    void setMode(Mode mode);
    // The tab for a HELLO auth mode, e.g. to reopen where a failed attempt was made.
    static Mode modeForAuth(const QString& authMode);
    // After a Discord sign-in for a Discord account that isn't linked yet:
    // log in (mode "discord_link") or create an account ("discord_register")
    // with the server's ticket to link it.
    void setDiscordLink(const QString& ticket, const QString& discordName);

    // Production lobby endpoint every client connects to. Exposed so callers
    // that bypass this dialog (e.g. the netplay launcher's rollback tab) can
    // connect with the same URL without duplicating the constant.
    static QString defaultServerUrl();

    // Remember-me token storage, keyed by the account's username.
    static QString savedSessionToken(const QString& username);
    static void saveSessionToken(const QString& username, const QString& token);
    static void clearSessionToken();
    // The account a saved login connects to without this dialog, or empty:
    // none is saved, or the player last joined as a guest.
    static QString savedLoginUser();
    // After a successful connection: open the Log in tab next time for an
    // account, Play as guest for a guest.
    static void saveConnectedMode(bool account);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onConnect();
    void validateInput();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void updateSavedLoginRow();
    void updateCapsLock();
    void onDiscordClicked();
    void setDiscordWaiting(bool waiting);
    void clearDiscordLink();
    void onEmailCodeClicked();
    // why the current input can't be sent yet, or empty
    QString inputProblem(QWidget** field) const;

    Mode         m_mode = Mode::Login;
    QWidget*     m_modeBar        = nullptr;
    QPushButton* m_loginTab       = nullptr;
    QPushButton* m_registerTab    = nullptr;
    QPushButton* m_guestTab       = nullptr;
    QLabel*      m_modeNote       = nullptr;
    QFormLayout* m_form           = nullptr;
    QLineEdit*   m_usernameEdit   = nullptr;
    QLineEdit*   m_resetCodeEdit  = nullptr;
    QLineEdit*   m_passwordEdit   = nullptr;
    QLineEdit*   m_confirmEdit    = nullptr;
    QLabel*      m_resetLabel     = nullptr;
    QLabel*      m_passwordLabel  = nullptr;
    QLabel*      m_confirmLabel   = nullptr;
    QCheckBox*   m_showPassword   = nullptr;
    QLabel*      m_capsLockLbl    = nullptr;
    QLabel*      m_forgotLink     = nullptr; // Log in: switch to Reset
    QLabel*      m_backLink       = nullptr; // Reset: back to Log in
    QCheckBox*   m_rememberCheck  = nullptr;
    QLabel*      m_savedLoginLbl  = nullptr;
    QPushButton* m_forgetButton   = nullptr;
    QPushButton* m_connectButton  = nullptr;
    QLabel*      m_validationLbl  = nullptr;
    QLabel*      m_orLabel        = nullptr; // between Discord and the password form
    // form rows (field plus the Show password column)
    QWidget*     m_usernameRow    = nullptr;
    QWidget*     m_resetRow       = nullptr;
    QWidget*     m_passwordRow    = nullptr;
    QWidget*     m_confirmRow     = nullptr;
    QWidget*     m_savedRow       = nullptr;
    QPushButton* m_discordButton  = nullptr; // shown when the lobby supports Discord sign-in
    QLabel*      m_discordLinkLbl = nullptr;
    QLabel*      m_dontLinkLink   = nullptr; // link mode: back to a plain login
    Utilities::DiscordLogin* m_discord = nullptr;
    bool    m_discordAvailable = false;
    bool    m_discordWaiting   = false;
    QString m_discordTicket;    // link mode while set
    // Reset: emails a reset code when the lobby can send email; the account's
    // confirmed address is required, not just the username
    QLabel*      m_resetEmailLabel = nullptr;
    QLineEdit*   m_resetEmailEdit  = nullptr;
    QWidget*     m_resetEmailRow   = nullptr;
    QPushButton* m_emailCodeButton = nullptr;
    Utilities::PasswordResetEmail* m_resetEmail = nullptr;
    bool    m_emailAvailable = false;
    QString m_emailRequestUser;
    QString m_noticeMessage;    // shown (not as an error) until the username or mode changes
    // problems show as grey hints until the first attempt to connect
    bool    m_attempted = false;

    QString m_serverUrl;
    QString m_username;
    QString m_statusMessage;
    LobbyClient::LobbyAuth m_auth;
    bool    m_rememberLogin = true;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYCONNECTDIALOG_HPP
