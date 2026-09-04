/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "Replay.hpp"
#include "ReplayMemory.hpp"
#include "Settings.hpp"
#include "Callback.hpp"
#include "File.hpp"
#include "Library.hpp"
#include "RomSettings.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "kailleraclient.h"
#endif

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
#pragma pack(push, 1)

// v5, ground-up rewrite - see docs/RMGR_SPEC.md. Breaking change from
// everything recorded before this document existed: no migration path, none
// planned. `version` keeps incrementing (this fork's prior in-development
// numbering reached 4) rather than resetting to 1, purely so a reader that
// only understands the old, unspecified layout sees an unfamiliar number and
// correctly refuses to parse, instead of the value colliding with an
// unrelated earlier meaning.
struct FileHeader
{
    char     magic[4];        // "RMGR"
    uint8_t  version;         // 5
    uint8_t  reserved[3];     // zero
    // Which game-family extension event set (below) applies - a coarser,
    // slower-growing identity than goodName. Empty (all zero) if the loaded
    // ROM isn't recognized: the file is still a fully valid core-only
    // recording in that case, just with no extension events. See
    // docs/RMGR_SPEC.md section 2.1/3.2.
    char     gameFamily[16];
    // Exact ROM build identity (mupen64plus-core's own ROM database string) -
    // distinct from gameFamily: two different goodNames can share one family
    // (e.g. a future vanilla-SSB64 recorder alongside Smash Remix, both
    // "smash64"), each with its own recorderSchemaVersion numbering space.
    char     goodName[64];
    uint32_t recorderSchemaVersion; // see kRecorderSchemaVersion below; 0 when gameFamily is empty
    uint64_t recordedAtEpochMillis; // milliseconds since the Unix epoch (UTC)
    // Byte length of the event stream after decompression - lets a reader
    // preallocate its output buffer instead of growing it dynamically.
    uint32_t uncompressedLength;
    // Byte length of the deflate-compressed block immediately following this
    // header. Always correct on disk: the whole match is buffered in memory
    // and compressed once, at match end (see s_EventBuffer below), so there
    // is no "0 until finalized, patched via seek" convention to speak of -
    // and, as an accepted trade-off, a crash or force-quit mid-match now
    // produces no file at all rather than a truncated one.
    uint32_t compressedLength;
};
static_assert(sizeof(FileHeader) == 108, "FileHeader must be 108 bytes");

enum class EventCode : uint8_t
{
    EventPayloads = 0x01,
    // Core (always present, any recognized-or-not N64 ROM):
    MatchStart = 0x02,
    InputFrame = 0x03,
    MatchEnd   = 0x05,
    // smash64 game-family extension (present only when gameFamily ==
    // "smash64" - see IsSmash64() below):
    StateFrame        = 0x04,
    ItemUpdate        = 0x06,
    StageHazardUpdate = 0x07,
    MatchSettings     = 0x08,
    MatchResult       = 0x09,
};

// Core event, code 0x02. Written exactly once, immediately after
// EventPayloads. Player display names are sourced from netplay room
// metadata (RMG-K's own slot-indexed name table), never from any in-game
// name tag - for an offline match, or a port with no assigned name, the
// corresponding playerNames entry is all zero bytes. Game-family-specific
// match settings (stage, character, stock count, damage ratio, items,
// teams, handicap, CPU difficulty, ...) are NOT part of this event - see
// MatchSettingsEvent below.
struct MatchStartEvent
{
    char    playerNames[4][32]; // NUL-padded; not necessarily NUL-terminated if it fills the field. UTF-8.
    uint8_t slotType[4];        // 0 human, 1 CPU, 2 empty - per port 0-3
};
static_assert(sizeof(MatchStartEvent) == 132, "MatchStartEvent must be 132 bytes");

// Core event, code 0x03. Input-side data, captured before the game
// processes that frame's inputs. One event per seated port per frame. Uses
// the game's already-processed button/stick values, the one input
// representation available uniformly for both human and CPU-controlled
// ports.
struct InputFrameEvent
{
    int32_t  frame;
    uint8_t  port;
    uint16_t buttons;
    int8_t   stickX;
    int8_t   stickY;
};
static_assert(sizeof(InputFrameEvent) == 9, "InputFrameEvent must be 9 bytes");

// Core event, code 0x05. Written exactly once, as the last event in the
// stream. Final per-port results (e.g. smash64's stocks-remaining
// placements) aren't a universal concept across N64 titles and are NOT part
// of this event - see MatchResultEvent below.
struct MatchEndEvent
{
    int32_t finalFrame; // last frame value seen in any InputFrame event this match
    uint8_t endReason;  // 0 aborted (match-was-reset or process/emulation stopped mid-match), 1 normal end
};
static_assert(sizeof(MatchEndEvent) == 5, "MatchEndEvent must be 5 bytes");

