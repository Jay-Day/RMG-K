/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ScriptEngine.hpp"
#include "MemoryModule.hpp"
#include "EventModule.hpp"

#include <lua.hpp>

#include <iostream>
#include <chrono>
#include <condition_variable>

static constexpr const char* kRegistryScriptEngineKey = "RMG_SCRIPT_ENGINE";

static int LuaPrint(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryScriptEngineKey);
    auto* engine = static_cast<ScriptEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (engine == nullptr)
    {
        return 0;
    }

    const int nargs = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= nargs; i++)
    {
        size_t len = 0;
        const char* str = luaL_tolstring(L, i, &len);
        if (str != nullptr)
        {
            if (!line.empty())
            {
                line += "\t";
            }
            line.append(str, len);
        }
        lua_pop(L, 1);
    }

    engine->EmitOutput(line);
    return 0;
}

ScriptEngine::ScriptEngine() 
    : m_lua(nullptr), m_running(false), m_executorRunning(false) {
}

ScriptEngine::~ScriptEngine() {
    Shutdown();
}

bool ScriptEngine::Initialize() {
    if (m_running) {
        return true;
    }

    // Create Lua state
    m_lua = luaL_newstate();
    if (!m_lua) {
        std::cerr << "Failed to create Lua state" << std::endl;
        return false;
    }

    // Open standard libraries
    luaL_openlibs(m_lua);

    // Store ScriptEngine pointer in registry so C modules can access it
    lua_pushlightuserdata(m_lua, this);
    lua_setfield(m_lua, LUA_REGISTRYINDEX, kRegistryScriptEngineKey);

    // Replace Lua global print() so script output can be shown in the UI.
    lua_pushcfunction(m_lua, LuaPrint);
    lua_setglobal(m_lua, "print");

    // Register custom modules
    RMGScript::RegisterMemoryModule(m_lua);
    RMGScript::RegisterEventModule(m_lua);

    m_running = true;
    m_executorRunning = true;
    m_executorThread = std::thread(&ScriptEngine::ExecutorThreadMain, this);

    return true;
}

bool ScriptEngine::LoadScript(const std::string& filepath) {
    if (!m_lua) {
        return false;
    }

    std::lock_guard<std::mutex> luaLock(m_luaMutex);

    // Load the Lua file
    if (luaL_loadfile(m_lua, filepath.c_str()) != LUA_OK) {
        EmitOutput(std::string("Failed to load Lua script: ") + lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        return false;
    }

    // Execute the script (this defines functions but doesn't call them)
    if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK) {
        EmitOutput(std::string("Failed to execute Lua script: ") + lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        return false;
    }

    return true;
}

bool ScriptEngine::ExecuteFunction(const std::string& functionName) {
    if (!m_lua) {
        return false;
    }

    ScriptJob job;
    job.type = ScriptJob::EXECUTE_FUNCTION;
    job.functionName = functionName;

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobQueue.push(job);
    }

    return true;
}

void ScriptEngine::RegisterFrameCallback(FrameCallbackFunc callback) {
    std::lock_guard<std::mutex> lock(m_callbacksMutex);
    m_frameCallbacks.push_back(callback);
}

void ScriptEngine::RegisterBreakpointCallback(uint32_t address, BreakpointCallbackFunc callback) {
    // TODO: Implement breakpoint callback registration
    (void)address;
    (void)callback;
}

void ScriptEngine::RegisterLuaFrameCallback(int luaFuncRef) {
    std::lock_guard<std::mutex> lock(m_callbacksMutex);
    m_luaFrameCallbackRefs.push_back(luaFuncRef);
}

void ScriptEngine::RegisterLuaBreakpointCallback(uint32_t address, int luaFuncRef) {
    std::lock_guard<std::mutex> lock(m_callbacksMutex);
    m_luaBreakpointCallbackRefs[address].push_back(luaFuncRef);
}

void ScriptEngine::FireFrameCallbacks() {
    ScriptJob job;
    job.type = ScriptJob::CALL_FRAME_CALLBACKS;
    
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobQueue.push(job);
    }
}

void ScriptEngine::FireBreakpointCallback(uint32_t pc) {
    ScriptJob job;
    job.type = ScriptJob::CALL_BREAKPOINT_CALLBACKS;
    job.pc = pc;

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobQueue.push(job);
    }
}

