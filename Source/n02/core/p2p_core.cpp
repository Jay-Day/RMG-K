#include "p2p_core.h"
#include "../p2p_appcode.h"

#include "p2p_message.h"
#include "../common/k_framecache.h"
#include "../errr.h"

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <chrono>
#define Sleep(ms) usleep((ms) * 1000)
#define DWORD unsigned long
static inline unsigned long GetTickCount() {
    using namespace std::chrono;
    return (unsigned long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

extern int p2p_30fps_mode;
extern char recording_player_names[4][32];

typedef struct {
	int crframeno;
	bool local;
	char msg[251];
}p2p_chatstruct;

oslist<p2p_chatstruct, P2P_CHAT_BUFFER_LEN> p2p_chat_cache;

struct P2PCORESTAT {
	p2p_message * connection;
	p2p_message * rollback_data_connection;
	bool HOST;
	int status;//0 = NC; 1 = C; 2 = GL; 3 = NS; 4 = PL
	int throughput;
	int frameno;
	int crframeno;
	int DATALEN;
	k_framecache USERDATA;
	k_framecache PEERDATA;
	int PORT;
	int ROLLBACK_DATA_PORT;
	char IP[128];
	char APP[128];
	char GAME[128];
	bool USERREADY;
	bool PEERREADY;
	char USERNAME[32];
	char PEERNAME[32];
	int ping;
	int pingc;
	bool CONNECTED;
	int last_ping_sent_time;
	int last_ping_echo_time;
	bool USERLOADED;
	bool PEERLOADED;
	int LOCALPLAYER;
	int ROOMPLAYERS;
} P2PCORE;

bool p2p_core_initialized = false;
static std::recursive_mutex p2p_transport_mutex;

struct P2PRollbackRoomPeer {
	int player;
	char ip[128];
	int port;
	int rollback_data_port;
	char name[32];
	char app[128];
	bool ready;
	bool connected;
	sockaddr_in addr;
	unsigned char tx_serial;
};

static P2PRollbackRoomPeer p2p_rollback_room_peers[3];
static int p2p_rollback_room_peer_count = 0;
static int p2p_rollback_room_measured_ping = -1;
static int p2p_rollback_room_measured_delay = 0;
static bool p2p_rollback_room_ping_reported[5] = {};
static int p2p_rollback_room_ping_reports[5] = {};
static bool p2p_rollback_room_data_reported[5] = {};
static bool p2p_rollback_room_data_ok[5] = {};
static bool p2p_rollback_room_starting = false;
static bool p2p_rollback_room_wait_for_explicit_load = false;
static bool p2p_rollback_data_punch_active = false;

inline void p2p_handle_chat_instruction(p2p_instruction * ki);
int p2p_GetTime();

static void p2p_room_log(const char *format, ...){
	char path[260];
	snprintf(path, sizeof(path), "Logs\\rollback_n02_room_%s.log", P2PCORE.HOST ? "host" : "client");
#ifdef _WIN32
	CreateDirectoryA("Logs", NULL);
#endif
	FILE *file = fopen(path, "a");
	if (file == NULL)
		return;
	fprintf(file, "tick=%i status=%i connected=%i local=%i players=%i peers=%i ready=%i peerready=%i starting=%i wait_load=%i ",
		p2p_GetTime(), P2PCORE.status, P2PCORE.CONNECTED ? 1 : 0, P2PCORE.LOCALPLAYER,
		P2PCORE.ROOMPLAYERS, p2p_rollback_room_peer_count, P2PCORE.USERREADY ? 1 : 0,
		P2PCORE.PEERREADY ? 1 : 0, p2p_rollback_room_starting ? 1 : 0,
		p2p_rollback_room_wait_for_explicit_load ? 1 : 0);
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fprintf(file, "\n");
	fclose(file);
}

static int p2p_rollback_delay_for_ping(int ping){
	if (ping < 0)
		return 2;
	if (ping <= 50)
		return 1;
	if (ping <= 100)
		return 2;
	if (ping <= 140)
		return 3;
	if (ping <= 200)
		return 4;
	return 5;
}

static int p2p_rollback_data_port_for_control_port(int port){
	if (port <= 0)
		return 0;
	if (port < 65535)
		return port + 1;
	return port - 1;
}

static void p2p_clear_rollback_room(){
	memset(p2p_rollback_room_peers, 0, sizeof(p2p_rollback_room_peers));
	p2p_rollback_room_peer_count = 0;
	p2p_rollback_room_measured_ping = -1;
	p2p_rollback_room_measured_delay = 0;
	p2p_rollback_room_starting = false;
	p2p_rollback_room_wait_for_explicit_load = false;
	memset(p2p_rollback_room_ping_reported, 0, sizeof(p2p_rollback_room_ping_reported));
	memset(p2p_rollback_room_ping_reports, 0, sizeof(p2p_rollback_room_ping_reports));
	memset(p2p_rollback_room_data_reported, 0, sizeof(p2p_rollback_room_data_reported));
	memset(p2p_rollback_room_data_ok, 0, sizeof(p2p_rollback_room_data_ok));
	p2p_rollback_data_punch_active = false;
}

static int p2p_find_rollback_peer_by_addr(const sockaddr_in& addr){
	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		if (p2p_rollback_room_peers[i].addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
			p2p_rollback_room_peers[i].addr.sin_port == addr.sin_port)
			return i;
	}
	return -1;
}

static bool p2p_addr_to_endpoint(const sockaddr_in& addr, char *ip, int ip_len, int *port){
	if (ip == 0 || ip_len <= 0 || port == 0)
		return false;
	const char *value = inet_ntoa(addr.sin_addr);
	if (value == 0 || value[0] == 0)
		return false;
	strncpy(ip, value, ip_len - 1);
	ip[ip_len - 1] = 0;
	*port = ntohs(addr.sin_port);
	return ip[0] != 0 && *port > 0;
}

static bool p2p_add_rollback_room_peer(const sockaddr_in& addr, const char *name, const char *app){
	int index = p2p_find_rollback_peer_by_addr(addr);
	if (index < 0){
		if (p2p_rollback_room_peer_count >= 3)
			return false;
		index = p2p_rollback_room_peer_count++;
		memset(&p2p_rollback_room_peers[index], 0, sizeof(p2p_rollback_room_peers[index]));
		p2p_rollback_room_peers[index].player = index + 2;
		p2p_rollback_room_peers[index].addr = addr;
		p2p_rollback_room_peers[index].tx_serial = 0;
	}

	P2PRollbackRoomPeer& peer = p2p_rollback_room_peers[index];
	peer.connected = false;
	if (name != 0){
		strncpy(peer.name, name, sizeof(peer.name) - 1);
		peer.name[sizeof(peer.name) - 1] = 0;
	}
	if (app != 0){
		strncpy(peer.app, app, sizeof(peer.app) - 1);
		peer.app[sizeof(peer.app) - 1] = 0;
	}
	if (!p2p_addr_to_endpoint(addr, peer.ip, sizeof(peer.ip), &peer.port))
		return false;
	peer.rollback_data_port = p2p_rollback_data_port_for_control_port(peer.port);
	return peer.rollback_data_port > 0;
}

static void p2p_remove_rollback_room_peer(int index){
	if (index < 0 || index >= p2p_rollback_room_peer_count)
		return;
	for (int i = index; i + 1 < p2p_rollback_room_peer_count; i++)
		p2p_rollback_room_peers[i] = p2p_rollback_room_peers[i + 1];
	memset(&p2p_rollback_room_peers[p2p_rollback_room_peer_count - 1], 0, sizeof(p2p_rollback_room_peers[p2p_rollback_room_peer_count - 1]));
	p2p_rollback_room_peer_count--;
	P2PCORE.ROOMPLAYERS = p2p_rollback_room_peer_count + 1;
	P2PCORE.CONNECTED = p2p_rollback_room_peer_count > 0;
}

static bool p2p_all_rollback_peers_ready(){
	if (!P2PCORE.USERREADY || p2p_rollback_room_peer_count <= 0)
		return false;
	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		if (!p2p_rollback_room_peers[i].connected || !p2p_rollback_room_peers[i].ready)
			return false;
	}
	return true;
}

static std::string p2p_build_rollback_room_manifest(){
	std::string manifest = std::to_string(p2p_rollback_room_peer_count + 1);
	manifest += ";";
	manifest += "1@";
	manifest += "HOST";
	manifest += ":";
	manifest += std::to_string(P2PCORE.PORT);
	manifest += ":";
	manifest += std::to_string(P2PCORE.ROLLBACK_DATA_PORT);
	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		manifest += ",";
		manifest += std::to_string(p2p_rollback_room_peers[i].player);
		manifest += "@";
		manifest += p2p_rollback_room_peers[i].ip;
		manifest += ":";
		manifest += std::to_string(p2p_rollback_room_peers[i].port);
		manifest += ":";
		manifest += std::to_string(p2p_rollback_room_peers[i].rollback_data_port);
	}
	return manifest;
}

