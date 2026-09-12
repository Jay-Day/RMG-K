
#include "kaillera_core.h"
#include "k_message.h"
#include "../common/k_framecache.h"
#include "../errr.h"
#include "../kailleraclient.h"

#include <algorithm>
#include <chrono>
#include <string>

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

#define KAILLERA_CONNECTION_RESP_MAX_DELAY 5000
#define KAILLERA_LOGIN_RESP_MAX_DELAY 10000
#define KAILLERA_GAME_LOSTCON_TIMEOUT 10000
#define KAILLERA_TIMEOUT_RESET 60000
#define KAILLERA_TIMEOUT_NETSYNC 120000
#define KAILLERA_TIMEOUT_NETSYNC_RETR_INTERVAL 5000

extern int PACKETLOSSCOUNT;
static bool kaillera_spoofing = false;  // Set during connect based on kaillera_spoof_ping
static char kaillera_last_error[256] = "";

const char* kaillera_core_get_last_error() {
	return kaillera_last_error;
}

typedef struct {
	k_message * connection;
	int frameno;
	int dframeno;
	int throughput;
	int pps;
	int REQDATALEN;
	int DATALEN;
	k_framecache USERDATA;
	oslist<char*, 256> kaillera_incoming_data_cache;
	oslist<char*, 256> kaillera_gv_queue;
	int playerno;
	int PORT;
	char IP[128];
	char APP[128];
	char GAME[128];
	char USERNAME[32];
	char conset;
	int USERSTAT; //0 = dc; 1 = connected; 2 = logged in; 3 = in game
	int PLAYERSTAT; //0 = idle; 1 = netsync; 2 = playing
	bool owner;
	bool game_id_requested;
	unsigned int game_id;
	bool user_id_requested;
	bool leave_game_requested;
	unsigned short user_id;
		int tmoutrsttime;
		bool pending_spoof_announce;
		int spoof_announce_time;
	} KAILLERAC_;

KAILLERAC_ KAILLERAC;

// Helper function to send a pong response
static void SendPong() {
	if (!KAILLERAC.connection) return;
	k_instruction pong;
	pong.type = USERPONG;
	int x = 0;
	while(x < 4)
		pong.store_int(x++);
	KAILLERAC.connection->send_instruction(&pong);
}

static std::string normalizeKailleraWireText(const char* text)
{
	return n02::encoding::encodeKailleraText(text ? text : "");
}

static std::string normalizeKailleraUsernameForCompare(const char* text)
{
	const std::string encoded = normalizeKailleraWireText(text);
	const std::size_t length = n02::encoding::safeCp932PrefixLength(encoded, 31);
	return encoded.substr(0, length);
}

static bool kailleraWireEquals(const char* received, const char* local)
{
	return std::string(received ? received : "") == normalizeKailleraWireText(local);
}

static bool kailleraUsernameEquals(const char* received, const char* local)
{
	return std::string(received ? received : "") == normalizeKailleraUsernameForCompare(local);
}

void kaillera_print_core_status(){
	kaillera_core_debug("USS/PS: %i/%i", KAILLERAC.USERSTAT, KAILLERAC.PLAYERSTAT);
	kaillera_core_debug("ICACHE: %i - %i", KAILLERAC.kaillera_incoming_data_cache.length, KAILLERAC.kaillera_gv_queue.length);
}

//void __cdecl kprintf(char * arg_0, ...);

bool kaillera_core_initialized = false;
static volatile bool kaillera_step_active = false;

void p2p_InitializeTime();
int p2p_GetTime();


bool kaillera_is_connected(){
	return KAILLERAC.USERSTAT > 0;
}
int kaillera_get_frames_count(){
	return KAILLERAC.frameno;
}
int kaillera_get_delay(){
	return KAILLERAC.dframeno;
}
unsigned short kaillera_get_user_id(){
	return KAILLERAC.user_id;
}

