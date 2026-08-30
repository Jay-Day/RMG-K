/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#include "LobbyIce.hpp"

#include <juice/juice.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace
{
struct Peer
{
    std::uint64_t userId = 0;
    juice_agent_t* agent = nullptr;
    std::mutex agentMutex;
    std::atomic<LobbyIcePeerState> state{LobbyIcePeerState::Disconnected};
    std::atomic<bool> active{true};
    std::mutex pingMutex;
    std::map<std::uint64_t, std::uint64_t> pingSentAtUs;
    bool hasRemoteDescription = false;
    bool remoteGatheringDone = false;
    std::vector<std::string> pendingCandidates;
};

struct QueuedPacket
{
    std::uint64_t peerUserId = 0;
    LobbyIceChannel channel = LobbyIceChannel::Gekko;
    std::uint64_t roundTripUs = 0;
    std::vector<char> data;
};

struct PendingRemoteSignal
{
    std::string kind;
    std::string value;
};

std::mutex g_mutex;
std::uint64_t g_localUserId = 0;
std::string g_stunHost;
std::uint16_t g_stunPort = 0;
std::vector<LobbyIceTurnServer> g_turnServers;
std::map<std::uint64_t, std::shared_ptr<Peer>> g_peers;
// Consecutive agents for a peer alternate between disjoint IANA dynamic-port
// ranges. This guarantees that an ICE restart cannot reuse its prior local UDP
// port while still leaving thousands of ports available if individual binds
// collide with another process or peer agent.
std::map<std::uint64_t, bool> g_nextPeerUsesUpperPortRange;
std::map<std::uint64_t, std::vector<PendingRemoteSignal>> g_pendingRemoteSignals;
std::deque<LobbyIceSignal> g_localSignals;
std::deque<QueuedPacket> g_packets;
std::atomic<bool> g_diagnosticsEnabled{false};
std::vector<LobbyIceDiagnostic> g_diagnostics;
std::size_t g_diagnosticsDropped = 0;
constexpr std::size_t DIAGNOSTIC_QUEUE_CAP = 4096;
constexpr std::uint16_t DYNAMIC_PORT_RANGE_BEGIN = 49152;
constexpr std::uint16_t DYNAMIC_PORT_RANGE_MIDDLE = 57344;
constexpr std::uint16_t DYNAMIC_PORT_RANGE_END = 65535;

std::uint64_t steady_now_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t read_be_u64(const char* data)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    return value;
}

const char* diagnostic_level_name(juice_log_level_t level)
{
    switch (level)
    {
    case JUICE_LOG_LEVEL_VERBOSE: return "verbose";
    case JUICE_LOG_LEVEL_DEBUG: return "debug";
    case JUICE_LOG_LEVEL_INFO: return "info";
    case JUICE_LOG_LEVEL_WARN: return "warn";
    case JUICE_LOG_LEVEL_ERROR: return "error";
    case JUICE_LOG_LEVEL_FATAL: return "fatal";
    case JUICE_LOG_LEVEL_NONE:
    default: return "unknown";
    }
}

void on_libjuice_log(juice_log_level_t level, const char* message)
{
    if (!g_diagnosticsEnabled.load(std::memory_order_relaxed) || message == nullptr)
        return;

    // DEBUG otherwise emits two lines for every gameplay datagram. Those say
    // only that application traffic arrived, add no ICE diagnostic value, and
    // would make an opt-in connection trace enormous during a match.
    const std::string_view text(message);
    if (text.find("Received non-STUN datagram") != std::string_view::npos ||
        text.find("Received application datagram") != std::string_view::npos ||
        text.find("STUN selected entry matching incoming address") != std::string_view::npos ||
        text.find("STUN selected entry matching incoming relayed address") != std::string_view::npos)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_diagnostics.size() >= DIAGNOSTIC_QUEUE_CAP)
    {
        ++g_diagnosticsDropped;
        return;
    }
    g_diagnostics.push_back({diagnostic_level_name(level), message});
}

