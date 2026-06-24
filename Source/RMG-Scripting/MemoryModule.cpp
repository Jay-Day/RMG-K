/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "MemoryModule.hpp"
#include "../RMG-Core/Library.hpp"
#include "../RMG-Core/m64p/Handle.hpp"
#include "../RMG-Core/m64p/api/m64p_types.h"

#include <quickjs.h>

#include <cstring>
#include <cstdint>

namespace RMGScript {

// ─── RDRAM access helpers ────────────────────────────────────────────────────

template <typename T>
static T LoadCoreSymbol(const char* name)
{
    CoreLibraryHandle h = CoreGetM64PCoreHandle();
    if (!h) return nullptr;
    return reinterpret_cast<T>(CoreGetLibrarySymbol(h, name));
}

uint8_t* GetRDRAM()
{
    // DebugMemGetPointer is exported unconditionally (no DBG guard in core source).
    using Fn = void* (*)(m64p_dbg_memptr_type);
    static Fn getPtr = nullptr;
    if (!getPtr) getPtr = LoadCoreSymbol<Fn>("DebugMemGetPointer");
    if (!getPtr) return nullptr;
    return static_cast<uint8_t*>(getPtr(M64P_DBG_PTR_RDRAM));
}

// Map N64 virtual address to RDRAM byte offset.
// Accepts bare RDRAM offsets (< 0x800000) or full KSEG0/KSEG1 addresses.
static uint32_t ToRdramOffset(uint32_t addr)
{
    if (addr < 0x00800000u) return addr;                 // bare offset
    if ((addr >> 24) == 0x80 || (addr >> 24) == 0xa0)   // KSEG0/KSEG1
        return addr & 0x007FFFFFu;
    return addr & 0x007FFFFFu;                           // best-effort
}

// mupen64plus stores RDRAM as uint32_t[] in host (little-endian on x86) byte
// order. Each uint32_t IS the N64 big-endian 32-bit value. Byte/halfword reads
// must extract from the uint32 using big-endian bit shifts.

static inline uint8_t RdramRead8(const uint8_t* rdram, uint32_t off)
{
    uint32_t word;
    memcpy(&word, rdram + (off & ~3u), 4);
    return static_cast<uint8_t>((word >> ((3u - (off & 3u)) * 8u)) & 0xFFu);
}

static inline uint16_t RdramRead16(const uint8_t* rdram, uint32_t off)
{
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(RdramRead8(rdram, off))     << 8) |
         static_cast<uint32_t>(RdramRead8(rdram, off + 1)));
}

static inline uint32_t RdramRead32(const uint8_t* rdram, uint32_t off)
{
    uint32_t v;
    memcpy(&v, rdram + off, 4);
    return v;
}

