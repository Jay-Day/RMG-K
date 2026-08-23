/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
// winsock2.h must precede any windows.h (pulled in by later headers), or the
// legacy winsock.h it would otherwise drag in conflicts with these APIs.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>
#include <unistd.h>
#endif

#define CORE_INTERNAL
#include "rmgk_gekko.hpp"

#include "Directories.hpp"
#include "Error.hpp"
#include "Library.hpp"
#include "Settings.hpp"
#ifdef NETPLAY
#include "LobbyIce.hpp"
#endif

#include "PreciseWait.hpp"

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
#include <cstdio>
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
// Lightweight stall diagnostics: don't treat a wait as a stall until it exceeds a
// normal sub-frame hitch (~a frame), then snapshot per-peer stats at a coarse
// wall-clock cadence. This keeps a multi-second freeze down to a handful of log
// lines instead of the verbose per-frame firehose.
constexpr long long kGekkoStallReportThresholdMs = 20;
constexpr long long kGekkoStallSnapshotIntervalMs = 250;

// ---------------------------------------------------------------------------
// Frame-pacing / time-sync tuning. Two selectable models share the per-frame
// lerp + CoreRollbackSetTimesyncScale tail in apply_gekko_frame_pacing(); the
// active one is chosen per session by g_GekkoPacingMode (the Rollback_PacingMode
// setting, or the RMGK_GEKKO_PACING_MODE env override). See GekkoPacingMode.
// ---------------------------------------------------------------------------

// Symmetric ("aggressive") model — the default. Recomputes every frame and
// pulls a drifting client back hard from either side: a single signed
// strength*frames_ahead correction clamped to a wide scale window. Reacts fast,
// at the cost of nudging the ahead player's framerate a touch more visibly.
constexpr float  kGekkoSymTimesyncDeadzone       = 0.20f;
constexpr double kGekkoSymTimesyncStrength       = 0.015;
constexpr double kGekkoSymTimesyncMinScale       = 0.97;
constexpr double kGekkoSymTimesyncMaxScale       = 1.03;
constexpr double kGekkoSymTimesyncLerp           = 0.35;
// Recompute timesync every frame so clients that drift ahead are pulled back
// immediately instead of waiting on a coarse lockstep interval.
constexpr int    kGekkoSymTimesyncIntervalFrames = 1;

// Asymmetric (Slippi-style) model (see project-slippi Ishiiruka,
// EXI_DeviceSlippi.cpp shouldAdvanceOnlineFrame). The behind player speeds up
// at twice the authority the ahead player slows down, and the deadzone is
// biased so each client happily sits slightly ahead. This keeps the ahead
// player close to full speed while still shrinking its speculative window, so
// it sees fewer rollback "teleports" of the remote character.
//
// Slippi works in microseconds of clock offset; we work in gekko_frames_ahead()
// (signed frames, +ve = local ahead), so the windows/deadzones below are the
// frame-unit equivalents of Slippi's 8000us / -250us deadzone and 3-frame ramp.
constexpr float  kGekkoAsymTimesyncAheadDeadzone  = 0.48f;  // tolerate being ahead by ~half a frame
constexpr float  kGekkoAsymTimesyncBehindDeadzone = 0.015f; // but correct almost immediately when behind
constexpr double kGekkoAsymTimesyncSpeedUpWindow  = 3.0;    // frames behind to reach full speed-up
constexpr double kGekkoAsymTimesyncSlowDownWindow = 3.0;    // frames ahead to reach full slow-down
constexpr double kGekkoAsymTimesyncMaxSpeedUp     = 0.01;   // behind: up to +1.0% (scale -> 1.01)
constexpr double kGekkoAsymTimesyncMaxSlowDown    = 0.005;  // ahead:  up to -0.5% (scale -> 0.995)
constexpr double kGekkoAsymTimesyncLerp           = 0.15;
// Sample gekko_frames_ahead() this often when recomputing the target emulation
// speed in the asymmetric model. Mirrors Slippi's SLIPPI_ONLINE_LOCKSTEP_INTERVAL
// (30 frames @ 60Hz = once per ~500ms) — single-frame jitter spikes no longer
// kick the speed scale around; the lerp keeps speed_scale converging toward the
// cached target on every frame in between.
constexpr int    kGekkoAsymTimesyncIntervalFrames = 30;
constexpr size_t kGekkoClientReplayFrames = 600;

#ifdef RMGK_HAVE_GEKKONET
enum class ClientInputReplayMode
{
    Off,
    Recording,
    Playing
};

// Rollback time-sync model. Smooth (asymmetric) is the only supported model;
// symmetric survives solely behind the RMGK_GEKKO_PACING_MODE dev override.
// Cached per session in reset_gekko_log() so apply_gekko_frame_pacing() —
// which runs every frame — never hits the environment on the hot path.
enum class GekkoPacingMode
{
    Symmetric  = 0, // aggressive per-frame correction (dev override only)
    Asymmetric = 1, // Slippi-style biased correction (the default)
};

struct PendingGekkoSave
{
    int frame = 0;
    unsigned int* checksum = nullptr;
    unsigned int* stateLen = nullptr;
    unsigned char* state = nullptr;
};

GekkoSession* g_GekkoSession = nullptr;
// Number of N64 controller ports this session drives — the highest occupied
// seat, NOT the player count. Seats may be sparse (players in P1 and P3 with P2
// empty), so this is >= g_GekkoActors.
int g_GekkoPlayers = 0;
// Number of GekkoNet actors, which is the actual player count. GekkoNet hands
// back handles in gekko_add_actor call order, so with sparse seats the handles
// are dense (0..g_GekkoActors-1) while g_GekkoPlayerHandles stays seat-indexed
// with -1 in the holes. Only the two places that talk to GekkoNet's own arrays
// — the per-frame input blob and the handle bounds check — use this; everything
// else is per-port and wants g_GekkoPlayers.
int g_GekkoActors = 0;
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

// Slots (1-indexed) queued by request_disconnect_player() on another thread,
// drained on the emulation thread before each frame's update_session. Guarded
// by its own mutex so the lobby/UI thread can push without touching the
// GekkoNet session (which is single-threaded on the emulation thread).
std::mutex g_GekkoDisconnectMutex;
std::vector<int> g_GekkoPendingDisconnectSlots;
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
// Timesync state. TargetScale is recomputed on sample frames (cadence depends on
// the active pacing mode); SpeedScale lerps toward it so pacing is smooth.
int g_GekkoTimesyncSampleCounter = 0;
double g_GekkoTimesyncTargetScale = 1.0;
// Active frame-pacing model, cached at session start (see GekkoPacingMode).
GekkoPacingMode g_GekkoPacingMode = GekkoPacingMode::Asymmetric;
bool g_GekkoLogEnabled = false;
// Lightweight stall diagnostics, independent of verbose logging. Only emits while a
// rollback session is genuinely stalled (waiting on a peer's input), so the log
// stays tiny during smooth play.
bool g_GekkoStallLogEnabled = false;
bool g_GekkoStallReported = false;
std::chrono::steady_clock::time_point g_GekkoStallBeginTime;
std::chrono::steady_clock::time_point g_GekkoStallLastSnapshotTime;
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

// ---------------------------------------------------------------------------
// Buffered rollback pacing trace.
//
// Enabled by Settings -> Rollback -> Logging -> Pacing Trace. Rows remain in
// memory during gameplay and are written only when rollback execution ends,
// avoiding per-frame disk I/O.
// ---------------------------------------------------------------------------
constexpr std::size_t kRmgkPacingTraceCapacity = 60000;
constexpr std::size_t kRmgkPacingTraceInvalidRow =
    static_cast<std::size_t>(-1);

struct RmgkPacingTraceRow
{
    std::uint64_t sequence = 0;
    long long timestampUs = 0;

    int coreFrameBegin = -1;
    int coreFrameSwap = -1;
    int coreFrameEnd = -1;

    float framesAhead = 0.0f;
    double targetScale = 1.0;
    double internalScale = 1.0;
    double clampedScale = 1.0;
    int pacingMode = 0;
    int sampleFrame = 0;

    int events = 0;
    int saves = 0;
    int loads = 0;
    int rollbackAdvances = 0;
    int runaheadAdvances = 0;
    int waitLoops = 0;

    long long debugBeginUs = 0;
    long long beginTotalUs = 0;
    long long networkPollUs = 0;
    long long pacingCalcUs = 0;
    long long submitInputUs = 0;
    long long updateSessionUs = 0;
    long long latchInputUs = 0;
    long long saveTotalUs = 0;
    long long loadTotalUs = 0;
    long long resimTotalUs = 0;
    long long resimMaxUs = 0;
    long long waitTotalUs = 0;
    long long waitMaxUs = 0;

    int presentPacerActive = 0;
    int presentPacerFirstFrame = 0;
    int presentPacerDeadlineMiss = 0;
    double presentPacerScale = 1.0;
    double presentPacerPeriodUs = 0.0;
    long long presentIntervalBeforeWaitUs = 0;
    long long presentWaitRequestedUs = 0;
    long long presentWaitActualUs = 0;
    long long presentLateUs = 0;
    long long presentIntervalAfterWaitUs = 0;

