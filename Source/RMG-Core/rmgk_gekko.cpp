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
#include "rmgk_gekko.hpp"

#include "Directories.hpp"
#include "Error.hpp"
#include "Library.hpp"
#include "Settings.hpp"

#ifdef RMGK_HAVE_GEKKONET
#include <gekkonet.h>
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "core/p2p_core.h"
#endif
#endif

// Recording hooks live in n02. The rollback flow needs to append per-frame
// synced inputs to the open .krec because it bypasses n02's normal frame loop.
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr unsigned int kGekkoStateCapacity = 24u * 1024u * 1024u;
constexpr int kGekkoMaxLoggedFrames = 600;
constexpr int kGekkoWaitSleepUs = 100;
// Slippi-style asymmetric time-sync (see project-slippi Ishiiruka,
// EXI_DeviceSlippi.cpp shouldAdvanceOnlineFrame). The behind player speeds up
// at twice the authority the ahead player slows down, and the deadzone is
// biased so each client happily sits slightly ahead. This keeps the ahead
// player close to full speed while still shrinking its speculative window, so
// it sees fewer rollback "teleports" of the remote character.
//
// Slippi works in microseconds of clock offset; we work in gekko_frames_ahead()
// (signed frames, +ve = local ahead), so the windows/deadzones below are the
// frame-unit equivalents of Slippi's 8000us / -250us deadzone and 3-frame ramp.
constexpr float  kGekkoTimesyncAheadDeadzone  = 0.48f;  // tolerate being ahead by ~half a frame
constexpr float  kGekkoTimesyncBehindDeadzone = 0.015f; // but correct almost immediately when behind
constexpr double kGekkoTimesyncSpeedUpWindow  = 3.0;    // frames behind to reach full speed-up
constexpr double kGekkoTimesyncSlowDownWindow = 3.0;    // frames ahead to reach full slow-down
constexpr double kGekkoTimesyncMaxSpeedUp     = 0.01;   // behind: up to +1.0% (scale -> 1.01)
constexpr double kGekkoTimesyncMaxSlowDown    = 0.005;  // ahead:  up to -0.5% (scale -> 0.995)
constexpr double kGekkoTimesyncLerp = 0.15;
// Sample gekko_frames_ahead() this often when recomputing the target
// emulation speed. Mirrors Slippi's SLIPPI_ONLINE_LOCKSTEP_INTERVAL (30
// frames @ 60Hz = once per ~500ms) — single-frame jitter spikes no
// longer kick the speed scale around; the lerp keeps speed_scale
// converging toward the cached target on every frame in between.
constexpr int kGekkoTimesyncIntervalFrames = 30;
constexpr size_t kGekkoClientReplayFrames = 600;

#ifdef RMGK_HAVE_GEKKONET
enum class ClientInputReplayMode
{
    Off,
    Recording,
    Playing
};

struct PendingGekkoSave
{
    int frame = 0;
    unsigned int* checksum = nullptr;
    unsigned int* stateLen = nullptr;
    unsigned char* state = nullptr;
};

GekkoSession* g_GekkoSession = nullptr;
int g_GekkoPlayers = 0;
int g_GekkoInputSize = 0;
int g_GekkoLocalPlayer = 0;
int g_GekkoLocalHandle = -1;
int g_GekkoRemoteHandle = -1;
std::vector<int> g_GekkoPlayerHandles;
std::vector<int> g_GekkoLocalHandles;
std::vector<unsigned char> g_GekkoLatchedInput;
bool g_GekkoHasLatchedInput = false;
// Frame number / flags associated with the most recent latch_gekko_input
// call. Used by synchronize_input to push the right key into the recording
// buffer once a PIF controller-read actually consumes the latched input.
int g_GekkoLatchedFrame = -1;
bool g_GekkoLatchedRunningAhead = false;
// Per-frame input buffer for rollback-aware krec recording. Pushed by
// synchronize_input (i.e. only when the game actually polled the controller
// that frame) and held until each frame ages past the rollback window, so
// rolling-back re-sims can overwrite the initial speculative entry with the
// corrected input before it gets committed to the .krec file.
std::map<int, std::vector<unsigned char>> g_GekkoFrameInputBuffer;
int g_GekkoMaxObservedFrame = -1;
constexpr int kGekkoRecordingRollbackHorizon = 32;
std::atomic_bool g_GekkoExecuting{false};
std::atomic_bool g_GekkoStopRequested{false};
std::vector<PendingGekkoSave> g_GekkoPendingSaves;
std::mutex g_GekkoLogMutex;
std::filesystem::path g_GekkoLogDirectory;
std::string g_GekkoLogPrefix;
int g_GekkoLogFrames = 0;
uint32_t g_GekkoLastSubmittedInput = 0xffffffffu;
std::vector<unsigned char> g_GekkoLastLatchedInput;
int g_GekkoWaitingLoops = 0;
int g_GekkoLocalInputLogRepeats = 0;
int g_GekkoPacingLogFrames = 0;
double g_GekkoSpeedScale = 1.0;
// Timesync sample state. Counter wraps every kGekkoTimesyncIntervalFrames
// to trigger a fresh frames_ahead sample. TargetScale is what g_GekkoSpeedScale
// lerps toward between samples.
int g_GekkoTimesyncSampleCounter = 0;
double g_GekkoTimesyncTargetScale = 1.0;
bool g_GekkoLogEnabled = false;
std::mutex g_GekkoClientReplayMutex;
ClientInputReplayMode g_GekkoClientReplayMode = ClientInputReplayMode::Off;
std::vector<uint32_t> g_GekkoClientReplayInputs;
size_t g_GekkoClientReplayIndex = 0;
#ifdef RMGK_HAVE_P2P_TRANSPORT
std::vector<GekkoNetResult*> g_GekkoP2PAdapterResults;
std::string g_GekkoP2PRemoteAddress;
#endif
long long g_GekkoLastLoadStateUs = 0;
long long g_GekkoLastSaveStateUs = 0;
long long g_GekkoLastRunFrameUs = 0;
long long g_GekkoLastPendingSaveUs = 0;
rmgk_gekko::InputProvider g_GekkoDebugInputProvider = nullptr;
rmgk_gekko::FrameCallback g_GekkoDebugBeginFrame = nullptr;
rmgk_gekko::FrameCallback g_GekkoDebugEndFrame = nullptr;
void* g_GekkoDebugUserData = nullptr;
int g_GekkoDebugFrameOutput = -1;
#endif

int rmgk_gekko_core_input_callback(void* values, int size, int players)
{
    return rmgk_gekko::synchronize_input(values, size, players) ? 1 : 0;
}

