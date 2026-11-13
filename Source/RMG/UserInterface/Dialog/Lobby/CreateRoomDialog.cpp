/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "CreateRoomDialog.hpp"

#include "LobbyWidgets.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSettings>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>

#include <algorithm>

using namespace UserInterface::Dialog;

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                                   const QString& gameName, const QString& gameMd5,
                                   QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Create Room"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setFixedWidth(450);
    // The game comes from the lobby's shared picker, not a picker of our own.
    m_romName = gameName;
    m_romMd5  = gameMd5;
    buildUi(defaultUsername, defaultRoomName);
    if (m_gameLabel)
        m_gameLabel->setText(gameName.isEmpty() ? QStringLiteral("—") : gameName);
    if (m_tile)
        m_tile->setGame(gameName);
    loadDefaults();
    validateInput();
}

void CreateRoomDialog::setCoverPath(const QString& coverPath)
{
    if (m_tile)
        m_tile->setGame(m_romName, coverPath);
}

void CreateRoomDialog::setMinimumPlayers(int players)
{
    m_minimumPlayers = players;
    for (auto* button : m_playersGroup->buttons())
    {
        const bool fits = m_playersGroup->id(button) >= players;
        button->setEnabled(fits);
        button->setToolTip(fits ? QString() : tr("You have %1 players on this PC.").arg(players));
    }
    if (m_playersGroup->checkedId() < players)
    {
        if (auto* button = m_playersGroup->button(players))
            button->setChecked(true);
    }
}