    long long swapUs = 0;
    long long makeCurrentUs = 0;
    int swapPath = 0; // 1 = native WGL, 2 = Qt OpenGL

    long long endTotalUs = 0;
    long long pendingSaveUs = 0;
    long long debugEndUs = 0;
};

std::vector<RmgkPacingTraceRow> g_RmgkPacingTraceRows;
bool g_RmgkPacingTraceEnabled = false;
std::size_t g_RmgkPacingTraceActiveRow =
    kRmgkPacingTraceInvalidRow;

float g_RmgkTraceFramesAhead = 0.0f;
double g_RmgkTraceTargetScale = 1.0;
double g_RmgkTraceInternalScale = 1.0;
double g_RmgkTraceClampedScale = 1.0;
int g_RmgkTracePacingMode = 0;
int g_RmgkTraceSampleFrame = 0;

// ---------------------------------------------------------------------------
// Rollback presentation pacer.
//
// The ordinary core limiter is bypassed for visible rollback frames. The
// emulation thread then waits here, immediately before SwapBuffers(), until the
// next phase-locked presentation deadline. Hidden rollback work therefore
// consumes idle time instead of being added after an earlier core sleep.
//
// The core publishes the exact nominal VI rate before the first swap.
// RMGK_ROLLBACK_PRESENT_HZ may be used as an explicit diagnostic override.
// ---------------------------------------------------------------------------
bool g_RollbackPresentPacerEnabled = true;
double g_RollbackPresentBaseHz = 60.0;
bool g_RollbackPresentBaseHzOverridden = false;
bool g_RollbackPresentPacerInitialized = false;
std::chrono::steady_clock::time_point g_RollbackPresentLastSwapTime;
std::chrono::steady_clock::time_point g_RollbackPresentTargetTime;

void reset_rollback_present_pacer()
{
    // The rollback presentation pacer is a permanent part of the rollback
    // frame path. It remains gated below by the active session/execution checks.
    g_RollbackPresentPacerEnabled = true;

    g_RollbackPresentBaseHz = 60.0;
    g_RollbackPresentBaseHzOverridden = false;

    const char* hzEnv =
        std::getenv("RMGK_ROLLBACK_PRESENT_HZ");

    if (hzEnv != nullptr && hzEnv[0] != '\0')
    {
        char* end = nullptr;
        const double value = std::strtod(hzEnv, &end);

        if (end != hzEnv &&
            value >= 30.0 &&
            value <= 240.0)
        {
            g_RollbackPresentBaseHz = value;
            g_RollbackPresentBaseHzOverridden = true;
        }
    }

    g_RollbackPresentPacerInitialized = false;
    g_RollbackPresentLastSwapTime =
        std::chrono::steady_clock::time_point{};
    g_RollbackPresentTargetTime =
        std::chrono::steady_clock::time_point{};
}

void record_rollback_present_pacing(
    int active,
    int firstFrame,
    int deadlineMiss,
    double scale,
    double periodUs,
    long long intervalBeforeWaitUs,
    long long waitRequestedUs,
    long long waitActualUs,
    long long lateUs,
    long long intervalAfterWaitUs)
{
    if (!g_RmgkPacingTraceEnabled ||
        g_RmgkPacingTraceActiveRow ==
            kRmgkPacingTraceInvalidRow ||
        g_RmgkPacingTraceActiveRow >=
            g_RmgkPacingTraceRows.size())
    {
        return;
    }

    auto& row =
        g_RmgkPacingTraceRows[
            g_RmgkPacingTraceActiveRow];

    row.presentPacerActive = active;
    row.presentPacerFirstFrame = firstFrame;
    row.presentPacerDeadlineMiss = deadlineMiss;
    row.presentPacerScale = scale;
    row.presentPacerPeriodUs = periodUs;
    row.presentIntervalBeforeWaitUs =
        intervalBeforeWaitUs;
    row.presentWaitRequestedUs = waitRequestedUs;
    row.presentWaitActualUs = waitActualUs;
    row.presentLateUs = lateUs;
    row.presentIntervalAfterWaitUs =
        intervalAfterWaitUs;
}

long long rmgk_pacing_trace_now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void rmgk_pacing_trace_flush()
{
    g_RmgkPacingTraceActiveRow = kRmgkPacingTraceInvalidRow;

    if (!g_RmgkPacingTraceEnabled ||
        g_RmgkPacingTraceRows.empty())
    {
        g_RmgkPacingTraceRows.clear();
        return;
    }

    const std::string filename =
        g_GekkoLogPrefix + "_pacing_frontend.csv";

    const std::filesystem::path path =
        g_GekkoLogDirectory.empty()
            ? std::filesystem::path(filename)
            : g_GekkoLogDirectory / filename;

    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (file)
    {
        file
            << "sequence,timestamp_us,"
            << "core_frame_begin,core_frame_swap,core_frame_end,"
            << "frames_ahead,target_scale,internal_scale,clamped_scale,"
            << "pacing_mode,sample_frame,"
            << "events,saves,loads,rollback_advances,runahead_advances,"
            << "wait_loops,debug_begin_us,begin_total_us,"
            << "network_poll_us,pacing_calc_us,submit_input_us,"
            << "update_session_us,latch_input_us,save_total_us,"
            << "load_total_us,resim_total_us,resim_max_us,"
            << "wait_total_us,wait_max_us,"
            << "present_pacer_active,present_pacer_first_frame,"
            << "present_pacer_deadline_miss,"
            << "present_pacer_scale,present_pacer_period_us,"
            << "present_interval_before_wait_us,"
            << "present_wait_requested_us,present_wait_actual_us,"
            << "present_late_us,present_interval_after_wait_us,"
            << "swap_us,make_current_us,swap_path,"
            << "end_total_us,pending_save_us,debug_end_us\n";

        file << std::fixed << std::setprecision(9);

        for (const auto& row : g_RmgkPacingTraceRows)
        {
            file
                << row.sequence << ','
                << row.timestampUs << ','
                << row.coreFrameBegin << ','
                << row.coreFrameSwap << ','
                << row.coreFrameEnd << ','
                << row.framesAhead << ','
                << row.targetScale << ','
                << row.internalScale << ','
                << row.clampedScale << ','
                << row.pacingMode << ','
                << row.sampleFrame << ','
                << row.events << ','
                << row.saves << ','
                << row.loads << ','
                << row.rollbackAdvances << ','
                << row.runaheadAdvances << ','
                << row.waitLoops << ','
                << row.debugBeginUs << ','
                << row.beginTotalUs << ','
                << row.networkPollUs << ','
                << row.pacingCalcUs << ','
                << row.submitInputUs << ','
                << row.updateSessionUs << ','
                << row.latchInputUs << ','
                << row.saveTotalUs << ','
                << row.loadTotalUs << ','
                << row.resimTotalUs << ','
                << row.resimMaxUs << ','
                << row.waitTotalUs << ','
                << row.waitMaxUs << ','
                << row.presentPacerActive << ','
                << row.presentPacerFirstFrame << ','
                << row.presentPacerDeadlineMiss << ','
                << row.presentPacerScale << ','
                << row.presentPacerPeriodUs << ','
                << row.presentIntervalBeforeWaitUs << ','
                << row.presentWaitRequestedUs << ','
                << row.presentWaitActualUs << ','
                << row.presentLateUs << ','
                << row.presentIntervalAfterWaitUs << ','
                << row.swapUs << ','
                << row.makeCurrentUs << ','
                << row.swapPath << ','
                << row.endTotalUs << ','
                << row.pendingSaveUs << ','
                << row.debugEndUs
                << '\n';
        }
    }

    g_RmgkPacingTraceRows.clear();
}

void rmgk_pacing_trace_reset()
{
    g_RmgkPacingTraceRows.clear();
    g_RmgkPacingTraceActiveRow =
        kRmgkPacingTraceInvalidRow;

    g_RmgkPacingTraceEnabled =
        CoreSettingsGetBoolValue(
            SettingsID::Rollback_PacingTrace);

    if (g_RmgkPacingTraceEnabled)
    {
        g_RmgkPacingTraceRows.reserve(
            kRmgkPacingTraceCapacity);
    }

    g_RmgkTraceFramesAhead = 0.0f;
    g_RmgkTraceTargetScale = 1.0;
    g_RmgkTraceInternalScale = 1.0;
    g_RmgkTraceClampedScale = 1.0;
    g_RmgkTracePacingMode = 0;
    g_RmgkTraceSampleFrame = 0;
}