// smash64 extension event, code 0x08. Written exactly once, immediately
// after MatchStart - the game-family-specific counterpart split out of what
// used to be one combined GameStart event. Everything here is
// Smash-specific and static for the whole match.
struct MatchSettingsEvent
{
    uint8_t stageId;
    uint8_t gameType;          // 1 time, 2 stock, 3 both (Remix always forces stock)
    uint8_t stockCountSetting; // 0-based (i.e. 2 means "3 stocks")
    uint8_t timeLimitMinutes;  // 100 = infinite
    uint8_t damageRatio;       // 50 = 50%, 200 = 200%
    uint8_t itemFrequency;     // 0 none .. 5 high
    uint8_t teamsEnabled;      // 0 off, 1 on
    uint8_t handicapMode;      // 0 off, 1 on, 2 auto
    uint8_t characterId[4];    // per port 0-3
    uint8_t costumeId[4];
    uint8_t teamColor[4];
    uint8_t portTeam[4];       // team number per port
    uint8_t portHandicap[4];   // meaningful only when handicapMode != 0
    uint8_t portCpuLevel[4];   // meaningless for a human port
};
static_assert(sizeof(MatchSettingsEvent) == 32, "MatchSettingsEvent must be 32 bytes");

// smash64 extension event, code 0x04. State-side data, captured after that
// frame's physics/collision resolution - the resulting state. One event per
// seated port per frame, always immediately following that port's
// InputFrame in the stream.
struct StateFrameEvent
{
    int32_t  frame; // same frame counter as the paired InputFrame
    uint8_t  port;
    uint8_t  characterId;
    uint16_t actionStateId;
    float    positionX;
    float    positionY;
    int32_t  facingDirection; // 1 right, -1 left
    float    velocityX;
    float    velocityY;
    uint32_t damagePercent;
    int8_t   stocksRemaining; // 0-based; negative once eliminated
    // jumpsMax (per-character, from FTAttributes) minus jumps_used
    // (playerStruct+0x148, a u8 that resets to 0 on landing). 0 through
    // most of a grounded match is normal; Remix can also force this to 0
    // without that many real jump inputs (e.g. certain up-specials).
    uint8_t  jumpsRemaining;
    uint8_t  groundedState; // 0 grounded, 1 airborne
    uint8_t  hurtboxState;  // 0x03 = intangible/invincible; see ReplayMemory.cpp
    uint16_t hitstunCounter;
    uint32_t actionFrameCounter;
    // Native engine combo tracking, not mod-added. Belongs to the victim
    // (this port), not the attacker: hits taken in the current unbroken
    // chain. 0 = no active chain, 1 = a single hit, 2+ = an actual combo.
    // Both zero the instant the chain breaks.
    uint32_t comboHitCount;
    uint32_t comboDamage;
};
static_assert(sizeof(StateFrameEvent) == 50, "StateFrameEvent must be 50 bytes");

// smash64 extension event, code 0x06. Zero or more per frame - one per live
// Item or Weapon GObj (ReplayMemory::ItemObject) currently not held by a
// fighter, following that frame's InputFrame/StateFrame pairs. "Weapon" is
// a free-flying character special-move projectile (boomerang, fireball,
// ...); "Item" covers thrown/spawned items and hazard objects, including
// some fighter-held things like Link's pulled bomb.
struct ItemUpdateEvent
{
    int32_t  frame;         // same numbering as InputFrame/StateFrame
    uint32_t objectAddress; // the object's own RDRAM address - not a semantic spawn ID, see ReplayMemory::ItemObject
    uint8_t  linkId;        // 4 = Item, 5 = Weapon - which enum `kind` below means (docs/RMGR_SPEC.md section 8.6)
    int32_t  kind;          // ITKind (linkId == 4) or WPKind (linkId == 5)
    float    positionX;
    float    positionY;
    float    positionZ;
};
static_assert(sizeof(ItemUpdateEvent) == 25, "ItemUpdateEvent must be 25 bytes");

// smash64 extension event, code 0x07. Zero or one per frame, following that
// frame's ItemUpdate events - written only when at least one tracked hazard
// is currently active, same sparse convention as ItemUpdate. Currently
// tracks exactly one hazard: Whispy Woods' wind on Dream Land.
struct StageHazardUpdateEvent
{
    int32_t frame;
    uint8_t hazardFlags; // bit 0 = Whispy Woods currently blowing (Dream Land only);
                          // bit 1 = blowing direction (0 = left, 1 = right) -
                          // only meaningful when bit 0 is set, and only ever
                          // written alongside it (see below)
};
static_assert(sizeof(StageHazardUpdateEvent) == 5, "StageHazardUpdateEvent must be 5 bytes");

constexpr uint8_t kHazardFlagWhispyBlowing      = 0x01;
constexpr uint8_t kHazardFlagWhispyBlowingRight = 0x02;

// smash64 extension event, code 0x09. Written exactly once, immediately
// after the core MatchEnd event - the game-family-specific counterpart
// split out of what used to be one combined GameEnd event, since "stocks
// remaining" is a Smash concept, not a universal one.
struct MatchResultEvent
{
    int8_t placements[4]; // final stocks remaining per port, -1 if never seated
};
static_assert(sizeof(MatchResultEvent) == 4, "MatchResultEvent must be 4 bytes");

#pragma pack(pop)

enum class State
{
    Idle,            // feature disabled for this emulation session
    WaitingForMatch, // enabled, no match currently being recorded, watching for one to start
    Recording,       // buffering Input/StateFrame events every frame for the in-progress match
};

