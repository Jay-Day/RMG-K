/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyConnectDialog.hpp"

#include "LobbyWidgets.hpp"
#include "Utilities/DiscordLogin.hpp"
#include "Utilities/PasswordResetEmail.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStyle>
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
    // Lobby servers hosted on Vultr. Update here when DNS is set up (e.g.
    // ws://lobby.rmgk.net:8080/ws). The dialog no longer exposes server
    // choice — tagged releases connect to prod, everything else to test.
#ifdef RMGK_TEST_BUILD
    constexpr const char* kDefaultLobbyUrl = "ws://64.177.116.159:8080/ws";
#else
    constexpr const char* kDefaultLobbyUrl = "ws://216.128.157.98:8080/ws";
#endif

    // Must match the lobby's account password rules.
    constexpr int kMinPasswordLength = 8;
    constexpr int kMaxPasswordLength = 128;

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
        auto* label = new QLabel(QString("<a href=\"#\" style=\"color:%1; text-decoration:none;\">%2</a>")
                                     .arg(LobbyTheme::css(LobbyTheme::tokens().accentInk), text.toHtmlEscaped()),
                                 parent);
        label->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        label->setCursor(Qt::PointingHandCursor);
        return label;
    }

    // The small upper-case caption above a field.
    QLabel* fieldLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        label->setFont(font);
        label->setProperty("role", "muted");
        return label;
    }

    QLineEdit* fieldEdit(QWidget* parent)
    {
        auto* edit = new QLineEdit(parent);
        edit->setMinimumHeight(38);
        return edit;
    }

    // A caption over its field, as one block that shows and hides together.
    QWidget* fieldBlock(QWidget* parent, QWidget* caption, QWidget* field)
    {
        auto* block = new QWidget(parent);
        auto* lay = new QVBoxLayout(block);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(5);
        lay->addWidget(caption);
        lay->addWidget(field);
        return block;
    }

    void setRole(QLabel* label, const char* role)
    {
        if (label->property("role").toString() == QLatin1String(role))
            return;
        label->setProperty("role", role);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
} // namespace

QString LobbyConnectDialog::defaultServerUrl()
{
    // Allow overriding the lobby server without a rebuild — handy for local
    // testing. e.g. set RMGK_LOBBY_URL=ws://127.0.0.1:8080/ws to point at a
    // server running on this machine. Falls back to the build's default server.
    const QString override = qEnvironmentVariable("RMGK_LOBBY_URL");
    if (!override.trimmed().isEmpty())
        return override.trimmed();
    return QString::fromUtf8(kDefaultLobbyUrl);
}

QRegularExpression LobbyConnectDialog::usernameCharacters()
{
    return QRegularExpression(QStringLiteral(R"([\p{L}\p{Mn}0-9_\-\.\x{30FB}]{1,20})"));
}

bool LobbyConnectDialog::usernameFits(const QString& username)
{
    return username.toUtf8().size() <= 31;
}