LobbyIcePeerState translate_state(juice_state_t state)
{
    switch (state)
    {
    case JUICE_STATE_GATHERING: return LobbyIcePeerState::Gathering;
    case JUICE_STATE_CONNECTING: return LobbyIcePeerState::Connecting;
    case JUICE_STATE_CONNECTED: return LobbyIcePeerState::Connected;
    case JUICE_STATE_COMPLETED: return LobbyIcePeerState::Completed;
    case JUICE_STATE_FAILED: return LobbyIcePeerState::Failed;
    case JUICE_STATE_DISCONNECTED:
    default: return LobbyIcePeerState::Disconnected;
    }
}

void on_state_changed(juice_agent_t*, juice_state_t state, void* userPtr)
{
    auto* peer = static_cast<Peer*>(userPtr);
    if (peer != nullptr && peer->active.load(std::memory_order_relaxed))
    {
        peer->state.store(translate_state(state), std::memory_order_relaxed);
    }
}

void on_candidate(juice_agent_t*, const char* sdp, void* userPtr)
{
    auto* peer = static_cast<Peer*>(userPtr);
    if (peer == nullptr || sdp == nullptr || !peer->active.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_localSignals.push_back({peer->userId, "candidate", sdp});
}

void on_gathering_done(juice_agent_t*, void* userPtr)
{
    auto* peer = static_cast<Peer*>(userPtr);
    if (peer == nullptr || !peer->active.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_localSignals.push_back({peer->userId, "gathering_done", {}});
}

void on_recv(juice_agent_t* agent, const char* data, size_t size, void* userPtr)
{
    auto* peer = static_cast<Peer*>(userPtr);
    if (peer == nullptr || data == nullptr || size < 1 ||
        !peer->active.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto channel = static_cast<LobbyIceChannel>(static_cast<std::uint8_t>(data[0]));
    if (channel != LobbyIceChannel::Ping && channel != LobbyIceChannel::Prematch &&
        channel != LobbyIceChannel::Gekko)
    {
        return;
    }

    // Ping payload: request/reply byte plus nonce. Echo requests directly from
    // libjuice's receive callback so the 25 ms Qt queue-poll interval is not
    // counted as network latency. The send timestamp is tracked privately per
    // peer, keeping the on-wire packet compatible with the first ICE client.
    if (channel == LobbyIceChannel::Ping && size == 10 && data[1] == 1)
    {
        std::vector<char> reply(data, data + size);
        reply[1] = 2;
        if (juice_send(agent, reply.data(), reply.size()) == JUICE_ERR_SUCCESS)
            return;
        // If the immediate send ever fails, queue it for the existing UI-side
        // fallback and normal retry machinery.
    }

    QueuedPacket packet;
    packet.peerUserId = peer->userId;
    packet.channel = channel;
    if (channel == LobbyIceChannel::Ping && size == 10 && data[1] == 2)
    {
        const std::uint64_t nonce = read_be_u64(data + 2);
        const std::uint64_t receivedAtUs = steady_now_us();
        std::lock_guard<std::mutex> pingLock(peer->pingMutex);
        const auto it = peer->pingSentAtUs.find(nonce);
        if (it != peer->pingSentAtUs.end())
        {
            if (receivedAtUs >= it->second)
                packet.roundTripUs = receivedAtUs - it->second;
            peer->pingSentAtUs.erase(it);
        }
    }
    packet.data.assign(data + 1, data + size);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_packets.push_back(std::move(packet));
}

std::shared_ptr<Peer> find_peer(std::uint64_t peerUserId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_peers.find(peerUserId);
    return it == g_peers.end() ? nullptr : it->second;
}

bool apply_remote_signal(const std::shared_ptr<Peer>& peer,
    const std::string& kind, const std::string& value)
{
    if (peer == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (peer->agent == nullptr || !peer->active.load(std::memory_order_relaxed))
        return false;

    if (kind == "description")
    {
        if (value.empty() || juice_set_remote_description(peer->agent, value.c_str()) != JUICE_ERR_SUCCESS)
        {
            return false;
        }
        peer->hasRemoteDescription = true;
        for (const std::string& candidate : peer->pendingCandidates)
        {
            juice_add_remote_candidate(peer->agent, candidate.c_str());
        }
        peer->pendingCandidates.clear();
        if (peer->remoteGatheringDone)
        {
            juice_set_remote_gathering_done(peer->agent);
        }
        return true;
    }
    if (kind == "candidate")
    {
        if (value.empty())
        {
            return false;
        }
        if (!peer->hasRemoteDescription)
        {
            peer->pendingCandidates.push_back(value);
            return true;
        }
        return juice_add_remote_candidate(peer->agent, value.c_str()) == JUICE_ERR_SUCCESS;
    }
    if (kind == "gathering_done")
    {
        peer->remoteGatheringDone = true;
        return !peer->hasRemoteDescription ||
            juice_set_remote_gathering_done(peer->agent) == JUICE_ERR_SUCCESS;
    }
    return false;
}
} // namespace

void LobbyIce::set_diagnostics_enabled(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_diagnostics.clear();
        g_diagnosticsDropped = 0;
    }
    g_diagnosticsEnabled.store(enabled, std::memory_order_relaxed);
    if (enabled)
    {
        juice_set_log_handler(on_libjuice_log);
        // DEBUG captures candidate gathering, STUN binding, pair checks, role
        // conflicts, and timeouts without VERBOSE's per-datagram chatter.
        juice_set_log_level(JUICE_LOG_LEVEL_DEBUG);
    }
    else
    {
        juice_set_log_level(JUICE_LOG_LEVEL_WARN);
        juice_set_log_handler(nullptr);
    }
}

std::vector<LobbyIceDiagnostic> LobbyIce::take_diagnostics()
{
    std::vector<LobbyIceDiagnostic> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    out.swap(g_diagnostics);
    if (g_diagnosticsDropped != 0)
    {
        out.push_back({"warn", "Diagnostic queue overflow: " +
            std::to_string(g_diagnosticsDropped) + " libjuice messages dropped"});
        g_diagnosticsDropped = 0;
    }
    return out;
}

bool LobbyIce::configure(std::uint64_t localUserId,
    const std::string& stunHost, std::uint16_t stunPort,
    const std::vector<LobbyIceTurnServer>& turnServers)
{
    reset();
    if (localUserId == 0 || stunHost.empty() || stunPort == 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_localUserId = localUserId;
    g_stunHost = stunHost;
    g_stunPort = stunPort;
    g_turnServers = turnServers;
    return true;
}

void LobbyIce::reset()
{
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& [id, peer] : g_peers)
        {
            (void)id;
            peer->active.store(false, std::memory_order_relaxed);
            peers.push_back(peer);
        }
        g_peers.clear();
        g_nextPeerUsesUpperPortRange.clear();
        g_pendingRemoteSignals.clear();
        g_localSignals.clear();
        g_packets.clear();
        g_localUserId = 0;
        g_stunHost.clear();
        g_stunPort = 0;
        g_turnServers.clear();
    }
    for (const auto& peer : peers)
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        if (peer->agent != nullptr)
        {
            juice_destroy(peer->agent);
            peer->agent = nullptr;
        }
    }
}

