/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "CreateRoomDialog.hpp"

#include <algorithm>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QSettings>
#include <QFileInfo>
#include <QIcon>

using namespace UserInterface::Dialog;

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                                   const QString& gameName, const QString& gameMd5,
                                   const CreateRoomRankedOptions& ranked,
                                   QWidget* parent)
    : QDialog(parent), m_rankedInfo(ranked)
{
    setWindowTitle("Create Room");
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    // The game comes from the lobby's shared picker, not a picker of our own.
    m_romName = gameName;
    m_romMd5  = gameMd5;
    buildUi(defaultUsername, defaultRoomName);
    if (m_gameLabel)
        m_gameLabel->setText(gameName.isEmpty() ? QStringLiteral("—") : gameName);
    loadDefaults();
    validateInput();
}

void CreateRoomDialog::buildUi(const QString& defaultUsername, const QString& defaultRoomName)
{
    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout;

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(48);
    if (!defaultRoomName.isEmpty())
        m_nameEdit->setText(defaultRoomName);
    else if (!defaultUsername.isEmpty())
        m_nameEdit->setText(QString("%1's Room").arg(defaultUsername));
    else
        m_nameEdit->setPlaceholderText("My Room");
    form->addRow("Room name:", m_nameEdit);

    m_gameLabel = new QLabel(this);
    m_gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("Game:", m_gameLabel);

    m_maxPlayersSpin = new QSpinBox(this);
    m_maxPlayersSpin->setRange(2, 4);
    m_maxPlayersSpin->setValue(2);
    m_maxPlayersSpin->setSuffix(" players");
    form->addRow("Max players:", m_maxPlayersSpin);

    root->addLayout(form);

    // Rollback settings are not surfaced here. Each player configures local
    // delay and prediction in the room.

    // Optional password (collapsed by default)
    auto* pwRow = new QHBoxLayout;
    m_passwordCheck = new QCheckBox("Password-protect this room", this);
    pwRow->addWidget(m_passwordCheck);
    pwRow->addStretch();
    root->addLayout(pwRow);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Room password");
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setVisible(false);
    root->addWidget(m_passwordEdit);

    // Ranked: 1v1 sets on the game's ladder. Hidden when the server doesn't
    // run ranked at all; disabled with the reason when this game/account
    // can't play it.
    const bool teams = std::any_of(m_rankedInfo.formats.begin(), m_rankedInfo.formats.end(),
        [](const CreateRoomRankedOptions::Format& f) { return f.teamSize > 1; });
    m_rankedCheck = new QCheckBox(teams ? tr("Ranked set (1v1 or 2v2, counts toward ratings)")
                                        : tr("Ranked set (1v1, counts toward ratings)"), this);
    root->addWidget(m_rankedCheck);
    m_rankedOptions = new QWidget(this);
    auto* rankedForm = new QFormLayout(m_rankedOptions);
    rankedForm->setContentsMargins(20, 0, 0, 0);
    m_formatCombo = new QComboBox(m_rankedOptions);
    for (int i = 0; i < m_rankedInfo.formats.size(); i++)
    {
        const auto& format = m_rankedInfo.formats[i];
        m_formatCombo->addItem(format.teamSize > 1
            ? tr("%1v%1 team battle (4 players, team attack on)").arg(format.teamSize) : QString("1v1"), i);
    }
    m_formatCombo->setToolTip(tr("Each format has its own rankings. 2v2: pick teams on character select, any ports."));
    rankedForm->addRow(tr("Format:"), m_formatCombo);
    rankedForm->setRowVisible(m_formatCombo, m_rankedInfo.formats.size() > 1);
    m_bestOfCombo = new QComboBox(m_rankedOptions);
    rankedForm->addRow(tr("Set length:"), m_bestOfCombo);
    m_rulesetCombo = new QComboBox(m_rankedOptions);
    m_rulesetCombo->setToolTip(tr("Which characters are legal. Games with other characters don't count."));
    rankedForm->addRow(tr("Roster:"), m_rulesetCombo);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &CreateRoomDialog::onFormatChanged);
    onFormatChanged();
    m_skillCombo = new QComboBox(m_rankedOptions);
    m_skillCombo->addItem(tr("Anyone"), 0);
    m_skillCombo->addItem(tr("Within 1 tier of me"), 1);
    m_skillCombo->addItem(tr("Within 2 tiers of me"), 2);
    m_skillCombo->addItem(tr("Within 3 tiers of me"), 3);
    rankedForm->addRow(tr("Opponents:"), m_skillCombo);
    m_pingCombo = new QComboBox(m_rankedOptions);
    m_pingCombo->addItem(tr("Any"), 0);
    m_pingCombo->addItem("60 ms", 60);
    m_pingCombo->addItem("90 ms", 90);
    m_pingCombo->addItem("120 ms", 120);
    m_pingCombo->addItem("160 ms", 160);
    m_pingCombo->setCurrentIndex(m_pingCombo->findData(120));
    m_pingCombo->setToolTip(tr("The highest ping you'll play a ranked set at. Players are matched by their regions' "
                               "usual ping first; if the measured ping is higher, the set doesn't start."));
    rankedForm->addRow(tr("Max ping:"), m_pingCombo);
    m_provisionalCheck = new QCheckBox(tr("Allow players still in placement"), m_rankedOptions);
    m_provisionalCheck->setChecked(true);
    m_provisionalCheck->setToolTip(tr("New players have no reliable tier yet; let them join regardless of the range."));
    rankedForm->addRow(QString(), m_provisionalCheck);
    root->addWidget(m_rankedOptions);
    m_rankedOptions->setVisible(false);

    const bool rankedKnown = m_rankedInfo.available || !m_rankedInfo.unavailableReason.isEmpty();
    m_rankedCheck->setVisible(rankedKnown);
    m_rankedCheck->setEnabled(m_rankedInfo.available);
    if (m_rankedInfo.available && !m_rankedInfo.formats.isEmpty())
        m_rankedCheck->setToolTip(tr("Plays on the %1 ladder.").arg(m_rankedInfo.formats.first().ladderName));
    else if (!m_rankedInfo.unavailableReason.isEmpty())
    {
        m_rankedCheck->setText(tr("Ranked set — %1").arg(m_rankedInfo.unavailableReason));
        m_rankedCheck->setToolTip(m_rankedInfo.unavailableReason);
    }
    connect(m_rankedCheck, &QCheckBox::toggled, this, &CreateRoomDialog::onRankedToggled);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: gray;");
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    auto* btns = new QDialogButtonBox(this);
    m_createButton = btns->addButton("Create", QDialogButtonBox::AcceptRole);
    m_cancelButton = btns->addButton(QDialogButtonBox::Cancel);
    m_createButton->setDefault(true); // explicitly: Enter = Create when focus is on Create
    m_cancelButton->setAutoDefault(false);
    m_cancelButton->setDefault(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &CreateRoomDialog::onCreateClicked);
    root->addWidget(btns);

    // Validation hooks
    connect(m_nameEdit,      &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
    connect(m_passwordCheck, &QCheckBox::toggled,     this, &CreateRoomDialog::onPasswordToggled);
    connect(m_passwordEdit,  &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
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

void CreateRoomDialog::onPasswordToggled(bool enabled)
{
    m_passwordEdit->setEnabled(enabled);
    m_passwordEdit->setVisible(enabled);
    if (!enabled)
        m_passwordEdit->clear();
    adjustSize();
    validateInput();
}

void CreateRoomDialog::onFormatChanged()
{
    const int index = m_formatCombo ? m_formatCombo->currentData().toInt() : 0;
    if (index < 0 || index >= m_rankedInfo.formats.size())
        return;
    const auto& format = m_rankedInfo.formats[index];
    const int bestOf = m_bestOfCombo->currentData().toInt();
    m_bestOfCombo->clear();
    for (int n : format.bestOf)
        m_bestOfCombo->addItem(tr("Best of %1").arg(n), n);
    if (m_bestOfCombo->findData(bestOf) >= 0)
        m_bestOfCombo->setCurrentIndex(m_bestOfCombo->findData(bestOf));
    // keep the roster's place (Vanilla stays Vanilla)
    const int roster = qMax(0, m_rulesetCombo->currentIndex());
    m_rulesetCombo->clear();
    for (const auto& ruleset : format.rulesets)
        m_rulesetCombo->addItem(ruleset.second, ruleset.first);
    m_rulesetCombo->setCurrentIndex(qMin(roster, m_rulesetCombo->count() - 1));
    if (m_rankedCheck && m_rankedCheck->isChecked())
        m_maxPlayersSpin->setValue(2 * format.teamSize);
}

void CreateRoomDialog::onRankedToggled(bool enabled)
{
    // A ranked room seats exactly the set's players; the server enforces it too.
    m_rankedOptions->setVisible(enabled);
    if (enabled)
    {
        const int index = m_formatCombo->currentData().toInt();
        const int teamSize = index >= 0 && index < m_rankedInfo.formats.size() ? m_rankedInfo.formats[index].teamSize : 1;
        m_maxPlayersSpin->setValue(2 * teamSize);
    }
    m_maxPlayersSpin->setEnabled(!enabled);
    adjustSize();
    validateInput();
}

void CreateRoomDialog::validateInput()
{
    const QString name = m_nameEdit->text().trimmed();
    const bool   hasRom = !m_romMd5.isEmpty();
    const bool passwordRequired = m_passwordCheck->isChecked();
    const QString pwd  = m_passwordEdit->text();

    QString reason;
    if (name.isEmpty())
        reason = "Room name is required.";
    else if (!hasRom)
        reason = "Add a ROM to your library before creating a room.";
    else if (passwordRequired && pwd.isEmpty())
        reason = "Password cannot be empty when enabled.";

    m_statusLabel->setText(reason);
    m_createButton->setEnabled(reason.isEmpty());
}

void CreateRoomDialog::onCreateClicked()
{
    // The legacy room seeds stay at their loadDefaults() values.
    m_name = m_nameEdit->text().trimmed();
    // m_romName / m_romMd5 were set from the lobby picker in the constructor.
    m_romRegion  = ""; // baked into the ROM; resolved later via md5 lookup
    m_maxPlayers = m_maxPlayersSpin->value();
    m_password   = m_passwordCheck->isChecked() ? m_passwordEdit->text() : QString();
    m_isRanked   = m_rankedCheck->isEnabled() && m_rankedCheck->isChecked();
    m_bestOf     = m_bestOfCombo->currentData().toInt();
    m_rulesetId  = m_rulesetCombo->currentData().toString();
    {
        const int index = m_formatCombo->currentData().toInt();
        const bool known = index >= 0 && index < m_rankedInfo.formats.size();
        m_ladderId = known ? m_rankedInfo.formats[index].ladderId : QString();
        m_teamSize = known ? m_rankedInfo.formats[index].teamSize : 1;
    }
    m_maxTierGap = m_skillCombo->currentData().toInt();
    m_maxPing    = m_pingCombo->currentData().toInt();
    m_allowProvisional = m_provisionalCheck->isChecked();

    saveDefaults();
    setFormEnabled(false);
    m_statusLabel->setText("Creating room...");
    emit createRequested();
}

void CreateRoomDialog::showCreateFailure(const QString& reason)
{
    setFormEnabled(true);
    QString human = reason;
    if (reason == "already_in_room") human = "You're already in a room. Leave it first.";
    else if (reason == "invalid_payload") human = "Server rejected the room settings.";
    else if (reason == "not_verified") human = "Log into an account to create ranked rooms.";
    else if (reason == "ranked_unavailable") human = "This game can't be played ranked on this server.";
    m_statusLabel->setText(QString("Couldn't create room: %1").arg(human));
}

void CreateRoomDialog::setFormEnabled(bool enabled)
{
    m_nameEdit->setEnabled(enabled);
    m_maxPlayersSpin->setEnabled(enabled && !m_rankedCheck->isChecked());
    m_passwordCheck->setEnabled(enabled);
    m_rankedCheck->setEnabled(enabled && m_rankedInfo.available);
    m_rankedOptions->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled && m_passwordCheck->isChecked());
    m_createButton->setEnabled(enabled);
}

void CreateRoomDialog::loadDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    if (s.contains("MaxPlayers")) m_maxPlayersSpin->setValue(s.value("MaxPlayers").toInt());
    if (const int ping = m_pingCombo->findData(s.value("MaxPing", 120).toInt()); ping >= 0)
        m_pingCombo->setCurrentIndex(ping);
    // Seed legacy room fields for older clients. Current clients publish their
    // own rollback settings after entering the room.
    if (s.contains("Delay")) m_delay = s.value("Delay").toInt();
    m_prediction = 7;
    s.endGroup();
}

void CreateRoomDialog::saveDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    // The game selection is persisted by the lobby's shared picker, not here.
    s.setValue("MaxPlayers", m_maxPlayers);
    if (m_isRanked)
        s.setValue("MaxPing", m_maxPing);
    // Rollback-setting persistence lives in the room view.
    s.endGroup();
}

#endif // NETPLAY
