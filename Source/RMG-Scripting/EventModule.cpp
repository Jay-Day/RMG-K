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
#include "../RMG-Core/Settings.hpp"
#include "../RMG-Core/m64p/Handle.hpp"
#include "../RMG-Core/m64p/api/m64p_types.h"

#include <quickjs.h>

#include <cstdio>
#include <cstring>
#include <string>
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

static void* GetCPUData(m64p_dbg_cpu_data type)
{
    using FnPtr = void* (*)(m64p_dbg_cpu_data);
    static FnPtr fn = nullptr;
    if (!fn) fn = LoadCoreSymbol<FnPtr>("DebugGetCPUDataPtr");
    return fn ? fn(type) : nullptr;
}

uint32_t GetCurrentPC()
{
    void* p = GetCPUData(M64P_CPU_PC);
    return p ? *static_cast<uint32_t*>(p) : 0;
}

// ─── GPR helpers ─────────────────────────────────────────────────────────────

static const char* const kGPRNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

static int ResolveGPRIndex(JSContext* ctx, JSValue v)
{
    if (JS_IsNumber(v)) {
        uint32_t n;
        if (JS_ToUint32(ctx, &n, v) != 0) return -1;
        return (n < 32) ? static_cast<int>(n) : -1;
    }
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (!s) return -1;
        int idx = -1;
        for (int i = 0; i < 32; i++) {
            if (strcmp(s, kGPRNames[i]) == 0) { idx = i; break; }
        }
        JS_FreeCString(ctx, s);
        return idx;
    }
    return -1;
}

// ─── emu.getEPC() ────────────────────────────────────────────────────────────

static JSValue Emu_GetEPC(JSContext* ctx, JSValue, int, JSValue*)
{
    uint32_t* cp0 = static_cast<uint32_t*>(GetCPUData(M64P_CPU_REG_COP0));
    return JS_NewUint32(ctx, cp0 ? cp0[14] : 0);
}

// ─── emu.getReg(n) ───────────────────────────────────────────────────────────

static JSValue Emu_GetReg(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getReg(index_or_name)");
    int idx = ResolveGPRIndex(ctx, argv[0]);
    if (idx < 0)
        return JS_ThrowRangeError(ctx, "getReg: use 0-31 or a name like 'a0', 't0', 'ra'");
    int64_t* regs = static_cast<int64_t*>(GetCPUData(M64P_CPU_REG_REG));
    if (!regs)
        return JS_ThrowTypeError(ctx, "registers not available (emulation not running)");
    return JS_NewUint32(ctx, static_cast<uint32_t>(regs[idx]));
}

// ─── emu.getRegs() ───────────────────────────────────────────────────────────

static JSValue Emu_GetRegs(JSContext* ctx, JSValue, int, JSValue*)
{
    int64_t* regs = static_cast<int64_t*>(GetCPUData(M64P_CPU_REG_REG));
    if (!regs)
        return JS_ThrowTypeError(ctx, "registers not available (emulation not running)");
    JSValue obj = JS_NewObject(ctx);
    for (int i = 0; i < 32; i++)
        JS_SetPropertyStr(ctx, obj, kGPRNames[i], JS_NewUint32(ctx, static_cast<uint32_t>(regs[i])));
    return obj;
}

// ─── emu.getHI() / emu.getLO() ───────────────────────────────────────────────

static JSValue Emu_GetHI(JSContext* ctx, JSValue, int, JSValue*)
{
    int64_t* p = static_cast<int64_t*>(GetCPUData(M64P_CPU_REG_HI));
    if (!p) return JS_ThrowTypeError(ctx, "registers not available (emulation not running)");
    return JS_NewUint32(ctx, static_cast<uint32_t>(*p));
}

static JSValue Emu_GetLO(JSContext* ctx, JSValue, int, JSValue*)
{
    int64_t* p = static_cast<int64_t*>(GetCPUData(M64P_CPU_REG_LO));
    if (!p) return JS_ThrowTypeError(ctx, "registers not available (emulation not running)");
    return JS_NewUint32(ctx, static_cast<uint32_t>(*p));
}

// ─── emu.getFReg(n) ──────────────────────────────────────────────────────────

static JSValue Emu_GetFReg(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "getFReg(index)");
    uint32_t n;
    if (JS_ToUint32(ctx, &n, argv[0]) != 0) return JS_EXCEPTION;
    if (n >= 32) return JS_ThrowRangeError(ctx, "getFReg: index must be 0-31");
    float** ptrs = static_cast<float**>(GetCPUData(M64P_CPU_REG_COP1_SIMPLE_PTR));
    if (!ptrs || !ptrs[n])
        return JS_ThrowTypeError(ctx, "FP registers not available (emulation not running)");
    return JS_NewFloat64(ctx, static_cast<double>(*ptrs[n]));
}

// ─── emu.getFRegs() ──────────────────────────────────────────────────────────

static JSValue Emu_GetFRegs(JSContext* ctx, JSValue, int, JSValue*)
{
    float** ptrs = static_cast<float**>(GetCPUData(M64P_CPU_REG_COP1_SIMPLE_PTR));
    if (!ptrs) return JS_ThrowTypeError(ctx, "FP registers not available (emulation not running)");
    JSValue obj = JS_NewObject(ctx);
    for (int i = 0; i < 32; i++) {
        char name[4];
        snprintf(name, sizeof(name), "f%d", i);
        float val = ptrs[i] ? *ptrs[i] : 0.0f;
        JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, static_cast<double>(val)));
    }
    return obj;
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

// ─── emu.on_pc(addr, callback) ───────────────────────────────────────────────

static JSValue Emu_OnBreakpoint(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "on_pc(addr, callback): expected (number, function)");

    if (!CoreAreOnPCHooksSupported()) {
        int cpuEmulator = CoreSettingsGetIntValue(SettingsID::Core_CPU_Emulator);
        std::string modeName = (cpuEmulator == 0) ? "Pure Interpreter" :
                               (cpuEmulator == 1) ? "Cached Interpreter" : "Dynamic Recompiler";
        std::string errMsg = std::string("on_pc() requires interpreter mode (Pure Interpreter or Cached Interpreter). "
            "Current mode is '") + modeName + "' which doesn't support per-instruction hooks.";
        return JS_ThrowRangeError(ctx, errMsg.c_str());
    }

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
    SET("on_pc",         Emu_OnBreakpoint, 2);
    SET("getPC",         Emu_GetPC,        0);
    SET("getEPC",        Emu_GetEPC,       0);
    SET("isPaused",      Emu_IsPaused,     0);
    SET("isRunning",     Emu_IsRunning,    0);
    SET("getReg",        Emu_GetReg,       1);
    SET("getRegs",       Emu_GetRegs,      0);
    SET("getHI",         Emu_GetHI,        0);
    SET("getLO",         Emu_GetLO,        0);
    SET("getFReg",       Emu_GetFReg,      1);
    SET("getFRegs",      Emu_GetFRegs,     0);

#undef SET

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "emu", emu);
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