#ifdef RMGK_HAVE_GEKKONET
std::string hex_input(uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

void set_environment_value(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string make_rollback_log_prefix()
{
    static unsigned int sessionCounter = 0;

    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
        std::chrono::seconds(1);
    std::tm localTime = {};

#ifdef _WIN32
    localtime_s(&localTime, &nowTime);
#else
    localtime_r(&nowTime, &localTime);
#endif

    std::ostringstream stream;
    stream << "rollback_"
           << std::put_time(&localTime, "%Y%m%d_%H%M%S")
           << "_"
           << std::setw(3) << std::setfill('0') << nowMs.count()
           << "_"
           << ++sessionCounter;
    return stream.str();
}

std::filesystem::path create_rollback_log_directory()
{
    std::error_code errorCode;
    std::filesystem::path directory = CoreGetLibraryDirectory() / "Logs";

    if (std::filesystem::is_directory(directory, errorCode) ||
        std::filesystem::create_directories(directory, errorCode))
    {
        return directory.make_preferred();
    }

    errorCode.clear();
    directory = "Logs";
    if (std::filesystem::is_directory(directory, errorCode) ||
        std::filesystem::create_directories(directory, errorCode))
    {
        return directory.make_preferred();
    }

    return std::filesystem::path();
}

void reset_rollback_log_session()
{
    g_GekkoLogDirectory = create_rollback_log_directory();
    g_GekkoLogPrefix = make_rollback_log_prefix();

    set_environment_value("RMGK_ROLLBACK_LOG_DIR", g_GekkoLogDirectory.string());
    set_environment_value("RMGK_ROLLBACK_LOG_PREFIX", g_GekkoLogPrefix);
}

std::filesystem::path get_gekko_log_path()
{
    if (g_GekkoLogPrefix.empty())
    {
        reset_rollback_log_session();
    }

    std::string filename = g_GekkoLogPrefix;
    filename += g_GekkoLocalPlayer == 2 ? "_gekko_client.log" : "_gekko_host.log";

    if (!g_GekkoLogDirectory.empty())
    {
        return g_GekkoLogDirectory / filename;
    }

    return std::filesystem::path(filename);
}

void reset_gekko_log()
{
    reset_rollback_log_session();

    const char* logEnv = std::getenv("RMGK_GEKKO_LOG");
    g_GekkoLogEnabled = CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats) ||
        (logEnv != nullptr && std::strcmp(logEnv, "0") != 0);
    if (!g_GekkoLogEnabled)
    {
        g_GekkoLogFrames = 0;
        g_GekkoLastSubmittedInput = 0xffffffffu;
        g_GekkoLastLatchedInput.clear();
        g_GekkoWaitingLoops = 0;
        g_GekkoLocalInputLogRepeats = 0;
        g_GekkoPacingLogFrames = 0;
        g_GekkoSpeedScale = 1.0;
        g_GekkoTimesyncSampleCounter = 0;
        g_GekkoTimesyncTargetScale = 1.0;
        g_GekkoLastLoadStateUs = 0;
        g_GekkoLastSaveStateUs = 0;
        g_GekkoLastRunFrameUs = 0;
        g_GekkoLastPendingSaveUs = 0;
        return;
    }

    std::lock_guard<std::mutex> lock(g_GekkoLogMutex);
    const std::filesystem::path path = get_gekko_log_path();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    file << "RMG-K GekkoNet input log\n";
    g_GekkoLogFrames = 0;
    g_GekkoLastSubmittedInput = 0xffffffffu;
    g_GekkoLastLatchedInput.clear();
    g_GekkoWaitingLoops = 0;
    g_GekkoLocalInputLogRepeats = 0;
    g_GekkoPacingLogFrames = 0;
    g_GekkoSpeedScale = 1.0;
    g_GekkoTimesyncSampleCounter = 0;
    g_GekkoTimesyncTargetScale = 1.0;
    g_GekkoLastLoadStateUs = 0;
    g_GekkoLastSaveStateUs = 0;
    g_GekkoLastRunFrameUs = 0;
    g_GekkoLastPendingSaveUs = 0;
}

void write_gekko_log(const std::string& message)
{
    if (!g_GekkoLogEnabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_GekkoLogMutex);
    const std::filesystem::path path = get_gekko_log_path();
    std::ofstream file(path, std::ios::out | std::ios::app);
    file << "core_frame=" << CoreGetCurrentFrameCount() << " " << message << "\n";
}

#ifdef RMGK_HAVE_P2P_TRANSPORT
void p2p_adapter_send(GekkoNetAddress* addr, const char* data, int length)
{
    (void)addr;
    if (!p2p_rollback_transport_send(data, length) && g_GekkoLogEnabled)
    {
        write_gekko_log("p2p_adapter_send result=fail");
    }
}

GekkoNetResult** p2p_adapter_receive(int* length)
{
    g_GekkoP2PAdapterResults.clear();
    if (length == nullptr)
    {
        return g_GekkoP2PAdapterResults.data();
    }

    for (;;)
    {
        char data[2048];
        char addr[128];
        const int dataLen = p2p_rollback_transport_receive(data, static_cast<int>(sizeof(data)), addr, static_cast<int>(sizeof(addr)));
        if (dataLen <= 0)
        {
            break;
        }

        const char* resultAddr = g_GekkoP2PRemoteAddress.empty() ? addr : g_GekkoP2PRemoteAddress.c_str();
        const size_t addrLen = std::strlen(resultAddr);
        if (addrLen == 0)
        {
            break;
        }

        GekkoNetResult* result = reinterpret_cast<GekkoNetResult*>(std::malloc(sizeof(*result)));
        if (result == nullptr)
        {
            break;
        }

        result->addr.data = std::malloc(addrLen);
        result->data = std::malloc(static_cast<size_t>(dataLen));
        if (result->addr.data == nullptr || result->data == nullptr)
        {
            std::free(result->addr.data);
            std::free(result->data);
            std::free(result);
            break;
        }

        result->addr.size = static_cast<unsigned int>(addrLen);
        std::memcpy(result->addr.data, resultAddr, addrLen);
        result->data_len = static_cast<unsigned int>(dataLen);
        std::memcpy(result->data, data, static_cast<size_t>(dataLen));
        g_GekkoP2PAdapterResults.push_back(result);
    }

    *length = static_cast<int>(g_GekkoP2PAdapterResults.size());
    return g_GekkoP2PAdapterResults.data();
}

void p2p_adapter_free(void* data)
{
    std::free(data);
}

GekkoNetAdapter g_GekkoP2PAdapter{
    p2p_adapter_send,
    p2p_adapter_receive,
    p2p_adapter_free
};
#endif

const char* gekko_session_event_name(GekkoSessionEventType type)
{
    switch (type)
    {
    case GekkoPlayerSyncing: return "player_syncing";
    case GekkoPlayerConnected: return "player_connected";
    case GekkoPlayerDisconnected: return "player_disconnected";
    case GekkoSessionStarted: return "session_started";
    case GekkoSpectatorPaused: return "spectator_paused";
    case GekkoSpectatorUnpaused: return "spectator_unpaused";
    case GekkoDesyncDetected: return "desync_detected";
    default: return "unknown";
    }
}

const char* gekko_game_event_name(GekkoGameEventType type)
{
    switch (type)
    {
    case GekkoAdvanceEvent: return "advance";
    case GekkoSaveEvent: return "save";
    case GekkoLoadEvent: return "load";
    default: return "unknown";
    }
}

void log_session_events()
{
    if (!g_GekkoLogEnabled)
    {
        return;
    }

    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(g_GekkoSession, &count);
    for (int i = 0; i < count; i++)
    {
        GekkoSessionEvent* event = events[i];
        if (event == nullptr)
        {
            continue;
        }

        std::ostringstream stream;
        stream << "event name=" << gekko_session_event_name(event->type);
        switch (event->type)
        {
        case GekkoPlayerSyncing:
            stream << " handle=" << event->data.syncing.handle
                   << " count=" << static_cast<int>(event->data.syncing.current)
                   << " total=" << static_cast<int>(event->data.syncing.max);
            break;
        case GekkoPlayerConnected:
            stream << " handle=" << event->data.connected.handle;
            break;
        case GekkoPlayerDisconnected:
            stream << " handle=" << event->data.disconnected.handle;
            break;
        case GekkoDesyncDetected:
            stream << " frame=" << event->data.desynced.frame
                   << " remote_handle=" << event->data.desynced.remote_handle
                   << " local_checksum=" << event->data.desynced.local_checksum
                   << " remote_checksum=" << event->data.desynced.remote_checksum;
            break;
        default:
            break;
        }
        write_gekko_log(stream.str());
    }
}

bool save_gekko_state(const PendingGekkoSave& save)
{
    const auto beginTime = std::chrono::steady_clock::now();
    CoreRollbackState state;
    const int coreFrame = std::max(0, save.frame);
    if (g_GekkoLogEnabled)
    {
        std::ostringstream stream;
        stream << "save_state begin frame=" << save.frame
               << " core_frame=" << coreFrame
               << " state_ptr=" << static_cast<void*>(save.state)
               << " state_len_ptr=" << static_cast<void*>(save.stateLen)
               << " checksum_ptr=" << static_cast<void*>(save.checksum);
        write_gekko_log(stream.str());
    }

    if (save.state == nullptr || save.stateLen == nullptr)
    {
        write_gekko_log("save_state result=fail reason=null_event_buffer");
        CoreSetError("GekkoNet save event did not provide a state buffer");
        return false;
    }

    if (save.frame < 0)
    {
        *save.stateLen = 0;
        if (save.checksum != nullptr)
        {
            *save.checksum = 0;
        }
        write_gekko_log("save_state result=skipped reason=pre_frame_baseline");
        return true;
    }

    if (!CoreRollbackSaveGameStateInto(state, save.state, static_cast<int>(kGekkoStateCapacity), coreFrame))
    {
        g_GekkoLastSaveStateUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - beginTime).count();
        std::ostringstream stream;
        stream << "save_state result=fail elapsed_us=" << g_GekkoLastSaveStateUs;
        write_gekko_log(stream.str());
        return false;
    }

    if (state.len < 1 || static_cast<unsigned int>(state.len) > kGekkoStateCapacity)
    {
        std::ostringstream stream;
        stream << "save_state result=fail reason=state_too_large len=" << state.len
               << " capacity=" << kGekkoStateCapacity;
        write_gekko_log(stream.str());
        CoreSetError("GekkoNet rollback state exceeded configured state buffer");
        return false;
    }

    if (state.buffer != save.state)
    {
        write_gekko_log("save_state result=fail reason=state_not_written_in_place");
        CoreSetError("GekkoNet rollback state was not saved into the provided state buffer");
        return false;
    }

    if (save.stateLen != nullptr)
    {
        *save.stateLen = static_cast<unsigned int>(state.len);
    }
    if (save.checksum != nullptr)
    {
        *save.checksum = static_cast<unsigned int>(state.checksum);
    }

    g_GekkoLastSaveStateUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - beginTime).count();

    if (g_GekkoLogEnabled && (g_GekkoLogFrames < kGekkoMaxLoggedFrames || g_GekkoLastSaveStateUs >= 2000))
    {
        std::ostringstream stream;
        stream << "save_state result=ok frame=" << save.frame
               << " len=" << state.len
               << " checksum=" << static_cast<unsigned int>(state.checksum)
               << " elapsed_us=" << g_GekkoLastSaveStateUs;
        write_gekko_log(stream.str());
    }

    return true;
}

