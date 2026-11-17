/*
 * Netplay save data: the cartridge saves (EEPROM, SRAM, FlashRAM) a rollback
 * match starts from, the in-memory sandbox the game writes to instead of the
 * player's own save files, and the .rmgsave files players keep them in.
 */
#ifndef CORE_NETPLAYSAVE_HPP
#define CORE_NETPLAYSAVE_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class CoreSaveDataKind
{
    Eeprom   = 0,
    Sram     = 1,
    FlashRam = 2,
};

constexpr int CoreSaveDataKindCount = 3;

struct CoreNetplaySaveData
{
    // Bytes per kind, indexed by CoreSaveDataKind. Empty starts that kind blank.
    std::array<std::vector<uint8_t>, CoreSaveDataKindCount> Kinds;

    bool IsEmpty(void) const;
    size_t TotalBytes(void) const;

    bool operator==(const CoreNetplaySaveData& other) const
    {
        return Kinds == other.Kinds;
    }
};

// What a .rmgsave says about where its save came from.
struct CoreNetplaySaveInfo
{
    // What the player called it.
    std::string Name;
    // The game it belongs to. Both are empty for a plain emulator save file.
    std::string GoodName;
    std::string RomMD5;
    int64_t CreatedUnix = 0;
    std::vector<std::string> Players;
};

// Stages the save the next netplay session starts from. Empty data starts
// every save blank, which is what netplay did before save data existed.
void CoreSetNetplaySaveData(const CoreNetplaySaveData& data);

// Drops the staged save, so the next session starts blank.
void CoreClearNetplaySaveData(void);

// The staged save.
const CoreNetplaySaveData& CoreGetNetplaySaveData(void);

// The running game's saves and the core's count of the game's writes.
// Emulation thread only, between frames.
bool CoreGetRunningSaveData(CoreNetplaySaveData& data, uint32_t& generation);
bool CoreGetRunningSaveGeneration(uint32_t& generation);

// The latest copy of the game's saves that every peer agrees on: taken at a
// confirmed frame once the game stopped writing for about a second. Returns
// false when nothing new was published since the last take. UI thread.
bool CoreTakeConfirmedSaveData(CoreNetplaySaveData& data, uint32_t& generation);

// The save section of the pre-match manifest (uncompressed).
bool CoreSerializeNetplaySaveData(const CoreNetplaySaveData& data, std::string& out);
bool CoreDeserializeNetplaySaveData(const std::string& in, CoreNetplaySaveData& data);

// Reads a .rmgsave, or one emulator save file (.eep, .sra or .fla) as the
// kind its extension names.
bool CoreReadNetplaySaveFile(const std::filesystem::path& file, CoreNetplaySaveData& data, CoreNetplaySaveInfo& info);

// Writes a .rmgsave, replacing the file in one step so a crash can't leave
// half of one behind.
bool CoreWriteNetplaySaveFile(const std::filesystem::path& file, const CoreNetplaySaveData& data, const CoreNetplaySaveInfo& info);

// Reads the save files offline play uses for this ROM. files lists the ones
// that exist; returns false when there are none.
bool CoreReadOfflineSaveData(const std::filesystem::path& rom, CoreNetplaySaveData& data, std::vector<std::filesystem::path>& files);

// Where netplay saves go unless the player picks a folder: <save dir>/Netplay.
std::filesystem::path CoreGetNetplaySaveDirectory(void);

#ifdef CORE_INTERNAL
// CoreStartEmulation: sandbox the saves with the staged data for a netplay
// session (enabled), or turn the sandbox off for offline play.
bool CoreApplyNetplaySaveData(bool enabled);

// The rollback session's hooks for CoreTakeConfirmedSaveData.
void CoreNetplaySaveTrackReset(void);
void CoreNetplaySaveTrackStateSaved(int frame);
void CoreNetplaySaveTrackEndFrame(int confirmedFrame);
void CoreNetplaySaveTrackSessionEnd(int confirmedFrame);
#endif // CORE_INTERNAL

#endif // CORE_NETPLAYSAVE_HPP
