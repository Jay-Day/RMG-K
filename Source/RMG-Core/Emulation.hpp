/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_EMULATION_HPP
#define CORE_EMULATION_HPP

#include <filesystem>

enum class CoreEmulationState
{
    Stopped = 1,
    Running,
    Paused
};

// starts emulation with given ROM
bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom, std::string address = "", int port = -1, int player = -1);

// stops emulation
bool CoreStopEmulation(void);

// pauses emulation
bool CorePauseEmulation(void);

// resumes emulation
bool CoreResumeEmulation(void);

// advances emulation by one frame (must be paused)
bool CoreAdvanceFrame(void);

// advances emulation by several frames
bool CoreAdvanceFrames(int frames);

enum CoreFrameOutputFlags
{
    CoreFrameOutput_None   = 0,
    CoreFrameOutput_Video  = 1 << 0,
    CoreFrameOutput_Audio  = 1 << 1,
    CoreFrameOutput_Pacing = 1 << 2,
    CoreFrameOutput_Input  = 1 << 3,
    CoreFrameOutput_All    = CoreFrameOutput_Video | CoreFrameOutput_Audio | CoreFrameOutput_Pacing | CoreFrameOutput_Input
};

// configures frame side effects used by normal, hidden, and rollback frames
bool CoreSetFrameOutput(int flags);

// advances emulation by several frames using per-call frame side effects
bool CoreRunFrames(int frames, int flags);

// resets emulation
bool CoreResetEmulation(bool hard);

// returns whether emulation is running
bool CoreIsEmulationRunning(void);

// returns whether emulation is paused
bool CoreIsEmulationPaused(void);

// returns whether synchronized netplay is active
bool CoreIsSynchronizedNetplayActive(void);

// returns current VI (vertical interrupt) frame count
// used for synchronization in netplay/Kaillera
int CoreGetCurrentFrameCount(void);

// returns controller state captured from the last PIF read for port 0-3.
// rx receives [buttons_hi, buttons_lo, x_axis, y_axis] when valid is true.
bool CoreGetControllerState(int port, bool& connected, bool& valid, uint8_t (&rx)[4]);

#endif // CORE_EMULATION_HPP
