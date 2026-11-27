#ifndef STAGELISTDIALOG_HPP
#define STAGELISTDIALOG_HPP

#ifdef NETPLAY

#include <RMG-Core/StageList.hpp>

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

#include <functional>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{
class LobbyButton;

// The list a share code holds, or none when code is empty or this game's
// stage list can't read it.
std::vector<CoreStageListEntry> stageListEntries(const CoreStageListInfo& info, const QString& code);

// The stage select's icons, by stage id, read once per session from the
// player's own ROM at romPath. Empty when it can't be read: the editor then
// shows names only.
const QHash<int, QImage>& stageListIcons(const QString& romMd5, const QString& optionId, const QString& romPath);

// "1 stage", "12 stages".
QString stageCountText(int stages);
// "12 stages on 1 page" and the like.
QString stageListCountText(const CoreStageListInfo& info, int stages);

// A stage list kept under a name, per game, in QSettings("RMG-K", "n02")
// under CreateRoomDialog::romSettingsGroup(), value "SavedStageLists": a JSON
// list of { "id", "name", "code" }.
struct SavedStageList
{
    // 8 lowercase hex digits, kept through renames.
    QString Id;
    QString Name;
    QString Code;
};

// A row of a list's first icons, for the Custom view.
class StageStrip : public QWidget
{
  public:
    explicit StageStrip(QWidget* parent = nullptr);
    void setStages(const QHash<int, QImage>& icons, const std::vector<CoreStageListEntry>& entries);

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QList<QImage> m_icons;
    int m_more = 0;
};

// The stage list editor: the game's stages as cards with the stage select's
// own icons, the list in match order as the stage select pages it, a
// hazards and a movement switch per stage that has them, the pack's preset
// lists and the saved ones to start from, and share codes. Read-only it shows
// the host's list, which a guest can copy as a code or keep as their own.
class StageListDialog : public QDialog
{
    Q_OBJECT

  public:
    // info: the game's list (CoreGetStageList). code: the list to start
    // from, empty for none. romPath: the ROM the icons come from (empty:
    // names only).
    StageListDialog(const QString& gameName, CoreStageListInfo info, const QString& code, bool editable,
                    const QString& romMd5, const QString& romPath, QWidget* parent = nullptr);
    ~StageListDialog() override;

    // The list's share code; empty for an empty list.
    QString code() const;
    // Read-only: the room's list now.
    void showCode(const QString& code);
    // Read-only: the room's name for the list (empty for none), and whether
    // it's one the host saved, which Save as offers to keep.
    void setRoomListName(const QString& name, bool savedByHost);

    static QList<SavedStageList> savedLists(const QString& romMd5, const CoreStageListInfo& info);
    static void storeSavedLists(const QString& romMd5, const QList<SavedStageList>& lists);
    static constexpr int MaxSavedLists = 30;
    static constexpr int MaxSavedNameLength = 48;

  signals:
    void savedListsChanged();

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    class Card;
    class Row;
    class ListView;
    class Preview;

    QWidget* buildBrowsePanel();
    QWidget* buildListPanel();
    void buildTopBar(QVBoxLayout* body);

    bool inList(int stage) const;
    int indexOf(int stage) const;
    // Adds a stage at the end, or takes it out when it's on the list.
    void toggleStage(int stage);
    void setMode(int index, uint8_t mode);
    void move(int from, int to);
    void removeAt(int index);
    // Replaces the whole list, with an undo for what it was.
    void replaceList(const std::vector<CoreStageListEntry>& entries, const QString& note);
    void setAll(uint8_t clear, uint8_t set, const QString& note);
    void undo();
    void changed();
    void refreshCards();
    void rebuildRows();
    void refreshStatus();
    void applyFilter();
    void focusRow(int index, int part);
    void showPage(int page);

    void fillStartCombo();
    void startFrom(int index);
    void copyCode();
    void openPasteBar();
    void closePasteBar();
    void usePastedCode();
    QString problemText(CoreStageListProblem problem) const;

    void openSaveBar();
    void closeSaveBar();
    void checkSaveName();
    // Why the list can't be saved as it is (empty, one of the preset lists,
    // or saved already); empty when it can.
    QString saveBlocker() const;
    void saveAs();
    void refreshSavedActions();
    const SavedStageList* savedMatching() const;
    const CoreStageListPreset* presetMatching() const;
    void renameSaved();
    void deleteSaved();

    void showNote(const QString& text, bool undoable, bool bad = false);

    CoreStageListInfo m_info;
    bool m_editable;
    QString m_romMd5;
    QHash<int, QImage> m_icons;
    std::vector<CoreStageListEntry> m_entries;
    std::vector<CoreStageListEntry> m_undo;
    bool m_canUndo = false;
    QList<SavedStageList> m_saved;
    QString m_filter;
    int m_filterKind = 0;
    int m_page = 0;

    QList<Card*> m_cards;
    QList<QWidget*> m_sections;
    QLabel* m_noMatch = nullptr;
    QLineEdit* m_search = nullptr;
    QList<QPushButton*> m_filterButtons;

    QLabel* m_count = nullptr;
    Preview* m_preview = nullptr;
    QLabel* m_pageLabel = nullptr;
    QWidget* m_pager = nullptr;
    QWidget* m_prevPage = nullptr;
    QWidget* m_nextPage = nullptr;
    QScrollArea* m_listScroll = nullptr;
    ListView* m_listView = nullptr;
    QWidget* m_empty = nullptr;
    QList<Row*> m_rows;
    QList<LobbyButton*> m_bulkButtons;

    QComboBox* m_startCombo = nullptr;
    QLabel* m_savedAs = nullptr;
    LobbyButton* m_renameButton = nullptr;
    LobbyButton* m_deleteButton = nullptr;
    LobbyButton* m_copyButton = nullptr;
    LobbyButton* m_pasteButton = nullptr;
    QWidget* m_pasteBar = nullptr;
    QLineEdit* m_pasteEdit = nullptr;
    QLabel* m_pasteError = nullptr;
    QWidget* m_noteBar = nullptr;
    QLabel* m_note = nullptr;
    LobbyButton* m_undoButton = nullptr;

    QWidget* m_saveBar = nullptr;
    QLineEdit* m_saveName = nullptr;
    LobbyButton* m_saveConfirm = nullptr;
    QLabel* m_saveMessage = nullptr;
    LobbyButton* m_saveAsButton = nullptr;
    // Renaming a saved list: the field takes its new name.
    QString m_renamingId;
    // Read-only: the room's name for the list.
    QString m_roomListName;
    bool m_roomSavedByHost = false;
    LobbyButton* m_doneButton = nullptr;
    LobbyButton* m_mainButton = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // STAGELISTDIALOG_HPP