State                s_State = State::Idle;
int32_t              s_FrameNumber = 0;
// Everything for the in-progress match accumulates here instead of being
// streamed to disk incrementally - see docs/RMGR_SPEC.md section 2 for the
// buffered/compressed-once rationale and its accepted crash-safety
// trade-off. Cleared in OpenNewFile(); written out (compressed, once) in
// FinalizeFile().
std::vector<uint8_t> s_EventBuffer;
bool                 s_HasPendingRecording = false; // true between a successful OpenNewFile() and the matching FinalizeFile()
std::filesystem::path s_PendingOutputPath;
// Whether the loaded ROM's game family is recognized for THIS pending
// recording - gates every smash64 extension event (StateFrame/ItemUpdate/
// StageHazardUpdate/MatchSettings/MatchResult); the core events (MatchStart/
// InputFrame/MatchEnd) are written unconditionally. Cached at OpenNewFile()
// time rather than re-checked every frame, since the loaded ROM can't change
// mid-session.
bool                 s_FamilyRecognized = false;
FileHeader           s_PendingHeader{};

// Guards all of the above (and, transitively, everything the helpers below
// touch). OnFrame() runs on the emulation thread while OnEmulationStop() is
// always called from the UI thread (via CoreStopEmulation() and, for paths
// that don't reach that, MainWindow::on_Emulation_Finished), so without this
// a quit-mid-match can have both threads touching shared state at once.
// Locked at the top of each of the 3 public entry points; every static
// helper here is only ever reached through one of those.
std::mutex s_Mutex;

// Per-launch override for OnEmulationStart(), set via Replay::SetEnabledOverride().
// Consumed (cleared) the next time OnEmulationStart() runs - see that
// function and SetEnabledOverride's own doc comment in Replay.hpp.
bool s_HasOverride = false;
bool s_OverrideValue = false;

// Per-session output path override, set via Replay::SetOutputPathOverride().
// Applies to every OpenNewFile() call for the rest of this session; cleared
// at OnEmulationStop() - see that function and SetOutputPathOverride's own
// doc comment in Replay.hpp.
bool        s_HasOutputPathOverride = false;
std::string s_OutputPathOverride;
// Which numbered match this session is on for the override path (see
// OpenNewFile()) - reset to 0 whenever a new override is set.
int s_OverrideMatchNumber = 0;

// Per-session playerNames override, set via Replay::SetPlayerNamesOverride().
// Applies to every OpenNewFile() call for the rest of this session; cleared
// at OnEmulationStop() - see that function and SetPlayerNamesOverride's own
// doc comment in Replay.hpp.
bool                       s_HasPlayerNamesOverride = false;
std::array<std::string, 4> s_PlayerNamesOverride;

// Per-session recordedAtEpochMillis override, set via
// Replay::SetRecordedAtBaseOverride(). Applies to every OpenNewFile() call
// for the rest of this session; cleared at OnEmulationStop() - see that
// function and SetRecordedAtBaseOverride's own doc comment in Replay.hpp.
// Still stored as whole seconds - that's all the .krec format this is
// derived from actually has (see OpenNewFile(), which converts to
// milliseconds when combining it with the frame-derived offset).
bool                          s_HasRecordedAtBaseOverride = false;
uint64_t                      s_RecordedAtBaseEpochSeconds = 0;
Replay::FrameIndexProvider    s_RecordedAtFrameIndexProvider = nullptr;

// This feature's memory offsets were only ever derived/verified against
// Smash Remix 2.0.1 (see docs/RMGR_SPEC.md); recording its extension events
// against any other ROM would pointer-chase addresses that mean nothing
// there. GoodName comes from mupen64plus-core's own ROM database
// (CoreRomSettings::GoodName, via CoreGetCurrentRomSettings()) - for a
// ROM/hack absent from that database it degrades to a filename-derived
// value, so this exact-match check can only ever be as reliable as that
// database entry.
constexpr const char* kSmashRemixGoodName = "SmashRemix2.0.1";
constexpr const char* kSmash64Family      = "smash64";

// Bump whenever this recorder's interpretation of a goodName's memory
// layout changes in a way that affects what an smash64-family reader gets -
// not just when a field is newly appended (which the per-event
// EventPayloads declared-size mechanism, docs/RMGR_SPEC.md section 6,
// already handles on its own), but also e.g. a bugfix to an existing
// field's offset that silently changes recorded *values* without changing
// any event's byte size. This is its own counter per goodName - see
// docs/RMGR_SPEC.md section 3.2.
//
// Starts fresh at 1 for this container rewrite: every memory-offset fix
// this recorder previously accumulated (schema 2 through 9 under the old,
// unspecified container layout - see git history for that trail) is already
// reflected as correct in ReplayMemory.cpp today. There's nothing left to
// carry forward; the old numbering tracked a struct layout (GameStart/
// PostFrameUpdate) that no longer exists.
constexpr uint32_t kRecorderSchemaVersion = 1;

// Whether the currently-loaded ROM is a recognized smash64-family build.
// Only Smash Remix 2.0.1 is recognized today - see kSmashRemixGoodName's
// doc comment. Returns "" (not recognized - the recording, if any, stays
// core-only) or kSmash64Family.
//
// NOTE: this only decides whether the smash64 EXTENSION layer gets written.
// The recording *trigger* itself (OnFrame()'s state machine below) still
// depends entirely on ReplayMemory::IsInVsMatchScreen()/ReadMatchInfo(),
// which are themselves Smash-specific memory reads - there is currently no
// game-agnostic "a match is happening, here are this port's inputs" source
// wired into this recorder (RMG-K's PIF-level controller sync, used
// elsewhere for Kaillera netplay, would be the natural one). In practice
// this means core-only recording for an unrecognized game isn't reachable
// yet even though the wire format (docs/RMGR_SPEC.md section 2.1) already
// supports it - a real gap to close in a follow-up, not something this pass
// claims to have finished.
std::string DetermineGameFamily(const CoreRomSettings& romSettings)
{
    if (romSettings.GoodName == kSmashRemixGoodName)
    {
        return kSmash64Family;
    }
    return "";
}

