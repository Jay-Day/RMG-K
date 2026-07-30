#include "backend.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>

namespace
{
    u16 GenerateSessionMagic()
    {
        static std::atomic<unsigned int> counter{1};

        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const unsigned int sequence = counter.fetch_add(1, std::memory_order_relaxed);
        std::uintptr_t stack_addr = reinterpret_cast<std::uintptr_t>(&sequence);

        std::uint64_t mixed = static_cast<std::uint64_t>(now);
        mixed ^= static_cast<std::uint64_t>(sequence) * 0x9e3779b97f4a7c15ull;
        mixed ^= static_cast<std::uint64_t>(stack_addr);
        mixed ^= mixed >> 33;
        mixed *= 0xff51afd7ed558ccdull;
        mixed ^= mixed >> 33;

        u16 magic = static_cast<u16>((mixed ^ (mixed >> 16) ^ (mixed >> 32) ^ (mixed >> 48)) & 0xffffu);
        return magic == 0 ? 1 : magic;
    }
}

// register poly types.
namespace
{
    zpp::serializer::register_types<
        zpp::serializer::make_type<Gekko::SyncMsg, zpp::serializer::make_id("Gekko::SyncMsg")>,
        zpp::serializer::make_type<Gekko::InputMsg, zpp::serializer::make_id("Gekko::InputMsg")>,
        zpp::serializer::make_type<Gekko::InputAckMsg, zpp::serializer::make_id("Gekko::InputAckMsg")>,
        zpp::serializer::make_type<Gekko::SessionHealthMsg, zpp::serializer::make_id("Gekko::SessionHealthMsg")>,
        zpp::serializer::make_type<Gekko::NetworkHealthMsg, zpp::serializer::make_id("Gekko::NetworkHealthMsg")>
    > _;
}

Gekko::MessageSystem::MessageSystem()
{
    _num_players = 0;
	_input_size = 0;
	_last_sent_network_check = 0;

	_session_magic = GenerateSessionMagic();

    session_events = SessionEventSystem();
}

void Gekko::MessageSystem::Init(u8 num_players, u32 input_size)
{
    _num_players = num_players;
	_input_size = input_size;

    _net_player_queue.resize(num_players);

}


bool Gekko::InputCache::IsValid(Frame current_ack, Frame current_last_input) const
{
    return !packets.empty()
        && last_acked_frame == current_ack
        && last_input_frame == current_last_input;
}

void Gekko::MessageSystem::NetInputQueue::TrimToAck(Frame min_ack, u32 max_size)
{
    if (inputs.empty()) return;

    if (min_ack != INT_MAX) {
        const Frame oldest = last_added_input - (Frame)inputs.size() + 1;
        const Frame acked = std::max((Frame)0, min_ack - oldest + 1);
        const u32 target = (u32)inputs.size() - std::min((u32)acked, (u32)inputs.size());

        while (inputs.size() > target) {
            inputs.pop_front();
        }
        return;
    }

    while (inputs.size() > max_size) {
        inputs.pop_front();
    }
}

void Gekko::MessageSystem::AddInput(Frame input_frame, Handle player, u8 input[], bool remote)
{
    auto& input_q = _net_player_queue[player];
	if (input_q.last_added_input + 1 == input_frame) {
        input_q.last_added_input++;
        input_q.inputs.push_back(std::make_unique<u8[]>(_input_size));
        std::memcpy(input_q.inputs.back().get(), input, _input_size);
	}

    // discard acked inputs (local) or just cap the queue (remote)
    Frame min_ack = remote ? (Frame)INT_MAX : GetMinLastAckedFrame(false);
    input_q.TrimToAck(min_ack, MAX_INPUT_QUEUE_SIZE);
}