bool load_gekko_state(const GekkoGameEvent* event)
{
    const auto beginTime = std::chrono::steady_clock::now();
    CoreRollbackState state;
    state.buffer = event->data.load.state;
    state.len = static_cast<int>(event->data.load.state_len);
    state.frame = event->data.load.frame;
    if (!CoreRollbackLoadGameState(state))
    {
        g_GekkoLastLoadStateUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - beginTime).count();
        std::ostringstream stream;
        stream << "load_state result=fail elapsed_us=" << g_GekkoLastLoadStateUs;
        write_gekko_log(stream.str());
        return false;
    }
    g_GekkoLastLoadStateUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - beginTime).count();

    if (g_GekkoLogEnabled && (g_GekkoLogFrames < kGekkoMaxLoggedFrames || g_GekkoLastLoadStateUs >= 2000))
    {
        std::ostringstream stream;
        stream << "load_state result=ok frame=" << event->data.load.frame
               << " len=" << event->data.load.state_len
               << " elapsed_us=" << g_GekkoLastLoadStateUs;
        write_gekko_log(stream.str());
    }
    return true;
}

bool submit_local_input()
{
    const bool localOnlySession = g_GekkoRemoteHandle < 0 && g_GekkoLocalHandles.size() > 1;
    const int samplePlayers = localOnlySession ? g_GekkoPlayers : 1;
    std::vector<uint32_t> physicalInputs(static_cast<size_t>(std::max(samplePlayers, 1)), 0);
    if (!CoreRollbackSampleInput(physicalInputs.data(), g_GekkoInputSize, samplePlayers))
    {
        write_gekko_log("add_local_input sample=fail");
        return false;
    }

    bool submitted = false;

    for (int player = 1; player <= g_GekkoPlayers; player++)
    {
        const size_t playerIndex = static_cast<size_t>(player - 1);
        const int handle = playerIndex < g_GekkoLocalHandles.size() ? g_GekkoLocalHandles[playerIndex] : -1;
        if (handle < 0)
        {
            continue;
        }

        uint32_t input = localOnlySession ? physicalInputs[playerIndex] : physicalInputs[0];
        {
            std::lock_guard<std::mutex> lock(g_GekkoClientReplayMutex);
            if (!localOnlySession && g_GekkoLocalPlayer == 2 && g_GekkoClientReplayMode == ClientInputReplayMode::Recording)
            {
                g_GekkoClientReplayInputs.push_back(input);
                if (g_GekkoClientReplayInputs.size() >= kGekkoClientReplayFrames)
                {
                    g_GekkoClientReplayMode = ClientInputReplayMode::Playing;
                    g_GekkoClientReplayIndex = 0;
                    write_gekko_log("client_input_replay mode=playing recorded_frames=600");
                }
            }
            else if (!localOnlySession && g_GekkoLocalPlayer == 2 && g_GekkoClientReplayMode == ClientInputReplayMode::Playing &&
                !g_GekkoClientReplayInputs.empty())
            {
                input = g_GekkoClientReplayInputs[g_GekkoClientReplayIndex++];
                if (g_GekkoClientReplayIndex >= g_GekkoClientReplayInputs.size())
                {
                    g_GekkoClientReplayIndex = 0;
                }
            }
        }

        gekko_add_local_input(g_GekkoSession, handle, &input);
        submitted = true;

        if (player != g_GekkoLocalPlayer)
        {
            continue;
        }

        const bool changed = input != g_GekkoLastSubmittedInput;
        if (changed)
        {
            g_GekkoLocalInputLogRepeats = 0;
        }
        else
        {
            g_GekkoLocalInputLogRepeats++;
        }

        if (g_GekkoLogEnabled &&
            (changed || g_GekkoLocalInputLogRepeats <= 20 || (g_GekkoLocalInputLogRepeats % 60) == 0))
        {
            std::ostringstream stream;
            stream << "add_local_input local_player=" << player
                   << " handle=" << handle
                   << " physical_p" << (localOnlySession ? player : 1) << "=" << hex_input(input)
                   << " repeat=" << g_GekkoLocalInputLogRepeats;
            write_gekko_log(stream.str());
        }
        g_GekkoLastSubmittedInput = input;
    }

    return submitted;
}

bool latch_gekko_input(const GekkoGameEvent* event)
{
    const int expectedBytes = g_GekkoPlayers * g_GekkoInputSize;
    if (event->data.adv.inputs == nullptr || static_cast<int>(event->data.adv.input_len) < expectedBytes)
    {
        write_gekko_log("sync_input result=fail reason=shape");
        return false;
    }

    if (static_cast<int>(g_GekkoLatchedInput.size()) != expectedBytes)
    {
        g_GekkoLatchedInput.resize(static_cast<size_t>(expectedBytes));
    }
    std::memset(g_GekkoLatchedInput.data(), 0, static_cast<size_t>(expectedBytes));
    for (int player = 1; player <= g_GekkoPlayers; player++)
    {
        const size_t playerIndex = static_cast<size_t>(player - 1);
        const int handle = playerIndex < g_GekkoPlayerHandles.size() ? g_GekkoPlayerHandles[playerIndex] : -1;
        if (handle < 0 || handle >= g_GekkoPlayers)
        {
            write_gekko_log("sync_input result=fail reason=handle_map");
            return false;
        }

        std::memcpy(g_GekkoLatchedInput.data() + (playerIndex * static_cast<size_t>(g_GekkoInputSize)),
            event->data.adv.inputs + (handle * g_GekkoInputSize),
            static_cast<size_t>(g_GekkoInputSize));
    }
    g_GekkoHasLatchedInput = true;

    if (g_GekkoDebugInputProvider != nullptr)
    {
        std::vector<uint32_t> debugInputs(static_cast<size_t>(g_GekkoPlayers), 0);
        for (int player = 0; player < g_GekkoPlayers; player++)
        {
            std::memcpy(&debugInputs[static_cast<size_t>(player)],
                g_GekkoLatchedInput.data() + (player * g_GekkoInputSize),
                std::min(g_GekkoInputSize, static_cast<int>(sizeof(uint32_t))));
        }

        if (!g_GekkoDebugInputProvider(debugInputs.data(), g_GekkoPlayers, g_GekkoDebugUserData))
        {
            write_gekko_log("sync_input result=fail reason=debug_provider");
            return false;
        }

        std::memset(g_GekkoLatchedInput.data(), 0, static_cast<size_t>(expectedBytes));
        for (int player = 0; player < g_GekkoPlayers; player++)
        {
            std::memcpy(g_GekkoLatchedInput.data() + (player * g_GekkoInputSize),
                &debugInputs[static_cast<size_t>(player)],
                std::min(g_GekkoInputSize, static_cast<int>(sizeof(uint32_t))));
        }
    }

    // Remember which frame/flags this latched input belongs to so that
    // synchronize_input — invoked from the rollback PIF callback only when
    // the game actually issues a JCMD_CONTROLLER_READ that frame — can push
    // it to the per-frame recording buffer. Pushing here instead would record
    // one entry per gekko advance regardless of whether the emulator polled
    // the controller, which drifts against playback (PlaybackBuffer consumes
    // one entry per PIF poll) and silently desyncs the .krec.
    g_GekkoLatchedFrame = event->data.adv.frame;
    g_GekkoLatchedRunningAhead = event->data.adv.running_ahead;

    if (g_GekkoLogEnabled &&
        (g_GekkoLogFrames < kGekkoMaxLoggedFrames || g_GekkoLatchedInput != g_GekkoLastLatchedInput))
    {
        std::ostringstream stream;
        stream << "sync_input result=ok frame=" << event->data.adv.frame
               << " rolling_back=" << (event->data.adv.rolling_back ? "true" : "false")
               << " running_ahead=" << (event->data.adv.running_ahead ? "true" : "false");
        for (int player = 0; player < g_GekkoPlayers; player++)
        {
            uint32_t input = 0;
            std::memcpy(&input, g_GekkoLatchedInput.data() + (player * g_GekkoInputSize),
                std::min(g_GekkoInputSize, static_cast<int>(sizeof(input))));
            stream << " p" << (player + 1) << "=" << hex_input(input);
        }
        write_gekko_log(stream.str());
        g_GekkoLastLatchedInput = g_GekkoLatchedInput;
        g_GekkoLogFrames++;
    }
    return true;
}

