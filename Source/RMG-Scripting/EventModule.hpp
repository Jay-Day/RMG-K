/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_EVENT_MODULE_HPP
#define RMGK_EVENT_MODULE_HPP

struct lua_State;

namespace RMGScript {

// Register event module functions to Lua
void RegisterEventModule(lua_State* L);

// Event/EMU functions (called from Lua)
class EventModule {
public:
    // Event registration functions
    static int OnFrame(lua_State* L);
    static int OnBreakpoint(lua_State* L);
    static int OnStop(lua_State* L);
    static int OnPause(lua_State* L);
    static int OnResume(lua_State* L);
    
    // Emulation control
    static int GetPC(lua_State* L);
    static int IsPaused(lua_State* L);
    static int IsRunning(lua_State* L);
    
    // Get memory region address
    static int GetAddress(lua_State* L);
};

} // namespace RMGScript

#endif // RMGK_EVENT_MODULE_HPP
