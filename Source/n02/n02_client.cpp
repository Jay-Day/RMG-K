/*
 * n02 - Open Kaillera Client
 * Main client implementation
 * Refactored from kailleraclient.cpp:
 * - Removed DllMain, __declspec(dllexport), __stdcall
 * - Moved to n02 namespace
 * - Replaced __try/__except SEH with try/catch
 * - Replaced Win32 file I/O (CreateFile/WriteFile) with std::fstream
 * - Replaced PeekMessage/DispatchMessage with callback hook
 * - Stubbed out GUI function pointers (Qt UI in Phase 5)
 */

#include "kailleraclient.h"
#include "n02_client.h"

#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <fstream>
#include <filesystem>
#include <vector>

#include "common/nThread.h"
#include "common/k_socket.h"
#include "common/nSettings.h"

#include "errr.h"

// Forward declarations for core protocol functions
// P2P mode
extern int p2p_modify_play_values(void *values, int size);
extern void p2p_send_chat(char * xxx);
extern void p2p_EndGame();

// Kaillera mode
extern int kaillera_modify_play_values(void * values, int size);
extern void kaillera_game_chat_send(char * text);
extern void kaillera_end_game();
extern unsigned short kaillera_get_user_id();
extern bool kaillera_SelectServerDlgStep();
extern bool p2p_SelectServerDlgStep();
extern int kaillera_get_delay();

///////////////////////////////////////////////////////////////////////////////
// Global state
///////////////////////////////////////////////////////////////////////////////

kailleraInfos infos;
kailleraInfos infos_copy;

char GAME[128];
char APP[128];
int playerno;
int numplayers;
char recording_player_names[4][32];

// Settings used by core protocol files
int kaillera_spoof_ping = 0;
int kaillera_30fps_mode = 0;
int p2p_30fps_mode = 0;

///////////////////////////////////////////////////////////////////////////////
// Module system
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    bool (*SSDSTEP)();
    int  (*MPV)(void*, int);
    void (*ChatSend)(char*);
    void (*EndGame)();
    bool (*RecordingEnabled)();
} n02_MODULE;

static n02_MODULE active_mod;
static n02_MODULE mod_kaillera;
static n02_MODULE mod_p2p;
static n02_MODULE mod_playback;
static int mod_active_no = -1;
static int active_mod_index;
static bool mod_rerun;

///////////////////////////////////////////////////////////////////////////////
// KSSDFA State Machine
///////////////////////////////////////////////////////////////////////////////

int KSSDFAST[16] = {
    0, 0, 1, 1,
    1, 0, 1, 0,
    2, 0, 2, 0,
    3, 3, 3, 3
};

KSSDFA_ KSSDFA;

char * gamelist = 0;

///////////////////////////////////////////////////////////////////////////////
// Recording system (std::fstream replacement for Win32 file I/O)
///////////////////////////////////////////////////////////////////////////////

static std::ofstream recording_file;
static std::filesystem::path recording_directory_path = std::filesystem::path("records");