static inline float RdramReadFloat(const uint8_t* rdram, uint32_t off)
{
    uint32_t bits = RdramRead32(rdram, off);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static inline void RdramWrite8(uint8_t* rdram, uint32_t off, uint8_t byte)
{
    uint32_t word;
    uint32_t a = off & ~3u;
    memcpy(&word, rdram + a, 4);
    uint32_t shift = (3u - (off & 3u)) * 8u;
    word = (word & ~(0xFFu << shift)) | (static_cast<uint32_t>(byte) << shift);
    memcpy(rdram + a, &word, 4);
}

static inline void RdramWrite16(uint8_t* rdram, uint32_t off, uint16_t val)
{
    RdramWrite8(rdram, off,     static_cast<uint8_t>(val >> 8));
    RdramWrite8(rdram, off + 1, static_cast<uint8_t>(val & 0xFF));
}

static inline void RdramWrite32(uint8_t* rdram, uint32_t off, uint32_t val)
{
    memcpy(rdram + off, &val, 4);
}

// ─── JS glue helpers ─────────────────────────────────────────────────────────

// Returns nullptr and throws a JS TypeError if RDRAM is unavailable.
static uint8_t* RequireRDRAM(JSContext* ctx)
{
    uint8_t* rdram = GetRDRAM();
    if (!rdram)
        JS_ThrowTypeError(ctx, "RDRAM not available (emulation not running)");
    return rdram;
}

// Reads the first argument as a uint32_t address.
static bool GetAddress(JSContext* ctx, JSValue arg, uint32_t* out)
{
    uint32_t v;
    if (JS_ToUint32(ctx, &v, arg) != 0) return false;
    *out = ToRdramOffset(v);
    return true;
}

// ─── memory.read* ────────────────────────────────────────────────────────────

static JSValue Mem_Read8(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read8(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewUint32(ctx, RdramRead8(rdram, off));
}

static JSValue Mem_Read8s(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read8s(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int8_t>(RdramRead8(rdram, off)));
}

static JSValue Mem_Read16(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read16(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewUint32(ctx, RdramRead16(rdram, off));
}

static JSValue Mem_Read16s(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read16s(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int16_t>(RdramRead16(rdram, off)));
}

static JSValue Mem_Read32(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read32(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    // Return as double to handle unsigned 32-bit range without sign issues.
    return JS_NewFloat64(ctx, static_cast<double>(RdramRead32(rdram, off)));
}

static JSValue Mem_Read32s(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read32s(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int32_t>(RdramRead32(rdram, off)));
}

static JSValue Mem_ReadFloat(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "readFloat(addr)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, static_cast<double>(RdramReadFloat(rdram, off)));
}

// ─── memory.write* ───────────────────────────────────────────────────────────

static JSValue Mem_Write8(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "write8(addr, val)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint32_t v; if (JS_ToUint32(ctx, &v, argv[1]) != 0) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    RdramWrite8(rdram, off, static_cast<uint8_t>(v));
    return JS_UNDEFINED;
}

static JSValue Mem_Write16(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "write16(addr, val)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint32_t v; if (JS_ToUint32(ctx, &v, argv[1]) != 0) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    RdramWrite16(rdram, off, static_cast<uint16_t>(v));
    return JS_UNDEFINED;
}

static JSValue Mem_Write32(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "write32(addr, val)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    uint32_t v; if (JS_ToUint32(ctx, &v, argv[1]) != 0) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    RdramWrite32(rdram, off, v);
    return JS_UNDEFINED;
}

static JSValue Mem_WriteFloat(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFloat(addr, val)");
    uint32_t off; if (!GetAddress(ctx, argv[0], &off)) return JS_EXCEPTION;
    double d; if (JS_ToFloat64(ctx, &d, argv[1]) != 0) return JS_EXCEPTION;
    uint8_t* rdram = RequireRDRAM(ctx); if (!rdram) return JS_EXCEPTION;
    float fv = static_cast<float>(d);
    uint32_t bits; memcpy(&bits, &fv, 4);
    RdramWrite32(rdram, off, bits);
    return JS_UNDEFINED;
}

// ─── Registration ─────────────────────────────────────────────────────────────

void RegisterMemoryModule(JSContext* ctx)
{
    JSValue mem = JS_NewObject(ctx);

#define SET(name, fn, nargs) \
    JS_SetPropertyStr(ctx, mem, name, JS_NewCFunction(ctx, fn, name, nargs))

    SET("read8",       Mem_Read8,      1);
    SET("read8s",      Mem_Read8s,     1);
    SET("read16",      Mem_Read16,     1);
    SET("read16s",     Mem_Read16s,    1);
    SET("read32",      Mem_Read32,     1);
    SET("read32s",     Mem_Read32s,    1);
    SET("readFloat",   Mem_ReadFloat,  1);
    SET("write8",      Mem_Write8,     2);
    SET("write16",     Mem_Write16,    2);
    SET("write32",     Mem_Write32,    2);
    SET("writeFloat",  Mem_WriteFloat, 2);

#undef SET

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "memory", mem);
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
