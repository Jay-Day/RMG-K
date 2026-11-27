#ifndef GAMESETTINGSDIALOG_HPP
#define GAMESETTINGSDIALOG_HPP

#ifdef NETPLAY

#include "LobbyClient.hpp"
#include "StageListDialog.hpp"

#include <RMG-Core/GameOptions.hpp>
#include <RMG-Core/StageList.hpp>

#include <QCoreApplication>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QSettings;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{
class CartridgeTile;
class LobbyButton;
class StageStrip;
class ToggleSwitch;

// What the host picked for each option that takes a choice of its own
// (CoreGameOptionInfo::StageList): option id -> its list's share code.
using GameOptionLists = QMap<QString, QString>;

// A set of a game's options the host saved under a name of their own. Kept on
// this PC per ROM, in QSettings("RMG-K", "n02") under
// CreateRoomDialog::romSettingsGroup(), value "SavedGameSettings": a JSON list
// of { "id", "name", "options", "lists" } ("lists": { option id: code }).
struct SavedGameSettings
{
    // 8 lowercase hex digits, kept through renames and edits.
    QString Id;
    QString Name;
    // In pack order, without the ids the packs no longer offer.
    QStringList Options;
    // Why it can't be used now that the packs changed; empty when it can.
    QString Problem;
    // The stage list of a stage list option among Options.
    GameOptionLists Lists;

    // What a room using it sends as its preset id: other clients tell a
    // host's saved set from a pack's preset by the prefix.
    QString presetId() const { return QStringLiteral("user-") + Id; }
    static bool isPresetId(const QString& presetId) { return presetId.startsWith(QLatin1String("user-")); }
};

// What the host has picked: Default, a pack's preset, a saved set, or a set
// of options that's none of those.
struct GameSettingsChoice
{
    enum class Kind { Default, Preset, Saved, Custom };
    Kind Type = Kind::Default;
    // The preset's id, or the saved set's Id.
    QString Id;
    // The options it turns on, in pack order.
    QStringList Options;
    // Their stage lists.
    GameOptionLists Lists;

    // How a Game settings list's entry carries it: "default", "preset:<id>",
    // "saved:<id>" or "custom".
    QString data() const;
    // The values romSettingsGroup() remembers it by: "GameSettings" (default,
    // preset, saved or custom), "GameSettingsPreset", "GameSettingsSaved",
    // "GameOptions" (the option ids) and "GameOptionLists" (a JSON object of
    // their stage lists).
    QVariantMap settingsValues() const;
};

// What a stage list option has, the way GameSettingsChoice says it for the
// whole set: off, one of the pack's preset lists ("Dream Land only"), one of
// the lists saved on this PC, or a list that's none of those.
struct StageListChoice
{
    enum class Kind { Off, Preset, Saved, Custom };
    Kind Type = Kind::Off;
    // The preset list's id, or the saved list's Id.
    QString Id;
    // The list's share code; empty when off.
    QString Code;

    // How a stage list entry carries it: "off", "preset:<id>", "saved:<id>"
    // or "custom".
    QString data() const;
    // What a room carries as the list's name (LobbyGameOption::paramPresetId):
    // a preset list's id, or "user-" and a saved list's id; empty otherwise.
    QString presetId() const;
};

// One entry of a Game settings list.
struct GameSettingsListItem
{
    QString Text;
    QString Data;
    QString Tip;
    bool Enabled = true;
    bool Separator = false;
};

// What this client's game options packs offer for one game: its options,
// the groups they're listed under and the presets, in the UI's language.
struct GameSettingsCatalog
{
    Q_DECLARE_TR_FUNCTIONS(UserInterface::Dialog::GameSettingsDialog)

  public:
    QString RomMd5;
    std::vector<CoreGameOptionInfo> Options;
    std::vector<CoreGameOptionGroupInfo> Groups;
    std::vector<CoreGameOptionPresetInfo> Presets;

    // At most this many saved sets per game, and this long a name: the lobby
    // takes preset labels of up to 48 characters.
    static constexpr int MaxSavedSets = 20;
    static constexpr int MaxSavedNameLength = 48;

    static GameSettingsCatalog forRom(const QString& romMd5);
    // The packs' translations follow the language the UI was started in.
    static const std::string& language();