static bool p2p_apply_rollback_room_manifest(const char *manifest){
	if (manifest == 0 || manifest[0] == 0)
		return false;
	p2p_clear_rollback_room();

	std::string value(manifest);
	const size_t semicolon = value.find(';');
	if (semicolon == std::string::npos)
		return false;
	const int player_count = atoi(value.substr(0, semicolon).c_str());
	if (player_count < 2 || player_count > 4)
		return false;
	P2PCORE.ROOMPLAYERS = player_count;
	p2p_rollback_room_wait_for_explicit_load = true;

	std::string entries = value.substr(semicolon + 1);
	size_t start = 0;
	for (;;){
		const size_t end = entries.find(',', start);
		const std::string entry = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const size_t at = entry.find('@');
		const size_t first_colon = entry.find(':', at == std::string::npos ? 0 : at + 1);
		const size_t second_colon = first_colon == std::string::npos ? std::string::npos : entry.find(':', first_colon + 1);
		if (at != std::string::npos && first_colon != std::string::npos && first_colon > at){
			const int player = atoi(entry.substr(0, at).c_str());
			const std::string ip = entry.substr(at + 1, first_colon - at - 1);
			const int control_port = atoi(entry.substr(first_colon + 1, second_colon == std::string::npos ? std::string::npos : second_colon - first_colon - 1).c_str());
			const int data_port = second_colon == std::string::npos ? p2p_rollback_data_port_for_control_port(control_port) : atoi(entry.substr(second_colon + 1).c_str());
			if (player > 0 && player != P2PCORE.LOCALPLAYER && player <= player_count && control_port > 0 && data_port > 0 &&
				p2p_rollback_room_peer_count < 3){
				P2PRollbackRoomPeer& peer = p2p_rollback_room_peers[p2p_rollback_room_peer_count++];
				memset(&peer, 0, sizeof(peer));
				peer.player = player;
				if (ip == "HOST" && P2PCORE.connection != 0){
					p2p_addr_to_endpoint(P2PCORE.connection->addr, peer.ip, sizeof(peer.ip), &peer.port);
					strncpy(peer.name, P2PCORE.PEERNAME, sizeof(peer.name) - 1);
					peer.name[sizeof(peer.name) - 1] = 0;
					peer.addr = P2PCORE.connection->addr;
				} else {
					strncpy(peer.ip, ip.c_str(), sizeof(peer.ip) - 1);
					peer.port = control_port;
					peer.addr.sin_family = AF_INET;
					peer.addr.sin_port = htons((u_short)control_port);
					peer.addr.sin_addr.s_addr = inet_addr(peer.ip);
				}
				peer.rollback_data_port = data_port;
				peer.connected = true;
			}
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return p2p_rollback_room_peer_count == player_count - 1;
}

static bool p2p_initialize_rollback_data_socket_locked(){
	if (P2PCORE.rollback_data_connection != 0){
		delete P2PCORE.rollback_data_connection;
		P2PCORE.rollback_data_connection = 0;
	}

	const int requestedPort = p2p_rollback_data_port_for_control_port(P2PCORE.PORT);
	if (requestedPort <= 0)
		return false;

	P2PCORE.rollback_data_connection = new p2p_message;
	if (!P2PCORE.rollback_data_connection->initialize(requestedPort)){
		delete P2PCORE.rollback_data_connection;
		P2PCORE.rollback_data_connection = 0;
		P2PCORE.ROLLBACK_DATA_PORT = 0;
		p2p_room_log("rollback_data_socket result=fail requested_port=%i", requestedPort);
		return false;
	}

	P2PCORE.ROLLBACK_DATA_PORT = P2PCORE.rollback_data_connection->get_port();
	p2p_room_log("rollback_data_socket result=ok port=%i", P2PCORE.ROLLBACK_DATA_PORT);
	return true;
}

static std::string p2p_peer_data_endpoint(const P2PRollbackRoomPeer& peer){
	const int dataPort = peer.rollback_data_port > 0 ? peer.rollback_data_port : p2p_rollback_data_port_for_control_port(peer.port);
	if (peer.ip[0] == 0 || dataPort <= 0)
		return std::string();
	return std::string(peer.ip) + ":" + std::to_string(dataPort);
}

static void p2p_update_rollback_data_peers_locked(){
	if (P2PCORE.rollback_data_connection == 0)
		return;
	std::vector<std::string> endpoints;
	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		const std::string endpoint = p2p_peer_data_endpoint(p2p_rollback_room_peers[i]);
		if (!endpoint.empty())
			endpoints.push_back(endpoint);
	}
	P2PCORE.rollback_data_connection->set_rollback_peers(endpoints);
}

static bool p2p_parse_rollback_data_punch(const char *data, int len, int *player, int *sequence, int *ack){
	static const char prefix[] = "RMGKDP1";
	if (data == 0 || len <= 0 || player == 0 || sequence == 0 || ack == 0)
		return false;
	char buffer[128];
	const int copyLen = std::min(len, static_cast<int>(sizeof(buffer) - 1));
	memcpy(buffer, data, copyLen);
	buffer[copyLen] = 0;
	int parsedPlayer = 0;
	int parsedSequence = 0;
	int parsedAck = 0;
	if (sscanf(buffer, "RMGKDP1:%i:%i:%i", &parsedPlayer, &parsedSequence, &parsedAck) != 3)
		return false;
	if (strncmp(buffer, prefix, sizeof(prefix) - 1) != 0)
		return false;
	*player = parsedPlayer;
	*sequence = parsedSequence;
	*ack = parsedAck;
	return true;
}

static void p2p_mark_rollback_data_peer_observed_locked(int player, const sockaddr_in& source, bool observed[5]){
	if (player <= 0 || player >= 5)
		return;
	observed[player] = true;
	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		if (p2p_rollback_room_peers[i].player != player)
			continue;
		const char *ip = inet_ntoa(source.sin_addr);
		const int port = ntohs(source.sin_port);
		if (ip != 0 && ip[0] != 0 && port > 0){
			strncpy(p2p_rollback_room_peers[i].ip, ip, sizeof(p2p_rollback_room_peers[i].ip) - 1);
			p2p_rollback_room_peers[i].ip[sizeof(p2p_rollback_room_peers[i].ip) - 1] = 0;
			p2p_rollback_room_peers[i].rollback_data_port = port;
		}
		break;
	}
}

static void p2p_poll_rollback_data_socket_locked(){
	if (P2PCORE.rollback_data_connection == 0)
		return;
	for (int i = 0; i < 256; i++){
		k_socket::check_sockets(0, 0);
		if (!P2PCORE.rollback_data_connection->has_data_waiting)
			break;
		P2PCORE.rollback_data_connection->poll_raw_socket();
	}
}

static bool p2p_run_rollback_data_punch_locked(){
	if (P2PCORE.rollback_data_connection == 0 && !p2p_initialize_rollback_data_socket_locked())
		return false;
	if (P2PCORE.rollback_data_connection == 0 || p2p_rollback_room_peer_count <= 0)
		return false;

	p2p_update_rollback_data_peers_locked();
	P2PCORE.rollback_data_connection->set_rollback_accept_any(true);
	p2p_rollback_data_punch_active = true;

	const int localPlayer = P2PCORE.LOCALPLAYER > 0 ? P2PCORE.LOCALPLAYER : (P2PCORE.HOST ? 1 : 2);
	const int sequence = p2p_GetTime();
	bool observed[5] = {};
	const int deadline = p2p_GetTime() + 1200;
	while (p2p_GetTime() < deadline){
		char payload[64];
		snprintf(payload, sizeof(payload), "RMGKDP1:%i:%i:0", localPlayer, sequence);
		for (int i = 0; i < p2p_rollback_room_peer_count; i++){
			const std::string endpoint = p2p_peer_data_endpoint(p2p_rollback_room_peers[i]);
			if (!endpoint.empty())
				P2PCORE.rollback_data_connection->send_rollback_packet_to_endpoint(endpoint.c_str(), payload, (int)strlen(payload));
		}

		p2p_poll_rollback_data_socket_locked();
		for (;;){
			char data[2048];
			int dataLen = static_cast<int>(sizeof(data));
			sockaddr_in source;
			memset(&source, 0, sizeof(source));
			if (!P2PCORE.rollback_data_connection->receive_rollback_packet(data, &dataLen, &source))
				break;
			int player = 0;
			int packetSequence = 0;
			int ack = 0;
			if (!p2p_parse_rollback_data_punch(data, dataLen, &player, &packetSequence, &ack))
				continue;
			if (ack != 0 && packetSequence != sequence)
				continue;
			p2p_mark_rollback_data_peer_observed_locked(player, source, observed);
			if (ack == 0){
				char ackPayload[64];
				snprintf(ackPayload, sizeof(ackPayload), "RMGKDP1:%i:%i:1", localPlayer, packetSequence);
				P2PCORE.rollback_data_connection->send_rollback_packet_to_addr(&source, ackPayload, (int)strlen(ackPayload));
			}
		}

		bool allObserved = true;
		for (int i = 0; i < p2p_rollback_room_peer_count; i++){
			const int player = p2p_rollback_room_peers[i].player;
			if (player <= 0 || player >= 5 || !observed[player])
				allObserved = false;
		}
		if (allObserved){
			p2p_update_rollback_data_peers_locked();
			P2PCORE.rollback_data_connection->set_rollback_accept_any(false);
			p2p_rollback_data_punch_active = false;
			p2p_room_log("rollback_data_punch result=ok local_player=%i peers=%i", localPlayer, p2p_rollback_room_peer_count);
			return true;
		}
		Sleep(10);
	}

	P2PCORE.rollback_data_connection->set_rollback_accept_any(false);
	p2p_rollback_data_punch_active = false;
	p2p_room_log("rollback_data_punch result=fail local_player=%i peers=%i", localPlayer, p2p_rollback_room_peer_count);
	return false;
}

static void p2p_send_instruction_to_rollback_peer(int index, p2p_instruction *instruction){
	if (index < 0 || index >= p2p_rollback_room_peer_count || instruction == 0 || P2PCORE.connection == 0)
		return;
	P2PCORE.connection->send_instruction_to_addr(instruction, &p2p_rollback_room_peers[index].addr,
		&p2p_rollback_room_peers[index].tx_serial);
}

static void p2p_broadcast_rollback_room_instruction(p2p_instruction *instruction){
	for (int i = 0; i < p2p_rollback_room_peer_count; i++)
		p2p_send_instruction_to_rollback_peer(i, instruction);
}

static bool p2p_send_instruction_to_addr_locked(p2p_instruction *instruction, sockaddr_in *addr, unsigned char *serial){
	if (instruction == 0 || addr == 0 || serial == 0 || P2PCORE.connection == 0)
		return false;
	return P2PCORE.connection->send_instruction_to_addr(instruction, addr, serial);
}

static bool p2p_receive_room_instruction_locked(p2p_instruction *instruction, sockaddr_in *source){
	k_socket::check_sockets(0, 0);
	if (P2PCORE.connection == 0 || !P2PCORE.connection->has_data_waiting)
		return false;
	return P2PCORE.connection->receive_raw_instruction(instruction, source);
}

static void p2p_handle_room_ping_report_locked(p2p_instruction *instruction){
	if (instruction == 0)
		return;
	const int player = instruction->load_int();
	const int ping = instruction->load_int();
	if (P2PCORE.HOST && player > 0 && player < 5){
		p2p_rollback_room_ping_reported[player] = true;
		p2p_rollback_room_ping_reports[player] = std::max(0, ping);
	}
}

static void p2p_handle_room_data_report_locked(p2p_instruction *instruction){
	if (instruction == 0)
		return;
	const int player = instruction->load_int();
	const int ok = instruction->load_int();
	if (P2PCORE.HOST && player > 0 && player < 5){
		p2p_rollback_room_data_reported[player] = true;
		p2p_rollback_room_data_ok[player] = ok != 0;
		p2p_room_log("host_recv_data_report player=%i ok=%i", player, ok);
	}
}

static void p2p_handle_room_ping_probe_locked(p2p_instruction *instruction, const sockaddr_in& source){
	if (instruction == 0)
		return;
	const int originalPlayer = instruction->load_int();
	const int sequence = instruction->load_int();
	const int sentTime = instruction->load_int();

	p2p_instruction echo(PING, PING_ROOM_PROBE_ECHO);
	echo.store_int(originalPlayer);
	echo.store_int(sequence);
	echo.store_int(sentTime);
	echo.store_int(P2PCORE.LOCALPLAYER > 0 ? P2PCORE.LOCALPLAYER : (P2PCORE.HOST ? 1 : 2));
	unsigned char serial = 0;
	sockaddr_in target = source;
	p2p_send_instruction_to_addr_locked(&echo, &target, &serial);
}