void Gekko::MessageSystem::AddSpectatorInput(Frame input_frame, u8 input[])
{
    auto& input_q = _net_spectator_queue;
	if (input_q.last_added_input + 1 == input_frame) {
        input_q.last_added_input++;
        const size_t num_players = locals.size() + remotes.size();
        input_q.inputs.push_back(std::make_unique<u8[]>(_input_size * num_players));
        std::memcpy(input_q.inputs.back().get(), input, _input_size * num_players);
	}

    // discard acked inputs and cap the queue
    input_q.TrimToAck(GetMinLastAckedFrame(true), MAX_INPUT_QUEUE_SIZE);
}

void Gekko::MessageSystem::SendPendingOutput(GekkoNetAdapter* host)
{
	// send per-peer input packets to remotes
	if (!remotes.empty() && !locals.empty()) {
		for (auto& peer : remotes) {
			SendInputsToPeer(peer.get(), host, false);
		}
	}
	// check for disconnects (runs even in spectator sessions with no locals)
	if (!remotes.empty()) {
		HandleTooFarBehindActors(false);
	}

	// send per-peer input packets to spectators
	if (!spectators.empty() && !locals.empty()) {
		for (auto& peer : spectators) {
			SendInputsToPeer(peer.get(), host, true);
		}
	}
	// check for disconnects
	if (!spectators.empty()) {
		HandleTooFarBehindActors(true);
	}

	// drain remaining messages (acks, sync, health, etc.)
	while (!_pending_output.empty()) {
		auto& pkt = _pending_output.front();
        if ((pkt->pkt.header.type == SessionHealth || pkt->pkt.header.type == NetworkHealth) && pkt->addr.GetSize() == 0) {
            // send to remotes
            SendDataToAll(pkt.get(), host);
            // send to spectators
            SendDataToAll(pkt.get(), host, true);
        }
		else {
			SendDataTo(pkt.get(), host);
		}
		_pending_output.pop();
	}
}

void Gekko::MessageSystem::HandleData(GekkoNetAdapter* host, GekkoNetResult** data, u32 length)
{
    for (u32 i = 0; i < length; i++) {
        auto res = data[i];
        auto addr = NetAddress(res->addr.data, res->addr.size);

        _bin_buffer.clear();

        try {
            _bin_buffer.insert(_bin_buffer.begin(), (u8*)res->data, (u8*)res->data + res->data_len);

            NetPacket pkt;
            zpp::serializer::memory_input_archive in(_bin_buffer);
            in(pkt.header, pkt.body);

            ParsePacket(addr, pkt, res->data_len);
        }
        catch (const std::exception&) {
            // Stray non-GekkoNet UDP datagram (e.g. leftover ping probe or NAT punch) — ignore.
        }

        // cleanup :)
        host->free_data(res->addr.data);
        host->free_data(res->data);
        host->free_data(res);
    }
}

void Gekko::MessageSystem::SendSyncRequest(NetAddress* addr)
{
    if (!addr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
	auto& message = _pending_output.back();

	message->addr.Copy(addr);

	message->pkt.header.type = SyncRequest;
	message->pkt.header.magic = 0;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);
}

void Gekko::MessageSystem::SendSyncResponse(NetAddress* addr, u16 magic)
{
    if (!addr || magic == 0) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(addr);
	message->pkt.header.type = SyncResponse;
	message->pkt.header.magic = magic;

    auto body = std::make_unique<SyncMsg>();
    body->rng_data = _session_magic;

    message->pkt.body = std::move(body);
}

void Gekko::MessageSystem::SendInputAck(Handle player, Frame frame, i8 local_advantage)
{
	auto plyr = GetPlayerByHandle(player);

    if (!plyr) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

	message->addr.Copy(&plyr->address);
	message->pkt.header.magic = plyr->session_magic;
	message->pkt.header.type = InputAck;

    auto body = std::make_unique<InputAckMsg>();
	body->ack_frame = frame;
	body->frame_advantage = local_advantage;

    message->pkt.body = std::move(body);
}