bool LobbyIce::add_peer(std::uint64_t peerUserId)
{
    std::string stunHost;
    std::uint16_t stunPort = 0;
    bool useUpperPortRange = false;
    std::vector<LobbyIceTurnServer> turnServers;
    std::vector<PendingRemoteSignal> pending;
    auto peer = std::make_shared<Peer>();
    peer->userId = peerUserId;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (peerUserId == 0 || peerUserId == g_localUserId || g_localUserId == 0 ||
            g_stunHost.empty() || g_stunPort == 0)
        {
            return false;
        }
        if (g_peers.find(peerUserId) != g_peers.end())
        {
            return true;
        }
        useUpperPortRange = g_nextPeerUsesUpperPortRange[peerUserId];
        g_nextPeerUsesUpperPortRange[peerUserId] = !useUpperPortRange;
        stunHost = g_stunHost;
        stunPort = g_stunPort;
        turnServers = g_turnServers;
        auto pendingIt = g_pendingRemoteSignals.find(peerUserId);
        if (pendingIt != g_pendingRemoteSignals.end())
        {
            pending = std::move(pendingIt->second);
            g_pendingRemoteSignals.erase(pendingIt);
        }
        g_peers.emplace(peerUserId, peer);
    }

    std::vector<juice_turn_server_t> juiceTurnServers;
    juiceTurnServers.reserve(turnServers.size());
    for (const LobbyIceTurnServer& server : turnServers)
    {
        juiceTurnServers.push_back({server.host.c_str(), server.username.c_str(),
                                    server.password.c_str(), server.port});
    }

    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    config.stun_server_host = stunHost.c_str();
    config.stun_server_port = stunPort;
    config.local_port_range_begin = useUpperPortRange ?
        DYNAMIC_PORT_RANGE_MIDDLE : DYNAMIC_PORT_RANGE_BEGIN;
    config.local_port_range_end = useUpperPortRange ?
        DYNAMIC_PORT_RANGE_END : static_cast<std::uint16_t>(DYNAMIC_PORT_RANGE_MIDDLE - 1);
    config.turn_servers = juiceTurnServers.empty() ? nullptr : juiceTurnServers.data();
    config.turn_servers_count = static_cast<int>(juiceTurnServers.size());
    config.cb_state_changed = on_state_changed;
    config.cb_candidate = on_candidate;
    config.cb_gathering_done = on_gathering_done;
    config.cb_recv = on_recv;
    config.user_ptr = peer.get();
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        peer->agent = juice_create(&config);
    }
    char description[JUICE_MAX_SDP_STRING_LEN]{};
    bool haveDescription = false;
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        haveDescription = peer->agent != nullptr &&
            juice_get_local_description(peer->agent, description, sizeof(description)) == JUICE_ERR_SUCCESS;
    }
    if (!haveDescription || description[0] == '\0')
    {
        remove_peer(peerUserId);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_localSignals.push_back({peerUserId, "description", description});
    }

    for (const PendingRemoteSignal& signal : pending)
    {
        apply_remote_signal(peer, signal.kind, signal.value);
    }
    int gatherResult = JUICE_ERR_FAILED;
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        if (peer->agent != nullptr)
            gatherResult = juice_gather_candidates(peer->agent);
    }
    if (gatherResult != JUICE_ERR_SUCCESS)
    {
        remove_peer(peerUserId);
        return false;
    }
    return true;
}