static std::filesystem::path pathFromUtf8String(const std::string& utf8) {
#if defined(__cpp_char8_t)
    std::u8string converted;
    converted.reserve(utf8.size());
    for (unsigned char ch : utf8) {
        converted.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(converted);
#else
    return std::filesystem::u8path(utf8);
#endif
}

static std::filesystem::path pathFromUtf8String(const char* utf8) {
    if (utf8 == nullptr) {
        return std::filesystem::path();
    }
    return pathFromUtf8String(std::string(utf8));
}

class RecordingBufferC {
public:
    char buffer[500];
    char* position;

    void reset() {
        position = buffer;
    }

    int len() {
        ptrdiff_t diff = position - buffer;
        if (diff < 0)
            return 0;
        if (diff > INT_MAX)
            return INT_MAX;
        return (int)diff;
    }

    void put_char(char x) {
        *position++ = x;
    }

    void put_short(short x) {
        *(short*)position = x;
        position += 2;
    }

    void put_bytes(char* x, int len) {
        for (int t = 0; t < len; t++) {
            *position++ = *x++;
        }
    }

    void write() {
        if (recording_file.is_open()) {
            recording_file.write(buffer, len());
            recording_file.flush();
        }
        reset();
    }
} RecordingBuffer;

static void close_recording() {
    if (recording_file.is_open()) {
        if (RecordingBuffer.len() > 0)
            RecordingBuffer.write();
        recording_file.close();

        auto& callbacks = n02::getUICallbacks();
        if (callbacks.recordingFileClosedCallback) {
            callbacks.recordingFileClosedCallback();
        }
    }
}

static int gameCallbackWrapper(char *game, int player, int numplayers_arg) {
    close_recording();

    if (active_mod.RecordingEnabled && active_mod.RecordingEnabled()) {
        n02_TRACE();
        RecordingBuffer.reset();

        // Create records directory
        std::filesystem::create_directories(recording_directory_path);

        // Build filename: YYMMDDHHMMSS-Player1-Player2.krec
        // Player names are truncated to fit within MAX_PATH (260) on Windows.
        time_t t = time(0);
        tm * lt = localtime(&t);
        char datePart[16];
        strftime(datePart, sizeof(datePart), "%y%m%d%H%M%S", lt);

        std::string filename = datePart;

        // Collect non-empty player names
        std::vector<std::string> names;
        for (int i = 0; i < numplayers_arg && i < 4; i++)
        {
            if (recording_player_names[i][0] != 0)
            {
                std::string name(recording_player_names[i]);
                // Replace characters unsafe for filenames
                for (char& c : name)
                {
                    if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                        c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                        c = '_';
                }
                names.push_back(name);
            }
        }

        if (!names.empty())
        {
            // Calculate path budget:
            // MAX_PATH(260) - directory - separator(1) - timestamp(12) - ".krec"(5)
            std::string dirStr = recording_directory_path.string();
            int dirLen = (int)dirStr.size();
            int budget = 260 - dirLen - 1 - 12 - 5;

            // Budget for player names: subtract "-" separator before each name
            int nameBudget = budget - (int)names.size();

            if (nameBudget >= 3 * (int)names.size())
            {
                // Truncate each name to fit. Two passes: first pass uses equal
                // share, second pass redistributes leftover from short names.
                int numNames = (int)names.size();
                std::vector<int> lengths(numNames);
                int maxPerName = nameBudget / numNames;

                // First pass: clamp each name, track leftover
                int leftover = 0;
                int needMore = 0;
                for (int i = 0; i < numNames; i++)
                {
                    if ((int)names[i].size() <= maxPerName)
                    {
                        lengths[i] = (int)names[i].size();
                        leftover += maxPerName - lengths[i];
                    }
                    else
                    {
                        lengths[i] = maxPerName;
                        needMore++;
                    }
                }

                // Second pass: distribute leftover to truncated names
                if (leftover > 0 && needMore > 0)
                {
                    int extra = leftover / needMore;
                    for (int i = 0; i < numNames; i++)
                    {
                        if (lengths[i] == maxPerName && (int)names[i].size() > maxPerName)
                        {
                            int newLen = maxPerName + extra;
                            if (newLen > (int)names[i].size())
                                newLen = (int)names[i].size();
                            lengths[i] = newLen;
                        }
                    }
                }

                for (int i = 0; i < numNames; i++)
                {
                    filename += "-";
                    filename += names[i].substr(0, lengths[i]);
                }
            }
        }

        filename += ".krec";

        std::filesystem::path recordingPath = recording_directory_path / filename;
        recording_file.open(recordingPath, std::ios::binary | std::ios::trunc);

        if (!recording_file.is_open()) {
            std::string pathString = recordingPath.string();
            kprintf("recording %s failed", pathString.c_str());
        } else {
            char GameName[128];
            strncpy(GameName, (game != NULL) ? game : "", sizeof(GameName) - 1);
            GameName[sizeof(GameName) - 1] = 0;

            recording_file.write("KRC1", 4);
            recording_file.write(infos_copy.appName ? infos_copy.appName : "", 128);
            recording_file.write(GameName, 128);
            time_t mytime = time(NULL);
            recording_file.write((char*)&mytime, 4);
            recording_file.write((char*)&player, 4);
            recording_file.write((char*)&numplayers_arg, 4);
            recording_file.write((char*)recording_player_names, 128);
        }
        n02_TRACE();
    }

    if (infos_copy.gameCallback)
    {
        const int result = infos_copy.gameCallback(game, player, numplayers_arg);
        return result;
    }
    return 0;
}

static void chatReceivedCallbackWrapper(char *nick, char *text) {
    if (recording_file.is_open()) {
        RecordingBuffer.put_char(8);
        {
            size_t nickLen = strlen(nick) + 1;
            size_t textLen = strlen(text) + 1;
            int nickLenI = (nickLen > (size_t)INT_MAX) ? INT_MAX : (int)nickLen;
            int textLenI = (textLen > (size_t)INT_MAX) ? INT_MAX : (int)textLen;
            RecordingBuffer.put_bytes(nick, nickLenI);
            RecordingBuffer.put_bytes(text, textLenI);
        }
    }
    if (infos_copy.chatReceivedCallback)
        infos_copy.chatReceivedCallback(nick, text);
}

static void clientDroppedCallbackWrapper(char *nick, int playernb) {
    if (recording_file.is_open()) {
        RecordingBuffer.put_char(20);
        {
            size_t nickLen = strlen(nick) + 1;
            int nickLenI = (nickLen > (size_t)INT_MAX) ? INT_MAX : (int)nickLen;
            RecordingBuffer.put_bytes(nick, nickLenI);
        }
        RecordingBuffer.put_bytes((char*)&playernb, 4);
    }
    if (infos_copy.clientDroppedCallback)
        infos_copy.clientDroppedCallback(nick, playernb);
}

///////////////////////////////////////////////////////////////////////////////
// Stub functions for UI modes (replaced by Qt dialogs in Phase 5)
///////////////////////////////////////////////////////////////////////////////

// Kaillera server mode stubs
static bool kaillera_SelectServerDlgStep_stub() {
    extern void kaillera_step();
    extern bool kaillera_core_initialized;
    if (kaillera_core_initialized)
        kaillera_step();
    return true;
}

bool n02_kaillera_recording_enabled = false;

static bool kaillera_RecordingEnabled_stub() {
    return n02_kaillera_recording_enabled;
}

// P2P mode stubs
static bool p2p_SelectServerDlgStep_stub() {
    extern void p2p_step();
    extern bool p2p_core_initialized;
    if (p2p_core_initialized)
        p2p_step();
    return true;
}

static void p2p_EndGame_stub() {
    extern void p2p_drop_game();
    p2p_drop_game();
}

static bool p2p_RecordingEnabled_stub() {
    return n02_kaillera_recording_enabled;
}

///////////////////////////////////////////////////////////////////////////////
// Playback system
///////////////////////////////////////////////////////////////////////////////

static bool player_playing = false;
static bool player_was_dropped[16] = {};

class PlayBackBufferC {
public:
    char* buffer = nullptr;
    char* ptr = nullptr;
    char* end = nullptr;

    std::vector<char*> frameIndex;
    int currentFrameIdx = 0;

    void load_bytes(void* dest, unsigned int len) {
        if (ptr + 10 < end) {
            unsigned int avail = (unsigned int)(end - ptr);
            unsigned int n = (len < avail) ? len : avail;
            memcpy(dest, ptr, n);
            ptr += n;
        }
    }
    void load_str(char* dest, unsigned int maxlen) {
        unsigned int slen = (unsigned int)strlen(ptr) + 1;
        if (slen > maxlen) slen = maxlen;
        unsigned int avail = (unsigned int)(end - ptr + 1);
        if (slen > avail) slen = avail;
        load_bytes(dest, slen);
        dest[slen] = 0;
    }
    int load_int() { int x; load_bytes(&x, 4); return x; }
    unsigned char load_char() { unsigned char x; load_bytes(&x, 1); return x; }
    unsigned short load_short() { unsigned short x; load_bytes(&x, 2); return x; }
} PlayBackBuffer;

static void player_EndGame();

static int player_MPV(void* values, int size) {
    if (player_playing) {
        if (PlayBackBuffer.ptr + 10 < PlayBackBuffer.end) {
            char b = PlayBackBuffer.load_char();
            if (b == 0x12) {
                int l = PlayBackBuffer.load_short();
                if (l < 0) {
                    player_EndGame();
                    return -1;
                }
                if (l > 0)
                    PlayBackBuffer.load_bytes((char*)values, l);
                PlayBackBuffer.currentFrameIdx++;
                return l;
            }
            if (b == 20) {
                char playernick[100];
                PlayBackBuffer.load_str(playernick, 100);
                int pn = PlayBackBuffer.load_int();
                if (pn >= 1 && pn <= 16)
                    player_was_dropped[pn - 1] = true;
                if (pn == playerno) {
                    player_EndGame();
                    return -1;
                }
                return player_MPV(values, size);
            }
            if (b == 8) {
                char nick[100];
                char msg[500];
                PlayBackBuffer.load_str(nick, 100);
                PlayBackBuffer.load_str(msg, 500);
                if (infos.chatReceivedCallback)
                    infos.chatReceivedCallback(nick, msg);
                return player_MPV(values, size);
            }
        } else {
            player_EndGame();
        }
    }
    return -1;
}

static void player_EndGame() {
    player_playing = false;
    for (int i = numplayers; i >= 1; i--) {
        if (i <= 16 && player_was_dropped[i - 1])
            continue;
        if (infos.clientDroppedCallback) {
            char dropname[32];
            snprintf(dropname, sizeof(dropname), "Player %d", i);
            infos.clientDroppedCallback(dropname, i);
        }
    }
    KSSDFA.input = KSSDFA_END_GAME;
    KSSDFA.state = 0;
    if (PlayBackBuffer.buffer) {
        free(PlayBackBuffer.buffer);
        PlayBackBuffer.buffer = nullptr;
    }
}

static bool player_SSDSTEP() {
    return false;
}

static void player_ChatSend(char*) {
    // Can't send chat during playback
}

static bool player_RecordingEnabled() {
    return false;
}

static bool player_play(const char* fn) {
    if (fn == nullptr)
        return false;

    std::filesystem::path playbackPath = pathFromUtf8String(fn);
    std::ifstream file(playbackPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    std::streamsize len = file.tellg();
    file.seekg(0, std::ios::beg);

    if (len < 272) {
        file.close();
        return false;
    }

    PlayBackBuffer.buffer = (char*)malloc((size_t)len + 66);
    if (!PlayBackBuffer.buffer) {
        file.close();
        return false;
    }

    file.read(PlayBackBuffer.buffer, len);
    file.close();

    PlayBackBuffer.end = PlayBackBuffer.buffer + len;

    // Detect format version
    bool isKRC1 = (memcmp(PlayBackBuffer.buffer, "KRC1", 4) == 0);
    size_t headerSize = isKRC1 ? 400 : 272;

    if ((size_t)len < headerSize) {
        free(PlayBackBuffer.buffer);
        PlayBackBuffer.buffer = nullptr;
        return false;
    }

    PlayBackBuffer.ptr = PlayBackBuffer.buffer + 132;
    PlayBackBuffer.load_str(GAME, 128);

    PlayBackBuffer.ptr = PlayBackBuffer.buffer + 264;
    playerno = PlayBackBuffer.load_int();
    numplayers = PlayBackBuffer.load_int();

    // Skip to record data
    PlayBackBuffer.ptr = PlayBackBuffer.buffer + headerSize;

    // Build frame index for seeking
    PlayBackBuffer.frameIndex.clear();
    {
        char* scan = PlayBackBuffer.buffer + headerSize;
        while (scan + 1 < PlayBackBuffer.end) {
            unsigned char type = (unsigned char)*scan;
            if (type == 0x12) {
                PlayBackBuffer.frameIndex.push_back(scan);
                scan++;
                if (scan + 2 > PlayBackBuffer.end) break;
                unsigned short rlen = *(unsigned short*)scan;
                scan += 2;
                if (rlen > 0) {
                    if (scan + rlen > PlayBackBuffer.end) break;
                    scan += rlen;
                }
            } else if (type == 0x14) {
                scan++;
                while (scan < PlayBackBuffer.end && *scan != 0) scan++;
                if (scan < PlayBackBuffer.end) scan++;
                scan += 4;
            } else if (type == 0x08) {
                scan++;
                while (scan < PlayBackBuffer.end && *scan != 0) scan++;
                if (scan < PlayBackBuffer.end) scan++;
                while (scan < PlayBackBuffer.end && *scan != 0) scan++;
                if (scan < PlayBackBuffer.end) scan++;
            } else {
                break;
            }
        }
    }
    PlayBackBuffer.currentFrameIdx = 0;

    player_playing = true;
    memset(player_was_dropped, 0, sizeof(player_was_dropped));

    KSSDFA.input = KSSDFA_START_GAME;

    return true;
}

static bool player_seekToFrame(int frameIdx) {
    if (!player_playing) return false;
    if (frameIdx < 0 || frameIdx >= (int)PlayBackBuffer.frameIndex.size()) return false;
    PlayBackBuffer.ptr = PlayBackBuffer.frameIndex[frameIdx];
    PlayBackBuffer.currentFrameIdx = frameIdx;
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Mode management
///////////////////////////////////////////////////////////////////////////////

bool activate_mode(int mode) {
    if (mode != mod_active_no) {
        mod_active_no = mode;
        active_mod_index = mode;
        mod_rerun = true;

        if (mode == 0)
            active_mod = mod_p2p;
        else if (mode == 1)
            active_mod = mod_kaillera;
        else
            active_mod = mod_playback;

        return true;
    }
    return false;
}

int get_active_mode_index() {
    return active_mod_index;
}

static void loadSettings() {
    nSettings::Initialize("okai");
    active_mod_index = nSettings::get_int("AM", 1);
    nSettings::Terminate();
}

static void saveSettings() {
    nSettings::Initialize("okai");
    nSettings::set_int("AM", active_mod_index);
    nSettings::Terminate();
}

///////////////////////////////////////////////////////////////////////////////
// UI Callbacks (stub implementations, wired up in Phase 4)
///////////////////////////////////////////////////////////////////////////////

static n02::UICallbacks s_uiCallbacks;

///////////////////////////////////////////////////////////////////////////////
// n02 namespace - Public API
///////////////////////////////////////////////////////////////////////////////

namespace n02 {

void getVersion(char *version) {
    memcpy(version, "0.9", 4);
}

int init() {
    k_socket::Initialize();
    loadSettings();

    // Setup playback module
    mod_playback.MPV = player_MPV;
    mod_playback.EndGame = player_EndGame;
    mod_playback.SSDSTEP = player_SSDSTEP;
    mod_playback.ChatSend = player_ChatSend;
    mod_playback.RecordingEnabled = player_RecordingEnabled;

    // Setup kaillera server module
    mod_kaillera.SSDSTEP = kaillera_SelectServerDlgStep_stub;
    mod_kaillera.MPV = kaillera_modify_play_values;
    mod_kaillera.ChatSend = kaillera_game_chat_send;
    mod_kaillera.EndGame = kaillera_end_game;
    mod_kaillera.RecordingEnabled = kaillera_RecordingEnabled_stub;

    // Setup P2P module
    mod_p2p.SSDSTEP = p2p_SelectServerDlgStep_stub;
    mod_p2p.MPV = p2p_modify_play_values;
    mod_p2p.ChatSend = p2p_send_chat;
    mod_p2p.EndGame = p2p_EndGame_stub;
    mod_p2p.RecordingEnabled = p2p_RecordingEnabled_stub;

    activate_mode(active_mod_index);

    return 0;
}

void shutdown() {
    k_socket::Cleanup();
    saveSettings();
    if (gamelist != 0)
        free(gamelist);
    gamelist = 0;
    close_recording();
}

void setInfos(kailleraInfos *infos_) {
    infos = *infos_;
    strncpy(APP, (infos.appName != NULL) ? infos.appName : "", 127);
    APP[127] = 0;

    if (gamelist != 0)
        free(gamelist);
    gamelist = 0;

    char * xx = infos.gameList;
    size_t l = 0;
    if (xx != 0) {
        size_t p;
        while ((p = strlen(xx)) != 0) {
            l += p + 1;
            xx += p + 1;
        }
        l++;
        gamelist = (char*)malloc(l);
        memcpy(gamelist, infos.gameList, l);
    }
    infos.gameList = gamelist;
    infos_copy = infos;

    // Wrap callbacks for recording support
    infos.chatReceivedCallback = chatReceivedCallbackWrapper;
    infos.clientDroppedCallback = clientDroppedCallbackWrapper;
    infos.gameCallback = gameCallbackWrapper;
}

void selectServerDialog(void* parent) {
    (void)parent;

    KSSDFA.state = 0;
    KSSDFA.input = 0;

    // In Phase 6, the Qt dialog will drive the state machine via QTimer.
    // For now, run a blocking loop similar to the original but without
    // Win32 message pump - the host app must integrate via processStateMachineStep().

    // Note: In the final Qt integration, this function will NOT block.
    // Instead, the Qt dialog will call processStateMachineStep() from a QTimer.
    // This blocking version is provided as a fallback for Phase 1 testing.

    while (KSSDFA.state != 3) {
        processStateMachineStep();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool processStateMachineStep() {
    if (KSSDFA.state == 3)
        return false;

    int prev_state = KSSDFA.state;
    KSSDFA.state = KSSDFAST[((KSSDFA.state) << 2) | KSSDFA.input];
    KSSDFA.input = 0;

    if (prev_state == 2 && KSSDFA.state != 2) {
        close_recording();
    }

    if (KSSDFA.state == 2) {
        // Game running - nothing to do here, emulation thread handles MPV
    }
    else if (KSSDFA.state == 1) {
        // Game callback pending
        KSSDFA.state = 2;
        if (infos.gameCallback)
            infos.gameCallback(GAME, playerno, numplayers);
    }
    else if (KSSDFA.state == 0) {
        close_recording();
        // Poll network
        try {
            if (active_mod.SSDSTEP) {
                active_mod.SSDSTEP();
            }
        } catch (...) {
            // Swallow exceptions from network polling
        }
    }

    return (KSSDFA.state != 3);
}

int modifyPlayValues(void *values, int size) {
    if (KSSDFA.state == 2) {
        short siz = active_mod.MPV(values, size);

        if (recording_file.is_open()) {
            RecordingBuffer.put_char(0x12);
            RecordingBuffer.put_short(siz);
            RecordingBuffer.put_bytes((char*)values, siz);
            RecordingBuffer.write();
        }
        return siz;
    }
    return -1;
}

void recordingWriteInputs(const void* values, int size) {
    if (!recording_file.is_open() || values == nullptr || size <= 0) {
        return;
    }
    const short siz = (size > 0x7FFF) ? 0x7FFF : (short)size;
    RecordingBuffer.put_char(0x12);
    RecordingBuffer.put_short(siz);
    RecordingBuffer.put_bytes((char*)values, siz);
    RecordingBuffer.write();
}

void chatSend(char *text) {
    if (active_mod.ChatSend)
        active_mod.ChatSend(text);
}

void endGame() {
    close_recording();
    if (active_mod.EndGame)
        active_mod.EndGame();
}

int getFrameDelay() {
    if (active_mod_index == 1) {
        // Kaillera server mode
        return kaillera_get_delay();
    }
    // P2P mode doesn't expose delay this way
    return 0;
}

unsigned short getUserId() {
    if (active_mod_index == 1) {
        return kaillera_get_user_id();
    }
    return 0;
}

int getActiveMode() {
    return active_mod_index;
}

bool activateMode(int mode) {
    return activate_mode(mode);
}

void setRecordsDirectory(const std::string& recordsDirectory) {
    if (recordsDirectory.empty()) {
        recording_directory_path = std::filesystem::path("records");
    } else {
        recording_directory_path = pathFromUtf8String(recordsDirectory);
    }
}

bool isGameRunning() {
    return KSSDFA.state == 2;
}

int getState() {
    return KSSDFA.state;
}

void setStateInput(int input) {
    KSSDFA.input = input;
}

void resetStateMachine() {
    close_recording();
    KSSDFA.state = 0;
    KSSDFA.input = 0;
}

void setUICallbacks(const UICallbacks& callbacks) {
    s_uiCallbacks = callbacks;
}

UICallbacks& getUICallbacks() {
    return s_uiCallbacks;
}

bool playbackLoad(const char* filename) {
    return player_play(filename);
}

bool isPlaybackActive() {
    return player_playing;
}

bool playbackSeekToFrame(int frameIdx) {
    return player_seekToFrame(frameIdx);
}

int playbackGetCurrentFrame() {
    return PlayBackBuffer.currentFrameIdx;
}

int playbackGetTotalFrames() {
    return (int)PlayBackBuffer.frameIndex.size();
}

} // namespace n02