// Copies as much of `s` as fits into `dest` (size `destSize`), NUL-padding
// or truncating as needed - `dest` is assumed zero-initialized already, so
// this only needs to write the bytes that actually fit.
void WriteFixedString(char* dest, size_t destSize, const std::string& s)
{
    std::memcpy(dest, s.data(), std::min(s.size(), destSize));
}

// Appends raw bytes to the in-memory buffer for the match currently being
// recorded - nothing touches disk until FinalizeFile() compresses and
// writes the whole thing out at once. The one primitive WriteEvent() and
// WriteEventPayloadsEvent() below both build on.
void AppendBytes(const void* data, size_t size)
{
    const size_t offset = s_EventBuffer.size();
    s_EventBuffer.resize(offset + size);
    std::memcpy(s_EventBuffer.data() + offset, data, size);
}

template <typename T>
void AppendValue(const T& value)
{
    AppendBytes(&value, sizeof(value));
}

// Appends one event (code byte + payload) to the buffer.
template <typename T>
void WriteEvent(EventCode code, const T& payload)
{
    AppendValue(static_cast<uint8_t>(code));
    AppendValue(payload);
}

// The Event Payloads event (0x01) is always first: it declares the exact
// payload size of every other event code THIS file uses, so a parser
// reading an unfamiliar/old-version file can skip unknown or resized events
// instead of breaking. Declares only the 3 core codes for a core-only
// (unrecognized game) recording; declares all 8 for a smash64 recording -
// see docs/RMGR_SPEC.md section 5.0.
void WriteEventPayloadsEvent(bool familyRecognized)
{
    struct EventSize
    {
        EventCode code;
        uint16_t  size;
    };
    static constexpr EventSize kCoreSizes[] = {
        {EventCode::MatchStart, static_cast<uint16_t>(sizeof(MatchStartEvent))},
        {EventCode::InputFrame, static_cast<uint16_t>(sizeof(InputFrameEvent))},
        {EventCode::MatchEnd,   static_cast<uint16_t>(sizeof(MatchEndEvent))},
    };
    static constexpr EventSize kSmash64Sizes[] = {
        {EventCode::StateFrame,        static_cast<uint16_t>(sizeof(StateFrameEvent))},
        {EventCode::ItemUpdate,        static_cast<uint16_t>(sizeof(ItemUpdateEvent))},
        {EventCode::StageHazardUpdate, static_cast<uint16_t>(sizeof(StageHazardUpdateEvent))},
        {EventCode::MatchSettings,     static_cast<uint16_t>(sizeof(MatchSettingsEvent))},
        {EventCode::MatchResult,       static_cast<uint16_t>(sizeof(MatchResultEvent))},
    };
    constexpr size_t kCoreCount    = sizeof(kCoreSizes) / sizeof(kCoreSizes[0]);
    constexpr size_t kSmash64Count = sizeof(kSmash64Sizes) / sizeof(kSmash64Sizes[0]);

    const uint8_t count = static_cast<uint8_t>(kCoreCount + (familyRecognized ? kSmash64Count : 0));
    AppendValue(static_cast<uint8_t>(EventCode::EventPayloads));
    AppendValue(count);

    auto appendEntries = [](const EventSize* entries, size_t entryCount)
    {
        for (size_t i = 0; i < entryCount; i++)
        {
            AppendValue(static_cast<uint8_t>(entries[i].code));
            AppendValue(entries[i].size);
        }
    };

    appendEntries(kCoreSizes, kCoreCount);
    if (familyRecognized)
    {
        appendEntries(kSmash64Sizes, kSmash64Count);
    }
}

// zlib deflate, max compression level - see docs/RMGR_SPEC.md section 3.4.
// Compression only ever runs once per match, at FinalizeFile() time, not
// per-frame, so the cost of the highest level is a non-issue. Returns an
// empty vector on failure (extremely unlikely - compressBound() already
// sizes the destination generously); the caller must check for that and
// skip writing a file rather than write a bogus one.
std::vector<uint8_t> DeflateCompress(const std::vector<uint8_t>& input)
{
    uLongf boundLen = compressBound(static_cast<uLong>(input.size()));
    std::vector<uint8_t> output(boundLen);
    uLongf actualLen = boundLen;

    const Bytef* sourcePtr = input.empty() ? nullptr : reinterpret_cast<const Bytef*>(input.data());
    int result = compress2(output.data(), &actualLen, sourcePtr,
        static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
    if (result != Z_OK)
    {
        return {};
    }

    output.resize(actualLen);
    return output;
}

std::string SanitizeForFilename(const std::string& input)
{
    std::string result = input.substr(0, 24);
    for (char& c : result)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }
    return result;
}