void ScriptEngine::Shutdown() {
    if (!m_running) {
        return;
    }

    m_running = false;
    m_executorRunning = false;

    // Send shutdown job
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        ScriptJob shutdownJob;
        shutdownJob.type = ScriptJob::SHUTDOWN;
        m_jobQueue.push(shutdownJob);
    }

    // Wait for executor thread to finish
    if (m_executorThread.joinable()) {
        m_executorThread.join();
    }

    // Close Lua state
    if (m_lua) {
        lua_close(m_lua);
        m_lua = nullptr;
    }
}

void ScriptEngine::ProcessJobs() {
    // This is currently unused as jobs are processed in the executor thread
    // But kept for potential future use for UI thread processing
}

void ScriptEngine::SetOutputCallback(ScriptOutputCallbackFunc callback)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    m_outputCallback = std::move(callback);
}

bool ScriptEngine::CallLuaFunction(const std::string& functionName) {
    if (!m_lua) {
        return false;
    }

    std::lock_guard<std::mutex> luaLock(m_luaMutex);

    // Get the function from the global table
    lua_getglobal(m_lua, functionName.c_str());
    
    if (!lua_isfunction(m_lua, -1)) {
        lua_pop(m_lua, 1);
        return false;
    }

    // Call the function with no arguments and no return values
    if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK) {
        EmitOutput(std::string("Error calling Lua function '") + functionName +
                   "': " + lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        return false;
    }

    return true;
}

static bool CallLuaRef(lua_State* L, ScriptEngine* engine, int ref, uint32_t pc, bool passPc) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    if (passPc) {
        lua_pushinteger(L, pc);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            if (engine != nullptr) {
                engine->EmitOutput(std::string("Error calling Lua callback: ") + lua_tostring(L, -1));
            }
            lua_pop(L, 1);
            return false;
        }
    } else {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            if (engine != nullptr) {
                engine->EmitOutput(std::string("Error calling Lua callback: ") + lua_tostring(L, -1));
            }
            lua_pop(L, 1);
            return false;
        }
    }

    return true;
}

void ScriptEngine::EmitOutput(const std::string& line)
{
    ScriptOutputCallbackFunc cb;
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        cb = m_outputCallback;
    }

    if (cb)
    {
        cb(line);
    }
    else
    {
        std::cerr << line << std::endl;
    }
}

void ScriptEngine::ExecutorThreadMain() {
    while (m_executorRunning) {
        ScriptJob job;
        bool hasJob = false;

        // Check if there's a job to process
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            if (!m_jobQueue.empty()) {
                job = m_jobQueue.front();
                m_jobQueue.pop();
                hasJob = true;
            }
        }

        if (!hasJob) {
            // Sleep for a short time to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Process the job
        switch (job.type) {
            case ScriptJob::EXECUTE_FUNCTION:
                CallLuaFunction(job.functionName);
                break;

            case ScriptJob::CALL_FRAME_CALLBACKS: {
                std::vector<FrameCallbackFunc> callbacks;
                std::vector<int> luaRefs;
                {
                    std::lock_guard<std::mutex> lock(m_callbacksMutex);
                    callbacks = m_frameCallbacks;
                    luaRefs = m_luaFrameCallbackRefs;
                }
                
                for (const auto& callback : callbacks) {
                    try {
                        callback();
                    } catch (const std::exception& e) {
                        std::cerr << "Exception in frame callback: " << e.what() << std::endl;
                    }
                }

                if (!luaRefs.empty() && m_lua) {
                    std::lock_guard<std::mutex> luaLock(m_luaMutex);
                    for (int ref : luaRefs) {
                        CallLuaRef(m_lua, this, ref, 0, false);
                    }
                }
                break;
            }

            case ScriptJob::CALL_BREAKPOINT_CALLBACKS: {
                std::vector<int> luaRefs;
                {
                    std::lock_guard<std::mutex> lock(m_callbacksMutex);
                    auto it = m_luaBreakpointCallbackRefs.find(job.pc);
                    if (it != m_luaBreakpointCallbackRefs.end()) {
                        luaRefs = it->second;
                    }
                }

                if (!luaRefs.empty() && m_lua) {
                    std::lock_guard<std::mutex> luaLock(m_luaMutex);
                    for (int ref : luaRefs) {
                        CallLuaRef(m_lua, this, ref, job.pc, true);
                    }
                }
                break;
            }

            case ScriptJob::SHUTDOWN:
                m_executorRunning = false;
                break;
        }
    }
}
