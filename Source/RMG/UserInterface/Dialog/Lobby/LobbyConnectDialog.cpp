/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyConnectDialog.hpp"

#include "LobbyWidgets.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QFont>
#include <QIcon>

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

LobbyConnectDialog::LobbyConnectDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Join Rollback Netplay"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setFixedWidth(420);
    buildUi();
    loadSettings();
    validateInput();
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
    auto* word = new QLabel(QStringLiteral("RMG-K"), header);
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
    auto* heading = new QLabel(tr("Welcome to Netplay!"), header);
    heading->setFont(LobbyTheme::uiFont(13));
    heading->setProperty("role", "ink2");
    headerLay->addWidget(heading);
    root->addWidget(header);

    auto* rule = new QFrame(this);
    rule->setObjectName("FieldRule");
    root->addWidget(rule);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(22, 16, 22, 4);
    body->setSpacing(6);
    auto* label = new QLabel(tr("Enter a username:").toUpper(), this);
    QFont labelFont = LobbyTheme::uiFont(10, QFont::DemiBold);
    labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    label->setFont(labelFont);
    label->setProperty("role", "muted");
    body->addWidget(label);

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setMaxLength(20);
    m_usernameEdit->setMinimumHeight(38);
    m_usernameEdit->setPlaceholderText(tr("2-20 characters: letters, numbers, _ - ."));
    m_usernameEdit->setValidator(new QRegularExpressionValidator(usernameCharacters(), this));
    body->addWidget(m_usernameEdit);

    m_validationLbl = new QLabel(this);
    m_validationLbl->setFont(LobbyTheme::uiFont(11));
    m_validationLbl->setProperty("role", "bad");
    m_validationLbl->setWordWrap(true);
    body->addWidget(m_validationLbl);
    root->addLayout(body);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(22, 12, 22, 18);
    footer->setSpacing(10);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
    cancel->setButtonSize(LobbyButton::Size::Large);
    m_connectButton = new LobbyButton(tr("Connect"), LobbyButton::Kind::Primary, this);
    m_connectButton->setObjectName("ConnectButton");
    m_connectButton->setButtonSize(LobbyButton::Size::Large);
    m_connectButton->setPixelLabel(true);
    m_connectButton->setDefault(true);
    m_connectButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    footer->addWidget(cancel);
    footer->addWidget(m_connectButton, 1);
    root->addLayout(footer);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_connectButton, &QPushButton::clicked, this, &LobbyConnectDialog::onConnect);

    connect(m_usernameEdit, &QLineEdit::textChanged, this, [this]() {
        m_statusMessage.clear();
        validateInput();
    });
}

void LobbyConnectDialog::validateInput()
{
    const QString user = m_usernameEdit->text().trimmed();

    QString reason;
    if (user.length() < 2)
        reason = tr("Username must be at least 2 characters.");
    else if (!usernameFits(user))
        reason = tr("That name is too long. The limit is 15 Greek or Cyrillic characters, "
                    "or 10 Japanese, Chinese or Korean characters.");

    const QString message = reason.isEmpty() ? m_statusMessage : reason;
    m_validationLbl->setText(message);
    m_connectButton->setEnabled(reason.isEmpty());
    // The message can wrap, which the dialog has to grow for. The label only marks
    // the top layout dirty, so its own stays stale without the activate.
    if (isVisible())
    {
        layout()->activate();
        adjustSize();
    }
}

void LobbyConnectDialog::onConnect()
{
    m_serverUrl = defaultServerUrl();
    m_username  = m_usernameEdit->text().trimmed();
    saveSettings();
    accept();
}

void LobbyConnectDialog::setUsername(const QString& username)
{
    m_usernameEdit->setText(username);
    m_usernameEdit->setFocus();
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
}

void LobbyConnectDialog::saveSettings()
{
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/Username", m_username);
}

#endif // NETPLAY