void rmgk_pacing_trace_begin_frame(
    int events,
    int saves,
    int loads,
    int rollbackAdvances,
    int runaheadAdvances,
    int waitLoops,
    long long debugBeginUs,
    long long beginTotalUs,
    long long networkPollUs,
    long long pacingCalcUs,
    long long submitInputUs,
    long long updateSessionUs,
    long long latchInputUs,
    long long saveTotalUs,
    long long loadTotalUs,
    long long resimTotalUs,
    long long resimMaxUs,
    long long waitTotalUs,
    long long waitMaxUs)
{
    g_RmgkPacingTraceActiveRow =
        kRmgkPacingTraceInvalidRow;

    if (!g_RmgkPacingTraceEnabled ||
        g_RmgkPacingTraceRows.size() >=
            kRmgkPacingTraceCapacity)
    {
        return;
    }

    RmgkPacingTraceRow row;

    row.sequence =
        static_cast<std::uint64_t>(
            g_RmgkPacingTraceRows.size());

    row.timestampUs = rmgk_pacing_trace_now_us();
    row.coreFrameBegin = CoreGetCurrentFrameCount();

    row.framesAhead = g_RmgkTraceFramesAhead;
    row.targetScale = g_RmgkTraceTargetScale;
    row.internalScale = g_RmgkTraceInternalScale;
    row.clampedScale = g_RmgkTraceClampedScale;
    row.pacingMode = g_RmgkTracePacingMode;
    row.sampleFrame = g_RmgkTraceSampleFrame;

    row.events = events;
    row.saves = saves;
    row.loads = loads;
    row.rollbackAdvances = rollbackAdvances;
    row.runaheadAdvances = runaheadAdvances;
    row.waitLoops = waitLoops;

    row.debugBeginUs = debugBeginUs;
    row.beginTotalUs = beginTotalUs;
    row.networkPollUs = networkPollUs;
    row.pacingCalcUs = pacingCalcUs;
    row.submitInputUs = submitInputUs;
    row.updateSessionUs = updateSessionUs;
    row.latchInputUs = latchInputUs;
    row.saveTotalUs = saveTotalUs;
    row.loadTotalUs = loadTotalUs;
    row.resimTotalUs = resimTotalUs;
    row.resimMaxUs = resimMaxUs;
    row.waitTotalUs = waitTotalUs;
    row.waitMaxUs = waitMaxUs;

    g_RmgkPacingTraceRows.push_back(row);
    g_RmgkPacingTraceActiveRow =
        g_RmgkPacingTraceRows.size() - 1;
}

void rmgk_pacing_trace_end_frame(
    long long endTotalUs,
    long long pendingSaveUs,
    long long debugEndUs)
{
    if (!g_RmgkPacingTraceEnabled ||
        g_RmgkPacingTraceActiveRow ==
            kRmgkPacingTraceInvalidRow ||
        g_RmgkPacingTraceActiveRow >=
            g_RmgkPacingTraceRows.size())
    {
        return;
    }

    auto& row =
        g_RmgkPacingTraceRows[
            g_RmgkPacingTraceActiveRow];

    row.coreFrameEnd = CoreGetCurrentFrameCount();
    row.endTotalUs = endTotalUs;
    row.pendingSaveUs = pendingSaveUs;
    row.debugEndUs = debugEndUs;

    g_RmgkPacingTraceActiveRow =
        kRmgkPacingTraceInvalidRow;
}

// --- Spectate keyframe capture-and-hold (broadcaster side) ---
// The UI requests a keyframe; we snapshot the live frame's state, then HOLD it until
// the recording confirms that frame (flushes it to the krec, past the rollback
// horizon) with no rollback having touched frame <= it since the snapshot. Only then
// is it a state a spectator can restore and replay the confirmed krec from without
// desyncing. The snapshot is taken right after the live frame's input is latched into
// PIF RAM, so its "next" input to apply is the following frame — hence the replay
// frame the spectator starts at is the captured frame + this offset. We snapshot via
// GekkoNet's own per-frame save (the safe VI-boundary point), whose state is "before
// frame F's input" — the rollback load+replay convention — so the spectator replays
// from frame F itself (offset 0). If testing reveals a 1-frame misalignment, nudge this.
constexpr int kKeyframeReplayFrameOffset = 0;
std::atomic<bool> g_GekkoKeyframeRequested{false};
bool g_GekkoKeyframePending = false;
int  g_GekkoKeyframePendingLiveFrame = -1; // the live frame F we snapshotted (confirm when flushed)
std::vector<unsigned char> g_GekkoKeyframePendingBuf;
std::mutex g_GekkoKeyframeReadyMutex;
bool g_GekkoKeyframeReady = false;
int  g_GekkoKeyframeReadyFrame = -1; // krec frame the spectator replays from (F + offset)
std::vector<unsigned char> g_GekkoKeyframeReadyBuf;

// Spectate keyframe divergence probe (diagnostic): once a keyframe is captured, log a
// content hash of the broadcaster's confirmed state for the keyframe frame and the next
// kSpectateProbeWindow frames, so they can be diffed against the spectator's replayed-
// state hashes (see Emulation.cpp) to find the first divergent frame. -1 = inactive.
constexpr int kSpectateProbeWindow = 30;
int g_GekkoSpectateProbeStartFrame = -1;
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
    rmgk_pacing_trace_flush();
    reset_rollback_log_session();
    rmgk_pacing_trace_reset();
    reset_rollback_present_pacer();

    const char* logEnv = std::getenv("RMGK_GEKKO_LOG");
    g_GekkoLogEnabled = CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats) ||
        (logEnv != nullptr && std::strcmp(logEnv, "0") != 0);
    const char* stallEnv = std::getenv("RMGK_GEKKO_STALL_LOG");
    g_GekkoStallLogEnabled = CoreSettingsGetBoolValue(SettingsID::Rollback_StallDiagnostics) ||
        (stallEnv != nullptr && std::strcmp(stallEnv, "0") != 0);
    g_GekkoStallReported = false;

    // Frame-pacing model, cached per session regardless of logging (this runs
    // before the early-out below). Smooth (asymmetric) is the only supported
    // model now — the UI selector is gone and the persisted Rollback_PacingMode
    // setting is ignored, so stale configs and room-sync writes are inert.
    // RMGK_GEKKO_PACING_MODE=0 survives as a dev-only A/B override for the old
    // symmetric model.
    const char* pacingEnv = std::getenv("RMGK_GEKKO_PACING_MODE");
    g_GekkoPacingMode = (pacingEnv != nullptr &&
            std::atoi(pacingEnv) == static_cast<int>(GekkoPacingMode::Symmetric))
        ? GekkoPacingMode::Symmetric
        : GekkoPacingMode::Asymmetric;

    if (!g_GekkoLogEnabled && !g_GekkoStallLogEnabled)
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
    file << "RMG-K GekkoNet log (verbose=" << (g_GekkoLogEnabled ? 1 : 0)
         << " stall_only=" << ((g_GekkoStallLogEnabled && !g_GekkoLogEnabled) ? 1 : 0) << ")\n";
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

// Stall-diagnostics writer: fires when EITHER verbose logging or the lightweight
// stall-only toggle is on, so stall records show up in both modes (and in the
// stall-only mode they are the only thing written). Same one-line-append shape as
// write_gekko_log.
void write_gekko_stall_log(const std::string& message)
{
    if (!g_GekkoLogEnabled && !g_GekkoStallLogEnabled)
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

// ── n02-style shared transport ───────────────────────────────────────
// The lobby's anchor socket, adopted for the duration of a lobby session so
// the port and its NAT mappings never change hands (n02 keeps one k_socket
// alive for lobby and game alike; this is that model). The descriptor is
// borrowed from Qt on the UI thread and used for sendto/recvfrom here on the
// emulation thread — concurrent use of one UDP socket from two threads is
// well-defined, and the lobby side stops *reading* while a session runs so
// datagrams are never stolen from GekkoNet (the same reader-discipline n02
// uses between p2p_step and p2p_modify_play_values).
#ifdef _WIN32
using ExtSocket = SOCKET;
constexpr ExtSocket kExtSocketNone = INVALID_SOCKET;
#else
using ExtSocket = int;
constexpr ExtSocket kExtSocketNone = -1;
#endif
static std::atomic<ExtSocket> g_GekkoExternalSocket{kExtSocketNone};
// True while the CURRENT session runs on the adopted socket (latched at
// start_lobby_session so a mid-session clear can't strand the adapter).
static bool g_GekkoUsingExternalSocket = false;
static bool g_GekkoUsingIceAdapter = false;
static std::vector<GekkoNetResult*> g_GekkoExtResults;

static void ext_adapter_send(GekkoNetAddress* addr, const char* data, int length)
{
    const ExtSocket fd = g_GekkoExternalSocket.load();
    if (fd == kExtSocketNone || addr == nullptr || addr->data == nullptr)
    {
        return;
    }
    // Address arrives as an "ip:port" string, same contract as the default
    // asio adapter (see STOE in gekkonet.cpp).
    const std::string endpoint(reinterpret_cast<const char*>(addr->data), addr->size);
    const std::string::size_type colon = endpoint.rfind(':');
    if (colon == std::string::npos)
    {
        return;
    }
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<unsigned short>(std::atoi(endpoint.c_str() + colon + 1)));
    if (inet_pton(AF_INET, endpoint.substr(0, colon).c_str(), &out.sin_addr) != 1)
    {
        return;
    }
    sendto(fd, data, length, 0, reinterpret_cast<const sockaddr*>(&out), sizeof(out));
}

