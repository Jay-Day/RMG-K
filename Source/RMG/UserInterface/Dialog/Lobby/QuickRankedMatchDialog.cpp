/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "QuickRankedMatchDialog.hpp"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QIcon>

using namespace UserInterface::Dialog;

QuickRankedMatchDialog::QuickRankedMatchDialog(const QString& gameName, const CreateRoomRankedOptions& ranked,
                                               QWidget* parent)
    : QDialog(parent), m_rankedInfo(ranked)
{
    setWindowTitle(tr("Quick Ranked Match"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Search for a ranked set. You're matched with players who pick the same "
                                "set and whose opponent range includes you."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout;
    m_form = form;
    auto* gameLabel = new QLabel(gameName.isEmpty() ? QStringLiteral("—") : gameName, this);
    gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Game:"), gameLabel);

    m_formatCombo = new QComboBox(this);
    m_formatCombo->setObjectName("QuickRankedFormat");
    for (int i = 0; i < m_rankedInfo.formats.size(); i++)
    {
        const auto& format = m_rankedInfo.formats[i];
        m_formatCombo->addItem(format.teamSize > 1
            ? tr("%1v%1 team battle (4 players, team attack on)").arg(format.teamSize) : QString("1v1"), i);
    }
    m_formatCombo->setToolTip(tr("Each format has its own rankings. 2v2: teams are picked on character select, "
                                 "and a match starts once four players are found."));
    form->addRow(tr("Format:"), m_formatCombo);
    form->setRowVisible(m_formatCombo, m_rankedInfo.formats.size() > 1);

    m_teammateEdit = new QLineEdit(this);
    m_teammateEdit->setObjectName("QuickRankedTeammate");
    m_teammateEdit->setPlaceholderText(tr("Random (or a player's name)"));
    m_teammateEdit->setClearButtonEnabled(true);
    m_teammateEdit->setToolTip(tr("2v2: the player you want as your teammate. You're matched only once they "
                                  "search with you as their teammate too. Leave it empty for a random teammate."));
    form->addRow(tr("Teammate:"), m_teammateEdit);

    m_bestOfCombo = new QComboBox(this);
    m_bestOfCombo->setObjectName("QuickRankedBestOf");
    form->addRow(tr("Set length:"), m_bestOfCombo);

    m_rulesetCombo = new QComboBox(this);
    m_rulesetCombo->setObjectName("QuickRankedRoster");
    m_rulesetCombo->setToolTip(tr("Which characters are legal. Games with other characters don't count."));
    form->addRow(tr("Roster:"), m_rulesetCombo);

    m_standingLabel = new QLabel(this);
    m_standingLabel->setObjectName("QuickRankedStanding");
    form->addRow(tr("You:"), m_standingLabel);

    m_skillCombo = new QComboBox(this);
    m_skillCombo->setObjectName("QuickRankedOpponents");
    m_skillCombo->addItem(tr("Anyone"), 0);
    m_skillCombo->addItem(tr("Within 1 tier of me"), 1);
    m_skillCombo->addItem(tr("Within 2 tiers of me"), 2);
    m_skillCombo->addItem(tr("Within 3 tiers of me"), 3);
    m_skillCombo->setToolTip(tr("Tiers on the chosen format and roster. Your opponents' ranges must include you too."));
    form->addRow(tr("Opponents:"), m_skillCombo);

    m_pingCombo = new QComboBox(this);
    m_pingCombo->setObjectName("QuickRankedMaxPing");
    m_pingCombo->addItem(tr("Any"), 0);
    m_pingCombo->addItem("60 ms", 60);
    m_pingCombo->addItem("90 ms", 90);
    m_pingCombo->addItem("120 ms", 120);
    m_pingCombo->addItem("160 ms", 160);
    m_pingCombo->setCurrentIndex(m_pingCombo->findData(120));
    m_pingCombo->setToolTip(tr("The highest ping you'll play a ranked set at. Players are matched by their regions' "
                               "usual ping first; if the measured ping is higher, the set doesn't start."));
    form->addRow(tr("Max ping:"), m_pingCombo);

    m_provisionalCheck = new QCheckBox(tr("Allow players still in placement"), this);
    m_provisionalCheck->setChecked(true);
    m_provisionalCheck->setToolTip(tr("New players have no reliable tier yet; match them regardless of the range."));
    form->addRow(QString(), m_provisionalCheck);
    root->addLayout(form);

    m_continuousCheck = new QCheckBox(tr("Keep searching after each set (a new opponent each time)"), this);
    m_continuousCheck->setObjectName("QuickRankedContinuous");
    m_continuousCheck->setChecked(true);
    m_continuousCheck->setToolTip(tr("When a set ends, the room closes after a few seconds and the next search starts "
                                     "with the same settings. Your last opponent is skipped; if nobody else turns up "
                                     "within 2 minutes, you're asked whether to play them again."));
    root->addWidget(m_continuousCheck);

    m_keepTeammateCheck = new QCheckBox(tr("Keep my teammate after each set (when they keep me too)"), this);
    m_keepTeammateCheck->setObjectName("QuickRankedKeepTeammate");
    m_keepTeammateCheck->setToolTip(tr("2v2, with a chosen teammate: when a set ends and your teammate keeps you too, "
                                       "the two of you search again as a team. With a random teammate you can turn "
                                       "this on in the room."));
    root->addWidget(m_keepTeammateCheck);
    connect(m_keepTeammateCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_keepTeammateCheck->isEnabled())
            m_keepTeammateChoice = on;
    });

    auto* btns = new QDialogButtonBox(this);
    QPushButton* search = btns->addButton(tr("Search"), QDialogButtonBox::AcceptRole);
    QPushButton* cancel = btns->addButton(QDialogButtonBox::Cancel);
    cancel->setText(tr("Cancel"));
    search->setDefault(true);
    cancel->setAutoDefault(false);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(search, &QPushButton::clicked, this, &QuickRankedMatchDialog::onSearchClicked);
    root->addWidget(btns);

    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &QuickRankedMatchDialog::onFormatChanged);
    connect(m_rulesetCombo, &QComboBox::currentIndexChanged, this, &QuickRankedMatchDialog::updateStanding);
    connect(m_continuousCheck, &QCheckBox::toggled, this, &QuickRankedMatchDialog::updateKeepTeammate);
    connect(m_teammateEdit, &QLineEdit::textChanged, this, &QuickRankedMatchDialog::updateKeepTeammate);
    onFormatChanged();
    loadDefaults();
}

