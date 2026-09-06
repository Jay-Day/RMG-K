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
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <vector>
#include <utility>

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

// Optional sink that receives the exact same bytes written to the .krec
// (header + every flushed record). Used by the broadcast path to stream a live
// match to the lobby server. Set/cleared on the UI thread only while emulation
// is stopped; invoked on the emulation thread during play. Null when not
// broadcasting (the common case), so it costs a single null-check per flush.
static std::function<void(const void*, int)> recording_stream_sink;

// Count of input (0x12) records written so far for the current recording — i.e.
// the broadcaster's live frame in the same units a spectator's playback counts.
// Bumped on the emulation thread per record, read on the UI thread by the
// broadcast drain to stamp BROADCAST_DATA so spectators know the live edge.
static std::atomic<int> recording_frame_count{0};

// Lobby room chat arrives on the UI thread, but RecordingBuffer is only safe to
// touch on the emulation thread. Queue chat here and drain it into the krec (as
// 0x08 records) on the emulation thread just before the next input frame, so the
// buffer always has a single writer. recording_active gates enqueues so the
// queue can't grow without bound while nothing is being recorded.
static std::atomic<bool> recording_active{false};
static std::mutex recording_chat_mutex;
static std::vector<std::pair<std::string, std::string>> recording_chat_queue;

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
            int n = len();
            recording_file.write(buffer, n);
            recording_file.flush();
            if (n > 0 && recording_stream_sink)
                recording_stream_sink(buffer, n);
        }
        reset();
    }
} RecordingBuffer;

static void close_recording() {
    recording_active.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(recording_chat_mutex);
        recording_chat_queue.clear();
    }
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

// Drain queued chat into the open krec as 0x08 records (nick\0 msg\0). Called on
// the emulation thread before an input frame, so RecordingBuffer keeps a single
// writer. Each record is flushed on its own; lines are length-bounded at enqueue
// time so a record always fits RecordingBuffer's 500-byte buffer.
static void drain_recording_chat() {
    std::vector<std::pair<std::string, std::string>> pending;
    {
        std::lock_guard<std::mutex> lk(recording_chat_mutex);
        if (recording_chat_queue.empty())
            return;
        pending.swap(recording_chat_queue);
    }
    for (auto& c : pending) {
        RecordingBuffer.put_char(8);
        RecordingBuffer.put_bytes(const_cast<char*>(c.first.c_str()),  (int)c.first.size()  + 1);
        RecordingBuffer.put_bytes(const_cast<char*>(c.second.c_str()), (int)c.second.size() + 1);
        RecordingBuffer.write();
    }
}

// Open a fresh .krec recording and write its KRC1 header. The caller fills the
// recording_player_names global first (used for both the filename and header).
// Shared by the n02 game-start path (gameCallbackWrapper) and the rollback
// lobby path (n02::recordingOpen) so every transport writes an identical layout.
static void openRecordingFile(const char* appName, const char* game, int player, int numplayers_arg) {
        RecordingBuffer.reset();
        recording_frame_count.store(0, std::memory_order_relaxed);

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
            // Build the 400-byte KRC1 header in one zero-filled buffer. Bounded
            // strncpy avoids reading past appName/game (which may be shorter than
            // their 128-byte fields), and writing it in one shot lets the live
            // broadcast tee capture the header too.
            char header[400];
            memset(header, 0, sizeof(header));
            memcpy(header + 0, "KRC1", 4);
            if (appName) strncpy(header + 4, appName, 127);   // [4,132)  appName
            if (game)    strncpy(header + 132, game, 127);    // [132,260) gameName
            int32_t mytime = (int32_t)time(NULL);
            memcpy(header + 260, &mytime, 4);                 // [260,264) timestamp
            memcpy(header + 264, &player, 4);                 // [264,268) local player
            memcpy(header + 268, &numplayers_arg, 4);         // [268,272) num players
            memcpy(header + 272, recording_player_names, 128);// [272,400) player names

            recording_file.write(header, sizeof(header));
            recording_file.flush();
            if (recording_stream_sink)
                recording_stream_sink(header, (int)sizeof(header));
        }

        // Fresh recording: drop any chat queued before this session and arm chat
        // capture only if the file actually opened.
        {
            std::lock_guard<std::mutex> lk(recording_chat_mutex);
            recording_chat_queue.clear();
        }
        recording_active.store(recording_file.is_open(), std::memory_order_release);
}