bool kaillera_core_cleanup(){
	//kprintf(__FILE__ ":%i", __LINE__);
	n02_TRACE();
	if (kaillera_core_initialized){
		// Null the pointer before deleting so the other thread
		// sees NULL if it races past the KAILLERA_CORE_INITIALIZED check.
		k_message* conn = KAILLERAC.connection;
		KAILLERAC.connection = 0;
		// Wait for any in-flight kaillera_step() to finish before
		// freeing the connection (it may be inside check_sockets which
		// blocks up to 200ms).
		while (kaillera_step_active)
			Sleep(10);
		if (conn)
			delete conn;

		while (KAILLERAC.kaillera_incoming_data_cache.length>0) {
			free(KAILLERAC.kaillera_incoming_data_cache[0]);
			KAILLERAC.kaillera_incoming_data_cache.removei(0);
		}

		while (KAILLERAC.kaillera_gv_queue.length>0) {
			free(KAILLERAC.kaillera_gv_queue[0]);
			KAILLERAC.kaillera_gv_queue.removei(0);
		}

		kaillera_core_initialized = false;
	}

	// Always reset state on cleanup
	KAILLERAC.USERSTAT = 0;
	KAILLERAC.PLAYERSTAT = -1;
	KAILLERAC.game_id_requested = false;
	KAILLERAC.user_id_requested = false;
	KAILLERAC.leave_game_requested = false;
	KAILLERAC.pending_spoof_announce = false;

	return true;
}

bool kaillera_disconnect(char * quitmsg){
	n02_TRACE();
	// Send leave message if we have sent USERLOGN (USERSTAT >= 1)
	// USERSTAT 1 = login sent, 2 = logged in (LONGSUCC received)
	if (KAILLERAC.USERSTAT >= 1 && KAILLERAC.connection) {
		k_instruction ls;
		ls.type = USERLEAV;
		ls.store_short(-1);
		ls.store_string(quitmsg ? quitmsg : "");
		KAILLERAC.connection->send_instruction(&ls);
	}
	// Always reset state
	KAILLERAC.USERSTAT = 0;
	KAILLERAC.PLAYERSTAT = -1;
	return true;
}


void kaillera_set_spoof_ping(int spoof_ping_ms) {
	if (spoof_ping_ms > 0 && spoof_ping_ms < 1000) {
		kaillera_spoof_ping = spoof_ping_ms;
		kaillera_spoofing = true;
		kaillera_core_debug("Ping spoofing set to %dms", spoof_ping_ms);
	} else {
		kaillera_spoof_ping = 0;
		kaillera_spoofing = false;
	}
}

bool kaillera_core_initialize(int port, char * appname, char * username, char connection_setting){
	p2p_InitializeTime();

	if (kaillera_core_initialized) kaillera_core_cleanup();

	// Reset all state
	KAILLERAC.USERSTAT = 0;
	KAILLERAC.PLAYERSTAT = -1;
	KAILLERAC.game_id_requested = false;
	KAILLERAC.user_id_requested = false;
	KAILLERAC.leave_game_requested = false;
	KAILLERAC.pending_spoof_announce = false;
	KAILLERAC.owner = false;

	KAILLERAC.PORT = port;
	KAILLERAC.conset = connection_setting;

	strncpy(KAILLERAC.APP, (appname != NULL) ? appname : "", sizeof(KAILLERAC.APP) - 1);
	KAILLERAC.APP[sizeof(KAILLERAC.APP) - 1] = 0;
	strncpy(KAILLERAC.USERNAME, (username != NULL) ? username : "", sizeof(KAILLERAC.USERNAME) - 1);
	KAILLERAC.USERNAME[sizeof(KAILLERAC.USERNAME) - 1] = 0;

	KAILLERAC.connection = new k_message;
	if (!KAILLERAC.connection->initialize(KAILLERAC.PORT)){
		delete KAILLERAC.connection;
		KAILLERAC.connection = 0;
		return false;
	}
	KAILLERAC.PORT = KAILLERAC.connection->get_port();

	KAILLERAC.kaillera_incoming_data_cache.clear();
	KAILLERAC.kaillera_gv_queue.clear();

	kaillera_core_initialized = true;

	return true;
}

int kaillera_core_get_port(){
	return KAILLERAC.PORT;
}

