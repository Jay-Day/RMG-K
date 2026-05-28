/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ScriptManager.hpp"
#include "ScriptEngine.hpp"

#include <iostream>

ScriptManager& ScriptManager::GetInstance() {
    static ScriptManager instance;
    return instance;
}

bool ScriptManager::LoadScript(const std::string& filepath) {
    if (m_engines.count(filepath)) {
        if (m_outputCallback)
            m_outputCallback("[already running] " + filepath);
        return false;
    }

    auto engine = std::make_unique<ScriptEngine>();
    if (!engine->Initialize()) {
        std::cerr << "ScriptManager: failed to initialize engine for " << filepath << std::endl;
        return false;
    }

    // Capture the filepath so the output prefix identifies the script.
    std::string fp = filepath;
    std::function<void(const std::string&)> cb = m_outputCallback;
    engine->SetOutputCallback([fp, cb](const std::string& line) {
        if (cb) cb(line);
    });

    if (!engine->LoadScript(filepath)) {
        return false;
    }

    m_engines[filepath] = std::move(engine);
    return true;
}

bool ScriptManager::StopScript(const std::string& filepath) {
    auto it = m_engines.find(filepath);
    if (it == m_engines.end()) return false;
    it->second->Shutdown();
    m_engines.erase(it);
    return true;
}

void ScriptManager::StopAll() {
    for (auto& [fp, engine] : m_engines)
        engine->Shutdown();
    m_engines.clear();
}

std::vector<std::string> ScriptManager::GetRunningScripts() const {
    std::vector<std::string> result;
    result.reserve(m_engines.size());
    for (const auto& [fp, _] : m_engines)
        result.push_back(fp);
    return result;
}

bool ScriptManager::IsScriptRunning(const std::string& filepath) const {
    return m_engines.count(filepath) > 0;
}

void ScriptManager::SetOutputCallback(std::function<void(const std::string&)> callback) {
    m_outputCallback = std::move(callback);
    // Update all already-running engines.
    std::function<void(const std::string&)> cb = m_outputCallback;
    for (auto& [fp, engine] : m_engines) {
        engine->SetOutputCallback(cb);
    }
}

void ScriptManager::OnFrameUpdate(uint32_t pc) {
    for (auto& [fp, engine] : m_engines) {
        engine->FireFrameCallbacks();
        if (pc != 0)
            engine->FireBreakpointCallback(pc);
    }
}

void ScriptManager::OnEmulationStop() {
    StopAll();
}

void ScriptManager::OnEmulationPause() {
}

void ScriptManager::OnEmulationResume() {
}