std::vector<Handle> Gekko::MessageSystem::GetRemoteHandlesForAddress(NetAddress* addr)
{
	auto result = std::vector<Handle>();
	for (auto& player: remotes) {
		if (player->address.Equals(*addr)) {
			result.push_back(player->handle);
		}
	}
	return result;
}

Gekko::Player* Gekko::MessageSystem::GetPlayerByHandle(Handle handle) 
{
    std::vector<std::unique_ptr<Player>>* current = &locals;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &remotes;
        }

        for (auto& player : *current) {
            if (player->handle == handle) {
                return player.get();
            }
        }
    }

	return nullptr;
}

Frame Gekko::MessageSystem::GetMinLastAckedFrame(bool spectator) 
{
	Frame min = INT_MAX;
	for (auto& player : spectator ? spectators : remotes) {
		if (player->GetStatus() == Connected) {
			min = std::min(player->stats.last_acked_frame, min);
		}
	}
	return min;
}

Frame Gekko::MessageSystem::GetLastAddedInput(bool spectator)
{
    if (spectator) return _net_spectator_queue.last_added_input;

    Frame result = INT_MAX;
    for (auto& local : locals) {
        result = std::min(_net_player_queue[local->handle].last_added_input, result);
    }

    return result;
}

bool Gekko::MessageSystem::CheckStatusActors()
{
	i32 result = 0;
	u64 now = TimeSinceEpoch();

    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }
        for (auto& player : *current) {
            if (player->GetStatus() == Initiating) {
                if (player->stats.last_sent_sync_message + NetStats::SYNC_MSG_DELAY < now) {
                    if (player->sync_num == 0) {
                        SendSyncRequest(&player->address);
                        player->stats.last_sent_sync_message = now;
                    }
                    else if (player->sync_num < NUM_TO_SYNC) {
                        SendSyncResponse(&player->address, player->session_magic);
                        player->stats.last_sent_sync_message = now;
                    }
                    else {
                        player->SetStatus(Connected);
                        session_events.AddPlayerConnectedEvent(player->handle);
                        result++;
                    }
                }
                result--;
            }
        }
    }

	return result == 0;
}

void Gekko::MessageSystem::SendSessionHealth(Frame frame, u32 checksum)
{
    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = SessionHealth;

    auto body = std::make_unique<SessionHealthMsg>();
    body->frame = frame;
    body->checksum = checksum;

    message->pkt.body = std::move(body);
}

void Gekko::MessageSystem::SendNetworkHealth()
{
    u64 now = TimeSinceEpoch();

    // dont want to spam the network with network health packets
    if (_last_sent_network_check + NetStats::NET_CHECK_DELAY > now) {
        return;
    }

    _pending_output.push(std::make_unique<NetData>());
    auto& message = _pending_output.back();

    // the address and magic is set later so dont worry about it now
    message->pkt.header.type = NetworkHealth;

    auto body = std::make_unique<NetworkHealthMsg>();
    body->send_time = now;
    body->received = false;

    message->pkt.body = std::move(body);

    _last_sent_network_check = now;
}

Frame Gekko::MessageSystem::GetLastAddedInputFrom(Handle player)
{
    return _net_player_queue[player].last_added_input;
}

std::deque<std::unique_ptr<u8[]>>& Gekko::MessageSystem::GetNetPlayerQueue(Handle player)
{
    return _net_player_queue[player].inputs;
}

void Gekko::MessageSystem::HandleTooFarBehindActors(bool spectator)
{
    const u64 now = TimeSinceEpoch();
	for (auto& actor : spectator ? spectators : remotes) {
		if (actor->GetStatus() == Connected) {
            // give the actor a chance to save itself.
            if (actor->stats.last_received_message == 0) {
                actor->stats.last_received_message = now;
                continue;
            }
            // check whether messages are being sent if not disconnect.
            const u64 msg_diff = now - actor->stats.last_received_message;
			if (msg_diff > NetStats::DISCONNECT_TIMEOUT) {
                session_events.AddPlayerDisconnectedEvent(actor->handle);
                actor->SetStatus(Disconnected);
                actor->sync_num = 0;
			}
		}
	}
}

