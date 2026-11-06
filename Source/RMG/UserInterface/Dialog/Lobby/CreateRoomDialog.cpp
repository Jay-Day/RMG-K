/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "CreateRoomDialog.hpp"

#include "LobbyWidgets.hpp"

#include <algorithm>

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

#include <algorithm>

using namespace UserInterface::Dialog;

namespace
{
    // The highest pings a ranked room can ask for (ms); 0 takes anyone.
    constexpr int kRankedPings[] = { 0, 60, 90, 120, 160 };
}

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                                   const QString& gameName, const QString& gameMd5,
                                   const CreateRoomRankedOptions& ranked,
                                   QWidget* parent)
    : QDialog(parent), m_rankedInfo(ranked)
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
    // a ranked seat is one account at one PC
    m_localPlayers = players > 1;
    if (m_localPlayers && m_rankedInfo.available)
    {
        m_rankedSwitch->setChecked(false);
        m_rankedSwitch->setEnabled(false);
        m_rankedSwitch->setSubText(tr("Ranked takes one player per PC. Remove your local players first."));
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
    const auto makeSegBar = [this](QHBoxLayout** lay) {
        auto* bar = new QFrame(this);
        bar->setObjectName("Seg");
        *lay = new QHBoxLayout(bar);
        (*lay)->setContentsMargins(3, 3, 3, 3);
        (*lay)->setSpacing(3);
        return bar;
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
    QHBoxLayout* playersLay = nullptr;
    auto* playersSeg = makeSegBar(&playersLay);
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
    QHBoxLayout* joinLay = nullptr;
    auto* joinSeg = makeSegBar(&joinLay);
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
    // Hidden for a ranked set, which has its own maximum ping.
    m_joinLimits = new QWidget(this);
    auto* limits = new QHBoxLayout(m_joinLimits);
    limits->setContentsMargins(0, 0, 0, 0);
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
    m_wiredOnlyButton->setIcon(QIcon(LobbyTheme::icon(QStringLiteral("router-line"), LobbyTheme::tokens().ink2, 14, devicePixelRatioF())));
    m_wiredOnlyButton->setToolTip(tr("Players on Wi-Fi can't join."));
    auto* wifiGroup = new QButtonGroup(this);
    wifiGroup->addButton(m_anyConnectionButton);
    wifiGroup->addButton(m_wiredOnlyButton);
    wifiLay->addWidget(m_anyConnectionButton, 1);
    wifiLay->addWidget(m_wiredOnlyButton, 1);
    wifiBox->addWidget(wifiSeg);
    limits->addLayout(wifiBox, 1);
    body->addWidget(m_joinLimits);

    m_liveReplaySwitch = new ToggleSwitch(tr("Live replay for spectators"), this);
    m_liveReplaySwitch->setObjectName("LiveReplaySwitch");
    m_liveReplaySwitch->setSubText(tr("Shows in Live now for anyone to watch, with spectator chat"));
    body->addWidget(m_liveReplaySwitch);

    // Ranked: sets on the game's ladder. Hidden when the server doesn't run
    // ranked at all; disabled with the reason when this game or account can't
    // play it.
    const bool teams = std::any_of(m_rankedInfo.formats.begin(), m_rankedInfo.formats.end(),
        [](const CreateRoomRankedOptions::Format& f) { return f.teamSize > 1; });
    m_rankedSwitch = new ToggleSwitch(tr("Ranked set"), this);
    m_rankedSwitch->setObjectName("RankedSwitch");
    if (m_rankedInfo.available)
    {
        m_rankedSwitch->setSubText(teams ? tr("1v1 or 2v2, counts toward ratings")
                                         : tr("1v1, counts toward ratings"));
        if (!m_rankedInfo.formats.isEmpty())
            m_rankedSwitch->setToolTip(tr("Plays on the %1 ladder.").arg(m_rankedInfo.formats.first().ladderName));
    }
    else
    {
        m_rankedSwitch->setSubText(m_rankedInfo.unavailableReason);
    }
    m_rankedSwitch->setVisible(m_rankedInfo.available || !m_rankedInfo.unavailableReason.isEmpty());
    m_rankedSwitch->setEnabled(m_rankedInfo.available);
    body->addWidget(m_rankedSwitch);

    m_rankedOptions = new QWidget(this);
    auto* rankedLay = new QVBoxLayout(m_rankedOptions);
    rankedLay->setContentsMargins(0, 0, 0, 0);
    rankedLay->setSpacing(13);

    m_formatBox = new QWidget(m_rankedOptions);
    auto* formatBoxLay = new QVBoxLayout(m_formatBox);
    formatBoxLay->setContentsMargins(0, 0, 0, 0);
    formatBoxLay->setSpacing(6);
    formatBoxLay->addWidget(makeFieldLabel(tr("Format")));
    QHBoxLayout* formatLay = nullptr;
    auto* formatSeg = makeSegBar(&formatLay);
    m_formatGroup = new QButtonGroup(this);
    for (int i = 0; i < m_rankedInfo.formats.size(); i++)
    {
        const auto& format = m_rankedInfo.formats[i];
        auto* button = makeSegment(format.teamSize > 1 ? tr("%1v%1 teams").arg(format.teamSize) : QStringLiteral("1v1"));
        button->setObjectName(QString("FormatSeg%1").arg(i));
        button->setToolTip(format.teamSize > 1
            ? tr("%1v%1 team battle (4 players, team attack on)").arg(format.teamSize)
            : tr("Each format has its own rankings. 2v2: pick teams on character select, any ports."));
        m_formatGroup->addButton(button, i);
        formatLay->addWidget(button, 1);
    }
    if (auto* first = m_formatGroup->button(0))
        first->setChecked(true);
    formatBoxLay->addWidget(formatSeg);
    m_formatBox->setVisible(m_rankedInfo.formats.size() > 1);
    rankedLay->addWidget(m_formatBox);

    auto* rosterRow = new QHBoxLayout;
    rosterRow->setSpacing(14);
    auto* rosterBox = new QVBoxLayout;
    rosterBox->setSpacing(6);
    rosterBox->addWidget(makeFieldLabel(tr("Roster")));
    auto* rosterSeg = makeSegBar(&m_rulesetLay);
    rosterSeg->setToolTip(tr("Which characters are legal. Games with other characters don't count."));
    m_rulesetGroup = new QButtonGroup(this);
    rosterBox->addWidget(rosterSeg);
    rosterRow->addLayout(rosterBox, 1);
    auto* setBox = new QVBoxLayout;
    setBox->setSpacing(6);
    setBox->addWidget(makeFieldLabel(tr("Set length")));
    m_bestOfLabel = new QLabel(this);
    m_bestOfLabel->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    m_bestOfLabel->setProperty("role", "ink2");
    m_bestOfLabel->setMinimumHeight(36);
    setBox->addWidget(m_bestOfLabel);
    rosterRow->addLayout(setBox);
    rankedLay->addLayout(rosterRow);

    auto* rankedPingBox = new QVBoxLayout;
    rankedPingBox->setSpacing(6);
    rankedPingBox->addWidget(makeFieldLabel(tr("Max ping")));
    QHBoxLayout* pingLay = nullptr;
    auto* pingSeg = makeSegBar(&pingLay);
    pingSeg->setToolTip(tr("The highest ping you'll play a ranked set at. Players are matched by their regions' "
                           "usual ping first; if the measured ping is higher, the set doesn't start."));
    m_pingGroup = new QButtonGroup(this);
    for (const int ping : kRankedPings)
    {
        auto* button = makeSegment(ping == 0 ? tr("Any") : QString("%1 ms").arg(ping));
        button->setObjectName(QString("PingSeg%1").arg(ping));
        m_pingGroup->addButton(button, ping);
        pingLay->addWidget(button, 1);
    }
    m_pingGroup->button(120)->setChecked(true);
    rankedPingBox->addWidget(pingSeg);
    rankedLay->addLayout(rankedPingBox);
    m_rankedOptions->setVisible(false);
    body->addWidget(m_rankedOptions);

    connect(m_formatGroup, &QButtonGroup::idClicked, this, &CreateRoomDialog::onFormatChanged);
    onFormatChanged();
    connect(m_rankedSwitch, &QCheckBox::toggled, this, &CreateRoomDialog::onRankedToggled);

    m_seatsHint = new QLabel(tr("You can start alone and add people as they join."), this);
    m_seatsHint->setFont(LobbyTheme::uiFont(11));
    m_seatsHint->setProperty("role", "muted");
    m_seatsHint->setWordWrap(true);
    body->addWidget(m_seatsHint);

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
    adjustSize();
    validateInput();
}

int CreateRoomDialog::selectedFormat() const
{
    const int index = m_formatGroup ? m_formatGroup->checkedId() : -1;
    return index >= 0 && index < m_rankedInfo.formats.size() ? index : -1;
}

void CreateRoomDialog::onFormatChanged()
{
    const int index = selectedFormat();
    if (index < 0)
        return;
    const auto& format = m_rankedInfo.formats[index];
    m_bestOfLabel->setText(tr("Best of %1").arg(format.bestOf));
    // keep the roster's place (Vanilla stays Vanilla)
    int roster = 0;
    const QList<QAbstractButton*> old = m_rulesetGroup->buttons();
    for (int i = 0; i < old.size(); i++)
    {
        if (old[i]->isChecked())
            roster = i;
        m_rulesetGroup->removeButton(old[i]);
        old[i]->deleteLater();
    }
    for (int i = 0; i < format.rulesets.size(); i++)
    {
        auto* button = new QPushButton(format.rulesets[i].second, this);
        button->setProperty("seg", true);
        button->setProperty("rulesetId", format.rulesets[i].first);
        button->setObjectName(QString("RosterSeg%1").arg(i));
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        if (format.standings.contains(format.rulesets[i].first))
            button->setToolTip(format.standings.value(format.rulesets[i].first));
        m_rulesetGroup->addButton(button, i);
        m_rulesetLay->addWidget(button, 1);
    }
    if (auto* button = m_rulesetGroup->button(qMin(roster, int(format.rulesets.size()) - 1)))
        button->setChecked(true);
    updateSeats();
}

void CreateRoomDialog::updateSeats()
{
    const bool ranked = m_rankedSwitch && m_rankedSwitch->isChecked();
    const int index = selectedFormat();
    const int teamSize = index >= 0 ? m_rankedInfo.formats[index].teamSize : 1;
    // A ranked room seats exactly the set's players; the server enforces it too.
    if (ranked)
    {
        if (auto* button = m_playersGroup->button(2 * teamSize))
            button->setChecked(true);
    }
    for (auto* button : m_playersGroup->buttons())
        button->setEnabled(!ranked && m_playersGroup->id(button) >= m_minimumPlayers);
    if (m_seatsHint)
    {
        m_seatsHint->setText(ranked ? tr("A ranked set starts with all %1 players, and nobody joins it later.").arg(2 * teamSize)
                                    : tr("You can start alone and add people as they join."));
    }
}

void CreateRoomDialog::onRankedToggled(bool enabled)
{
    m_rankedOptions->setVisible(enabled);
    m_joinLimits->setVisible(!enabled);
    updateSeats();
    adjustSize();
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
    m_createButton->setEnabled(reason.isEmpty());
}

void CreateRoomDialog::onCreateClicked()
{
    // The legacy room seeds stay at their loadDefaults() values.
    m_name = m_nameEdit->text().trimmed();
    // m_romName / m_romMd5 were set from the lobby picker in the constructor.
    m_romRegion  = ""; // baked into the ROM; resolved later via md5 lookup
    m_maxPlayers = m_playersGroup->checkedId();
    m_password   = m_passwordButton->isChecked() ? m_passwordEdit->text() : QString();
    m_isRanked   = m_rankedSwitch->isEnabled() && m_rankedSwitch->isChecked();
    QAbstractButton* roster = m_rulesetGroup->checkedButton();
    m_rulesetId  = roster ? roster->property("rulesetId").toString() : QString();
    {
        const int index = selectedFormat();
        m_ladderId = index >= 0 ? m_rankedInfo.formats[index].ladderId : QString();
        m_teamSize = index >= 0 ? m_rankedInfo.formats[index].teamSize : 1;
    }
    m_rankedMaxPing = qMax(0, m_pingGroup->checkedId());
    // Ranked has its own maximum ping and takes any logged-in player.
    m_maxPing    = m_isRanked ? 0 : m_maxPingCombo->currentData().toInt();
    m_wiredOnly  = !m_isRanked && m_wiredOnlyButton->isChecked();

    saveDefaults();
    setFormEnabled(false);
    m_statusLabel->setText(tr("Creating room..."));
    emit createRequested();
}

void CreateRoomDialog::showCreateFailure(const QString& reason)
{
    setFormEnabled(true);
    QString human = reason;
    if (reason == "already_in_room") human = tr("You're already in a room. Leave it first.");
    else if (reason == "invalid_payload") human = tr("Server rejected the room settings.");
    else if (reason == "not_verified") human = tr("Log into an account to create ranked rooms.");
    else if (reason == "ranked_unavailable") human = tr("This game can't be played ranked on this server.");
    else if (reason == "not_enough_seats") human = tr("It needs a seat for everyone on this PC.");
    else if (reason == "local_players") human = tr("Ranked takes one player per PC. Remove your local players first.");
    m_statusLabel->setText(QString(tr("Couldn't create room: %1")).arg(human));
}

void CreateRoomDialog::setFormEnabled(bool enabled)
{
    m_nameEdit->setEnabled(enabled);
    m_anyoneButton->setEnabled(enabled);
    m_passwordButton->setEnabled(enabled);
    m_maxPingCombo->setEnabled(enabled);
    m_anyConnectionButton->setEnabled(enabled);
    m_wiredOnlyButton->setEnabled(enabled);
    m_liveReplaySwitch->setEnabled(enabled);
    m_rankedSwitch->setEnabled(enabled && m_rankedInfo.available && !m_localPlayers);
    m_rankedOptions->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled && m_passwordButton->isChecked());
    m_createButton->setEnabled(enabled);
    if (enabled)
        updateSeats();
    else
    {
        for (auto* button : m_playersGroup->buttons())
            button->setEnabled(false);
    }
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
    if (auto* ping = m_pingGroup->button(s.value("RankedMaxPing", 120).toInt()))
        ping->setChecked(true);
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
    // A ranked room's seats and ping come from the ranked options, not the
    // casual choices.
    if (m_isRanked)
        s.setValue("RankedMaxPing", m_rankedMaxPing);
    else
    {
        s.setValue("MaxPlayers", m_maxPlayers);
        s.setValue("MaxPing", m_maxPing);
        s.setValue("WiredOnly", m_wiredOnly);
    }
    // Rollback-setting persistence lives in the room view.
    s.endGroup();
    // The room view publishes this preference when the new room arrives.
    s.setValue("RollbackLobby/LiveReplayEnabled", m_liveReplaySwitch->isChecked());
}

#endif // NETPLAY