static GekkoNetResult** ext_adapter_receive(int* length)
{
    g_GekkoExtResults.clear();
    if (length == nullptr)
    {
        return g_GekkoExtResults.data();
    }
    const ExtSocket fd = g_GekkoExternalSocket.load();
    if (fd == kExtSocketNone)
    {
        *length = 0;
        return g_GekkoExtResults.data();
    }

    for (;;)
    {
        char buffer[2048];
        sockaddr_in from{};
#ifdef _WIN32
        int fromLen = static_cast<int>(sizeof(from));
#else
        socklen_t fromLen = sizeof(from);
#endif
        const int received = static_cast<int>(recvfrom(fd, buffer, static_cast<int>(sizeof(buffer)), 0,
                                                       reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (received <= 0)
        {
            break; // WOULDBLOCK / no more datagrams (socket is non-blocking)
        }

        // Lobby traffic (anchor keepalive acks, ping probes) shares this
        // socket and is RMGK-magic prefixed; it isn't GekkoNet's to parse.
        if (received >= 4 && std::memcmp(buffer, "RMGK", 4) == 0)
        {
            continue;
        }

        char addrText[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &from.sin_addr, addrText, sizeof(addrText)) == nullptr)
        {
            continue;
        }
        const std::string endpoint = std::string(addrText) + ":" + std::to_string(ntohs(from.sin_port));

        GekkoNetResult* result = reinterpret_cast<GekkoNetResult*>(std::malloc(sizeof(*result)));
        if (result == nullptr)
        {
            break;
        }
        result->addr.data = std::malloc(endpoint.size());
        result->data = std::malloc(static_cast<size_t>(received));
        if (result->addr.data == nullptr || result->data == nullptr)
        {
            std::free(result->addr.data);
            std::free(result->data);
            std::free(result);
            break;
        }
        result->addr.size = static_cast<unsigned int>(endpoint.size());
        std::memcpy(result->addr.data, endpoint.data(), endpoint.size());
        result->data_len = static_cast<unsigned int>(received);
        std::memcpy(result->data, buffer, static_cast<size_t>(received));
        g_GekkoExtResults.push_back(result);
    }

    *length = static_cast<int>(g_GekkoExtResults.size());
    return g_GekkoExtResults.data();
}

static void ext_adapter_free(void* data_ptr)
{
    std::free(data_ptr);
}

static GekkoNetAdapter g_GekkoExternalAdapter{
    ext_adapter_send,
    ext_adapter_receive,
    ext_adapter_free
};

#ifdef NETPLAY
static std::vector<GekkoNetResult*> g_GekkoIceResults;

static std::uint64_t ice_user_id(const GekkoNetAddress* addr)
{
    if (addr == nullptr || addr->data == nullptr)
        return 0;
    const std::string value(reinterpret_cast<const char*>(addr->data), addr->size);
    constexpr const char* prefix = "ice:";
    if (value.rfind(prefix, 0) != 0)
        return 0;
    try
    {
        return std::stoull(value.substr(std::char_traits<char>::length(prefix)));
    }
    catch (...)
    {
        return 0;
    }
}

static void ice_adapter_send(GekkoNetAddress* addr, const char* data, int length)
{
    const std::uint64_t userId = ice_user_id(addr);
    if (userId == 0 || length < 0 ||
        !LobbyIce::send(userId, LobbyIceChannel::Gekko, data, static_cast<std::size_t>(length)))
    {
        if (g_GekkoLogEnabled)
            write_gekko_log("ice_adapter_send result=fail");
    }
}

static GekkoNetResult** ice_adapter_receive(int* length)
{
    g_GekkoIceResults.clear();
    if (length == nullptr)
        return g_GekkoIceResults.data();

    for (LobbyIcePacket& packet : LobbyIce::take_packets(LobbyIceChannel::Gekko))
    {
        const std::string endpoint = "ice:" + std::to_string(packet.peerUserId);
        GekkoNetResult* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(*result)));
        if (result == nullptr)
            break;
        result->addr.data = std::malloc(endpoint.size());
        result->data = std::malloc(packet.data.size());
        if (result->addr.data == nullptr || (result->data == nullptr && !packet.data.empty()))
        {
            std::free(result->addr.data);
            std::free(result->data);
            std::free(result);
            break;
        }
        result->addr.size = static_cast<unsigned int>(endpoint.size());
        std::memcpy(result->addr.data, endpoint.data(), endpoint.size());
        result->data_len = static_cast<unsigned int>(packet.data.size());
        if (!packet.data.empty())
            std::memcpy(result->data, packet.data.data(), packet.data.size());
        g_GekkoIceResults.push_back(result);
    }
    *length = static_cast<int>(g_GekkoIceResults.size());
    return g_GekkoIceResults.data();
}

static void ice_adapter_free(void* data)
{
    std::free(data);
}