u64 Gekko::MessageSystem::TimeSinceEpoch()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Gekko::MessageSystem::SendDataToAll(NetData* pkt, GekkoNetAdapter* host, bool spectators_only)
{
    auto& actors = spectators_only ? spectators : remotes;

    std::vector<u8> body_buffer;

    try {
        zpp::serializer::memory_output_archive out(body_buffer);
        out(pkt->pkt.body);
    }
    catch (const std::exception&)
    {
        printf("failed to serialize packet body\n");
        return;
    }


    for (auto& actor : actors) {
        _bin_buffer.clear();
        if (actor->address.GetSize() != 0 && actor->GetStatus() != Disconnected) {

            pkt->addr.Copy(&actor->address);
            pkt->pkt.header.magic = actor->session_magic;

            try {
                zpp::serializer::memory_output_archive out(_bin_buffer);
                out(pkt->pkt.header);
            }
            catch (const std::exception&)
            {
                printf("failed to serialize packet header\n");
                continue;
            }

            _bin_buffer.insert(
                _bin_buffer.end(),
                body_buffer.begin(),
                body_buffer.end()
            );

            auto addr = GekkoNetAddress();
            addr.data = actor->address.GetAddress();
            addr.size = actor->address.GetSize();

            host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());
            actor->stats.bytes_sent_accum += (u32)_bin_buffer.size();
        }
    }
}

void Gekko::MessageSystem::SendDataTo(NetData* pkt, GekkoNetAdapter* host)
{
    _bin_buffer.clear();

    try {
        zpp::serializer::memory_output_archive out(_bin_buffer);
        out(pkt->pkt.header, pkt->pkt.body);
    }
    catch (const std::exception&)
    {
        printf("failed to serialize packet\n");
        return;
    }

    auto addr = GekkoNetAddress();
    addr.data = pkt->addr.GetAddress();
    addr.size = pkt->addr.GetSize();

    host->send_data(&addr, (char*)_bin_buffer.data(), (int)_bin_buffer.size());

    u32 sent_size = (u32)_bin_buffer.size();
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& actor : *current) {
            if (actor->address.Equals(pkt->addr)) {
                actor->stats.bytes_sent_accum += sent_size;
            }
        }
    }
}

void Gekko::MessageSystem::ParsePacket(NetAddress& addr, NetPacket& pkt, u32 packet_size)
{
    u64 now = TimeSinceEpoch();
    // update receive timers.
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                player->stats.last_received_message = now;
                player->stats.bytes_received_accum += packet_size;
            }
        }
    }

    // handle packet.
    if (pkt.header.magic != _session_magic) {
        if (pkt.header.type == SyncRequest) {
            OnSyncRequest(addr, pkt);
        }
        else {
            printf("dropped packet!\n");
        }
    }
    else {
        switch (pkt.header.type)
        {
        case SyncResponse:
            OnSyncResponse(addr, pkt);
            return;
        case Inputs:
        case SpectatorInputs:
            OnInputs(addr, pkt);
            return;
        case InputAck:
            OnInputAck(addr, pkt);
            return;
        case SessionHealth:
            OnSessionHealth(addr, pkt);
            return;
        case NetworkHealth:
            OnNetworkHealth(addr, pkt);
            return;
        default:
            assert(false && "cannot process an unknown event!");
            return;
        }
    }
}

