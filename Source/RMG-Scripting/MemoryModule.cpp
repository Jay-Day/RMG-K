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

#include <lua.hpp>

#include <cstring>

namespace RMGScript {

// Forward declare the memory functions to register.
// Naming: N64/MIPS convention (byte=8, half/word=16, word/dword=32) is confusing,
// so we expose clear numeric names (read8/read16/read32) as the primary API and
// keep the old names as aliases for compatibility.
static const luaL_Reg memoryLib[] = {
    // Primary names
    {"read8",            MemoryModule::ReadByte},
    {"read16",           MemoryModule::ReadWord},
    {"read32",           MemoryModule::ReadDword},
    {"read8s",           MemoryModule::ReadByteSigned},
    {"read16s",          MemoryModule::ReadWordSigned},
    {"read32s",          MemoryModule::ReadDwordSigned},
    {"readfloat",        MemoryModule::ReadFloat},
    {"readdouble",       MemoryModule::ReadDouble},
    {"write8",           MemoryModule::WriteByte},
    {"write16",          MemoryModule::WriteWord},
    {"write32",          MemoryModule::WriteDword},
    {"writefloat",       MemoryModule::WriteFloat},
    {"writedouble",      MemoryModule::WriteDouble},
    // Legacy aliases (kept for backwards compatibility)
    {"readbyte",         MemoryModule::ReadByte},
    {"readbytesigned",   MemoryModule::ReadByteSigned},
    {"readword",         MemoryModule::ReadWord},
    {"readwordsigned",   MemoryModule::ReadWordSigned},
    {"readdword",        MemoryModule::ReadDword},
    {"readdwordsigned",  MemoryModule::ReadDwordSigned},
    {"readsize",         MemoryModule::ReadSize},
    {"writebyte",        MemoryModule::WriteByte},
    {"writeword",        MemoryModule::WriteWord},
    {"writedword",       MemoryModule::WriteDword},
    {"writesize",        MemoryModule::WriteSize},
    {NULL, NULL}
};

void RegisterMemoryModule(lua_State* L) {
    // Create memory table
    lua_newtable(L);
    
    // Register all memory functions
    luaL_setfuncs(L, memoryLib, 0);
    
    // Set as global "memory"
    lua_setglobal(L, "memory");
}

template <typename T>
static T LoadCoreSymbol(const char* name) {
    CoreLibraryHandle coreHandle = CoreGetM64PCoreHandle();
    if (!coreHandle) return nullptr;
    return reinterpret_cast<T>(CoreGetLibrarySymbol(coreHandle, name));
}

static uint32_t NormalizeAddress(uint32_t address) {
    // Allow scripts to pass RDRAM offsets directly (0..0x7FFFFF)
    if (address < 0x00800000) {
        return 0x80000000u + address;
    }
    return address;
}

uint8_t* MemoryModule::GetRDRAM() {
    // DebugMemGetPointer is exported unconditionally (no DBG guard in core source).
    using ptr_DebugMemGetPointer = void* (*)(m64p_dbg_memptr_type);
    static ptr_DebugMemGetPointer getPtr = nullptr;
    if (!getPtr) {
        getPtr = LoadCoreSymbol<ptr_DebugMemGetPointer>("DebugMemGetPointer");
    }
    if (!getPtr) return nullptr;
    return static_cast<uint8_t*>(getPtr(M64P_DBG_PTR_RDRAM));
}

// Read a single N64 big-endian byte from the raw RDRAM buffer.
// mupen64plus stores RDRAM as uint32_t[] in host byte order; each uint32_t holds
// 4 N64 bytes with byte 0 in the MSB (big-endian).  Reading through uint8_t* on a
// little-endian host gives the bytes in reversed order, so we must realign.
static inline uint8_t RdramReadByte(const uint8_t* rdram, uint32_t off) {
    uint32_t word;
    std::memcpy(&word, rdram + (off & ~3u), 4);
    return static_cast<uint8_t>((word >> ((3u - (off & 3u)) * 8u)) & 0xFFu);
}

// Helper function to safely get a memory value with bounds checking
static inline uint32_t SafeMemoryRead(uint32_t address, size_t size) {
    address = NormalizeAddress(address);

    // Only RDRAM (KSEG0 0x80000000-0x807FFFFF) is supported via direct pointer.
    // Other regions would need the debugger API which requires DBG build flag.
    uint8_t* rdram = MemoryModule::GetRDRAM();
    if (!rdram) return 0;
    if ((address & 0xFF800000u) != 0x80000000u) return 0;
    uint32_t off = address & 0x007FFFFFu;
    if (off + size > 0x00800000u) return 0;

    if (size == 4) {
        // memcpy of 4 bytes gives the uint32_t in host byte order, which equals the
        // N64 big-endian 32-bit value directly.
        uint32_t value;
        std::memcpy(&value, rdram + off, 4);
        return value;
    }
    if (size == 1) {
        return RdramReadByte(rdram, off);
    }
    if (size == 2) {
        return (static_cast<uint32_t>(RdramReadByte(rdram, off)) << 8) |
                static_cast<uint32_t>(RdramReadByte(rdram, off + 1));
    }
    return 0;
}

static inline void RdramWriteByte(uint8_t* rdram, uint32_t off, uint8_t byte) {
    uint32_t word;
    uint32_t aligned_off = off & ~3u;
    std::memcpy(&word, rdram + aligned_off, 4);
    uint32_t shift = (3u - (off & 3u)) * 8u;
    word = (word & ~(0xFFu << shift)) | (static_cast<uint32_t>(byte) << shift);
    std::memcpy(rdram + aligned_off, &word, 4);
}

// Helper function to safely write to memory
static inline void SafeMemoryWrite(uint32_t address, uint32_t value, size_t size) {
    address = NormalizeAddress(address);

    uint8_t* rdram = MemoryModule::GetRDRAM();
    if (!rdram) return;
    if ((address & 0xFF800000u) != 0x80000000u) return;
    uint32_t off = address & 0x007FFFFFu;
    if (off + size > 0x00800000u) return;

    if (size == 4) {
        std::memcpy(rdram + off, &value, 4);
    } else if (size == 1) {
        RdramWriteByte(rdram, off, static_cast<uint8_t>(value & 0xFF));
    } else if (size == 2) {
        RdramWriteByte(rdram, off,     static_cast<uint8_t>((value >> 8) & 0xFF));
        RdramWriteByte(rdram, off + 1, static_cast<uint8_t>(value & 0xFF));
    }
}

int MemoryModule::ReadByte(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = SafeMemoryRead(address, 1);
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadByteSigned(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    int8_t value = static_cast<int8_t>(SafeMemoryRead(address, 1));
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadWord(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = SafeMemoryRead(address, 2);
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadWordSigned(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    int16_t value = static_cast<int16_t>(SafeMemoryRead(address, 2));
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadDword(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = SafeMemoryRead(address, 4);
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadDwordSigned(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    int32_t value = static_cast<int32_t>(SafeMemoryRead(address, 4));
    lua_pushinteger(L, value);
    return 1;
}

int MemoryModule::ReadFloat(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t bits = SafeMemoryRead(address, 4);
    float value = 0.0f;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&value, &bits, sizeof(value));
    lua_pushnumber(L, static_cast<double>(value));
    return 1;
}

int MemoryModule::ReadDouble(lua_State* L) {
    uint32_t address = NormalizeAddress(luaL_checkinteger(L, 1));
    uint32_t hi = SafeMemoryRead(address, 4);
    uint32_t lo = SafeMemoryRead(address + 4, 4);
    uint64_t bits = (static_cast<uint64_t>(hi) << 32) | lo;
    double value = 0.0;
    static_assert(sizeof(double) == sizeof(uint64_t));
    std::memcpy(&value, &bits, sizeof(value));
    lua_pushnumber(L, value);
    return 1;
}

int MemoryModule::ReadSize(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    int size = luaL_checkinteger(L, 2);
    
    switch (size) {
        case 1:
            lua_pushinteger(L, SafeMemoryRead(address, 1));
            break;
        case 2:
            lua_pushinteger(L, SafeMemoryRead(address, 2));
            break;
        case 4:
            lua_pushinteger(L, SafeMemoryRead(address, 4));
            break;
        case -1:
            lua_pushinteger(L, static_cast<int8_t>(SafeMemoryRead(address, 1)));
            break;
        case -2:
            lua_pushinteger(L, static_cast<int16_t>(SafeMemoryRead(address, 2)));
            break;
        case -4:
            lua_pushinteger(L, static_cast<int32_t>(SafeMemoryRead(address, 4)));
            break;
        default:
            luaL_error(L, "size must be 1, 2, 4, -1, -2, or -4");
            break;
    }
    
    return 1;
}

int MemoryModule::WriteByte(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = luaL_checkinteger(L, 2);
    SafeMemoryWrite(address, value, 1);
    return 0;
}

int MemoryModule::WriteWord(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = luaL_checkinteger(L, 2);
    SafeMemoryWrite(address, value, 2);
    return 0;
}

int MemoryModule::WriteDword(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    uint32_t value = luaL_checkinteger(L, 2);
    SafeMemoryWrite(address, value, 4);
    return 0;
}

int MemoryModule::WriteFloat(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    float fvalue = luaL_checknumber(L, 2);
    uint32_t value = 0;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&value, &fvalue, sizeof(value));
    SafeMemoryWrite(address, value, 4);
    return 0;
}

int MemoryModule::WriteDouble(lua_State* L) {
    double dvalue = luaL_checknumber(L, 2);
    uint32_t address = NormalizeAddress(luaL_checkinteger(L, 1));
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t));
    std::memcpy(&bits, &dvalue, sizeof(bits));
    SafeMemoryWrite(address,     static_cast<uint32_t>(bits >> 32),         4);
    SafeMemoryWrite(address + 4, static_cast<uint32_t>(bits & 0xFFFFFFFFu), 4);
    return 0;
}

int MemoryModule::WriteSize(lua_State* L) {
    uint32_t address = luaL_checkinteger(L, 1);
    int size = luaL_checkinteger(L, 2);
    uint32_t value = luaL_checkinteger(L, 3);
    
    switch (size) {
        case 1:
            SafeMemoryWrite(address, value, 1);
            break;
        case 2:
            SafeMemoryWrite(address, value, 2);
            break;
        case 4:
            SafeMemoryWrite(address, value, 4);
            break;
        case -1:
            SafeMemoryWrite(address, static_cast<uint32_t>(static_cast<int8_t>(value)), 1);
            break;
        case -2:
            SafeMemoryWrite(address, static_cast<uint32_t>(static_cast<int16_t>(value)), 2);
            break;
        case -4:
            SafeMemoryWrite(address, static_cast<uint32_t>(static_cast<int32_t>(value)), 4);
            break;
        default:
            luaL_error(L, "size must be 1, 2, 4, -1, -2, or -4");
            break;
    }
    
    return 0;
}

} // namespace RMGScript