static void p2p_drain_room_ping_packets_locked(bool echoReceived[5], int echoRtt[5], int localPlayer, int sequence){
	p2p_instruction incoming;
	sockaddr_in source;
	while (p2p_receive_room_instruction_locked(&incoming, &source)){
		if (incoming.inst.type == PING && incoming.inst.flags == PING_ROOM_PROBE){
			p2p_handle_room_ping_probe_locked(&incoming, source);
		} else if (incoming.inst.type == PING && incoming.inst.flags == PING_ROOM_PROBE_ECHO){
			const int originalPlayer = incoming.load_int();
			const int echoSequence = incoming.load_int();
			const int sentTime = incoming.load_int();
			const int echoerPlayer = incoming.load_int();
			if (originalPlayer == localPlayer && echoSequence == sequence &&
				echoerPlayer > 0 && echoerPlayer < 5){
				echoReceived[echoerPlayer] = true;
				echoRtt[echoerPlayer] = std::max(echoRtt[echoerPlayer], std::max(0, p2p_GetTime() - sentTime));
			}
		} else if (incoming.inst.type == PING && incoming.inst.flags == PING_ROOM_REPORT){
			p2p_handle_room_ping_report_locked(&incoming);
		} else if (incoming.inst.type == PING && incoming.inst.flags == PING_ROOM_DATA_REPORT){
			p2p_handle_room_data_report_locked(&incoming);
		} else if (incoming.inst.type == CHAT){
			p2p_handle_chat_instruction(&incoming);
		}
	}
}

static int p2p_measure_rollback_room_rtt_locked(){
	const int localPlayer = P2PCORE.LOCALPLAYER > 0 ? P2PCORE.LOCALPLAYER : (P2PCORE.HOST ? 1 : 2);
	const int sequence = p2p_GetTime();
	bool echoReceived[5] = {};
	int echoRtt[5] = {};
	int expectedEchoes = 0;

	for (int i = 0; i < p2p_rollback_room_peer_count; i++){
		if (p2p_rollback_room_peers[i].player > 0)
			expectedEchoes++;
	}

	for (int probeIndex = 0; probeIndex < 10; probeIndex++){
		const int sentTime = p2p_GetTime();
		for (int i = 0; i < p2p_rollback_room_peer_count; i++){
			if (p2p_rollback_room_peers[i].player <= 0)
				continue;
			p2p_instruction probe(PING, PING_ROOM_PROBE);
			probe.store_int(localPlayer);
			probe.store_int(sequence);
			probe.store_int(sentTime);
			p2p_send_instruction_to_rollback_peer(i, &probe);
		}
		p2p_drain_room_ping_packets_locked(echoReceived, echoRtt, localPlayer, sequence);
		Sleep(10);
	}

	const int deadline = p2p_GetTime() + 250;
	while (p2p_GetTime() < deadline){
		p2p_drain_room_ping_packets_locked(echoReceived, echoRtt, localPlayer, sequence);

		int receivedCount = 0;
		for (int player = 1; player < 5; player++){
			if (echoReceived[player])
				receivedCount++;
		}
		if (expectedEchoes > 0 && receivedCount >= expectedEchoes)
			break;
		Sleep(2);
	}

	int maxRtt = 0;
	for (int player = 1; player < 5; player++){
		if (echoReceived[player])
			maxRtt = std::max(maxRtt, echoRtt[player]);
	}
	return maxRtt;
}

static int p2p_run_local_rollback_room_ping_locked(){
	const int localMaxPing = p2p_measure_rollback_room_rtt_locked();
	p2p_rollback_room_measured_ping = localMaxPing;
	p2p_rollback_room_measured_delay = p2p_rollback_delay_for_ping(localMaxPing);
	P2PCORE.ping = p2p_rollback_room_measured_ping;
	P2PCORE.throughput = p2p_rollback_room_measured_delay;
	p2p_ping_callback(P2PCORE.ping);
	p2p_core_debug("Rollback room local max ping = %ims, local delay = %if",
		P2PCORE.ping, P2PCORE.throughput);
	return localMaxPing;
}

static bool p2p_client_run_rollback_room_ping_locked(){
	const int localPlayer = P2PCORE.LOCALPLAYER > 0 ? P2PCORE.LOCALPLAYER : 2;
	const int localMaxPing = p2p_run_local_rollback_room_ping_locked();
	p2p_instruction report(PING, PING_ROOM_REPORT);
	report.store_int(localPlayer);
	report.store_int(localMaxPing);
	p2p_room_log("client_send_ping_report player=%i ping=%i", localPlayer, localMaxPing);
	for (int i = 0; i < 8; i++){
		P2PCORE.connection->send_instruction(&report);
		Sleep(5);
	}
	return true;
}

static bool p2p_host_wait_for_rollback_room_reports_locked(){
	memset(p2p_rollback_room_ping_reported, 0, sizeof(p2p_rollback_room_ping_reported));
	memset(p2p_rollback_room_ping_reports, 0, sizeof(p2p_rollback_room_ping_reports));
	const int localMaxPing = p2p_run_local_rollback_room_ping_locked();
	p2p_rollback_room_ping_reported[1] = true;
	p2p_rollback_room_ping_reports[1] = localMaxPing;
	p2p_room_log("host_wait_reports begin local_ping=%i", localMaxPing);

	const int deadline = p2p_GetTime() + 1500;
	while (p2p_GetTime() < deadline){
		bool dummyReceived[5] = {};
		int dummyRtt[5] = {};
		p2p_drain_room_ping_packets_locked(dummyReceived, dummyRtt, 1, -1);

		bool allReported = true;
		for (int i = 0; i < p2p_rollback_room_peer_count; i++){
			const int player = p2p_rollback_room_peers[i].player;
			if (player > 0 && player < 5 && !p2p_rollback_room_ping_reported[player])
				allReported = false;
		}
		if (allReported)
			break;
		Sleep(2);
	}

	for (int player = 1; player < 5; player++){
		if (p2p_rollback_room_ping_reported[player])
			p2p_core_debug("Rollback room P%i local max ping report = %ims",
				player, p2p_rollback_room_ping_reports[player]);
	}
	p2p_room_log("host_wait_reports end p1=%i/%i p2=%i/%i p3=%i/%i p4=%i/%i",
		p2p_rollback_room_ping_reported[1] ? 1 : 0, p2p_rollback_room_ping_reports[1],
		p2p_rollback_room_ping_reported[2] ? 1 : 0, p2p_rollback_room_ping_reports[2],
		p2p_rollback_room_ping_reported[3] ? 1 : 0, p2p_rollback_room_ping_reports[3],
		p2p_rollback_room_ping_reported[4] ? 1 : 0, p2p_rollback_room_ping_reports[4]);
	return true;
}

static void p2p_start_rollback_room_if_ready(){
	if (p2p_rollback_room_starting)
		return;
	if (!p2p_all_rollback_peers_ready())
		return;

	p2p_rollback_room_starting = true;
	P2PCORE.ROOMPLAYERS = p2p_rollback_room_peer_count + 1;
	const std::string manifest = p2p_build_rollback_room_manifest();
	p2p_room_log("start_room begin manifest=%s", manifest.c_str());
	p2p_instruction pingStart(PING, PING_ROOM_START);
	pingStart.store_string(manifest.c_str());
	p2p_broadcast_rollback_room_instruction(&pingStart);
	p2p_room_log("start_room sent_ping_start");
	if (!p2p_host_wait_for_rollback_room_reports_locked()){
		p2p_room_log("start_room failed reason=ping_reports");
		p2p_core_debug("Rollback room ping measurement failed; game will not start");
		p2p_rollback_room_starting = false;
		return;
	}

	p2p_instruction load(LOAD, LOAD_LOAD);
	load.store_string(manifest.c_str());
	p2p_broadcast_rollback_room_instruction(&load);
	p2p_room_log("start_room sent_load");

	Sleep(P2P_GAMECB_WAIT);
	memset(recording_player_names, 0, sizeof(recording_player_names));
	strncpy(recording_player_names[0], P2PCORE.USERNAME, 31);
	for (int i = 0; i < p2p_rollback_room_peer_count && i + 1 < 4; i++)
		strncpy(recording_player_names[i + 1], p2p_rollback_room_peers[i].name, 31);
	p2p_room_log("start_room local_game_callback");
	p2p_game_callback(P2PCORE.GAME, 1, P2PCORE.ROOMPLAYERS);
}

static bool p2p_process_host_room_packet(){
	if (!P2PCORE.HOST || P2PCORE.connection == 0 || !P2PCORE.connection->has_data_waiting)
		return false;

	p2p_instruction ki;
	sockaddr_in source;
	if (!P2PCORE.connection->receive_raw_instruction(&ki, &source))
		return false;

	switch (ki.inst.type){
	case LOGN:
		if (ki.inst.flags == LOGN_REQ && P2PCORE.status == 1){
			char peer_name[32] = {};
			char peer_app[128] = {};
			ki.load_sstring(peer_name);
			ki.load_string(peer_app);
			const int peer_data_port = ki.pos < ki.len ? ki.load_int() : 0;
			if (!p2p_add_rollback_room_peer(source, peer_name, peer_app)){
				p2p_instruction reject(LOGN, LOGN_RNEG);
				unsigned char serial = 0;
				P2PCORE.connection->send_instruction_to_addr(&reject, &source, &serial);
				p2p_core_debug("Connection rejected: rollback room is full");
				return true;
			}

			const int peer_index = p2p_find_rollback_peer_by_addr(source);
			if (peer_index < 0)
				return true;
			if (peer_data_port > 0)
				p2p_rollback_room_peers[peer_index].rollback_data_port = peer_data_port;
			P2PCORE.ROOMPLAYERS = p2p_rollback_room_peer_count + 1;

			p2p_instruction accept(LOGN, LOGN_RPOS);
			accept.store_sstring(P2PCORE.USERNAME);
			accept.store_string(P2PCORE.GAME);
			accept.store_int(p2p_rollback_room_peers[peer_index].player);
			accept.store_int(P2PCORE.ROOMPLAYERS);
			accept.store_int(P2PCORE.ROLLBACK_DATA_PORT);
			p2p_send_instruction_to_rollback_peer(peer_index, &accept);

			p2p_room_log("host_lobby_data_punch begin name=%s player=%i control_port=%i data_port=%i",
				peer_name, p2p_rollback_room_peers[peer_index].player,
				p2p_rollback_room_peers[peer_index].port, p2p_rollback_room_peers[peer_index].rollback_data_port);
			if (!p2p_run_rollback_data_punch_locked()){
				p2p_instruction reject(LOGN, LOGN_RNEG);
				p2p_send_instruction_to_rollback_peer(peer_index, &reject);
				p2p_room_log("host_lobby_data_punch result=fail name=%s player=%i", peer_name, p2p_rollback_room_peers[peer_index].player);
				p2p_core_debug("Rollback data punchthrough failed while accepting %s", peer_name);
				p2p_remove_rollback_room_peer(peer_index);
				return true;
			}

			p2p_rollback_room_peers[peer_index].connected = true;
			P2PCORE.CONNECTED = true;
			P2PCORE.ROOMPLAYERS = p2p_rollback_room_peer_count + 1;
			strncpy(P2PCORE.PEERNAME, peer_name, sizeof(P2PCORE.PEERNAME) - 1);
			P2PCORE.PEERNAME[sizeof(P2PCORE.PEERNAME) - 1] = 0;
			p2p_peer_info_callback(peer_name, peer_app);
			p2p_core_debug("Peer joined rollback room: %s as P%i", peer_name, p2p_rollback_room_peers[peer_index].player);
			p2p_room_log("host_peer_joined name=%s player=%i", peer_name, p2p_rollback_room_peers[peer_index].player);
			p2p_peer_joined_callback();
			if (P2PCORE.USERREADY){
				p2p_instruction ready(PREADY, PREADY_READY);
				p2p_send_instruction_to_rollback_peer(peer_index, &ready);
			}
			return true;
		}
		if (ki.inst.flags == LOGN_RPOS){
			const int peer_index = p2p_find_rollback_peer_by_addr(source);
			if (peer_index >= 0)
				p2p_rollback_room_peers[peer_index].connected = true;
			return true;
		}
		break;
	case PREADY:
		{
			const int peer_index = p2p_find_rollback_peer_by_addr(source);
			if (peer_index >= 0){
				p2p_rollback_room_peers[peer_index].ready = (ki.inst.flags == PREADY_READY);
				p2p_core_debug("%s is %s", p2p_rollback_room_peers[peer_index].name,
					p2p_rollback_room_peers[peer_index].ready ? "ready" : "not ready");
				p2p_room_log("host_peer_ready peer_index=%i ready=%i", peer_index, p2p_rollback_room_peers[peer_index].ready ? 1 : 0);
				p2p_start_rollback_room_if_ready();
			}
			return true;
		}
	case PING:
		if (ki.inst.flags == PING_ROOM_PROBE){
			p2p_handle_room_ping_probe_locked(&ki, source);
		} else if (ki.inst.flags == PING_ROOM_REPORT){
			p2p_room_log("host_recv_ping_report");
			p2p_handle_room_ping_report_locked(&ki);
		} else if (ki.inst.flags == PING_ROOM_DATA_REPORT){
			p2p_handle_room_data_report_locked(&ki);
		}
		return true;
	case CHAT:
		p2p_handle_chat_instruction(&ki);
		return true;
	case EXIT:
		{
			const int peer_index = p2p_find_rollback_peer_by_addr(source);
			if (peer_index >= 0){
				p2p_client_dropped_callback(p2p_rollback_room_peers[peer_index].name,
					p2p_rollback_room_peers[peer_index].player);
				for (int i = peer_index; i + 1 < p2p_rollback_room_peer_count; i++)
					p2p_rollback_room_peers[i] = p2p_rollback_room_peers[i + 1];
				p2p_rollback_room_peer_count--;
				P2PCORE.ROOMPLAYERS = p2p_rollback_room_peer_count + 1;
				P2PCORE.CONNECTED = p2p_rollback_room_peer_count > 0;
			}
			return true;
		}
	default:
		break;
	}
	return true;
}

