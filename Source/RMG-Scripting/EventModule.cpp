/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "EventModule.hpp"
#include "../RMG-Core/Emulation.hpp"
#include "../RMG-Core/Library.hpp"
#include "../RMG-Core/m64p/Handle.hpp"
#include "../RMG-Core/m64p/api/m64p_common.h"
#include "../RMG-Core/m64p/api/m64p_types.h"
#include "ScriptEngine.hpp"

#include <lua.hpp>

#include <cstring>

namespace RMGScript {

static constexpr const char* kRegistryScriptEngineKey = "RMG_SCRIPT_ENGINE";

static ScriptEngine* GetEngine(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryScriptEngineKey);
    auto* engine = static_cast<ScriptEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return engine;
}

template <typename T>
static T LoadCoreSymbol(const char* name) {
    CoreLibraryHandle coreHandle = CoreGetM64PCoreHandle();
    if (!coreHandle) return nullptr;
    return reinterpret_cast<T>(CoreGetLibrarySymbol(coreHandle, name));
}

static bool CoreDebuggerSupported() {
    static bool resolved = false;
    static bool supported = false;
    if (resolved) {
        return supported;
    }

    CoreLibraryHandle coreHandle = CoreGetM64PCoreHandle();
    if (!coreHandle) {
        resolved = true;
        supported = false;
        return supported;
    }

    auto getApiVersions = reinterpret_cast<ptr_CoreGetAPIVersions>(
        CoreGetLibrarySymbol(coreHandle, "CoreGetAPIVersions"));
    if (!getApiVersions) {
        resolved = true;
        supported = false;
        return supported;
    }

    int config = 0;
    int debug = 0;
    int vidext = 0;
    int extra = 0;
    if (getApiVersions(&config, &debug, &vidext, &extra) == M64ERR_SUCCESS) {
        supported = (extra & M64CAPS_DEBUGGER) != 0;
    }

    resolved = true;
    return supported;
}

static uint32_t DebugGetPC() {
    if (!CoreDebuggerSupported()) {
        return 0;
    }

    using ptr_DebugGetCPUDataPtr = void* (*)(m64p_dbg_cpu_data);
    static ptr_DebugGetCPUDataPtr getPtr = nullptr;
    static bool resolved = false;
    if (!resolved) {
        getPtr = LoadCoreSymbol<ptr_DebugGetCPUDataPtr>("DebugGetCPUDataPtr");
        resolved = true;
    }
    if (!getPtr) return 0;
    void* pcPtr = getPtr(M64P_CPU_PC);
    if (!pcPtr) return 0;
    return *static_cast<uint32_t*>(pcPtr);
}

// Forward declare the event functions to register
static const luaL_Reg emuLib[] = {
    {"on_frame", EventModule::OnFrame},
    {"on_breakpoint", EventModule::OnBreakpoint},
    {"on_stop", EventModule::OnStop},
    {"on_pause", EventModule::OnPause},
    {"on_resume", EventModule::OnResume},
    {"get_pc", EventModule::GetPC},
    {"is_paused", EventModule::IsPaused},
    {"is_running", EventModule::IsRunning},
    {"get_address", EventModule::GetAddress},
    {NULL, NULL}
};

void RegisterEventModule(lua_State* L) {
    // Create emu table
    lua_newtable(L);
    
    // Register all event functions
    luaL_setfuncs(L, emuLib, 0);
    
    // Set as global "emu"
    lua_setglobal(L, "emu");
}

int EventModule::OnFrame(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    ScriptEngine* engine = GetEngine(L);
    if (!engine) {
        return luaL_error(L, "script engine unavailable");
    }

    // Store function in registry and register it
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    engine->RegisterLuaFrameCallback(ref);
    return 0;
}

int EventModule::OnBreakpoint(lua_State* L) {
    // Register a breakpoint callback
    uint32_t address = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    ScriptEngine* engine = GetEngine(L);
    if (!engine) {
        return luaL_error(L, "script engine unavailable");
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    engine->RegisterLuaBreakpointCallback(address, ref);
    return 0;
}

int EventModule::OnStop(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    // TODO: Register stop callback
    return 0;
}

int EventModule::OnPause(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    // TODO: Register pause callback
    return 0;
}

int EventModule::OnResume(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    // TODO: Register resume callback
    return 0;
}

int EventModule::GetPC(lua_State* L) {
    // Get current program counter
    uint32_t pc = DebugGetPC();
    lua_pushinteger(L, pc);
    return 1;
}

int EventModule::IsPaused(lua_State* L) {
    bool paused = CoreIsEmulationPaused();
    lua_pushboolean(L, paused);
    return 1;
}

int EventModule::IsRunning(lua_State* L) {
    bool running = CoreIsEmulationRunning();
    lua_pushboolean(L, running);
    return 1;
}

int EventModule::GetAddress(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    
    // Map of memory region names to addresses
    if (strcmp(name, "rdram") == 0) {
        lua_pushinteger(L, 0x80000000);
    } else if (strcmp(name, "SP_DMEM") == 0) {
        lua_pushinteger(L, 0xA4000000);
    } else if (strcmp(name, "PIF_RAM") == 0) {
        lua_pushinteger(L, 0xBFC007C0);
    } else if (strcmp(name, "MI_register") == 0) {
        lua_pushinteger(L, 0xA4300000);
    } else if (strcmp(name, "VI_register") == 0) {
        lua_pushinteger(L, 0xA4400000);
    } else if (strcmp(name, "AI_register") == 0) {
        lua_pushinteger(L, 0xA4500000);
    } else if (strcmp(name, "PI_register") == 0) {
        lua_pushinteger(L, 0xA4600000);
    } else if (strcmp(name, "RI_register") == 0) {
        lua_pushinteger(L, 0xA4700000);
    } else if (strcmp(name, "SI_register") == 0) {
        lua_pushinteger(L, 0xA4800000);
    } else {
        luaL_error(L, "Unknown memory region: %s", name);
    }
    
    return 1;
}

} // namespace RMGScript
