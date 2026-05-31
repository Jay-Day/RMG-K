/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "MediaLoader.hpp"
#include "RomSettings.hpp"
#include "Emulation.hpp"
#include "RomHeader.hpp"
#include "Settings.hpp"
#include "SaveState.hpp"
#include "Library.hpp"
#include "Netplay.hpp"
#include "Kaillera.hpp"
#include "Plugins.hpp"
#include "Cheats.hpp"
#include "Error.hpp"
#include "File.hpp"
#include "Rom.hpp"
#include "rmgk_gekko.hpp"

#include "m64p/Api.hpp"

#ifdef SCRIPTING_ENABLED
#include "../RMG-Scripting/ScriptManager.hpp"
#endif

#include <cstdlib>

// Windows/POSIX dynamic loading
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void setRollbackLoggingEnvironment(void)
{
    const bool pifLogging = CoreSettingsGetBoolValue(SettingsID::Rollback_VerbosePifInputLogging);
    const bool glideLogging = CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseGlideInputLogging);
#ifdef _WIN32
    _putenv_s("RMGK_VERBOSE_PIF_INPUT_LOGGING", pifLogging ? "1" : "0");
    _putenv_s("RMGK_VERBOSE_GLIDE_INPUT_LOGGING", glideLogging ? "1" : "0");
#else
    setenv("RMGK_VERBOSE_PIF_INPUT_LOGGING", pifLogging ? "1" : "0", 1);
    setenv("RMGK_VERBOSE_GLIDE_INPUT_LOGGING", glideLogging ? "1" : "0", 1);
#endif
}

// Forward declarations for PIF structures
extern "C" {
    struct pif;
    struct pif_channel {
        void* jbd;
        const void* ijbd;
        uint8_t* tx;
        uint8_t* tx_buf;
        uint8_t* rx;
        uint8_t* rx_buf;
    };
    enum {
        PIF_CHANNELS_COUNT = 5,
        PIF_CONTROLLER_CHANNELS_COUNT = 4
    };

    struct pif {
        uint8_t* base;
        uint8_t* ram;
        struct pif_channel channels[PIF_CHANNELS_COUNT];
    };

    // Joybus command constants
    enum {
        JCMD_STATUS = 0x00,
        JCMD_CONTROLLER_READ = 0x01,
        JCMD_PAK_READ = 0x02,
        JCMD_PAK_WRITE = 0x03,
        JCMD_EEPROM_READ = 0x04,
        JCMD_EEPROM_WRITE = 0x05,
        JCMD_RESET = 0xff
    };

    typedef void (*pif_sync_callback_t)(struct pif*);
}

static bool parse_gekko_address(const std::string& address, std::string& remoteAddress, int& remotePort, int& frameDelay, int& predictionWindow)
{
    constexpr const char* prefix = "GEKKO|";
    if (address.rfind(prefix, 0) != 0)
    {
        return false;
    }

    const size_t remoteStart = std::char_traits<char>::length(prefix);
    const size_t portSeparator = address.find('|', remoteStart);
    if (portSeparator == std::string::npos)
    {
        return false;
    }
    const size_t delaySeparator = address.find('|', portSeparator + 1);
    if (delaySeparator == std::string::npos)
    {
        return false;
    }
    const size_t predictionSeparator = address.find('|', delaySeparator + 1);

    remoteAddress = address.substr(remoteStart, portSeparator - remoteStart);
    try
    {
        remotePort = std::stoi(address.substr(portSeparator + 1, delaySeparator - portSeparator - 1));
        if (predictionSeparator == std::string::npos)
        {
            frameDelay = std::stoi(address.substr(delaySeparator + 1));
            predictionWindow = 7;
        }
        else
        {
            frameDelay = std::stoi(address.substr(delaySeparator + 1, predictionSeparator - delaySeparator - 1));
            predictionWindow = std::stoi(address.substr(predictionSeparator + 1));
        }
    }
    catch (...)
    {
        return false;
    }

    return !remoteAddress.empty() && remotePort > 0 && remotePort <= 65535 && frameDelay >= 0 && predictionWindow >= 1;
}

