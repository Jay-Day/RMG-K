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
#include "InputModule.hpp"
#include "NetworkModule.hpp"
#include "NetplayModule.hpp"

#include <quickjs.h>
#include <quickjs-libc.h>

#include <iostream>
#include <fstream>
#include <sstream>

ScriptEngine* GetEngineFromContext(JSContext* ctx)
{
    return static_cast<ScriptEngine*>(JS_GetContextOpaque(ctx));
}

// ─── print / console.log ─────────────────────────────────────────────────────

static std::string ArgsToString(JSContext* ctx, int argc, JSValue* argv)
{
    std::string line;
    for (int i = 0; i < argc; i++) {
        if (i > 0) line += ' ';
        // JS_ToString before ToCString to ensure ropes are linearised first.
        JSValue str = JS_ToString(ctx, argv[i]);
        if (!JS_IsException(str)) {
            const char* s = JS_ToCString(ctx, str);
            if (s) { line += s; JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, str);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    return line;
}

static JSValue JS_Print(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    std::string line = ArgsToString(ctx, argc, argv);
    ScriptEngine* engine = GetEngineFromContext(ctx);
    if (engine) engine->EmitOutput(line);
    else        std::cerr << line << '\n';
    return JS_UNDEFINED;
}

static JSValue JS_ConsoleLog(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    return JS_Print(ctx, JS_UNDEFINED, argc, argv);
}


// ─── ScriptEngine ────────────────────────────────────────────────────────────

ScriptEngine::ScriptEngine() = default;
ScriptEngine::~ScriptEngine() { Shutdown(); }

bool ScriptEngine::Initialize()
{
    if (m_running) return true;

    m_rt  = JS_NewRuntime();
    m_ctx = JS_NewContext(m_rt);
    if (!m_rt || !m_ctx) {
        std::cerr << "ScriptEngine: failed to create JS runtime/context\n";
        return false;
    }

    JS_SetContextOpaque(m_ctx, this);

    // Register quickjs-libc std/os modules and their module loader.
    js_std_init_handlers(m_rt);
    js_init_module_std(m_ctx, "std");
    js_init_module_os(m_ctx, "os");
    JS_SetModuleLoaderFunc2(m_rt, nullptr, js_module_loader,
                             js_module_check_attributes, nullptr);

    // Expose std and os as globals so scripts don't need import syntax.
    // Evaluated as a module so the import resolves, then stored on globalThis.
    {
        const char* bootstrap =
            "import * as _std from 'std';\n"
            "import * as _os  from 'os';\n"
            "globalThis.std = _std;\n"
            "globalThis.os  = _os;\n";
        JSValue p = JS_Eval(m_ctx, bootstrap, strlen(bootstrap),
                            "<bootstrap>", JS_EVAL_TYPE_MODULE);
        if (!JS_IsException(p))
            js_std_loop(m_ctx); // run event loop until module resolves
        else
            JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        JS_FreeValue(m_ctx, p);
    }

    // Our print / console.log override (routes to the UI output panel).
    JSValue global = JS_GetGlobalObject(m_ctx);
    JS_SetPropertyStr(m_ctx, global, "print",
        JS_NewCFunction(m_ctx, JS_Print, "print", 1));

    JSValue console = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, console, "log",   JS_NewCFunction(m_ctx, JS_ConsoleLog, "log",   1));
    JS_SetPropertyStr(m_ctx, console, "warn",  JS_NewCFunction(m_ctx, JS_ConsoleLog, "warn",  1));
    JS_SetPropertyStr(m_ctx, console, "error", JS_NewCFunction(m_ctx, JS_ConsoleLog, "error", 1));
    JS_SetPropertyStr(m_ctx, global, "console", console);
    JS_FreeValue(m_ctx, global);

    RMGScript::RegisterMemoryModule(m_ctx);
    RMGScript::RegisterEventModule(m_ctx);
    RMGScript::RegisterInputModule(m_ctx);
    RMGScript::RegisterNetworkModule(m_ctx);
    RMGScript::RegisterNetplayModule(m_ctx);

    m_running = true;
    return true;
}

bool ScriptEngine::LoadScript(const std::string& filepath)
{
    if (!m_ctx) return false;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) { EmitOutput("Failed to open: " + filepath); return false; }
    std::ostringstream ss; ss << file.rdbuf();
    std::string src = ss.str();

    std::lock_guard<std::mutex> lock(m_jsMutex);
    // Re-anchor the stack limit to the current (UI) thread before evaluating.
    JS_UpdateStackTop(m_rt);

    JSValue result = JS_Eval(m_ctx, src.c_str(), src.size(),
                             filepath.c_str(), JS_EVAL_TYPE_GLOBAL);
    bool ok = !JS_IsException(result);
    if (!ok) ReportException("LoadScript");
    JS_FreeValue(m_ctx, result);
    return ok;
}

