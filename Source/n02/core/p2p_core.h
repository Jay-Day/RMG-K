#pragma once

#include "../kcore/kaillera_core.h"

#define P2P_CHAT_BUFFER_LEN 32
#define P2P_GAMESYNC_WAIT 900
#define P2P_GAMECB_WAIT 200
#define P2P_VERSION N02_VER

void p2p_chat_callback(char * nick, char * msg);
//bool p2p_add_delay_callback();
int p2p_getSelectedDelay();
void p2p_game_callback(char * game, int playerno, int maxplayers);
void p2p_end_game_callback();
void p2p_client_dropped_callback(char * nick, int playerno);
void __cdecl p2p_core_debug(char * arg_0, ...);

bool p2p_core_cleanup();
bool p2p_core_initialize(bool host, int port, char * appname, char * gamename, char * username);
bool p2p_core_connect(char * ip, int port);
void p2p_print_core_status();
void p2p_retransmit();
void p2p_drop_game();
void p2p_set_ready(bool bx);
void p2p_ping();
void p2p_send_chat(char * xxx);
bool p2p_kick_peer();
bool p2p_disconnect();
void p2p_step();
bool p2p_is_connected();
int p2p_get_frames_count();
void p2p_fodipp();
void p2p_fodipp_callback(char * host);

int p2p_modify_play_values(void *values, int size);
int p2p_core_get_port();
bool p2p_core_get_peer_endpoint(char *ip, int ip_len, int *port);
void p2p_hosted_game_callback(char * game);
void p2p_ping_callback(int PING);

void p2p_send_ssrv_packet(char * cmd, int len, char * host, int port);
void p2p_ssrv_packet_recv_callback(char *cmd, int len, void*sadr);

void p2p_send_ssrv_packet(char * cmd, int len, void * sadr);

bool p2p_rollback_transport_send(const char *data, int len);
int p2p_rollback_transport_receive(char *data, int data_len, char *addr, int addr_len);
void p2p_rollback_transport_clear();
bool p2p_rollback_process_control();


void p2p_peer_left_callback();
void p2p_peer_joined_callback();
void p2p_peer_info_callback(char* p33rname, char* app);