static GekkoNetAdapter g_GekkoIceAdapter{
    ice_adapter_send,
    ice_adapter_receive,
    ice_adapter_free
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

    // Spectate keyframe: when the UI asked for one and we aren't already holding a snapshot,
    // capture a FULL normal-format savestate of THIS frame (not GekkoNet's stripped rollback
    // save in save.state). A cold spectator must load a complete state via the normal path;
    // the rollback variant omits the TLB LUT and isn't zeroed (fine for a warm same-machine
    // reload, wrong for a cold join). Taken here at the same frame-aligned VI boundary GekkoNet
    // just saved at, so it stays aligned with the krec. The full save is read-only on the
    // device and calls no plugins (verified in savestates_save_m64p), so it cannot disturb the
    // live rollback. Core mallocs the buffer; we copy it out and free it. HOLD the snapshot —
    // it's only committed once the recording flushes this frame to the krec without an
    // intervening rollback, so the bytes match the confirmed krec a spectator replays.
    if (g_GekkoKeyframeRequested.load(std::memory_order_relaxed) && !g_GekkoKeyframePending &&
        save.frame >= 0 && state.len > 0 && save.state != nullptr)
    {
        CoreRollbackState fullState{};
        if (CoreRollbackSaveFullStateInto(fullState, nullptr, 0, save.frame) &&
            fullState.buffer != nullptr && fullState.len > 0)
        {
            g_GekkoKeyframePendingBuf.assign(fullState.buffer, fullState.buffer + fullState.len);
            g_GekkoKeyframePendingLiveFrame = save.frame;
            g_GekkoKeyframePending = true;
            g_GekkoKeyframeRequested.store(false, std::memory_order_relaxed);
            // Arm the divergence probe window for this keyframe's neighborhood.
            g_GekkoSpectateProbeStartFrame = save.frame;
            if (g_GekkoLogEnabled)
            {
                std::ostringstream stream;
                stream << "keyframe snapshot frame=" << save.frame
                       << " full_len=" << fullState.len << " rollback_len=" << state.len;
                write_gekko_log(stream.str());
            }
        }
        CoreRollbackFreeGameState(fullState);
    }

    // Spectate divergence probe (diagnostic): hash the broadcaster's confirmed state for
    // the keyframe frame and the following frames so a spectator's replayed-state hashes
    // can be diffed against them (see Emulation.cpp). Rollback can re-save a frame more
    // than once; when reading the log, take the LAST hash logged per frame number.
    if (g_GekkoSpectateProbeStartFrame >= 0 &&
        save.frame >= g_GekkoSpectateProbeStartFrame &&
        save.frame <= g_GekkoSpectateProbeStartFrame + kSpectateProbeWindow)
    {
        const uint64_t h = rmgk_gekko::hash_bytes(save.state, static_cast<size_t>(state.len));
        std::ostringstream stream;
        stream << "spectate_probe side=host frame=" << save.frame
               << " hash=" << std::hex << h << std::dec
               << " len=" << state.len;
        rmgk_gekko::write_spectate_probe(stream.str());
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
            if (!localOnlySession && g_GekkoLocalPlayer > 1 && g_GekkoClientReplayMode == ClientInputReplayMode::Recording)
            {
                g_GekkoClientReplayInputs.push_back(input);
                if (g_GekkoClientReplayInputs.size() >= kGekkoClientReplayFrames)
                {
                    g_GekkoClientReplayMode = ClientInputReplayMode::Playing;
                    g_GekkoClientReplayIndex = 0;
                    write_gekko_log("client_input_replay mode=playing recorded_frames=600");
                }
            }
            else if (!localOnlySession && g_GekkoLocalPlayer > 1 && g_GekkoClientReplayMode == ClientInputReplayMode::Playing &&
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
    // GekkoNet's blob is packed by handle — one entry per actor. The latched
    // buffer we hand the PIF is packed by port — one entry per seat, including
    // the empty ones.
    const int actorBytes = g_GekkoActors * g_GekkoInputSize;
    const int expectedBytes = g_GekkoPlayers * g_GekkoInputSize;
    if (event->data.adv.inputs == nullptr || static_cast<int>(event->data.adv.input_len) < actorBytes)
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
        // handle == -1 is an intentionally empty seat (a gap below an occupied
        // port). start_lobby_session already proved every *occupied* seat got an
        // actor, so a hole here is by design — leave the memset zeros in place,
        // which the N64 reads as a controller holding nothing.
        if (handle < 0)
        {
            continue;
        }
        if (handle >= g_GekkoActors)
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

// Append per-peer GekkoNet network stats (one labelled segment per remote actor)
// to a log stream. The session only exposes counters per handle via
// gekko_network_stats(), and the rest of this file historically logged just
// g_GekkoRemoteHandle (the FIRST remote) — blind to peers 2 and 3 in a 4-player
// mesh. Since the local sim blocks until EVERY peer's input arrives, a freeze is
// almost always one specific peer stalling; logging all of them lets a single
// logging client name the culprit (whichever peer's kb_recv flatlines / ping goes
// stale during the stall).
void append_peer_network_stats(std::ostringstream& stream)
{
    if (g_GekkoSession == nullptr)
    {
        return;
    }
    for (int player = 1; player <= g_GekkoPlayers; player++)
    {
        if (player == g_GekkoLocalPlayer)
        {
            continue;
        }
        if (player > static_cast<int>(g_GekkoPlayerHandles.size()))
        {
            continue;
        }
        const int handle = g_GekkoPlayerHandles[static_cast<size_t>(player - 1)];
        if (handle < 0)
        {
            continue;
        }
        GekkoNetworkStats stats = {};
        gekko_network_stats(g_GekkoSession, handle, &stats);
        stream << " peer" << player << "_handle=" << handle
               << " peer" << player << "_ping_ms=" << stats.last_ping
               << " peer" << player << "_avg_ping_ms=" << std::fixed << std::setprecision(1) << stats.avg_ping
               << " peer" << player << "_jitter_ms=" << stats.jitter
               << " peer" << player << "_kb_sent=" << std::setprecision(2) << stats.kb_sent
               << " peer" << player << "_kb_recv=" << stats.kb_received;
    }
}

void apply_gekko_frame_pacing()
{
    // Read frames_ahead every call (cheap, just a member access in
    // GekkoSession) but only recompute the target scale on sample frames. The
    // per-frame lerp below carries the speed scale toward the cached target
    // between samples. Both the sample cadence and the target computation depend
    // on the active pacing model (see GekkoPacingMode); the lerp tail is shared.
    const float framesAhead = gekko_frames_ahead(g_GekkoSession);
    const bool asymmetric = (g_GekkoPacingMode == GekkoPacingMode::Asymmetric);
    const int intervalFrames =
        asymmetric ? kGekkoAsymTimesyncIntervalFrames : kGekkoSymTimesyncIntervalFrames;
    const double lerp = asymmetric ? kGekkoAsymTimesyncLerp : kGekkoSymTimesyncLerp;
    const bool isSampleFrame =
        (g_GekkoTimesyncSampleCounter % intervalFrames) == 0;
    if (isSampleFrame)
    {
        if (asymmetric)
        {
            // Asymmetric correction: the behind player speeds up at twice the
            // authority the ahead player slows down, with a deadzone biased
            // toward sitting slightly ahead. Ramp linearly to the cap over the
            // configured frame window, matching Slippi's shouldAdvanceOnlineFrame.
            double deviation = 0.0;
            if (framesAhead < -kGekkoAsymTimesyncBehindDeadzone)
            {
                const double multiplier =
                    std::min(-static_cast<double>(framesAhead) / kGekkoAsymTimesyncSpeedUpWindow, 1.0);
                deviation = multiplier * kGekkoAsymTimesyncMaxSpeedUp;
            }
            else if (framesAhead > kGekkoAsymTimesyncAheadDeadzone)
            {
                const double multiplier =
                    std::min(static_cast<double>(framesAhead) / kGekkoAsymTimesyncSlowDownWindow, 1.0);
                deviation = multiplier * -kGekkoAsymTimesyncMaxSlowDown;
            }
            g_GekkoTimesyncTargetScale = 1.0 + deviation;
        }
        else
        {
            // Symmetric correction: a single signed strength*frames_ahead nudge
            // clamped to a wide scale window, recomputed every frame so a client
            // drifting ahead is pulled back immediately. Stronger and more
            // reactive than the asymmetric model.
            double newTarget = 1.0;
            if (framesAhead >= kGekkoSymTimesyncDeadzone || framesAhead <= -kGekkoSymTimesyncDeadzone)
            {
                newTarget = 1.0 - (static_cast<double>(framesAhead) * kGekkoSymTimesyncStrength);
                newTarget = std::clamp(newTarget, kGekkoSymTimesyncMinScale, kGekkoSymTimesyncMaxScale);
            }
            g_GekkoTimesyncTargetScale = newTarget;
        }
    }
    g_GekkoTimesyncSampleCounter++;

    g_GekkoSpeedScale += (g_GekkoTimesyncTargetScale - g_GekkoSpeedScale) * lerp;
    CoreRollbackSetTimesyncScale(g_GekkoSpeedScale);

    g_RmgkTraceFramesAhead = framesAhead;
    g_RmgkTraceTargetScale = g_GekkoTimesyncTargetScale;
    g_RmgkTraceInternalScale = g_GekkoSpeedScale;
    g_RmgkTraceClampedScale =
        std::clamp(g_GekkoSpeedScale, 0.99, 1.01);
    g_RmgkTracePacingMode =
        static_cast<int>(g_GekkoPacingMode);
    g_RmgkTraceSampleFrame = isSampleFrame ? 1 : 0;

    g_GekkoPacingLogFrames++;
    if (g_GekkoLogEnabled &&
        (g_GekkoTimesyncTargetScale != 1.0 || g_GekkoPacingLogFrames <= 10 || (g_GekkoPacingLogFrames % 60) == 0))
    {
        std::ostringstream stream;
        stream << "pacing mode=" << (asymmetric ? "asym" : "sym")
               << " frames_ahead=" << std::fixed << std::setprecision(2) << framesAhead
               << " sample=" << (isSampleFrame ? 1 : 0)
               << " target_scale=" << std::setprecision(4) << g_GekkoTimesyncTargetScale
               << " speed_scale=" << g_GekkoSpeedScale;

        append_peer_network_stats(stream);

        write_gekko_log(stream.str());
    }
}

// Drain slots queued by request_disconnect_player() and force GekkoNet to drop
// those actors now. Runs on the emulation thread (inside the frame pump), so it
// touches the GekkoNet session safely; the cross-thread hand-off is just the
// mutex-guarded slot list. (Already inside the file's RMGK_HAVE_GEKKONET block.)
void process_pending_disconnects()
{
    std::vector<int> slots;
    {
        std::lock_guard<std::mutex> lock(g_GekkoDisconnectMutex);
        if (g_GekkoPendingDisconnectSlots.empty())
        {
            return;
        }
        slots.swap(g_GekkoPendingDisconnectSlots);
    }
    for (int slot : slots)
    {
        if (slot < 1 || slot > g_GekkoPlayers || slot == g_GekkoLocalPlayer)
        {
            continue;
        }
        const int handle = g_GekkoPlayerHandles[static_cast<size_t>(slot - 1)];
        if (handle < 0)
        {
            continue;
        }
        gekko_disconnect_player(g_GekkoSession, handle);
        std::ostringstream stream;
        stream << "force_disconnect slot=" << slot << " handle=" << handle
               << " reason=lobby_peer_left";
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
    long long summaryWaitUs = 0;
    long long summaryMaxWaitUs = 0;
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
    // Apply any lobby-driven force-disconnects before update_session, so a known
    // peer drop substitutes idle input this very frame instead of stalling out
    // GekkoNet's 5 s silence timeout.
    process_pending_disconnects();
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

        // Lightweight stall diagnostics: a stall we reported has now resolved
        // (events arrived) — emit one compact end record before the wait counter is
        // cleared below, so the log captures the freeze's total wall-clock duration.
        if (count != 0 && g_GekkoStallReported &&
            (g_GekkoStallLogEnabled || g_GekkoLogEnabled))
        {
            const long long waitedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - g_GekkoStallBeginTime).count();
            std::ostringstream stream;
            stream << "stall end waited_ms=" << waitedMs
                   << " loops=" << g_GekkoWaitingLoops
                   << " frames_ahead=" << std::fixed << std::setprecision(2)
                   << gekko_frames_ahead(g_GekkoSession);
            append_peer_network_stats(stream);
            write_gekko_stall_log(stream.str());
            g_GekkoStallReported = false;
        }

        if (count == 0)
        {
            if (g_GekkoWaitingLoops == 0)
            {
                g_GekkoStallBeginTime = std::chrono::steady_clock::now();
                g_GekkoStallReported = false;
            }
            g_GekkoWaitingLoops++;
            summaryWaitLoops++;

            // Lightweight stall diagnostics: once a wait grows past a normal
            // sub-frame hitch, snapshot per-peer network stats at a coarse
            // wall-clock cadence. A peer whose kb_recv stops climbing across these
            // snapshots is the one starving the mesh.
            if (g_GekkoStallLogEnabled || g_GekkoLogEnabled)
            {
                const auto stallNow = std::chrono::steady_clock::now();
                const long long waitedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(stallNow - g_GekkoStallBeginTime).count();
                const long long sinceSnapMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(stallNow - g_GekkoStallLastSnapshotTime).count();
                if (waitedMs >= kGekkoStallReportThresholdMs &&
                    (!g_GekkoStallReported || sinceSnapMs >= kGekkoStallSnapshotIntervalMs))
                {
                    g_GekkoStallLastSnapshotTime = stallNow;
                    std::ostringstream stream;
                    stream << "stall " << (g_GekkoStallReported ? "ongoing" : "begin")
                           << " waited_ms=" << waitedMs
                           << " loops=" << g_GekkoWaitingLoops
                           << " frames_ahead=" << std::fixed << std::setprecision(2)
                           << gekko_frames_ahead(g_GekkoSession);
                    append_peer_network_stats(stream);
                    write_gekko_stall_log(stream.str());
                    g_GekkoStallReported = true;
                }
            }

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
                // A rollback to frame <= a held keyframe means that frame may be
                // re-simulated differently than we snapshotted — discard the pending
                // keyframe so we never commit a state that diverges from the krec.
                if (g_GekkoKeyframePending &&
                    event->data.load.frame <= g_GekkoKeyframePendingLiveFrame)
                {
                    g_GekkoKeyframePending = false;
                    write_gekko_log("keyframe discarded reason=rollback");
                }
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
                           << gekko_frames_ahead(g_GekkoSession)
                           << " target_scale=" << std::setprecision(4) << g_GekkoTimesyncTargetScale
                           << " speed_scale=" << g_GekkoSpeedScale;
                    if (summaryWaitLoops > 0 || summaryLoadCount > 0)
                    {
                        append_peer_network_stats(stream);
                    }
                    write_gekko_log(stream.str());
                }
            }
            const auto traceBeginTotalUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - beginTime).count();

            rmgk_pacing_trace_begin_frame(
                summaryEventCount,
                summarySaveCount,
                summaryLoadCount,
                summaryRollbackAdvanceCount,
                summaryRunaheadAdvanceCount,
                summaryWaitLoops,
                summaryDebugBeginUs,
                traceBeginTotalUs,
                summaryNetworkPollUs,
                summaryPacingUs,
                summarySubmitInputUs,
                summaryUpdateSessionUs,
                summaryLatchInputUs,
                summarySaveUs,
                summaryLoadUs,
                summaryResimUs,
                summaryMaxResimUs,
                summaryWaitUs,
                summaryMaxWaitUs);

            write_gekko_log("begin_frame result=real_frame");
            return 1;
        }

        //std::this_thread::sleep_for(std::chrono::microseconds(kGekkoWaitSleepUs)); // Not accurate enough in windows
        const auto traceWaitBegin =
            std::chrono::steady_clock::now();

        rmgk::timing::PreciseWaitFor(
            std::chrono::microseconds{kGekkoWaitSleepUs},
            std::chrono::microseconds{kGekkoWaitSleepUs});

        const long long traceWaitUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                traceWaitBegin).count();

        summaryWaitUs += traceWaitUs;
        summaryMaxWaitUs =
            std::max(summaryMaxWaitUs, traceWaitUs);
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

    const auto traceEndTotalUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - beginTime).count();

    rmgk_pacing_trace_end_frame(
        traceEndTotalUs,
        pendingSaveUs,
        debugEndUs);

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
    g_GekkoActors = players; // direct P2P seats are always contiguous 1..players
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

