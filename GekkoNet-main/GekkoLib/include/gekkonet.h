/*
BSD 2-Clause License

Copyright (c) 2024-2026, Jamie Meyer

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#ifdef _WIN32
#ifdef GEKKONET_STATIC
    // Static library - no import/export needed
#define GEKKONET_API
#else
#ifdef GEKKONET_DLL_EXPORT
    // Building the DLL
#define GEKKONET_API __declspec(dllexport)
#else
    // Using the DLL
#define GEKKONET_API __declspec(dllimport)
#endif
#endif
#else
    // Non-Windows platforms don't need special export macros
#define GEKKONET_API
#endif

// GekkoNet is mostly self contained memory wise using internal memory buffers.
// Technically the user shouldnt need to free or create any memory (look at the examples).
// The only case the user needs to create memory is when slotting in their own GekkoNetAdapter
typedef struct GekkoSession GekkoSession;

typedef enum GekkoSessionType {
    GekkoGameSession, // session for an active player.
    GekkoStressSession, // session to test your local simulation for state desyncs.
    GekkoSpectateSession, // session for spectators watching an active player.
} GekkoSessionType;

typedef struct GekkoConfig {
    unsigned char num_players;
    unsigned char max_spectators;
    unsigned char input_prediction_window;
    unsigned int spectator_delay;
    unsigned int input_size;
    unsigned int state_size;
    bool limited_saving;
    bool desync_detection;
    unsigned int check_distance;
} GekkoConfig;

typedef enum GekkoPlayerType {
    GekkoLocalPlayer,
    GekkoRemotePlayer,
    GekkoSpectator
} GekkoPlayerType;

typedef struct GekkoNetAddress {
    void* data;
    unsigned int size;
} GekkoNetAddress;

typedef struct GekkoNetResult {
    GekkoNetAddress addr;
    unsigned int data_len;
    void* data;
} GekkoNetResult;

typedef struct GekkoNetAdapter {
    // send data to another peer
    void (*send_data)(GekkoNetAddress* addr, const char* data, int length);
    // receive all packets accumulated between the last frame and now
    // the results within the array will be freed after use using the free_data function
    // the array itself won't be touched
    GekkoNetResult** (*receive_data)(int* length);
    // free data function so gekkonet can cleanup data that the user created.
    void (*free_data)(void* data_ptr);
} GekkoNetAdapter;

typedef enum GekkoGameEventType {
    GekkoEmptyGameEvent = -1,
    GekkoAdvanceEvent,
    GekkoSaveEvent,
    GekkoLoadEvent
} GekkoGameEventType;

typedef struct GekkoGameEvent {
    GekkoGameEventType type;

    union GekkoEventData {
        // events 
        struct GekkoAdvance {
            int frame;
            unsigned int input_len;
            unsigned char* inputs;
            bool rolling_back;
            bool running_ahead;
        } adv;
        struct GekkoSave {
            int frame;
            unsigned int* checksum;
            unsigned int* state_len;
            unsigned char* state;
        } save;
        struct GekkoLoad {
            int frame;
            unsigned int state_len;
            unsigned char* state;
        } load;
    } data;
} GekkoGameEvent;

typedef enum GekkoSessionEventType {
    GekkoEmptySessionEvent = -1,
    GekkoPlayerSyncing,
    GekkoPlayerConnected,
    GekkoPlayerDisconnected,
    GekkoSessionStarted,
    GekkoSpectatorPaused,
    GekkoSpectatorUnpaused,
    GekkoDesyncDetected
} GekkoSessionEventType;

typedef struct GekkoSessionEvent {
    GekkoSessionEventType type;

    union GekkoSessionData {
        struct GekkoSyncing {
            int handle;
            unsigned char current;
            unsigned char max;
        } syncing;
        struct GekkoConnected {
            int handle;
        } connected;
        struct GekkoDisconnected {
            int handle;
        } disconnected;
        struct GekkoDesynced {
            int frame;
            unsigned int local_checksum;
            unsigned int remote_checksum;
            int remote_handle;
        } desynced;
    } data;
} GekkoSessionEvent;

typedef struct GekkoNetworkStats {
    float kb_sent;
    float kb_received;
    unsigned short last_ping;
    float avg_ping;
    float jitter;
} GekkoNetworkStats;

// Public Facing API
GEKKONET_API bool gekko_create(GekkoSession** session, GekkoSessionType session_type);

GEKKONET_API bool gekko_destroy(GekkoSession** session);

GEKKONET_API void gekko_start(GekkoSession* session, GekkoConfig* config);

GEKKONET_API void gekko_net_adapter_set(GekkoSession* session, GekkoNetAdapter* adapter);

GEKKONET_API int gekko_add_actor(GekkoSession* session, GekkoPlayerType player_type, GekkoNetAddress* addr);

GEKKONET_API void gekko_set_local_delay(GekkoSession* session, int player, unsigned char delay);

GEKKONET_API void gekko_set_runahead(GekkoSession* session, unsigned char runahead);

GEKKONET_API void gekko_add_local_input(GekkoSession* session, int player, void* input);

GEKKONET_API GekkoGameEvent** gekko_update_session(GekkoSession* session, int* count);

GEKKONET_API GekkoSessionEvent** gekko_session_events(GekkoSession* session, int* count);

GEKKONET_API float gekko_frames_ahead(GekkoSession* session);

GEKKONET_API void gekko_network_stats(GekkoSession* session, int player, GekkoNetworkStats* stats);

GEKKONET_API void gekko_network_poll(GekkoSession* session);

// Force a remote actor into the Disconnected state immediately, instead of
// waiting out the NetStats::DISCONNECT_TIMEOUT silence window. `handle` is the
// value returned by gekko_add_actor for the remote player. Used when an
// out-of-band channel (e.g. a lobby server) already knows the peer is gone, so
// the session stops stalling on their input right away. No-op if the handle is
// not a connected remote.
GEKKONET_API void gekko_disconnect_player(GekkoSession* session, int handle);

// How long a remote player may stay silent before this session marks them
// Disconnected (default NetStats::DISCONNECT_TIMEOUT). 0 disables the timeout,
// so players only leave through gekko_disconnect_player. Each peer applies the
// timeout on its own, so a hitch near the limit can drop a player on some peers
// but not others; disable it when an out-of-band channel (e.g. a lobby server)
// decides drops for every peer. Spectator timeouts are unaffected.
GEKKONET_API void gekko_set_disconnect_timeout(GekkoSession* session, unsigned int timeout_ms);

#ifndef GEKKONET_NO_ASIO

GEKKONET_API GekkoNetAdapter* gekko_default_adapter(unsigned short port);

GEKKONET_API bool gekko_default_adapter_destroy();

#endif // GEKKONET_NO_ASIO

#ifdef __cplusplus  
}
#endif
