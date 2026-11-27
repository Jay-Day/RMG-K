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
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

class QButtonGroup;
class QComboBox;
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
struct GameSettingsChoice;

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
    // Checks a set of game options (and their stage lists) against this PC's
    // ROM when the Custom view applies it (false, with why, when the ROM
    // can't take it).
    void setGameOptionsCheck(
        std::function<bool(const QStringList& ids, const QMap<QString, QString>& lists, QString& error)> check);
    // The selected game's ROM, for the stage list editor's icons.
    void setRomPath(const QString& romPath);
    // Rule out rooms too small for everyone on this PC.
    void setMinimumPlayers(int players);

    // Called by the lobby when the server replies ROOM_CREATE_FAIL.
    // Re-enables the form so the user can edit and retry.
    void showCreateFailure(const QString& reason);

    // Display-name formatting, exposed so the lobby's browse ROM picker can
    // render identical game names (keeps the Create Room pre-select in sync).
    static QString displayGameName(const QString& goodName, const QString& filePath);

    // The QSettings("RMG-K", "n02") group holding what was chosen for the
    // last room made for this game: this form's choices, and the ones made in
    // the room itself (late join, save data, game settings). Game settings
    // are "GameSettings" (default, preset, saved or custom),
    // "GameSettingsPreset" (the preset's id), "GameSettingsSaved" (the saved
    // set's id) and "GameOptions" (the option ids); the game's saved sets are
    // "SavedGameSettings" (SavedGameSettings).
    static QString romSettingsGroup(const QString& romMd5);

    // The lobby accepts once the server made the room: the choices are
    // remembered then, not when the room couldn't be made.
    void accept() override;

signals:
    // Emitted when user clicks Create with valid input. The dialog stays
    // open in a "Creating..." state until either showCreateFailure() is
    // called (re-enables form) or the caller calls accept().
    void createRequested();

private slots:
    void onCreateClicked();
    void onPasswordToggled(bool enabled);
    void validateInput();

private:
    void buildUi(const QString& defaultUsername, const QString& defaultRoomName);
    void loadDefaults();
    void saveDefaults();
    void setFormEnabled(bool enabled);
    // Game settings: the list's entries around the choice made, the choice,
    // and the Custom view that Customize... opens on it.
    void fillGameSettings(const GameSettingsChoice& current);
    GameSettingsChoice gameSettingsChoice() const;
    void onGameSettingsPicked();
    void openCustomGameSettings();

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
    // Default, the game options packs' presets, the host's saved sets and,
    // while it's the choice, "Custom (n)"; null for a game without a pack.
    // The choice is its entry's data; a custom set's options are kept apart.
    // Customize... beside it opens the Custom view.
    QComboBox*   m_gameSettingsCombo = nullptr;
    LobbyButton* m_gameSettingsButton = nullptr;
    QString      m_gameSettingsData;
    QStringList  m_customGameOptions;
    QMap<QString, QString> m_customGameLists;
    bool         m_customGameSettingsOpen = false;
    QString      m_coverPath;
    QString      m_romPath;
    std::function<bool(const QStringList&, const QMap<QString, QString>&, QString&)> m_gameOptionsCheck;
    LobbyButton* m_createButton   = nullptr;
    LobbyButton* m_cancelButton   = nullptr;
    QLabel*      m_statusLabel    = nullptr;

    // Accessor backing
    QString m_name;
    QString m_romName;
    QString m_romMd5;
    QString m_romRegion;
    int     m_maxPlayers = 2;
    int     m_minimumPlayers = 2;
    int     m_delay      = 2;
    int     m_prediction = 7;
    QString m_password;
    int     m_maxPing    = 0;
    bool    m_wiredOnly  = false;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // CREATEROOMDIALOG_HPP