CORE_EXPORT bool rmgk_gekko::start_lobby_session(const char* gameName, int players, int inputSize,
    int localPlayer, unsigned short localPort,
    const LobbyRemotePeer* remotes, int numRemotes,
    int localDelay, int predictionWindow)
{
#ifndef RMGK_HAVE_GEKKONET
    (void)gameName;
    (void)players;
    (void)inputSize;
    (void)localPlayer;
    (void)localPort;
    (void)remotes;
    (void)numRemotes;
    (void)localDelay;
    (void)predictionWindow;
    return false;
#else
    // The lobby lends its anchor descriptor immediately before starting the
    // match, but close_session() treats the descriptor as session state and
    // clears it. Latch the lend across the cleanup, or every lobby match falls
    // through to the default adapter and tries to bind a port the lobby's own
    // socket still holds — which can never succeed under the shared-socket
    // model (the anchor is deliberately never released).
    const ExtSocket lentSocket = g_GekkoExternalSocket.load();
    close_session();
    g_GekkoExternalSocket.store(lentSocket);

    g_GekkoLocalPlayer = localPlayer;
    g_GekkoStopRequested.store(false, std::memory_order_relaxed);
    reset_gekko_log();

    // `players` is the highest occupied SEAT, not the player count — seats may
    // be sparse (P1 + P3 with P2 empty), so numRemotes can be short of
    // players - 1. It can never exceed it: seats are unique and one is ours.
    if (gameName == nullptr || players < 2 || players > 4 || inputSize != static_cast<int>(sizeof(uint32_t)) ||
        localPlayer < 1 || localPlayer > players || remotes == nullptr ||
        numRemotes < 1 || numRemotes > players - 1)
    {
        write_gekko_log("start_lobby_session result=fail reason=invalid_params");
        return false;
    }

    // Validate per-remote ICE identities (or legacy endpoints) and slot
    // uniqueness before we touch GekkoNet.
    bool slotSeen[5] = { false, false, false, false, false }; // index 1..4
    slotSeen[localPlayer] = true;
    bool sawIce = false;
    bool sawLegacyEndpoint = false;
    for (int i = 0; i < numRemotes; i++)
    {
        const LobbyRemotePeer& peer = remotes[i];
        if (peer.slot < 1 || peer.slot > players || peer.slot == localPlayer ||
            slotSeen[peer.slot] ||
            (peer.userId == 0 && (peer.ip.empty() || peer.port == 0)))
        {
            write_gekko_log("start_lobby_session result=fail reason=bad_remote_peer");
            return false;
        }
        sawIce = sawIce || peer.userId != 0;
        sawLegacyEndpoint = sawLegacyEndpoint || peer.userId == 0;
        slotSeen[peer.slot] = true;
    }
    if (sawIce && sawLegacyEndpoint)
    {
        write_gekko_log("start_lobby_session result=fail reason=mixed_lobby_transports");
        return false;
    }
    bool useIce = sawIce;
#ifdef NETPLAY
    if (useIce)
    {
        for (int i = 0; i < numRemotes; ++i)
        {
            if (!LobbyIce::peer_connected(remotes[i].userId))
            {
                CoreSetError("A rollback lobby ICE peer disconnected before emulation started.");
                write_gekko_log("start_lobby_session result=fail reason=ice_peer_not_connected");
                return false;
            }
        }
    }
#else
    useIce = false;
#endif

    if (!gekko_create(&g_GekkoSession, GekkoGameSession))
    {
        write_gekko_log("gekko_create result=fail");
        return false;
    }

    const int clampedLocalDelay = std::clamp(localDelay, 0, 10);
    const int clampedPredictionWindow = std::clamp(predictionWindow, 1, 10);

    GekkoConfig config = {};
    // ACTOR count, not seat count. GekkoNet's frame advance
    // (SyncSystem::GetCurrentInputs) requires an input in every buffer
    // 0..num_players-1, and handles are handed out densely in add order — so
    // sizing the session by the highest occupied seat leaves the buffers above
    // the actor count owned by nobody, no frame ever advances, and a sparse
    // room (P1+P2+P4 with P3 empty) sits on a black screen. Seats stay sparse
    // at the PORT layer: g_GekkoPlayers keeps the seat count and
    // latch_gekko_input maps seat -> dense handle, zeroing the holes.
    config.num_players = static_cast<unsigned char>(numRemotes + 1);
    config.max_spectators = 0;
    config.input_prediction_window = static_cast<unsigned char>(clampedPredictionWindow);
    config.input_size = static_cast<unsigned int>(inputSize);
    config.state_size = kGekkoStateCapacity;
    config.limited_saving = false;
    config.desync_detection = true;
    config.check_distance = 10;
    gekko_start(g_GekkoSession, &config);

    g_GekkoUsingIceAdapter = useIce;
    g_GekkoUsingExternalSocket = !useIce && (g_GekkoExternalSocket.load() != kExtSocketNone);
#ifdef NETPLAY
    if (g_GekkoUsingIceAdapter)
    {
        LobbyIce::take_packets(LobbyIceChannel::Gekko);
        gekko_net_adapter_set(g_GekkoSession, &g_GekkoIceAdapter);
        write_gekko_log("start_lobby_session transport=libjuice_ice");
    }
    else
#endif
    if (g_GekkoUsingExternalSocket)
    {
        gekko_net_adapter_set(g_GekkoSession, &g_GekkoExternalAdapter);
        if (g_GekkoLogEnabled)
        {
            std::ostringstream stream;
            stream << "start_lobby_session transport=shared_anchor_socket local_port=" << localPort;
            write_gekko_log(stream.str());
        }
    }
    else
    {
        try
        {
            gekko_net_adapter_set(g_GekkoSession, gekko_default_adapter(localPort));
        }
        catch (const std::exception& e)
        {
            std::ostringstream stream;
            stream << "start_lobby_session result=fail reason=adapter local_port=" << localPort
                   << " error=" << e.what();
            write_gekko_log(stream.str());
            CoreSetError("GekkoNet adapter initialization failed: " + std::string(e.what()));
            close_session();
            return false;
        }
    }

    gekko_set_runahead(g_GekkoSession, 0);

    g_GekkoPlayers = players;
    g_GekkoActors = numRemotes + 1;
    g_GekkoInputSize = inputSize;
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLocalHandles.assign(static_cast<size_t>(players), -1);
    g_GekkoLatchedInput.assign(static_cast<size_t>(players * inputSize), 0);
    g_GekkoHasLatchedInput = false;
    g_GekkoPendingSaves.clear();

    if (g_GekkoLogEnabled)
    {
        std::ostringstream stream;
        stream << "start_lobby_session game=" << gameName
               << " seats=" << players
               << " actors=" << g_GekkoActors
               << " input_size=" << inputSize
               << " local_player=" << localPlayer
               << " local_port=" << localPort
               << " local_delay=" << localDelay
               << " clamped_local_delay=" << clampedLocalDelay
               << " prediction_window=" << predictionWindow
               << " clamped_prediction_window=" << clampedPredictionWindow
               << " transport=" << (useIce ? "libjuice_ice" :
                    (g_GekkoUsingExternalSocket ? "shared_anchor_socket" : "gekko_default_udp"))
               << " num_remotes=" << numRemotes;
        for (int i = 0; i < numRemotes; i++)
        {
            stream << " remote[" << i << "]=slot" << remotes[i].slot;
            if (remotes[i].userId != 0)
                stream << "@ice:" << remotes[i].userId;
            else
                stream << "@" << remotes[i].ip << ":" << remotes[i].port;
        }
        write_gekko_log(stream.str());
    }

    // Pre-build per-remote endpoint strings. These need to outlive the
    // gekko_add_actor calls because GekkoNetAddress.data is a raw pointer.
    std::vector<std::string> remoteAddrStrings(static_cast<size_t>(numRemotes));
    for (int i = 0; i < numRemotes; i++)
    {
        remoteAddrStrings[static_cast<size_t>(i)] = remotes[i].userId != 0
            ? "ice:" + std::to_string(remotes[i].userId)
            : remotes[i].ip + ":" + std::to_string(remotes[i].port);
    }

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
                stream << "gekko_add_actor result=ok player=" << player
                       << " type=local handle=" << handle
                       << " local_delay=" << clampedLocalDelay;
                write_gekko_log(stream.str());
            }
        }
        else
        {
            // Find the remote entry that owns this slot.
            int remoteIndex = -1;
            for (int i = 0; i < numRemotes; i++)
            {
                if (remotes[i].slot == player)
                {
                    remoteIndex = i;
                    break;
                }
            }
            if (remoteIndex < 0)
            {
                // Nobody owns this seat — an intentional gap, not a missing
                // endpoint. The caller already proved that: remote slots are
                // unique, in range, and none collides with ours, so the only
                // seats left unclaimed are the empty ones. Add no actor and
                // leave g_GekkoPlayerHandles[seat-1] == -1; latch_gekko_input
                // feeds that port zeroed input. Every peer walks seats in the
                // same ascending order, so the surviving actors get identical
                // handles on both ends.
                if (g_GekkoLogEnabled)
                {
                    std::ostringstream stream;
                    stream << "gekko_add_actor skipped player=" << player << " reason=empty_seat";
                    write_gekko_log(stream.str());
                }
                continue;
            }
            std::string& addrString = remoteAddrStrings[static_cast<size_t>(remoteIndex)];
            GekkoNetAddress address = {};
            address.data = addrString.data();
            address.size = static_cast<unsigned int>(addrString.size());
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
                       << " remote=" << addrString;
                write_gekko_log(stream.str());
            }
        }
    }

    if (g_GekkoLocalHandle < 0)
    {
        write_gekko_log("start_lobby_session result=fail reason=no_local_handle");
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
    g_GekkoActors = players; // local/stress seats are always contiguous 1..players
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

CORE_EXPORT void rmgk_gekko::set_external_socket(uintptr_t socketDescriptor)
{
#ifdef RMGK_HAVE_GEKKONET
    g_GekkoExternalSocket.store(static_cast<ExtSocket>(socketDescriptor));
#else
    (void)socketDescriptor;
#endif
}

CORE_EXPORT void rmgk_gekko::clear_external_socket()
{
#ifdef RMGK_HAVE_GEKKONET
    g_GekkoExternalSocket.store(kExtSocketNone);
#endif
}

CORE_EXPORT void rmgk_gekko::close_session()
{
#ifdef RMGK_HAVE_GEKKONET
    rmgk_pacing_trace_flush();
    g_GekkoStopRequested.store(false, std::memory_order_relaxed);
    clear_core_input_callback();
    if (g_GekkoSession != nullptr)
    {
        gekko_destroy(&g_GekkoSession);
        // The adopted anchor socket is borrowed — the lobby client owns and
        // keeps it; only the default adapter's own socket is ours to destroy.
        if (!g_GekkoUsingExternalSocket && !g_GekkoUsingIceAdapter)
        {
            gekko_default_adapter_destroy();
        }
    }
    g_GekkoUsingExternalSocket = false;
    g_GekkoUsingIceAdapter = false;
    g_GekkoExternalSocket.store(kExtSocketNone);
    g_GekkoExtResults.clear();
#ifdef NETPLAY
    g_GekkoIceResults.clear();
    LobbyIce::take_packets(LobbyIceChannel::Gekko);
#endif
    g_GekkoSession = nullptr;
    g_GekkoPlayers = 0;
    g_GekkoActors = 0;
    g_GekkoInputSize = 0;
    g_GekkoLocalPlayer = 0;
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.clear();
    g_GekkoLocalHandles.clear();
    {
        std::lock_guard<std::mutex> lock(g_GekkoDisconnectMutex);
        g_GekkoPendingDisconnectSlots.clear();
    }
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
    // Reset spectate keyframe capture state for the next session.
    g_GekkoKeyframeRequested.store(false, std::memory_order_relaxed);
    g_GekkoKeyframePending = false;
    g_GekkoKeyframePendingLiveFrame = -1;
    g_GekkoKeyframePendingBuf.clear();
    g_GekkoSpectateProbeStartFrame = -1;
    {
        std::lock_guard<std::mutex> lock(g_GekkoKeyframeReadyMutex);
        g_GekkoKeyframeReady = false;
        g_GekkoKeyframeReadyFrame = -1;
        g_GekkoKeyframeReadyBuf.clear();
    }
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

CORE_EXPORT void rmgk_gekko::request_disconnect_player(int slot)
{
#ifdef RMGK_HAVE_GEKKONET
    if (slot < 1 || slot > 4)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_GekkoDisconnectMutex);
    for (int queued : g_GekkoPendingDisconnectSlots)
    {
        if (queued == slot)
        {
            return; // already queued — de-dup
        }
    }
    g_GekkoPendingDisconnectSlots.push_back(slot);
#else
    (void)slot;
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


CORE_EXPORT void rmgk_gekko::pace_before_present()
{
#ifdef RMGK_HAVE_GEKKONET
    if (!g_RollbackPresentPacerEnabled ||
        g_GekkoSession == nullptr ||
        !g_GekkoExecuting.load(std::memory_order_relaxed))
    {
        return;
    }

    /*
     * The core publishes the exact nominal VI rate before the first bypassed
     * visible-frame limiter call. An explicit environment override wins.
     */
    if (!g_RollbackPresentBaseHzOverridden)
    {
        const char* effectiveHzEnv =
            std::getenv(
                "RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE");

        if (effectiveHzEnv != nullptr &&
            effectiveHzEnv[0] != '\0')
        {
            char* end = nullptr;
            const double value =
                std::strtod(effectiveHzEnv, &end);

            if (end != effectiveHzEnv &&
                value >= 30.0 &&
                value <= 240.0)
            {
                g_RollbackPresentBaseHz = value;
            }
        }
    }

    const double scale =
        std::clamp(g_GekkoSpeedScale, 0.99, 1.01);

    const double periodUs =
        1000000.0 /
        (g_RollbackPresentBaseHz * scale);

    auto now = std::chrono::steady_clock::now();

    if (!g_RollbackPresentPacerInitialized)
    {
        g_RollbackPresentPacerInitialized = true;
        g_RollbackPresentLastSwapTime = now;
        g_RollbackPresentTargetTime = now;

        record_rollback_present_pacing(
            1,
            1,
            0,
            scale,
            periodUs,
            0,
            0,
            0,
            0,
            0);
        return;
    }

    const auto period =
        std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<
                    double,
                    std::micro>(periodUs));

    const auto deadline =
        g_RollbackPresentTargetTime + period;

    const long long intervalBeforeWaitUs =
        std::chrono::duration_cast<
            std::chrono::microseconds>(
                now -
                g_RollbackPresentLastSwapTime).count();

    long long waitRequestedUs = 0;
    long long waitActualUs = 0;
    long long lateUs = 0;
    int deadlineMiss = 0;

    if (now < deadline)
    {
        const auto remainingNs =
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    deadline - now).count();

        /*
         * Round upward so microsecond conversion never intentionally
         * shortens the requested deadline.
         */
        waitRequestedUs =
            (remainingNs + 999) / 1000;

        const auto waitBegin =
            std::chrono::steady_clock::now();

        rmgk::timing::PreciseWaitFor(
            std::chrono::microseconds{
                waitRequestedUs},
            std::chrono::microseconds{
                std::min<long long>(
                    waitRequestedUs,
                    100)});

        now = std::chrono::steady_clock::now();

        waitActualUs =
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                    now - waitBegin).count();
    }
    else
    {
        deadlineMiss = 1;
        lateUs =
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                    now - deadline).count();
    }

    const long long intervalAfterWaitUs =
        std::chrono::duration_cast<
            std::chrono::microseconds>(
                now -
                g_RollbackPresentLastSwapTime).count();

    /*
     * Advance from the scheduled deadline rather than the actual late wake.
     * Normal wait overshoot is therefore removed from the next wait instead
     * of being permanently added to every frame interval.
     *
     * A stall which leaves us at least one complete period behind is rebased
     * to the current time. This prevents a long pause from causing a burst of
     * immediate catch-up swaps while still preserving phase across ordinary
     * sub-frame scheduler jitter.
     */
    if (now >= deadline + period)
    {
        g_RollbackPresentTargetTime = now;
    }
    else
    {
        g_RollbackPresentTargetTime = deadline;
    }

    g_RollbackPresentLastSwapTime = now;

    record_rollback_present_pacing(
        1,
        0,
        deadlineMiss,
        scale,
        periodUs,
        intervalBeforeWaitUs,
        waitRequestedUs,
        waitActualUs,
        lateUs,
        intervalAfterWaitUs);