//
// Local Variables
//

// Frame counter for Kaillera sync (updated via frame callback)
static int s_CurrentFrame = 0;

static bool pif_channel_has_command(const pif_channel& channel)
{
    return channel.tx != nullptr &&
           channel.rx != nullptr &&
           channel.tx_buf != nullptr &&
           channel.rx_buf != nullptr;
}

// Snapshot of controller channel data captured on each PIF read.
// Populated by SnapshotPifChannels; consumed by CoreGetControllerState.
struct PifControllerSnapshot {
    bool valid = false;
    bool connected = false;
    uint8_t data[4] = {};  // [buttons_hi][buttons_lo][x][y]
};
static PifControllerSnapshot s_ControllerSnapshot[PIF_CONTROLLER_CHANNELS_COUNT] = {};

static void SnapshotPifChannels(struct pif* pif)
{
    for (int i = 0; i < PIF_CONTROLLER_CHANNELS_COUNT; i++) {
        const pif_channel& ch = pif->channels[i];
        const bool has = pif_channel_has_command(ch);
        // Bit 0x80 of the rx length byte is the JoyBus "device not present" error flag.
        // It is set by the core when no controller is connected on this channel.
        const bool connected = has && (*ch.rx & 0x80) == 0;
        s_ControllerSnapshot[i].connected = connected;
        if (connected && ch.tx_buf[0] == JCMD_CONTROLLER_READ) {
            s_ControllerSnapshot[i].valid = true;
            s_ControllerSnapshot[i].data[0] = ch.rx_buf[0];
            s_ControllerSnapshot[i].data[1] = ch.rx_buf[1];
            s_ControllerSnapshot[i].data[2] = ch.rx_buf[2];
            s_ControllerSnapshot[i].data[3] = ch.rx_buf[3];
        } else {
            s_ControllerSnapshot[i].valid = false;
        }
    }
}

#ifdef SCRIPTING_ENABLED
static void ScriptingPifCallback(struct pif* pif)
{
    SnapshotPifChannels(pif);
}
#endif

#ifdef NETPLAY
// Maximum players supported by Kaillera
#define MAX_PLAYERS 8

// Cache for preventing duplicate syncs within same frame
static int s_LastSyncFrame = -1;
static uint32_t s_CachedSyncBuffer[MAX_PLAYERS] = {0};
static int s_CachedNumReceived = 0;

// Track whether we've already synced since the last frame advance
// This is more reliable than comparing frame numbers due to callback timing
static bool s_SyncedThisFrame = false;
#endif
// Frame callback function
static void FrameCallback(unsigned int frameIndex)
{
    s_CurrentFrame = frameIndex;
#ifdef NETPLAY
    // Reset sync flag at the start of each new frame
    // This ensures we sync exactly once per frame regardless of PIF polling timing
    s_SyncedThisFrame = false;
#endif
    
#ifdef SCRIPTING_ENABLED
    // Call script engine frame callbacks (fire on a non-emulation thread)
    // If the core exports debugger functions, sample the current PC once per frame.
    uint32_t pc = 0;
    {
        using ptr_DebugGetCPUDataPtr = void* (*)(m64p_dbg_cpu_data);
        static ptr_DebugGetCPUDataPtr getPtr = nullptr;
        static bool resolved = false;
        if (!resolved) {
            void* coreHandle = m64p::Core.GetHandle();
            if (coreHandle) {
#ifdef _WIN32
                getPtr = (ptr_DebugGetCPUDataPtr)GetProcAddress((HMODULE)coreHandle, "DebugGetCPUDataPtr");
#else
                getPtr = (ptr_DebugGetCPUDataPtr)dlsym(coreHandle, "DebugGetCPUDataPtr");
#endif
            }
            resolved = true;
        }
        if (getPtr) {
            void* pcPtr = getPtr(M64P_CPU_PC);
            if (pcPtr) {
                pc = *static_cast<uint32_t*>(pcPtr);
            }
        }
    }

    ScriptManager::GetInstance().OnFrameUpdate(pc);
#endif
}