void QuickRankedMatchDialog::setTeammateSuggestions(const QStringList& names)
{
    auto* completer = new QCompleter(names, m_teammateEdit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    m_teammateEdit->setCompleter(completer);
}

// A teammate is chosen only in 2v2, and keeping one from the start needs a
// chosen teammate and a session.
void QuickRankedMatchDialog::updateKeepTeammate()
{
    const auto* format = currentFormat();
    const bool teams = format != nullptr && format->teamSize > 1;
    m_form->setRowVisible(m_teammateEdit, teams);
    m_keepTeammateCheck->setVisible(teams);
    const bool keepable = m_continuousCheck->isChecked() && !m_teammateEdit->text().trimmed().isEmpty();
    m_keepTeammateCheck->setEnabled(keepable);
    const QSignalBlocker blocker(m_keepTeammateCheck);
    m_keepTeammateCheck->setChecked(keepable && m_keepTeammateChoice);
}

const CreateRoomRankedOptions::Format* QuickRankedMatchDialog::currentFormat() const
{
    const int index = m_formatCombo->currentData().toInt();
    return index >= 0 && index < m_rankedInfo.formats.size() ? &m_rankedInfo.formats[index] : nullptr;
}

void QuickRankedMatchDialog::onFormatChanged()
{
    const auto* format = currentFormat();
    if (format == nullptr)
        return;
    const int bestOf = m_bestOfCombo->currentData().toInt();
    m_bestOfCombo->clear();
    for (int n : format->bestOf)
        m_bestOfCombo->addItem(tr("Best of %1").arg(n), n);
    if (m_bestOfCombo->findData(bestOf) >= 0)
        m_bestOfCombo->setCurrentIndex(m_bestOfCombo->findData(bestOf));
    // keep the roster's place (Vanilla stays Vanilla)
    const int roster = qMax(0, m_rulesetCombo->currentIndex());
    m_rulesetCombo->blockSignals(true);
    m_rulesetCombo->clear();
    for (const auto& ruleset : format->rulesets)
        m_rulesetCombo->addItem(ruleset.second, ruleset.first);
    m_rulesetCombo->setCurrentIndex(qMin(roster, m_rulesetCombo->count() - 1));
    m_rulesetCombo->blockSignals(false);
    updateStanding();
    updateKeepTeammate();
}

void QuickRankedMatchDialog::updateStanding()
{
    const auto* format = currentFormat();
    const QString text = format ? format->standings.value(m_rulesetCombo->currentData().toString()) : QString();
    m_standingLabel->setText(text.isEmpty() ? tr("not rated yet") : text);
}

void QuickRankedMatchDialog::onSearchClicked()
{
    const auto* format = currentFormat();
    if (format == nullptr)
        return;
    m_ladderId         = format->ladderId;
    m_teamSize         = format->teamSize;
    m_bestOf           = m_bestOfCombo->currentData().toInt();
    m_rulesetId        = m_rulesetCombo->currentData().toString();
    m_maxTierGap       = m_skillCombo->currentData().toInt();
    m_allowProvisional = m_provisionalCheck->isChecked();
    m_continuous       = m_continuousCheck->isChecked();
    m_teammate         = m_teamSize > 1 ? m_teammateEdit->text().trimmed() : QString();
    m_keepTeammate     = m_continuous && !m_teammate.isEmpty() && m_keepTeammateCheck->isChecked();
    m_maxPing          = m_pingCombo->currentData().toInt();
    saveDefaults();
    accept();
}

void QuickRankedMatchDialog::loadDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/QuickRanked");
    const QString ladder = s.value("Ladder").toString();
    for (int i = 0; i < m_rankedInfo.formats.size(); i++)
    {
        if (m_rankedInfo.formats[i].ladderId == ladder)
            m_formatCombo->setCurrentIndex(m_formatCombo->findData(i));
    }
    const int bestOf = m_bestOfCombo->findData(s.value("BestOf").toInt());
    if (bestOf >= 0)
        m_bestOfCombo->setCurrentIndex(bestOf);
    const int roster = m_rulesetCombo->findData(s.value("Ruleset").toString());
    if (roster >= 0)
        m_rulesetCombo->setCurrentIndex(roster);
    const int gap = m_skillCombo->findData(s.value("MaxTierGap", 0).toInt());
    if (gap >= 0)
        m_skillCombo->setCurrentIndex(gap);
    m_provisionalCheck->setChecked(s.value("AllowProvisional", true).toBool());
    if (const int ping = m_pingCombo->findData(s.value("MaxPing", 120).toInt()); ping >= 0)
        m_pingCombo->setCurrentIndex(ping);
    m_continuousCheck->setChecked(s.value("Continuous", true).toBool());
    m_keepTeammateChoice = s.value("KeepTeammate", false).toBool();
    s.endGroup();
    updateStanding();
    updateKeepTeammate();
}

void QuickRankedMatchDialog::saveDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/QuickRanked");
    s.setValue("Ladder", m_ladderId);
    s.setValue("BestOf", m_bestOf);
    s.setValue("Ruleset", m_rulesetId);
    s.setValue("MaxTierGap", m_maxTierGap);
    s.setValue("AllowProvisional", m_allowProvisional);
    s.setValue("MaxPing", m_maxPing);
    s.setValue("Continuous", m_continuous);
    // the box's choice, so a search without a chosen teammate keeps it
    s.setValue("KeepTeammate", m_keepTeammateChoice);
    s.endGroup();
}

#endif // NETPLAY
