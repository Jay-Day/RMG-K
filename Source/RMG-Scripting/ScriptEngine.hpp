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

#include <quickjs.h>

#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>

using ScriptOutputCallbackFunc  = std::function<void(const std::string&)>;
using ScriptInputPromptCallbackFunc = std::function<std::string(const std::string&)>;

// Retrieve the ScriptEngine* stored in a JSContext's opaque slot.
class ScriptEngine;
ScriptEngine* GetEngineFromContext(JSContext* ctx);

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    bool Initialize();
    bool LoadScript(const std::string& filepath);

    // Called directly from the emulation thread — executes callbacks synchronously.
    void FireFrameCallbacks();
    void FireBreakpointCallback(uint32_t pc);

    // Called from the UI thread when the user submits text input.
    void FireInputCallback(const std::string& text);

    bool IsRunning() const { return m_running; }
    bool HasCallbacks() const {
        return !m_frameCallbackRefs.empty() ||
               !m_bpCallbackRefs.empty()   ||
               !m_inputCallbackRefs.empty();
    }
    void Shutdown();

    void SetOutputCallback(ScriptOutputCallbackFunc callback);
    void EmitOutput(const std::string& line);

    void SetInputPromptCallback(ScriptInputPromptCallbackFunc callback);
    std::string DoInputPrompt(const std::string& message);

    // Called from JS C-function glue during script load (same thread as JS_Eval).
    void RegisterJSFrameCallback(JSValue fn);
    void RegisterJSBreakpointCallback(uint32_t address, JSValue fn);
    void RegisterJSInputCallback(JSValue fn);

    JSContext* GetContext() { return m_ctx; }

    ScriptOutputCallbackFunc CopyOutputCallback() {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        return m_outputCallback;
    }

private:
    JSRuntime* m_rt  = nullptr;
    JSContext* m_ctx = nullptr;
    bool m_running = false;

    std::vector<JSValue>                               m_frameCallbackRefs;
    std::unordered_map<uint32_t, std::vector<JSValue>> m_bpCallbackRefs;
    std::vector<JSValue>                               m_inputCallbackRefs;

    // Serializes all JS context access (load vs. fire can come from different threads).
    std::mutex m_jsMutex;

    ScriptOutputCallbackFunc        m_outputCallback;
    std::mutex                      m_outputMutex;

    ScriptInputPromptCallbackFunc   m_inputPromptCallback;
    std::mutex                      m_inputPromptMutex;

    void FreeCallbackRefs();
    bool ReportException(const std::string& context);
};

#endif // RMGK_SCRIPT_ENGINE_HPP