void ScriptEngine::RegisterJSFrameCallback(JSValue fn)
{
    // Called from JS code running on the load thread — no extra lock needed
    // because we're already inside the m_jsMutex via LoadScript.
    m_frameCallbackRefs.push_back(fn);
}

void ScriptEngine::RegisterJSBreakpointCallback(uint32_t address, JSValue fn)
{
    m_bpCallbackRefs[address].push_back(fn);
}

void ScriptEngine::FireFrameCallbacks()
{
    if (m_frameCallbackRefs.empty()) return;

    std::lock_guard<std::mutex> lock(m_jsMutex);
    // Re-anchor stack limit to the calling (emulation) thread's stack.
    JS_UpdateStackTop(m_rt);

    for (JSValue fn : m_frameCallbackRefs) {
        JSValue ret = JS_Call(m_ctx, fn, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(ret)) ReportException("on_frame");
        JS_FreeValue(m_ctx, ret);
    }
}

void ScriptEngine::FireBreakpointCallback(uint32_t pc)
{
    auto it = m_bpCallbackRefs.find(pc);
    if (it == m_bpCallbackRefs.end()) return;

    std::lock_guard<std::mutex> lock(m_jsMutex);
    JS_UpdateStackTop(m_rt);

    JSValue pcVal = JS_NewUint32(m_ctx, pc);
    for (JSValue fn : it->second) {
        JSValue ret = JS_Call(m_ctx, fn, JS_UNDEFINED, 1, &pcVal);
        if (JS_IsException(ret)) ReportException("on_breakpoint");
        JS_FreeValue(m_ctx, ret);
    }
    JS_FreeValue(m_ctx, pcVal);
}

void ScriptEngine::Shutdown()
{
    if (!m_running) return;
    m_running = false;

    std::lock_guard<std::mutex> lock(m_jsMutex);
    FreeCallbackRefs();
    JS_FreeContext(m_ctx); m_ctx = nullptr;
    js_std_free_handlers(m_rt);
    JS_FreeRuntime(m_rt);  m_rt  = nullptr;
}

void ScriptEngine::SetOutputCallback(ScriptOutputCallbackFunc cb)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    m_outputCallback = std::move(cb);
}

void ScriptEngine::EmitOutput(const std::string& line)
{
    ScriptOutputCallbackFunc cb;
    { std::lock_guard<std::mutex> lock(m_outputMutex); cb = m_outputCallback; }
    if (cb) cb(line);
    else    std::cerr << line << '\n';
}

bool ScriptEngine::ReportException(const std::string& ctx_name)
{
    JSValue exc = JS_GetException(m_ctx);

    // Convert to string — use JS_TAG checks to avoid triggering more JS calls
    // while exception handling might itself be in a bad state.
    JSValue msg = JS_ToString(m_ctx, exc);
    if (!JS_IsException(msg)) {
        const char* str = JS_ToCString(m_ctx, msg);
        if (str) { EmitOutput(std::string("[error] ") + str); JS_FreeCString(m_ctx, str); }
        JS_FreeValue(m_ctx, msg);
    } else {
        JS_FreeValue(m_ctx, JS_GetException(m_ctx)); // clear secondary exception
        EmitOutput("[error] (unknown exception in " + ctx_name + ")");
    }

    // Stack trace
    JSValue stack = JS_GetPropertyStr(m_ctx, exc, "stack");
    if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
        const char* st = JS_ToCString(m_ctx, stack);
        if (st && st[0] != '\0') { EmitOutput(st); }
        JS_FreeCString(m_ctx, st);
    }
    JS_FreeValue(m_ctx, stack);
    JS_FreeValue(m_ctx, exc);
    return false;
}

void ScriptEngine::FreeCallbackRefs()
{
    for (JSValue v : m_frameCallbackRefs) JS_FreeValue(m_ctx, v);
    m_frameCallbackRefs.clear();
    for (auto& [addr, refs] : m_bpCallbackRefs)
        for (JSValue v : refs) JS_FreeValue(m_ctx, v);
    m_bpCallbackRefs.clear();
}
