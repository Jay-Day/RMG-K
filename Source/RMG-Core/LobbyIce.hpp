/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef RMGK_LOBBY_ICE_HPP
#define RMGK_LOBBY_ICE_HPP

#include "Library.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum class LobbyIceChannel : std::uint8_t
{
    Ping = 1,
    Prematch = 2,
    Gekko = 3,
};

enum class LobbyIcePeerState
{
    Disconnected = 0,
    Gathering,
    Connecting,
    Connected,
    Completed,
    Failed,
};

struct LobbyIceTurnServer
{
    std::string host;
    std::uint16_t port = 3478;
    std::string username;
    std::string password;
};

struct LobbyIceSignal
{
    std::uint64_t peerUserId = 0;
    std::string kind; // "description", "candidate", or "gathering_done"
    std::string value;
};

struct LobbyIcePacket
{
    std::uint64_t peerUserId = 0;
    std::vector<char> data;
};

// Process-wide room ICE mesh. Lobby signaling calls this API from the Qt UI
// thread; GekkoNet consumes the same agents from the emulation thread.
class LobbyIce
{
  public:
    static CORE_EXPORT bool configure(std::uint64_t localUserId,
        const std::string& stunHost, std::uint16_t stunPort,
        const std::vector<LobbyIceTurnServer>& turnServers = {});
    static CORE_EXPORT void reset();

    static CORE_EXPORT bool add_peer(std::uint64_t peerUserId);
    static CORE_EXPORT void remove_peer(std::uint64_t peerUserId);
    static CORE_EXPORT bool has_peer(std::uint64_t peerUserId);
    static CORE_EXPORT LobbyIcePeerState peer_state(std::uint64_t peerUserId);
    static CORE_EXPORT bool peer_connected(std::uint64_t peerUserId);
    static CORE_EXPORT std::string peer_selected_addresses(std::uint64_t peerUserId);

    static CORE_EXPORT std::vector<LobbyIceSignal> take_local_signals();
    static CORE_EXPORT bool handle_remote_signal(std::uint64_t peerUserId,
        const std::string& kind, const std::string& value);

    static CORE_EXPORT bool send(std::uint64_t peerUserId, LobbyIceChannel channel,
        const char* data, std::size_t size);
    static CORE_EXPORT std::vector<LobbyIcePacket> take_packets(LobbyIceChannel channel);
};

#endif // RMGK_LOBBY_ICE_HPP