bool kaillera_core_connect(char * ip, int port){

	kaillera_last_error[0] = 0;

	if (ip == NULL)
		return false;

	kaillera_core_debug("Connecting to %s:%i", ip, port);

	strncpy(KAILLERAC.IP, (ip != NULL) ? ip : "", sizeof(KAILLERAC.IP) - 1);
	KAILLERAC.IP[sizeof(KAILLERAC.IP) - 1] = 0;

	k_socket ksock;
	ksock.initialize(0, 2048);

	if (ksock.set_address(ip, port)){
		ksock.send("HELLO0.83", 10);

		DWORD tout = p2p_GetTime();

		while ((!k_socket::check_sockets(0, 100) || !ksock.has_data()) && p2p_GetTime() - tout < KAILLERA_CONNECTION_RESP_MAX_DELAY);

		if (!ksock.has_data()) {
			strncpy(kaillera_last_error, "Connection timed out - you may be temporarily blocked by this server", sizeof(kaillera_last_error) - 1);
			return false;
		}

		{

				char srsp[257];
				srsp[0] = 0;
				int srspl = 256;
				sockaddr_in addr;

			kaillera_core_debug("server replied");

				if (ksock.check_recv(srsp, &srspl, false, &addr)) {
					if (srspl < 0)
						srspl = 0;
					if (srspl > 256)
						srspl = 256;
					srsp[srspl] = 0;

					if (srspl >= 9 && strncmp("HELLOD00D", srsp, 9) == 0) {

						kaillera_core_debug("logging in");

						int server_port = atoi(srsp + 9);
						if (server_port <= 0 || server_port > 65535) {
							strncpy(kaillera_last_error, "Error connecting to server", sizeof(kaillera_last_error) - 1);
							return false;
						}
						addr.sin_port = htons((u_short)server_port);
						KAILLERAC.connection->set_addr(&addr);
						KAILLERAC.USERSTAT = 1;
						KAILLERAC.PLAYERSTAT = -1;

					k_instruction ki;
					ki.type = USERLOGN;
					ki.store_string(KAILLERAC.APP);
					ki.store_char(KAILLERAC.conset);
					ki.set_username(KAILLERAC.USERNAME);

					KAILLERAC.connection->send_instruction(&ki);

					// Ping spoofing: send 4 delayed pongs to make server think we have higher latency
					// This allows us to control what frame delay the server calculates for us
					if (kaillera_spoofing && kaillera_spoof_ping > 0) {
						kaillera_core_debug("Ping spoofing enabled: %dms delay", kaillera_spoof_ping);
						Sleep(kaillera_spoof_ping);
						SendPong();
						Sleep(kaillera_spoof_ping);
						SendPong();
						Sleep(kaillera_spoof_ping);
						SendPong();
						Sleep(kaillera_spoof_ping);
						SendPong();
					}

					return true;
					}
				} else {
					if (strcmp("TOO", srsp) == 0) {
						strncpy(kaillera_last_error, "Server is full", sizeof(kaillera_last_error) - 1);
					} else {
						strncpy(kaillera_last_error, "Error connecting to server", sizeof(kaillera_last_error) - 1);
					//protocol different or unrecognized protocol
				}
			}
		}

		return false;
	}
	return false;
}

bool kaillera_core_finish_login(int timeout_ms) {
	if (timeout_ms <= 0)
		return KAILLERAC.USERSTAT > 1;

	const int start = p2p_GetTime();
	while (KAILLERAC.connection &&
		KAILLERAC.USERSTAT == 1 &&
		(p2p_GetTime() - start) < timeout_ms) {
		kaillera_step();
	}

	return KAILLERAC.USERSTAT > 1;
}

