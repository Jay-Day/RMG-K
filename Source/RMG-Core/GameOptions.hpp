/*
 * Netplay game options: per-game choices a rollback match's host can turn on
 * (say, "Vanilla CSS" for Smash Remix). Each is a set of ROM patches that
 * every player, late joiner, live viewer and replay applies to the game before
 * it boots. Packs of them are JSON files in Data/GameOptions, keyed by ROM
 * MD5; only the host needs one, since the resolved patches travel with the
 * match and its replays.
 */
#ifndef CORE_GAMEOPTIONS_HPP
#define CORE_GAMEOPTIONS_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// What one match may carry. Received manifests are held to the same limits.
constexpr int CoreGameOptionsMaxOptions = 16;
// Clients from before presets refuse a manifest or replay with more options
// than this, so a match only carries more when every client in it takes them
// (the lobby's HELLO gameOptionsMax).
constexpr int CoreGameOptionsLegacyMaxOptions = 8;
constexpr int CoreGameOptionMaxPatches = 256;
constexpr size_t CoreGameOptionsMaxPatchBytes = 64 * 1024;
constexpr size_t CoreGameOptionMaxIdLength = 32;
constexpr size_t CoreGameOptionMaxLabelLength = 256;

struct CoreGameOptionPatch
{
    // ROM file offset, counting bytes in .z64 (big-endian) order.
    uint32_t Offset = 0;
    std::vector<uint8_t> Bytes;
    // The unpatched ROM's bytes there, checked before patching. Only a pack
    // has them; manifests and replays don't, so it's empty there.
    std::vector<uint8_t> Expect;

    // Spelled out: RMG builds this header as C++17 on Linux.
    bool operator==(const CoreGameOptionPatch& other) const
    {
        return Offset == other.Offset && Bytes == other.Bytes && Expect == other.Expect;
    }
};

struct CoreNetplayGameOption
{
    // [a-z0-9-], 1 to 32 characters.
    std::string Id;
    // English, for players and replays without the pack.
    std::string Label;
    std::vector<CoreGameOptionPatch> Patches;

    bool operator==(const CoreNetplayGameOption& other) const
    {
        return Id == other.Id && Label == other.Label && Patches == other.Patches;
    }
};

// The game options a match is played with.
struct CoreNetplayGameOptions
{
    std::vector<CoreNetplayGameOption> Options;

    bool IsEmpty(void) const;
    size_t TotalPatchBytes(void) const;

    bool operator==(const CoreNetplayGameOptions& other) const
    {
        return Options == other.Options;
    }
};

// An id that named an option before (pack "aliases" on the option): a choice
// remembered by it, such as a saved set or the last room's settings, turns
// this option on instead, with the list of one of its stage list presets
// when it takes a list, and the options in With. A room's option ids are
// never read as aliases.
struct CoreGameOptionAlias
{
    std::string Id;
    // The id of one of the option's stage list presets (CoreStageListInfo::
    // Presets): always set for an option that takes a list, else empty.
    std::string StageListPreset;
    // Other options it turns on too, in pack order.
    std::vector<std::string> With;
};

// An option a pack offers for a ROM.
struct CoreGameOptionInfo
{
    std::string Id;
    // In the language asked for when the pack has it, else English.
    std::string Label;
    std::string Description;
    std::string EnglishLabel;
    std::string EnglishDescription;
    // The pack it's from, and the pack's name for the game.
    std::string PackId;
    std::string Game;
    // The id of the group it's listed under (CoreGetGameOptionGroups), or
    // empty for none.
    std::string Group;
    // Options that must be on with it, and options that can't be on with it,
    // by id. Both only name options offered for the same ROM. Conflicts go
    // both ways: an option lists the ones that name it too.
    std::vector<std::string> Requires;
    std::vector<std::string> Conflicts;
    // The option only works as meant from a blank save (pack "saveData":
    // "blank"), say because the game reads the settings it changes from save
    // data: while it's on, a room's matches start without save data, whatever
    // save the host picked.
    bool NeedsBlankSave = false;
    // The option is made from a list of stages the host picks (pack
    // "stageList", see StageList.hpp), carried as its share code: it only
    // resolves with one. A preset turns it on with one of its stage list
    // presets (CoreGameOptionPresetInfo::Lists).
    bool StageList = false;
    // Never listed, counted or named to players (pack "hidden": true), but
    // applied like any other when a match has it: for rules a preset, an
    // alias or the lobby turns on, such as forcing Team Battle in ranked.
    bool Hidden = false;
    // The ids it stood in for before, valid ones only (none names an option
    // that's offered).
    std::vector<CoreGameOptionAlias> Aliases;
};

// A heading a pack sorts its options under ("Characters", "Stages").
struct CoreGameOptionGroupInfo
{
    std::string Id;
    // In the language asked for when the pack has it, else English.
    std::string Label;
    std::string EnglishLabel;
};