// Kaillera PIF sync callback (called from mupen64plus-core after netplay sync)
static void KailleraPifSyncCallback(struct pif* pif)
{
#ifdef SCRIPTING_ENABLED
    SnapshotPifChannels(pif);
#endif
#ifdef NETPLAY
    if (rmgk_gekko::is_netplay_session_active()) {
        return;
    }

    if (!CoreHasInitKaillera()) {
        return;
    }

    int player_num = CoreGetKailleraPlayerNumber();
    int num_players = CoreGetKailleraNumPlayers();

    if (player_num < 1 || player_num > MAX_PLAYERS) {
        return; // Invalid player number
    }

    // Check if this is a controller read command for channel 0 (local player)
    // We only want to sync on actual input reads, not status queries or other commands
    bool isControllerRead = (pif_channel_has_command(pif->channels[0]) &&
                             pif->channels[0].tx_buf[0] == JCMD_CONTROLLER_READ);

    // Only sync with Kaillera on controller read commands, and only once per frame
    // This prevents syncing on JCMD_STATUS which would send zero input
    if (isControllerRead && !s_SyncedThisFrame) {
        // First controller read this frame - read local input and sync with Kaillera
        s_SyncedThisFrame = true;  // Mark as synced BEFORE calling Kaillera

        // Read 4-byte controller response from local controller
        // N64 controller format: [buttons_hi][buttons_lo][x_axis][y_axis]
        uint8_t* rx = pif->channels[0].rx_buf;
        uint32_t local_input = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];

        uint32_t sync_buffer[MAX_PLAYERS] = {0};
        sync_buffer[0] = local_input;

        // Synchronize with Kaillera - this must be called exactly ONCE per emulator frame
        int ret = CoreModifyKailleraPlayValues(sync_buffer, sizeof(uint32_t));

        if (ret < 0) {
            // Game ended or network error. For live netplay we keep emulation
            // running so the user can manually exit; for krec playback there
            // are no more inputs to feed, so stop emulation here (the dialog
            // timer would otherwise only catch this while the dialog is open).
            const bool wasPlayback = CoreIsKailleraPlaybackMode();
            CoreMarkKailleraGameInactive();
            s_CachedNumReceived = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                s_CachedSyncBuffer[i] = 0;
            }
            if (wasPlayback) {
                CoreStopEmulation();
            }
            return;
        }

        if (ret == 0) {
            // Frame delay period - n02 returns 0 while buffering initial frames
            // Use cached input from previous sync (or zeros if none yet)
            return;
        }

        int num_received = ret / sizeof(uint32_t);

        // Cache synced results for subsequent polls this frame and for writing to PIF
        s_CachedNumReceived = num_received;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            s_CachedSyncBuffer[i] = sync_buffer[i];
        }
    }

    // Write synchronized inputs to PIF RAM for all netplay players.
    // JCMD_STATUS/JCMD_RESET are handled unconditionally so games detect
    // controllers even before the first Kaillera sync (needed for playback
    // without a physical controller connected).
    // JCMD_CONTROLLER_READ data injection is gated on cache availability.
    int numPlayers = CoreGetKailleraNumPlayers();
    if (numPlayers > PIF_CONTROLLER_CHANNELS_COUNT) {
        numPlayers = PIF_CONTROLLER_CHANNELS_COUNT;
    }

    for (int i = 0; i < numPlayers && i < MAX_PLAYERS; i++) {
        if (pif_channel_has_command(pif->channels[i])) {
            // Always clear error bits to show controller as connected
            *pif->channels[i].rx &= ~0xC0;

            uint8_t cmd = pif->channels[i].tx_buf[0];

            if (cmd == JCMD_STATUS || cmd == JCMD_RESET) {
                // Controller detection - force standard controller type response
                if (pif->channels[i].rx_buf != NULL) {
                    uint16_t type = 0x0500; // JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0; // No pak status
                }
            }
            else if (cmd == JCMD_CONTROLLER_READ) {
                // Write synced controller input from cache (only when populated)
                if (s_CachedNumReceived > 0 && i < s_CachedNumReceived && pif->channels[i].rx_buf != NULL) {
                    uint8_t* rx = pif->channels[i].rx_buf;
                    rx[0] = (s_CachedSyncBuffer[i] >> 24) & 0xFF;
                    rx[1] = (s_CachedSyncBuffer[i] >> 16) & 0xFF;
                    rx[2] = (s_CachedSyncBuffer[i] >> 8) & 0xFF;
                    rx[3] = s_CachedSyncBuffer[i] & 0xFF;
                }
            }
            else if (cmd == JCMD_PAK_READ && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[32] = 255;
            }
            else if (cmd == JCMD_PAK_WRITE && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[0] = 255;
            }
        }
    }