void CreateRoomDialog::buildUi(const QString& defaultUsername, const QString& defaultRoomName)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header: the game this room will play.
    auto* header = new LobbyBand(this);
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(18, 12, 18, 12);
    headerLay->setSpacing(12);
    m_tile = new CartridgeTile(CartridgeTile::Size::Small, header);
    headerLay->addWidget(m_tile);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel(tr("Create Room").toUpper(), header);
    title->setFont(LobbyTheme::pixelFont(15, 0.5));
    titles->addWidget(title);
    m_gameLabel = new QLabel(header);
    m_gameLabel->setFont(LobbyTheme::uiFont(11));
    m_gameLabel->setProperty("role", "muted");
    m_gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_gameLabel);
    headerLay->addLayout(titles, 1);
    root->addWidget(header);

    auto* rule = new QFrame(this);
    rule->setObjectName("FieldRule");
    root->addWidget(rule);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(18, 14, 18, 14);
    body->setSpacing(13);

    const auto makeFieldLabel = [this](const QString& text) {
        auto* label = new QLabel(text.toUpper(), this);
        QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        label->setFont(font);
        label->setProperty("role", "muted");
        return label;
    };
    const auto makeSegment = [this](const QString& text) {
        auto* button = new QPushButton(text, this);
        button->setProperty("seg", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        return button;
    };

    auto* nameBox = new QVBoxLayout;
    nameBox->setSpacing(6);
    nameBox->addWidget(makeFieldLabel(tr("Room name")));
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(48);
    m_nameEdit->setMinimumHeight(38);
    if (!defaultRoomName.isEmpty())
        m_nameEdit->setText(defaultRoomName);
    else if (!defaultUsername.isEmpty())
        m_nameEdit->setText(QString(tr("%1's Room")).arg(defaultUsername));
    else
        m_nameEdit->setPlaceholderText(tr("My Room"));
    nameBox->addWidget(m_nameEdit);
    body->addLayout(nameBox);

    auto* choices = new QHBoxLayout;
    choices->setSpacing(14);

    auto* playersBox = new QVBoxLayout;
    playersBox->setSpacing(6);
    playersBox->addWidget(makeFieldLabel(tr("Players")));
    auto* playersSeg = new QFrame(this);
    playersSeg->setObjectName("Seg");
    auto* playersLay = new QHBoxLayout(playersSeg);
    playersLay->setContentsMargins(3, 3, 3, 3);
    playersLay->setSpacing(3);
    m_playersGroup = new QButtonGroup(this);
    for (int seats = 2; seats <= 4; ++seats)
    {
        auto* button = makeSegment(QString::number(seats));
        button->setObjectName(QString("PlayersSeg%1").arg(seats));
        m_playersGroup->addButton(button, seats);
        playersLay->addWidget(button);
    }
    m_playersGroup->button(2)->setChecked(true);
    playersBox->addWidget(playersSeg);
    choices->addLayout(playersBox);

    auto* joinBox = new QVBoxLayout;
    joinBox->setSpacing(6);
    joinBox->addWidget(makeFieldLabel(tr("Who can join")));
    auto* joinSeg = new QFrame(this);
    joinSeg->setObjectName("Seg");
    auto* joinLay = new QHBoxLayout(joinSeg);
    joinLay->setContentsMargins(3, 3, 3, 3);
    joinLay->setSpacing(3);
    m_anyoneButton = makeSegment(tr("Anyone"));
    m_anyoneButton->setChecked(true);
    m_anyoneButton->setIcon(QIcon(LobbyTheme::icon(QStringLiteral("global-line"), LobbyTheme::tokens().ink2, 14, devicePixelRatioF())));
    m_passwordButton = makeSegment(tr("Password"));
    m_passwordButton->setObjectName("PasswordSeg");
    m_passwordButton->setIcon(QIcon(LobbyTheme::icon(QStringLiteral("lock-line"), LobbyTheme::tokens().ink2, 14, devicePixelRatioF())));
    auto* joinGroup = new QButtonGroup(this);
    joinGroup->addButton(m_anyoneButton);
    joinGroup->addButton(m_passwordButton);
    joinLay->addWidget(m_anyoneButton, 1);
    joinLay->addWidget(m_passwordButton, 1);
    joinBox->addWidget(joinSeg);
    choices->addLayout(joinBox, 1);
    body->addLayout(choices);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText(tr("Room password"));
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setVisible(false);
    body->addWidget(m_passwordEdit);

    // Who the room takes, by connection. The server checks both on every join.
    auto* limits = new QHBoxLayout;
    limits->setSpacing(14);
    auto* pingBox = new QVBoxLayout;
    pingBox->setSpacing(6);
    pingBox->addWidget(makeFieldLabel(tr("Max ping")));
    m_maxPingCombo = new QComboBox(this);
    m_maxPingCombo->setObjectName("MaxPingCombo");
    m_maxPingCombo->setMinimumHeight(34);
    fillPingLimitCombo(m_maxPingCombo);
    m_maxPingCombo->setToolTip(tr("Players whose ping to you is higher can't join. "
                                  "Their own measurement is used, or an estimate from where they are."));
    pingBox->addWidget(m_maxPingCombo);
    limits->addLayout(pingBox, 1);

    auto* wifiBox = new QVBoxLayout;
    wifiBox->setSpacing(6);
    wifiBox->addWidget(makeFieldLabel(tr("Connection")));
    auto* wifiSeg = new QFrame(this);
    wifiSeg->setObjectName("Seg");
    auto* wifiLay = new QHBoxLayout(wifiSeg);
    wifiLay->setContentsMargins(3, 3, 3, 3);
    wifiLay->setSpacing(3);
    m_anyConnectionButton = makeSegment(tr("Any"));
    m_anyConnectionButton->setChecked(true);
    m_anyConnectionButton->setIcon(QIcon(LobbyTheme::icon(QStringLiteral("wifi-line"), LobbyTheme::tokens().ink2, 14, devicePixelRatioF())));
    m_wiredOnlyButton = makeSegment(tr("Wired only"));
    m_wiredOnlyButton->setObjectName("WiredOnlySeg");
    const qreal ratio = devicePixelRatioF();
    QPixmap wiredIcon(QSize(14, 14) * ratio);
    wiredIcon.setDevicePixelRatio(ratio);
    wiredIcon.fill(Qt::transparent);
    {
        QPainter painter(&wiredIcon);
        ConnectionGlyph::paintSymbol(painter, QRectF(0, 0, 14, 14), QStringLiteral("lan"), LobbyTheme::tokens().ink2);
    }
    m_wiredOnlyButton->setIcon(QIcon(wiredIcon));
    m_wiredOnlyButton->setToolTip(tr("Players on Wi-Fi can't join."));
    auto* wifiGroup = new QButtonGroup(this);
    wifiGroup->addButton(m_anyConnectionButton);
    wifiGroup->addButton(m_wiredOnlyButton);
    wifiLay->addWidget(m_anyConnectionButton, 1);
    wifiLay->addWidget(m_wiredOnlyButton, 1);
    wifiBox->addWidget(wifiSeg);
    limits->addLayout(wifiBox, 1);
    body->addLayout(limits);

    m_liveReplaySwitch = new ToggleSwitch(tr("Allow Spectators"), this);
    m_liveReplaySwitch->setObjectName("LiveReplaySwitch");
    body->addWidget(m_liveReplaySwitch);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setFont(LobbyTheme::uiFont(11));
    m_statusLabel->setProperty("role", "bad");
    m_statusLabel->setWordWrap(true);
    body->addWidget(m_statusLabel);
    root->addLayout(body);

    auto* footRule = new QFrame(this);
    footRule->setObjectName("FieldRule");
    root->addWidget(footRule);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    footer->addStretch(1);
    m_cancelButton = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
    m_createButton = new LobbyButton(tr("Create"), LobbyButton::Kind::Primary, this);
    m_createButton->setObjectName("CreateButton");
    m_createButton->setIconName(QStringLiteral("add-line"));
    m_createButton->setPixelLabel(true);
    m_createButton->setButtonSize(LobbyButton::Size::Large);
    m_cancelButton->setButtonSize(LobbyButton::Size::Large);
    m_createButton->setDefault(true); // explicitly: Enter = Create when focus is on Create
    footer->addWidget(m_cancelButton);
    footer->addWidget(m_createButton);
    root->addLayout(footer);

    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &CreateRoomDialog::onCreateClicked);

    // Validation hooks
    connect(m_nameEdit,       &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
    connect(m_passwordButton, &QPushButton::toggled,   this, &CreateRoomDialog::onPasswordToggled);
    connect(m_passwordEdit,   &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
}

QString CreateRoomDialog::displayGameName(const QString& goodName, const QString& filePath)
{
    QString name = goodName.trimmed();
    if (name.isEmpty() || name.endsWith("(unknown rom)") || name.endsWith("(unknown disk)"))
    {
        name = QFileInfo(filePath).fileName();
    }
    // Strip "(unknown rom)" suffix even from otherwise-named entries, matches
    // what the Kaillera dialog does.
    const QString suffix = " (unknown rom)";
    if (name.endsWith(suffix))
        name.chop(suffix.length());
    return name;
}

QString CreateRoomDialog::pingLimitText(int maxPingMs)
{
    return maxPingMs > 0 ? tr("≤ %1 ms").arg(maxPingMs) : tr("Any ping");
}

void CreateRoomDialog::fillPingLimitCombo(QComboBox* combo)
{
    const qreal ratio = combo->devicePixelRatioF();
    combo->setIconSize(QSize(16, 14));
    for (const int limit : pingLimits())
        combo->addItem(QIcon(LobbyTheme::pingLimitIcon(limit, LobbyTheme::tokens().ink2, ratio)),
                       pingLimitText(limit), limit);
}

void CreateRoomDialog::onPasswordToggled(bool enabled)
{
    m_passwordEdit->setEnabled(enabled);
    m_passwordEdit->setVisible(enabled);
    if (!enabled)
        m_passwordEdit->clear();
    validateInput();
}

void CreateRoomDialog::validateInput()
{
    const QString name = m_nameEdit->text().trimmed();
    const bool   hasRom = !m_romMd5.isEmpty();
    const bool passwordRequired = m_passwordButton->isChecked();
    const QString pwd  = m_passwordEdit->text();

    QString reason;
    if (name.isEmpty())
        reason = tr("Room name is required.");
    else if (!hasRom)
        reason = tr("Add a ROM to your library before creating a room.");
    else if (passwordRequired && pwd.isEmpty())
        reason = tr("Password cannot be empty when enabled.");

    m_statusLabel->setText(reason);
    m_statusLabel->setVisible(!reason.isEmpty());
    m_createButton->setEnabled(reason.isEmpty());
    adjustSize();
}

void CreateRoomDialog::onCreateClicked()
{
    // The legacy room seeds stay at their loadDefaults() values.
    m_name = m_nameEdit->text().trimmed();
    // m_romName / m_romMd5 were set from the lobby picker in the constructor.
    m_romRegion  = ""; // baked into the ROM; resolved later via md5 lookup
    m_maxPlayers = m_playersGroup->checkedId();
    m_password   = m_passwordButton->isChecked() ? m_passwordEdit->text() : QString();
    m_maxPing    = m_maxPingCombo->currentData().toInt();
    m_wiredOnly  = m_wiredOnlyButton->isChecked();

    saveDefaults();
    setFormEnabled(false);
    m_statusLabel->setText(tr("Creating room..."));
    m_statusLabel->show();
    adjustSize();
    emit createRequested();
}

void CreateRoomDialog::showCreateFailure(const QString& reason)
{
    setFormEnabled(true);
    QString human = reason;
    if (reason == "already_in_room") human = tr("You're already in a room. Leave it first.");
    else if (reason == "invalid_payload") human = tr("Server rejected the room settings.");
    else if (reason == "not_enough_seats") human = tr("It needs a seat for everyone on this PC.");
    m_statusLabel->setText(QString(tr("Couldn't create room: %1")).arg(human));
    m_statusLabel->show();
    adjustSize();
}

void CreateRoomDialog::setFormEnabled(bool enabled)
{
    m_nameEdit->setEnabled(enabled);
    for (auto* button : m_playersGroup->buttons())
        button->setEnabled(enabled && m_playersGroup->id(button) >= m_minimumPlayers);
    m_anyoneButton->setEnabled(enabled);
    m_passwordButton->setEnabled(enabled);
    m_maxPingCombo->setEnabled(enabled);
    m_anyConnectionButton->setEnabled(enabled);
    m_wiredOnlyButton->setEnabled(enabled);
    m_liveReplaySwitch->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled && m_passwordButton->isChecked());
    m_createButton->setEnabled(enabled);
}