static int gameCallbackWrapper(char *game, int player, int numplayers_arg) {
    close_recording();

    if (active_mod.RecordingEnabled && active_mod.RecordingEnabled()) {
        n02_TRACE();
        openRecordingFile(infos_copy.appName, game, player, numplayers_arg);
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

// Playback buffer. Holds the krec bytes in a growable vector and tracks the
// read / scan cursors as OFFSETS (not raw pointers) so the buffer can grow
// under a live spectate stream without invalidating any cursor. File playback
// fills `data` once; streaming appends to it via playbackAppendBytes().
class PlayBackBufferC {
public:
    std::vector<char> data;          // krec bytes (grows while streaming)
    size_t pos = 0;                  // read cursor
    size_t headerSize = 0;
    size_t scanPos = 0;              // incremental frame-index scan cursor
    bool   streaming = false;        // live: wait at the live edge, don't end
    bool   headerParsed = false;
    bool   streamBufferPrimed = false;
    bool   streamBuffering = false;

    std::vector<size_t> frameIndex;  // offsets of 0x12 (input) records
    int currentFrameIdx = 0;

    void reset_all() {
        data.clear();
        pos = 0; headerSize = 0; scanPos = 0;
        streaming = false; headerParsed = false;
        streamBufferPrimed = false; streamBuffering = false;
        frameIndex.clear(); currentFrameIdx = 0;
    }

    size_t size() const { return data.size(); }

    void load_bytes(void* dest, unsigned int len) {
        if (pos >= data.size()) return;
        size_t avail = data.size() - pos;
        size_t n = (len < avail) ? len : avail;
        memcpy(dest, data.data() + pos, n);
        pos += n;
    }
    void load_str(char* dest, unsigned int maxlen) {
        if (maxlen == 0) return;
        if (pos >= data.size()) { dest[0] = 0; return; }
        size_t avail = data.size() - pos;
        size_t slen = strnlen(data.data() + pos, avail) + 1; // include NUL
        if (slen > maxlen) slen = maxlen;
        if (slen > avail) slen = avail;
        load_bytes(dest, (unsigned int)slen);
        dest[(slen > 0) ? (slen - 1) : 0] = 0;
    }
    int load_int() { int x = 0; load_bytes(&x, 4); return x; }
    unsigned char load_char() { unsigned char x = 0; load_bytes(&x, 1); return x; }
    unsigned short load_short() { unsigned short x = 0; load_bytes(&x, 2); return x; }
} PlayBackBuffer;

// Guards all PlayBackBuffer access. During a live spectate stream the buffer is
// fed by playbackAppendBytes() on the UI thread while player_MPV() consumes it
// on the emulation thread; without this lock a vector realloc on one thread races
// the other's read (use-after-free → corrupted inputs → desync).
static std::recursive_mutex playbackMutex;
static std::condition_variable_any playbackCondition;

static int streamBufferedFrames(const PlayBackBufferC& pb) {
    const int buffered = static_cast<int>(pb.frameIndex.size()) - pb.currentFrameIdx;
    return buffered > 0 ? buffered : 0;
}

// Streaming: is a complete next record present at pb.pos? Guards against
// consuming a record that has only partially arrived at the live edge.
static bool streamRecordComplete(PlayBackBufferC& pb) {
    const size_t n = pb.size();
    size_t p = pb.pos;
    if (p >= n) return false;
    unsigned char t = (unsigned char)pb.data[p];
    if (t == 0x12) {
        if (p + 3 > n) return false;
        unsigned short rlen;
        memcpy(&rlen, pb.data.data() + p + 1, 2);
        return p + 3 + (size_t)rlen <= n;
    } else if (t == 0x14) {
        size_t z = p + 1;
        while (z < n && pb.data[z] != 0) z++;
        if (z >= n) return false;     // nick NUL not yet present
        z++;
        return z + 4 <= n;
    } else if (t == 0x08) {
        size_t z = p + 1;
        while (z < n && pb.data[z] != 0) z++;
        if (z >= n) return false;
        z++;
        size_t z2 = z;
        while (z2 < n && pb.data[z2] != 0) z2++;
        return z2 < n;                // both strings terminated
    }
    return false;
}

// Parse the KRC1/KRC0 header (game name, player number/count). Returns false
// until enough bytes are present.
static bool parsePlaybackHeader() {
    PlayBackBufferC& pb = PlayBackBuffer;
    if (pb.size() < 272) return false;
    bool isKRC1 = (memcmp(pb.data.data(), "KRC1", 4) == 0);
    size_t hs = isKRC1 ? 400 : 272;
    if (pb.size() < hs) return false;

    pb.pos = 132;
    pb.load_str(GAME, 128);
    pb.pos = 264;
    playerno = pb.load_int();
    numplayers = pb.load_int();

    // Player names live at [272,400) on KRC1. Copy them into the shared global so
    // a spectator can label the OSD ports from the stream, just like a live match
    // does (KRC0 has no names — leave them blank).
    memset(recording_player_names, 0, sizeof(recording_player_names));
    if (isKRC1)
    {
        memcpy(recording_player_names, pb.data.data() + 272, sizeof(recording_player_names));
        for (int i = 0; i < 4; i++)
            recording_player_names[i][31] = '\0';
    }

    pb.headerSize = hs;
    pb.pos = hs;
    pb.scanPos = hs;
    pb.headerParsed = true;
    return true;
}

// Extend frameIndex with any newly-complete records from scanPos forward. Stops
// at the first partial record, so it's safe to call repeatedly as a live stream
// grows.
static void scanFrames() {
    PlayBackBufferC& pb = PlayBackBuffer;
    const size_t n = pb.size();
    const char* d = pb.data.data();
    size_t scan = pb.scanPos;
    while (scan < n) {
        unsigned char t = (unsigned char)d[scan];
        if (t == 0x12) {
            if (scan + 3 > n) break;
            unsigned short rlen;
            memcpy(&rlen, d + scan + 1, 2);
            if (scan + 3 + (size_t)rlen > n) break;
            pb.frameIndex.push_back(scan);
            scan += 3 + (size_t)rlen;
        } else if (t == 0x14) {
            size_t z = scan + 1;
            while (z < n && d[z] != 0) z++;
            if (z >= n) break;
            z++;
            if (z + 4 > n) break;
            scan = z + 4;
        } else if (t == 0x08) {
            size_t z = scan + 1;
            while (z < n && d[z] != 0) z++;
            if (z >= n) break;
            z++;
            size_t z2 = z;
            while (z2 < n && d[z2] != 0) z2++;
            if (z2 >= n) break;
            scan = z2 + 1;
        } else {
            break;
        }
    }
    pb.scanPos = scan;
}

static void player_EndGame();

static int player_MPV(void* values, int size) {
    std::unique_lock<std::recursive_mutex> lock(playbackMutex);
    if (!player_playing)
        return -1;

    PlayBackBufferC& pb = PlayBackBuffer;

    // Process metadata records in a loop rather than recursively. Besides being
    // simpler, this ensures a condition-variable wait owns the recursive mutex
    // only once, so appending bytes can always wake it.
    for (;;) {
        // A streamed replay must never advance without a fresh input record. Prime
        // two seconds before the first frame, and rebuild the full cushion after any
        // starvation event. The condition wait releases playbackMutex so incoming
        // network bytes and a user-requested stop can wake the emulation thread.
        if (pb.streaming && (!pb.streamBufferPrimed || streamBufferedFrames(pb) == 0)) {
            pb.streamBufferPrimed = false;
            pb.streamBuffering = true;
            playbackCondition.wait(lock, [&pb]() {
                return !player_playing || !pb.streaming ||
                       streamBufferedFrames(pb) >= n02::LIVE_REPLAY_BUFFER_FRAMES;
            });
            if (!player_playing || !pb.streaming)
                return -1;
            pb.streamBufferPrimed = true;
            pb.streamBuffering = false;
        }

        // File playback uses the historical "more than 10 bytes left" guard; a live
        // stream instead checks whether the full next record has actually arrived.
        bool canRead = pb.streaming ? streamRecordComplete(pb)
                                    : (pb.pos + 10 < pb.size());
        if (!canRead) {
            // A complete indexed input implies that every record before it is also
            // complete. Reaching this branch in streaming mode therefore means the
            // stream ended or was reset while the waiter was waking.
            if (pb.streaming)
                return -1;
            player_EndGame();
            return -1;
        }

        unsigned char b = pb.load_char();
        if (b == 0x12) {
            int l = pb.load_short();
            if (l < 0) {
                player_EndGame();
                return -1;
            }
            if (l > 0)
                pb.load_bytes((char*)values, l);
            pb.currentFrameIdx++;
            return l;
        }
        if (b == 20) {
            char playernick[100];
            pb.load_str(playernick, 100);
            int pn = pb.load_int();
            if (pn >= 1 && pn <= 16)
                player_was_dropped[pn - 1] = true;
            if (pn == playerno) {
                player_EndGame();
                return -1;
            }
            continue;
        }
        if (b == 8) {
            char nick[100];
            char msg[500];
            pb.load_str(nick, 100);
            pb.load_str(msg, 500);
            if (infos.chatReceivedCallback)
                infos.chatReceivedCallback(nick, msg);
            continue;
        }
        return -1;
    }
}

static void player_EndGame() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
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
    PlayBackBuffer.reset_all();
    playbackCondition.notify_all();
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
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
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

    PlayBackBufferC& pb = PlayBackBuffer;
    pb.reset_all();
    pb.data.resize((size_t)len);
    file.read(pb.data.data(), len);
    file.close();

    if (!parsePlaybackHeader()) {
        pb.reset_all();
        return false;
    }
    scanFrames(); // whole file present → indexes every frame

    pb.streaming = false;
    player_playing = true;
    memset(player_was_dropped, 0, sizeof(player_was_dropped));

    KSSDFA.input = KSSDFA_START_GAME;

    return true;
}

static bool player_seekToFrame(int frameIdx) {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    if (!player_playing) return false;
    if (frameIdx < 0 || frameIdx >= (int)PlayBackBuffer.frameIndex.size()) return false;
    PlayBackBuffer.pos = PlayBackBuffer.frameIndex[frameIdx];
    PlayBackBuffer.currentFrameIdx = frameIdx;
    return true;
}

// Live spectate: begin a stream fed incrementally by player_appendStream().
static bool player_beginStream() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    PlayBackBufferC& pb = PlayBackBuffer;
    pb.reset_all();
    pb.streaming = true;
    player_playing = false; // becomes true once the header has been received
    memset(player_was_dropped, 0, sizeof(player_was_dropped));
    return true;
}

