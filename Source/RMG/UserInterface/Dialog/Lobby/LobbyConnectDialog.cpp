/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyConnectDialog.hpp"
#include "Utilities/DiscordLogin.hpp"
#include "Utilities/PasswordResetEmail.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QButtonGroup>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QDialogButtonBox>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace UserInterface::Dialog;

namespace
{
    // Production lobby server hosted on Vultr Chicago. Update here when DNS
    // is set up (e.g. ws://lobby.rmgk.net:8080/ws). The dialog no longer
    // exposes server choice — every client connects here.
    constexpr const char* kDefaultLobbyUrl = "ws://216.128.157.98:8080/ws";

    // Must match the lobby's account password rules.
    constexpr int kMinPasswordLength = 8;
    constexpr int kMaxPasswordLength = 128;

    constexpr const char* kHintStyle   = "color: gray;";
    constexpr const char* kErrorStyle  = "color: #c0392b;";
    constexpr const char* kNoticeStyle = "color: #2e8b57;";

    bool capsLockOn()
    {
#ifdef _WIN32
        return (GetKeyState(VK_CAPITAL) & 1) != 0;
#else
        return false; // no portable way to read it; the warning is Windows-only
#endif
    }

    // Discord's logo (the "Clyde" mark), white for the blurple button. The
    // pixmap is wider than the logo: the transparent gap on the right spaces
    // it from the button text (QPushButton has no icon spacing setting).
    QIcon discordIcon(int size, int gap, qreal dpr)
    {
        static const char* svg =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='#ffffff' d='"
            "M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648"
            "-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00"
            "-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799"
            ".0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00"
            ".0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495"
            "-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105"
            "c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077"
            " 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628"
            " 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313"
            "-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0"
            "-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419"
            " 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554"
            "-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z'/></svg>";
        QSvgRenderer renderer{QByteArray(svg)};
        QPixmap pixmap(QSize(size + gap, size) * dpr);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter, QRectF(0, 0, size * dpr, size * dpr));
        painter.end();
        pixmap.setDevicePixelRatio(dpr);
        return QIcon(pixmap);
    }

    QLabel* makeLink(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(QString("<a href=\"#\">%1</a>").arg(text.toHtmlEscaped()), parent);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        label->setCursor(Qt::PointingHandCursor);
        return label;
    }
} // namespace

QString LobbyConnectDialog::defaultServerUrl()
{
    // Allow overriding the lobby server without a rebuild — handy for local
    // testing. e.g. set RMGK_LOBBY_URL=ws://127.0.0.1:8080/ws to point at a
    // server running on this machine. Falls back to the production server.
    const QString override = qEnvironmentVariable("RMGK_LOBBY_URL");
    if (!override.trimmed().isEmpty())
        return override.trimmed();
    return QString::fromUtf8(kDefaultLobbyUrl);
}

QString LobbyConnectDialog::savedSessionToken(const QString& username)
{
    QSettings s("RMG-K", "n02");
    if (s.value("Lobby/SessionUser").toString().compare(username, Qt::CaseInsensitive) != 0)
        return QString();
    return s.value("Lobby/SessionToken").toString();
}

void LobbyConnectDialog::saveSessionToken(const QString& username, const QString& token)
{
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/SessionUser", username);
    s.setValue("Lobby/SessionToken", token);
}

void LobbyConnectDialog::clearSessionToken()
{
    QSettings s("RMG-K", "n02");
    s.remove("Lobby/SessionUser");
    s.remove("Lobby/SessionToken");
}

LobbyConnectDialog::Mode LobbyConnectDialog::modeForAuth(const QString& authMode)
{
    if (authMode.isEmpty())        return Mode::Guest;
    if (authMode == "register")    return Mode::Register;
    if (authMode == "reset")       return Mode::Reset;
    if (authMode == "discord_register") return Mode::Register;
    return Mode::Login; // login, token, discord, discord_link
}

