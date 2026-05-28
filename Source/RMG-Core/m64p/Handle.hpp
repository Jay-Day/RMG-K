/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_M64P_HANDLE_HPP
#define CORE_M64P_HANDLE_HPP

#include "../Library.hpp"

// Returns the dynamic library handle of the loaded mupen64plus-core instance.
// This is intended for optional features which resolve core-exported symbols at runtime
// (e.g. debugger helpers) without requiring RMG-Core internal C++ symbols to be exported.
CORE_EXPORT CoreLibraryHandle CoreGetM64PCoreHandle(void);

#endif // CORE_M64P_HANDLE_HPP