void kaillera_ProcessGeneralInstruction(k_instruction * ki) {
	n02_TRACE();
	switch (ki->type) {
	case USERJOIN:
		{
			unsigned short id = ki->load_short();
			int ping = ki->load_int();
			int conn = ki->load_char();
			kaillera_user_join_callback(ki->user, ping, id, conn);
			break;
		}
	case USERLEAV:
		{
			unsigned short id = ki->load_short();
			char quitmsg[128];
			ki->load_str(quitmsg, 128);
			kaillera_user_leave_callback(ki->user, quitmsg, id);
			break;
		}
	case GAMEMAKE:
		{
			//kaillera_core_debug("GAMEMAKE");

			char gname[128];
			ki->load_str(gname, 128);
			char emulator[128];
			ki->load_str(emulator, 128);
			unsigned int id = ki->load_int();
			
			kaillera_game_create_callback(gname, id, emulator, ki->user);
			
			if (KAILLERAC.game_id_requested) {
				if (kailleraWireEquals(gname, KAILLERAC.GAME) &&
					kailleraWireEquals(emulator, KAILLERAC.APP) &&
					kailleraUsernameEquals(ki->user, KAILLERAC.USERNAME)) {
					KAILLERAC.game_id = id;
					KAILLERAC.game_id_requested = false;
					KAILLERAC.user_id_requested = true;
					
					KAILLERAC.USERSTAT = 3;
					KAILLERAC.PLAYERSTAT = 0;
					
					kaillera_user_game_create_callback();
				}
			}
			break;
		}
	case INSTRUCTION_GAMESTAT:
		{
			unsigned int id = ki->load_int();
			char status = ki->load_char();
			int players = ki->load_char();
			int maxplayers = ki->load_char();
			kaillera_game_status_change_callback(id, status, players, maxplayers);
			break;
		}
	case INSTRUCTION_GAMESHUT:
		{
			unsigned int id = ki->load_int();
			kaillera_game_close_callback(id);
			if (id==KAILLERAC.game_id){
				kaillera_user_game_closed_callback();
				kaillera_end_game_callback();
				KAILLERAC.USERSTAT = 2;
				KAILLERAC.PLAYERSTAT = -1;
			}
			break;
		}
	case GAMEBEGN:
		{
			if (KAILLERAC.leave_game_requested) {
				kaillera_core_debug("Ignoring GAMEBEGN (leave requested)");
				break;
			}
			//KAILLERAC.USERSTAT = 3;
			KAILLERAC.PLAYERSTAT = 1;
			KAILLERAC.throughput = ki->load_short();
			KAILLERAC.dframeno = (KAILLERAC.throughput + 1) * KAILLERAC.conset - 1;

			// Halve delay for 30fps ROMs (new emulators call MPV at 30fps instead of 60fps)
			if (kaillera_30fps_mode && KAILLERAC.dframeno > 1) {
				int original = KAILLERAC.dframeno;
				KAILLERAC.dframeno = (KAILLERAC.dframeno + 1) / 2;  // Round up
				kaillera_core_debug("30fps mode: halved delay from %i to %i frames", original, KAILLERAC.dframeno);
			}

			kaillera_core_debug("Server says: delay is %i frames (throughput=%i, conset=%i)",
				KAILLERAC.dframeno, KAILLERAC.throughput, KAILLERAC.conset);

			KAILLERAC.playerno = ki->load_char();
			int players = ki->load_char();
			kaillera_game_callback(KAILLERAC.GAME, KAILLERAC.playerno, players);
			break;
		}
	case GAMRSLST:
		{
			//kaillera_core_debug("GAMRSLST");

			if (!KAILLERAC.owner)
				kaillera_user_game_joined_callback();

			KAILLERAC.USERSTAT = 3;
			KAILLERAC.PLAYERSTAT = 0;
			int numplayers = ki->load_int();
			for (int x=0; x < numplayers; x++) {
				char name[32];
				ki->load_str(name, 32);
				int ping = ki->load_int();
				unsigned short id = ki->load_short();
				int conn = ki->load_char();
				kaillera_player_add_callback(name, ping, id, conn);
			}

			// Schedule ping spoof announcement when joining game (delayed to avoid flood control)
			if (kaillera_spoofing && kaillera_spoof_ping > 0) {
				KAILLERAC.pending_spoof_announce = true;
				KAILLERAC.spoof_announce_time = p2p_GetTime() + 2000;
			}
			break;
		}
	case GAMRJOIN:
		{
			//kaillera_core_debug("GAMRJOIN");
			ki->load_int();
			char username[32];
			ki->load_str(username, 32);					
			int ping = ki->load_int();					
			unsigned short uid = ki->load_short();
			char connset = ki->load_char();

			kaillera_player_joined_callback(username, ping, uid, connset);

			if (KAILLERAC.user_id_requested) {
				if (KAILLERAC.conset == connset && kailleraUsernameEquals(username, KAILLERAC.USERNAME)) {
					KAILLERAC.user_id = uid;
					KAILLERAC.user_id_requested = false;
					KAILLERAC.USERSTAT = 3;
					KAILLERAC.PLAYERSTAT = 0;
				}
			}
			break;
		}
	case GAMRLEAV:
		{
			unsigned short id;
			kaillera_player_left_callback(ki->user, id = ki->load_short());
			if (id==KAILLERAC.user_id && !KAILLERAC.leave_game_requested) {
				kaillera_user_kicked_callback();
				KAILLERAC.USERSTAT = 2;
				KAILLERAC.PLAYERSTAT = -1;
			}
			break;
		}
	case GAMRDROP:
		{
			int gdpl = ki->load_char();
			if (KAILLERAC.playerno == gdpl) {
				KAILLERAC.USERSTAT = 3;
				KAILLERAC.PLAYERSTAT = 0;
				kaillera_end_game_callback();
				// Don't call dropped callback here for local player -
				// already called in kaillera_game_drop() before sending
			} else {
				// Only notify for remote players dropping
				kaillera_player_dropped_callback(ki->user, gdpl);
			}
			break;
		}
	case PARTCHAT:
		kaillera_chat_callback(ki->user, ki->buffer);
		break;
	case GAMECHAT:
		kaillera_game_chat_callback(ki->user, ki->buffer);
		break;
	case MOTDLINE:
		kaillera_motd_callback(ki->user, ki->buffer);
		break;				
	case LONGSUCC:
		{
			KAILLERAC.USERSTAT = KAILLERAC.USERSTAT == 1? 2:KAILLERAC.USERSTAT;
			int gameZ;
			int userZ;
			userZ = ki->load_int();
			gameZ = ki->load_int();
			int x;
			//users
			for(x=0;x<userZ; x++) {
				char name[32];
				ki->load_str(name, 32);
				int ping = ki->load_int();
				int status = ki->load_char();
				unsigned short id = ki->load_short();
				int conn = ki->load_char();
				kaillera_user_add_callback(name, ping, status, id, conn);
			}
			for(x=0;x<gameZ; x++) {
				char gname[128];
				ki->load_str(gname, 128);
				unsigned int id = ki->load_int();
				char emulator[128];
				ki->load_str(emulator, 128);
				char owner[32];
				ki->load_str(owner, 32);
				char users[20];
				ki->load_str(users, 20);
				int status = ki->load_char();
				kaillera_game_add_callback(gname, id, emulator, owner, users, status);
			}
			KAILLERAC.tmoutrsttime = p2p_GetTime();

			// Schedule ping spoof announcement (delayed to avoid flood control)
			if (kaillera_spoofing && kaillera_spoof_ping > 0) {
				KAILLERAC.pending_spoof_announce = true;
				KAILLERAC.spoof_announce_time = p2p_GetTime() + 2000;
			}
			break;
		}
	case SERVPING:
		{
			// Only respond to pings if not spoofing (spoofed pongs were sent during login)
			if (!kaillera_spoofing) {
				SendPong();
			}
			break;
		}
	case LOGNSTAT:
		{
			ki->load_short();
			char lsmsg[128];
			ki->load_str(lsmsg, 128);
			kaillera_login_stat_callback(lsmsg);
			break;
		}
	default:
		return;
	}
}

