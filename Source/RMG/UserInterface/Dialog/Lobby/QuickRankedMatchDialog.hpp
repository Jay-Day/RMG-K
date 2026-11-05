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

class QButtonGroup;
class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;

namespace UserInterface
{
namespace Dialog
{

// Picks the set a Quick Ranked Match searches for: format, roster and max
// ping (2v2: optionally a teammate). The lobby matches players who chose the
// same format and roster; set length and opponents' skill aren't choices, so
// the queue isn't split. Remembers the last choice.
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
    QString rulesetId()        const { return m_rulesetId; }
    int     maxPing()          const { return m_maxPing; }
    // 2v2: the player wanted as teammate, empty = a random one. The search
    // waits until that player searches with this one as theirs.
    QString teammate()         const { return m_teammate; }
    // 2v2 sessions with a chosen teammate: search with each set's teammate
    // again when they keep the team too
    bool    keepTeammate()     const { return m_keepTeammate; }
    // the chosen set in a few words, e.g. "1v1 · Vanilla roster · Best of 3"
    QString formatSummary()    const { return m_formatSummary; }

    // names offered while typing a teammate (players in the lobby)
    void setTeammateSuggestions(const QStringList& names);

private slots:
    void onFormatChanged();
    void updateStanding();
    void updateKeepTeammate();
    void onSearchClicked();

private:
    const CreateRoomRankedOptions::Format* currentFormat() const;
    QString currentRuleset() const;
    void loadDefaults();
    void saveDefaults();

    CreateRoomRankedOptions m_rankedInfo;

    QWidget*   m_formatBox        = nullptr;
    QWidget*   m_teammateBox      = nullptr;
    QLineEdit* m_teammateEdit     = nullptr;
    QButtonGroup* m_formatGroup   = nullptr; // by index into m_rankedInfo.formats
    QButtonGroup* m_rulesetGroup  = nullptr; // the format's rosters, rebuilt with it
    QHBoxLayout*  m_rulesetLay    = nullptr;
    QButtonGroup* m_pingGroup     = nullptr; // by max ping in ms, 0 = any
    QCheckBox* m_keepTeammateCheck = nullptr;
    QLabel*    m_standingLabel    = nullptr;
    QLabel*    m_bestOfLabel      = nullptr;

    QString m_ladderId;
    int     m_teamSize = 1;
    QString m_rulesetId;
    int     m_maxPing = 120;
    QString m_teammate;
    bool    m_keepTeammate = false;
    QString m_formatSummary;
    // the box's choice, kept while it's disabled (no teammate chosen)
    bool    m_keepTeammateChoice = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // QUICKRANKEDMATCHDIALOG_HPP