#endif // NETPLAY
}

//
// Local Functions
//

static bool get_emulation_state(m64p_emu_state& state)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &state);
    if (ret != M64ERR_SUCCESS)
    {
        error = "get_emulation_state m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

static void apply_coresettings_overlay(void)
{
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CPU_Emulator));
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem));
    CoreSettingsSetValue(SettingsID::Core_EnableDebugger, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_EnableDebugger));
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOpDenomPot));
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration));
    CoreSettingsSetValue(SettingsID::Core_SaveFileNameFormat, CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat));
    CoreSettingsSetValue(SettingsID::Core_GbCameraVideoCaptureBackend1, CoreSettingsGetStringValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1));
    // Reset DisableSaveFileLoading to default (false) - Kaillera will override this later if needed
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, false);
}

static void apply_game_coresettings_overlay(void)
{
    std::string section;
    CoreRomSettings romSettings;
    bool overrideCoreSettings;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentDefaultRomSettings(romSettings))
    {
        return;
    }

    section = romSettings.MD5;

    // when we don't need to override the core settings, return
    overrideCoreSettings = CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, section);
    if (!overrideCoreSettings)
    {
        return;
    }

    // apply settings overlay
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::Game_RandomizeInterrupt, section));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::Game_CPU_Emulator, section));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, section));
}

#ifdef NETPLAY
// Force HLE RSP plugin for Kaillera netplay - must be called BEFORE ROM open
// so the setting takes effect when plugins are loaded
static void apply_kaillera_rsp_override(void)
{
#ifdef _WIN32
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.dll"));
#else
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.so"));
#endif
}

// Force deterministic settings for Kaillera netplay to prevent desync
// These settings MUST be identical across all clients
// Called AFTER overlays so user/game settings don't override these
static void apply_kaillera_deterministic_settings(void)
{
    // Disable RandomizeInterrupt - critical for deterministic emulation
    // When enabled, interrupt timing varies randomly which causes desync
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, false);

    // Use dynamic recompiler for best performance
    // Value 0 = Pure Interpreter, 1 = Cached Interpreter, 2 = Dynamic Recompiler
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);

    // Set consistent CountPerOp values for deterministic timing
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, 0);
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, 0);

    // Set consistent SI DMA duration
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, -1);

    // Force extra memory enabled (8MB expansion pak) for consistent memory layout
    // Different memory configurations between players causes desync
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, false);

    // Disable save file loading so all players start with fresh/empty saves
    // This prevents desync from players having different in-game settings saved
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, true);
}
#endif

static void apply_pif_rom_settings(void)
{
    CoreRomHeader romHeader;
    std::string error;
    m64p_error ret;
    int cpuEmulator;
    bool usePifROM;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentRomHeader(romHeader))
    {
        return;
    }

    // when we're using the dynarec, return
    cpuEmulator = CoreSettingsGetIntValue(SettingsID::Core_CPU_Emulator);
    if (cpuEmulator >= 2)
    {
        return;
    }

    usePifROM = CoreSettingsGetBoolValue(SettingsID::Core_PIF_Use);
    if (!usePifROM)
    {
        return;
    }

    const SettingsID settingsIds[] =
    {
        SettingsID::Core_PIF_NTSC,
        SettingsID::Core_PIF_PAL,
    };

    std::string rom = CoreSettingsGetStringValue(settingsIds[static_cast<int>(romHeader.SystemType)]);
    if (!std::filesystem::is_regular_file(rom))
    {
        return;
    }

    std::vector<char> buffer;
    if (!CoreReadFile(rom, buffer))
    {
        return;
    }

    ret = m64p::Core.DoCommand(M64CMD_PIF_OPEN, buffer.size(), buffer.data());
    if (ret != M64ERR_SUCCESS)
    {
        error = "open_pif_rom m64p::Core.DoCommand(M64CMD_PIF_OPEN) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }
}