/////////////////////////////////////////////////

void kaillera_step(){
	n02_TRACE();//kprintf(__FILE__ ":%i", __LINE__);
	if (!KAILLERAC.connection) return;
	kaillera_step_active = true;
	if (!KAILLERAC.connection) { kaillera_step_active = false; return; }
	// Short poll: give Winsock a moment to buffer incoming data.
	// Original used 200ms which blocked the Qt event loop.
	k_socket::check_sockets(0,1);
	while (KAILLERAC.connection && KAILLERAC.connection->has_data()){
		//kprintf(__FILE__ ":%i", __LINE__);
		k_instruction ki;
		sockaddr_in saddr;
		if (!KAILLERAC.connection) break;
		if (KAILLERAC.connection->receive_instruction(&ki, false, &saddr)){
			kaillera_ProcessGeneralInstruction(&ki);
		}
	}
	// Send pending ping spoof announcement after delay
	if (KAILLERAC.pending_spoof_announce && p2p_GetTime() >= KAILLERAC.spoof_announce_time) {
		KAILLERAC.pending_spoof_announce = false;
		// Calculate target frame delay from spoof ping: (spoof_ping + 8) / 16
		int target_delay = (kaillera_spoof_ping + 8) / 16;
		char spoof_msg[64];
		sprintf(spoof_msg, "Ping spoofing: %dms (target: %df)", kaillera_spoof_ping, target_delay);
		// Use game chat if in a game room, otherwise lobby chat
		if (KAILLERAC.USERSTAT == 3) {
			kaillera_game_chat_send(spoof_msg);
		} else {
			kaillera_chat_send(spoof_msg);
		}
	}
	if (KAILLERAC.USERSTAT > 1 && KAILLERAC.connection && p2p_GetTime() - KAILLERAC.tmoutrsttime > KAILLERA_TIMEOUT_RESET) {
		KAILLERAC.tmoutrsttime = p2p_GetTime();
		k_instruction trst;
		trst.type = TMOUTRST;
		KAILLERAC.connection->send_instruction(&trst);
		//kaillera_core_debug("TMOUTRST");
	}
	kaillera_step_active = false;
}