static void p2p_set_rollback_room_single_peer(){
	p2p_clear_rollback_room();
	if (!p2p_core_initialized || P2PCORE.connection == 0 || !P2PCORE.CONNECTED)
		return;

	const char *ip = inet_ntoa(P2PCORE.connection->addr.sin_addr);
	const int port = ntohs(P2PCORE.connection->addr.sin_port);
	if (ip == 0 || port <= 0)
		return;

	p2p_rollback_room_peers[0].player = P2PCORE.HOST ? 2 : 1;
	strncpy(p2p_rollback_room_peers[0].name, P2PCORE.PEERNAME, sizeof(p2p_rollback_room_peers[0].name) - 1);
	p2p_rollback_room_peers[0].name[sizeof(p2p_rollback_room_peers[0].name) - 1] = 0;
	strncpy(p2p_rollback_room_peers[0].ip, ip, sizeof(p2p_rollback_room_peers[0].ip) - 1);
	p2p_rollback_room_peers[0].ip[sizeof(p2p_rollback_room_peers[0].ip) - 1] = 0;
	p2p_rollback_room_peers[0].port = port;
	p2p_rollback_room_peers[0].rollback_data_port = p2p_rollback_data_port_for_control_port(port);
	p2p_rollback_room_peers[0].ready = P2PCORE.PEERREADY;
	p2p_rollback_room_peers[0].connected = true;
	p2p_rollback_room_peers[0].addr = P2PCORE.connection->addr;
	p2p_rollback_room_peer_count = 1;
	p2p_rollback_room_wait_for_explicit_load = true;
}

//void __cdecl kprintf(char * arg_0, ...);


void p2p_send_ssrv_packet(char * cmd, int len, char * host, int port){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (p2p_core_initialized){
		P2PCORE.connection->send_ssrv(cmd, len, host, port);
	}
}
void p2p_send_ssrv_packet(char * cmd, int len, void * sadr){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (p2p_core_initialized){
		P2PCORE.connection->send_ssrv(cmd, len, (sockaddr_in*)sadr);
	}
}

static void p2p_poll_raw_socket_locked(){
	if (!p2p_core_initialized || P2PCORE.connection == 0)
		return;

	for (int i = 0; i < 256; i++){
		k_socket::check_sockets(0, 0);
		if (!P2PCORE.connection->has_data_waiting)
			break;
		P2PCORE.connection->poll_raw_socket();
	}
}

static p2p_message* p2p_rollback_data_socket_locked(){
	return P2PCORE.rollback_data_connection != 0 ? P2PCORE.rollback_data_connection : P2PCORE.connection;
}

static void p2p_clear_rollback_packets_locked(bool drain_socket){
	if (!p2p_core_initialized)
		return;

	p2p_message* dataSocket = p2p_rollback_data_socket_locked();
	if (dataSocket == 0)
		return;

	if (drain_socket){
		if (P2PCORE.rollback_data_connection != 0)
			p2p_poll_rollback_data_socket_locked();
		else
			p2p_poll_raw_socket_locked();
	}

	dataSocket->clear_rollback_packets();
}

bool p2p_rollback_transport_send(const char *data, int len){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	p2p_message* dataSocket = p2p_rollback_data_socket_locked();
	if (!p2p_core_initialized || dataSocket == 0 || !P2PCORE.CONNECTED)
		return false;
	return dataSocket->send_rollback_packet(data, len);
}

bool p2p_rollback_transport_send_to(const char *addr, const char *data, int len){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	p2p_message* dataSocket = p2p_rollback_data_socket_locked();
	if (!p2p_core_initialized || dataSocket == 0 || !P2PCORE.CONNECTED)
		return false;
	return dataSocket->send_rollback_packet_to_endpoint(addr, data, len);
}

int p2p_rollback_transport_receive(char *data, int data_len, char *addr, int addr_len){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	p2p_message* dataSocket = p2p_rollback_data_socket_locked();
	if (!p2p_core_initialized || dataSocket == 0 || data == 0 || data_len <= 0)
		return 0;
	if (addr != 0 && addr_len > 0)
		addr[0] = 0;

	if (P2PCORE.rollback_data_connection != 0)
		p2p_poll_rollback_data_socket_locked();
	else
		p2p_poll_raw_socket_locked();

	for (;;){
		int len = data_len;
		sockaddr_in packet_addr;
		memset(&packet_addr, 0, sizeof(packet_addr));
		if (!dataSocket->receive_rollback_packet(data, &len, &packet_addr))
			return 0;

		int player = 0;
		int sequence = 0;
		int ack = 0;
		if (p2p_parse_rollback_data_punch(data, len, &player, &sequence, &ack))
			continue;

		if (addr != 0 && addr_len > 0){
			const char *ip = inet_ntoa(packet_addr.sin_addr);
			const int port = ntohs(packet_addr.sin_port);
			if (ip != 0 && port > 0)
				snprintf(addr, addr_len, "%s:%i", ip, port);
		}

		return len;
	}
}

void p2p_rollback_transport_set_peers(const char **addrs, int count){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	p2p_message* dataSocket = p2p_rollback_data_socket_locked();
	if (!p2p_core_initialized || dataSocket == 0)
		return;

	std::vector<std::string> endpoints;
	if (addrs != 0 && count > 0){
		for (int i = 0; i < count; i++){
			if (addrs[i] != 0 && addrs[i][0] != 0)
				endpoints.push_back(addrs[i]);
		}
	}
	dataSocket->set_rollback_peers(endpoints);
}

void p2p_rollback_transport_clear(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	p2p_clear_rollback_packets_locked(true);
}

bool p2p_rollback_transport_connected(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return p2p_core_initialized && p2p_rollback_data_socket_locked() != 0 && P2PCORE.CONNECTED;
}
//===========================================================
//===========================================================
//===========================================================

//comment next line to enable tracing
//#define TRACE
//HFILE traceou;
//void TRACE_INIT() {
//	#ifndef TRACE
//	OFSTRUCT of;
//	traceou = OpenFile("trace.txt", &of, OF_WRITE|OF_CREATE);
//	#endif
//}
//void TRACE_TERM() {
//#ifndef TRACE
//	_lclose(traceou);
//#endif
//}
//void TRACEX(char * file, int LINE) {
//	char buf[300];
//	wsprintf(buf, "%s -- %i \r\n", file, LINE);
//	_lwrite(traceou, buf, strlen(buf)+1);
//}
//#ifndef TRACE
//#define TRACE TRACEX(__FILE__,__LINE__);
//#endif



//===========================================================
//===========================================================
//===========================================================
DWORD p2p_initial_time;
#define ADJUST_RATIO_T 1/30
void p2p_InitializeTime(){
	p2p_initial_time = GetTickCount();
}

int p2p_GetTime(){
	return GetTickCount() - p2p_initial_time;
}
void p2p_AdjustTime(int TIME){
	p2p_initial_time += ((p2p_initial_time - TIME) * ADJUST_RATIO_T);
}

int p2p_PING_TIME;

static void p2p_mark_lobby_ping_alive() {
	const int now = p2p_GetTime();
	P2PCORE.last_ping_sent_time = now;
	P2PCORE.last_ping_echo_time = now;
}

bool p2p_is_connected(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return P2PCORE.status != 0 && P2PCORE.ping != -1 && P2PCORE.connection;
}
int p2p_get_frames_count(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return P2PCORE.frameno;
}

bool p2p_core_cleanup(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();//TRACE_TERM();
	//kprintf(__FILE__ ":%i", __LINE__);
	if (p2p_core_initialized){
		p2p_clear_rollback_room();

		//kprintf(__FILE__ ":%i", __LINE__);

		if (P2PCORE.connection)
			delete P2PCORE.connection;
		P2PCORE.connection = 0;
		if (P2PCORE.rollback_data_connection)
			delete P2PCORE.rollback_data_connection;
		P2PCORE.rollback_data_connection = 0;
		
		//kprintf(__FILE__ ":%i", __LINE__);
		
		while (p2p_chat_cache.length>0){
			//delete p2p_chat_cache[0];
			p2p_chat_cache.removei(0);
		}
		

		
		p2p_core_initialized = false;
		
	} return true;

}