void CreateRoomDialog::loadDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    if (s.contains("MaxPlayers"))
    {
        if (auto* button = m_playersGroup->button(s.value("MaxPlayers").toInt()))
            button->setChecked(true);
    }
    // Seed legacy room fields for older clients. Current clients publish their
    // own rollback settings after entering the room.
    if (s.contains("Delay")) m_delay = s.value("Delay").toInt();
    m_prediction = 7;
    const int maxPing = m_maxPingCombo->findData(s.value("MaxPing", 0).toInt());
    m_maxPingCombo->setCurrentIndex(std::max(0, maxPing));
    (s.value("WiredOnly", false).toBool() ? m_wiredOnlyButton : m_anyConnectionButton)->setChecked(true);
    s.endGroup();
    m_liveReplaySwitch->setChecked(s.value("RollbackLobby/LiveReplayEnabled", true).toBool());
}

void CreateRoomDialog::saveDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    // The game selection is persisted by the lobby's shared picker, not here.
    s.setValue("MaxPlayers", m_maxPlayers);
    s.setValue("MaxPing", m_maxPing);
    s.setValue("WiredOnly", m_wiredOnly);
    // Rollback-setting persistence lives in the room view.
    s.endGroup();
    // The room view publishes this preference when the new room arrives.
    s.setValue("RollbackLobby/LiveReplayEnabled", m_liveReplaySwitch->isChecked());
}

#endif // NETPLAY