bool process_pending_saves()
{
    const auto beginTime = std::chrono::steady_clock::now();
    const size_t pendingCount = g_GekkoPendingSaves.size();
    for (const auto& save : g_GekkoPendingSaves)
    {
        if (!save_gekko_state(save))
        {
            g_GekkoPendingSaves.clear();
            return false;
        }
    }
    g_GekkoPendingSaves.clear();
    g_GekkoLastPendingSaveUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - beginTime).count();
    if (g_GekkoLogEnabled && pendingCount > 0)
    {
        std::ostringstream stream;
        stream << "pending_saves result=ok count=" << pendingCount
               << " elapsed_us=" << g_GekkoLastPendingSaveUs
               << " last_save_us=" << g_GekkoLastSaveStateUs;
        write_gekko_log(stream.str());
    }
    return true;
}

void apply_gekko_frame_pacing()
{
    // Read frames_ahead every call (cheap, just a member access in
    // GekkoSession) but only recompute the target scale once per
    // sampling interval. The per-frame lerp below carries the speed scale
    // toward the cached target between samples.
    const float framesAhead = gekko_frames_ahead(g_GekkoSession);
    const bool isSampleFrame =
        (g_GekkoTimesyncSampleCounter % kGekkoTimesyncIntervalFrames) == 0;
    if (isSampleFrame)
    {
        // Asymmetric correction: the behind player speeds up at twice the
        // authority the ahead player slows down, with a deadzone biased toward
        // sitting slightly ahead. Ramp linearly to the cap over the configured
        // frame window, matching Slippi's shouldAdvanceOnlineFrame.
        double deviation = 0.0;
        if (framesAhead < -kGekkoTimesyncBehindDeadzone)
        {
            const double multiplier =
                std::min(-static_cast<double>(framesAhead) / kGekkoTimesyncSpeedUpWindow, 1.0);
            deviation = multiplier * kGekkoTimesyncMaxSpeedUp;
        }
        else if (framesAhead > kGekkoTimesyncAheadDeadzone)
        {
            const double multiplier =
                std::min(static_cast<double>(framesAhead) / kGekkoTimesyncSlowDownWindow, 1.0);
            deviation = multiplier * -kGekkoTimesyncMaxSlowDown;
        }
        g_GekkoTimesyncTargetScale = 1.0 + deviation;
    }
    g_GekkoTimesyncSampleCounter++;

    g_GekkoSpeedScale += (g_GekkoTimesyncTargetScale - g_GekkoSpeedScale) * kGekkoTimesyncLerp;
    CoreRollbackSetTimesyncScale(g_GekkoSpeedScale);

    g_GekkoPacingLogFrames++;
    if (g_GekkoLogEnabled &&
        (g_GekkoTimesyncTargetScale != 1.0 || g_GekkoPacingLogFrames <= 10 || (g_GekkoPacingLogFrames % 60) == 0))
    {
        std::ostringstream stream;
        stream << "pacing frames_ahead=" << std::fixed << std::setprecision(2) << framesAhead
               << " sample=" << (isSampleFrame ? 1 : 0)
               << " target_scale=" << std::setprecision(4) << g_GekkoTimesyncTargetScale
               << " speed_scale=" << g_GekkoSpeedScale;

        if (g_GekkoRemoteHandle >= 0)
        {
            GekkoNetworkStats stats = {};
            gekko_network_stats(g_GekkoSession, g_GekkoRemoteHandle, &stats);
            stream << " remote_handle=" << g_GekkoRemoteHandle
                   << " ping_ms=" << stats.last_ping
                   << " avg_ping_ms=" << std::setprecision(1) << stats.avg_ping
                   << " jitter_ms=" << stats.jitter
                   << " kb_sent=" << stats.kb_sent
                   << " kb_recv=" << stats.kb_received;
        }

        write_gekko_log(stream.str());
    }
}

