/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "Library.hpp"
#include "ScriptingHooks.hpp"

static ScriptingHooks s_hooks;

CORE_EXPORT void CoreSetScriptingHooks(const ScriptingHooks& hooks)
{
    s_hooks = hooks;
}

const ScriptingHooks& CoreGetScriptingHooks()
{
    return s_hooks;
}