bool p2p_disconnect(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	if (P2PCORE.CONNECTED) {
		if (P2PCORE.status > 1) {
			p2p_core_debug("Cant quit while in game");
			return false;
		} else {
			P2PCORE.connection->send_tinst(EXIT, 0);
			P2PCORE.status = 0;
			P2PCORE.CONNECTED = false;
			p2p_clear_rollback_room();

			return true;
		}
	} else {
		P2PCORE.status = 0;
		return true;
	}
}

bool p2p_kick_peer(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	if (!P2PCORE.HOST || !P2PCORE.CONNECTED || P2PCORE.status > 1 || P2PCORE.connection == 0) {
		return false;
	}

	P2PCORE.connection->send_tinst(EXIT, 0);
	P2PCORE.CONNECTED = false;
	P2PCORE.status = 1;
	P2PCORE.ping = -1;
	P2PCORE.USERREADY = false;
	P2PCORE.PEERREADY = false;
	p2p_clear_rollback_room();
	P2PCORE.last_ping_sent_time = 0;
	P2PCORE.last_ping_echo_time = 0;
	p2p_clear_rollback_packets_locked(false);
	p2p_peer_left_callback();

	delete P2PCORE.connection;
	P2PCORE.connection = new p2p_message;
	if (!P2PCORE.connection->initialize(P2PCORE.PORT)){
		p2p_core_debug("Error initializing socket at specified port");
		P2PCORE.status = 0;
		return false;
	}

	P2PCORE.PORT = P2PCORE.connection->get_port();
	p2p_initialize_rollback_data_socket_locked();
	return true;
}

bool p2p_core_initialize(bool host, int port, char * appname, char * gamename, char * username){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();//TRACE_INIT();

	p2p_InitializeTime();

	if (p2p_core_initialized) p2p_core_cleanup();

	P2PCORE.HOST = host;
	P2PCORE.PORT = port;
	strncpy(P2PCORE.APP, (appname != NULL) ? appname : "", sizeof(P2PCORE.APP) - 1);
	P2PCORE.APP[sizeof(P2PCORE.APP) - 1] = 0;
	strncpy(P2PCORE.USERNAME, (username != NULL) ? username : "", sizeof(P2PCORE.USERNAME) - 1);
	P2PCORE.USERNAME[sizeof(P2PCORE.USERNAME) - 1] = 0;
	strncpy(P2PCORE.GAME, (gamename != NULL) ? gamename : "", sizeof(P2PCORE.GAME) - 1);
	P2PCORE.GAME[sizeof(P2PCORE.GAME) - 1] = 0;

	P2PCORE.connection = new p2p_message;
	if (!P2PCORE.connection->initialize(P2PCORE.PORT)){
		return false;
	}
	P2PCORE.PORT = P2PCORE.connection->get_port();
	p2p_initialize_rollback_data_socket_locked();

	//TRACE

	P2PCORE.status = 1;
	P2PCORE.ping = -1;
	P2PCORE.throughput = 3;
	P2PCORE.USERREADY = false;
	P2PCORE.PEERREADY = false;
	p2p_rollback_room_wait_for_explicit_load = false;
	P2PCORE.LOCALPLAYER = P2PCORE.HOST ? 1 : 0;
	P2PCORE.ROOMPLAYERS = 1;
	P2PCORE.last_ping_sent_time = 0;
	P2PCORE.last_ping_echo_time = 0;
	P2PCORE.connection->dsc = 0;
	p2p_chat_cache.clear();

	p2p_core_initialized = true;

	//TRACE
	
	return true;
}

int p2p_core_get_port(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return P2PCORE.PORT;
}

int p2p_core_get_rollback_data_port(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return P2PCORE.ROLLBACK_DATA_PORT > 0 ? P2PCORE.ROLLBACK_DATA_PORT : p2p_rollback_data_port_for_control_port(P2PCORE.PORT);
}

bool p2p_core_get_peer_endpoint(char *ip, int ip_len, int *port){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (!p2p_core_initialized || P2PCORE.connection == 0 || ip == 0 || ip_len <= 0 || port == 0)
		return false;

	const char *addr = inet_ntoa(P2PCORE.connection->addr.sin_addr);
	if (addr == 0 || addr[0] == 0)
		return false;

	strncpy(ip, addr, ip_len - 1);
	ip[ip_len - 1] = 0;
	*port = ntohs(P2PCORE.connection->addr.sin_port);
	return *port > 0;
}

int p2p_core_get_rollback_room_player_count(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return std::max(P2PCORE.ROOMPLAYERS, p2p_rollback_room_peer_count + 1);
}

int p2p_core_get_rollback_room_local_player(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (P2PCORE.LOCALPLAYER > 0)
		return P2PCORE.LOCALPLAYER;
	return P2PCORE.HOST ? 1 : 2;
}

int p2p_core_get_rollback_room_remote_count(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return p2p_rollback_room_peer_count;
}

bool p2p_core_get_rollback_room_remote(int index, int *player, char *ip, int ip_len, int *port){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (index < 0 || index >= p2p_rollback_room_peer_count || player == 0 || ip == 0 || ip_len <= 0 || port == 0)
		return false;

	*player = p2p_rollback_room_peers[index].player;
	strncpy(ip, p2p_rollback_room_peers[index].ip, ip_len - 1);
	ip[ip_len - 1] = 0;
	*port = p2p_rollback_room_peers[index].rollback_data_port > 0 ?
		p2p_rollback_room_peers[index].rollback_data_port :
		p2p_rollback_data_port_for_control_port(p2p_rollback_room_peers[index].port);
	return *player > 0 && ip[0] != 0 && *port > 0;
}

bool p2p_core_get_rollback_room_remote_info(int index, int *player, char *name, int name_len, bool *ready, char *ip, int ip_len, int *port){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (index < 0 || index >= p2p_rollback_room_peer_count || player == 0 || name == 0 || name_len <= 0 ||
		ready == 0 || ip == 0 || ip_len <= 0 || port == 0)
		return false;

	const P2PRollbackRoomPeer& peer = p2p_rollback_room_peers[index];
	*player = peer.player;
	strncpy(name, peer.name, name_len - 1);
	name[name_len - 1] = 0;
	*ready = peer.ready;
	strncpy(ip, peer.ip, ip_len - 1);
	ip[ip_len - 1] = 0;
	*port = peer.rollback_data_port > 0 ? peer.rollback_data_port : p2p_rollback_data_port_for_control_port(peer.port);
	return *player > 0 && ip[0] != 0 && *port > 0;
}

int p2p_core_get_rollback_room_ping(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return p2p_rollback_room_measured_ping >= 0 ? p2p_rollback_room_measured_ping : P2PCORE.ping;
}

bool p2p_core_is_rollback_room_start(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	return p2p_rollback_room_starting || p2p_rollback_room_wait_for_explicit_load;
}

bool p2p_core_connect(char * ip, int port){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	if (ip == NULL)
		return false;
	strncpy(P2PCORE.IP, (ip != NULL) ? ip : "", sizeof(P2PCORE.IP) - 1);
	P2PCORE.IP[sizeof(P2PCORE.IP) - 1] = 0;
	if (!P2PCORE.HOST && P2PCORE.connection->set_address(ip, port)){
		p2p_instruction contreq(LOGN, LOGN_REQ);
		contreq.store_sstring(P2PCORE.USERNAME);
		contreq.store_string(P2PCORE.APP);
		contreq.store_int(P2PCORE.ROLLBACK_DATA_PORT);
		P2PCORE.connection->send_instruction(&contreq);
		return true;
	}
	return false;
}


void p2p_print_core_status(){
	p2p_core_debug( "P2PCORE{ \r\n"
		"\tHOST: %s \r\n"
		"\tPORT: %i \r\n"
		"\tIP: %s \r\n"
		"\tAPP: %s \r\n"
		"\tGAME: %s \r\n"
		"\tUSERREADY: %s \r\n"
		"\tPEERREADY: %s \r\n"
		"\tUSERNAME: %s \r\n"
		"\tPEERNAME: %s \r\n"
		"\tping: %i \r\n"
		"\tpingc: %i \r\n"
		"\tCONNECTED: %s \r\n"
		"\tUSERLOADED: %s \r\n"
		"\tPEERLOADED: %s \r\n"
		"\tstatus: %i \r\n"
		"\tthroughput: %i \r\n"
		"\tframeno: %i \r\n"
		"\tDATALEN: %i \r\n"
		"\tINCACHE: %i \r\n"
		"\tOUTCACHE: %i \r\n"
		"\tCOMPILE: " __DATE__ " - " __TIME__ "\r\n}"
		, P2PCORE.HOST? "true":"false"
		, P2PCORE.PORT
		, P2PCORE.HOST? "irrelevent" : P2PCORE.IP
		, P2PCORE.APP
		, P2PCORE.GAME
		, P2PCORE.USERREADY? "true":"false"
		, P2PCORE.PEERREADY? "true":"false"
		, P2PCORE.USERNAME
		, P2PCORE.PEERNAME
		, P2PCORE.ping
		, P2PCORE.pingc
		, P2PCORE.CONNECTED? "true":"false"
		, P2PCORE.USERLOADED? "true":"false"
		, P2PCORE.PEERLOADED? "true":"false"
		, P2PCORE.status
		, P2PCORE.throughput
		, P2PCORE.frameno
		, P2PCORE.DATALEN
		, P2PCORE.USERDATA.pos
		, P2PCORE.PEERDATA.pos);
}

void p2p_retransmit(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	P2PCORE.connection->send_message(P2PCORE.throughput+2);
}

void p2p_drop_game(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	if (P2PCORE.connection) {
		P2PCORE.connection->send_tinst(DROP, 0);
		P2PCORE.status = 1;
		P2PCORE.USERREADY = false;
		P2PCORE.PEERREADY = false;
		p2p_rollback_room_wait_for_explicit_load = false;
		// Entering lobby from gameplay: reset ping timeout baseline to "now"
		// so old pre-game timestamps cannot trigger immediate false timeouts.
		p2p_mark_lobby_ping_alive();
		p2p_retransmit();
		p2p_clear_rollback_packets_locked(false);

		p2p_client_dropped_callback(P2PCORE.PEERNAME, P2PCORE.HOST? 2: 1);
		p2p_client_dropped_callback(P2PCORE.USERNAME, P2PCORE.HOST? 1: 2);
		
		p2p_end_game_callback();


	}
}

void p2p_set_ready(bool bx){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);

	if (P2PCORE.USERREADY != bx) {
		P2PCORE.USERREADY = bx;
		if (P2PCORE.connection && P2PCORE.CONNECTED){
			if (P2PCORE.HOST && p2p_rollback_room_peer_count > 0){
				p2p_instruction ready(PREADY, bx ? PREADY_READY : PREADY_NREADY);
				p2p_broadcast_rollback_room_instruction(&ready);
				p2p_start_rollback_room_if_ready();
			} else {
				P2PCORE.connection->send_tinst(PREADY, bx? PREADY_READY:PREADY_NREADY);
			}
		}
		p2p_core_debug("You are marked as %s", bx? "ready":"not ready");
	}
}


