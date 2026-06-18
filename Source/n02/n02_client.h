/*
 * n02 - Open Kaillera Client
 * Public API header for static library integration
 */
#pragma once

#include <functional>
#include <string>

// Forward declaration - internal struct defined in kailleraclient.h
struct kailleraInfos;

namespace n02 {

// Initialize n02 subsystem (sockets, settings, modules)
// Returns 0 on success
int init();

// Shutdown n02 subsystem
void shutdown();

// Get version string
void getVersion(char *version);

// Set emulator callback info (app name, game list, callbacks)
void setInfos(kailleraInfos *infos);

// Main server dialog / state machine entry point (blocking)
// parent: unused in Qt mode, kept for API compatibility
void selectServerDialog(void* parent);

// Per-frame input synchronization
// values: input buffer (modified in place with synced data from all players)
// size: size of local input in bytes
// Returns: number of bytes of synced data, 0 during delay frames, -1 on error
int modifyPlayValues(void *values, int size);

// Append a synchronized-inputs record (0x12 marker + size + bytes) to the
// open recording. Mirrors what modifyPlayValues writes for normal play,
// but is callable from outside n02's frame loop — the P2P rollback path
// uses GekkoNet for input sync so it never reaches modifyPlayValues.
// No-op if no recording is open.
void recordingWriteInputs(const void* values, int size);

// Send chat message to other players
void chatSend(char *text);

// End current game session
void endGame();

// Get the current frame delay (for Kaillera server mode)
int getFrameDelay();

// Get the current Kaillera user ID (server mode)
unsigned short getUserId();

// Get the current active mode index (0=P2P, 1=Server, 2=Playback)
int getActiveMode();

// Activate a specific mode
// mode: 0=P2P, 1=Server, 2=Playback
// Returns true if mode changed
bool activateMode(int mode);

// Set the records directory path used for .krec writes
void setRecordsDirectory(const std::string& recordsDirectory);

// Check if a game is currently in the "running" state (KSSDFA state 2)
bool isGameRunning();

// Get KSSDFA state (0=polling, 1=game callback pending, 2=game running, 3=shutdown)
int getState();

// Set KSSDFA input to trigger state transition
void setStateInput(int input);

// Reset the n02 state machine to lobby/polling state without emitting game lifecycle callbacks.
void resetStateMachine();

// Process one step of the state machine (non-blocking)
// Returns true if state machine is still active (state != 3)
bool processStateMachineStep();

// UI Callback interface - set by Qt UI layer
struct UICallbacks {
    // P2P callbacks
    std::function<void(char* nick, char* msg)> p2pChatCallback;
    std::function<void(char* game, int playerno, int maxplayers)> p2pGameCallback;
    std::function<void()> p2pEndGameCallback;
    std::function<void(char* nick, int playerno)> p2pClientDroppedCallback;
    std::function<void(char* msg)> p2pDebugCallback;
    std::function<void(char* game)> p2pHostedGameCallback;
    std::function<void(int ping)> p2pPingCallback;
    std::function<void()> p2pPeerLeftCallback;
    std::function<void()> p2pPeerJoinedCallback;
    std::function<void(char* name, char* app)> p2pPeerInfoCallback;
    std::function<int()> p2pGetSelectedDelayCallback;
    std::function<void(char* cmd, int len, void* sadr)> p2pSsrvPacketRecvCallback;
    std::function<void(char* host)> p2pFodippCallback;

    // Kaillera callbacks
    std::function<void(char* name, int ping, int status, unsigned short id, char conn)> kailleraUserAddCallback;
    std::function<void(char* gname, unsigned int id, char* emulator, char* owner, char* users, char status)> kailleraGameAddCallback;
    std::function<void(char* name, char* msg)> kailleraChatCallback;
    std::function<void(char* name, char* msg)> kailleraGameChatCallback;
    std::function<void(char* name, char* msg)> kailleraMotdCallback;
    std::function<void(char* name, int ping, unsigned short id, char conn)> kailleraUserJoinCallback;
    std::function<void(char* name, char* quitmsg, unsigned short id)> kailleraUserLeaveCallback;
    std::function<void(char* gname, unsigned int id, char* emulator, char* owner)> kailleraGameCreateCallback;
    std::function<void(unsigned int id)> kailleraGameCloseCallback;
    std::function<void(unsigned int id, char status, int players, int maxplayers)> kailleraGameStatusChangeCallback;
    std::function<void()> kailleraUserGameCreateCallback;
    std::function<void()> kailleraUserGameJoinedCallback;
    std::function<void()> kailleraUserGameClosedCallback;
    std::function<void(char* name, int ping, unsigned short id, char conn)> kailleraPlayerAddCallback;
    std::function<void(char* username, int ping, unsigned short uid, char connset)> kailleraPlayerJoinedCallback;
    std::function<void(char* user, unsigned short id)> kailleraPlayerLeftCallback;
    std::function<void()> kailleraUserKickedCallback;
    std::function<void(char* lsmsg)> kailleraLoginStatCallback;
    std::function<void(char* user, int gdpl)> kailleraPlayerDroppedCallback;
    std::function<void(char* game, char player, char players)> kailleraGameStartCallback;
    std::function<void(int tx)> kailleraGameNetsyncWaitCallback;
    std::function<void()> kailleraEndGameCallback;
    std::function<void(char* msg)> kailleraDebugCallback;
    std::function<void(char* msg)> kailleraErrorCallback;
    std::function<void()> recordingFileClosedCallback;
};

// Register UI callbacks (called by Qt UI layer)
void setUICallbacks(const UICallbacks& callbacks);

// Get current UI callbacks
UICallbacks& getUICallbacks();

// Load and play a .krec recording file
// Returns true on success
bool playbackLoad(const char* filename);

// Check if playback is currently active
bool isPlaybackActive();

// Seek playback to a specific frame index (0-based). Only safe while paused.
bool playbackSeekToFrame(int frameIdx);

// Get the current playback frame index
int playbackGetCurrentFrame();

// Get the total number of frames in the loaded recording
int playbackGetTotalFrames();

} // namespace n02

// Global recording flag — set by Qt UI, read by recording stub
extern bool n02_kaillera_recording_enabled;
