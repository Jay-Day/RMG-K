/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Ranked game module API (C ABI).
 *
 * A ranked game module is a shared library that teaches RMG-K how to run a
 * ranked set for one game: it enforces the ruleset inside the running game
 * and reports game/set results. RMG-K ("the host") loads it from the Ranked
 * directory next to the Plugin directory.
 *
 * Every frame is split into two phases:
 *
 *  1. OnFrame (speculative). Called at the boundary *before* emulated frame
 *     `frame` executes. Under rollback netplay the same frame number may be
 *     called many times (once live, again for every resimulation), and not
 *     necessarily in increasing order. The module may:
 *       - write game memory to enforce the ruleset (lock menus, force rules),
 *       - fill `observation` with whatever it needs to later judge results.
 *     Rules for OnFrame:
 *       - DETERMINISTIC: the writes and the observation must depend only on
 *         game memory. Never on wall-clock time, local settings, the local
 *         player's slot, or any state the module keeps between calls.
 *         Otherwise peers desync.
 *       - IDEMPOTENT: running it twice on the same memory must give the same
 *         memory and the same observation as running it once. The host may
 *         call it again on memory it already wrote to (rollback reloads).
 *       - The observation is taken AFTER the module's own writes.
 *
 *  2. OnConfirmedFrame (authoritative). Called exactly once per frame, in
 *     strictly increasing frame order, with the observation recorded by the
 *     last OnFrame call for that frame, once no peer can roll that frame back
 *     any more. This is the only place the module may keep state (score,
 *     set progress) and the only place it may call EmitEvent.
 *
 * Because every peer confirms the same frames with the same observations,
 * every peer reaches the same verdict about the set.
 *
 * Memory helpers take N64 virtual addresses (0x80000000-0x807FFFFF, KSEG0 or
 * KSEG1) and handle RDRAM byte order. Out-of-range accesses read as 0 and are
 * ignored on write.
 */

#ifndef RMGK_RANKED_MODULE_API_H
#define RMGK_RANKED_MODULE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RANKED_MODULE_API_VERSION 1

/* maximum bytes a module may store per frame in OnFrame */
#define RANKED_MAX_OBSERVATION_SIZE 128
/* maximum controller ports */
#define RANKED_MAX_PLAYERS 4

typedef enum
{
    RankedLog_Error   = 1,
    RankedLog_Warning = 2,
    RankedLog_Info    = 3,
    RankedLog_Verbose = 4,
} RankedLogLevel;

typedef enum
{
    /* a game (e.g. a single stock match) began */
    RankedEvent_GameStarted = 1,
    /* a game finished with a winner; counts toward the set */
    RankedEvent_GameEnded   = 2,
    /* a game finished without a valid result (reset, CPU, wrong rules...);
       it does not count and is replayed */
    RankedEvent_GameVoided  = 3,
    /* the set is decided; the host ends the session after this */
    RankedEvent_SetEnded    = 4,
} RankedEventType;

typedef struct
{
    /* character id as the game defines it, -1 when unknown */
    int32_t character;
    /* remaining stocks (or equivalent), -1 when unknown */
    int32_t stocksLeft;
    /* free-form per-game stat (e.g. damage percent), -1 when unused */
    int32_t extra;
} RankedPlayerStats;

typedef struct
{
    uint32_t size; /* sizeof(RankedEvent), set by the module */
    RankedEventType type;
    /* confirmed frame the event happened on */
    uint32_t frame;
    /* 1-based game number within the set */
    int32_t gameNumber;
    /* 1-based controller port of the game/set winner, 0 when none */
    int32_t winnerPort;
    /* games won per port (index 0 = port 1) after this event */
    int32_t score[RANKED_MAX_PLAYERS];
    /* per port stats of the game that just ended (GameEnded only) */
    RankedPlayerStats players[RANKED_MAX_PLAYERS];
    /* game specific stage/map id, -1 when unknown */
    int32_t stage;
    /* short human readable detail, e.g. why a game was voided */
    char detail[128];

    /* Team sets (added later; present when size covers them). */
    /* each port's team, 1 or 2 (team 1 holds the lowest ranked port), 0 for
       ports outside the set or when the set isn't a team set */
    int32_t teams[RANKED_MAX_PLAYERS];
    /* the team that won the game/set, 1 or 2; 0 when none or not a team set */
    int32_t winnerTeam;
} RankedEvent;

/* size of a RankedEvent from modules that predate the team fields */
#define RANKED_EVENT_SIZE_V1 offsetof(RankedEvent, teams)