int rollback_execute_begin_frame(void* userData)
{
    (void)userData;
    const auto beginTime = std::chrono::steady_clock::now();
    int summaryEventCount = 0;
    int summarySaveCount = 0;
    int summaryLoadCount = 0;
    int summaryRollbackAdvanceCount = 0;
    int summaryRunaheadAdvanceCount = 0;
    int summaryWaitLoops = 0;
    long long summaryNetworkPollUs = 0;
    long long summaryPacingUs = 0;
    long long summarySubmitInputUs = 0;
    long long summaryUpdateSessionUs = 0;
    long long summaryLatchInputUs = 0;
    long long summarySaveUs = 0;
    long long summaryLoadUs = 0;
    long long summaryResimUs = 0;
    long long summaryMaxResimUs = 0;
    long long summaryDebugBeginUs = 0;

    if (g_GekkoSession == nullptr)
    {
        return 0;
    }
    if (g_GekkoStopRequested.load(std::memory_order_relaxed))
    {
        write_gekko_log("begin_frame result=stop_requested");
        return 0;
    }
    if (g_GekkoDebugBeginFrame != nullptr)
    {
        const auto debugBeginTime = std::chrono::steady_clock::now();
        if (!g_GekkoDebugBeginFrame(g_GekkoDebugUserData))
        {
            write_gekko_log("begin_frame result=fail reason=debug_hook");
            return 0;
        }
        summaryDebugBeginUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - debugBeginTime).count();
    }

    g_GekkoHasLatchedInput = false;
    g_GekkoPendingSaves.clear();

    const auto networkPollTime = std::chrono::steady_clock::now();
    gekko_network_poll(g_GekkoSession);
    summaryNetworkPollUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - networkPollTime).count();

    const auto pacingTime = std::chrono::steady_clock::now();
    apply_gekko_frame_pacing();
    summaryPacingUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pacingTime).count();

    const auto submitInputTime = std::chrono::steady_clock::now();
    if (!submit_local_input())
    {
        return 0;
    }
    summarySubmitInputUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - submitInputTime).count();

    for (;;)
    {
        if (g_GekkoStopRequested.load(std::memory_order_relaxed))
        {
            write_gekko_log("begin_frame result=stop_requested");
            return 0;
        }

        int count = 0;
        const auto updateSessionTime = std::chrono::steady_clock::now();
        GekkoGameEvent** events = gekko_update_session(g_GekkoSession, &count);
        summaryUpdateSessionUs +=
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - updateSessionTime).count();
        log_session_events();

        if (count == 0)
        {
            g_GekkoWaitingLoops++;
            summaryWaitLoops++;
            if (g_GekkoLogEnabled &&
                (g_GekkoWaitingLoops <= 20 || (g_GekkoWaitingLoops % 60) == 0))
            {
                std::ostringstream stream;
                stream << "update_session result=waiting loop=" << g_GekkoWaitingLoops
                       << " events=0";
                write_gekko_log(stream.str());
            }
        }
        else if (g_GekkoLogEnabled)
        {
            std::ostringstream stream;
            stream << "update_session result=events count=" << count;
            for (int i = 0; i < count; i++)
            {
                if (events[i] != nullptr)
                {
                    stream << " event" << i << "=" << gekko_game_event_name(events[i]->type);
                    if (events[i]->type == GekkoAdvanceEvent)
                    {
                        stream << "(frame=" << events[i]->data.adv.frame
                               << ",rollback=" << (events[i]->data.adv.rolling_back ? "true" : "false")
                               << ",runahead=" << (events[i]->data.adv.running_ahead ? "true" : "false")
                               << ")";
                    }
                    else if (events[i]->type == GekkoSaveEvent)
                    {
                        stream << "(frame=" << events[i]->data.save.frame << ")";
                    }
                    else if (events[i]->type == GekkoLoadEvent)
                    {
                        stream << "(frame=" << events[i]->data.load.frame
                               << ",len=" << events[i]->data.load.state_len << ")";
                    }
                }
            }
            write_gekko_log(stream.str());
            g_GekkoWaitingLoops = 0;
        }
        else
        {
            g_GekkoWaitingLoops = 0;
        }

        bool deferSavesUntilFrameEnd = false;
        bool hasRealAdvance = false;
        summaryEventCount += count;
        for (int i = 0; i < count; i++)
        {
            GekkoGameEvent* event = events[i];
            if (event == nullptr)
            {
                continue;
            }

            switch (event->type)
            {
            case GekkoSaveEvent:
            {
                summarySaveCount++;
                PendingGekkoSave save;
                save.frame = event->data.save.frame;
                save.checksum = event->data.save.checksum;
                save.stateLen = event->data.save.state_len;
                save.state = event->data.save.state;
                if (deferSavesUntilFrameEnd)
                {
                    write_gekko_log("save_state result=deferred");
                    g_GekkoPendingSaves.push_back(save);
                }
                else if (!save_gekko_state(save))
                {
                    return 0;
                }
                else
                {
                    summarySaveUs += g_GekkoLastSaveStateUs;
                }
                break;
            }
            case GekkoLoadEvent:
                summaryLoadCount++;
                write_gekko_log("load_state begin");
                if (!load_gekko_state(event))
                {
                    return 0;
                }
                summaryLoadUs += g_GekkoLastLoadStateUs;
                break;
            case GekkoAdvanceEvent:
                write_gekko_log("advance_frame begin");
            {
                const auto latchInputTime = std::chrono::steady_clock::now();
                if (!latch_gekko_input(event))
                {
                    return 0;
                }
                summaryLatchInputUs +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - latchInputTime).count();

                if (event->data.adv.rolling_back || event->data.adv.running_ahead)
                {
                    if (event->data.adv.rolling_back)
                    {
                        summaryRollbackAdvanceCount++;
                    }
                    if (event->data.adv.running_ahead)
                    {
                        summaryRunaheadAdvanceCount++;
                    }
                    const auto runFrameBeginTime = std::chrono::steady_clock::now();
                    if (!CoreRollbackRunFrame(CoreFrameOutput_None))
                    {
                        write_gekko_log("advance_frame result=fail reason=run_frame");
                        return 0;
                    }
                    g_GekkoLastRunFrameUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - runFrameBeginTime).count();
                    summaryResimUs += g_GekkoLastRunFrameUs;
                    summaryMaxResimUs = std::max(summaryMaxResimUs, g_GekkoLastRunFrameUs);
                    if (g_GekkoLogEnabled)
                    {
                        CoreRollbackRunFrameStats runFrameStats;
                        const bool hasRunFrameStats = CoreRollbackGetRunFrameStats(runFrameStats);
                        std::ostringstream stream;
                        stream << "advance_frame result=rollback_frame_ok elapsed_us=" << g_GekkoLastRunFrameUs
                               << " rolling_back=" << (event->data.adv.rolling_back ? "true" : "false")
                               << " running_ahead=" << (event->data.adv.running_ahead ? "true" : "false");
                        if (hasRunFrameStats)
                        {
                            stream << " core_total_us=" << runFrameStats.totalUs
                                   << " r4300_us=" << runFrameStats.r4300Us
                                   << " vi_us=" << runFrameStats.viUs
                                   << " new_frame_us=" << runFrameStats.newFrameUs
                                   << " cheats_us=" << runFrameStats.cheatsUs
                                   << " pacing_us=" << runFrameStats.pacingUs
                                   << " input_us=" << runFrameStats.inputUs
                                   << " pause_us=" << runFrameStats.pauseUs
                                   << " netplay_us=" << runFrameStats.netplayUs
                                   << " dynarec_recompiles=" << runFrameStats.dynarecRecompileCount
                                   << " dynarec_recompile_us=" << runFrameStats.dynarecRecompileUs
                                   << " dynarec_invalidate_us=" << runFrameStats.dynarecInvalidateUs
                                   << " dynarec_full_invalidates=" << runFrameStats.dynarecFullInvalidateCount
                                   << " dynarec_range_invalidates=" << runFrameStats.dynarecRangeInvalidateCount
                                   << " dynarec_block_invalidates=" << runFrameStats.dynarecBlockInvalidateCount
                                   << " dynarec_verify_dirty_count=" << runFrameStats.dynarecVerifyDirtyCount
                                   << " dynarec_verify_dirty_us=" << runFrameStats.dynarecVerifyDirtyUs
                                   << " dynarec_get_addr_count=" << runFrameStats.dynarecGetAddrCount
                                   << " dynarec_get_addr_us=" << runFrameStats.dynarecGetAddrUs
                                   << " dynarec_get_addr_ht_count=" << runFrameStats.dynarecGetAddrHtCount
                                   << " dynarec_get_addr_32_count=" << runFrameStats.dynarecGetAddr32Count
                                   << " dynarec_dynamic_linker_count=" << runFrameStats.dynarecDynamicLinkerCount
                                   << " dynarec_dynamic_linker_us=" << runFrameStats.dynarecDynamicLinkerUs
                                   << " dynarec_dynamic_linker_ds_count=" << runFrameStats.dynarecDynamicLinkerDsCount
                                   << " dynarec_dynamic_linker_ds_us=" << runFrameStats.dynarecDynamicLinkerDsUs
                                   << " cached_code_full_invalidates=" << runFrameStats.cachedCodeFullInvalidateCount
                                   << " cached_code_range_invalidates=" << runFrameStats.cachedCodeRangeInvalidateCount
                                   << " interrupt_count=" << runFrameStats.interruptCount
                                   << " interrupt_us=" << runFrameStats.interruptUs
                                   << " interrupt_max_us=" << runFrameStats.interruptMaxUs
                                   << " interrupt_max_type=" << runFrameStats.interruptMaxType
                                   << " interrupt_vi_count=" << runFrameStats.interruptViCount
                                   << " interrupt_vi_us=" << runFrameStats.interruptViUs
                                   << " interrupt_compare_count=" << runFrameStats.interruptCompareCount
                                   << " interrupt_compare_us=" << runFrameStats.interruptCompareUs
                                   << " interrupt_check_count=" << runFrameStats.interruptCheckCount
                                   << " interrupt_check_us=" << runFrameStats.interruptCheckUs
                                   << " interrupt_si_count=" << runFrameStats.interruptSiCount
                                   << " interrupt_si_us=" << runFrameStats.interruptSiUs
                                   << " interrupt_pi_count=" << runFrameStats.interruptPiCount
                                   << " interrupt_pi_us=" << runFrameStats.interruptPiUs
                                   << " interrupt_ai_count=" << runFrameStats.interruptAiCount
                                   << " interrupt_ai_us=" << runFrameStats.interruptAiUs
                                   << " interrupt_sp_count=" << runFrameStats.interruptSpCount
                                   << " interrupt_sp_us=" << runFrameStats.interruptSpUs
                                   << " interrupt_dp_count=" << runFrameStats.interruptDpCount
                                   << " interrupt_dp_us=" << runFrameStats.interruptDpUs
                                   << " interrupt_rsp_dma_count=" << runFrameStats.interruptRspDmaCount
                                   << " interrupt_rsp_dma_us=" << runFrameStats.interruptRspDmaUs
                                   << " interrupt_rsp_task_count=" << runFrameStats.interruptRspTaskCount
                                   << " interrupt_rsp_task_us=" << runFrameStats.interruptRspTaskUs
                                   << " ai_set_frequency_count=" << runFrameStats.aiSetFrequencyCount
                                   << " ai_set_frequency_us=" << runFrameStats.aiSetFrequencyUs
                                   << " ai_push_samples_count=" << runFrameStats.aiPushSamplesCount
                                   << " ai_push_samples_us=" << runFrameStats.aiPushSamplesUs
                                   << " ai_fifo_pop_count=" << runFrameStats.aiFifoPopCount
                                   << " ai_fifo_pop_us=" << runFrameStats.aiFifoPopUs
                                   << " ai_raise_interrupt_count=" << runFrameStats.aiRaiseInterruptCount
                                   << " ai_raise_interrupt_us=" << runFrameStats.aiRaiseInterruptUs
                                   << " emumode=" << runFrameStats.emumode
                                   << " cp0_count_before=" << runFrameStats.cp0CountBefore
                                   << " cp0_count_after=" << runFrameStats.cp0CountAfter
                                   << " cp0_count_delta=" << (runFrameStats.cp0CountAfter - runFrameStats.cp0CountBefore)
                                   << " next_interrupt_before=" << runFrameStats.nextInterruptBefore
                                   << " next_interrupt_after=" << runFrameStats.nextInterruptAfter
                                   << " pc_before=0x" << std::hex << runFrameStats.pcBefore
                                   << " pc_after=0x" << runFrameStats.pcAfter
                                   << " dynarec_pcaddr_before=0x" << runFrameStats.dynarecPcaddrBefore
                                   << " dynarec_pcaddr_after=0x" << runFrameStats.dynarecPcaddrAfter
                                   << " cp0_last_addr_before=0x" << runFrameStats.cp0LastAddrBefore
                                   << " cp0_last_addr_after=0x" << runFrameStats.cp0LastAddrAfter << std::dec
                                   << " dynarec_cycle_count_before=" << runFrameStats.dynarecCycleCountBefore
                                   << " dynarec_cycle_count_after=" << runFrameStats.dynarecCycleCountAfter
                                   << " dynarec_pending_exception_before=" << runFrameStats.dynarecPendingExceptionBefore
                                   << " dynarec_pending_exception_after=" << runFrameStats.dynarecPendingExceptionAfter
                                   << " dynarec_stop_before=" << runFrameStats.dynarecStopBefore
                                   << " dynarec_stop_after=" << runFrameStats.dynarecStopAfter
                                   << " delay_slot_before=" << runFrameStats.delaySlotBefore
                                   << " delay_slot_after=" << runFrameStats.delaySlotAfter
                                   << " current_frame_before=" << runFrameStats.currentFrameBefore
                                   << " current_frame_after=" << runFrameStats.currentFrameAfter
                                   << " output_flags=" << runFrameStats.outputFlags;
                        }
                        write_gekko_log(stream.str());
                    }
                    g_GekkoHasLatchedInput = false;
                }
                else
                {
                    write_gekko_log("advance_frame result=real_frame_ready");
                    hasRealAdvance = true;
                    deferSavesUntilFrameEnd = true;
                }
                break;
            }
            default:
                break;
            }
        }

        if (hasRealAdvance)
        {
            if (g_GekkoDebugFrameOutput >= 0)
            {
                CoreSetFrameOutput(g_GekkoDebugFrameOutput);
                if (g_GekkoLogEnabled && g_GekkoLogFrames < kGekkoMaxLoggedFrames)
                {
                    std::ostringstream stream;
                    stream << "debug_frame_output flags=" << g_GekkoDebugFrameOutput;
                    write_gekko_log(stream.str());
                }
            }
            if (g_GekkoLogEnabled)
            {
                const auto endTime = std::chrono::steady_clock::now();
                const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(endTime - beginTime).count();
                if (elapsedUs >= 2000 || summaryRollbackAdvanceCount > 0 || summaryLoadCount > 0 || summaryWaitLoops > 0)
                {
                    std::ostringstream stream;
                    stream << "frame_summary elapsed_us=" << elapsedUs
                           << " events=" << summaryEventCount
                           << " saves=" << summarySaveCount
                           << " loads=" << summaryLoadCount
                           << " rollback_advances=" << summaryRollbackAdvanceCount
                           << " runahead_advances=" << summaryRunaheadAdvanceCount
                           << " wait_loops=" << summaryWaitLoops
                           << " debug_begin_us=" << summaryDebugBeginUs
                           << " network_poll_us=" << summaryNetworkPollUs
                           << " pacing_us=" << summaryPacingUs
                           << " submit_input_us=" << summarySubmitInputUs
                           << " update_session_us=" << summaryUpdateSessionUs
                           << " latch_input_us=" << summaryLatchInputUs
                           << " save_total_us=" << summarySaveUs
                           << " load_total_us=" << summaryLoadUs
                           << " resim_total_us=" << summaryResimUs
                           << " resim_max_us=" << summaryMaxResimUs
                           << " last_load_us=" << g_GekkoLastLoadStateUs
                           << " last_save_us=" << g_GekkoLastSaveStateUs
                           << " last_run_frame_us=" << g_GekkoLastRunFrameUs
                           << " pending_save_us=" << g_GekkoLastPendingSaveUs
                           << " frames_ahead=" << std::fixed << std::setprecision(2)
                           << gekko_frames_ahead(g_GekkoSession);
                    write_gekko_log(stream.str());
                }
            }
            write_gekko_log("begin_frame result=real_frame");
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(kGekkoWaitSleepUs));
    }
}

