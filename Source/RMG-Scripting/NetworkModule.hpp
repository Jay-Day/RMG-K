/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_NETWORK_MODULE_HPP
#define RMGK_NETWORK_MODULE_HPP

extern "C" { typedef struct JSContext JSContext; }

namespace RMGScript {
// Registers the global fetch() function into the JS context.
void RegisterNetworkModule(JSContext* ctx);
} // namespace RMGScript

#endif // RMGK_NETWORK_MODULE_HPP