//
// Exported Functions
//

CORE_EXPORT bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom,
    std::string address, int port, int player)
{
    std::string error;
    m64p_error  m64p_ret = M64ERR_SUCCESS;
    bool        netplay_ret = false;
    CoreRomType type;
    bool        netplay = !address.empty();

#ifdef NETPLAY
    // Apply RSP plugin override and reload plugins BEFORE ROM open
    if (netplay && (address == "KAILLERA" || address.rfind("GEKKO|", 0) == 0))
    {
        apply_kaillera_rsp_override();
        CoreApplyPluginSettings();  // Force reload with HLE RSP
    }
#endif

    if (!CoreOpenRom(n64rom))
    {
        return false;
    }

    if (!CoreApplyRomPluginSettings())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreArePluginsReady())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (!CoreAttachPlugins())
    {
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    if (netplay)
    { // netplay cheats
        if (!CoreApplyNetplayCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }
    else
    { // local cheats
        if (!CoreApplyCheats())
        {
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }
    }

    if (!CoreGetRomType(type))
    {
        CoreClearCheats();
        CoreDetachPlugins();
        CoreApplyPluginSettings();
        CoreCloseRom();
        return false;
    }

    // set disk file in media loader when ROM is a cartridge
    if (type == CoreRomType::Cartridge)
    {
        CoreMediaLoaderSetDiskFile(n64ddrom);
    }

    // apply core settings overlay
    apply_coresettings_overlay();

    // apply game core settings overrides
    apply_game_coresettings_overlay();

    // apply pif rom settings
    apply_pif_rom_settings();

    const bool localRollbackEnabled = !netplay &&
        CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting);

#ifdef NETPLAY
    // Apply deterministic settings AFTER all overlays for synchronized netplay or
    // explicit local rollback testing.
    if (localRollbackEnabled || (netplay && (address == "KAILLERA" || address.rfind("GEKKO|", 0) == 0)))
    {
        apply_kaillera_deterministic_settings();
    }

    // Kaillera connection happens BEFORE emulation via kailleraSelectServerDialog
    // Just verify it's initialized if netplay was requested
    if (netplay)
    {
        // Check if address is "KAILLERA" marker (set by UI when using Kaillera)
        if (address == "KAILLERA")
        {
            if (!CoreHasInitKaillera())
            {
                CoreSetError("CoreStartEmulation: Kaillera not initialized");
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                // Store player number for input plugin to use
                CoreSetKailleraPlayerNumber(player);
                netplay_ret = true;
            }
        }
        else if (address.rfind("GEKKO|", 0) == 0)
        {
            std::string remoteAddress;
            int remotePort = 0;
            int frameDelay = 0;
            int predictionWindow = 7;
            if (!parse_gekko_address(address, remoteAddress, remotePort, frameDelay, predictionWindow))
            {
                CoreSetError("CoreStartEmulation: invalid GekkoNet session parameters");
                m64p_ret = M64ERR_INPUT_INVALID;
                netplay_ret = false;
            }
            else if (!rmgk_gekko::set_deterministic(true))
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);
                netplay_ret = rmgk_gekko::start_p2p_session("rmgk-gekko",
                    2, static_cast<int>(sizeof(uint32_t)), player, static_cast<unsigned short>(port),
                    remoteAddress.c_str(), static_cast<unsigned short>(remotePort), frameDelay, predictionWindow);
                if (!netplay_ret)
                {
                    if (CoreGetError().empty())
                    {
                        CoreSetError("CoreStartEmulation: GekkoNet session initialization failed");
                    }
                    m64p_ret = M64ERR_SYSTEM_FAIL;
                }
            }
        }
        else
        {
            // Legacy netplay (Mupen64Plus built-in)
            netplay_ret = CoreInitNetplay(address, port, player);
            if (!netplay_ret)
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
            }
        }
    }