    bool isEmpty() const { return Options.empty(); }
    const CoreGameOptionInfo* option(const QString& id) const;
    const CoreGameOptionPresetInfo* preset(const QString& id) const;
    // preset(), or the preset that had this id before (its aliases). Only for
    // choices remembered on this PC: a room's preset id means what it says.
    const CoreGameOptionPresetInfo* presetOrAlias(const QString& id) const;
    // The preset that turns on exactly these options, in any order, with the
    // same stage lists, or null.
    const CoreGameOptionPresetInfo* presetFor(const QStringList& ids, const GameOptionLists& lists = {}) const;
    // A preset's stage lists (CoreGameOptionPresetInfo::Lists).
    static GameOptionLists presetLists(const CoreGameOptionPresetInfo& preset);

    // Hidden options (CoreGameOptionInfo::Hidden) are part of a set like any
    // other, but never listed, counted or named: these split a set by it.
    // Options the packs don't have count as shown.
    bool isHidden(const QString& id) const;
    QStringList visible(const QStringList& ids) const;
    QStringList hidden(const QStringList& ids) const;
    // What a preset turns on as well in a match with this many players.
    static QStringList extraOptionIds(const CoreGameOptionPresetInfo& preset, int players);
    // What a match with this many players is played with, for a room with
    // these options on and this preset id: when it's a pack's preset and the
    // room has its options, they and what it adds for that many players.
    // Otherwise (saved sets, custom options) the room's options.
    QStringList matchOptions(const QStringList& on, const QString& presetId, int players) const;
    // Of these options, the ones that need a blank save (NeedsBlankSave).
    QStringList blankSaveOptions(const QStringList& ids) const;
    // A line per preset.WithPlayers entry saying what it adds, or empty.
    QString withPlayersText(const CoreGameOptionPresetInfo& preset) const;
    // These ids in pack order, without the ones the packs don't offer.
    QStringList ordered(const QStringList& ids) const;
    // Ids as remembered on this PC (a saved set, the last room's settings):
    // an option's old id is that option now (CoreGameOptionInfo::Aliases),
    // with the options it brings, and lists, when given, gets the alias's
    // stage list for an option that takes one; then ordered().
    QStringList fromStored(const QStringList& ids, GameOptionLists* lists = nullptr) const;
    // Why this set can't be on, by the packs' rules (one an option needs is
    // missing, two can't be on together, too many): empty when it can.
    QString problemWith(const QStringList& ids) const;
    // A preset's option ids.
    static QStringList optionIds(const CoreGameOptionPresetInfo& preset);
    // The options' names, in this client's language.
    QStringList labels(const QStringList& ids) const;
    // The same, with each stage list's name or how many stages it has.
    QStringList labels(const QStringList& ids, const GameOptionLists& lists) const;

    // Stage lists: whether an option takes one, its list's stages (none when
    // the code isn't one this pack reads), the lists of the options among
    // ids that take one, and why those can't be used (one without stages, or
    // with a code this pack can't read): empty when they can.
    bool takesList(const QString& id) const;
    bool stageList(const QString& id, CoreStageListInfo& info) const;
    int stageCount(const QString& id, const QString& code) const;
    GameOptionLists listsFor(const QStringList& ids, const GameOptionLists& lists) const;
    QString listsProblem(const QStringList& ids, const GameOptionLists& lists) const;
    static GameOptionLists listsFromJson(const QString& json);
    static QString listsToJson(const GameOptionLists& lists);

    // A stage list option's lists by name, as Game settings has presets and
    // saved sets: the pack's preset lists, then the lists saved on this PC
    // (StageListDialog::savedLists).
    QList<SavedStageList> savedStageLists(const QString& id) const;
    // The code of the option's preset list with this id, or empty.
    QString stageListPresetCode(const QString& id, const QString& presetId) const;
    // What a code is as a choice: Off when empty, a preset list when one has
    // the same stages (they win), then a saved list (savedHint first), else
    // Custom.
    StageListChoice stageListChoiceFor(const QString& id, const QString& code, const QList<SavedStageList>& saved,
                                       const QString& savedHint = QString()) const;
    // The choice an entry's data names, or Off when there's no such list.
    StageListChoice stageListChoiceFromData(const QString& id, const QString& data,
                                            const QList<SavedStageList>& saved) const;
    // The list's name in this client's language: a preset list's label, a
    // saved list's name, or empty.
    QString stageListName(const QString& id, const StageListChoice& choice, const QList<SavedStageList>& saved) const;
    // The list's name, or how many stages it has ("12 stages") without one.
    QString stageListText(const QString& id, const QString& code) const;
    // A stage list option's list: Off, the preset lists, the saved lists,
    // then "Custom (n stages)" while current is custom (the Customize...
    // button beside it makes one). currentIndex: current's entry.
    QList<GameSettingsListItem> stageListItems(const QString& id, const QList<SavedStageList>& saved,
                                               const StageListChoice& current, int* currentIndex) const;