// Options a preset turns on as well once a match has this many players.
struct CoreGameOptionPresetExtra
{
    // 2 to 4. Every seated player counts, players on the same PC too.
    int MinPlayers = 0;
    // In pack order; none of them is among the preset's own Options.
    std::vector<std::string> Options;
    // What they do, in the language asked for when the pack has it, else
    // English: the end of "With 4 players in the match, <preset> ...", or
    // empty when the pack has none (the options' labels say it then).
    std::string Description;
};

// A named set of a pack's options ("Vanilla Tournament").
struct CoreGameOptionPresetInfo
{
    // [a-z0-9-], 1 to 32 characters, like an option id.
    std::string Id;
    // In the language asked for when the pack has it, else English.
    std::string Label;
    std::string Description;
    std::string EnglishLabel;
    std::string EnglishDescription;
    // The ids of the options it turns on, in pack order: at least one, and a
    // set CoreResolveGameOptions takes.
    std::vector<std::string> Options;
    // The share code of each stage list option among Options, from the
    // stage list preset the pack names for it: option id -> code.
    std::map<std::string, std::string> Lists;
    // What it adds with more players, fewest players first. The options with
    // every entry up to a match's player count added are a set
    // CoreResolveGameOptions takes too. A room on the preset still shows only
    // Options; the host's client adds these when the match starts.
    std::vector<CoreGameOptionPresetExtra> WithPlayers;
    // Ids the preset had before: a choice remembered by one of them is this
    // preset now. A room's preset id is never looked up by them.
    std::vector<std::string> Aliases;
    std::string PackId;
};

// Reads the game options packs again: <shared data>/GameOptions/*.json, then
// <user config>/GameOptions/*.json, where a pack with a shared pack's id
// replaces it. Malformed packs and options are skipped and logged. The first
// query below loads them; this picks up changed files. UI thread.
void CoreReloadGameOptionPacks(void);

// The options the packs offer for the ROM with this MD5, in pack order.
// language is like "ja_JP" or "ja"; empty or one a pack lacks gives English.
// UI thread.
std::vector<CoreGameOptionInfo> CoreGetGameOptions(const std::string& romMd5, const std::string& language = std::string());

// The groups the packs list this ROM's options under, in pack order. An
// option whose Group isn't among them has none. UI thread.
std::vector<CoreGameOptionGroupInfo> CoreGetGameOptionGroups(const std::string& romMd5, const std::string& language = std::string());

// The presets the packs offer for this ROM, in pack order. Each names only
// options CoreGetGameOptions offers (in WithPlayers too), and no two have the
// same Options and Lists. UI thread.
std::vector<CoreGameOptionPresetInfo> CoreGetGameOptionPresets(const std::string& romMd5, const std::string& language = std::string());

// Resolves the chosen option ids into what a match carries, checking every
// patch against the ROM file at rom (any format the emulator opens). Fails,
// with CoreGetError() saying why, for an id the ROM isn't offered, an option
// whose bytes don't match this ROM, options that change the same bytes or
// conflict, or an option without one it requires. No ids resolve to no
// options. UI thread.
bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                            const std::vector<std::string>& ids, CoreNetplayGameOptions& options);

// The same, with each stage list option's list: option id -> share code. An
// option that takes a list fails without a valid one; others ignore theirs.
bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                            const std::vector<std::string>& ids, const std::map<std::string, std::string>& lists,
                            CoreNetplayGameOptions& options);

// Stages the game options the next synced session (rollback match, late
// join, live replay, replay) patches its ROM with. Offline and Kaillera
// server games never use them. Valid from staging until the next stage or clear.
void CoreSetNetplayGameOptions(const CoreNetplayGameOptions& options);

// Drops the staged game options.
void CoreClearNetplayGameOptions(void);

// The staged game options: what a match that was just synced, or is running,
// is played with.
const CoreNetplayGameOptions& CoreGetNetplayGameOptions(void);

// The game options section of the pre-match manifest and of a .krec's game
// options record ("RMGKGOP1"). Expect bytes aren't part of it. Deserializing
// holds the data to the limits above and refuses anything else.
bool CoreSerializeNetplayGameOptions(const CoreNetplayGameOptions& options, std::string& out);
bool CoreDeserializeNetplayGameOptions(const std::string& in, CoreNetplayGameOptions& options);

#ifdef CORE_INTERNAL
// CoreStartEmulation, with the ROM open and before it runs: patches the ROM
// with the staged options for a synced session (enabled). Refuses to start,
// with an error, when a patch doesn't fit this ROM.
bool CoreApplyNetplayGameOptions(bool enabled);
#endif // CORE_INTERNAL

#endif // CORE_GAMEOPTIONS_HPP