void p2p_ping(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (!p2p_core_initialized || P2PCORE.connection == 0 || !P2PCORE.CONNECTED)
		return;

	p2p_instruction kx;
	kx.inst.type = PING;
	kx.inst.flags = PING_PING;
	kx.store_sstring((char*)&P2PCORE);
	P2PCORE.connection->send_instruction(&kx);
	p2p_PING_TIME = p2p_GetTime();
	P2PCORE.last_ping_sent_time = p2p_PING_TIME;
}

void p2p_send_chat(char * xxx){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (xxx == NULL)
		xxx = (char*)"";

	p2p_chatstruct * ps = &p2p_chat_cache.items[p2p_chat_cache.length];
	ps->crframeno = P2PCORE.frameno + P2PCORE.throughput + 2;
	
	if (p2p_chat_cache.length == 0)
		P2PCORE.crframeno = ps->crframeno;

	strncpy(ps->msg, (xxx != NULL) ? xxx : "", sizeof(ps->msg) - 1);
	ps->msg[sizeof(ps->msg) - 1] = 0;
	ps->local = true;
	p2p_chat_cache.length++;
	
	p2p_instruction kx(CHAT, 0);
	kx.store_int(P2PCORE.crframeno);
	kx.store_vstring(xxx);
	P2PCORE.connection->send_instruction(&kx);

}








inline bool p2p_is_internal_rmgk_chat_message(const char* msg) {
	return msg != nullptr && strncmp(msg, "RMGK:", 5) == 0;
}

inline void p2p_handle_chat_instruction(p2p_instruction * ki){
	int crframeno = ki->load_int();
	char msg[251] = {};
	ki->load_vstring(msg, (unsigned int)sizeof(msg));

	if (p2p_is_internal_rmgk_chat_message(msg)) {
		p2p_chat_callback(P2PCORE.PEERNAME, msg);
		return;
	}

	p2p_chatstruct * pc = &p2p_chat_cache.items[p2p_chat_cache.length];
	pc->crframeno = crframeno;
	p2p_chat_cache.length++;
	pc->local = false;
	strncpy(pc->msg, msg, sizeof(pc->msg) - 1);
	pc->msg[sizeof(pc->msg) - 1] = 0;

	if (p2p_chat_cache.length>0)
		P2PCORE.crframeno = p2p_chat_cache.items[0].crframeno;
}

bool p2p_rollback_process_control(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	if (!p2p_core_initialized || P2PCORE.connection == 0)
		return false;

	p2p_poll_raw_socket_locked();

	bool ended_game = false;
	bool processed_instruction = false;
	for (;;) {
		p2p_instruction ki;
		sockaddr_in saddr;
		if (!P2PCORE.connection->receive_instruction(&ki, false, &saddr))
			break;

		processed_instruction = true;
		switch (ki.inst.type) {
		case CHAT:
			p2p_handle_chat_instruction(&ki);
			break;
		case DROP:
			p2p_core_debug("peer dropped");
			P2PCORE.status = 1;
			P2PCORE.USERREADY = false;
			P2PCORE.PEERREADY = false;
			p2p_mark_lobby_ping_alive();
			p2p_clear_rollback_packets_locked(false);
			p2p_client_dropped_callback(P2PCORE.PEERNAME, P2PCORE.HOST? 2: 1);
			p2p_client_dropped_callback(P2PCORE.USERNAME, P2PCORE.HOST? 1: 2);
			p2p_end_game_callback();
			ended_game = true;
			break;
		case EXIT:
			p2p_peer_left_callback();
			P2PCORE.status = P2PCORE.HOST?1:0;
			P2PCORE.CONNECTED = false;
			P2PCORE.last_ping_sent_time = 0;
			P2PCORE.last_ping_echo_time = 0;
			p2p_clear_rollback_packets_locked(false);
			p2p_end_game_callback();
			ended_game = true;
			break;
		case PING:
			if (ki.inst.flags == PING_PING) {
				ki.inst.flags = PING_ECHO;
				ki.pos = ki.len;
				P2PCORE.connection->send_instruction(&ki);
			} else {
				P2PCORE.last_ping_echo_time = p2p_GetTime();
				P2PCORE.ping = p2p_GetTime() - p2p_PING_TIME;
				p2p_ping_callback(P2PCORE.ping);
			}
			break;
		default:
			break;
		}

		if (ended_game)
			break;
	}

	if (p2p_chat_cache.length > 0){
		do {
			p2p_chatstruct * cma = &p2p_chat_cache.items[0];
			p2p_chat_callback(cma->local? P2PCORE.USERNAME:P2PCORE.PEERNAME, cma->msg);
			p2p_chat_cache.removei(0);
		} while (p2p_chat_cache.length > 0);
	}

	return ended_game || processed_instruction;
}

bool p2p_WaitForPeerToLoadOrDie(){
	n02_TRACE();
	//kprintf("WaitForPeerToLoad:" __FILE__ ":%i", __LINE__);
	
	sockaddr_in saddr;
	
	p2p_core_debug("Muffin loaded, waiting for Donut.");
	
	P2PCORE.USERLOADED = true;
	P2PCORE.PEERLOADED = false;
	
	P2PCORE.connection->send_tinst(LOAD, LOAD_LOADED);
	
	//kprintf(__FILE__ ":%i", __LINE__);
	
	while (!P2PCORE.PEERLOADED && P2PCORE.status == 1){
		if (P2PCORE.connection->has_data() || (k_socket::check_sockets(1,0) && P2PCORE.connection->has_data())){
			p2p_instruction ki;
			//kprintf(__FILE__ ":%i", __LINE__);
			if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)) {
				switch(ki.inst.type){
				case LOAD:
					p2p_core_debug("Donut loaded.");
					P2PCORE.PEERLOADED = true;
					P2PCORE.status = 2;
					
					P2PCORE.connection->send_tinst(LOAD, LOAD_LOADED);
					
					break;
				case CHAT:
					p2p_handle_chat_instruction(&ki);
					break;
				}
			}
		}
	}
	return P2PCORE.status == 2;
}


bool p2p_CalculatePingOrDie(){
	n02_TRACE();
#define p2p_PING_TIMES 4
	sockaddr_in saddr;
	//outp("calculating alcohol content in raspberry muffin...");
	if (P2PCORE.HOST) 
		Sleep(250);
	P2PCORE.pingc = 0;
	if (P2PCORE.HOST) {
		p2p_instruction kix(PING, PING_PING);
		kix.store_bytes(&P2PCORE, 64);
		
		P2PCORE.connection->send_tinst(PING, PING_PING);
		p2p_PING_TIME = p2p_GetTime();
	}
	while (P2PCORE.pingc < p2p_PING_TIMES){
		if (P2PCORE.connection->has_data() || (k_socket::check_sockets(1,0) && P2PCORE.connection->has_data())) {
			p2p_instruction ki;
			//kprintf(__FILE__ ":%i", __LINE__);
			if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)) {
				if (ki.inst.type==PING) {
					if (ki.inst.flags==PING_PING){
						//kprintf(__FILE__ ":%i", __LINE__);
						if (P2PCORE.HOST && ++P2PCORE.pingc>=p2p_PING_TIMES) {
							break;
						}
						P2PCORE.connection->send_instruction(&ki);
					} else {
						P2PCORE.ping = ki.load_int();
						P2PCORE.pingc = 99;
					}
				} else if (ki.inst.type == CHAT){
					p2p_handle_chat_instruction(&ki);
				}
			}
		}
	}
	if (P2PCORE.HOST) {
		P2PCORE.ping = (p2p_GetTime() - p2p_PING_TIME)/p2p_PING_TIMES;// * 7/5;
		p2p_instruction kx(PING, PING_ECHO);
		kx.store_int(P2PCORE.ping);
		P2PCORE.connection->send_instruction(&kx);
	}
	return true;
}


bool p2p_SynChronizeClocksOrDie(){
	n02_TRACE();
	sockaddr_in saddr;
	
	//p2p_core_debug("calculate no of atoms in a blueberry muffin...");
	
	if (P2PCORE.HOST) {
		
		//kprintf("cSyncServer");
		p2p_InitializeTime();

		Sleep(250);
		
		int selected_delay = p2p_getSelectedDelay();
		int ttime = (P2PCORE.ping / 2) + 5;
		int predicted = p2p_GetTime() + ttime;
		
		p2p_instruction kxx_pdd(TSYNC,TSYNC_FORCE);
		kxx_pdd.store_int(predicted);
		P2PCORE.connection->send_instruction(&kxx_pdd);
		
		Sleep(250);
		
		p2p_instruction kxx_psl(TSYNC, TSYNC_CHECK);
		predicted = p2p_GetTime() + ttime;
		kxx_psl.store_int(predicted);
		P2PCORE.connection->send_instruction(&kxx_psl);
		
		int readjustments = 0;
		
		while (readjustments < 4){
			if (P2PCORE.connection->has_data() || (k_socket::check_sockets(1,0) && P2PCORE.connection->has_data())) {
				p2p_instruction ki;
				if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)) {
					//kprintf(__FILE__ ":%i", __LINE__);
					if (ki.inst.type == TSYNC){
						if (ki.inst.flags == TSYNC_ADJUST){
							//p2p_core_debug("TSYNC_ADJUST: %i, %i", predicted, 0);
							int dx = ki.load_int();
							StatsAppendLine("TSYNC_ADJUST: %i, %i", predicted, dx);

							if (dx > 0 && dx < ttime * ADJUST_RATIO_T)
								ttime += dx * ADJUST_RATIO_T;
							
							if (dx == 0)
								readjustments = 4;
							
							if (++readjustments < 4){
								
								Sleep(ttime + 10);
								predicted = p2p_GetTime() + ttime;
								p2p_instruction kx_rl4;
								kx_rl4.inst.type = TSYNC;
								kx_rl4.inst.flags = TSYNC_CHECK;
								kx_rl4.store_int(predicted);
								P2PCORE.connection->send_instruction(&kx_rl4);
								//p2p_core_debug("Sending TSYNC_CHECK %i", predicted);
							}
						}
					} else if (ki.inst.type == CHAT) {
						p2p_handle_chat_instruction(&ki);
					}
				}
			}
		}
		
		int calculated_delay = 1 + (ttime * 60/1000);
		if (selected_delay > 0) {
			P2PCORE.throughput = selected_delay;
			p2p_core_debug("Calculated delay: %i frames, using host-selected delay: %i frames", calculated_delay, P2PCORE.throughput);
		} else {
			P2PCORE.throughput = calculated_delay;
		}

		// Halve delay for 30fps ROMs (new emulators call MPV at 30fps instead of 60fps)
		if (p2p_30fps_mode && P2PCORE.throughput > 1) {
			int original = P2PCORE.throughput;
			P2PCORE.throughput = (P2PCORE.throughput + 1) / 2;  // Round up
			p2p_core_debug("30fps mode: halved delay from %i to %i frames", original, P2PCORE.throughput);
		}

		p2p_instruction kxxx(TTIME, 0);
		kxxx.store_int(P2PCORE.throughput);
		P2PCORE.connection->send_instruction(&kxxx);
	} else {
		//kprintf("cSyncClient");
		int readjustments = 0;
		
		while (readjustments < 4){
			if (P2PCORE.connection->has_data() || (k_socket::check_sockets(1,0) && P2PCORE.connection->has_data())) {
				p2p_instruction ki;
				if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)) {
					//kprintf(__FILE__ ":%i", __LINE__);
					if (ki.inst.type == TSYNC){
						if (ki.inst.flags == TSYNC_FORCE){
							p2p_InitializeTime();
							unsigned int tx = ki.load_int();
							p2p_initial_time += (p2p_GetTime() - tx);
							
							StatsAppendLine("TSYNC_FORCE: %i, %i", tx, p2p_GetTime());
							
						} else if (ki.inst.flags == TSYNC_CHECK){
							int tx = ki.load_int();
							int dx = p2p_GetTime() - tx;
							p2p_initial_time += dx * ADJUST_RATIO_T;
							
							StatsAppendLine("TSYNC_CHECK: %i, %i", tx, p2p_GetTime());
							
							tx += dx * ADJUST_RATIO_T;
							
							p2p_instruction kxx(TSYNC, TSYNC_ADJUST);
							kxx.store_int(dx);
							P2PCORE.connection->send_instruction(&kxx);
						}
					} else if (ki.inst.type == TTIME) {
						int tx = ki.load_int();
						P2PCORE.throughput = tx;
						readjustments = 20;
						break;
					} else if (ki.inst.type == CHAT) {
						p2p_handle_chat_instruction(&ki);
					}
				}
			}
		}
	}
	
	//kprintf(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
	P2PCORE.status = 3;
	
	//* /
	return true;
}



