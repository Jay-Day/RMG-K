/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_HPP
#define CORE_HPP

// initializes the core library,
// returns false when failed
bool CoreInit(void);

// shuts down the core library
void CoreShutdown(void);

// Marks this process as a guest in another RMG-K's user folders (a live
// replay viewer window): it reads the config and caches but never writes
// them, so two processes can't overwrite each other's files. Covers the
// mupen64plus core and the plugins too, through the environment variable
// below. Call before CoreInit.
inline constexpr char CoreReadOnlyUserFilesVariable[] = "RMGK_READ_ONLY_USER_FILES";
void CoreSetUserFilesReadOnly(void);
bool CoreUserFilesReadOnly(void);

#endif // CORE_HPP
