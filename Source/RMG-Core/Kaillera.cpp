/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "Kaillera.hpp"
#include "Settings.hpp"
#include "Error.hpp"

#ifdef NETPLAY

#include "n02_client.h"
#include "kailleraclient.h"

#include <cstring>
#include <filesystem>
#include <system_error>

//
// Static Variables
//

static bool s_Initialized = false;
static bool s_GameActive = false;
static int s_PlayerNumber = 0; // 0 = not in netplay, 1-4 = player number
static int s_NumPlayers = 0;   // Total number of players in the game
static std::string s_AppName;
static std::string s_GameList;
static bool s_RecordingStorageStatusInitialized = false;
static bool s_RecordingStorageOverCap = false;
static uint64_t s_RecordingStorageBytes = 0;

// Callback storage
static CoreKaillera::GameStartCallback s_GameStartCallback;
static CoreKaillera::ChatReceivedCallback s_ChatReceivedCallback;
static CoreKaillera::ClientDroppedCallback s_ClientDroppedCallback;
static CoreKaillera::MoreInfosCallback s_MoreInfosCallback;

static std::filesystem::path pathFromUtf8String(const std::string& utf8)
{
#if defined(__cpp_char8_t)
    std::u8string converted;
    converted.reserve(utf8.size());
    for (unsigned char ch : utf8)
    {
        converted.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(converted);
#else
    return std::filesystem::u8path(utf8);
#endif
}

static std::filesystem::path getKailleraRecordsDirectoryPath()
{
    std::string recordsDirectory = CoreSettingsGetStringValue(SettingsID::Kaillera_RecordsDirectory);
    if (recordsDirectory.empty())
    {
        recordsDirectory = "records";
    }
    return pathFromUtf8String(recordsDirectory);
}

static uint64_t computeKrecDirectorySizeBytes(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
    {
        return 0;
    }

    uint64_t totalBytes = 0;
    std::filesystem::directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::directory_iterator end;

    while (!ec && it != end)
    {
        const auto& entry = *it;
        // Only count .krec files — the cap manages recordings, not exported MP4s or other files.
        if (entry.is_regular_file(ec) && entry.path().extension() == ".krec")
        {
            uintmax_t fileSize = entry.file_size(ec);
            if (!ec)
            {
                totalBytes += static_cast<uint64_t>(fileSize);
            }
        }

        ec.clear();
        it.increment(ec);
    }

    return totalBytes;
}

//
// C Callback Bridges (called by n02 from its internal thread)
//

static int GameCallbackBridge(char *game, int player, int numplayers)
{
    // Store player number and total player count
    s_PlayerNumber = player;
    s_NumPlayers = numplayers;

    // Set game active BEFORE callback so emulation thread sees it immediately
    s_GameActive = true;

    if (s_GameStartCallback)
    {
        try
        {
            s_GameStartCallback(std::string(game ? game : ""), player, numplayers);
            return 0; // Success
        }
        catch (...)
        {
            s_GameActive = false;
            return -1; // Error
        }
    }
    return 0;
}

static void ChatReceivedCallbackBridge(char *nick, char *text)
{
    if (s_ChatReceivedCallback)
    {
        try
        {
            s_ChatReceivedCallback(std::string(nick ? nick : ""), std::string(text ? text : ""));
        }
        catch (...)
        {
            // Ignore errors in callback
        }
    }
}

static void ClientDroppedCallbackBridge(char *nick, int playernb)
{
    if (s_ClientDroppedCallback)
    {
        try
        {
            s_ClientDroppedCallback(std::string(nick ? nick : ""), playernb);
        }
        catch (...)
        {
            // Ignore errors in callback
        }
    }
}

static void MoreInfosCallbackBridge(char *gamename)
{
    if (s_MoreInfosCallback)
    {
        try
        {
            s_MoreInfosCallback(std::string(gamename ? gamename : ""));
        }
        catch (...)
        {
            // Ignore errors in callback
        }
    }
}

//
// Exported Functions
//

CORE_EXPORT bool CoreInitKaillera(void)
{
    if (s_Initialized)
    {
        return true; // Already initialized
    }

    // Initialize n02 subsystem (sockets, settings, modules)
    int ret = n02::init();
    if (ret != 0)
    {
        CoreSetError("Kaillera initialization failed with error code: " + std::to_string(ret));
        return false;
    }

    // Verify version
    char version[16];
    n02::getVersion(version);

    // Load settings from RMG-K config into n02 globals
    int mode = CoreSettingsGetIntValue(SettingsID::Kaillera_ActiveMode);
    if (mode < 0 || mode > 1) mode = 1; // Clamp to P2P(0) or Server(1)
    n02::activateMode(mode);

    kaillera_spoof_ping = CoreSettingsGetIntValue(SettingsID::Kaillera_SpoofPing);
    kaillera_30fps_mode = CoreSettingsGetBoolValue(SettingsID::Kaillera_30fpsMode) ? 1 : 0;
    p2p_30fps_mode = CoreSettingsGetBoolValue(SettingsID::Kaillera_30fpsMode) ? 1 : 0;
    n02::setRecordsDirectory(CoreGetKailleraRecordsDirectory());

    s_Initialized = true;
    s_GameActive = false;

    return true;
}

CORE_EXPORT bool CoreShutdownKaillera(void)
{
    if (!s_Initialized)
    {
        return true; // Not initialized, nothing to do
    }

    // End game if active
    if (s_GameActive)
    {
        CoreEndKailleraGame();
    }

    // Save settings back to RMG-K config
    CoreSettingsSetValue(SettingsID::Kaillera_ActiveMode, n02::getActiveMode());

    // Shutdown n02 subsystem
    n02::shutdown();

    s_Initialized = false;
    s_GameActive = false;

    return true;
}

CORE_EXPORT bool CoreHasInitKaillera(void)
{
    return s_Initialized && s_GameActive;
}

CORE_EXPORT bool CoreIsKailleraPlaybackMode(void)
{
    return s_Initialized && s_GameActive && (n02::getActiveMode() == 2);
}

CORE_EXPORT bool CoreShowKailleraServerDialog(void* parentHwnd)
{
    if (!s_Initialized)
    {
        CoreSetError("Kaillera not initialized. Call CoreInitKaillera() first");
        return false;
    }

    // Show n02 server dialog (blocking in Phase 1, Qt-driven in Phase 6)
    n02::selectServerDialog(parentHwnd);

    return true;
}

CORE_EXPORT int CoreModifyKailleraPlayValues(void* values, int size)
{
    if (!s_Initialized || !s_GameActive)
    {
        return -1; // Not in netplay mode
    }

    // Call n02 to synchronize input
    return n02::modifyPlayValues(values, size);
}

CORE_EXPORT bool CoreKailleraSendChat(std::string text)
{
    if (!s_Initialized || !s_GameActive)
    {
        return false;
    }

    // n02 expects non-const char*
    char* textBuf = new char[text.length() + 1];
    strcpy(textBuf, text.c_str());

    n02::chatSend(textBuf);

    delete[] textBuf;
    return true;
}

CORE_EXPORT bool CoreEndKailleraGame(void)
{
    if (!s_Initialized)
    {
        return false;
    }

    n02::endGame();

    s_GameActive = false;
    return true;
}

CORE_EXPORT void CoreMarkKailleraGameInactive(void)
{
    // Mark game as inactive without calling n02::endGame()
    // Used when the game ends due to network issues or another player dropping
    s_GameActive = false;
}

CORE_EXPORT void CoreSetKailleraCallbacks(
    CoreKaillera::GameStartCallback gameStartCallback,
    CoreKaillera::ChatReceivedCallback chatReceivedCallback,
    CoreKaillera::ClientDroppedCallback clientDroppedCallback,
    CoreKaillera::MoreInfosCallback moreInfosCallback)
{
    s_GameStartCallback = gameStartCallback;
    s_ChatReceivedCallback = chatReceivedCallback;
    s_ClientDroppedCallback = clientDroppedCallback;
    s_MoreInfosCallback = moreInfosCallback;

    // Set up kailleraInfos structure
    if (s_Initialized)
    {
        static kailleraInfos infos;

        // Allocate app name (must persist)
        static char appNameBuf[128];
        strncpy(appNameBuf, s_AppName.c_str(), sizeof(appNameBuf) - 1);
        appNameBuf[sizeof(appNameBuf) - 1] = '\0';
        infos.appName = appNameBuf;

        // Allocate game list (must persist)
        static char* gameListBuf = nullptr;
        if (gameListBuf)
        {
            delete[] gameListBuf;
        }
        gameListBuf = new char[s_GameList.length() + 1];
        memcpy(gameListBuf, s_GameList.c_str(), s_GameList.length() + 1);
        infos.gameList = gameListBuf;

        // Set callbacks
        infos.gameCallback = GameCallbackBridge;
        infos.chatReceivedCallback = s_ChatReceivedCallback ? ChatReceivedCallbackBridge : nullptr;
        infos.clientDroppedCallback = s_ClientDroppedCallback ? ClientDroppedCallbackBridge : nullptr;
        infos.moreInfosCallback = s_MoreInfosCallback ? MoreInfosCallbackBridge : nullptr;

        n02::setInfos(&infos);
    }
}

CORE_EXPORT bool CoreSetKailleraAppInfo(std::string appName, std::string gameList)
{
    s_AppName = appName;
    s_GameList = gameList;

    // Update kailleraInfos if already initialized
    if (s_Initialized)
    {
        // Trigger callback update which will also update app info
        CoreSetKailleraCallbacks(
            s_GameStartCallback,
            s_ChatReceivedCallback,
            s_ClientDroppedCallback,
            s_MoreInfosCallback
        );
    }

    return true;
}

CORE_EXPORT void CoreSetKailleraPlayerNumber(int playerNumber)
{
    s_PlayerNumber = playerNumber;
}

CORE_EXPORT int CoreGetKailleraPlayerNumber(void)
{
    return s_PlayerNumber;
}

CORE_EXPORT int CoreGetKailleraNumPlayers(void)
{
    return s_NumPlayers;
}

CORE_EXPORT int CoreGetKailleraFrameDelay(void)
{
    if (!s_Initialized || !s_GameActive)
    {
        return 0;
    }

    return n02::getFrameDelay();
}

CORE_EXPORT std::string CoreGetKailleraRecordsDirectory(void)
{
    std::string recordsDirectory = CoreSettingsGetStringValue(SettingsID::Kaillera_RecordsDirectory);
    if (recordsDirectory.empty())
    {
        recordsDirectory = "records";
    }
    return recordsDirectory;
}

CORE_EXPORT bool CoreRefreshKailleraRecordingStorageStatus(void)
{
    const bool recordingEnabled = CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingEnabled);
    const bool capEnabled = CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingCapEnabled);
    if (!recordingEnabled || !capEnabled)
    {
        s_RecordingStorageBytes = 0;
        s_RecordingStorageStatusInitialized = true;
        s_RecordingStorageOverCap = false;
        return false;
    }

    s_RecordingStorageBytes = computeKrecDirectorySizeBytes(getKailleraRecordsDirectoryPath());
    s_RecordingStorageStatusInitialized = true;

    int capMB = CoreSettingsGetIntValue(SettingsID::Kaillera_RecordingCapMB);
    if (capMB < 1)
    {
        capMB = 1;
    }
    const uint64_t capBytes = static_cast<uint64_t>(capMB) * 1024ULL * 1024ULL;

    s_RecordingStorageOverCap = (s_RecordingStorageBytes > capBytes);
    return s_RecordingStorageOverCap;
}