QString LobbyConnectDialog::fitUsername(QString username)
{
    username = username.normalized(QString::NormalizationForm_C);
    static const QRegularExpression notInName(QStringLiteral(R"([^\p{L}\p{Mn}0-9_\-\.\x{30FB}])"));
    username.remove(notInName);
    username.truncate(20);
    if (!username.isEmpty() && username.back().isHighSurrogate())
        username.chop(1);
    while (!username.isEmpty() && !usernameFits(username))
        username.chop(username.back().isLowSurrogate() ? 2 : 1);
    return username;
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

QString LobbyConnectDialog::savedLoginUser()
{
    QSettings s("RMG-K", "n02");
    if (s.value("Lobby/ConnectMode").toString() == "guest" || s.value("Lobby/SessionToken").toString().isEmpty())
        return QString();
    return s.value("Lobby/SessionUser").toString();
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
    setFixedWidth(420);
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
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new LobbyBand(this);
    auto* headerLay = new QVBoxLayout(header);
    headerLay->setContentsMargins(22, 18, 22, 14);
    headerLay->setSpacing(10);
    auto* brand = new QHBoxLayout;
    brand->setSpacing(10);
    brand->addWidget(new BrandMark(header));
    auto* words = new QVBoxLayout;
    words->setSpacing(1);
    auto* word = new QLabel(QStringLiteral("Dale64"), header);
    word->setFont(LobbyTheme::pixelFont(18, 0.3));
    words->addWidget(word);
    auto* tagline = new QLabel(tr("ROLLBACK NETPLAY"), header);
    QFont taglineFont = LobbyTheme::uiFont(8, QFont::Bold);
    taglineFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.2);
    tagline->setFont(taglineFont);
    tagline->setProperty("role", "muted");
    words->addWidget(tagline);
    brand->addLayout(words);
    brand->addStretch(1);
    headerLay->addLayout(brand);
    m_heading = new QLabel(tr("Log in to play ranked and keep your name, or jump in as a guest."), header);
    m_heading->setFont(LobbyTheme::uiFont(13));
    m_heading->setProperty("role", "ink2");
    m_heading->setWordWrap(true);
    headerLay->addWidget(m_heading);
    root->addWidget(header);

    auto* rule = new QFrame(this);
    rule->setObjectName("FieldRule");
    root->addWidget(rule);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(22, 16, 22, 4);
    body->setSpacing(10);

    // link mode: a Discord sign-in found no linked account yet; the message
    // goes above the tabs, which then offer Log in and Create account
    m_discordLinkLbl = new QLabel(this);
    m_discordLinkLbl->setWordWrap(true);
    m_discordLinkLbl->setFont(LobbyTheme::uiFont(12));
    m_discordLinkLbl->setProperty("role", "ink2");
    m_discordLinkLbl->setVisible(false);
    body->addWidget(m_discordLinkLbl);

    // ── How to join: Log in | Create account | Play as guest ──
    m_modeBar = new QFrame(this);
    m_modeBar->setObjectName("Seg");
    auto* barLay = new QHBoxLayout(m_modeBar);
    barLay->setContentsMargins(3, 3, 3, 3);
    barLay->setSpacing(3);
    auto* tabs = new QButtonGroup(this);
    tabs->setExclusive(true);
    auto makeTab = [&](const QString& text, const char* name) {
        auto* tab = new QPushButton(text, m_modeBar);
        tab->setObjectName(name);
        tab->setProperty("seg", true);
        tab->setCheckable(true);
        tab->setAutoDefault(false);
        tab->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        tab->setCursor(Qt::PointingHandCursor);
        tabs->addButton(tab);
        barLay->addWidget(tab, 1);
        return tab;
    };
    m_loginTab    = makeTab(tr("Log in"), "LoginTab");
    m_registerTab = makeTab(tr("Create account"), "RegisterTab");
    m_guestTab    = makeTab(tr("Play as guest"), "GuestTab");
    body->addWidget(m_modeBar);
    connect(m_loginTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Login); });
    connect(m_registerTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Register); });
    connect(m_guestTab, &QPushButton::clicked, this, [this]() { setMode(Mode::Guest); });

    // Log in: Discord first (a shortcut into an account that keeps its
    // password), then OR, then the password form
    m_discordButton = new QPushButton(tr("Log in with Discord"), this);
    m_discordButton->setObjectName("DiscordButton");
    m_discordButton->setAutoDefault(false);
    m_discordButton->setCursor(Qt::PointingHandCursor);
    m_discordButton->setIcon(discordIcon(16, 8, devicePixelRatioF()));
    m_discordButton->setIconSize(QSize(16 + 8, 16));
    m_discordButton->setMinimumHeight(38);
    m_discordButton->setFont(LobbyTheme::uiFont(13, QFont::DemiBold));
    // Discord's own blurple, with the lip every lobby button has
    m_discordButton->setStyleSheet(
        "QPushButton#DiscordButton { background: #5865F2; color: white; border: 1px solid #4752C4;"
        "  border-bottom: 3px solid #3640A0; border-radius: 8px; padding: 4px 12px; }"
        "QPushButton#DiscordButton:hover { background: #4752C4; }"
        "QPushButton#DiscordButton:pressed { background: #3C45A5; border-bottom-width: 1px; }"
        "QPushButton#DiscordButton:disabled { background: #8189D8; }");
    m_discordButton->setToolTip(tr("Sign in with your linked Discord account instead of typing your password.\n"
                                   "Not linked yet? Sign in with Discord, then enter your password once to link it."));
    m_discordButton->setVisible(false);
    body->addWidget(m_discordButton);

    m_orRow = new QWidget(this);
    auto* orLay = new QHBoxLayout(m_orRow);
    orLay->setContentsMargins(0, 0, 0, 0);
    orLay->setSpacing(10);
    auto orLine = [this]() {
        auto* line = new QFrame(m_orRow);
        line->setObjectName("FieldRule");
        return line;
    };
    auto* orLabel = new QLabel(tr("OR"), m_orRow);
    QFont orFont = LobbyTheme::uiFont(10, QFont::DemiBold);
    orFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    orLabel->setFont(orFont);
    orLabel->setProperty("role", "faint");
    orLay->addWidget(orLine(), 1);
    orLay->addWidget(orLabel);
    orLay->addWidget(orLine(), 1);
    m_orRow->setVisible(false);
    body->addWidget(m_orRow);

    m_modeNote = new QLabel(this);
    m_modeNote->setWordWrap(true);
    m_modeNote->setFont(LobbyTheme::uiFont(12));
    m_modeNote->setProperty("role", "muted");
    body->addWidget(m_modeNote);

    m_usernameEdit = fieldEdit(this);
    m_usernameEdit->setMaxLength(20);
    m_usernameEdit->setPlaceholderText(tr("2-20 characters: letters, numbers, _ - ."));
    m_usernameEdit->setValidator(new QRegularExpressionValidator(usernameCharacters(), this));
    body->addWidget(fieldBlock(this, fieldLabel(tr("Username"), this), m_usernameEdit));

    // Forgotten passwords: a one-time reset code from a moderator or, with the
    // account's confirmed address, by email.
    m_resetEmailEdit = fieldEdit(this);
    m_resetEmailEdit->setMaxLength(254);
    m_resetEmailEdit->setPlaceholderText(tr("The address confirmed on your account"));
    m_emailCodeButton = new LobbyButton(tr("Email me a code"), LobbyButton::Kind::Secondary, this);
    m_emailCodeButton->setIconName("mail-send-line");
    m_emailCodeButton->setToolTip(tr("Emails a reset code if this is the address confirmed on your account."));
    auto* emailField = new QWidget(this);
    auto* emailLay = new QHBoxLayout(emailField);
    emailLay->setContentsMargins(0, 0, 0, 0);
    emailLay->setSpacing(8);
    emailLay->addWidget(m_resetEmailEdit, 1);
    emailLay->addWidget(m_emailCodeButton);
    m_resetEmailLabel = fieldLabel(tr("Email"), this);
    m_resetEmailRow = fieldBlock(this, m_resetEmailLabel, emailField);
    body->addWidget(m_resetEmailRow);

    m_resetCodeEdit = fieldEdit(this);
    m_resetCodeEdit->setMaxLength(32);
    m_resetCodeEdit->setPlaceholderText("XXXX-XXXX-XXXX");
    m_resetLabel = fieldLabel(tr("Reset code"), this);
    m_resetRow = fieldBlock(this, m_resetLabel, m_resetCodeEdit);
    body->addWidget(m_resetRow);

    // Password, with "Forgot password?" on its caption and the eye in the field
    m_passwordEdit = fieldEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMaxLength(kMaxPasswordLength);
    m_showPassword = m_passwordEdit->addAction(QIcon(), QLineEdit::TrailingPosition);
    m_showPassword->setObjectName("ShowPassword");
    connect(m_showPassword, &QAction::triggered, this, [this]() { setPasswordShown(!m_passwordShown); });
    m_passwordLabel = fieldLabel(tr("Password"), this);
    m_forgotLink = makeLink(tr("Forgot password?"), this);
    auto* passwordCaption = new QWidget(this);
    auto* captionLay = new QHBoxLayout(passwordCaption);
    captionLay->setContentsMargins(0, 0, 0, 0);
    captionLay->addWidget(m_passwordLabel);
    captionLay->addStretch(1);
    captionLay->addWidget(m_forgotLink);
    m_passwordRow = fieldBlock(this, passwordCaption, m_passwordEdit);
    body->addWidget(m_passwordRow);

    m_confirmEdit = fieldEdit(this);
    m_confirmEdit->setEchoMode(QLineEdit::Password);
    m_confirmEdit->setMaxLength(kMaxPasswordLength);
    m_confirmLabel = fieldLabel(tr("Confirm password"), this);
    m_confirmRow = fieldBlock(this, m_confirmLabel, m_confirmEdit);
    body->addWidget(m_confirmRow);
    setPasswordShown(false);

    m_capsLockLbl = new QLabel(tr("Caps Lock is on"), this);
    m_capsLockLbl->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
    m_capsLockLbl->setProperty("role", "warn");
    m_capsLockLbl->setVisible(false);
    body->addWidget(m_capsLockLbl);

    // what's missing or what went wrong, right under the fields
    m_validationLbl = new QLabel(this);
    m_validationLbl->setFont(LobbyTheme::uiFont(11));
    m_validationLbl->setProperty("role", "muted");
    m_validationLbl->setWordWrap(true);
    body->addWidget(m_validationLbl);

    m_rememberCheck = new ToggleSwitch(tr("Keep me logged in"), this);
    m_rememberCheck->setObjectName("RememberLogin");
    m_rememberCheck->setChecked(true);
    body->addWidget(m_rememberCheck);

    m_savedRow = new QWidget(this);
    auto* savedLay = new QHBoxLayout(m_savedRow);
    savedLay->setContentsMargins(0, 0, 0, 0);
    savedLay->setSpacing(8);
    m_savedLoginLbl = new QLabel(m_savedRow);
    m_savedLoginLbl->setFont(LobbyTheme::uiFont(12));
    m_savedLoginLbl->setProperty("role", "muted");
    m_savedLoginLbl->setWordWrap(true);
    m_forgetButton = new LobbyButton(tr("Log out"), LobbyButton::Kind::Ghost, m_savedRow);
    m_forgetButton->setButtonSize(LobbyButton::Size::Small);
    m_forgetButton->setIconName("logout-box-r-line");
    savedLay->addWidget(m_savedLoginLbl, 1);
    savedLay->addWidget(m_forgetButton);
    body->addWidget(m_savedRow);

    // Reset: back to Log in; linking: log in without linking
    m_backLink = makeLink(tr("Back to log in"), this);
    m_dontLinkLink = makeLink(tr("Don't link"), this);
    m_dontLinkLink->setToolTip(tr("Log in or create an account without linking this Discord account."));
    m_linksRow = new QWidget(this);
    auto* linksLay = new QHBoxLayout(m_linksRow);
    linksLay->setContentsMargins(0, 0, 0, 0);
    linksLay->setSpacing(14);
    linksLay->addWidget(m_backLink);
    linksLay->addWidget(m_dontLinkLink);
    linksLay->addStretch(1);
    body->addWidget(m_linksRow);
    root->addLayout(body);

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

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(22, 12, 22, 18);
    footer->setSpacing(10);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
    cancel->setButtonSize(LobbyButton::Size::Large);
    m_connectButton = new LobbyButton(tr("Log in"), LobbyButton::Kind::Primary, this);
    m_connectButton->setObjectName("ConnectButton");
    m_connectButton->setButtonSize(LobbyButton::Size::Large);
    m_connectButton->setPixelLabel(true);
    m_connectButton->setDefault(true);
    m_connectButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    footer->addWidget(cancel);
    footer->addWidget(m_connectButton, 1);
    root->addLayout(footer);
    connect(cancel, &QPushButton::clicked, this, [this]() {
        m_discord->cancel();
        reject();
    });
    connect(m_connectButton, &QPushButton::clicked, this, &LobbyConnectDialog::onConnect);

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