LobbyConnectDialog::LobbyConnectDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Join Rollback Netplay"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setMinimumWidth(420);
    buildUi();
    loadSettings();

    // the Discord button appears once the lobby says it supports it, and
    // "Email me a code" once it says it can send email
    m_discord->checkAvailable(defaultServerUrl());
    m_resetEmail->checkAvailable(defaultServerUrl());
}

void LobbyConnectDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Welcome to Netplay!"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    heading->setFont(headingFont);
    root->addWidget(heading);

    // ── How to join: Log in | Create account | Play as guest ──
    m_modeBar = new QWidget(this);
    m_modeBar->setObjectName("ConnectModeBar");
    auto* barLay = new QHBoxLayout(m_modeBar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->setSpacing(0);
    auto* tabs = new QButtonGroup(this);
    tabs->setExclusive(true);
    auto makeTab = [&](const QString& text, const char* name) {
        auto* tab = new QPushButton(text, m_modeBar);
        tab->setObjectName(name);
        tab->setCheckable(true);
        tab->setAutoDefault(false);
        tab->setCursor(Qt::PointingHandCursor);
        tabs->addButton(tab);
        barLay->addWidget(tab, 1);
        return tab;
    };
    m_loginTab    = makeTab(tr("Log in"), "LoginTab");
    m_registerTab = makeTab(tr("Create account"), "RegisterTab");
    m_guestTab    = makeTab(tr("Play as guest"), "GuestTab");
    m_modeBar->setStyleSheet(
        "QPushButton { padding: 6px 10px; border: 1px solid palette(mid); border-radius: 0px;"
        "  background: transparent; }"
        "QPushButton:hover { background: palette(alternate-base); }"
        "QPushButton:checked { background: palette(highlight); color: palette(highlighted-text);"
        "  border-color: palette(highlight); font-weight: 600; }"
        "QPushButton#LoginTab { border-top-left-radius: 6px; border-bottom-left-radius: 6px; }"
        "QPushButton#GuestTab { border-top-right-radius: 6px; border-bottom-right-radius: 6px; }");
    root->addWidget(m_modeBar);
    connect(m_loginTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Login); });
    connect(m_registerTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Register); });
    connect(m_guestTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Guest); });

    // link mode: a Discord sign-in found no linked account yet; the message
    // goes above the tabs, which then offer Log in and Create account
    m_discordLinkLbl = new QLabel(this);
    m_discordLinkLbl->setWordWrap(true);
    root->insertWidget(root->indexOf(m_modeBar), m_discordLinkLbl);
    m_discordLinkLbl->setVisible(false);

    // Log in: Discord first (a shortcut into an account that keeps its
    // password), then OR, then the password form
    m_discordButton = new QPushButton(tr("Log in with Discord"), this);
    m_discordButton->setObjectName("DiscordButton");
    m_discordButton->setAutoDefault(false);
    m_discordButton->setCursor(Qt::PointingHandCursor);
    m_discordButton->setIcon(discordIcon(16, 8, devicePixelRatioF()));
    m_discordButton->setIconSize(QSize(16 + 8, 16));
    m_discordButton->setMinimumHeight(32);
    m_discordButton->setStyleSheet(
        "QPushButton#DiscordButton { background: #5865F2; color: white; border: none; border-radius: 6px;"
        "  padding: 6px 12px; font-weight: 600; }"
        "QPushButton#DiscordButton:hover { background: #4752C4; }"
        "QPushButton#DiscordButton:pressed { background: #3C45A5; }");
    m_discordButton->setToolTip(tr("Sign in with your linked Discord account instead of typing your password.\n"
                                   "Not linked yet? Sign in with Discord, then enter your password once to link it."));
    m_discordButton->setVisible(false);
    root->addWidget(m_discordButton);

    m_orLabel = new QLabel(tr("OR"), this);
    m_orLabel->setAlignment(Qt::AlignCenter);
    QFont orFont = m_orLabel->font();
    orFont.setPointSizeF(orFont.pointSizeF() + 2.0);
    m_orLabel->setFont(orFont);
    m_orLabel->setVisible(false);
    root->addWidget(m_orLabel);

    m_modeNote = new QLabel(this);
    m_modeNote->setWordWrap(true);
    m_modeNote->setStyleSheet(kHintStyle);
    root->addWidget(m_modeNote);

    m_form = new QFormLayout;
    m_form->setVerticalSpacing(8);

    // Show password sits beside the (first) password field; the other fields
    // keep the same width so their right edges line up.
    m_showPassword = new QCheckBox(tr("Show password"), this);
    // Reset: "Email me a code" sits beside the email field when the lobby can send email
    m_emailCodeButton = new QPushButton(tr("Email me a code"), this);
    m_emailCodeButton->setAutoDefault(false);
    m_emailCodeButton->setToolTip(tr("Emails a reset code if this is the address confirmed on your account."));
    const int sideWidth = qMax(m_showPassword->sizeHint().width(), m_emailCodeButton->sizeHint().width());
    auto fieldRow = [this, sideWidth](QLineEdit* edit, QWidget* side) {
        auto* row = new QWidget(this);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(edit, 1);
        if (side == nullptr)
        {
            side = new QWidget(row);
        }
        side->setFixedWidth(sideWidth);
        lay->addWidget(side);
        return row;
    };

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setMaxLength(20);
    m_usernameEdit->setPlaceholderText(tr("2-20 characters: letters, numbers, _ - ."));
    m_usernameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(R"([A-Za-z0-9_\-\.]{1,20})"), this));
    m_usernameRow = fieldRow(m_usernameEdit, nullptr);
    m_form->addRow(tr("Username:"), m_usernameRow);

    // Forgotten passwords: a one-time reset code from a moderator or, with the
    // account's confirmed address, by email.
    m_resetEmailEdit = new QLineEdit(this);
    m_resetEmailEdit->setMaxLength(254);
    m_resetEmailEdit->setPlaceholderText(tr("The address confirmed on your account"));
    m_resetEmailLabel = new QLabel(tr("Email:"), this);
    m_resetEmailRow = fieldRow(m_resetEmailEdit, m_emailCodeButton);
    m_form->addRow(m_resetEmailLabel, m_resetEmailRow);

    m_resetCodeEdit = new QLineEdit(this);
    m_resetCodeEdit->setMaxLength(32);
    m_resetCodeEdit->setPlaceholderText("XXXX-XXXX-XXXX");
    m_resetLabel = new QLabel(tr("Reset code:"), this);
    m_resetRow = fieldRow(m_resetCodeEdit, nullptr);
    m_form->addRow(m_resetLabel, m_resetRow);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMaxLength(kMaxPasswordLength);
    m_passwordLabel = new QLabel(tr("Password:"), this);
    m_passwordRow = fieldRow(m_passwordEdit, m_showPassword);
    m_form->addRow(m_passwordLabel, m_passwordRow);

    m_confirmEdit = new QLineEdit(this);
    m_confirmEdit->setEchoMode(QLineEdit::Password);
    m_confirmEdit->setMaxLength(kMaxPasswordLength);
    m_confirmLabel = new QLabel(tr("Confirm password:"), this);
    m_confirmRow = fieldRow(m_confirmEdit, nullptr);
    m_form->addRow(m_confirmLabel, m_confirmRow);

    m_capsLockLbl = new QLabel(QStringLiteral("⚠ ") + tr("Caps Lock is on"), this);
    m_capsLockLbl->setStyleSheet("color: #d68910; font-weight: 600;");
    m_form->addRow(QString(), m_capsLockLbl);
    m_form->setRowVisible(m_capsLockLbl, false);

    // what's missing or what went wrong, right under the fields
    m_validationLbl = new QLabel(this);
    m_validationLbl->setStyleSheet(kHintStyle);
    m_validationLbl->setWordWrap(true);
    m_form->addRow(QString(), m_validationLbl);

    m_rememberCheck = new QCheckBox(tr("Keep me logged in"), this);
    m_rememberCheck->setChecked(true);
    m_form->addRow(QString(), m_rememberCheck);

    m_savedRow = new QWidget(this);
    auto* savedLay = new QHBoxLayout(m_savedRow);
    savedLay->setContentsMargins(0, 0, 0, 0);
    m_savedLoginLbl = new QLabel(m_savedRow);
    m_savedLoginLbl->setStyleSheet(kHintStyle);
    m_savedLoginLbl->setWordWrap(true);
    m_forgetButton = new QPushButton(tr("Log out"), m_savedRow);
    m_forgetButton->setAutoDefault(false);
    savedLay->addWidget(m_savedLoginLbl, 1);
    savedLay->addWidget(m_forgetButton);
    m_form->addRow(QString(), m_savedRow);

    root->addLayout(m_form);

    m_discord = new Utilities::DiscordLogin(this);
    connect(m_discord, &Utilities::DiscordLogin::availabilityKnown, this, [this](bool available) {
        m_discordAvailable = available;
        setMode(m_mode); // shows the button on the Log in tab
    });
    connect(m_discordButton, &QPushButton::clicked, this, &LobbyConnectDialog::onDiscordClicked);
    connect(m_discord, &Utilities::DiscordLogin::failed, this, [this](const QString& message) {
        setDiscordWaiting(false);
        setStatusMessage(message);
    });
    connect(m_discord, &Utilities::DiscordLogin::codeReceived, this,
            [this](const QString& code, const QString& verifier, const QString& redirectUri) {
        setDiscordWaiting(false);
        m_serverUrl     = defaultServerUrl();
        // the server signs in to whichever account the Discord account is
        // linked to and sends back its name
        m_username      = m_usernameEdit->text().trimmed();
        m_rememberLogin = m_rememberCheck->isChecked();
        m_auth = LobbyClient::LobbyAuth();
        m_auth.mode         = "discord";
        m_auth.code         = code;
        m_auth.codeVerifier = verifier;
        m_auth.redirectUri  = redirectUri;
        m_passwordEdit->clear();
        m_confirmEdit->clear();
        QSettings s("RMG-K", "n02");
        s.setValue("Lobby/RememberLogin", m_rememberLogin);
        accept();
    });

    m_resetEmail = new Utilities::PasswordResetEmail(this);
    connect(m_resetEmail, &Utilities::PasswordResetEmail::availabilityKnown, this, [this](bool available) {
        m_emailAvailable = available;
        setMode(m_mode); // Reset offers "Email me a code"
    });
    connect(m_resetEmail, &Utilities::PasswordResetEmail::requestFinished, this, [this](bool ok, const QString& message) {
        m_emailCodeButton->setText(tr("Email me a code"));
        if (ok)
        {
            m_noticeMessage = tr("If that's the confirmed email address of %1, a code is on its way. "
                                 "It works once, for %2 minutes.")
                                  .arg(m_emailRequestUser).arg(m_resetEmail->codeMinutes());
            // a short pause stops repeat clicks; the lobby limits requests too
            QTimer::singleShot(10000, m_emailCodeButton, [this]() { m_emailCodeButton->setEnabled(true); });
            m_resetCodeEdit->setFocus();
        }
        else
        {
            m_emailCodeButton->setEnabled(true);
            m_statusMessage = message;
        }
        validateInput();
    });
    connect(m_emailCodeButton, &QPushButton::clicked, this, &LobbyConnectDialog::onEmailCodeClicked);

    // bottom: Forgot password? / Back to log in on the left, the action on the right
    m_forgotLink = makeLink(tr("Forgot password?"), this);
    m_backLink   = makeLink(tr("Back to log in"), this);
    m_dontLinkLink = makeLink(tr("Don't link"), this);
    m_dontLinkLink->setToolTip(tr("Log in or create an account without linking this Discord account."));
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_connectButton = btnBox->addButton(tr("Log in"), QDialogButtonBox::AcceptRole);
    m_connectButton->setDefault(true);
    connect(btnBox, &QDialogButtonBox::rejected, this, [this]() {
        m_discord->cancel();
        reject();
    });
    connect(m_connectButton, &QPushButton::clicked, this, &LobbyConnectDialog::onConnect);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(m_forgotLink);
    bottom->addWidget(m_backLink);
    bottom->addWidget(m_dontLinkLink);
    bottom->addStretch(1);
    bottom->addWidget(btnBox);
    root->addLayout(bottom);

    auto inputChanged = [this]() {
        m_statusMessage.clear();
        updateSavedLoginRow();
        validateInput();
    };
    connect(m_usernameEdit, &QLineEdit::textChanged, this, [this, inputChanged]() {
        m_noticeMessage.clear(); // it named the previous username
        inputChanged();
    });
    connect(m_resetEmailEdit, &QLineEdit::textChanged, this, [this, inputChanged]() {
        m_noticeMessage.clear();
        inputChanged();
    });
    connect(m_passwordEdit, &QLineEdit::textChanged, this, inputChanged);
    connect(m_confirmEdit, &QLineEdit::textChanged, this, inputChanged);
    connect(m_resetCodeEdit, &QLineEdit::textChanged, this, inputChanged);
    connect(m_showPassword, &QCheckBox::toggled, this, [this](bool show) {
        const auto echo = show ? QLineEdit::Normal : QLineEdit::Password;
        m_passwordEdit->setEchoMode(echo);
        m_confirmEdit->setEchoMode(echo);
    });
    connect(m_forgotLink, &QLabel::linkActivated, this, [this]() { setMode(Mode::Reset); });
    connect(m_backLink, &QLabel::linkActivated, this, [this]() { setMode(Mode::Login); });
    connect(m_dontLinkLink, &QLabel::linkActivated, this, [this]() { clearDiscordLink(); });
    connect(m_forgetButton, &QPushButton::clicked, this, [this, inputChanged]() {
        clearSessionToken();
        inputChanged();
    });

    // Tab moves through the Forgot password? fields top to bottom
    setTabOrder(m_usernameEdit, m_resetEmailEdit);
    setTabOrder(m_resetEmailEdit, m_emailCodeButton);
    setTabOrder(m_emailCodeButton, m_resetCodeEdit);
    setTabOrder(m_resetCodeEdit, m_passwordEdit);

    // Caps Lock: checked while typing a password
    for (QLineEdit* edit : { m_passwordEdit, m_confirmEdit })
        edit->installEventFilter(this);
}