CORE_EXPORT bool CoreIsKailleraRecordingStorageOverCap(void)
{
    return s_RecordingStorageStatusInitialized && s_RecordingStorageOverCap;
}

CORE_EXPORT uint64_t CoreGetKailleraRecordingStorageBytes(void)
{
    return s_RecordingStorageBytes;
}

CORE_EXPORT bool CoreGetKailleraEffectiveRecordingDefault(void)
{
    const bool recordingEnabled = CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingEnabled);
    const bool capEnabled = CoreSettingsGetBoolValue(SettingsID::Kaillera_RecordingCapEnabled);
    if (!recordingEnabled || !capEnabled)
    {
        return recordingEnabled;
    }

    if (!s_RecordingStorageStatusInitialized)
    {
        return recordingEnabled;
    }

    return !s_RecordingStorageOverCap;
}

#else // !NETPLAY

// Stub implementations for platforms/builds without integrated Kaillera support

CORE_EXPORT bool CoreInitKaillera(void)
{
    CoreSetError("Kaillera is only available in Windows builds with netplay enabled");
    return false;
}

CORE_EXPORT bool CoreShutdownKaillera(void)
{
    return true;
}

CORE_EXPORT bool CoreHasInitKaillera(void)
{
    return false;
}

CORE_EXPORT bool CoreIsKailleraPlaybackMode(void)
{
    return false;
}