void Gekko::MessageSystem::OnSyncRequest(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();

    // handle requests and set the peer its session magic for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->address.Equals(addr)) {
                if (player->GetStatus() == Connected) {
                    should_send++;
                    continue;
                }

                player->session_magic = body->rng_data;
                if (player->sync_num == 0) {
                    player->stats.last_sent_sync_message = now;
                    should_send++;
                }
            }
        }
    }

    if (should_send > 0) {
	    // send a packet containing the local session magic
	    SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnSyncResponse(NetAddress& addr, NetPacket& pkt)
{
    i32 should_send = 0;
    u64 now = TimeSinceEpoch();
    auto body = (SyncMsg*)pkt.body.get();

    // handle sync responses for both remotes and spectators
    std::vector<std::unique_ptr<Player>>* current = &remotes;
    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& player : *current) {
            if (player->GetStatus() == Connected) {
                // connected but the remote is still asking ? maybe high packet loss? send a response again
                should_send++;
                continue;
            }

            if (player->address.Equals(addr)) {
                player->session_magic = body->rng_data;
                if (player->sync_num < NUM_TO_SYNC) {
                    player->sync_num++;
                    should_send++;
                    player->stats.last_sent_sync_message = now;
                    session_events.AddPlayerSyncingEvent(
                        player->handle,
                        player->sync_num,
                        NUM_TO_SYNC
                    );
                    continue;
                }

                if (player->sync_num >= NUM_TO_SYNC) {
                    player->SetStatus(Connected);
                    session_events.AddPlayerConnectedEvent(player->handle);
                    continue;
                }
            }
        }
    }

    if (should_send > 0) {
    	// send a packet containing the local session magic
    	SendSyncResponse(&addr, body->rng_data);
    }
}

void Gekko::MessageSystem::OnInputs(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputMsg*)pkt.body.get();

    // RLE decompress if the sender compressed this packet
    if (body->compressed) {
        auto decompressed = Compression::RLEDecode(body->inputs.data(), (u32)body->inputs.size());
        body->inputs = std::move(decompressed);
    }

    const Frame start_frame = body->start_frame;
    const u32 input_count = body->input_count;

    const bool is_spectator = (pkt.header.type == SpectatorInputs);

    if (is_spectator) {
        for (u32 frame_idx = 0; frame_idx < input_count; frame_idx++) {
            const Frame recv_frame = start_frame + frame_idx;
            const u32 frame_offset = frame_idx * _num_players * _input_size;

            for (u32 player = 0; player < _num_players; player++) {
                u8* input = &body->inputs[frame_offset + player * _input_size];
                AddInput(recv_frame, player, input, true);
            }
        }
    } else {
        auto handles = GetRemoteHandlesForAddress(&addr);
        const u32 player_count = (u32)handles.size();

        for (u32 i = 0; i < player_count; i++) {
            const u32 player_offset = i * input_count * _input_size;

            for (u32 frame_idx = 0; frame_idx < input_count; frame_idx++) {
                const Frame recv_frame = start_frame + frame_idx;
                u8* input = &body->inputs[player_offset + frame_idx * _input_size];
                AddInput(recv_frame, handles[i], input, true);
            }

            auto player = GetPlayerByHandle(handles[i]);
            if (player) {
                player->stats.last_received_frame = TimeSinceEpoch();
            }
        }
    }
}

void Gekko::MessageSystem::OnInputAck(NetAddress& addr, NetPacket& pkt)
{
    auto body = (InputAckMsg*)pkt.body.get();
    const Frame ack_frame = body->ack_frame;
    const i8 remote_advantage = (i8)body->frame_advantage;

    for (auto& player : remotes) {
        if (player->address.Equals(addr) && player->stats.last_acked_frame < ack_frame) {
            player->stats.last_acked_frame = ack_frame;
            player->adv_history.SetRemoteAdvantage(remote_advantage);
        }
    }

    for (auto& player : spectators) {
        if (player->address.Equals(addr) && player->stats.last_acked_frame < ack_frame) {
            player->stats.last_acked_frame = ack_frame;
        }
    }
}