// Resolves the effective player names for this match: the per-session
// override (see SetPlayerNamesOverride) if one was set, otherwise the
// netplay room's own recording_player_names global. Used for BOTH the
// filename suffix (BuildFileName below) and MatchStart.playerNames, so the
// two can never disagree about whose names these are - previously
// BuildFileName read recording_player_names directly and ignored the
// override entirely, so headless .krec export produced files with correct
// in-header names but no player-name suffix on disk.
std::array<std::string, 4> ResolvePlayerNames(void)
{
    if (s_HasPlayerNamesOverride)
    {
        return s_PlayerNamesOverride;
    }

    std::array<std::string, 4> names{};
#ifdef RMGK_HAVE_P2P_TRANSPORT
    for (int i = 0; i < 4; i++)
    {
        names[i] = recording_player_names[i];
    }
#endif
    return names;
}

// Loosely mirrors n02_client.cpp's "<date>-Player1-Player2.krec" convention
// - player-name suffix and 24-char cap the same, but YYYYMMDD-HHMMSS
// (4-digit year, dashed) rather than krec's compact YYMMDDHHMMSS, for a
// name that reads as a timestamp at a glance. `now` is passed in (rather
// than this function calling time(nullptr) itself) so the caller can derive
// it from the exact same instant it writes into the file's own
// recordedAtEpochMillis header field - the filename and the header should
// never disagree about when the recording started, even though the
// filename itself is only second-resolution.
std::string BuildFileName(time_t now, const std::array<std::string, 4>& playerNames)
{
    tm localNow{};
#ifdef _WIN32
    localtime_s(&localNow, &now);
#else
    localtime_r(&now, &localNow);
#endif
    char datePart[16];
    strftime(datePart, sizeof(datePart), "%Y%m%d-%H%M%S", &localNow);

    std::string filename = datePart;

    for (const std::string& name : playerNames)
    {
        if (!name.empty())
        {
            filename += "-";
            filename += SanitizeForFilename(name);
        }
    }

    filename += ".rmgr";
    return filename;
}

