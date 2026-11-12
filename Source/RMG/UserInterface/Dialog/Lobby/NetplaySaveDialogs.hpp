#ifndef NETPLAYSAVEDIALOGS_HPP
#define NETPLAYSAVEDIALOGS_HPP

#ifdef NETPLAY

#include <RMG-Core/NetplaySave.hpp>

#include <QDialog>
#include <QStringList>

#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;

namespace UserInterface
{
namespace Dialog
{
class CartridgeTile;
class LobbyButton;
class ToggleSwitch;

// The save a room's next match starts from, as the host picked it.
struct NetplaySaveChoice
{
    CoreNetplaySaveData Data;
    // Shown to the room.
    QString Label;
    // The file it came from, empty for the offline save and the last match's.
    QString File;
    // After a match that changed the save, the next one starts from where it
    // ended instead of from this save again.
    bool ContinueAfterMatch = true;
};

// "EEPROM 4K", "SRAM", "FlashRAM": the kinds data holds.
QStringList netplaySaveKindLabels(const CoreNetplaySaveData& data);
// "SRAM · 32 KB".
QString netplaySaveSummary(const CoreNetplaySaveData& data);
// 16 hex digits naming these exact bytes, so the room can tell saves apart.
QString netplaySaveHash(const CoreNetplaySaveData& data);

// The host picks the save the room's next match starts from: their offline
// save for the game, where the last match left off, a netplay save they kept,
// or any save file.
class StartingSaveDialog : public QDialog
{
    Q_OBJECT

  public:
    StartingSaveDialog(const QString& romFile, const QString& romMd5, const QString& gameName,
                       const std::optional<NetplaySaveChoice>& current,
                       const std::optional<CoreNetplaySaveData>& lastMatch, QWidget* parent = nullptr);

    void setCoverPath(const QString& coverPath);
    NetplaySaveChoice choice(void) const;

  private:
    struct Entry
    {
        QString Title;
        QString Detail;
        QString File;
        CoreNetplaySaveData Data;
        CoreNetplaySaveInfo Info;
        bool Native = false;    // a plain emulator save file: no record of its game
        bool OtherRom = false;  // a .rmgsave made with a different ROM
        bool Blank = false;     // a new cartridge: nothing saved yet
    };

    void addEntry(Entry entry, bool select);
    void browse(void);
    void showDetails(void);

    QString m_romMd5;
    QString m_gameName;
    std::vector<Entry> m_entries;
    CartridgeTile* m_tile = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_details = nullptr;
    ToggleSwitch* m_continueCheck = nullptr;
    LobbyButton* m_useButton = nullptr;
};

// Save As for the match's save: a name and folder, and whether to keep the
// file updated each time the game saves.
class SaveNetplaySaveDialog : public QDialog
{
    Q_OBJECT

  public:
    SaveNetplaySaveDialog(const QString& gameName, const QString& currentFile, bool keepUpdated,
                          QWidget* parent = nullptr);

    void setCoverPath(const QString& coverPath);
    QString filePath(void) const;
    bool keepUpdated(void) const;

  protected:
    void accept(void) override;

  private:
    QString m_gameName;
    CartridgeTile* m_tile = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_folder = nullptr;
    ToggleSwitch* m_keepCheck = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // NETPLAYSAVEDIALOGS_HPP