#else
    return;
#endif
}

CORE_EXPORT void rmgk_gekko::trace_swap_duration(
    long long swapUs,
    long long makeCurrentUs,
    int path)
{
#ifdef RMGK_HAVE_GEKKONET
    if (!g_RmgkPacingTraceEnabled ||
        g_RmgkPacingTraceActiveRow ==
            kRmgkPacingTraceInvalidRow ||
        g_RmgkPacingTraceActiveRow >=
            g_RmgkPacingTraceRows.size())
    {
        return;
    }

    auto& row =
        g_RmgkPacingTraceRows[
            g_RmgkPacingTraceActiveRow];

    row.coreFrameSwap = CoreGetCurrentFrameCount();
    row.swapUs = swapUs;
    row.makeCurrentUs = makeCurrentUs;
    row.swapPath = path;
#else
    (void)swapUs;
    (void)makeCurrentUs;
    (void)path;
#endif
}

static void rollback_pace_before_present(void* userData)
{
    (void)userData;
    rmgk_gekko::pace_before_present();
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
    callbacks.pace_before_present = rollback_pace_before_present;
    callbacks.pacing_trace_enabled =
        g_RmgkPacingTraceEnabled ? 1 : 0;
    g_GekkoExecuting.store(true, std::memory_order_relaxed);
    bool result = CoreRollbackExecute(callbacks);
    g_GekkoExecuting.store(false, std::memory_order_relaxed);
    rmgk_pacing_trace_flush();
    g_RollbackPresentPacerInitialized = false;
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
                // Spectate input probe: log the confirmed krec input per frame (the exact
                // bytes a spectator must replay) so it can be diffed against the
                // spectator's applied input — revealing whether the boundary divergence is
                // an input misalignment or hidden state.
                if (g_GekkoSpectateProbeStartFrame >= 0 &&
                    it->first >= g_GekkoSpectateProbeStartFrame &&
                    it->first <= g_GekkoSpectateProbeStartFrame + kSpectateProbeWindow &&
                    g_GekkoInputSize >= 4)
                {
                    // Label by krec RECORD INDEX (not the GekkoNet frame) so it shares the
                    // spectator's coordinate system — the spectator counts records from 0.
                    // The record for this frame was just written above, so its 0-based index
                    // is recordingFrameCount() - 1.
                    const int hostRecordIdx = n02::recordingFrameCount() - 1;
                    std::ostringstream istream;
                    istream << "spectate_input side=host frame=" << hostRecordIdx
                            << " gekko=" << it->first;
                    const std::vector<unsigned char>& in = it->second;
                    const int players = static_cast<int>(in.size()) / g_GekkoInputSize;
                    for (int p = 0; p < players && p < 2; p++)
                    {
                        const size_t off = static_cast<size_t>(p) * g_GekkoInputSize;
                        char buf[32];
                        snprintf(buf, sizeof(buf), " p%d=%02x%02x%02x%02x", p,
                                 in[off], in[off + 1], in[off + 2], in[off + 3]);
                        istream << buf;
                    }
                    rmgk_gekko::write_spectate_probe(istream.str());
                }
                // Confirm a held keyframe once its frame has flushed to the krec: it's
                // now past the rollback horizon and survived any rollback (else the
                // Load handler would have discarded it), so the snapshot matches the
                // confirmed krec. Hand it to the UI thread to compress + upload.
                if (g_GekkoKeyframePending && it->first == g_GekkoKeyframePendingLiveFrame)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_GekkoKeyframeReadyMutex);
                        g_GekkoKeyframeReadyBuf = g_GekkoKeyframePendingBuf;
                        // The spectator restores at the krec RECORD INDEX, not the GekkoNet
                        // frame: the server (krecFrameOffset) and the spectator's frameIndex
                        // both count 0x12 records from 0, but recording starts ~N frames into
                        // the match (handshake/rollback-horizon warmup), so record_index =
                        // gekko_frame - N. recordingWriteInputs just wrote THIS frame's record
                        // (line above), so its 0-based index is recordingFrameCount() - 1.
                        // Sending the gekko frame instead shifted the spectator's inputs by N.
                        g_GekkoKeyframeReadyFrame =
                            n02::recordingFrameCount() - 1 + kKeyframeReplayFrameOffset;
                        g_GekkoKeyframeReady = true;
                    }
                    g_GekkoKeyframePending = false;
                    if (g_GekkoLogEnabled)
                    {
                        std::ostringstream stream;
                        stream << "keyframe confirmed live_frame=" << g_GekkoKeyframePendingLiveFrame
                               << " replay_frame=" << g_GekkoKeyframeReadyFrame
                               << " len=" << g_GekkoKeyframeReadyBuf.size();
                        write_gekko_log(stream.str());
                    }
                }
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