static void player_appendStream(const void* data, int len) {
    if (data == nullptr || len <= 0)
        return;
    {
        std::lock_guard<std::recursive_mutex> lock(playbackMutex);
        PlayBackBufferC& pb = PlayBackBuffer;
        if (!pb.streaming)
            return;
        const char* p = (const char*)data;
        pb.data.insert(pb.data.end(), p, p + len);
        if (!pb.headerParsed) {
            if (parsePlaybackHeader()) {
                // Header complete — start the game (state machine fires gameCallback).
                player_playing = true;
                KSSDFA.input = KSSDFA_START_GAME;
            }
        }
        if (pb.headerParsed)
            scanFrames();
    }
    playbackCondition.notify_all();
}

static void player_stopStream() {
    {
        std::lock_guard<std::recursive_mutex> lock(playbackMutex);
        if (PlayBackBuffer.streaming && player_playing) {
            player_EndGame();
        } else {
            PlayBackBuffer.reset_all();
            player_playing = false;
        }
    }
    playbackCondition.notify_all();
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
            recording_frame_count.fetch_add(1, std::memory_order_relaxed);
        }
        return siz;
    }
    return -1;
}

void recordingQueueChat(const char* nick, const char* msg) {
    if (!recording_active.load(std::memory_order_acquire))
        return;
    std::string n(nick ? nick : "");
    std::string m(msg ? msg : "");
    // Bound so a 0x08 record always fits RecordingBuffer (500 bytes) and the
    // playback reader (nick[100] / msg[500]).
    if (n.size() > 31)  n.resize(31);
    if (m.size() > 255) m.resize(255);
    std::lock_guard<std::mutex> lk(recording_chat_mutex);
    if (recording_chat_queue.size() < 256) // backstop against runaway growth
        recording_chat_queue.emplace_back(std::move(n), std::move(m));
}

