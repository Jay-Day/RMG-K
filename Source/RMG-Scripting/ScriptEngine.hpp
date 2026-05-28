/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_SCRIPT_ENGINE_HPP
#define RMGK_SCRIPT_ENGINE_HPP

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <unordered_map>

struct lua_State;

// Callback types for script events
using FrameCallbackFunc = std::function<void()>;
using BreakpointCallbackFunc = std::function<void(uint32_t pc)>;
using EventCallbackFunc = std::function<void()>;
using ScriptOutputCallbackFunc = std::function<void(const std::string&)>;

// Script execution job for async execution
struct ScriptJob {
    enum Type {
        EXECUTE_FUNCTION,
        CALL_FRAME_CALLBACKS,
        CALL_BREAKPOINT_CALLBACKS,
        SHUTDOWN
    };
    
    Type type;
    std::string functionName;  // for EXECUTE_FUNCTION
    uint32_t pc = 0; // for CALL_BREAKPOINT_CALLBACKS
};

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();
    
    // Initialize the script engine
    bool Initialize();
    
    // Load and execute a Lua script file
    bool LoadScript(const std::string& filepath);
    
    // Execute a Lua function by name
    bool ExecuteFunction(const std::string& functionName);
    
    // Register frame callback (called every emulated frame)
    void RegisterFrameCallback(FrameCallbackFunc callback);
    
    // Register breakpoint callback (called when PC reaches address)
    void RegisterBreakpointCallback(uint32_t address, BreakpointCallbackFunc callback);
    
    // Register a Lua callback currently on stack.
    // These are intended to be called from Lua C functions during script load.
    void RegisterLuaFrameCallback(int luaFuncRef);
    void RegisterLuaBreakpointCallback(uint32_t address, int luaFuncRef);

    // Call all registered frame callbacks (non-blocking, async)
    void FireFrameCallbacks();
    
    // Call breakpoint callbacks for an address
    void FireBreakpointCallback(uint32_t pc);
    
    // Check if engine is running
    bool IsRunning() const { return m_running; }
    
    // Shutdown the engine
    void Shutdown();
    
    // Get the Lua state (for direct access if needed)
    lua_State* GetLuaState() { return m_lua; }
    
    // Process async jobs (should be called from UI thread if doing I/O)
    void ProcessJobs();

    // Receive print/error output from the Lua runtime.
    void SetOutputCallback(ScriptOutputCallbackFunc callback);
    void EmitOutput(const std::string& line);

private:
    lua_State* m_lua;
    bool m_running;
    std::vector<FrameCallbackFunc> m_frameCallbacks;
    std::mutex m_callbacksMutex;

    // All Lua API usage must be serialized.
    std::mutex m_luaMutex;

    // Lua callback refs (registry indices)
    std::vector<int> m_luaFrameCallbackRefs;
    std::unordered_map<uint32_t, std::vector<int>> m_luaBreakpointCallbackRefs;
    ScriptOutputCallbackFunc m_outputCallback;
    std::mutex m_outputMutex;
    
    // Async execution
    std::queue<ScriptJob> m_jobQueue;
    std::mutex m_jobMutex;
    std::thread m_executorThread;
    bool m_executorRunning;
    
    // Worker thread for executing Lua functions
    void ExecutorThreadMain();
    
    // Helper to safely call Lua functions
    bool CallLuaFunction(const std::string& functionName);
};

#endif // RMGK_SCRIPT_ENGINE_HPP