CORE_EXPORT void rmgk_gekko::request_keyframe()
{
    // Just arm the request; the emulation thread snapshots at its next save and
    // commits once confirmed (see save_gekko_state + the recording flush).
    g_GekkoKeyframeRequested.store(true, std::memory_order_relaxed);
}

CORE_EXPORT bool rmgk_gekko::take_keyframe(std::vector<unsigned char>& out, int& frame)
{
    std::lock_guard<std::mutex> lock(g_GekkoKeyframeReadyMutex);
    if (!g_GekkoKeyframeReady)
    {
        return false;
    }
    out = std::move(g_GekkoKeyframeReadyBuf);
    g_GekkoKeyframeReadyBuf.clear();
    frame = g_GekkoKeyframeReadyFrame;
    g_GekkoKeyframeReady = false;
    return true;
}

CORE_EXPORT uint64_t rmgk_gekko::hash_bytes(const unsigned char* data, size_t len)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    if (data != nullptr)
    {
        for (size_t i = 0; i < len; i++)
        {
            h ^= static_cast<uint64_t>(data[i]);
            h *= 1099511628211ull; // FNV-1a 64-bit prime
        }
    }
    return h;
}

CORE_EXPORT void rmgk_gekko::write_spectate_probe(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_GekkoLogMutex);
    get_gekko_log_path(); // side effect: ensures g_GekkoLogPrefix / g_GekkoLogDirectory are set
    const std::string filename = g_GekkoLogPrefix + "_spectate.log";
    const std::filesystem::path path = g_GekkoLogDirectory.empty()
        ? std::filesystem::path(filename)
        : g_GekkoLogDirectory / filename;
    std::ofstream file(path, std::ios::out | std::ios::app);
    file << message << "\n";
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
    if (g_GekkoSession == nullptr || g_GekkoLocalPlayer <= 1)
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