#endif // NETPLAY

    bool rollbackExecute = false;
    if (localRollbackEnabled)
    {
        if (!rmgk_gekko::set_deterministic(true))
        {
            m64p_ret = M64ERR_SYSTEM_FAIL;
        }
        else
        {
            CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);
            netplay_ret = rmgk_gekko::start_local_session("rmgk-gekko-local",
                2, static_cast<int>(sizeof(uint32_t)), 0);
            rollbackExecute = netplay_ret;
            if (!netplay_ret)
            {
                if (CoreGetError().empty())
                {
                    CoreSetError("CoreStartEmulation: local GekkoNet session initialization failed");
                }
                m64p_ret = M64ERR_SYSTEM_FAIL;
            }
        }
    }
#ifdef NETPLAY
    else
    {
        rollbackExecute = address.rfind("GEKKO|", 0) == 0;
    }
#endif

    // only start emulation when initializing netplay/local rollback
    // is successful or if there's legacy netplay requested
    if ((!netplay && (!localRollbackEnabled || rollbackExecute)) || (netplay && netplay_ret))
    {
        // Register frame callback for frame counter (used by Kaillera)
        s_CurrentFrame = 0;
        m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)FrameCallback);

#ifdef NETPLAY
        // Reset Kaillera sync state to prevent stale cache from previous sessions
        s_LastSyncFrame = -1;
        s_SyncedThisFrame = false;
        s_CachedNumReceived = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            s_CachedSyncBuffer[i] = 0;
        }
#endif

#if defined(SCRIPTING_ENABLED) || defined(NETPLAY)
        // Register PIF sync callback for scripting input capture and/or Kaillera netplay.
        // Get function pointer dynamically since mupen64plus is loaded at runtime.
        typedef void (*set_pif_sync_callback_t)(pif_sync_callback_t);
        void* coreHandle = m64p::Core.GetHandle();
        if (coreHandle)
        {
#ifdef _WIN32
            set_pif_sync_callback_t set_callback =
                (set_pif_sync_callback_t)GetProcAddress((HMODULE)coreHandle, "set_pif_sync_callback");
#else
            set_pif_sync_callback_t set_callback =
                (set_pif_sync_callback_t)dlsym(coreHandle, "set_pif_sync_callback");
#endif
            if (set_callback)
            {
#ifdef NETPLAY
                if (address == "KAILLERA")
                    set_callback(KailleraPifSyncCallback);
                else
#endif
#ifdef SCRIPTING_ENABLED
                    set_callback(ScriptingPifCallback);
#else
                    set_callback(nullptr);
#endif
            }
        }
#endif

        CoreRollbackSetVerboseStats(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
        setRollbackLoggingEnvironment();

        if (rollbackExecute)
        {
            m64p_ret = rmgk_gekko::execute() ? M64ERR_SUCCESS : M64ERR_SYSTEM_FAIL;
        }
        else
        {
            m64p_ret = m64p::Core.DoCommand(M64CMD_EXECUTE, 0, nullptr);
        }
        if (m64p_ret != M64ERR_SUCCESS)
        {
            error = rollbackExecute ?
                "CoreStartEmulation rollback execute Failed: " :
                "CoreStartEmulation m64p::Core.DoCommand(M64CMD_EXECUTE) Failed: ";
            if (!CoreGetError().empty())
            {
                error += CoreGetError();
            }
            else
            {
                error += m64p::Core.ErrorMessage(m64p_ret);
            }
        }
    }

    if (!netplay && rollbackExecute)
    {
        rmgk_gekko::close_session();
    }

#ifdef NETPLAY
    if (netplay && netplay_ret)
    {
        // Check if we used Kaillera or legacy netplay
        if (address == "KAILLERA")
        {
            // Don't shutdown Kaillera here - keep connection alive for restart
            // Kaillera will be shutdown when user leaves the server dialog
        }
        else if (address.rfind("GEKKO|", 0) == 0)
        {
            rmgk_gekko::close_session();
        }
        else
        {
            CoreShutdownNetplay();
        }
    }
