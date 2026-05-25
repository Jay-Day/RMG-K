/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMGK_GEKKO_HPP
#define RMGK_GEKKO_HPP

#include "Emulation.hpp"
#include "RollbackNetcode.hpp"

#include <cstdint>
#include <string>
#include <vector>

class rmgk_gekko
{
  public:
    struct RemotePlayer
    {
        int player = 0;
        std::string address;
    };

    using InputProvider = bool (*)(uint32_t* inputs, int players, void* userData);
    using FrameCallback = bool (*)(void* userData);

    static bool start_p2p_session(const char* gameName, int players, int inputSize,
        int localPlayer, unsigned short localPort, const char* remoteIp, unsigned short remotePort, int localDelay, int predictionWindow);
    static bool start_p2p_session(const char* gameName, int players, int inputSize,
        int localPlayer, unsigned short localPort, const std::vector<RemotePlayer>& remotePlayers, int localDelay, int predictionWindow);
    static bool sync_prematch_manifest(int localPlayer);
    static bool start_local_session(const char* gameName, int players, int inputSize, int localDelay);
    static void close_session();
    static void request_stop();
    static bool is_netplay_session_active();
    static bool execute();
    static bool set_deterministic(bool enabled);
    static bool install_core_input_callback();
    static void clear_core_input_callback();
    static bool synchronize_input(void* values, int size, int players);
    static void set_debug_hooks(InputProvider inputProvider, FrameCallback beginFrame, FrameCallback endFrame, void* userData);
    static void set_debug_frame_output(int flags);
    static bool debug_run_frame_with_inputs(const uint32_t* inputs, int players, int flags);
    static bool toggle_client_input_replay();
};

#endif // RMGK_GEKKO_HPP
