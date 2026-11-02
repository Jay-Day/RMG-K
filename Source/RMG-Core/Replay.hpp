/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_HPP
#define REPLAY_HPP

#include <array>
#include <cstdint>
#include <string>

// New, independent per-match ".rmgr" replay recorder. Entirely separate
// from Source/n02's .krec format - does not read, write, or otherwise
// touch anything under Source/n02/.
//
// Thread safety: these three functions can be called from more than one
// thread (OnFrame() from the emulation thread; OnEmulationStart()/
// OnEmulationStop() from whichever thread starts/stops emulation, which is
// usually the UI thread but not always - e.g. CoreStopEmulation() can also
// run from the emulation thread on some Kaillera playback paths). All three
// are internally synchronized (a mutex is held for the duration of each
// call), so callers don't need any of their own locking.
namespace Replay
{
// Call once, right after CoreStartEmulation registers the frame callback.
// Determines whether to arm the per-match state machine driven by
// OnFrame(): uses SetEnabledOverride()'s value if one was set since the
// last call (consumed exactly once - see SetEnabledOverride's own comment),
// otherwise falls back to SettingsID::GameStats_ReplayEnabled. Also
// defensively closes any file left open from an abnormal prior session end.
void OnEmulationStart(void);

// Per-launch override for whether replay recording is enabled, distinct
// from (and never persisted to) the SettingsID::GameStats_ReplayEnabled
// default - mirrors n02's n02_kaillera_recording_enabled pattern for
// krec's own "Record game" checkboxes (Source/n02/kailleraclient.h), one
// independent checkbox per netplay/playback dialog rather than a single
// shared global. Call this from a dialog's own "Record replay" checkbox
// right before triggering its launch, the same way those dialogs
// re-assert n02_kaillera_recording_enabled right before their own launch.
//
// The override is consumed (cleared) the next time OnEmulationStart()
// runs, so a launch path that never calls this (e.g. a plain offline ROM
// launch, which has no per-launch checkbox to hang this off of) always
// falls through to the persisted Settings default, and a stale override
// from a previous session can never leak into an unrelated later launch.
void SetEnabledOverride(bool enabled);

// Per-session override for the output file's base path, bypassing the
// default "replays/<timestamp>[-players].rmgr" naming (see BuildFileName()
// in Replay.cpp) entirely. For headless/offline export tooling that needs
// a predictable, caller-controlled destination rather than the
// live-recording convention - pass e.g. "replays/<krec name>.rmgr" (the
// same stem as the source .krec, so the export plainly corresponds back
// to it) and each match gets its own explicitly-numbered file from that
// base: "<krec name>-1.rmgr", "<krec name>-2.rmgr", ... (not just
// "<krec name>.rmgr" for the first) - a single headless export of a
// multi-game .krec produces one .rmgr per match. FindCollisionFreePath()
// in Replay.cpp is still applied on top as a safety net (in case this
// exact numbered name is already taken, e.g. the same .krec was already
// exported once before), so the caller does not need its own collision
// handling - just pass the same desired base path every time.
//
// Cleared at OnEmulationStop(), so a launch path that never calls this
// always falls through to the default naming, and a stale override can
// never leak into an unrelated later session.
void SetOutputPathOverride(const std::string& path);

// Per-session override for the GameStart event's playerNames field,
// indexed by port. For headless/offline export tooling that has no live
// Kaillera/rollback session to read n02's recording_player_names global
// from (that global is only ever populated by the in-app netplay/lobby
// UI code) - the .krec being replayed already has its own player names
// stored in its header, and the caller passes those through here.
//
// Each name is truncated to the field's storage width the same way every
// other fixed-width string in this format is (see WriteFixedString) - no
// other escaping is needed since this is a fixed-size binary field, not a
// delimited one.
//
// Applies to every match recorded for the rest of this session, the same
// way and for the same reason as SetOutputPathOverride() - a multi-game
// .krec's later matches are the same two players as its first. Cleared at
// OnEmulationStop(): a launch path that never calls this always falls
// through to recording_player_names, and a stale override can never leak
// into an unrelated later session.
void SetPlayerNamesOverride(const std::array<std::string, 4>& names);

// Per-session override for how OpenNewFile() derives recordedAtEpochMillis,
// for headless/offline .krec export tooling. Without this, OpenNewFile()
// stamps every match with the wall-clock time it reaches it - which, since
// headless replay runs at up to 2000% speed (see ReplayFileExport.cpp),
// reflects when the *export* happened to reach that match, not when it was
// originally played, and drifts further from the truth with every match in
// a multi-game .krec.
//
// `krecBaseEpochSeconds` is the source .krec's own recording-start
// timestamp (its header's timestamp field, or a filename-derived fallback -
// see KailleraExport::ParseKrecFile) - still whole seconds, since that's
// all the .krec format itself stores. `frameIndexProvider` is called fresh
// every time OpenNewFile() runs for the rest of this session; its return
// value is how many of the .krec's own input frames have been consumed so
// far (see KailleraExport::GetPifReplayFrameIndex()) - assuming the
// original recording ran at a constant 60fps, converting that to
// milliseconds (elapsedFrames * 1000 / 60, rounded to the nearest
// millisecond) gives a frame-accurate offset from krecBaseEpochSeconds,
// which OpenNewFile() adds (after converting the base to milliseconds) to
// derive that specific match's recordedAtEpochMillis. A null
// frameIndexProvider is treated as "0 frames elapsed" (i.e. every match
// gets krecBaseEpochSeconds itself, converted to milliseconds).
//
// Cleared at OnEmulationStop(), same as the two overrides above - a launch
// path that never calls this always falls through to time(nullptr), and a
// stale override can never leak into an unrelated later session.
using FrameIndexProvider = int (*)(void);
void SetRecordedAtBaseOverride(uint64_t krecBaseEpochSeconds, FrameIndexProvider frameIndexProvider);

// Call from CoreStopEmulation, and also from any UI-thread path that ends
// emulation without necessarily going through CoreStopEmulation (see
// MainWindow::on_Emulation_Finished). Finalizes (patches the length field
// and closes) any file still open, e.g. if the user quit mid-match. Safe
// to call when nothing is open/recording.
void OnEmulationStop(void);

// Call once per real emulated frame (i.e. from the same place krec's
// frame-counter hook lives) - never during GekkoNet rollback resimulation.
// Owns the whole "waiting for a match to start" / "recording" state
// machine; safe to call every frame regardless of current state.
void OnFrame(void);
} // namespace Replay

#endif // REPLAY_HPP
