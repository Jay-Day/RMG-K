/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_SCRIPTING_HOOKS_HPP
#define CORE_SCRIPTING_HOOKS_HPP

#include <cstdint>
#include "Library.hpp"

// Function-pointer table registered by RMG after both RMG-Core and RMG-Scripting
// are loaded. Keeps RMG-Core.dll free of any direct dependency on RMG-Scripting
// symbols, which would cause undefined-symbol errors on Windows at DLL link time.
struct ScriptingHooks {
    void (*onFrameUpdate)(uint32_t pc) = nullptr;
    void (*onPCUpdate)(uint32_t pc) = nullptr;
    void (*onEmulationStop)() = nullptr;
    void (*onEmulationPause)() = nullptr;
    void (*onEmulationResume)() = nullptr;
};

// Called by RMG once scripting is initialised (guarded by #ifdef SCRIPTING_ENABLED).
CORE_EXPORT void CoreSetScriptingHooks(const ScriptingHooks& hooks);

// Internal use by Emulation.cpp only — not exported from the DLL.
const ScriptingHooks& CoreGetScriptingHooks();

#endif // CORE_SCRIPTING_HOOKS_HPP
