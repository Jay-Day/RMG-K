/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_KAILLERA_HPP
#define CORE_KAILLERA_HPP

#include "Library.hpp"

#include <string>
#include <functional>
#include <cstdint>

namespace CoreKaillera
{
    // Callback function types
    using GameStartCallback = std::function<void(std::string gameName, int playerNumber, int numPlayers)>;
    using ChatReceivedCallback = std::function<void(std::string nickname, std::string text)>;
    using ClientDroppedCallback = std::function<void(std::string nickname, int playerNumber)>;
    using MoreInfosCallback = std::function<void(std::string gameName)>;
}

//
// Exported Functions
//

// Initialize Kaillera client library
// Returns true on success, false on failure (check CoreGetError() for details)
CORE_EXPORT bool CoreInitKaillera(void);

// Shutdown Kaillera client library
CORE_EXPORT bool CoreShutdownKaillera(void);

// Check if Kaillera has been initialized
CORE_EXPORT bool CoreHasInitKaillera(void);

// Check if Kaillera is in playback mode (as opposed to live netplay)
CORE_EXPORT bool CoreIsKailleraPlaybackMode(void);

// Show Kaillera server selection dialog
// parentHwnd: Handle to parent window (HWND on Windows)
// Returns true if user connected to a server, false if cancelled
CORE_EXPORT bool CoreShowKailleraServerDialog(void* parentHwnd);

// Modify play values (called every frame during netplay)
// Sends local input data and receives synchronized data from all players
// values: Pointer to input buffer (will be modified in-place with synced data)
// size: Size of input buffer in bytes
// Returns number of bytes received, or -1 on error
CORE_EXPORT int CoreModifyKailleraPlayValues(void* values, int size);

// Send chat message to other players
CORE_EXPORT bool CoreKailleraSendChat(std::string text);

// End current Kaillera game session
CORE_EXPORT bool CoreEndKailleraGame(void);

// Mark Kaillera game as inactive without calling kailleraEndGame()
// Used when game ends due to network issues or player dropping
CORE_EXPORT void CoreMarkKailleraGameInactive(void);

// Set Kaillera callbacks
// These callbacks are invoked from Kaillera's internal thread
// Implementations must be thread-safe!
CORE_EXPORT void CoreSetKailleraCallbacks(
    CoreKaillera::GameStartCallback gameStartCallback,
    CoreKaillera::ChatReceivedCallback chatReceivedCallback,
    CoreKaillera::ClientDroppedCallback clientDroppedCallback,
    CoreKaillera::MoreInfosCallback moreInfosCallback
);

// Set application info for Kaillera
// appName: Name displayed in Kaillera dialogs
// gameList: List of supported games (NULL-terminated strings with double-NULL at end)
CORE_EXPORT bool CoreSetKailleraAppInfo(std::string appName, std::string gameList);

// Set/Get the current player number for Kaillera netplay
// Player numbers are 1-4 (0 = not in netplay)
CORE_EXPORT void CoreSetKailleraPlayerNumber(int playerNumber);
CORE_EXPORT int CoreGetKailleraPlayerNumber(void);

// Get the total number of players in the current Kaillera game
// Returns 0 if not in a game, otherwise 1-8
CORE_EXPORT int CoreGetKailleraNumPlayers(void);

// Get the frame delay assigned by the Kaillera server
// Returns the number of frames to buffer inputs (0 if not in game)
CORE_EXPORT int CoreGetKailleraFrameDelay(void);

// Get the configured records directory path used for Kaillera recordings
CORE_EXPORT std::string CoreGetKailleraRecordsDirectory(void);

// Refresh cached records-folder usage status by scanning the records directory.
// Returns true if the folder is currently above the configured cap.
CORE_EXPORT bool CoreRefreshKailleraRecordingStorageStatus(void);

// Return the cached "over cap" state. This does not scan.
CORE_EXPORT bool CoreIsKailleraRecordingStorageOverCap(void);

// Return the cached records-folder size in bytes.
CORE_EXPORT uint64_t CoreGetKailleraRecordingStorageBytes(void);

// Return the effective "record by default" value:
// user setting && !(cap enabled && cached over-cap state)
CORE_EXPORT bool CoreGetKailleraEffectiveRecordingDefault(void);

// Set/get player display names for the current netplay session.
// playerNumber is 1-indexed (1-4). Unknown players default to "Player N".
// Call CoreNetplayClearPlayerNames() when a session ends.
CORE_EXPORT void CoreNetplaySetPlayerName(int playerNumber, std::string name);
CORE_EXPORT std::string CoreNetplayGetPlayerName(int playerNumber);
CORE_EXPORT void CoreNetplayClearPlayerNames(void);

#endif // CORE_KAILLERA_HPP