void Gekko::MessageSystem::OnSessionHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (SessionHealthMsg*)pkt.body.get();

    const Frame frame = body->frame;
    const u32 checksum = body->checksum;

    for (auto& player : remotes) {
        if (player->address.Equals(addr)) {
            player->SetChecksum(frame, checksum);

            for (auto iter = player->session_health.begin();
                iter != player->session_health.end(); ) {
                if (iter->first < (_net_player_queue[player->handle].last_added_input - 128)) {
                    iter = player->session_health.erase(iter);
                } else {
                    ++iter;
                }
            }
            break;
        }
    }
}

void Gekko::MessageSystem::OnNetworkHealth(NetAddress& addr, NetPacket& pkt)
{
    auto body = (NetworkHealthMsg*)pkt.body.get();

    // ok if its not a returned packet then update it and send it back to its specifc peer.
    if (!body->received) {
        // find the sender in remotes or spectators
        Player* player = nullptr;
        auto handles = GetRemoteHandlesForAddress(&addr);
        if (!handles.empty()) {
            player = GetPlayerByHandle(handles.at(0));
        }

        // check spectators if not found in remotes
        if (!player) {
            for (auto& spec : spectators) {
                if (spec->address.Equals(addr)) {
                    player = spec.get();
                    break;
                }
            }
        }

        if (!player) {
            return;
        }

        _pending_output.push(std::make_unique<NetData>());
        auto& message = _pending_output.back();

        message->pkt.header.magic = player->session_magic;
        message->pkt.header.type = NetworkHealth;

        auto new_body = std::make_unique<NetworkHealthMsg>();
        new_body->send_time = body->send_time;
        new_body->received = true;

        message->pkt.body = std::move(new_body);
        message->addr.Copy(&addr);
        return;
    }

    // else update network stats
    u16 rtt_ms = (u16)(TimeSinceEpoch() - body->send_time);
    std::vector<std::unique_ptr<Player>>* current = &remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &spectators;
        }

        for (auto& actor : *current) {
            if (addr.Equals(actor->address)) {
                actor->stats.AddRTT(rtt_ms);
            }
        }
    }
}