void kaillera_chat_send(char * text) {
	if (KAILLERAC.USERSTAT > 1 && KAILLERAC.connection) {
		k_instruction sgc;
		sgc.type = PARTCHAT;
		sgc.store_string(text);
		KAILLERAC.connection->send_instruction(&sgc);
	}
}
void kaillera_game_chat_send(char * text) {
	if (KAILLERAC.USERSTAT > 1 && KAILLERAC.connection) {
		k_instruction sgc;
		sgc.type = GAMECHAT;
		sgc.store_string(text);
		KAILLERAC.connection->send_instruction(&sgc);
	}
}


void kaillera_kick_user (unsigned short id) {
	if (KAILLERAC.USERSTAT > 1 && KAILLERAC.connection) {
		k_instruction sgc;
		sgc.type = GAMRKICK;
		sgc.store_short(id);
		KAILLERAC.connection->send_instruction(&sgc);
	}
}

void kaillera_join_game(unsigned int id, const char* gameName){
	if (!KAILLERAC.connection) return;

	// Set the game name so GAMEBEGN passes it to the emulator
	if (gameName && gameName[0]) {
		strncpy(KAILLERAC.GAME, gameName, sizeof(KAILLERAC.GAME) - 1);
		KAILLERAC.GAME[sizeof(KAILLERAC.GAME) - 1] = 0;
	}

	KAILLERAC.game_id_requested = false;
	KAILLERAC.game_id = id;
	KAILLERAC.leave_game_requested = false;

	KAILLERAC.owner = false;

	k_instruction jog;
	jog.type = GAMRJOIN;
	jog.store_int(id);
	jog.store_char(0);
	jog.store_int(0);
	jog.store_short(-1);
	jog.store_char(KAILLERAC.conset);

	KAILLERAC.connection->send_instruction(&jog);
}
void kaillera_create_game(char * name) {
	if (!KAILLERAC.connection) return;

	strncpy(KAILLERAC.GAME, (name != NULL) ? name : "", sizeof(KAILLERAC.GAME) - 1);
	KAILLERAC.GAME[sizeof(KAILLERAC.GAME) - 1] = 0;
	KAILLERAC.game_id_requested = true;
	k_instruction cg;
	cg.type = GAMEMAKE;
	cg.store_string(name);
	cg.store_char(0);
	cg.store_int(-1);

	KAILLERAC.owner = true;

	KAILLERAC.connection->send_instruction(&cg);
}

void kaillera_leave_game (){
	KAILLERAC.leave_game_requested = true;
	if (KAILLERAC.connection) {
		k_instruction lg;
		lg.type = INSTRUCTION_GAMRLEAV;
		lg.store_short(-1);
		KAILLERAC.connection->send_instruction(&lg);
	}

	// Notify RMG to stop emulation (same as dropping)
	kaillera_player_dropped_callback(KAILLERAC.USERNAME, KAILLERAC.playerno);
	KAILLERAC.PLAYERSTAT = 0;
	kaillera_end_game_callback();

	kaillera_user_game_closed_callback();
}

void kaillera_start_game() {
	KAILLERAC.leave_game_requested = false;

	if (KAILLERAC.connection) {
		// Always send GAMEBEGN to server first - this notifies other players
		k_instruction kx;
		kx.type = INSTRUCTION_GAMEBEGN;
		kx.store_int(-1);
		KAILLERAC.connection->send_instruction(&kx);
	}
}
void kaillera_game_drop(){
	KAILLERAC.leave_game_requested = false;
	if (KAILLERAC.connection) {
		k_instruction kx;
		kx.type=GAMRDROP;
		kx.store_char(0);  // First byte (matches Supraclient)
		kx.store_char(0);  // Second byte (matches Supraclient)
		KAILLERAC.connection->send_instruction(&kx);
	}
	// Notify RMG that the local player dropped (must be before PLAYERSTAT=0)
	kaillera_player_dropped_callback(KAILLERAC.USERNAME, KAILLERAC.playerno);
	KAILLERAC.PLAYERSTAT = 0;
	kaillera_end_game_callback();
}

void kaillera_end_game(){
	//kaillera_core_debug("kailleraEndGame");
	if (KAILLERAC.connection) {
		k_instruction kx;
		kx.type = GAMRDROP;
		kx.store_char(0);  // First byte (matches Supraclient)
		kx.store_char(0);  // Second byte (matches Supraclient)
		KAILLERAC.connection->send_instruction(&kx);
	}
	if (KAILLERAC.USERSTAT > 1) {
		kaillera_player_dropped_callback(KAILLERAC.USERNAME, KAILLERAC.playerno);
		KAILLERAC.PLAYERSTAT = 0;
	}
	kaillera_end_game_callback();
}
bool kaillera_is_game_running(){
	return KAILLERAC.USERSTAT == 3 && KAILLERAC.PLAYERSTAT > 1;
}