// Resets per-match state and buffers the header/MatchStart(/MatchSettings)
// events in memory - nothing touches disk here. Returns true if recording
// should proceed (the caller transitions to State::Recording); false if the
// caller should stay in WaitingForMatch and retry next frame (e.g. the
// output directory couldn't be created).
bool OpenNewFile(const ReplayMemory::MatchInfo& matchInfo)
{
    s_FrameNumber = 0;
    s_EventBuffer.clear();
    // A rough head start on capacity so the first ~1000 frames' worth of
    // events (well past a typical short stock) don't force repeated
    // reallocate+copy growth; the buffer still grows geometrically beyond
    // this for a longer match, same as any std::vector.
    s_EventBuffer.reserve(256 * 1024);
    s_HasPendingRecording = false;

    // Default: live recording, stamped with wall-clock "now" - read from
    // system_clock (not time(nullptr)) for millisecond precision. Headless
    // .krec export overrides this (see SetRecordedAtBaseOverride's doc
    // comment) so the file reflects the match's *original* recording time
    // rather than when the headless replay (which can run at up to 2000%
    // speed) happened to reach it.
    std::chrono::system_clock::time_point nowTimePoint;
    if (s_HasRecordedAtBaseOverride)
    {
        const int elapsedFrames = s_RecordedAtFrameIndexProvider != nullptr ? s_RecordedAtFrameIndexProvider() : 0;
        // Milliseconds, not truncated whole seconds, so multiple matches
        // recorded from the same .krec land at distinguishable,
        // frame-accurate (~16.67ms) offsets from krecBaseEpochSeconds
        // instead of all bucketing into whichever whole second they
        // happened to start in.
        const int64_t elapsedMillis = static_cast<int64_t>(std::llround(elapsedFrames * 1000.0 / 60.0));
        const int64_t baseMillis = static_cast<int64_t>(s_RecordedAtBaseEpochSeconds) * 1000;
        nowTimePoint = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(baseMillis) + std::chrono::milliseconds(elapsedMillis));
    }
    else
    {
        nowTimePoint = std::chrono::system_clock::now();
    }
    const uint64_t nowEpochMillis = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTimePoint.time_since_epoch()).count());
    // BuildFileName()'s date part is still just local wall-clock seconds -
    // see docs/RMGR_SPEC.md section 3.3.
    const time_t now = static_cast<time_t>(nowEpochMillis / 1000);

    const std::array<std::string, 4> playerNames = ResolvePlayerNames();

    std::filesystem::path path;
    if (s_HasOutputPathOverride)
    {
        // Not consumed here - see SetOutputPathOverride's doc comment. Every
        // match this session gets its own explicitly-numbered file -
        // "<override>-1.ext", "<override>-2.ext", ... - not just
        // "<override>.ext" for the first, so a multi-game .krec's export
        // still reads as "<krec name>-<game number>.rmgr" and plainly
        // corresponds back to its source .krec by name.
        // CoreFindCollisionFreePath() is still a safety net in case this
        // same krec was already exported before (so "-1" is already taken).
        s_OverrideMatchNumber++;
        const std::filesystem::path base(s_OutputPathOverride);
        const std::filesystem::path numberedPath = base.parent_path() /
            (base.stem().string() + "-" + std::to_string(s_OverrideMatchNumber) + base.extension().string());
        path = CoreFindCollisionFreePath(numberedPath);
    }
    else
    {
        // Bare relative path, resolved by CWD - deliberately mirrors krec's
        // own "records" directory convention (Source/n02/n02_client.cpp)
        // exactly, so .rmgr files land next to .krec files at the top level
        // instead of being nested under the per-platform user-data directory.
        path = CoreFindCollisionFreePath(std::filesystem::path("replays") / BuildFileName(now, playerNames));
    }

    std::error_code createDirErrorCode;
    std::filesystem::create_directories(path.parent_path(), createDirErrorCode);
    if (createDirErrorCode)
    {
        if (s_HasOutputPathOverride)
        {
            s_OverrideMatchNumber--; // undo - see the increment above, this attempt never happened
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to create directory for " + path.string() + " - not recording");
        return false;
    }

    // Test-open (and immediately close) the actual destination now, before
    // buffering a single event - a match's worth of data can be several MB,
    // and the buffered/compressed-once design (see FinalizeFile()) only
    // writes it out once the match ends, so a write failure discovered only
    // then would silently lose the whole recording instead of never having
    // started it. This intentionally leaves a truncated (0-byte) file at
    // `path` if the match never finishes - a much smaller regression than
    // losing a full match, and FinalizeFile() overwrites it with the real
    // content (also truncating) on success.
    {
        std::ofstream testOpen(path, std::ios::binary | std::ios::trunc);
        if (!testOpen.is_open())
        {
            if (s_HasOutputPathOverride)
            {
                s_OverrideMatchNumber--; // undo - see the increment above, this attempt never happened
            }
            CoreAddCallbackMessage(CoreDebugMessageType::Warning,
                "Replay: failed to open " + path.string() + " for recording");
            return false;
        }
    }
    s_PendingOutputPath = path;

    // Reaching here already implies replay recording is enabled (see
    // OnEmulationStart()) - this is just re-fetching the loaded ROM's
    // identity to decide whether the smash64 extension layer applies. The
    // loaded ROM can't change mid-session.
    CoreRomSettings romSettings;
    CoreGetCurrentRomSettings(romSettings);
    const std::string gameFamily = DetermineGameFamily(romSettings);
    s_FamilyRecognized = !gameFamily.empty();

    s_PendingHeader = FileHeader{};
    std::memcpy(s_PendingHeader.magic, "RMGR", 4);
    s_PendingHeader.version = 5;
    WriteFixedString(s_PendingHeader.gameFamily, sizeof(s_PendingHeader.gameFamily), gameFamily);
    WriteFixedString(s_PendingHeader.goodName, sizeof(s_PendingHeader.goodName), romSettings.GoodName);
    s_PendingHeader.recorderSchemaVersion = s_FamilyRecognized ? kRecorderSchemaVersion : 0;
    s_PendingHeader.recordedAtEpochMillis = nowEpochMillis;
    // uncompressedLength/compressedLength are filled in by FinalizeFile()
    // once the whole match's events are known.

    WriteEventPayloadsEvent(s_FamilyRecognized);

    MatchStartEvent startEvent{};
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        startEvent.slotType[port] = portInfo.slotType;
    }

    for (int port = 0; port < 4; port++)
    {
        WriteFixedString(startEvent.playerNames[port], sizeof(startEvent.playerNames[port]), playerNames[port]);
    }

    WriteEvent(EventCode::MatchStart, startEvent);

    if (s_FamilyRecognized)
    {
        MatchSettingsEvent settingsEvent{};
        settingsEvent.stageId           = matchInfo.stageId;
        settingsEvent.gameType          = matchInfo.gameType;
        settingsEvent.stockCountSetting = matchInfo.stockCountSetting;
        settingsEvent.timeLimitMinutes  = matchInfo.timeLimitMinutes;
        settingsEvent.damageRatio       = matchInfo.damageRatio;
        settingsEvent.itemFrequency     = matchInfo.itemFrequency;
        settingsEvent.teamsEnabled      = matchInfo.teamsEnabled ? 1 : 0;
        settingsEvent.handicapMode      = matchInfo.handicapMode;

        for (int port = 0; port < 4; port++)
        {
            ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
            settingsEvent.characterId[port] = portInfo.characterId;
            settingsEvent.costumeId[port]   = portInfo.costumeId;
            settingsEvent.teamColor[port]   = portInfo.teamColor;

            // team/handicap/cpuLevel need the player-object/player-struct
            // chase, which can be unpopulated if the file opened during the
            // pre-match countdown (game_status == 0) before characters have
            // spawned. Left at their zero-initialized default in that case.
            ReplayMemory::PortPlayerState playerState = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
            if (playerState.valid)
            {
                settingsEvent.portTeam[port]     = playerState.team;
                settingsEvent.portHandicap[port] = playerState.handicap;
                settingsEvent.portCpuLevel[port] = playerState.cpuLevel;
            }
        }

        WriteEvent(EventCode::MatchSettings, settingsEvent);
    }

    s_HasPendingRecording = true;
    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: recording match, will write to " + s_PendingOutputPath.string() + " once it ends");
    return true;
}

// Compresses `eventBuffer` and writes `header` + the compressed blob to
// `outputPath` - the actual disk I/O for a finished match. Deliberately a
// free function taking everything by value/move rather than touching any
// s_* state: it runs on a detached worker thread (see FinalizeFile()) so
// compressing a multi-MB buffer and writing it out never blocks the caller
// of OnEmulationStop()/OnFrame() - frequently the UI thread via
// CoreStopEmulation(). Needs no locking of its own since nothing it touches
// is shared with any other thread.
void CompressAndWriteFile(std::vector<uint8_t> eventBuffer, FileHeader header, std::filesystem::path outputPath)
{
    const std::vector<uint8_t> compressed = DeflateCompress(eventBuffer);
    if (compressed.empty() && !eventBuffer.empty())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to compress recorded match data - not writing " + outputPath.string());
        return;
    }

    header.uncompressedLength = static_cast<uint32_t>(eventBuffer.size());
    header.compressedLength   = static_cast<uint32_t>(compressed.size());

    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Replay: failed to open " + outputPath.string() + " for writing");
        return;
    }

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!compressed.empty())
    {
        file.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    }
    file.close();

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Replay: wrote " + outputPath.string() + " (" +
        std::to_string(compressed.size()) + " bytes compressed, " +
        std::to_string(eventBuffer.size()) + " bytes uncompressed)");
}