void LobbyIce::remove_peer(std::uint64_t peerUserId)
{
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Removing a peer is also the generation boundary for any signaling
        // that arrived before its agent existed. Otherwise an old description
        // queued during a room-state race can poison the replacement agent
        // before its fresh credentials arrive.
        g_pendingRemoteSignals.erase(peerUserId);
        const auto it = g_peers.find(peerUserId);
        if (it == g_peers.end())
        {
            return;
        }
        peer = it->second;
        peer->active.store(false, std::memory_order_relaxed);
        g_peers.erase(it);
        std::erase_if(g_localSignals, [peerUserId](const LobbyIceSignal& signal) {
            return signal.peerUserId == peerUserId;
        });
        std::erase_if(g_packets, [peerUserId](const QueuedPacket& packet) {
            return packet.peerUserId == peerUserId;
        });
    }
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        if (peer->agent != nullptr)
        {
            juice_destroy(peer->agent);
            peer->agent = nullptr;
        }
    }
}

bool LobbyIce::has_peer(std::uint64_t peerUserId)
{
    return find_peer(peerUserId) != nullptr;
}

LobbyIcePeerState LobbyIce::peer_state(std::uint64_t peerUserId)
{
    const auto peer = find_peer(peerUserId);
    return peer == nullptr ? LobbyIcePeerState::Disconnected :
        peer->state.load(std::memory_order_relaxed);
}