inline void kaillera_GameStartSequence(int size){
	n02_TRACE();
	if (!KAILLERAC.connection) {
		KAILLERAC.PLAYERSTAT = 0;
		return;
	}

	KAILLERAC.DATALEN = size;
	KAILLERAC.REQDATALEN = size * KAILLERAC.conset;
	KAILLERAC.USERDATA.reset();
	KAILLERAC.frameno = 0;
	while (KAILLERAC.kaillera_incoming_data_cache.length>0) {
		free(KAILLERAC.kaillera_incoming_data_cache[0]);
		KAILLERAC.kaillera_incoming_data_cache.removei(0);
	}

	while (KAILLERAC.kaillera_gv_queue.length>0) {
		free(KAILLERAC.kaillera_gv_queue[0]);
		KAILLERAC.kaillera_gv_queue.removei(0);
	}

	KAILLERAC.kaillera_incoming_data_cache.clear();
	KAILLERAC.kaillera_gv_queue.clear();

	k_instruction kx;
	kx.type = GAMRSRDY;
	DWORD ti = p2p_GetTime();
	DWORD tit = KAILLERA_TIMEOUT_NETSYNC_RETR_INTERVAL + ti;
	KAILLERAC.connection->send_instruction(&kx);
	while (KAILLERAC.PLAYERSTAT == 1 && KAILLERAC.connection) {
		if (k_socket::check_sockets(0,100) && KAILLERAC.connection && KAILLERAC.connection->has_data()){
			k_instruction ki;
			sockaddr_in saddr;
			if (KAILLERAC.connection->receive_instruction(&ki, false, &saddr)){
				if (ki.type== GAMRSRDY) {
					KAILLERAC.PLAYERSTAT = 2;
					kaillera_core_debug("All players are ready");
					break;
				} else {
					kaillera_ProcessGeneralInstruction(&ki);
				}
			}
		}
		int tx;
		if ((tx = p2p_GetTime() - ti) > KAILLERA_TIMEOUT_NETSYNC) {
			KAILLERAC.PLAYERSTAT = 0;
			break;
		} else {
			kaillera_game_netsync_wait_callback(KAILLERA_TIMEOUT_NETSYNC - tx);
			if (tit <= ti && KAILLERAC.connection) {
				tit = KAILLERA_TIMEOUT_NETSYNC_RETR_INTERVAL + ti;
				KAILLERAC.connection->resend_message(5);
			}
		}
	}
}
inline void kaillera_ProcessGameInstruction(k_instruction * ki) {
	n02_TRACE();
	if (ki->type==GAMEDATA) {
		
		unsigned short len = ki->load_short();
		char * kd;
		const size_t allocSize = static_cast<size_t>(len) + 2;
		
		if (KAILLERAC.kaillera_incoming_data_cache.length == 256){
			kd = KAILLERAC.kaillera_incoming_data_cache[0];
			KAILLERAC.kaillera_incoming_data_cache.removei(0);
			if (*((unsigned short*)kd) != len) {
				char* resized = static_cast<char*>(realloc(kd, allocSize));
				if (resized != nullptr) {
					kd = resized;
				} else {
					char* replacement = static_cast<char*>(malloc(allocSize));
					if (replacement == nullptr) {
						free(kd);
						return;
					}
					free(kd);
					kd = replacement;
				}
			}
		} else {
			kd = static_cast<char*>(malloc(allocSize));
			if (kd == nullptr) {
				return;
			}
		}
		
		*((unsigned short*)kd) = len;
		
		char * kdd = kd + 2;
		ki->load_bytes(kdd, len);
		
		//kaillera_core_debug("GAMEDATA %08X  l = %i", *((DWORD*)kdd), *((short*)kd));
		KAILLERAC.kaillera_incoming_data_cache.add(kd);
		int ilen = len / KAILLERAC.conset;
		
		//kaillera_core_debug("CACHESI = %i, PLC = %i, INCL = %i", KAILLERAC.kaillera_incoming_data_cache.length, PACKETLOSSCOUNT, KAILLERAC.connection->in_cache.length);
		
		for (int x = 0; x < len; x+= ilen) {
			char * kds = (char *)malloc(ilen + 2);
			*((short*)kds) = ilen;
			//kaillera_core_debug("GVADD %i, x=%i D: %i", x, ilen, *((DWORD*)(kds+2)));
			memcpy(kds+2, kdd + x, ilen);
			KAILLERAC.kaillera_gv_queue.add(kds);
		}
		
	} else if (ki->type==GAMCDATA) {
		
		//kaillera_core_debug("GAMCDATA");
		
		int index = ki->load_char();
		
		char * kd = KAILLERAC.kaillera_incoming_data_cache[index];
		
		short len = *((short*)kd);
		
		char * kdd = kd + 2;
		int ilen = len / KAILLERAC.conset;
		for (int x = 0; x < len; x+= ilen) {
			char * kds = (char *)malloc(ilen + 2);
			*((short*)kds) = ilen;
			//kaillera_core_debug("GVADD %i, x=%i D: %i", x, ilen, *((DWORD*)(kds+2)));
			memcpy(kds+2, kdd + x, ilen);
			KAILLERAC.kaillera_gv_queue.add(kds);
		}
	} else {
		kaillera_ProcessGeneralInstruction(ki);
	}
}