void Gekko::MessageSystem::SendInputsToPeer(Player* peer, GekkoNetAdapter* host, bool spectator)
{
    if (peer->address.GetSize() == 0 || peer->GetStatus() == Disconnected) {
        return;
    }

    const u32 MAX_INPUT_SIZE = 512;
    const auto packet_type = spectator ? SpectatorInputs : Inputs;
    auto& queue = spectator ? _net_spectator_queue : _net_player_queue[locals[0]->handle];
    const u32 num_players = spectator ? _num_players : (u32)locals.size();
    const u32 inputs_per_packet = MAX_INPUT_SIZE / (_input_size * num_players);
    const u32 q_size = (u32)queue.inputs.size();

    if (q_size == 0) return;

    const Frame queue_oldest_frame = queue.last_added_input - (Frame)q_size + 1;
    const Frame last_input = spectator ? _net_spectator_queue.last_added_input : GetLastAddedInput(false);

    // peer is caught up, nothing to send
    if (peer->stats.last_acked_frame >= last_input) return;

    // check per-peer cache
    if (peer->input_cache.IsValid(peer->stats.last_acked_frame, last_input)) {
        // cache hit: nothing new to send, this is a pure re-send — rate limit it
        const u64 now = TimeSinceEpoch();
        if (peer->last_input_send_time + NetStats::INPUT_RETRY_INTERVAL > now) {
            return;
        }
        for (const auto& cached_msg : peer->input_cache.packets) {
            NetData data;
            data.addr.Copy(&peer->address);
            data.pkt.header.type = packet_type;
            data.pkt.header.magic = peer->session_magic;

            auto message = std::make_unique<InputMsg>();
            message->Copy(&cached_msg);
            data.pkt.body = std::move(message);

            SendDataTo(&data, host);
        }
        peer->last_input_send_time = now;
        return;
    }

    // cache miss: rebuild packets for this peer
    const Frame peer_start_frame = std::max(peer->stats.last_acked_frame + 1, queue_oldest_frame);
    const u32 peer_start_idx = (u32)(peer_start_frame - queue_oldest_frame);
    if (peer_start_idx >= q_size) return;

    const u32 peer_input_count = q_size - peer_start_idx;

    if (peer_input_count == 0) return;

    peer->input_cache.packets.clear();

    const u32 packet_count = (peer_input_count + inputs_per_packet - 1) / inputs_per_packet;

    for (u32 pc = 0; pc < packet_count; pc++) {
        const u32 input_start_idx = peer_start_idx + pc * inputs_per_packet;
        const u32 input_end_idx = std::min(peer_start_idx + peer_input_count, input_start_idx + inputs_per_packet);
        const u32 input_count = input_end_idx - input_start_idx;

        InputMsg msg;
        msg.start_frame = queue_oldest_frame + input_start_idx;

        if (spectator) {
            for (u32 i = input_start_idx; i < input_end_idx; i++) {
                const auto& p_input = queue.inputs.at(i);
                msg.inputs.insert(msg.inputs.end(),
                    p_input.get(),
                    p_input.get() + _input_size * num_players);
            }
        }
        else {
            for (u32 player = 0; player < num_players; player++) {
                const auto& player_queue = _net_player_queue[locals[player]->handle];
                for (u32 i = input_start_idx; i < input_end_idx; i++) {
                    const auto& p_input = player_queue.inputs.at(i);
                    msg.inputs.insert(msg.inputs.end(),
                        p_input.get(),
                        p_input.get() + _input_size);
                }
            }
        }

        // RLE compress only when it actually reduces size
        msg.compressed = false;
        auto compressed = Compression::RLEEncode(msg.inputs.data(), (u32)msg.inputs.size());
        if (compressed.size() < msg.inputs.size()) {
            msg.inputs = std::move(compressed);
            msg.compressed = true;
        }

        msg.total_size = (u16)msg.inputs.size();
        msg.input_count = input_count;

        // cache and send
        InputMsg cached_msg;
        cached_msg.Copy(&msg);
        peer->input_cache.packets.push_back(std::move(cached_msg));

        NetData data;
        data.addr.Copy(&peer->address);
        data.pkt.header.type = packet_type;
        data.pkt.header.magic = peer->session_magic;
        data.pkt.body = std::make_unique<InputMsg>(std::move(msg));

        SendDataTo(&data, host);
    }

    peer->last_input_send_time = TimeSinceEpoch();

    // update cache keys
    peer->input_cache.last_acked_frame = peer->stats.last_acked_frame;
    peer->input_cache.last_input_frame = last_input;
}

void Gekko::AdvantageHistory::Init()
{
	_local_frame_adv = 0;
    _remote_frame_adv = 0;
	std::memset(_local, 0, HISTORY_SIZE * sizeof(i8));
	std::memset(_remote, 0, HISTORY_SIZE * sizeof(i8));
}

void Gekko::AdvantageHistory::Update(Frame frame)
{
	const u32 update_frame = std::max(frame, 0);
	_local[update_frame % HISTORY_SIZE] = _local_frame_adv;
	_remote[update_frame % HISTORY_SIZE] = _remote_frame_adv;
}

f32 Gekko::AdvantageHistory::GetAverageAdvantage()
{
	f32 sum_local = 0.f;
	f32 sum_remote = 0.f;

	for (i32 i = 0; i < HISTORY_SIZE; i++) {
		sum_local += _local[i];
		sum_remote += _remote[i];
	}

	f32 avg_local = sum_local / HISTORY_SIZE;
	f32 avg_remote = sum_remote / HISTORY_SIZE;

	// return the frames ahead (halved: each peer corrects its share of the gap)
	return (avg_local - avg_remote) / 2.f;
}

void Gekko::AdvantageHistory::SetLocalAdvantage(i8 adv) {
	_local_frame_adv = adv;
}

void Gekko::AdvantageHistory::SetRemoteAdvantage(i8 adv) {
    _remote_frame_adv = adv;
}
