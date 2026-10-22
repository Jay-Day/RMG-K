/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_RANKED_HPP
#define CORE_RANKED_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

//
// Ranked sets: loads a per-game ranked module (see Ranked/RankedModuleApi.h)
// and drives it from the emulation thread. The UI stages a session before
// starting emulation, then polls events while it runs.
//

struct CoreRankedModuleInfo
{
    std::string Id;
    std::string Name;
    std::string Version;
    std::filesystem::path Path;
};

struct CoreRankedSessionSettings
{
    std::string ModuleId;
    // empty = module default
    std::string RulesetId;
    int BestOf = 3;
    // 1-based controller ports of the ranked players
    std::vector<int> Ports;
    // display names per port (index 0 = port 1), used in the result record
    std::vector<std::string> PlayerNames;
    // team sets: each port's required team (index 0 = port 1), 1 or 2, when
    // the lobby matched teammates; empty or all 0 = players pick in the game
    std::vector<int> Teams;

    // Replay: the module's memory writes are part of the game state, so a
    // replay applies the write journal recorded live (see
    // CoreRankedGetLastReplayJson) instead of running the module. The module
    // is loaded only when ModuleVersion is installed, read-only, to show the
    // set's progress and check the recorded winners. Nothing is recorded or
    // reported; a difference from the recording is reported as ReplayMismatch.
    bool Replay = false;
    std::string ModuleVersion;
    // binary write journal (base64-decoded "journal.data")
    std::string Journal;
    std::string ExpectedRulesetHash;
    // recorded winner port per game (index 0 = game 1)
    std::vector<int> ExpectedWinners;
};

enum class CoreRankedEventType
{
    GameStarted = 1,
    GameEnded,
    GameVoided,
    SetEnded,
    // replay only: the replay diverged from the recorded set
    ReplayMismatch = 100,
};

struct CoreRankedEvent
{
    CoreRankedEventType Type = CoreRankedEventType::GameStarted;
    uint32_t Frame = 0;
    int GameNumber = 0;
    // 1-based port, 0 = none
    int WinnerPort = 0;
    // games won per port (index 0 = port 1); team sets: the port's team's
    std::vector<int> Score;
    std::string Detail;
    // team sets: each port's team (1 or 2, 0 = not in the set) and the
    // winning team; empty / 0 otherwise
    std::vector<int> Teams;
    int WinnerTeam = 0;
};

// returns all ranked modules found in the Ranked directory
std::vector<CoreRankedModuleInfo> CoreGetRankedModules(void);

// stages a ranked session for the next emulation start,
// the module is loaded and validated when emulation starts
bool CoreRankedStageSession(const CoreRankedSessionSettings& settings);

// clears a staged session
void CoreRankedClearStagedSession(void);

// returns whether a ranked session is currently running
bool CoreRankedIsActive(void);

// returns whether the current (or last) ranked session is a replay
bool CoreRankedIsReplay(void);

// returns and clears the events emitted since the last call (UI thread)
std::vector<CoreRankedEvent> CoreRankedTakeEvents(void);

// returns the module's short status text, empty when inactive
std::string CoreRankedGetStatusText(void);

// returns the FNV-1a 64 hash of "moduleId|moduleVersion|rulesetDescription"
// of the active session, 0 when inactive. Peers must match.
uint64_t CoreRankedGetRulesetHash(void);

// returns the JSON result record of the last finished set, empty when none
std::string CoreRankedGetLastResultJson(void);

// Live broadcasts of ranked matches (spectators need the write journal too):
// broadcaster: takes the journal frames confirmed since the last call,
// encoded (at most about maxBytes), false when there are none
bool CoreRankedTakeJournalUpdate(std::string& data, size_t maxBytes);
// spectator: before its emulation starts, makes the next session apply the
// broadcast's journal; chunks are added with CoreRankedAppendReplayJournal
void CoreRankedStageSpectatorJournal(void);
// spectator: adds a journal chunk (any thread), false when malformed
bool CoreRankedAppendReplayJournal(const std::string& data);

// returns the replay metadata of the last ranked session (module, ruleset,
// ROM, ports, .krec bookmarks and the module's write journal), empty when
// none. Appended to the session's .krec so it plays back without the module.
// Call after the recording is closed: the last records resolve late.
std::string CoreRankedGetLastReplayJson(void);

#ifdef CORE_INTERNAL

// called by CoreStartEmulation after the ROM is open and before execution.
// rollbackDriven: frames come from the rollback engine (CoreRankedOnFrame +
// CoreRankedConfirmThrough) instead of the core frame callback.
// Returns false (with CoreSetError) when a staged session can't be started.
bool CoreRankedBeginEmulation(bool netplay, bool rollbackDriven);

// called by CoreStartEmulation after execution ends
void CoreRankedEndEmulation(void);

// speculative frame hook, emulation thread, before `frame` executes
void CoreRankedOnFrame(uint32_t frame);

// confirms all observed frames up to and including `frame`
void CoreRankedConfirmThrough(int64_t frame);

// core frame callback hook (non-rollback execution): observes and
// immediately confirms. No-op when the session is rollback driven.
void CoreRankedOnCoreFrame(uint32_t frame);

// marks the running set as unverified (rollback engine desync detection)
void CoreRankedReportDesync(const std::string& detail);

// the rollback engine wrote .krec input record `recordIndex` (0-based) for
// `frame`; resolves bookmarks placed at or before that frame
void CoreRankedOnRecordWritten(int64_t frame, int recordIndex);

// spectator: a keyframe load was queued, after which playback continues
// with input record `record` (its local record 0)
void CoreRankedReplayRebase(int record);

// a savestate load completed (emulation thread)
void CoreRankedOnStateLoaded(void);

#endif // CORE_INTERNAL

#endif // CORE_RANKED_HPP