void FinalizeFile(uint8_t endReason, const ReplayMemory::MatchInfo& matchInfo)
{
    if (!s_HasPendingRecording)
    {
        return;
    }

    MatchEndEvent endEvent{};
    endEvent.finalFrame = s_FrameNumber > 0 ? (s_FrameNumber - 1) : 0;
    endEvent.endReason  = endReason;
    WriteEvent(EventCode::MatchEnd, endEvent);

    if (s_FamilyRecognized)
    {
        MatchResultEvent resultEvent{};
        for (int port = 0; port < 4; port++)
        {
            ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
            resultEvent.placements[port] = portInfo.seated ? portInfo.stocksRemaining : -1;
        }
        WriteEvent(EventCode::MatchResult, resultEvent);
    }

    // Hand the buffer off to a detached worker thread for compression +
    // writing (see CompressAndWriteFile()'s doc comment) and reset our own
    // state immediately - the caller (often the UI thread) doesn't wait for
    // either to finish.
    std::thread(CompressAndWriteFile, std::move(s_EventBuffer), s_PendingHeader, s_PendingOutputPath).detach();

    s_HasPendingRecording = false;
    s_EventBuffer.clear();
}

void RecordFrame(const ReplayMemory::MatchInfo& matchInfo)
{
    for (int port = 0; port < 4; port++)
    {
        ReplayMemory::PortMatchInfo portInfo = ReplayMemory::ReadPortMatchInfo(matchInfo.matchInfoPtr, port);
        if (!portInfo.seated)
        {
            continue;
        }

        ReplayMemory::PortPlayerState state = ReplayMemory::ReadPortPlayerState(matchInfo.matchInfoPtr, port);
        if (!state.valid)
        {
            continue;
        }

        InputFrameEvent input{};
        input.frame   = s_FrameNumber;
        input.port    = static_cast<uint8_t>(port);
        input.buttons = state.processedButtons;
        input.stickX  = state.stickX;
        input.stickY  = state.stickY;
        WriteEvent(EventCode::InputFrame, input);

        if (s_FamilyRecognized)
        {
            StateFrameEvent stateFrame{};
            stateFrame.frame             = s_FrameNumber;
            stateFrame.port              = static_cast<uint8_t>(port);
            stateFrame.characterId       = portInfo.characterId;
            stateFrame.actionStateId     = state.actionStateId;
            stateFrame.positionX         = state.positionX;
            stateFrame.positionY         = state.positionY;
            stateFrame.facingDirection   = state.facingDirection;
            stateFrame.velocityX         = state.velocityX;
            stateFrame.velocityY         = state.velocityY;
            stateFrame.damagePercent     = state.damagePercent;
            stateFrame.stocksRemaining   = portInfo.stocksRemaining;
            // Clamped rather than a raw cast: state.jumpsRemaining is signed
            // and defaults to 0 if the FTAttributes pointer chase ever
            // fails, but could in principle read momentarily negative
            // mid-transition - wrapping that to a large uint8_t via a raw
            // cast would be actively misleading, not just imprecise.
            stateFrame.jumpsRemaining    = static_cast<uint8_t>(std::max(0, state.jumpsRemaining));
            stateFrame.groundedState     = state.groundedState;
            stateFrame.hurtboxState      = state.hurtboxState;
            stateFrame.hitstunCounter    = state.hitstunCounter;
            stateFrame.actionFrameCounter = state.actionFrameCounter;
            stateFrame.comboHitCount     = portInfo.comboHitCount;
            stateFrame.comboDamage       = portInfo.comboDamage;
            WriteEvent(EventCode::StateFrame, stateFrame);
        }
    }

    if (s_FamilyRecognized)
    {
        // One ItemUpdate per currently-live Item/Weapon GObj - after every
        // seated port's InputFrame/StateFrame pair, same as the per-port
        // events above. Zero events written when the list is empty this
        // frame - never a zeroed/placeholder event, same convention as an
        // unseated port.
        for (const ReplayMemory::ItemObject& item : ReplayMemory::ReadItemObjects())
        {
            ItemUpdateEvent itemEvent{};
            itemEvent.frame         = s_FrameNumber;
            itemEvent.objectAddress = item.objectAddress;
            itemEvent.linkId        = item.linkId;
            itemEvent.kind          = item.kind;
            itemEvent.positionX     = item.positionX;
            itemEvent.positionY     = item.positionY;
            itemEvent.positionZ     = item.positionZ;
            WriteEvent(EventCode::ItemUpdate, itemEvent);
        }

        // StageHazardUpdate - only written when at least one tracked hazard
        // is active, same sparse convention as ItemUpdate above.
        const ReplayMemory::StageHazards hazards = ReplayMemory::ReadStageHazards(matchInfo.stageId);
        uint8_t hazardFlags = 0;
        if (hazards.whispyBlowing)
        {
            hazardFlags |= kHazardFlagWhispyBlowing;
            if (hazards.whispyBlowingRight)
            {
                hazardFlags |= kHazardFlagWhispyBlowingRight;
            }
        }
        if (hazardFlags != 0)
        {
            StageHazardUpdateEvent hazardEvent{};
            hazardEvent.frame       = s_FrameNumber;
            hazardEvent.hazardFlags = hazardFlags;
            WriteEvent(EventCode::StageHazardUpdate, hazardEvent);
        }
    }

    s_FrameNumber++;
}
} // namespace

