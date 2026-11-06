/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "QuickRankedMatchDialog.hpp"

#include "LobbyWidgets.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QIcon>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    // The highest pings a ranked search can ask for (ms); 0 takes anyone.
    constexpr int kRankedPings[] = { 0, 60, 90, 120, 160 };

    QLabel* fieldLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        label->setFont(font);
        label->setProperty("role", "muted");
        return label;
    }

    QPushButton* segment(const QString& text, QWidget* parent)
    {
        auto* button = new QPushButton(text, parent);
        button->setProperty("seg", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        return button;
    }

    QFrame* segBar(QHBoxLayout** lay, QWidget* parent)
    {
        auto* bar = new QFrame(parent);
        bar->setObjectName("Seg");
        *lay = new QHBoxLayout(bar);
        (*lay)->setContentsMargins(3, 3, 3, 3);
        (*lay)->setSpacing(3);
        return bar;
    }

    QWidget* block(QWidget* parent, const QString& caption, QWidget* field)
    {
        auto* box = new QWidget(parent);
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        lay->addWidget(fieldLabel(caption, box));
        lay->addWidget(field);
        return box;
    }
} // namespace

QuickRankedMatchDialog::QuickRankedMatchDialog(const QString& gameName, const CreateRoomRankedOptions& ranked,
                                               QWidget* parent)
    : QDialog(parent), m_rankedInfo(ranked)
{
    setWindowTitle(tr("Quick Ranked Match"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setFixedWidth(490);
    const Tokens& t = LobbyTheme::tokens();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header: a gold shield, RANKED and the game
    auto* header = new LobbyBand(this);
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(18, 12, 18, 12);
    headerLay->setSpacing(12);
    auto* shield = new IconLabel(QStringLiteral("shield-star-fill"), Role::Gold, 26, header);
    headerLay->addWidget(shield);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel(tr("Ranked").toUpper(), header);
    title->setFont(LobbyTheme::pixelFont(17, 0.5));
    title->setStyleSheet(QString("color: %1;").arg(t.goldInk.name()));
    titles->addWidget(title);
    auto* gameLabel = new QLabel(gameName.isEmpty() ? QStringLiteral("—") : gameName, header);
    gameLabel->setFont(LobbyTheme::uiFont(11));
    gameLabel->setProperty("role", "muted");
    gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(gameLabel);
    headerLay->addLayout(titles, 1);
    root->addWidget(header);

    auto* rule = new QFrame(this);
    rule->setObjectName("FieldRule");
    root->addWidget(rule);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(18, 14, 18, 14);
    body->setSpacing(13);

    auto* intro = new QLabel(tr("Search for a ranked set against anyone looking for the same format and roster. "
                                "After each set the next search starts by itself, with a new opponent, until you "
                                "stop the session."), this);
    intro->setWordWrap(true);
    intro->setFont(LobbyTheme::uiFont(12));
    intro->setProperty("role", "ink2");
    body->addWidget(intro);

    // Your standing on the chosen roster
    auto* standing = new QFrame(this);
    standing->setObjectName("RankedStanding");
    standing->setStyleSheet(QString("QFrame#RankedStanding { background-color: %1; border: 1px solid %2; border-radius: 10px; }")
                                .arg(LobbyTheme::css(t.goldTint), LobbyTheme::css(t.goldLine)));
    auto* standingLay = new QHBoxLayout(standing);
    standingLay->setContentsMargins(12, 9, 12, 9);
    standingLay->setSpacing(10);
    auto* standingIcon = new IconLabel(QStringLiteral("shield-star-line"), Role::Gold, 20, standing);
    standingLay->addWidget(standingIcon);
    m_standingLabel = new QLabel(standing);
    m_standingLabel->setObjectName("QuickRankedStanding");
    m_standingLabel->setFont(LobbyTheme::uiFont(13, QFont::DemiBold));
    m_standingLabel->setStyleSheet("background: transparent; border: none;");
    standingLay->addWidget(m_standingLabel, 1);
    body->addWidget(standing);

    QHBoxLayout* formatLay = nullptr;
    auto* formatSeg = segBar(&formatLay, this);
    formatSeg->setToolTip(tr("Each format has its own rankings. 2v2: teams are picked on character select, "
                             "and a match starts once four players are found."));
    m_formatGroup = new QButtonGroup(this);
    for (int i = 0; i < m_rankedInfo.formats.size(); i++)
    {
        const auto& format = m_rankedInfo.formats[i];
        auto* button = segment(format.teamSize > 1 ? tr("%1v%1 teams").arg(format.teamSize) : QStringLiteral("1v1"), formatSeg);
        button->setObjectName(QString("QuickRankedFormat%1").arg(i));
        if (format.teamSize > 1)
            button->setToolTip(tr("%1v%1 team battle (4 players, team attack on)").arg(format.teamSize));
        m_formatGroup->addButton(button, i);
        formatLay->addWidget(button, 1);
    }
    if (auto* first = m_formatGroup->button(0))
        first->setChecked(true);
    m_formatBox = block(this, tr("Format"), formatSeg);
    m_formatBox->setVisible(m_rankedInfo.formats.size() > 1);
    body->addWidget(m_formatBox);

    m_teammateEdit = new QLineEdit(this);
    m_teammateEdit->setObjectName("QuickRankedTeammate");
    m_teammateEdit->setMinimumHeight(38);
    m_teammateEdit->setPlaceholderText(tr("Random (or a player's name)"));
    m_teammateEdit->setClearButtonEnabled(true);
    m_teammateEdit->setToolTip(tr("2v2: the player you want as your teammate. You're matched only once they "
                                  "search with you as their teammate too. Leave it empty for a random teammate."));
    m_teammateBox = block(this, tr("Teammate"), m_teammateEdit);
    body->addWidget(m_teammateBox);

    auto* rosterRow = new QHBoxLayout;
    rosterRow->setSpacing(14);
    auto* rosterSeg = segBar(&m_rulesetLay, this);
    rosterSeg->setToolTip(tr("Which characters are legal. Games with other characters don't count."));
    m_rulesetGroup = new QButtonGroup(this);
    rosterRow->addWidget(block(this, tr("Roster"), rosterSeg), 1);
    m_bestOfLabel = new QLabel(this);
    m_bestOfLabel->setObjectName("QuickRankedBestOf");
    m_bestOfLabel->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    m_bestOfLabel->setProperty("role", "ink2");
    m_bestOfLabel->setMinimumHeight(36);
    rosterRow->addWidget(block(this, tr("Set length"), m_bestOfLabel));
    body->addLayout(rosterRow);

    QHBoxLayout* pingLay = nullptr;
    auto* pingSeg = segBar(&pingLay, this);
    pingSeg->setToolTip(tr("The highest ping you'll play a ranked set at. Players are matched by their regions' "
                           "usual ping first; if the measured ping is higher, the set doesn't start."));
    m_pingGroup = new QButtonGroup(this);
    for (const int ping : kRankedPings)
    {
        auto* button = segment(ping == 0 ? tr("Any") : QString("%1 ms").arg(ping), pingSeg);
        button->setObjectName(QString("QuickRankedMaxPing%1").arg(ping));
        m_pingGroup->addButton(button, ping);
        pingLay->addWidget(button, 1);
    }
    m_pingGroup->button(120)->setChecked(true);
    body->addWidget(block(this, tr("Max ping"), pingSeg));

    m_keepTeammateCheck = new ToggleSwitch(tr("Keep my teammate after each set"), this);
    m_keepTeammateCheck->setObjectName("QuickRankedKeepTeammate");
    static_cast<ToggleSwitch*>(m_keepTeammateCheck)->setSubText(tr("When they keep you too"));
    m_keepTeammateCheck->setToolTip(tr("2v2, with a chosen teammate: when a set ends and your teammate keeps you too, "
                                       "the two of you search again as a team. With a random teammate you can turn "
                                       "this on in the room."));
    body->addWidget(m_keepTeammateCheck);
    connect(m_keepTeammateCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_keepTeammateCheck->isEnabled())
            m_keepTeammateChoice = on;
    });
    root->addLayout(body);

    auto* footRule = new QFrame(this);
    footRule->setObjectName("FieldRule");
    root->addWidget(footRule);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    footer->addStretch(1);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
    cancel->setButtonSize(LobbyButton::Size::Large);
    auto* search = new LobbyButton(tr("Find match"), LobbyButton::Kind::Ranked, this);
    search->setObjectName("QuickRankedSearch");
    search->setButtonSize(LobbyButton::Size::Large);
    search->setPixelLabel(true);
    search->setIconName(QStringLiteral("shield-star-line"));
    search->setDefault(true);
    footer->addWidget(cancel);
    footer->addWidget(search);
    root->addLayout(footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(search, &QPushButton::clicked, this, &QuickRankedMatchDialog::onSearchClicked);

    connect(m_formatGroup, &QButtonGroup::idClicked, this, &QuickRankedMatchDialog::onFormatChanged);
    connect(m_rulesetGroup, &QButtonGroup::idClicked, this, &QuickRankedMatchDialog::updateStanding);
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
// chosen teammate.
void QuickRankedMatchDialog::updateKeepTeammate()
{
    const auto* format = currentFormat();
    const bool teams = format != nullptr && format->teamSize > 1;
    m_teammateBox->setVisible(teams);
    m_keepTeammateCheck->setVisible(teams);
    const bool keepable = !m_teammateEdit->text().trimmed().isEmpty();
    m_keepTeammateCheck->setEnabled(keepable);
    const QSignalBlocker blocker(m_keepTeammateCheck);
    m_keepTeammateCheck->setChecked(keepable && m_keepTeammateChoice);
    adjustSize();
}

const CreateRoomRankedOptions::Format* QuickRankedMatchDialog::currentFormat() const
{
    const int index = m_formatGroup->checkedId();
    return index >= 0 && index < m_rankedInfo.formats.size() ? &m_rankedInfo.formats[index] : nullptr;
}

QString QuickRankedMatchDialog::currentRuleset() const
{
    QAbstractButton* roster = m_rulesetGroup->checkedButton();
    return roster ? roster->property("rulesetId").toString() : QString();
}

void QuickRankedMatchDialog::onFormatChanged()
{
    const auto* format = currentFormat();
    if (format == nullptr)
        return;
    m_bestOfLabel->setText(tr("Best of %1").arg(format->bestOf));
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
    for (int i = 0; i < format->rulesets.size(); i++)
    {
        auto* button = segment(format->rulesets[i].second, this);
        button->setObjectName(QString("QuickRankedRoster%1").arg(i));
        button->setProperty("rulesetId", format->rulesets[i].first);
        m_rulesetGroup->addButton(button, i);
        m_rulesetLay->addWidget(button, 1);
    }
    if (auto* button = m_rulesetGroup->button(qMin(roster, int(format->rulesets.size()) - 1)))
        button->setChecked(true);
    updateStanding();
    updateKeepTeammate();
}

void QuickRankedMatchDialog::updateStanding()
{
    const auto* format = currentFormat();
    const QString text = format ? format->standings.value(currentRuleset()) : QString();
    m_standingLabel->setText(text.isEmpty() ? tr("not rated yet") : text);
}

void QuickRankedMatchDialog::onSearchClicked()
{
    const auto* format = currentFormat();
    if (format == nullptr)
        return;
    m_ladderId         = format->ladderId;
    m_teamSize         = format->teamSize;
    m_rulesetId        = currentRuleset();
    m_teammate         = m_teamSize > 1 ? m_teammateEdit->text().trimmed() : QString();
    m_keepTeammate     = !m_teammate.isEmpty() && m_keepTeammateCheck->isChecked();
    m_maxPing          = qMax(0, m_pingGroup->checkedId());
    QString roster;
    if (QAbstractButton* button = m_rulesetGroup->checkedButton())
        roster = button->text();
    m_formatSummary = QStringList({ m_teamSize > 1 ? QString("%1v%1").arg(m_teamSize) : QStringLiteral("1v1"),
                                    roster, tr("Best of %1").arg(format->bestOf) }).join(QStringLiteral(" · "));
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
        {
            if (auto* button = m_formatGroup->button(i))
                button->setChecked(true);
            onFormatChanged();
        }
    }
    const QString ruleset = s.value("Ruleset").toString();
    for (QAbstractButton* button : m_rulesetGroup->buttons())
    {
        if (button->property("rulesetId").toString() == ruleset)
            button->setChecked(true);
    }
    if (auto* ping = m_pingGroup->button(s.value("MaxPing", 120).toInt()))
        ping->setChecked(true);
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
    s.setValue("Ruleset", m_rulesetId);
    s.setValue("MaxPing", m_maxPing);
    // the box's choice, so a search without a chosen teammate keeps it
    s.setValue("KeepTeammate", m_keepTeammateChoice);
    s.endGroup();
}

#endif // NETPLAY