int rollback_execute_end_frame(void* userData)
{
    (void)userData;
    const auto beginTime = std::chrono::steady_clock::now();
    write_gekko_log("end_frame begin");
    const auto pendingSaveBeginTime = std::chrono::steady_clock::now();
    if (!process_pending_saves())
    {
        write_gekko_log("end_frame result=fail reason=save");
        return 0;
    }
    const auto pendingSaveUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pendingSaveBeginTime).count();
    long long debugEndUs = 0;
    if (g_GekkoDebugEndFrame != nullptr)
    {
        const auto debugEndBeginTime = std::chrono::steady_clock::now();
        if (!g_GekkoDebugEndFrame(g_GekkoDebugUserData))
        {
            write_gekko_log("end_frame result=fail reason=debug_hook");
            return 0;
        }
        debugEndUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - debugEndBeginTime).count();
    }
    g_GekkoHasLatchedInput = false;
    if (g_GekkoLogEnabled)
    {
        const auto totalUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - beginTime).count();
        std::ostringstream stream;
        stream << "end_frame result=ok total_us=" << totalUs
               << " pending_save_us=" << pendingSaveUs
               << " debug_end_us=" << debugEndUs;
        write_gekko_log(stream.str());
    }
    else
    {
        write_gekko_log("end_frame result=ok");
    }
    return 1;
}
#endif
} // namespace

CORE_EXPORT bool rmgk_gekko::start_p2p_session(const char* gameName, int players, int inputSize,
    int localPlayer, unsigned short localPort, const char* remoteIp, unsigned short remotePort, int localDelay, int predictionWindow)
{
#ifndef RMGK_HAVE_GEKKONET
    (void)gameName;
    (void)players;
    (void)inputSize;
    (void)localPlayer;
    (void)localPort;
    (void)remoteIp;
    (void)remotePort;
    (void)localDelay;
    (void)predictionWindow;
    return false;
#else
    close_session();

    g_GekkoLocalPlayer = localPlayer;
    g_GekkoStopRequested.store(false, std::memory_order_relaxed);
    reset_gekko_log();

    if (gameName == nullptr || players < 2 || players > 4 || inputSize != static_cast<int>(sizeof(uint32_t)) ||
        localPlayer < 1 || localPlayer > players || remoteIp == nullptr || remoteIp[0] == '\0' || remotePort == 0)
    {
        write_gekko_log("start_p2p_session result=fail reason=invalid_params");
        return false;
    }

    if (!gekko_create(&g_GekkoSession, GekkoGameSession))
    {
        write_gekko_log("gekko_create result=fail");
        return false;
    }

    const int clampedLocalDelay = std::clamp(localDelay, 0, 10);
    const int clampedPredictionWindow = std::clamp(predictionWindow, 1, 10);

    GekkoConfig config = {};
    config.num_players = static_cast<unsigned char>(players);
    config.max_spectators = 0;
    config.input_prediction_window = static_cast<unsigned char>(clampedPredictionWindow);
    config.input_size = static_cast<unsigned int>(inputSize);
    config.state_size = kGekkoStateCapacity;
    config.limited_saving = false;
    config.desync_detection = true;
    config.check_distance = 10;
    gekko_start(g_GekkoSession, &config);
#ifdef RMGK_HAVE_P2P_TRANSPORT
    p2p_rollback_transport_clear();
    gekko_net_adapter_set(g_GekkoSession, &g_GekkoP2PAdapter);
#else
#ifdef _WIN32
    CoreSetError("GekkoNet P2P transport is unavailable in this build");
    close_session();
    return false;
#else
    try
    {
        gekko_net_adapter_set(g_GekkoSession, gekko_default_adapter(localPort));
    }
    catch (const std::exception& e)
    {
        std::ostringstream stream;
        stream << "start_p2p_session result=fail reason=adapter local_port=" << localPort
               << " error=" << e.what();
        write_gekko_log(stream.str());
        CoreSetError("GekkoNet adapter initialization failed: " + std::string(e.what()));
        close_session();
        return false;
    }
#endif
#endif
    gekko_set_runahead(g_GekkoSession, 0);

    g_GekkoPlayers = players;
    g_GekkoInputSize = inputSize;
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLocalHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLatchedInput.assign(static_cast<size_t>(players * inputSize), 0);
    g_GekkoHasLatchedInput = false;
    g_GekkoLatchedFrame = -1;
    g_GekkoLatchedRunningAhead = false;
    g_GekkoFrameInputBuffer.clear();
    g_GekkoMaxObservedFrame = -1;
    g_GekkoPendingSaves.clear();

    if (g_GekkoLogEnabled)
    {
        std::ostringstream stream;
        stream << "start_p2p_session game=" << gameName
               << " players=" << players
               << " input_size=" << inputSize
               << " local_player=" << localPlayer
               << " local_port=" << localPort
               << " remote_ip=" << remoteIp
               << " remote_port=" << remotePort
               << " local_delay=" << localDelay
               << " clamped_local_delay=" << clampedLocalDelay
               << " prediction_window=" << predictionWindow
               << " clamped_prediction_window=" << clampedPredictionWindow
               << " cpu_mode=dynarec"
               << " dynarec_rollback=pumped"
               << " state_capacity=" << kGekkoStateCapacity;
        write_gekko_log(stream.str());
    }

    std::string remoteAddress = std::string(remoteIp) + ":" + std::to_string(remotePort);
#ifdef RMGK_HAVE_P2P_TRANSPORT
    remoteAddress = "p2p-peer";
    g_GekkoP2PRemoteAddress = remoteAddress;
#endif
    for (int player = 1; player <= players; player++)
    {
        if (player == localPlayer)
        {
            const int handle = gekko_add_actor(g_GekkoSession, GekkoLocalPlayer, nullptr);
            if (handle < 0)
            {
                write_gekko_log("gekko_add_actor result=fail type=local");
                close_session();
                return false;
            }
            g_GekkoLocalHandle = handle;
            g_GekkoPlayerHandles[static_cast<size_t>(player - 1)] = handle;
            g_GekkoLocalHandles[static_cast<size_t>(player - 1)] = handle;
            gekko_set_local_delay(g_GekkoSession, handle, static_cast<unsigned char>(clampedLocalDelay));
            if (g_GekkoLogEnabled)
            {
                std::ostringstream stream;
                stream << "gekko_add_actor result=ok player=" << player << " type=local handle=" << handle;
                write_gekko_log(stream.str());
            }
        }
        else
        {
            GekkoNetAddress address = {};
            address.data = remoteAddress.data();
            address.size = static_cast<unsigned int>(remoteAddress.size());
            const int handle = gekko_add_actor(g_GekkoSession, GekkoRemotePlayer, &address);
            if (handle < 0)
            {
                write_gekko_log("gekko_add_actor result=fail type=remote");
                close_session();
                return false;
            }
            if (g_GekkoRemoteHandle < 0)
            {
                g_GekkoRemoteHandle = handle;
            }
            g_GekkoPlayerHandles[static_cast<size_t>(player - 1)] = handle;
            if (g_GekkoLogEnabled)
            {
                std::ostringstream stream;
                stream << "gekko_add_actor result=ok player=" << player
                       << " type=remote handle=" << handle
                       << " remote=" << remoteAddress;
                write_gekko_log(stream.str());
            }
        }
    }

    if (g_GekkoLocalHandle < 0)
    {
        write_gekko_log("start_p2p_session result=fail reason=no_local_handle");
        close_session();
        return false;
    }

    if (!install_core_input_callback())
    {
        write_gekko_log("install_core_input_callback result=fail");
        close_session();
        return false;
    }
    write_gekko_log("install_core_input_callback result=ok");
    return true;
#endif
}

