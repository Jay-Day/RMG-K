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
#include "ScriptEngine.hpp"
#include "../RMG-Core/Emulation.hpp"
#include "../RMG-Core/Library.hpp"
#include "../RMG-Core/m64p/Handle.hpp"
#include "../RMG-Core/m64p/api/m64p_types.h"

#include <quickjs.h>

#include <cstring>
#include <cstdint>

namespace RMGScript {

// ─── helpers ─────────────────────────────────────────────────────────────────

template <typename T>
static T LoadCoreSymbol(const char* name)
{
    CoreLibraryHandle h = CoreGetM64PCoreHandle();
    if (!h) return nullptr;
    return reinterpret_cast<T>(CoreGetLibrarySymbol(h, name));
}

static uint32_t GetCurrentPC()
{
    using FnPtr = void* (*)(m64p_dbg_cpu_data);
    static FnPtr fn = nullptr;
    if (!fn) fn = LoadCoreSymbol<FnPtr>("DebugGetCPUDataPtr");
    if (!fn) return 0;
    void* p = fn(M64P_CPU_PC);
    return p ? *static_cast<uint32_t*>(p) : 0;
}

// ─── emu.on_frame(callback) ──────────────────────────────────────────────────

static JSValue Emu_OnFrame(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "on_frame(callback): expected a function");

    ScriptEngine* engine = GetEngineFromContext(ctx);
    if (!engine)
        return JS_ThrowInternalError(ctx, "script engine unavailable");

    // Duplicate the value so the engine owns its own reference.
    engine->RegisterJSFrameCallback(JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

// ─── emu.on_breakpoint(addr, callback) ───────────────────────────────────────

static JSValue Emu_OnBreakpoint(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "on_breakpoint(addr, callback): expected (number, function)");

    uint32_t addr;
    if (JS_ToUint32(ctx, &addr, argv[0]) != 0) return JS_EXCEPTION;

    ScriptEngine* engine = GetEngineFromContext(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "script engine unavailable");

    engine->RegisterJSBreakpointCallback(addr, JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}

// ─── emu.getPC() ─────────────────────────────────────────────────────────────

static JSValue Emu_GetPC(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewUint32(ctx, GetCurrentPC());
}

// ─── emu.isPaused() / emu.isRunning() ────────────────────────────────────────

static JSValue Emu_IsPaused(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewBool(ctx, CoreIsEmulationPaused());
}

static JSValue Emu_IsRunning(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewBool(ctx, CoreIsEmulationRunning());
}

// ─── Registration ─────────────────────────────────────────────────────────────

void RegisterEventModule(JSContext* ctx)
{
    JSValue emu = JS_NewObject(ctx);

#define SET(name, fn, nargs) \
    JS_SetPropertyStr(ctx, emu, name, JS_NewCFunction(ctx, fn, name, nargs))

    SET("on_frame",      Emu_OnFrame,      1);
    SET("on_breakpoint", Emu_OnBreakpoint, 2);
    SET("getPC",         Emu_GetPC,        0);
    SET("isPaused",      Emu_IsPaused,     0);
    SET("isRunning",     Emu_IsRunning,    0);

#undef SET

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "emu", emu);
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
