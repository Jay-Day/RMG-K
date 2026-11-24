#ifndef GAMESETTINGSDIALOG_HPP
#define GAMESETTINGSDIALOG_HPP

#ifdef NETPLAY

#include "LobbyClient.hpp"

#include <RMG-Core/GameOptions.hpp>

#include <QCoreApplication>
#include <QDialog>
#include <QHash>
#include <QList>
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
class ToggleSwitch;

// A set of a game's options the host saved under a name of their own. Kept on
// this PC per ROM, in QSettings("RMG-K", "n02") under
// CreateRoomDialog::romSettingsGroup(), value "SavedGameSettings": a JSON list
// of { "id", "name", "options" }.
struct SavedGameSettings
{
    // 8 lowercase hex digits, kept through renames and edits.
    QString Id;
    QString Name;
    // In pack order, without the ids the packs no longer offer.
    QStringList Options;
    // Why it can't be used now that the packs changed; empty when it can.
    QString Problem;

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

    // How a Game settings list's entry carries it: "default", "preset:<id>",
    // "saved:<id>" or "custom".
    QString data() const;
    // The values romSettingsGroup() remembers it by: "GameSettings" (default,
    // preset, saved or custom), "GameSettingsPreset", "GameSettingsSaved" and
    // "GameOptions" (the option ids).
    QVariantMap settingsValues() const;
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
    // The preset that turns on exactly these options, in any order, or null.
    const CoreGameOptionPresetInfo* presetFor(const QStringList& ids) const;
    // These ids in pack order, without the ones the packs don't offer.
    QStringList ordered(const QStringList& ids) const;
    // Ids as remembered in the settings: old names mapped to new ones, then ordered().
    QStringList fromStored(const QStringList& ids) const;
    // Why this set can't be on, by the packs' rules (one an option needs is
    // missing, two can't be on together, too many): empty when it can.
    QString problemWith(const QStringList& ids) const;
    // A preset's option ids.
    static QStringList optionIds(const CoreGameOptionPresetInfo& preset);
    // The options' names, in this client's language.
    QStringList labels(const QStringList& ids) const;

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
    // one matches (presets win), then a saved set (savedHint first), else Custom.
    GameSettingsChoice choiceFor(const QStringList& on, const QList<SavedGameSettings>& saved,
                                 const QString& savedHint = QString()) const;
    // The choice remembered in s (already in the game's romSettingsGroup()).
    // A saved set that's gone, or can't be used any more, is Default.
    GameSettingsChoice rememberedChoice(const QSettings& s, const QList<SavedGameSettings>& saved) const;
    // The choice an entry's data names, or Default when there's no such preset
    // or saved set. custom: the options for "custom".
    GameSettingsChoice choiceFromData(const QString& data, const QList<SavedGameSettings>& saved,
                                      const QStringList& custom) const;

    // The host's Game settings list: Default, the presets, the saved sets,
    // then Custom..., which names the set when current is custom. Entries that
    // need more than limit options are disabled. currentIndex: current's entry.
    QList<GameSettingsListItem> listItems(const QList<SavedGameSettings>& saved, const GameSettingsChoice& current,
                                          int limit, int* currentIndex) const;
    // Puts items in combo, leaving it alone when it already has them (an open
    // list stays open). True when it changed.
    static bool fillList(QComboBox* combo, const QList<GameSettingsListItem>& items);
};

// The host's list of a game's options, one switch each under the pack's
// group headings, with what each does and what it turns on or off with it.
// Read-only, it shows a guest (or the host during a match) exactly what's on.
class GameSettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    // limit: how many options can be on at once here. on: the options on now.
    GameSettingsDialog(const QString& gameName, GameSettingsCatalog catalog, const QStringList& on,
                       bool editable, int limit, QWidget* parent = nullptr);

    void setCoverPath(const QString& coverPath);
    // Why the limit is lower than this client's own (an older lobby server).
    void setLimitNote(const QString& note);
    // Read-only: what the room has now, including options this client's
    // packs don't know (shown with the host's label). savedByHost: the preset
    // is the host's own saved set, not one of the packs'.
    void showRoomOptions(const QList<LobbyClient::LobbyGameOption>& on, const QString& presetLabel,
                         bool savedByHost = false);
    // Checks the set on Apply; on failure the reason shows and the dialog
    // stays open.
    void setApplyCheck(std::function<bool(const QStringList& ids, QString& error)> check);
    // Editable: lets the host save the switches under a name, and load,
    // rename or delete the game's saved sets.
    void enableSavedSets(const QString& romMd5);

    // The options picked, in pack order.
    QStringList chosen() const;

  signals:
    // A saved set was added, replaced, renamed or deleted.
    void savedSetsChanged();

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    struct Row
    {
        QString Id;
        ToggleSwitch* Switch = nullptr;
    };

    void addOptionRow(QVBoxLayout* section, const CoreGameOptionInfo& option);
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
    // Why the switches can't be saved as they are (none on, or already in
    // the list); empty when they can.
    QString saveBlocker() const;
    void saveAs();
    void renameSaved(const QString& id, const QString& text, QLabel* error);
    void deleteSaved(const QString& id);
    void loadSaved(const QString& id);
    void storeSaved(const QString& note);
    // The button Enter presses: the open edit's, else Apply.
    void setEnterButton(LobbyButton* button);
    const SavedGameSettings* savedMatching(const QStringList& ids) const;

    GameSettingsCatalog m_catalog;
    bool m_editable = false;
    int m_limit = CoreGameOptionsMaxOptions;
    QStringList m_on;
    std::vector<Row> m_rows;
    // ids -> options that need it (the reverse of Requires)
    QHash<QString, QStringList> m_neededBy;
    std::function<bool(const QStringList&, QString&)> m_check;

    CartridgeTile* m_tile = nullptr;
    QString m_gameName;
    QVBoxLayout* m_sections = nullptr;
    QVBoxLayout* m_otherSection = nullptr;
    QWidget* m_otherBox = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_matches = nullptr;
    QLabel* m_note = nullptr;
    QLabel* m_error = nullptr;
    QString m_limitNote;
    LobbyButton* m_applyButton = nullptr;

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