CORE_EXPORT bool rmgk_gekko::start_local_session(const char* gameName, int players, int inputSize, int localDelay)
{
#ifndef RMGK_HAVE_GEKKONET
    (void)gameName;
    (void)players;
    (void)inputSize;
    (void)localDelay;
    return false;
#else
    close_session();

    g_GekkoLocalPlayer = 1;
    g_GekkoStopRequested.store(false, std::memory_order_relaxed);
    reset_gekko_log();

    if (gameName == nullptr || players < 1 || players > 4 || inputSize != static_cast<int>(sizeof(uint32_t)))
    {
        write_gekko_log("start_local_session result=fail reason=invalid_params");
        return false;
    }

    if (!gekko_create(&g_GekkoSession, GekkoGameSession))
    {
        write_gekko_log("gekko_create result=fail");
        return false;
    }

    const int clampedLocalDelay = std::clamp(localDelay, 0, 10);
    const int predictionWindow = 7;

    GekkoConfig config = {};
    config.num_players = static_cast<unsigned char>(players);
    config.max_spectators = 0;
    config.input_prediction_window = static_cast<unsigned char>(predictionWindow);
    config.input_size = static_cast<unsigned int>(inputSize);
    config.state_size = kGekkoStateCapacity;
    config.limited_saving = false;
    config.desync_detection = true;
    config.check_distance = 10;
    gekko_start(g_GekkoSession, &config);
    gekko_set_runahead(g_GekkoSession, 0);

    g_GekkoPlayers = players;
    g_GekkoInputSize = inputSize;
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLocalHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLatchedInput.assign(static_cast<size_t>(players * inputSize), 0);
    g_GekkoHasLatchedInput = false;
    g_GekkoLatchedFrame = -1;
    g_GekkoLatchedRunningAhead = false;
    g_GekkoFrameInputBuffer.clear();
    g_GekkoMaxObservedFrame = -1;
    g_GekkoPendingSaves.clear();

    for (int player = 1; player <= players; player++)
    {
        const int handle = gekko_add_actor(g_GekkoSession, GekkoLocalPlayer, nullptr);
        if (handle < 0)
        {
            write_gekko_log("gekko_add_actor result=fail type=local");
            close_session();
            return false;
        }
        if (player == 1)
        {
            g_GekkoLocalHandle = handle;
        }
        g_GekkoPlayerHandles[static_cast<size_t>(player - 1)] = handle;
        g_GekkoLocalHandles[static_cast<size_t>(player - 1)] = handle;
        gekko_set_local_delay(g_GekkoSession, handle, static_cast<unsigned char>(clampedLocalDelay));
        if (g_GekkoLogEnabled)
        {
            std::ostringstream stream;
            stream << "gekko_add_actor result=ok player=" << player << " type=local handle=" << handle;
            write_gekko_log(stream.str());
        }
    }

    if (g_GekkoLocalHandle < 0)
    {
        write_gekko_log("start_local_session result=fail reason=no_local_handle");
        close_session();
        return false;
    }

    if (!install_core_input_callback())
    {
        write_gekko_log("install_core_input_callback result=fail");
        close_session();
        return false;
    }
    write_gekko_log("start_local_session result=ok");
    return true;
#endif
}

CORE_EXPORT void rmgk_gekko::close_session()
{
#ifdef RMGK_HAVE_GEKKONET
    g_GekkoStopRequested.store(false, std::memory_order_relaxed);
    clear_core_input_callback();
    if (g_GekkoSession != nullptr)
    {
        gekko_destroy(&g_GekkoSession);
        gekko_default_adapter_destroy();
    }
    g_GekkoSession = nullptr;
    g_GekkoPlayers = 0;
    g_GekkoInputSize = 0;
    g_GekkoLocalPlayer = 0;
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.clear();
    g_GekkoLocalHandles.clear();
    g_GekkoLatchedInput.clear();
    g_GekkoHasLatchedInput = false;
    g_GekkoLatchedFrame = -1;
    g_GekkoLatchedRunningAhead = false;
#ifdef RMGK_HAVE_P2P_TRANSPORT
    // Flush any remaining frames still inside the rollback window at teardown.
    for (auto& entry : g_GekkoFrameInputBuffer)
    {
        n02::recordingWriteInputs(entry.second.data(), static_cast<int>(entry.second.size()));
    }
#endif
    g_GekkoFrameInputBuffer.clear();
    g_GekkoMaxObservedFrame = -1;
    g_GekkoExecuting.store(false, std::memory_order_relaxed);
    g_GekkoPendingSaves.clear();
    g_GekkoDebugInputProvider = nullptr;
    g_GekkoDebugBeginFrame = nullptr;
    g_GekkoDebugEndFrame = nullptr;
    g_GekkoDebugUserData = nullptr;
    g_GekkoDebugFrameOutput = -1;
    {
        std::lock_guard<std::mutex> lock(g_GekkoClientReplayMutex);
        g_GekkoClientReplayMode = ClientInputReplayMode::Off;
        g_GekkoClientReplayInputs.clear();
        g_GekkoClientReplayIndex = 0;
    }
    g_GekkoSpeedScale = 1.0;
    g_GekkoTimesyncSampleCounter = 0;
    g_GekkoTimesyncTargetScale = 1.0;
    CoreRollbackSetTimesyncScale(1.0);
#ifdef RMGK_HAVE_P2P_TRANSPORT
    g_GekkoP2PRemoteAddress.clear();
#endif
#endif
}

CORE_EXPORT void rmgk_gekko::request_stop()
{
#ifdef RMGK_HAVE_GEKKONET
    if (g_GekkoExecuting.load(std::memory_order_relaxed))
    {
        g_GekkoStopRequested.store(true, std::memory_order_relaxed);
    }
#endif
}

CORE_EXPORT bool rmgk_gekko::is_netplay_session_active()
{
#ifdef RMGK_HAVE_GEKKONET
    return g_GekkoSession != nullptr && g_GekkoRemoteHandle >= 0;
#else
    return false;
#endif
}

CORE_EXPORT bool rmgk_gekko::execute()
{
#ifndef RMGK_HAVE_GEKKONET
    return false;
#else
    if (g_GekkoSession == nullptr)
    {
        return false;
    }

    m64p_rollback_execute_callbacks callbacks = {};
    callbacks.begin_frame = rollback_execute_begin_frame;
    callbacks.end_frame = rollback_execute_end_frame;
    g_GekkoExecuting.store(true, std::memory_order_relaxed);
    bool result = CoreRollbackExecute(callbacks);
    g_GekkoExecuting.store(false, std::memory_order_relaxed);
    return result;
#endif
}

CORE_EXPORT bool rmgk_gekko::set_deterministic(bool enabled)
{
    return CoreRollbackSetDeterministic(enabled);
}

CORE_EXPORT bool rmgk_gekko::install_core_input_callback()
{
    return CoreRollbackSetInputPlayers(g_GekkoPlayers) &&
        CoreRollbackSetInputCallback(rmgk_gekko_core_input_callback);
}

CORE_EXPORT void rmgk_gekko::clear_core_input_callback()
{
    CoreRollbackSetInputCallback(nullptr);
}