void p2p_step(){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();
	// Non-blocking poll: Qt timer provides 1ms polling interval.
	k_socket::check_sockets(0,0);
	while (P2PCORE.connection && P2PCORE.connection->has_data()){
		if (P2PCORE.HOST && P2PCORE.status == 1 && p2p_process_host_room_packet()){
			k_socket::check_sockets(0,0);
			continue;
		}
		//p2p_core_debug(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
		p2p_instruction ki;
		sockaddr_in saddr;
		if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)){
			//p2p_core_debug(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
			//ki.to_string();
			switch(ki.inst.type){
			case LOGN:
				{
					if (P2PCORE.HOST && !P2PCORE.CONNECTED){
						if (P2PCORE.ping == -1 && ki.inst.flags == LOGN_REQ) {
							
							P2PCORE.connection->set_addr(&saddr);
							ki.load_sstring(P2PCORE.PEERNAME);
								char peerapp[128];
								ki.load_string(peerapp);

								char peerapp_base[128];
								strncpy(peerapp_base, peerapp, sizeof(peerapp_base) - 1);
								peerapp_base[sizeof(peerapp_base) - 1] = 0;
								p2p_appcode_split_inplace(peerapp_base, NULL, 0);

								char localapp_base[128];
								strncpy(localapp_base, P2PCORE.APP, sizeof(localapp_base) - 1);
								localapp_base[sizeof(localapp_base) - 1] = 0;
								p2p_appcode_split_inplace(localapp_base, NULL, 0);

								p2p_core_debug("Connection Request from %s (%s).. Waiting for reconfirmation...", P2PCORE.PEERNAME, peerapp_base);
								p2p_peer_info_callback(P2PCORE.PEERNAME, peerapp_base);

								if (strcmp(peerapp_base, localapp_base)!=0)
									p2p_send_chat("Emulator/version difference alert! Game may desync!");

							p2p_instruction kx;
							kx.inst.type = LOGN;
							kx.inst.flags = LOGN_RPOS;
							kx.store_sstring(P2PCORE.USERNAME);
							kx.store_string(P2PCORE.GAME);
							P2PCORE.connection->send_instruction(&kx);
							P2PCORE.ping = 0;
						}
						if (P2PCORE.ping == 0 && ki.inst.flags == LOGN_RPOS) {
							P2PCORE.CONNECTED = true;
							p2p_set_rollback_room_single_peer();
							P2PCORE.last_ping_echo_time = p2p_GetTime();
							p2p_core_debug("Peer reconfirmed connection");
							
							p2p_instruction kx;
							kx.inst.type = PING;
							kx.inst.flags = PING_PING;
							kx.store_string((char*)&P2PCORE);
							P2PCORE.connection->send_instruction(&kx);
							p2p_PING_TIME = p2p_GetTime();
							P2PCORE.last_ping_sent_time = p2p_PING_TIME;
							if (P2PCORE.USERREADY) {
								p2p_instruction kxx;
								kxx.inst.type = PREADY;
								kxx.inst.flags = PREADY_READY;
								P2PCORE.connection->send_instruction(&kxx);
							}

							p2p_peer_joined_callback();

						}
						
							if (P2PCORE.ping == 0 && ki.inst.flags == LOGN_RNEG) {
								P2PCORE.ping = -1;
								p2p_core_debug("Peer dropped connection (probbly doensot have the game)");
							}
						} else {
							if (!P2PCORE.HOST && !P2PCORE.CONNECTED){
								if (P2PCORE.ping == -1 && ki.inst.flags == LOGN_RPOS) {
									ki.load_sstring(P2PCORE.PEERNAME);
									ki.load_string(P2PCORE.GAME);
									if (ki.pos < ki.len) {
										P2PCORE.LOCALPLAYER = ki.load_int();
										p2p_rollback_room_wait_for_explicit_load = true;
									}
									if (ki.pos < ki.len) {
										P2PCORE.ROOMPLAYERS = ki.load_int();
										p2p_rollback_room_wait_for_explicit_load = true;
									}
									const int host_data_port = ki.pos < ki.len ? ki.load_int() : 0;
									if (P2PCORE.LOCALPLAYER <= 0)
										P2PCORE.LOCALPLAYER = 2;
									if (P2PCORE.ROOMPLAYERS < 2)
										P2PCORE.ROOMPLAYERS = 2;
									p2p_core_debug("Peer replied: %s (%s)", P2PCORE.PEERNAME, P2PCORE.GAME);
									
									P2PCORE.CONNECTED = true;
									p2p_set_rollback_room_single_peer();
									if (host_data_port > 0 && p2p_rollback_room_peer_count > 0)
										p2p_rollback_room_peers[0].rollback_data_port = host_data_port;
									p2p_room_log("client_lobby_data_punch begin host_control_port=%i host_data_port=%i",
										p2p_rollback_room_peer_count > 0 ? p2p_rollback_room_peers[0].port : 0,
										p2p_rollback_room_peer_count > 0 ? p2p_rollback_room_peers[0].rollback_data_port : host_data_port);
									if (!p2p_run_rollback_data_punch_locked()){
										p2p_instruction reject(LOGN, LOGN_RNEG);
										P2PCORE.connection->send_instruction(&reject);
										p2p_room_log("client_lobby_data_punch result=fail");
										p2p_core_debug("Rollback data punchthrough failed; lobby connection rejected");
										P2PCORE.CONNECTED = false;
										P2PCORE.ping = -1;
										p2p_clear_rollback_room();
										break;
									}
									p2p_peer_info_callback(P2PCORE.PEERNAME, P2PCORE.APP);
									P2PCORE.last_ping_echo_time = p2p_GetTime();

									p2p_hosted_game_callback(P2PCORE.GAME);
									/*
									char xxx[800];
									OutputHex(xxx, &ki, sizeof(ki), 0, true);
									outp(xxx);
									//*/
									p2p_instruction kx;
									kx.inst.type = LOGN;
									kx.inst.flags = LOGN_RPOS;
									P2PCORE.connection->send_instruction(&kx);
									P2PCORE.ping = 0;
									
									p2p_instruction kxx;
									kxx.inst.type = PING;
									kxx.inst.flags = PING_PING;
									kxx.store_string((char*)&P2PCORE);
									P2PCORE.connection->send_instruction(&kxx);
									p2p_PING_TIME = p2p_GetTime();
									P2PCORE.last_ping_sent_time = p2p_PING_TIME;
									p2p_peer_joined_callback();
									
								}
								if (P2PCORE.ping == -1 && ki.inst.flags == LOGN_RNEG) {
									p2p_core_debug("Peer dropped connection (Probably different emu version)");
									P2PCORE.ping = -1;
								}
							}
						}
					}
					break;
				
			case CHAT:
				{
					p2p_handle_chat_instruction(&ki);
				}
				break;
			case PING:
				{
					if (ki.inst.flags == PING_ROOM_START) {
						char manifest[256] = {};
						ki.load_string(manifest);
						p2p_room_log("client_recv_ping_start manifest=%s", manifest);
						if (!P2PCORE.HOST && manifest[0] != 0)
							p2p_apply_rollback_room_manifest(manifest);
						p2p_client_run_rollback_room_ping_locked();
					} else if (ki.inst.flags == PING_ROOM_PROBE) {
						p2p_handle_room_ping_probe_locked(&ki, saddr);
					} else if (ki.inst.flags == PING_ROOM_DATA_REPORT) {
						p2p_handle_room_data_report_locked(&ki);
					} else if (ki.inst.flags == PING_PING) {
						ki.inst.flags = PING_ECHO;
						ki.pos = ki.len;
						P2PCORE.connection->send_instruction(&ki);
					} else {
						P2PCORE.last_ping_echo_time = p2p_GetTime();
						P2PCORE.ping = p2p_GetTime() - p2p_PING_TIME;
						p2p_ping_callback(P2PCORE.ping);
					}
				}
				break;
			case PREADY:
				P2PCORE.PEERREADY = (ki.inst.flags == PREADY_READY);
				if (!P2PCORE.HOST && p2p_rollback_room_peer_count > 0)
					p2p_rollback_room_peers[0].ready = P2PCORE.PEERREADY;
				p2p_core_debug("%s is %s", P2PCORE.PEERNAME,P2PCORE.PEERREADY? "ready":"not ready");
				if (p2p_rollback_room_wait_for_explicit_load) {
					p2p_core_debug("Rollback room waiting for explicit LOAD before game start");
					p2p_room_log("client_legacy_pready_blocked");
					break;
				}
				if (P2PCORE.USERREADY && P2PCORE.PEERREADY) {
					p2p_send_chat("both players are ready, starting game");
					p2p_instruction kx;
					kx.inst.type = LOAD;
					kx.inst.flags = LOAD_LOAD;
					P2PCORE.connection->send_instruction(&kx);
					Sleep(P2P_GAMECB_WAIT);
					memset(recording_player_names, 0, sizeof(recording_player_names));
					strncpy(recording_player_names[0], P2PCORE.HOST ? P2PCORE.USERNAME : P2PCORE.PEERNAME, 31);
					strncpy(recording_player_names[1], P2PCORE.HOST ? P2PCORE.PEERNAME : P2PCORE.USERNAME, 31);
					p2p_game_callback(P2PCORE.GAME, P2PCORE.HOST? 1:2, 2);
				}
				break;
			case LOAD:
					p2p_room_log("client_recv_load pos=%i len=%i host=%i", ki.pos, ki.len, P2PCORE.HOST ? 1 : 0);
					if (!P2PCORE.HOST && ki.pos < ki.len){
						char manifest[256] = {};
						ki.load_string(manifest);
						p2p_apply_rollback_room_manifest(manifest);
					}
					Sleep(P2P_GAMECB_WAIT);
					memset(recording_player_names, 0, sizeof(recording_player_names));
					strncpy(recording_player_names[0], P2PCORE.HOST ? P2PCORE.USERNAME : P2PCORE.PEERNAME, 31);
					strncpy(recording_player_names[1], P2PCORE.HOST ? P2PCORE.PEERNAME : P2PCORE.USERNAME, 31);
					p2p_room_log("client_game_callback");
					p2p_game_callback(P2PCORE.GAME, p2p_core_get_rollback_room_local_player(), p2p_core_get_rollback_room_player_count());
					break;
			case EXIT:
					p2p_peer_left_callback();
					P2PCORE.status = P2PCORE.HOST?1:0;
					P2PCORE.CONNECTED = false;
					P2PCORE.last_ping_sent_time = 0;
					P2PCORE.last_ping_echo_time = 0;
					
					if (P2PCORE.status) {
						p2p_core_debug("reinitializing server...");
						delete P2PCORE.connection;
						P2PCORE.connection = 0;
						
						if (P2PCORE.connection != 0)
							delete P2PCORE.connection;
						P2PCORE.connection = new p2p_message;
						
						if (!P2PCORE.connection->initialize(P2PCORE.PORT)){
							p2p_core_debug("Error initializing socket at specified port");
						} else {
							P2PCORE.PORT = P2PCORE.connection->get_port();
							p2p_initialize_rollback_data_socket_locked();
							p2p_core_debug("Done");
						}
						
						P2PCORE.status = 1;
						P2PCORE.ping = -1;
						P2PCORE.USERREADY = false;
						P2PCORE.PEERREADY = false;
						P2PCORE.last_ping_sent_time = 0;
						P2PCORE.last_ping_echo_time = 0;
						
					}
					break;
				}
			}
		}
		// If the peer disappears without sending EXIT (UDP), either side can get
		// stuck thinking it's still connected. Use periodic lobby pings (sent by
		// the UI) to detect this and recover.
		{
			const int P2P_LOBBY_PING_TIMEOUT_MS = 10000;
			const int P2P_LOBBY_PING_ACTIVITY_MS = 5000;
			const int now = p2p_GetTime();

			if (P2PCORE.CONNECTED &&
				P2PCORE.status == 1 &&
				P2PCORE.last_ping_sent_time != 0 &&
				P2PCORE.last_ping_echo_time != 0 &&
				(now - P2PCORE.last_ping_sent_time) < P2P_LOBBY_PING_ACTIVITY_MS &&
				(now - P2PCORE.last_ping_echo_time) > P2P_LOBBY_PING_TIMEOUT_MS) {

				p2p_core_debug("Ping timeout. Peer dropped.");
				p2p_peer_left_callback();

				P2PCORE.CONNECTED = false;
				P2PCORE.status = 1;
				P2PCORE.ping = -1;
				P2PCORE.USERREADY = false;
				P2PCORE.PEERREADY = false;
				P2PCORE.last_ping_sent_time = 0;
				P2PCORE.last_ping_echo_time = 0;

				if (P2PCORE.HOST) {
					P2PCORE.status = 1;
					p2p_core_debug("reinitializing server...");
					delete P2PCORE.connection;
					P2PCORE.connection = new p2p_message;
					if (!P2PCORE.connection->initialize(P2PCORE.PORT)){
						p2p_core_debug("Error initializing socket at specified port");
					} else {
						P2PCORE.PORT = P2PCORE.connection->get_port();
						p2p_initialize_rollback_data_socket_locked();
						p2p_core_debug("Done");
					}
				} else {
					// Match existing EXIT behavior for non-host peers.
					P2PCORE.status = 0;
				}
			}
		}
		if (P2PCORE.connection && P2PCORE.connection->has_ssrv()){
			char xxx[2000];
			sockaddr_in saddr;
			p2p_ssrv_packet_recv_callback(xxx, P2PCORE.connection->receive_ssrv(xxx, &saddr), &saddr);
		}
		if (p2p_chat_cache.length > 0){
			do {
				p2p_chatstruct * cma = &p2p_chat_cache.items[0];
				p2p_chat_callback(cma->local? P2PCORE.USERNAME:P2PCORE.PEERNAME, cma->msg);
				p2p_chat_cache.removei(0);
			} while (p2p_chat_cache.length > 0);
		}
	}