    // The sets saved for this game, by name, each checked against these options.
    QList<SavedGameSettings> savedSets(const QString& romMd5) const;
    static void storeSavedSets(const QString& romMd5, const QList<SavedGameSettings>& sets);
    static const SavedGameSettings* findSaved(const QList<SavedGameSettings>& sets, const QString& id);
    // A name for a saved set as the lobby takes it, trimmed; empty with why
    // in error when it can't be one. savedNameFor also keeps it from passing
    // for one of the list's own entries.
    static QString cleanSavedName(const QString& text, QString* error);
    QString savedNameFor(const QString& text, QString* error) const;
    static QString newSavedId(const QList<SavedGameSettings>& sets);

    // What these options are as a choice: Default when none, a preset when
    // one matches (presets win), then a saved set with the same stage lists
    // (savedHint first), else Custom.
    GameSettingsChoice choiceFor(const QStringList& on, const GameOptionLists& lists,
                                 const QList<SavedGameSettings>& saved, const QString& savedHint = QString()) const;
    // The choice remembered in s (already in the game's romSettingsGroup()).
    // A saved set that's gone, or can't be used any more, is Default.
    GameSettingsChoice rememberedChoice(const QSettings& s, const QList<SavedGameSettings>& saved) const;
    // The choice an entry's data names, or Default when there's no such preset
    // or saved set. custom: the options for "custom", with their lists.
    GameSettingsChoice choiceFromData(const QString& data, const QList<SavedGameSettings>& saved,
                                      const QStringList& custom, const GameOptionLists& customLists = {}) const;

    // The host's Game settings list: Default, the presets, the saved sets,
    // then "Custom (n)" while current is custom (the Customize... button
    // beside the list makes one; n counts the options shown, not hidden
    // ones). Entries that need more than limit options are disabled.
    // currentIndex: current's entry.
    QList<GameSettingsListItem> listItems(const QList<SavedGameSettings>& saved, const GameSettingsChoice& current,
                                          int limit, int* currentIndex) const;
    // Puts items in combo, leaving it alone when it already has them (an open
    // list stays open). True when it changed.
    static bool fillList(QComboBox* combo, const QList<GameSettingsListItem>& items);
};

