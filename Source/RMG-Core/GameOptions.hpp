/*
 * Netplay game options: per-game choices a rollback match's host can turn on
 * (say, "Vanilla only" for Smash Remix). Each is a set of ROM patches that
 * every player, late joiner, live viewer and replay applies to the game before
 * it boots. Packs of them are JSON files in Data/GameOptions, keyed by ROM
 * MD5; only the host needs one, since the resolved patches travel with the
 * match and its replays. A ranked match's options come from the lobby
 * instead, patches and all, and nobody needs a pack.
 */
#ifndef CORE_GAMEOPTIONS_HPP
#define CORE_GAMEOPTIONS_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// What one match may carry. Received manifests are held to the same limits.
constexpr int CoreGameOptionsMaxOptions = 8;
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
    // and the lobby (ranked) have them; manifests and replays don't, so it's
    // empty there.
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
    // Only the lobby puts it on a match (a ranked ruleset's); never a host's switch.
    bool ServerOnly = false;
};

// Reads the game options packs again: <shared data>/GameOptions/*.json, then
// <user config>/GameOptions/*.json, where a pack with a shared pack's id
// replaces it. Malformed packs and options are skipped and logged. The first
// query below loads them; this picks up changed files. UI thread.
void CoreReloadGameOptionPacks(void);

// The options the packs offer a host for the ROM with this MD5, in pack
// order. language is like "ja_JP" or "ja"; empty or one a pack lacks gives
// English. Options marked "serverOnly" are left out unless includeServerOnly
// (for showing the labels of options the lobby set). UI thread.
std::vector<CoreGameOptionInfo> CoreGetGameOptions(const std::string& romMd5, const std::string& language = std::string(),
                                                   bool includeServerOnly = false);

// Resolves the chosen option ids into what a match carries, checking every
// patch against the ROM file at rom (any format the emulator opens). Fails,
// with CoreGetError() saying why, for an id the ROM isn't offered (server-only
// options never are), an option whose bytes don't match this ROM, or options
// that change the same bytes. No ids resolve to no options. UI thread.
bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                            const std::vector<std::string>& ids, CoreNetplayGameOptions& options);

// Checks game options someone else resolved (a ranked match's, from the
// lobby) against the ROM file at rom: within the limits above, no two
// patches changing the same bytes, and every patch's Expect, which must be
// there, matching the ROM. Fails with CoreGetError() saying why. No pack is
// needed. UI thread.
bool CoreVerifyGameOptions(const std::filesystem::path& rom, const CoreNetplayGameOptions& options);

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