CORE_EXPORT bool CoreShowKailleraServerDialog(void* parentHwnd)
{
    (void)parentHwnd;
    CoreSetError("Kaillera is only available in Windows builds with netplay enabled");
    return false;
}

CORE_EXPORT int CoreModifyKailleraPlayValues(void* values, int size)
{
    (void)values;
    (void)size;
    return -1;
}

CORE_EXPORT bool CoreKailleraSendChat(std::string text)
{
    (void)text;
    return false;
}

CORE_EXPORT bool CoreEndKailleraGame(void)
{
    return true;
}

CORE_EXPORT void CoreMarkKailleraGameInactive(void)
{
    // No-op on non-Windows
}

CORE_EXPORT void CoreSetKailleraCallbacks(
    CoreKaillera::GameStartCallback gameStartCallback,
    CoreKaillera::ChatReceivedCallback chatReceivedCallback,
    CoreKaillera::ClientDroppedCallback clientDroppedCallback,
    CoreKaillera::MoreInfosCallback moreInfosCallback)
{
    (void)gameStartCallback;
    (void)chatReceivedCallback;
    (void)clientDroppedCallback;
    (void)moreInfosCallback;
}

CORE_EXPORT bool CoreSetKailleraAppInfo(std::string appName, std::string gameList)
{
    (void)appName;
    (void)gameList;
    return false;
}

CORE_EXPORT void CoreSetKailleraPlayerNumber(int playerNumber)
{
    (void)playerNumber;
}

CORE_EXPORT int CoreGetKailleraPlayerNumber(void)
{
    return 0;
}

CORE_EXPORT int CoreGetKailleraNumPlayers(void)
{
    return 0;
}

CORE_EXPORT int CoreGetKailleraFrameDelay(void)
{
    return 0;
}

CORE_EXPORT std::string CoreGetKailleraRecordsDirectory(void)
{
    return "records";
}

CORE_EXPORT bool CoreRefreshKailleraRecordingStorageStatus(void)
{
    return false;
}

CORE_EXPORT bool CoreIsKailleraRecordingStorageOverCap(void)
{
    return false;
}

CORE_EXPORT uint64_t CoreGetKailleraRecordingStorageBytes(void)
{
    return 0;
}

CORE_EXPORT bool CoreGetKailleraEffectiveRecordingDefault(void)
{
    return false;
}

#endif // NETPLAY