// The host's list of a game's options, one switch each under the pack's
// group headings, with what each does and what it turns on or off with it;
// an option that takes a stage list gets a list of named lists and a
// Customize... button instead. Hidden options aren't shown, and the view
// leaves those that are on as they are (the room's rule, a preset's or an
// alias's), unless an option turned on here can't be on with one.
// Read-only, it shows a guest (or the host during a match) what's on, and
// lets them save it as their own.
class GameSettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    // limit: how many options can be on at once here. on: the options on
    // now, lists: their stage lists.
    GameSettingsDialog(const QString& gameName, GameSettingsCatalog catalog, const QStringList& on,
                       const GameOptionLists& lists, bool editable, int limit, QWidget* parent = nullptr);

    void setCoverPath(const QString& coverPath);
    // The ROM stage lists read their icons from (none: names only).
    void setRomPath(const QString& romPath);
    // Options that take a stage list can't be turned on: the lobby server
    // would drop the list (why: the reason shown).
    void setListsUnavailable(const QString& why);
    // Why the limit is lower than this client's own (an older lobby server).
    void setLimitNote(const QString& note);
    // Read-only: what the room has now, including options this client's
    // packs don't know (shown with the host's label). savedByHost: the preset
    // is the host's own saved set, not one of the packs'. Called again as the
    // room changes; a name being typed for the old settings is dropped.
    void showRoomOptions(const QList<LobbyClient::LobbyGameOption>& on, const QString& presetLabel,
                         bool savedByHost = false);
    // Checks the set on Apply; on failure the reason shows and the dialog
    // stays open.
    void setApplyCheck(std::function<bool(const QStringList& ids, const GameOptionLists& lists, QString& error)> check);
    // Editable: lets the host save the switches under a name, and load,
    // rename or delete the game's saved sets. Read-only: only Save as, which
    // copies the room's set into this client's own saved sets, named like
    // the host's saved set when it is one.
    void enableSavedSets(const QString& romMd5);

    // The options picked, in pack order, and their stage lists.
    QStringList chosen() const;
    GameOptionLists chosenLists() const;

  signals:
    // A saved set or a saved stage list was added, replaced, renamed or
    // deleted.
    void savedSetsChanged();

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    struct Row
    {
        QString Id;
        ToggleSwitch* Switch = nullptr;
    };
    // An option that takes a stage list: the list of named lists (Off, the
    // preset lists, saved lists, the host's own), the button that opens the
    // editor (or the host's list, read-only), and while it's on the list's
    // first icons and what's on it.
    struct StageRow
    {
        QString Id;
        QComboBox* Combo = nullptr;
        LobbyButton* Button = nullptr;
        QWidget* Box = nullptr;
        StageStrip* Strip = nullptr;
        QLabel* Summary = nullptr;
    };

    void addOptionRow(QVBoxLayout* section, const CoreGameOptionInfo& option);
    // The lines under an option saying what goes on or off with it. list:
    // worded for an option picked from a list rather than a switch.
    void addRelationLines(QVBoxLayout* lay, QWidget* row, const CoreGameOptionInfo& option, bool list);
    void addStageListRow(QVBoxLayout* section, const CoreGameOptionInfo& option);
    void refreshStageRows();
    // An entry of a stage list option's list was picked.
    void pickStageList(const QString& id, int index);
    // The editor on the option's list (when it's off, the list it had last
    // here), or the host's list, read-only.
    void openStageList(const QString& id);
    // Read-only: the room's list's name for a stage list option, in this
    // client's language when it's one of its pack's preset lists; empty for
    // none.
    QString roomListName(const QString& id) const;
    // Turns one option on or off, with what it needs or what needs it.
    void toggle(const QString& id, bool on);
    void refresh();
    void apply();
    // The option's name in this client's language.
    QString labelOf(const QString& id) const;
    QStringList labelsOf(const QStringList& ids) const;

    // Saved sets: the list at the top, and the Save as bar under the switches.
    void rebuildSavedSection();
    void openSaveBar();
    void closeSaveBar();
    void checkSaveName();
    // Why the switches can't be saved as they are (none on, already in the
    // list, or read-only: options this client's packs don't have); empty
    // when they can.
    QString saveBlocker() const;
    void saveAs();
    void renameSaved(const QString& id, const QString& text, QLabel* error);
    void deleteSaved(const QString& id);
    void loadSaved(const QString& id);
    void storeSaved(const QString& note);
    // The button Enter presses: the open edit's, else Apply.
    void setEnterButton(LobbyButton* button);
    const SavedGameSettings* savedMatching(const QStringList& ids, const GameOptionLists& lists) const;

    GameSettingsCatalog m_catalog;
    bool m_editable = false;
    int m_limit = CoreGameOptionsMaxOptions;
    QStringList m_on;
    GameOptionLists m_lists;
    std::vector<Row> m_rows;
    std::vector<StageRow> m_stageRows;
    QString m_romPath;
    QString m_listsUnavailable;
    QPointer<QDialog> m_stageDialog;
    QString m_stageDialogId;
    // ids -> options that need it (the reverse of Requires)
    QHash<QString, QStringList> m_neededBy;
    std::function<bool(const QStringList&, const GameOptionLists&, QString&)> m_check;

    CartridgeTile* m_tile = nullptr;
    QString m_gameName;
    QVBoxLayout* m_sections = nullptr;
    QVBoxLayout* m_otherSection = nullptr;
    QWidget* m_otherBox = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_matches = nullptr;
    // What the matching preset adds with more players, under m_matches.
    QWidget* m_playersRule = nullptr;
    QLabel* m_playersRuleText = nullptr;
    QLabel* m_note = nullptr;
    QLabel* m_error = nullptr;
    QString m_limitNote;
    LobbyButton* m_applyButton = nullptr;
    // Apply, or Close in the read-only view: what Enter presses otherwise.
    LobbyButton* m_mainButton = nullptr;

    // Read-only: what showRoomOptions was last given. Every option id in the
    // room's order, the labels of those this client's packs don't have, and
    // the room's preset name.
    bool m_roomShown = false;
    QStringList m_roomIds;
    QStringList m_otherLabels;
    QString m_roomPresetLabel;
    bool m_roomSavedByHost = false;
    // Stage list option id -> the room's name for its list (id, label).
    QMap<QString, QPair<QString, QString>> m_roomListNames;

    QString m_romMd5;
    QList<SavedGameSettings> m_saved;
    QWidget* m_savedBox = nullptr;
    QVBoxLayout* m_savedRows = nullptr;
    // The saved set being renamed, or asked about before deleting it.
    QString m_savedEditId;
    bool m_savedEditDelete = false;
    LobbyButton* m_saveAsButton = nullptr;
    QWidget* m_saveBar = nullptr;
    QLineEdit* m_saveName = nullptr;
    LobbyButton* m_saveConfirm = nullptr;
    QLabel* m_saveMessage = nullptr;
    QVBoxLayout* m_body = nullptr;
    QHBoxLayout* m_footer = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // GAMESETTINGSDIALOG_HPP