inline bool ProcessGameStuff(){
	n02_TRACE();
	while (P2PCORE.connection->has_data()){
		p2p_instruction ki;
		if (P2PCORE.connection->receive_instructionx(&ki)){
			if (ki.inst.type == DATA) {
				char data[32];
				ki.load_bytes(data, P2PCORE.DATALEN);
				P2PCORE.PEERDATA.put_data(data, P2PCORE.DATALEN);
			} else if (ki.inst.type == CHAT) {
				p2p_handle_chat_instruction(&ki);
			} else if (ki.inst.type == DROP) {
				p2p_core_debug("peer dropped");
				P2PCORE.status = 1;
				P2PCORE.USERREADY = false;
				P2PCORE.PEERREADY = false;
				// Entering lobby from gameplay: reset ping timeout baseline to "now"
				// so old pre-game timestamps cannot trigger immediate false timeouts.
				p2p_mark_lobby_ping_alive();
				p2p_client_dropped_callback(P2PCORE.PEERNAME, P2PCORE.HOST? 2: 1);
				p2p_client_dropped_callback(P2PCORE.USERNAME, P2PCORE.HOST? 1: 2);
				p2p_end_game_callback();
				return true;
			}
		}
		//data chat and drop
	}
	return false;
}

int p2p_modify_play_values(void *values, int size){
	std::lock_guard<std::recursive_mutex> lock(p2p_transport_mutex);
	n02_TRACE();//TRACE
	if (P2PCORE.status > 1) {
		//TRACE
		P2PCORE.USERDATA.put_data(values, P2PCORE.DATALEN);
		//TRACE
		p2p_instruction kix(DATA,0);
		kix.store_bytes(values, P2PCORE.DATALEN);
		P2PCORE.connection->send_instruction(&kix);
		//TRACE
		if (P2PCORE.frameno++ >= P2PCORE.throughput) {
			//TRACE
			int TI = p2p_GetTime();
			int TO = 0;
			//TRACE
			while (P2PCORE.PEERDATA.pos < P2PCORE.DATALEN && P2PCORE.status > 1){
				if (P2PCORE.connection->has_data() || (k_socket::check_sockets(0,10) && P2PCORE.connection->has_data())){
					if (ProcessGameStuff())
						return -1;
				}
				else { /*else if (p2p_GetTime()-TI > P2PCORE.throughput * 17) p2p_retransmit();*/
					if (p2p_GetTime() - TI > 20) {
						TI = p2p_GetTime();
						p2p_retransmit();
						TO++;
						if (TO == 320) {
							p2p_core_debug("Data timeout. Dropping game");
							p2p_drop_game();
							return -1;
						}
					}
				}
			}
			//TRACE
			if (P2PCORE.frameno > P2PCORE.crframeno && p2p_chat_cache.length > 0){
				p2p_chatstruct * cma = &p2p_chat_cache.items[0];
				p2p_chat_callback(cma->local? P2PCORE.USERNAME:P2PCORE.PEERNAME, cma->msg);
				p2p_chat_cache.removei(0);
				
				if (p2p_chat_cache.length > 0) {
					P2PCORE.crframeno = p2p_chat_cache.items[0].crframeno;
				} else 
					P2PCORE.crframeno = 5 * P2PCORE.frameno;
			}
			//TRACE
			if (P2PCORE.PEERDATA.pos >= P2PCORE.DATALEN) {
				if (P2PCORE.HOST) {
					P2PCORE.USERDATA.get_data((char*)values, P2PCORE.DATALEN);
					P2PCORE.PEERDATA.get_data((((char*)values)+P2PCORE.DATALEN), P2PCORE.DATALEN);
				} else {
					P2PCORE.PEERDATA.get_data((char*)values, P2PCORE.DATALEN);
					P2PCORE.USERDATA.get_data((((char*)values)+P2PCORE.DATALEN), P2PCORE.DATALEN);
				}
				return P2PCORE.DATALEN * 2;
			}
			//TRACE
			return -1;
			//other user's input
		} else {
			//TRACE
			return 0;
		}
	} else {
		n02_TRACE();//TRACE
		//kprintf(__FILE__ ":%i", __LINE__);
		//TRACE
		sockaddr_in saddr;
		//initialize cache
		P2PCORE.USERDATA.reset();
		P2PCORE.PEERDATA.reset();
		//TRACE
		p2p_chat_cache.clear();

		P2PCORE.DATALEN = size;
		
		P2PCORE.status = 1;
		//TRACE
		if (p2p_WaitForPeerToLoadOrDie()){
			//TRACE
			p2p_core_debug("== Everyone Loaded");
		}
		//TRACE
		if (P2PCORE.status == 2 && p2p_CalculatePingOrDie()){
			p2p_core_debug("== Calculated Ping = %ims", P2PCORE.ping);
		}
		//TRACE
		if (P2PCORE.status == 2 && p2p_SynChronizeClocksOrDie()) {
			p2p_core_debug("== Calculated delay %i frame(s)", P2PCORE.throughput);
		}
		//TRACE
		
		//kprintf(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
		p2p_core_debug("gamesync in less than %i second(s)", (P2P_GAMESYNC_WAIT/1000)+1);
		
		P2PCORE.frameno = 0;
		P2PCORE.crframeno = 0;
		int start_time = 0;
		//kprintf(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
		
		if (P2PCORE.HOST){
			start_time = p2p_GetTime()+P2P_GAMESYNC_WAIT;
			p2p_instruction ki(START, 0);
			ki.store_int(start_time);
			P2PCORE.connection->send_instruction(&ki);
		} else {
			while (start_time == 0){
				if (P2PCORE.connection->has_data() || (k_socket::check_sockets(1,0) && P2PCORE.connection->has_data())){
					p2p_instruction ki;
					if (P2PCORE.connection->receive_instruction(&ki, false, &saddr)) {
						if (ki.inst.type == START){
							start_time = ki.load_int();
							//kprintf("Start time %i", start_time);
						}
					}
				}
			}
		}
		//TRACE
		//kprintf(__FILE__ ":%i, %i", __LINE__, p2p_GetTime());
		
		P2PCORE.connection->default_ipm = std::max(2, std::min(P2PCORE.throughput + 1, 8));
		StatsAppendLine("default ipm changed to %i", P2PCORE.connection->default_ipm);

		int tl = start_time - p2p_GetTime();
		//kprintf("time left %i ms", tl);
		//TRACE
		k_socket::check_sockets(tl / 1000, tl % 1000);
		//TRACE
		return p2p_modify_play_values(values, size);
		
	}
}