#endif // NETPLAY

    CoreClearCheats();
    CoreDetachPlugins();
    CoreCloseRom();

    // restore plugin settings
    CoreApplyPluginSettings();

    // reset media loader state
    CoreResetMediaLoader();

    if (!netplay || netplay_ret)
    {
        // we need to set the emulation error last,
        // to prevent the other functions from
        // overriding the emulation error
        CoreSetError(error);
    }

    return m64p_ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

#ifdef NETPLAY
    rmgk_gekko::request_stop();
#endif

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STOP, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreStopEmulation m64p::Core.DoCommand(M64CMD_STOP) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

#ifdef NETPLAY
    // Clear Kaillera player number when stopping
    CoreSetKailleraPlayerNumber(0);
#endif

#ifdef SCRIPTING_ENABLED
    ScriptManager::GetInstance().OnEmulationStop();
#endif

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CorePauseEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CorePauseEmulation Failed: ";
        error += "cannot pause emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_PAUSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CorePauseEmulation m64p::Core.DoCommand(M64CMD_PAUSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

#ifdef SCRIPTING_ENABLED
    if (ret == M64ERR_SUCCESS)
    {
        ScriptManager::GetInstance().OnEmulationPause();
    }
#endif

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResumeEmulation(void)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationPaused())
    {
        error = "CoreIsEmulationPaused Failed: ";
        error += "cannot resume emulation when emulation isn't paused!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESUME, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResumeEmulation m64p::Core.DoCommand(M64CMD_RESUME) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

#ifdef SCRIPTING_ENABLED
    if (ret == M64ERR_SUCCESS)
    {
        ScriptManager::GetInstance().OnEmulationResume();
    }
#endif

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreAdvanceFrame(void)
{
    return CoreAdvanceFrames(1);
}

CORE_EXPORT bool CoreAdvanceFrames(int frames)
{
    return CoreRunFrames(frames, CoreFrameOutput_All);
}

CORE_EXPORT bool CoreRunFrames(int frames, int flags)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationRunning() && !CoreIsEmulationPaused())
    {
        return false;
    }

    if (frames < 1)
    {
        frames = 1;
    }

    ret = m64p::Core.DoCommand(M64CMD_RUN_FRAMES, frames, &flags);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreRunFrames DoCommand(M64CMD_RUN_FRAMES) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreSetFrameOutput(int flags)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_FRAME_OUTPUT_SET, flags, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSetFrameOutput DoCommand(M64CMD_FRAME_OUTPUT_SET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreResetEmulation(bool hard)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsEmulationPaused())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when paused!";
        CoreSetError(error);
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESET, hard, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResetEmulation m64p::Core.DoCommand(M64CMD_RESET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreIsEmulationRunning(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_RUNNING;
}

CORE_EXPORT bool CoreIsEmulationPaused(void)
{
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_PAUSED;
}

CORE_EXPORT bool CoreIsSynchronizedNetplayActive(void)
{
    if (CoreHasInitNetplay())
    {
        return true;
    }

    if (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode())
    {
        return true;
    }

#ifdef NETPLAY
    if (rmgk_gekko::is_netplay_session_active())
    {
        return true;
    }
#endif

    return false;
}

CORE_EXPORT int CoreGetCurrentFrameCount(void)
{
    // Return frame counter updated via frame callback
    return s_CurrentFrame;
}

CORE_EXPORT bool CoreGetControllerState(int port, bool& connected, bool& valid, uint8_t (&rx)[4])
{
    if (port < 0 || port >= PIF_CONTROLLER_CHANNELS_COUNT)
        return false;
    const PifControllerSnapshot& snap = s_ControllerSnapshot[port];
    connected = snap.connected;
    valid     = snap.valid;
    if (snap.valid) {
        rx[0] = snap.data[0];
        rx[1] = snap.data[1];
        rx[2] = snap.data[2];
        rx[3] = snap.data[3];
    }
    return true;
}
