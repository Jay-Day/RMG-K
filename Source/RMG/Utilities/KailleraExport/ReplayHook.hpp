#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace KailleraExport
{

// Isolation boundary between EmulatorProxy.hpp's self-contained, minimal
// mupen64plus C API type definitions and RMG-Core's real ones
// (RMG-Core/m64p/Api.hpp, which transitively pulls in mupen64plus-core's
// actual m64p_types.h). Both declare the same enum names in the global
// namespace, so a single translation unit can never include both - this
// file's declarations only use void*/bool, so ReplayFileExport.cpp (which
// needs EmulatorProxy.hpp) can call through to RMG-Core's Replay::/
// m64p::Core without ever seeing its real headers. The .cpp behind this
// does the reverse: it only ever includes the real RMG-Core headers, never
// EmulatorProxy.hpp.
//
// `coreLibraryHandle` is really an m64p_dynlib_handle (HMODULE on Windows,
// void* elsewhere) - passed as void* for the same reason.

// Resolves RMG-Core's m64p::Core function pointers (Replay::/ReplayMemory
// read emulated memory through these) against the given core library
// handle, without re-initializing or re-starting anything the caller's own
// EmulatorProxy already set up. Returns false (and does nothing) if this
// build wasn't compiled with GAME_STATS.
bool HookReplayCoreApi(void* coreLibraryHandle);
bool IsReplayCoreApiHooked();

// Thin pass-throughs to Replay::{SetEnabledOverride,SetOutputPathOverride,
// SetPlayerNamesOverride,SetRecordedAtBaseOverride,OnEmulationStart,OnFrame,
// OnEmulationStop} (see RMG-Core/Replay.hpp for what each does). No-ops if
// this build wasn't compiled with GAME_STATS.
void ReplaySetEnabledOverride(bool enabled);
void ReplaySetOutputPathOverride(const std::string& path);
void ReplaySetPlayerNamesOverride(const std::array<std::string, 4>& names);
// `frameIndexProvider` is really a Replay::FrameIndexProvider (int(*)(void))
// - taken as a plain function pointer here for the same reason every other
// type in this file is primitive, see the isolation-boundary comment above.
void ReplaySetRecordedAtBaseOverride(uint64_t krecBaseEpochSeconds, int (*frameIndexProvider)(void));
void ReplayOnEmulationStart();
void ReplayOnFrame();
void ReplayOnEmulationStop();

} // namespace KailleraExport