CORE_EXPORT bool rmgk_gekko::synchronize_input(void* values, int size, int players)
{
#ifdef RMGK_HAVE_GEKKONET
    if (g_GekkoSession != nullptr)
    {
        const int expectedBytes = g_GekkoPlayers * g_GekkoInputSize;
        if (values == nullptr || size != g_GekkoInputSize || players < g_GekkoPlayers ||
            static_cast<int>(g_GekkoLatchedInput.size()) < expectedBytes)
        {
            write_gekko_log("pif_sync result=fail reason=shape");
            return false;
        }

        if (!g_GekkoHasLatchedInput)
        {
            write_gekko_log("pif_sync result=fail reason=no_latched_input");
            return false;
        }

        std::memset(values, 0, static_cast<size_t>(size) * static_cast<size_t>(players));
        std::memcpy(values, g_GekkoLatchedInput.data(), static_cast<size_t>(expectedBytes));

        // Recording push happens here (not in latch_gekko_input) because this
        // callback only fires when the emulator actually issued a controller
        // read this frame — matching what playback consumes. Skipping the
        // push for frames the game didn't poll keeps the .krec consume/produce
        // cadences in lockstep and prevents the silent off-by-one drift that
        // would otherwise compound into a mid-session desync.
#ifdef RMGK_HAVE_P2P_TRANSPORT
        if (g_GekkoLatchedFrame >= 0 && !g_GekkoLatchedRunningAhead)
        {
            const int frame = g_GekkoLatchedFrame;
            g_GekkoFrameInputBuffer[frame] = g_GekkoLatchedInput;
            if (frame > g_GekkoMaxObservedFrame)
            {
                g_GekkoMaxObservedFrame = frame;
            }
            while (!g_GekkoFrameInputBuffer.empty())
            {
                auto it = g_GekkoFrameInputBuffer.begin();
                if (g_GekkoMaxObservedFrame - it->first <= kGekkoRecordingRollbackHorizon)
                {
                    break;
                }
                n02::recordingWriteInputs(it->second.data(), static_cast<int>(it->second.size()));
                g_GekkoFrameInputBuffer.erase(it);
            }
        }
#endif
        return true;
    }
#endif
    (void)values;
    (void)size;
    (void)players;
    return true;
}

CORE_EXPORT void rmgk_gekko::set_debug_hooks(InputProvider inputProvider, FrameCallback beginFrame, FrameCallback endFrame, void* userData)
{
#ifdef RMGK_HAVE_GEKKONET
    g_GekkoDebugInputProvider = inputProvider;
    g_GekkoDebugBeginFrame = beginFrame;
    g_GekkoDebugEndFrame = endFrame;
    g_GekkoDebugUserData = userData;
#else
    (void)inputProvider;
    (void)beginFrame;
    (void)endFrame;
    (void)userData;
#endif
}

CORE_EXPORT void rmgk_gekko::set_debug_frame_output(int flags)
{
#ifdef RMGK_HAVE_GEKKONET
    g_GekkoDebugFrameOutput = flags;
#else
    (void)flags;
#endif
}

CORE_EXPORT bool rmgk_gekko::debug_run_frame_with_inputs(const uint32_t* inputs, int players, int flags)
{
#ifdef RMGK_HAVE_GEKKONET
    if (inputs == nullptr || g_GekkoSession == nullptr || players < g_GekkoPlayers ||
        g_GekkoInputSize != static_cast<int>(sizeof(uint32_t)))
    {
        write_gekko_log("debug_run_frame_with_inputs result=fail reason=shape");
        return false;
    }

    const int expectedBytes = g_GekkoPlayers * g_GekkoInputSize;
    if (static_cast<int>(g_GekkoLatchedInput.size()) != expectedBytes)
    {
        g_GekkoLatchedInput.resize(static_cast<size_t>(expectedBytes));
    }

    std::memset(g_GekkoLatchedInput.data(), 0, static_cast<size_t>(expectedBytes));
    for (int player = 0; player < g_GekkoPlayers; player++)
    {
        std::memcpy(g_GekkoLatchedInput.data() + (player * g_GekkoInputSize),
            &inputs[player], sizeof(uint32_t));
    }
    g_GekkoHasLatchedInput = true;

    const auto runFrameBeginTime = std::chrono::steady_clock::now();
    const bool result = CoreRollbackRunFrame(flags);
    g_GekkoLastRunFrameUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - runFrameBeginTime).count();
    g_GekkoHasLatchedInput = false;

    if (g_GekkoLogEnabled)
    {
        CoreRollbackRunFrameStats runFrameStats;
        const bool hasRunFrameStats = CoreRollbackGetRunFrameStats(runFrameStats);
        std::ostringstream stream;
        stream << "debug_run_frame_with_inputs result=" << (result ? "ok" : "fail")
               << " elapsed_us=" << g_GekkoLastRunFrameUs
               << " flags=" << flags;
        if (hasRunFrameStats)
        {
            stream << " core_total_us=" << runFrameStats.totalUs
                   << " r4300_us=" << runFrameStats.r4300Us
                   << " vi_us=" << runFrameStats.viUs
                   << " new_frame_us=" << runFrameStats.newFrameUs
                   << " cheats_us=" << runFrameStats.cheatsUs
                   << " pacing_us=" << runFrameStats.pacingUs
                   << " input_us=" << runFrameStats.inputUs
                   << " pause_us=" << runFrameStats.pauseUs
                   << " netplay_us=" << runFrameStats.netplayUs
                   << " dynarec_recompiles=" << runFrameStats.dynarecRecompileCount
                   << " dynarec_recompile_us=" << runFrameStats.dynarecRecompileUs
                   << " dynarec_invalidate_us=" << runFrameStats.dynarecInvalidateUs
                   << " dynarec_full_invalidates=" << runFrameStats.dynarecFullInvalidateCount
                   << " dynarec_range_invalidates=" << runFrameStats.dynarecRangeInvalidateCount
                   << " dynarec_block_invalidates=" << runFrameStats.dynarecBlockInvalidateCount
                   << " dynarec_verify_dirty_count=" << runFrameStats.dynarecVerifyDirtyCount
                   << " dynarec_verify_dirty_us=" << runFrameStats.dynarecVerifyDirtyUs
                   << " dynarec_get_addr_count=" << runFrameStats.dynarecGetAddrCount
                   << " dynarec_get_addr_us=" << runFrameStats.dynarecGetAddrUs
                   << " dynarec_get_addr_ht_count=" << runFrameStats.dynarecGetAddrHtCount
                   << " dynarec_get_addr_32_count=" << runFrameStats.dynarecGetAddr32Count
                   << " dynarec_dynamic_linker_count=" << runFrameStats.dynarecDynamicLinkerCount
                   << " dynarec_dynamic_linker_us=" << runFrameStats.dynarecDynamicLinkerUs
                   << " dynarec_dynamic_linker_ds_count=" << runFrameStats.dynarecDynamicLinkerDsCount
                   << " dynarec_dynamic_linker_ds_us=" << runFrameStats.dynarecDynamicLinkerDsUs
                   << " cached_code_full_invalidates=" << runFrameStats.cachedCodeFullInvalidateCount
                   << " cached_code_range_invalidates=" << runFrameStats.cachedCodeRangeInvalidateCount
                   << " emumode=" << runFrameStats.emumode
                   << " cp0_count_before=" << runFrameStats.cp0CountBefore
                   << " cp0_count_after=" << runFrameStats.cp0CountAfter
                   << " cp0_count_delta=" << (runFrameStats.cp0CountAfter - runFrameStats.cp0CountBefore)
                   << " pc_before=0x" << std::hex << runFrameStats.pcBefore
                   << " pc_after=0x" << runFrameStats.pcAfter << std::dec
                   << " current_frame_before=" << runFrameStats.currentFrameBefore
                   << " current_frame_after=" << runFrameStats.currentFrameAfter
                   << " output_flags=" << runFrameStats.outputFlags;
        }
        for (int player = 0; player < g_GekkoPlayers; player++)
        {
            stream << " p" << (player + 1) << "=" << hex_input(inputs[player]);
        }
        write_gekko_log(stream.str());
    }
    return result;
#else
    (void)inputs;
    (void)players;
    (void)flags;
    return false;
#endif
}

CORE_EXPORT bool rmgk_gekko::toggle_client_input_replay()
{
#ifdef RMGK_HAVE_GEKKONET
    if (g_GekkoSession == nullptr || g_GekkoLocalPlayer != 2)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_GekkoClientReplayMutex);
    if (g_GekkoClientReplayMode == ClientInputReplayMode::Off)
    {
        g_GekkoClientReplayInputs.clear();
        g_GekkoClientReplayInputs.reserve(kGekkoClientReplayFrames);
        g_GekkoClientReplayIndex = 0;
        g_GekkoClientReplayMode = ClientInputReplayMode::Recording;
        write_gekko_log("client_input_replay mode=recording target_frames=600");
        return true;
    }

    g_GekkoClientReplayMode = ClientInputReplayMode::Off;
    g_GekkoClientReplayInputs.clear();
    g_GekkoClientReplayIndex = 0;
    write_gekko_log("client_input_replay mode=off");
    return true;
#else
    return false;
#endif
}
