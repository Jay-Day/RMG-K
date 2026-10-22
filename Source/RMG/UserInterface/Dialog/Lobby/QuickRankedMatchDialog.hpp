/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef QUICKRANKEDMATCHDIALOG_HPP
#define QUICKRANKEDMATCHDIALOG_HPP

#ifdef NETPLAY

#include "CreateRoomDialog.hpp"

#include <QDialog>

class QComboBox;
class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;

namespace UserInterface
{
namespace Dialog
{

// Picks the set a Quick Ranked Match searches for: format, set length,
// roster and which opponents to accept. The lobby matches players who chose
// the same set and whose ranges accept each other. Remembers the last choice.
class QuickRankedMatchDialog : public QDialog
{
    Q_OBJECT

public:
    // ranked.available must be true, with at least one format.
    QuickRankedMatchDialog(const QString& gameName, const CreateRoomRankedOptions& ranked,
                           QWidget* parent = nullptr);
    ~QuickRankedMatchDialog() override = default;

    // Valid after exec() returns Accepted.
    QString ladderId()         const { return m_ladderId; }
    int     teamSize()         const { return m_teamSize; }
    int     bestOf()           const { return m_bestOf; }
    QString rulesetId()        const { return m_rulesetId; }
    int     maxTierGap()       const { return m_maxTierGap; }
    bool    allowProvisional() const { return m_allowProvisional; }
    int     maxPing()          const { return m_maxPing; }
    // keep searching after each set, with a new opponent each time
    bool    continuous()       const { return m_continuous; }
    // 2v2: the player wanted as teammate, empty = a random one. The search
    // waits until that player searches with this one as theirs.
    QString teammate()         const { return m_teammate; }
    // 2v2 sessions with a chosen teammate: search with each set's teammate
    // again when they keep the team too
    bool    keepTeammate()     const { return m_keepTeammate; }

    // names offered while typing a teammate (players in the lobby)
    void setTeammateSuggestions(const QStringList& names);

private slots:
    void onFormatChanged();
    void updateStanding();
    void updateKeepTeammate();
    void onSearchClicked();

private:
    const CreateRoomRankedOptions::Format* currentFormat() const;
    void loadDefaults();
    void saveDefaults();

    CreateRoomRankedOptions m_rankedInfo;

    QFormLayout* m_form           = nullptr;
    QLineEdit* m_teammateEdit     = nullptr;
    QComboBox* m_formatCombo      = nullptr;
    QComboBox* m_bestOfCombo      = nullptr;
    QComboBox* m_rulesetCombo     = nullptr;
    QComboBox* m_skillCombo       = nullptr;
    QComboBox* m_pingCombo        = nullptr;
    QCheckBox* m_provisionalCheck = nullptr;
    QCheckBox* m_continuousCheck  = nullptr;
    QCheckBox* m_keepTeammateCheck = nullptr;
    QLabel*    m_standingLabel    = nullptr;

    QString m_ladderId;
    int     m_teamSize = 1;
    int     m_bestOf = 0;
    QString m_rulesetId;
    int     m_maxTierGap = 0;
    bool    m_allowProvisional = true;
    int     m_maxPing = 120;
    bool    m_continuous = true;
    QString m_teammate;
    bool    m_keepTeammate = false;
    // the box's choice, kept while it's disabled (no teammate chosen)
    bool    m_keepTeammateChoice = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // QUICKRANKEDMATCHDIALOG_HPP