typedef struct RankedHostApi
{
    uint32_t size; /* sizeof(RankedHostApi) */
    uint32_t apiVersion; /* RANKED_MODULE_API_VERSION */
    void* context; /* pass back as the first argument of every function */

    uint8_t  (*Read8)(void* context, uint32_t address);
    uint16_t (*Read16)(void* context, uint32_t address);
    uint32_t (*Read32)(void* context, uint32_t address);
    void     (*Write8)(void* context, uint32_t address, uint8_t value);
    void     (*Write16)(void* context, uint32_t address, uint16_t value);
    void     (*Write32)(void* context, uint32_t address, uint32_t value);

    void (*Log)(void* context, RankedLogLevel level, const char* message);

    /* only valid inside OnConfirmedFrame */
    void (*EmitEvent)(void* context, const RankedEvent* event);

    /* Writes a word of game *code* and drops any recompiled copy of it, so
       the patch takes effect under the dynamic recompiler. Use it (instead
       of Write32) only for instructions; it is much slower. Check
       host->size before use: older hosts don't have it. */
    void (*WriteCode32)(void* context, uint32_t address, uint32_t value);
} RankedHostApi;

typedef struct
{
    uint32_t size; /* sizeof(RankedModuleInfo), set by the host */
    /* filled by the module; static strings owned by the module */
    uint32_t apiVersion;
    const char* id;          /* stable id, e.g. "smashremix" */
    const char* name;        /* display name */
    const char* version;     /* module version; must match between peers */
} RankedModuleInfo;

typedef struct
{
    uint32_t size; /* sizeof(RankedSessionConfig) */
    /* ruleset id, e.g. "remix-1v1-standard"; NULL or "" = module default */
    const char* rulesetId;
    /* number of games in the set (odd: 1, 3, 5, ...) */
    int32_t bestOf;
    /* upper-case hex MD5 of the loaded ROM */
    const char* romMd5;
    /* directory holding this module's data files (UTF-8) */
    const char* dataDirectory;
    /* number of ranked players and their 1-based controller ports */
    int32_t numPlayers;
    int32_t ports[RANKED_MAX_PLAYERS];

    /* Team sets (added later; present when size covers them). */
    /* each port's required team (index 0 = port 1), 1 or 2 (team 1 holds the
       lowest ranked port), when the lobby matched these players as teammates;
       all 0 when the players pick their teams in the game */
    int32_t teams[RANKED_MAX_PLAYERS];
} RankedSessionConfig;

/* size of a RankedSessionConfig from hosts that predate the team field */
#define RANKED_SESSION_CONFIG_SIZE_V1 offsetof(RankedSessionConfig, teams)

typedef struct RankedModuleInstance RankedModuleInstance;

/*
 * Functions every module exports (C linkage, exact names):
 */

/* fills info; returns 0 on failure */
typedef int (*ptr_RankedModule_GetInfo)(RankedModuleInfo* info);

/* creates a session. Returns NULL on failure and writes a reason
   (NUL terminated) to error. */
typedef RankedModuleInstance* (*ptr_RankedModule_Create)(const RankedHostApi* host,
    const RankedSessionConfig* config, char* error, uint32_t errorSize);

/* writes a canonical description of the effective ruleset (everything that
   changes gameplay) to out; peers compare hashes of it before a ranked match.
   Returns the number of bytes needed (excluding NUL); out may be NULL
   when outSize is 0. Must not change during a session. */
typedef uint32_t (*ptr_RankedModule_GetRulesetDescription)(RankedModuleInstance* instance,
    char* out, uint32_t outSize);

/* speculative phase, see above. observation points at
   RANKED_MAX_OBSERVATION_SIZE zeroed bytes. */
typedef void (*ptr_RankedModule_OnFrame)(RankedModuleInstance* instance, uint32_t frame,
    uint8_t* observation);

/* authoritative phase, see above */
typedef void (*ptr_RankedModule_OnConfirmedFrame)(RankedModuleInstance* instance, uint32_t frame,
    const uint8_t* observation);

/* short status for on-screen display (e.g. "Game 2 - 1:0"); returns the
   number of bytes needed (excluding NUL). Called from the UI thread;
   the module must guard its own state. */
typedef uint32_t (*ptr_RankedModule_GetStatusText)(RankedModuleInstance* instance,
    char* out, uint32_t outSize);

typedef void (*ptr_RankedModule_Destroy)(RankedModuleInstance* instance);

#ifdef __cplusplus
}
#endif

#endif /* RMGK_RANKED_MODULE_API_H */