void LobbyConnectDialog::setPasswordShown(bool shown)
{
    m_passwordShown = shown;
    const auto echo = shown ? QLineEdit::Normal : QLineEdit::Password;
    m_passwordEdit->setEchoMode(echo);
    m_confirmEdit->setEchoMode(echo);
    const QColor color = shown ? LobbyTheme::tokens().accentInk : LobbyTheme::tokens().muted;
    m_showPassword->setIcon(QIcon(LobbyTheme::icon("eye-line", color, 16, devicePixelRatioF())));
    m_showPassword->setText(shown ? tr("Hide password") : tr("Show password"));
    m_showPassword->setToolTip(m_showPassword->text());
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

void LobbyConnectDialog::refit()
{
    // A label only marks the top layout dirty when it rewraps, so the dialog
    // keeps its old height without the activate.
    if (isVisible())
    {
        layout()->activate();
        adjustSize();
    }
}

void LobbyConnectDialog::updateCapsLock()
{
    const bool typingPassword = (m_passwordEdit->hasFocus() || m_confirmEdit->hasFocus()) &&
                                m_passwordEdit->isVisible();
    const bool show = typingPassword && capsLockOn();
    if (m_capsLockLbl->isVisibleTo(this) != show)
    {
        m_capsLockLbl->setVisible(show);
        refit();
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

    m_resetRow->setVisible(reset);
    m_passwordRow->setVisible(!guest);
    m_confirmRow->setVisible(register_ || reset);
    m_rememberCheck->setVisible(!guest);
    m_forgotLink->setVisible(login && !linking);
    m_backLink->setVisible(reset);
    m_linksRow->setVisible(reset || linking);
    m_resetEmailRow->setVisible(reset && m_emailAvailable);
    const bool discord = login && m_discordAvailable && !linking;
    m_discordButton->setVisible(discord);
    m_orRow->setVisible(discord);

    m_passwordLabel->setText((reset ? tr("New password") : tr("Password")).toUpper());
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
        action = m_renaming ? tr("Change") : tr("Join as guest");
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
    refit();
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
        setRole(m_validationLbl, "muted");
        m_validationLbl->setText(tr("Finish signing in to Discord in your browser."));
        m_validationLbl->setVisible(true);
        m_connectButton->setEnabled(false);
        refit();
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
    if (m_savedRow->isVisibleTo(this) != showSaved)
    {
        m_savedRow->setVisible(showSaved);
        refit();
    }
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
    if (!usernameFits(user))
        return at(m_usernameEdit, tr("That name is too long. The limit is 15 Greek or Cyrillic characters, "
                                     "or 10 Japanese, Chinese or Korean characters."));
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
    // Before the first attempt a problem is only a muted hint; the button
    // stays enabled so clicking it explains what's missing.
    const QString problem = inputProblem(nullptr);
    QString message;
    const char* role = "muted";
    if (!m_statusMessage.isEmpty())
    {
        message = m_statusMessage;
        role = "bad";
    }
    else if (!m_noticeMessage.isEmpty())
    {
        message = m_noticeMessage;
        role = "ok";
    }
    else if (!problem.isEmpty())
    {
        message = problem;
        role = m_attempted ? "bad" : "muted";
    }
    setRole(m_validationLbl, role);
    const bool resize = message != m_validationLbl->text() || m_validationLbl->isVisibleTo(this) == message.isEmpty();
    m_validationLbl->setText(message);
    m_validationLbl->setVisible(!message.isEmpty());
    m_connectButton->setEnabled(!m_discordWaiting);
    // fit a message that wraps onto more lines than the last one
    if (resize)
        refit();
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

void LobbyConnectDialog::setRenameMode()
{
    m_renaming = true;
    setWindowTitle(tr("Change Username"));
    m_heading->setText(tr("Pick a new name, or log in. You'll reconnect."));
    setMode(Mode::Guest);
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
