/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_GEKKO_HPP
#define RMGK_GEKKO_HPP

#include "Emulation.hpp"
#include "RollbackNetcode.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Describes one remote peer in a lobby-driven session. Each non-local
// slot needs its own endpoint — GekkoNet's gekko_add_actor takes a
// per-actor address, and in a 3-/4-player room every remote talks to
// a different peer.
struct LobbyRemotePeer
{
    int slot = 0;              // 1-indexed N64 controller slot
    std::uint64_t userId = 0;  // lobby identity used by the ICE adapter
    std::string ip;            // IPv4 dotted quad
    unsigned short port = 0;
};

// Where a late join's cut stands on this client (see rmgk_gekko::arm_admit_cut).
enum class AdmitCutStatus
{
    None,    // no cut armed
    Armed,   // still playing toward the cut frame
    Reached, // stopped at the cut; every frame before it is final
    Passed,  // the session was already past the cut frame when it was armed
};

class rmgk_gekko
{
  public:
    using InputProvider = bool (*)(uint32_t* inputs, int players, void* userData);
    using FrameCallback = bool (*)(void* userData);

    static bool start_p2p_session(const char* gameName, int players, int inputSize,
        int localPlayer, unsigned short localPort, const char* remoteIp, unsigned short remotePort, int localDelay, int predictionWindow);
    // Lobby variant: uses the room's libjuice ICE mesh. It remains separate
    // from the n02/connect-code transport and takes one lobby identity per
    // remote controller slot. `players` is the number of controller ports the
    // match emulates. A late joiner passes the session's epoch as joinEpoch:
    // its emulation then runs one boot frame and holds until
    // provide_join_state() hands it the state to start from.
    static bool start_lobby_session(const char* gameName, int players, int inputSize,
        int localPlayer, unsigned short localPort,
        const LobbyRemotePeer* remotes, int numRemotes,
        int localDelay, int predictionWindow, int joinEpoch = -1);
    static bool start_local_session(const char* gameName, int players, int inputSize, int localDelay);
    // n02-style shared transport: adopt an already-bound UDP socket (the
    // lobby's anchor) for the next lobby session instead of binding our own.
    // The socket is borrowed, never closed by us; close_session clears the
    // adoption. When no socket is adopted, start_lobby_session falls back to
    // GekkoNet's own adapter binding localPort (the old handoff model).
    static void set_external_socket(uintptr_t socketDescriptor);
    static void clear_external_socket();
    static void close_session();
    static void request_stop();
    // Thread-safe: queue a remote slot (1-indexed N64 controller) to be force-
    // disconnected from the rollback session. The request is drained on the
    // emulation thread at the start of the next frame, so GekkoNet drops the
    // actor immediately instead of waiting out its 5 s silence timeout. Safe to
    // call from any thread (e.g. the lobby UI when the server reports a drop);
    // a no-op if there's no active session or the slot isn't a remote.
    static void request_disconnect_player(int slot);
    // Thread-safe: the input delay this player should use in the running
    // rollback session, or -1 for none. The emulation thread notices a new value
    // at the start of the next frame and steps the delay toward it one frame at
    // a time, so the input stream every peer receives stays sequential. The
    // value outlives session starts, so one chosen while a match is still
    // starting applies once it runs; the caller clears it when its match ends.
    static void set_live_local_delay(int delay);
    // Thread-safe: likewise for the prediction window, which applies at the
    // start of the next frame. It only limits how far this client predicts
    // ahead, so peers may run with different windows.
    static void set_live_prediction_window(int predictionWindow);
    static bool is_netplay_session_active();

    // --- Late join ---
    // A late join moves the match to a new session that includes the joiner.
    // Every player stops on the same frame (the cut), where the frames before
    // it are final everywhere, then continues from the state it already has
    // in the new session; the joiner loads the host's copy of that state.
    //
    // Thread-safe: the frame the session advances next (-1 without one), and
    // which of the match's sessions that is (0 for the first, -1 without one).
    // A caller waiting for a new session to run checks both: the frame
    // resets before the epoch changes.
    static int current_frame();
    static int session_epoch();
    // Thread-safe: stop at `frame`. With captureState (the host), a full
    // savestate of the cut is kept for take_admit_state().
    static void arm_admit_cut(int frame, bool captureState);
    static AdmitCutStatus admit_cut_status();
    // Thread-safe: the state captured at the cut, once Reached.
    static bool take_admit_state(std::vector<unsigned char>& out);
    // Thread-safe: leave the cut for a new session with these remote players
    // on ICE channel `epoch`. Ignored until the cut is Reached.
    static void release_admit_cut(const std::vector<LobbyRemotePeer>& remotes, int epoch);
    // Thread-safe: drop the cut and carry on in the current session.
    static void cancel_admit_cut();
    // Thread-safe (joiner): true while booted and waiting for its state.
    static bool join_holding();
    // Thread-safe (joiner): the state to start from; the held session loads
    // it and connects to the players.
    static void provide_join_state(std::vector<unsigned char> state);
    static bool execute();
    // Rollback presentation pacer. The core invokes this through the video
    // plugin rendering callback immediately before OpenGL or Vulkan presents.
    static void pace_before_present();

    // Buffered rollback-pacing trace hook used by VidExt.cpp.
    // No-op unless Rollback -> Logging -> Pacing Trace is enabled.
    static void trace_swap_duration(long long swapUs, long long makeCurrentUs, int path);
    static bool set_deterministic(bool enabled);
    static bool install_core_input_callback();
    static void clear_core_input_callback();
    static bool synchronize_input(void* values, int size, int players);
    static void set_debug_hooks(InputProvider inputProvider, FrameCallback beginFrame, FrameCallback endFrame, void* userData);
    static void set_debug_frame_output(int flags);
    static bool debug_run_frame_with_inputs(const uint32_t* inputs, int players, int flags);
    static bool toggle_client_input_replay();

    // --- Spectate keyframe capture (broadcaster side) ---
    // Ask the engine to capture a savestate "keyframe" for the broadcast/spectate
    // feature. Thread-safe; the snapshot is taken on the emulation thread for the
    // current live frame, then HELD until the recording confirms that frame (flushes
    // it to the krec, past the rollback horizon) with no rollback having touched it
    // since — so the savestate is guaranteed to match the confirmed krec a spectator
    // will replay from. A no-op if there's no active session.
    static void request_keyframe();
    // Thread-safe (call from the UI thread): if a confirmed keyframe is ready, move
    // its raw savestate bytes into out, set frame to the first krec record index to
    // consume after restore, and return true (clearing the ready slot).
    // Returns false if none is ready.
    static bool take_keyframe(std::vector<unsigned char>& out, int& frame);

    // --- Spectate keyframe divergence probe (diagnostic) ---
    // FNV-1a 64-bit content hash over raw bytes — a real fingerprint of a rollback
    // state buffer. The core's own rollback "checksum" field is a stub (always 0), so
    // we hash the bytes ourselves. Shared so the broadcaster and spectator hash
    // identically; comparing the two pinpoints where a spectate replay diverges.
    static uint64_t hash_bytes(const unsigned char* data, size_t len);
    // Append one line to <prefix>_spectate.log (a sibling of the GekkoNet log),
    // UNCONDITIONALLY (independent of the verbose-log toggle, since the spectator is
    // never in a GekkoNet session). Thread-safe.
    static void write_spectate_probe(const std::string& message);
};

#endif // RMGK_GEKKO_HPP