int kaillera_modify_play_values (void * values, int size) {
	n02_TRACE();
	if (!KAILLERAC.connection) return -1;

	if (KAILLERAC.USERSTAT > 2 && KAILLERAC.PLAYERSTAT > 0) {
		if (KAILLERAC.PLAYERSTAT == 2) {
			KAILLERAC.USERDATA.put_data(values, size);
			if (KAILLERAC.USERDATA.pos >= KAILLERAC.REQDATALEN && KAILLERAC.connection){
				//outgoing caching goes here
				k_instruction kx;
				kx.type = GAMEDATA;
				kx.store_short(KAILLERAC.REQDATALEN);
				kx.store_bytes(KAILLERAC.USERDATA.buffer, KAILLERAC.REQDATALEN);
				KAILLERAC.connection->send_instruction(&kx);
				KAILLERAC.USERDATA.reset();
			}
			int pix = 0;
			int ttx = 0;
			do {
				if (!KAILLERAC.connection) return -1;
				if (KAILLERAC.connection->has_data() || (k_socket::check_sockets(0,pix) && KAILLERAC.connection && KAILLERAC.connection->has_data())){
					k_instruction ki;
					sockaddr_in saddr;
					if (KAILLERAC.connection->receive_instruction(&ki, false, &saddr)){
						kaillera_ProcessGameInstruction(&ki);
					}
				}
				if (KAILLERAC.frameno < KAILLERAC.dframeno) {
					KAILLERAC.frameno++;
					return 0;
				}
				if (KAILLERAC.kaillera_gv_queue.length <= 0) {
					if (ttx == 0) {
						pix++;
						ttx = p2p_GetTime();
					} else {
						DWORD tttx = p2p_GetTime();
						if (tttx - ttx > 10) {
							ttx = tttx;
							if (KAILLERAC.connection)
								KAILLERAC.connection->resend_message(5);
							pix++;
							if (pix == 500){
								kaillera_core_debug("Lost Connection");
								kaillera_game_drop();
								return -1;
							}
						}
					}
				}
			} while ((KAILLERAC.kaillera_gv_queue.length <= 0) && KAILLERAC.PLAYERSTAT > 1 && KAILLERAC.connection);
			if (KAILLERAC.PLAYERSTAT > 1 && KAILLERAC.kaillera_gv_queue.length > 0) {
				char * kd = KAILLERAC.kaillera_gv_queue[0];
				KAILLERAC.kaillera_gv_queue.removei(0);
				int l = *((short*)kd);
				memcpy(values, kd+2, l);
				free(kd);
				KAILLERAC.frameno++;
				return l;
			} else {
				return -1;
			}
		} else {
			kaillera_GameStartSequence(size);
			return kaillera_modify_play_values(values, size);
		}
	}
	return -1;
}



int kaillera_ping_server(char * host, int port, int limit) {
	k_socket psk;
	if (!psk.initialize(0) || !psk.set_address(host, port)) {
		return -1;
	}
	k_socket::check_sockets(0,0);
	const auto ti = std::chrono::steady_clock::now();
	if (!psk.send("PING", 5)) {
		return -1;
	}
	
	while (!psk.has_data()) {
		const int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - ti).count();
		if (elapsed >= limit) {
			return -1;
		}
		k_socket::check_sockets(0, std::max(1, std::min(10, limit - elapsed)));
	}
	
	return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - ti).count();
}
