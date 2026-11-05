/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef CREATEROOMDIALOG_HPP
#define CREATEROOMDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QList>
#include <QString>
#include <QList>
#include <QPair>
#include <QMap>

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLineEdit;
class QPushButton;
class QLabel;

namespace UserInterface
{
namespace Dialog
{

class CartridgeTile;
class LobbyButton;
class ToggleSwitch;

// What the lobby knows about ranked play for the selected game.
struct CreateRoomRankedOptions
{
    bool available = false;
    // Why ranked can't be chosen (shown when !available); empty hides the
    // ranked section entirely (server without ranked support).
    QString unavailableReason;
    // one per ladder the game is on: 1v1 and, when the server has one, 2v2
    struct Format
    {
        QString ladderId;
        QString ladderName;
        int teamSize = 1;
        int bestOf = 3; // every set's length
        // allowed rulesets (e.g. rosters) as id + display name, first = default
        QList<QPair<QString, QString>> rulesets;
        // the player's standing per roster, e.g. "✓ Gold 1532"
        QMap<QString, QString> standings;
    };
    QList<Format> formats;
};

// Modal form for creating a lobby room. Returns the chosen settings to the
// caller via accessors; caller is responsible for sending the protocol message.
class CreateRoomDialog : public QDialog
{
    Q_OBJECT

public:
    // The game is chosen in the lobby's shared picker and passed in here; this
    // dialog shows it read-only and reports it back via romName()/romMd5().
    CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                     const QString& gameName, const QString& gameMd5,
                     const CreateRoomRankedOptions& ranked = CreateRoomRankedOptions(),
                     QWidget* parent = nullptr);
    ~CreateRoomDialog() override = default;

    // Accessors valid only after exec() returns Accepted.
    QString name()       const { return m_name; }
    QString romName()    const { return m_romName; }
    QString romMd5()     const { return m_romMd5; }
    QString romRegion()  const { return m_romRegion; }
    int     maxPlayers() const { return m_maxPlayers; }
    int     delay()      const { return m_delay; }
    int     prediction() const { return m_prediction; }
    QString password()   const { return m_password; }
    bool    ranked()           const { return m_isRanked; }
    QString rulesetId()        const { return m_rulesetId; }
    QString ladderId()         const { return m_ladderId; }
    int teamSize()             const { return m_teamSize; }
    // The ranked set's maximum ping between players (0 for any).
    int     rankedMaxPing()    const { return m_rankedMaxPing; }
    // Who may join: the highest ping to the host (0 for any), and wired only.
    int     maxPing()    const { return m_maxPing; }
    bool    wiredOnly()  const { return m_wiredOnly; }

    // The ping limits a room can have, 0 meaning none; shared with the room's
    // own settings so both offer the same choices.
    static QList<int> pingLimits() { return { 0, 50, 100, 150 }; }
    static QString pingLimitText(int maxPingMs);
    // A combo box of pingLimits(), each with its signal bars.
    static void fillPingLimitCombo(QComboBox* combo);

    // The selected game's cover art, for the dialog's header tile.
    void setCoverPath(const QString& coverPath);
    // Rule out rooms too small for everyone on this PC.
    void setMinimumPlayers(int players);

    // Called by the lobby when the server replies ROOM_CREATE_FAIL.
    // Re-enables the form so the user can edit and retry.
    void showCreateFailure(const QString& reason);

    // Display-name formatting, exposed so the lobby's browse ROM picker can
    // render identical game names (keeps the Create Room pre-select in sync).
    static QString displayGameName(const QString& goodName, const QString& filePath);

signals:
    // Emitted when user clicks Create with valid input. The dialog stays
    // open in a "Creating..." state until either showCreateFailure() is
    // called (re-enables form) or the caller calls accept().
    void createRequested();

private slots:
    void onCreateClicked();
    void onPasswordToggled(bool enabled);
    void onRankedToggled(bool enabled);
    // shows the set length and fills the rosters for the chosen format
    void onFormatChanged();
    void validateInput();

private:
    void buildUi(const QString& defaultUsername, const QString& defaultRoomName);
    void loadDefaults();
    void saveDefaults();
    void setFormEnabled(bool enabled);
    int selectedFormat() const;
    // the players this room seats: the set's for a ranked room
    void updateSeats();

    // UI — local delay and prediction live in the room view.
    QLineEdit*   m_nameEdit       = nullptr;
    CartridgeTile* m_tile         = nullptr;
    QLabel*      m_gameLabel      = nullptr;   // read-only game from the lobby picker
    QButtonGroup* m_playersGroup  = nullptr;   // 2, 3 or 4 seats, by button id
    QPushButton* m_anyoneButton   = nullptr;
    QPushButton* m_passwordButton = nullptr;
    QLineEdit*   m_passwordEdit   = nullptr;
    QComboBox*   m_maxPingCombo   = nullptr;
    QPushButton* m_anyConnectionButton = nullptr;
    QPushButton* m_wiredOnlyButton = nullptr;
    ToggleSwitch* m_liveReplaySwitch = nullptr;
    LobbyButton* m_createButton   = nullptr;
    LobbyButton* m_cancelButton   = nullptr;
    QLabel*      m_statusLabel    = nullptr;
    QLabel*      m_seatsHint      = nullptr;
    ToggleSwitch* m_rankedSwitch  = nullptr;
    QWidget*     m_rankedOptions  = nullptr;
    QWidget*     m_joinLimits     = nullptr;   // max ping and connection; hidden for ranked
    QWidget*     m_formatBox      = nullptr;   // shown when the game has a 2v2 ladder too
    QLabel*      m_bestOfLabel    = nullptr;
    QButtonGroup* m_formatGroup   = nullptr;   // by index into m_rankedInfo.formats
    QButtonGroup* m_rulesetGroup  = nullptr;   // the format's rosters, rebuilt with it
    QHBoxLayout* m_rulesetLay     = nullptr;
    QButtonGroup* m_pingGroup     = nullptr;   // by max ping in ms, 0 = any
    CreateRoomRankedOptions m_rankedInfo;

    // Accessor backing
    QString m_name;
    QString m_romName;
    QString m_romMd5;
    QString m_romRegion;
    int     m_maxPlayers = 2;
    int     m_minimumPlayers = 2;
    bool    m_localPlayers = false; // this PC has local players: no ranked
    int     m_delay      = 2;
    int     m_prediction = 7;
    QString m_password;
    bool    m_isRanked = false;
    QString m_rulesetId;
    QString m_ladderId;
    int m_teamSize = 1;
    int     m_rankedMaxPing = 120;
    int     m_maxPing    = 0;
    bool    m_wiredOnly  = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // CREATEROOMDIALOG_HPP
