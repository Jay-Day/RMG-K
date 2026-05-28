/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_MEMORY_MODULE_HPP
#define RMGK_MEMORY_MODULE_HPP

#include <cstdint>

struct lua_State;

namespace RMGScript {

// Register memory module functions to Lua
void RegisterMemoryModule(lua_State* L);

// Memory access functions (called from Lua)
class MemoryModule {
public:
    // Read functions
    static int ReadByte(lua_State* L);
    static int ReadByteSigned(lua_State* L);
    static int ReadWord(lua_State* L);
    static int ReadWordSigned(lua_State* L);
    static int ReadDword(lua_State* L);
    static int ReadDwordSigned(lua_State* L);
    static int ReadFloat(lua_State* L);
    static int ReadDouble(lua_State* L);
    static int ReadSize(lua_State* L);
    
    // Write functions
    static int WriteByte(lua_State* L);
    static int WriteWord(lua_State* L);
    static int WriteDword(lua_State* L);
    static int WriteFloat(lua_State* L);
    static int WriteDouble(lua_State* L);
    static int WriteSize(lua_State* L);
    
    // Helper to get RDRAM pointer
    static uint8_t* GetRDRAM();
};

} // namespace RMGScript

#endif // RMGK_MEMORY_MODULE_HPP