void recordingWriteInputs(const void* values, int size) {
    if (!recording_file.is_open() || values == nullptr || size <= 0) {
        return;
    }
    drain_recording_chat(); // flush any queued chat just ahead of this frame
    const short siz = (size > 0x7FFF) ? 0x7FFF : (short)size;
    RecordingBuffer.put_char(0x12);
    RecordingBuffer.put_short(siz);
    RecordingBuffer.put_bytes((char*)values, siz);
    RecordingBuffer.write();
    recording_frame_count.fetch_add(1, std::memory_order_relaxed);
}

int recordingFrameCount() {
    return recording_frame_count.load(std::memory_order_relaxed);
}

void recordingOpen(const char* appName, const char* gameName, int localPlayer, int numPlayers) {
    // Close any recording left open from a previous match, then start a fresh
    // one only when recording is enabled. Mirrors gameCallbackWrapper's gate for
    // the GekkoNet rollback path, which syncs input via GekkoNet and so never
    // reaches that callback. The caller fills recording_player_names first.
    close_recording();
    if (!n02_kaillera_recording_enabled) {
        return;
    }
    openRecordingFile(appName, gameName, localPlayer, numPlayers);
}

void recordingClose() {
    close_recording();
}

void setRecordingStreamSink(std::function<void(const void*, int)> sink) {
    recording_stream_sink = std::move(sink);
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

bool playbackBeginStream() {
    return player_beginStream();
}

void playbackAppendBytes(const void* data, int len) {
    player_appendStream(data, len);
}

void playbackStopStream() {
    player_stopStream();
}

bool isPlaybackActive() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return player_playing;
}

bool playbackSeekToFrame(int frameIdx) {
    return player_seekToFrame(frameIdx);
}

int playbackGetCurrentFrame() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return PlayBackBuffer.currentFrameIdx;
}

int playbackGetTotalFrames() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return (int)PlayBackBuffer.frameIndex.size();
}

bool playbackIsBuffering() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return PlayBackBuffer.streaming && PlayBackBuffer.streamBuffering;
}

int playbackGetBufferedFrames() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return streamBufferedFrames(PlayBackBuffer);
}

bool playbackHeaderReady() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return PlayBackBuffer.headerParsed;
}

int playbackNumPlayers() {
    std::lock_guard<std::recursive_mutex> lock(playbackMutex);
    return numplayers;
}

} // namespace n02