namespace Replay
{
CORE_EXPORT void OnEmulationStart(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    // Defensively drop any leftover buffered-but-unwritten recording from
    // an abnormal prior session end (e.g. OnEmulationStop() never ran on
    // that session) - matches this project's accepted "a crash mid-match
    // loses the recording" trade-off (docs/RMGR_SPEC.md section 2) rather
    // than trying to resurrect it here.
    s_HasPendingRecording = false;
    s_EventBuffer.clear();

    bool enabled;
    if (s_HasOverride)
    {
        enabled = s_OverrideValue;
        s_HasOverride = false; // consumed - see SetEnabledOverride's doc comment
    }
    else
    {
        enabled = CoreSettingsGetBoolValue(SettingsID::GameStats_ReplayEnabled);
    }

    // NOTE: this is a coarser gate than the format itself supports - see
    // DetermineGameFamily()'s doc comment for why core-only recording of an
    // unrecognized game isn't actually reachable yet (the recording
    // trigger below is itself a Smash-specific memory read). Once that gap
    // is closed, this early-out should only skip the extension layer, not
    // recording entirely.
    if (enabled)
    {
        CoreRomSettings romSettings;
        if (!CoreGetCurrentRomSettings(romSettings) || DetermineGameFamily(romSettings).empty())
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "Replay: enabled, but the loaded ROM isn't a recognized game - not recording");
            enabled = false;
        }
    }

    s_State = enabled ? State::WaitingForMatch : State::Idle;
    if (enabled)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Replay: enabled, watching for a match to start");
    }
    s_FrameNumber = 0;
}

CORE_EXPORT void SetEnabledOverride(bool enabled)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasOverride = true;
    s_OverrideValue = enabled;
}

CORE_EXPORT void SetOutputPathOverride(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasOutputPathOverride = true;
    s_OutputPathOverride = path;
    s_OverrideMatchNumber = 0;
}

CORE_EXPORT void SetPlayerNamesOverride(const std::array<std::string, 4>& names)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasPlayerNamesOverride = true;
    s_PlayerNamesOverride = names;
}

CORE_EXPORT void SetRecordedAtBaseOverride(uint64_t krecBaseEpochSeconds, FrameIndexProvider frameIndexProvider)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_HasRecordedAtBaseOverride = true;
    s_RecordedAtBaseEpochSeconds = krecBaseEpochSeconds;
    s_RecordedAtFrameIndexProvider = frameIndexProvider;
}

CORE_EXPORT void OnEmulationStop(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    if (s_State == State::Recording)
    {
        ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
        FinalizeFile(0 /* aborted: emulation stopped mid-match */, matchInfo);
    }
    s_State = State::Idle;

    // Both overrides apply for the whole session that just ended - every
    // OpenNewFile() call in between reused them (see SetOutputPathOverride/
    // SetPlayerNamesOverride's own doc comments) - so clear them now,
    // otherwise a later, unrelated session (e.g. a plain offline ROM launch
    // with no override call of its own) would silently inherit them.
    s_HasOutputPathOverride = false;
    s_OutputPathOverride.clear();
    s_OverrideMatchNumber = 0;
    s_HasPlayerNamesOverride = false;
    s_PlayerNamesOverride = {};
    s_HasRecordedAtBaseOverride = false;
    s_RecordedAtBaseEpochSeconds = 0;
    s_RecordedAtFrameIndexProvider = nullptr;
}

CORE_EXPORT void OnFrame(void)
{
    std::lock_guard<std::mutex> lock(s_Mutex);

    if (s_State == State::Idle)
    {
        return;
    }

    if (!ReplayMemory::IsInVsMatchScreen())
    {
        if (s_State == State::Recording)
        {
            ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
            FinalizeFile(0 /* aborted: left the VS match screen unexpectedly */, matchInfo);
        }
        s_State = State::WaitingForMatch;
        return;
    }

    ReplayMemory::MatchInfo matchInfo = ReplayMemory::ReadMatchInfo();
    if (!matchInfo.valid)
    {
        return;
    }

    if (s_State == State::WaitingForMatch)
    {
        if (matchInfo.gameStatus == 0 || matchInfo.gameStatus == 1)
        {
            if (OpenNewFile(matchInfo))
            {
                s_State = State::Recording;
            }
            // else: stay in WaitingForMatch and retry next frame.
        }
        return;
    }

    // s_State == State::Recording
    if (matchInfo.gameStatus == 5)
    {
        uint8_t endReason = matchInfo.matchWasReset ? 0 : 1;
        FinalizeFile(endReason, matchInfo);
        s_State = State::WaitingForMatch;
        return;
    }

    if (matchInfo.gameStatus == 1)
    {
        RecordFrame(matchInfo);
    }
}
} // namespace Replay
