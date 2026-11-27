/*
 * Stage lists: a game option whose patches are made from a list of stages
 * the host picks (Smash Remix's "Stage list"). The pack gives
 * the stages, where the list goes in the ROM and how its share code is
 * salted (an option's "stageList" block); this turns a list into patches, a
 * list into a short text code and back, and reads the stage select's icons
 * out of the player's own ROM.
 */
#ifndef CORE_STAGELIST_HPP
#define CORE_STAGELIST_HPP

#include "GameOptions.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// What a list entry turns off on its stage. Only what the stage has counts:
// a list is normalized to it.
constexpr uint8_t CoreStageModeHazardsOff = 1;
constexpr uint8_t CoreStageModeMovementOff = 2;

// A stage on a list, in match order.
struct CoreStageListEntry
{
    uint8_t Stage = 0;
    // CoreStageModeHazardsOff | CoreStageModeMovementOff
    uint8_t Mode = 0;

    bool operator==(const CoreStageListEntry& other) const
    {
        return Stage == other.Stage && Mode == other.Mode;
    }
    bool operator!=(const CoreStageListEntry& other) const
    {
        return !(*this == other);
    }
};

// A stage a list can have.
struct CoreStageListStage
{
    uint8_t Id = 0;
    // As the game's stage select prints it.
    std::string Name;
    // A base stage's group (CoreStageListInfo::Groups); empty for a variant.
    std::string Group;
    // A variant's base stage, or -1 for a base stage.
    int VariantOf = -1;
    // A variant's name on its base stage's card ("DL", "World 1-1"), and its
    // kind (an id of CoreStageListInfo::Kinds), or empty.
    std::string Short;
    std::string Kind;
    // What the stage has, so what an entry can turn off.
    bool Hazards = false;
    bool Movement = false;
};

// A heading stages are listed under, in the UI's language.
struct CoreStageListText
{
    std::string Id;
    std::string Label;
    std::string Description;
};

// A ready-made list with a name ("Dream Land only"), offered next to the
// host's saved lists.
struct CoreStageListPreset
{
    // [a-z0-9-], 1 to 32 characters, never starting with "user-" (a host's
    // saved list in a room).
    std::string Id;
    // In the UI's language when the pack has it, else English.
    std::string Label;
    std::string Description;
    std::string EnglishLabel;
    // Normalized, and no two presets have the same list.
    std::vector<CoreStageListEntry> Stages;
};

struct CoreStageListInfo
{
    // The option the list is for.
    std::string OptionId;
    // At most this many stages, each once.
    int MaxStages = 0;
    // The stage select shows PageStages stages per page, then Random up to
    // PageIcons icons, on at most MaxPages pages.
    int PageStages = 0;
    int PageIcons = 0;
    int MaxPages = 0;
    // A page's icons are in rows of this many.
    int PageColumns = 0;
    // The Random icon's id, which CoreReadStageListIcons gives its icon under.
    uint8_t RandomStage = 0;
    // Share codes start with CodePrefix + "-"; their checksum is salted with
    // CodeSalt, so a code for another game or version doesn't read.
    std::string CodePrefix;
    std::string CodeSalt;
    // In display order. Stages lists base stages first, each followed by its
    // variants, in the game's own order.
    std::vector<CoreStageListText> Groups;
    std::vector<CoreStageListText> Kinds;
    std::vector<CoreStageListStage> Stages;
    std::vector<CoreStageListPreset> Presets;

    const CoreStageListStage* stage(int id) const;
    const CoreStageListPreset* preset(const std::string& id) const;
    // How many stage select pages a list of n stages takes.
    int pagesFor(int n) const;
};

enum class CoreStageListProblem
{
    None,
    Empty,
    TooMany,
    UnknownStage,
    Repeated,
    // Codes only:
    NotACode,
    BadLength,
    BadVersion,
    BadChecksum,
};

// The stage list the packs offer for this ROM's option, in this language
// (like CoreGetGameOptions). False when the option takes no stage list. UI thread.
bool CoreGetStageList(const std::string& romMd5, const std::string& optionId,
                      const std::string& language, CoreStageListInfo& info);

// Checks a list against the rules (1 to MaxStages stages the info has, each
// once) and reduces each mode to what its stage has.
CoreStageListProblem CoreNormalizeStageList(const CoreStageListInfo& info,
                                            std::vector<CoreStageListEntry>& entries);

// The share code for a list ("STG-20GC-HPK0"), normalized first: the same
// list always gives the same code. Empty for a list that breaks the rules.
std::string CoreEncodeStageList(const CoreStageListInfo& info, std::vector<CoreStageListEntry> entries);

// The list a share code holds, normalized. Case, spaces and hyphens don't
// matter, O reads as 0 and I or L as 1, and the prefix may be left out.
CoreStageListProblem CoreDecodeStageList(const CoreStageListInfo& info, const std::string& code,
                                         std::vector<CoreStageListEntry>& entries);

// An icon in 8-bit RGBA, rows top to bottom.
struct CoreStageIcon
{
    int Width = 0;
    int Height = 0;
    std::vector<uint8_t> Rgba;
};

// Reads the stage select's icons from the ROM file at rom (any format the
// emulator opens), by stage id, Random's under RandomStage. Nothing is
// shipped: they come from the player's own copy. False, with CoreGetError()
// saying why, when the file can't be read or isn't the ROM the pack is for.
// Icons it can't decode are left out.
bool CoreReadStageListIcons(const std::filesystem::path& rom, const std::string& romMd5,
                            const std::string& optionId, std::map<int, CoreStageIcon>& icons);

#ifdef CORE_INTERNAL
// Where an option's list goes in the ROM, and where its icons are.
struct CoreStageListLayout
{
    // The stage select's table: PageIcons x MaxPages stage ids.
    uint32_t TableOffset = 0;
    std::vector<uint8_t> TableExpect;
    // The page count, big-endian, as wide as its expect.
    uint32_t PagesOffset = 0;
    std::vector<uint8_t> PagesExpect;
    // A hazard mode byte per stage id.
    uint32_t ModesOffset = 0;
    std::vector<uint8_t> ModesExpect;

    // The game's file table (12-byte entries), the stage icon file, the
    // table of its sprites by stage id and Random's sprite (ROM offsets of
    // (file index << 24) | offset words), and where in the icon file the
    // pointers to other files' bases start (file index n >= 1).
    uint32_t FileTable = 0;
    uint32_t FileCount = 0;
    uint32_t IconFile = 0;
    uint32_t Sprites = 0;
    uint32_t RandomSprite = 0;
    uint32_t FileBases = 0;
};

// The three list patches for a normalized list, with their expect bytes.
std::vector<CoreGameOptionPatch> CoreStageListPatches(const CoreStageListInfo& info, const CoreStageListLayout& layout,
                                                      const std::vector<CoreStageListEntry>& entries);

// The icons out of a ROM image in .z64 order.
bool CoreDecodeStageListIcons(const std::vector<char>& rom, const CoreStageListInfo& info,
                              const CoreStageListLayout& layout, std::map<int, CoreStageIcon>& icons,
                              std::string& error);
#endif // CORE_INTERNAL

#endif // CORE_STAGELIST_HPP