bool LobbyIce::peer_connected(std::uint64_t peerUserId)
{
    const LobbyIcePeerState state = peer_state(peerUserId);
    return state == LobbyIcePeerState::Connected || state == LobbyIcePeerState::Completed;
}

std::string LobbyIce::peer_selected_addresses(std::uint64_t peerUserId)
{
    const auto peer = find_peer(peerUserId);
    if (peer == nullptr || !peer_connected(peerUserId))
    {
        return {};
    }
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (peer->agent == nullptr || !peer->active.load(std::memory_order_relaxed))
        return {};
    char local[JUICE_MAX_ADDRESS_STRING_LEN]{};
    char remote[JUICE_MAX_ADDRESS_STRING_LEN]{};
    if (juice_get_selected_addresses(peer->agent, local, sizeof(local), remote, sizeof(remote)) != JUICE_ERR_SUCCESS)
    {
        return {};
    }
    return std::string(local) + " -> " + remote;
}

std::vector<LobbyIceSignal> LobbyIce::take_local_signals()
{
    std::vector<LobbyIceSignal> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    out.reserve(g_localSignals.size());
    while (!g_localSignals.empty())
    {
        out.push_back(std::move(g_localSignals.front()));
        g_localSignals.pop_front();
    }
    return out;
}

bool LobbyIce::handle_remote_signal(std::uint64_t peerUserId,
    const std::string& kind, const std::string& value)
{
    const auto peer = find_peer(peerUserId);
    if (peer == nullptr)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingRemoteSignals[peerUserId].push_back({kind, value});
        return true;
    }
    return apply_remote_signal(peer, kind, value);
}

bool LobbyIce::send(std::uint64_t peerUserId, LobbyIceChannel channel,
    const char* data, std::size_t size)
{
    const auto peer = find_peer(peerUserId);
    if (peer == nullptr || !peer_connected(peerUserId) ||
        (data == nullptr && size != 0) || size > 4000)
    {
        return false;
    }
    std::vector<char> framed(size + 1);
    framed[0] = static_cast<char>(channel);
    if (size > 0)
    {
        std::copy_n(data, size, framed.data() + 1);
    }
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (peer->agent == nullptr || !peer->active.load(std::memory_order_relaxed))
        return false;

    std::uint64_t pingNonce = 0;
    std::uint64_t pingSentAtUs = 0;
    if (channel == LobbyIceChannel::Ping && size == 9 && framed[1] == 1)
    {
        pingNonce = read_be_u64(framed.data() + 2);
        pingSentAtUs = steady_now_us();
        std::lock_guard<std::mutex> pingLock(peer->pingMutex);
        const std::uint64_t cutoffUs = pingSentAtUs > 60'000'000
            ? pingSentAtUs - 60'000'000 : 0;
        std::erase_if(peer->pingSentAtUs, [cutoffUs](const auto& entry) {
            return entry.second < cutoffUs;
        });
        peer->pingSentAtUs[pingNonce] = pingSentAtUs;
    }
    const bool sent = juice_send(peer->agent, framed.data(), framed.size()) == JUICE_ERR_SUCCESS;
    if (!sent && pingSentAtUs != 0)
    {
        std::lock_guard<std::mutex> pingLock(peer->pingMutex);
        const auto it = peer->pingSentAtUs.find(pingNonce);
        if (it != peer->pingSentAtUs.end() && it->second == pingSentAtUs)
            peer->pingSentAtUs.erase(it);
    }
    return sent;
}

std::vector<LobbyIcePacket> LobbyIce::take_packets(LobbyIceChannel channel)
{
    std::vector<LobbyIcePacket> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_packets.begin(); it != g_packets.end();)
    {
        if (it->channel == channel)
        {
            out.push_back({it->peerUserId, it->roundTripUs, std::move(it->data)});
            it = g_packets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return out;
}