bool LobbyConnectDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_passwordEdit || watched == m_confirmEdit)
    {
        switch (event->type())
        {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::KeyPress:
            updateCapsLock();
            break;
        case QEvent::KeyRelease:
            // the toggle state settles after the Caps Lock key itself
            if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_CapsLock)
                QTimer::singleShot(0, this, &LobbyConnectDialog::updateCapsLock);
            else
                updateCapsLock();
            break;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void LobbyConnectDialog::updateCapsLock()
{
    const bool typingPassword = (m_passwordEdit->hasFocus() || m_confirmEdit->hasFocus()) &&
                                m_passwordEdit->isVisible();
    const bool show = typingPassword && capsLockOn();
    if (m_capsLockLbl->isVisibleTo(this) != show)
    {
        m_form->setRowVisible(m_capsLockLbl, show);
        adjustSize();
    }
}

void LobbyConnectDialog::setMode(Mode mode)
{
    // only the username carries over: a password typed for one tab isn't
    // meant for another (e.g. the old one into New password)
    const bool changed = mode != m_mode;
    if (changed)
    {
        m_passwordEdit->clear();
        m_confirmEdit->clear();
        if (mode != Mode::Reset)
            m_resetCodeEdit->clear();
        m_attempted = false;
        m_noticeMessage.clear();
    }
    m_mode = mode;
    const bool linking   = !m_discordTicket.isEmpty();
    const bool guest     = mode == Mode::Guest;
    const bool reset     = mode == Mode::Reset;
    const bool register_ = mode == Mode::Register;
    const bool login     = mode == Mode::Login;

    m_loginTab->setChecked(login || reset);
    m_registerTab->setChecked(register_);
    m_guestTab->setChecked(guest);
    // linking: Log in or Create account (a guest can't hold a link)
    m_guestTab->setVisible(!linking);
    m_dontLinkLink->setVisible(linking);

    m_form->setRowVisible(m_resetRow, reset);
    m_form->setRowVisible(m_passwordRow, !guest);
    m_form->setRowVisible(m_confirmRow, register_ || reset);
    m_form->setRowVisible(m_rememberCheck, !guest);
    m_forgotLink->setVisible(login && !linking);
    m_backLink->setVisible(reset);
    m_form->setRowVisible(m_resetEmailRow, reset && m_emailAvailable);
    const bool discord = login && m_discordAvailable && !linking;
    m_discordButton->setVisible(discord);
    m_orLabel->setVisible(discord);

    m_passwordLabel->setText(reset ? tr("New password:") : tr("Password:"));
    m_passwordEdit->setPlaceholderText((register_ || reset) ? tr("At least 8 characters")
                                       : linking ? tr("Required to link Discord")
                                       : QString());

    QString note;
    QString action;
    switch (mode)
    {
    case Mode::Login:
        action = linking ? tr("Log in and link") : tr("Log in");
        break;
    case Mode::Register:
        note = m_emailAvailable
            ? tr("An account keeps your name and can play ranked. Add an email address from the "
                 "account menu to reset a forgotten password yourself.")
            : tr("An account keeps your name and can play ranked. There's no email: if you forget "
                 "your password, a moderator can give you a reset code.");
        action = linking ? tr("Create account and link") : tr("Create account");
        break;
    case Mode::Guest:
        note = tr("Guests play unranked, and the name isn't reserved: someone with an account "
                  "can take it.");
        action = tr("Join as guest");
        break;
    case Mode::Reset:
        note = m_emailAvailable
            ? tr("Enter your username and the email address confirmed on your account to get a "
                 "one-time code, or ask a moderator for one. Then choose a new password.")
            : tr("Enter the one-time code a moderator gave you, then choose a new password. "
                 "Ask in the lobby chat or on Discord if you need one.");
        action = tr("Reset password");
        break;
    }
    m_modeNote->setText(note);
    m_modeNote->setVisible(!note.isEmpty());
    m_connectButton->setText(action);

    updateSavedLoginRow();
    updateCapsLock();
    validateInput();
    adjustSize();
}

void LobbyConnectDialog::onDiscordClicked()
{
    if (m_discordWaiting)
    {
        m_discord->cancel();
        setDiscordWaiting(false);
        return;
    }
    m_statusMessage.clear();
    setDiscordWaiting(true);
    m_discord->start(defaultServerUrl());
}

void LobbyConnectDialog::onEmailCodeClicked()
{
    const QString user = m_usernameEdit->text().trimmed();
    if (user.length() < 2)
    {
        m_attempted = true; // shows what's missing in red
        m_usernameEdit->setFocus();
        validateInput();
        return;
    }
    // the lobby only sends a code to the account's confirmed address, so a
    // username alone can't send emails to someone else's inbox
    const QString email = m_resetEmailEdit->text().trimmed();
    const int at = email.lastIndexOf('@');
    if (at <= 0 || email.indexOf('.', at) <= at + 1 || email.contains(' '))
    {
        m_statusMessage = tr("Enter the email address confirmed on your account.");
        m_resetEmailEdit->setFocus();
        validateInput();
        return;
    }
    m_statusMessage.clear();
    m_noticeMessage.clear();
    m_emailRequestUser = user;
    m_emailCodeButton->setEnabled(false);
    m_emailCodeButton->setText(tr("Sending..."));
    m_resetEmail->request(defaultServerUrl(), user, email);
    validateInput();
}

void LobbyConnectDialog::setDiscordWaiting(bool waiting)
{
    m_discordWaiting = waiting;
    m_discordButton->setText(waiting ? tr("Waiting for Discord in your browser... (Cancel)") : tr("Log in with Discord"));
    if (waiting)
    {
        m_validationLbl->setStyleSheet(kHintStyle);
        m_validationLbl->setText(tr("Finish signing in to Discord in your browser."));
        m_connectButton->setEnabled(false);
    }
    else
    {
        validateInput();
    }
}

void LobbyConnectDialog::setDiscordLink(const QString& ticket, const QString& discordName)
{
    m_discordTicket = ticket;
    m_discordLinkLbl->setText(tr("Signed into Discord as <b>%1</b>, which isn't linked to an account yet. "
                                 "Log in or create a new user to link it.").arg(discordName.toHtmlEscaped()));
    m_discordLinkLbl->setVisible(true);
    // stay on Create account when retrying a failed sign-up
    setMode(m_mode == Mode::Register ? Mode::Register : Mode::Login);
    (m_usernameEdit->text().isEmpty() ? m_usernameEdit : m_passwordEdit)->setFocus();
}

void LobbyConnectDialog::clearDiscordLink()
{
    m_discordTicket.clear();
    m_discordLinkLbl->setVisible(false);
    m_statusMessage.clear(); // the error was about the link attempt
    setMode(m_mode == Mode::Register ? Mode::Register : Mode::Login);
}

void LobbyConnectDialog::updateSavedLoginRow()
{
    const QString user = m_usernameEdit->text().trimmed();
    const bool showSaved = m_mode == Mode::Login && m_discordTicket.isEmpty() &&
                           !savedSessionToken(user).isEmpty() && m_passwordEdit->text().isEmpty();
    m_savedLoginLbl->setText(showSaved
        ? tr("Logged in as %1 (saved): no password needed.").arg(user)
        : QString());
    m_form->setRowVisible(m_savedRow, showSaved);
}

QString LobbyConnectDialog::inputProblem(QWidget** field) const
{
    const QString user = m_usernameEdit->text().trimmed();
    const QString password = m_passwordEdit->text();
    const bool newPassword = m_mode == Mode::Register || m_mode == Mode::Reset;
    auto at = [field](QWidget* w, const QString& text) {
        if (field)
            *field = w;
        return text;
    };

    if (user.length() < 2)
        return at(m_usernameEdit, tr("Enter a username of at least 2 characters."));
    if (m_mode == Mode::Guest)
        return QString();
    if (m_mode == Mode::Reset && m_resetCodeEdit->text().trimmed().isEmpty())
        return at(m_resetCodeEdit, m_emailAvailable ? tr("Enter the reset code from your email or a moderator.")
                                                    : tr("Enter the reset code a moderator gave you."));
    if (newPassword && password.length() < kMinPasswordLength)
        return at(m_passwordEdit, tr("Choose a password of at least %1 characters.").arg(kMinPasswordLength));
    if (newPassword && m_confirmEdit->text() != password)
        return at(m_confirmEdit, tr("The passwords don't match."));
    if (m_mode == Mode::Login && password.isEmpty() &&
        (!m_discordTicket.isEmpty() || savedSessionToken(user).isEmpty()))
        return at(m_passwordEdit, m_discordTicket.isEmpty() ? tr("Enter your password.")
                                                           : tr("Enter your password to link Discord."));
    return QString();
}

void LobbyConnectDialog::validateInput()
{
    // Before the first attempt a problem is only a grey hint; the button
    // stays enabled so clicking it explains what's missing.
    const QString problem = inputProblem(nullptr);
    QString message;
    const char* style = kHintStyle;
    if (!m_statusMessage.isEmpty())
    {
        message = m_statusMessage;
        style = kErrorStyle;
    }
    else if (!m_noticeMessage.isEmpty())
    {
        message = m_noticeMessage;
        style = kNoticeStyle;
    }
    else if (!problem.isEmpty())
    {
        message = problem;
        style = m_attempted ? kErrorStyle : kHintStyle;
    }
    m_validationLbl->setStyleSheet(style);
    const bool resize = message != m_validationLbl->text();
    m_validationLbl->setText(message);
    m_form->setRowVisible(m_validationLbl, !message.isEmpty());
    m_connectButton->setEnabled(!m_discordWaiting);
    // fit a message that wraps onto more lines than the last one
    if (resize)
        adjustSize();
}

void LobbyConnectDialog::onConnect()
{
    QWidget* field = nullptr;
    if (!inputProblem(&field).isEmpty())
    {
        m_attempted = true;
        m_statusMessage.clear();
        validateInput();
        if (field)
            field->setFocus();
        return;
    }

    m_serverUrl = defaultServerUrl();
    m_username  = m_usernameEdit->text().trimmed();
    m_rememberLogin = m_rememberCheck->isChecked();

    m_auth = LobbyClient::LobbyAuth();
    const QString password = m_passwordEdit->text();
    switch (m_mode)
    {
    case Mode::Login:
        if (!m_discordTicket.isEmpty())
        {
            m_auth.mode = "discord_link";
            m_auth.password = password;
            m_auth.ticket = m_discordTicket;
        }
        else if (!password.isEmpty())
        {
            m_auth.mode = "login";
            m_auth.password = password;
        }
        else
        {
            m_auth.mode = "token";
            m_auth.token = savedSessionToken(m_username);
        }
        break;
    case Mode::Register:
        m_auth.mode = m_discordTicket.isEmpty() ? "register" : "discord_register";
        m_auth.password = password;
        m_auth.ticket = m_discordTicket;
        break;
    case Mode::Reset:
        m_auth.mode = "reset";
        m_auth.password = password;
        m_auth.resetCode = m_resetCodeEdit->text().trimmed();
        break;
    case Mode::Guest:
        break; // no auth: a guest
    }
    m_passwordEdit->clear();
    m_confirmEdit->clear();
    m_resetCodeEdit->clear();

    saveSettings();
    accept();
}

void LobbyConnectDialog::setUsername(const QString& username)
{
    m_usernameEdit->setText(username);
    updateSavedLoginRow();
    // focus where typing starts: the password when the name is filled in
    QLineEdit* focus = username.isEmpty() || m_mode == Mode::Guest ? m_usernameEdit
                     : m_mode == Mode::Reset ? m_resetCodeEdit : m_passwordEdit;
    if (m_mode == Mode::Login && !savedSessionToken(username).isEmpty())
        focus = m_usernameEdit;
    focus->setFocus();
    if (focus == m_usernameEdit)
        m_usernameEdit->selectAll();
}

void LobbyConnectDialog::setStatusMessage(const QString& message)
{
    m_statusMessage = message;
    validateInput();
}

void LobbyConnectDialog::loadSettings()
{
    QSettings s("RMG-K", "n02");
    m_usernameEdit->setText(s.value("Lobby/Username").toString());
    m_rememberCheck->setChecked(s.value("Lobby/RememberLogin", true).toBool());
    // the way this player last got in (saved on success, so a failed login
    // doesn't move a guest off their tab); a saved login means an account
    QString mode = s.value("Lobby/ConnectMode").toString();
    if (mode.isEmpty())
        mode = s.value("Lobby/SessionToken").toString().isEmpty() ? "guest" : "login";
    setMode(mode == "guest" ? Mode::Guest : Mode::Login);
}

void LobbyConnectDialog::saveSettings()
{
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/Username", m_username);
    s.setValue("Lobby/RememberLogin", m_rememberLogin);
}

void LobbyConnectDialog::saveConnectedMode(bool account)
{
    // the tab to open next time; a new or reset account logs in
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/ConnectMode", account ? "login" : "guest");
}

#endif // NETPLAY
