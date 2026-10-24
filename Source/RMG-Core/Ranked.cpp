/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "Ranked.hpp"
#include "RankedJournal.hpp"
#include "Ranked/RankedModuleApi.h"
#include "RomSettings.hpp"
#include "Directories.hpp"
#include "Callback.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"
#include "m64p/api/m64p_debugger.h"

#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <sstream>

static_assert(std::endian::native == std::endian::little,
    "Ranked RDRAM access assumes a little-endian host");

//
// Local Structs
//

struct l_RankedModule
{
    CoreLibraryHandle Handle = nullptr;
    CoreRankedModuleInfo Info;

    ptr_RankedModule_GetInfo               GetInfo               = nullptr;
    ptr_RankedModule_Create                Create                = nullptr;
    ptr_RankedModule_GetRulesetDescription GetRulesetDescription = nullptr;
    ptr_RankedModule_OnFrame               OnFrame               = nullptr;
    ptr_RankedModule_OnConfirmedFrame      OnConfirmedFrame      = nullptr;
    ptr_RankedModule_GetStatusText         GetStatusText         = nullptr;
    ptr_RankedModule_Destroy               Destroy               = nullptr;
};

using l_JournalWrite = CoreRankedJournalWrite;
using l_JournalFrame = CoreRankedJournalFrame;

struct l_ObservationSlot
{
    int64_t Frame = -1;
    std::array<uint8_t, RANKED_MAX_OBSERVATION_SIZE> Data{};
    std::vector<l_JournalWrite> Writes;
};

//
// Local Variables
//

// frames of speculative observations kept; must exceed the rollback horizon
static constexpr int64_t kObservationRingSize = 512;
static constexpr uint32_t kRdramSize = 0x800000;

// frames without writes still get an entry this often, so a replay keeps
// checking that it's aligned with the recording
static constexpr int64_t kJournalCheckpointFrames = 600;
static constexpr size_t  kRecordHistorySize = 512;

static std::mutex                l_StageMutex;
static bool                      l_HasStagedSession = false;
static CoreRankedSessionSettings l_StagedSession;

// guards l_Module/l_Instance against UI thread status queries,
// frame hooks run on the emulation thread which also owns begin/end
static std::mutex             l_InstanceMutex;
static l_RankedModule         l_Module;
static RankedModuleInstance*  l_Instance = nullptr;
static std::atomic<bool>      l_Active{false};
static std::atomic<uint64_t>  l_RulesetHash{0};
static std::atomic<bool>      l_Replay{false};

static bool                      l_RollbackDriven = false;
static CoreRankedSessionSettings l_Session;
static std::string               l_RulesetId;
static std::string               l_RomMd5;
static uint8_t*                  l_Rdram = nullptr;

static std::vector<l_ObservationSlot> l_ObservationRing;
static int64_t l_NextConfirmFrame = -1;
static int64_t l_MaxObservedFrame = -1;
static bool    l_InConfirmedFrame = false;
static bool    l_SetEnded = false;
static std::string l_IntegrityIssue;

static std::vector<RankedEvent> l_Games;
static int l_VoidedGames = 0;

static std::mutex                   l_EventMutex;
static std::vector<CoreRankedEvent> l_Events;
static std::string                  l_LastResultJson;

// .krec bookmarks. Game events are confirmed at a rollback frame, but the
// frame's input record is written later (once past the rollback horizon), so
// a mark waits until the first record at or after its frame is written.
struct l_Bookmark
{
    int64_t     Frame  = 0;
    int         Record = -1;
    std::string Label;
    std::string Kind;
    int         Game = 0;
};
// The write journal. Every module write that changes memory during OnFrame
// is kept with its frame's observation and committed once the frame is
// confirmed. Replays apply it instead of running the module, so a replay
// doesn't depend on the module version.
//
// Journal frames and bookmarks carry the number of .krec records written
// before their frame. Records are written past the rollback horizon (and the
// last ones only when the rollback session closes, after emulation ended), so
// this state outlives the session and is guarded by l_JournalMutex.
static std::mutex                  l_JournalMutex;
static bool                        l_JournalLive = false;
static std::vector<l_JournalFrame> l_Journal;
static size_t                      l_JournalResolved = 0;
static int64_t                     l_LastJournalFrame = -1;
static std::vector<l_Bookmark>     l_Bookmarks;
static std::deque<std::pair<int64_t, int>> l_RecordHistory; // (frame, record)
static int64_t                     l_HistoryPrevFrame = -1; // frame of the record before the history
static size_t                      l_JournalStreamed = 0; // frames handed to a live broadcast
static int                         l_RecordsWritten = 0;
static std::string                 l_ReplayHeaderJson; // set when the session ends
static bool                        l_JournalOutsideFrameWarned = false;
// writes of the frame the module is running, nullptr outside OnFrame
static std::vector<l_JournalWrite>* l_FrameWrites = nullptr;
// words the module changed this frame -> index in *l_FrameWrites
static std::unordered_map<uint32_t, size_t> l_FrameWords;
static bool                        l_InModuleCall = false;

// replay: the journal being applied. A spectator's journal arrives in chunks
// from the UI thread while it plays, so it's guarded by l_ReplayMutex.
static std::mutex                  l_ReplayMutex;
static CoreRankedJournalPlayer     l_ReplayPlayer;
static bool                        l_StagedSpectator = false; // guarded by l_StageMutex

// Emulation.cpp: a spectator keyframe is staged but not loaded yet
bool CoreSpectateKeyframeAwaitingLoad(void);

//
// Internal Functions
//

static void ranked_log(CoreDebugMessageType type, const std::string& message)
{
    CoreDebugCallback(const_cast<char*>("[RANKED] "), static_cast<int>(type), message.c_str());
}

static std::filesystem::path get_ranked_directory(void)
{
    std::filesystem::path pluginDirectory = CoreGetPluginDirectory();
    std::filesystem::path parent = pluginDirectory.parent_path();
    return (parent.empty() ? std::filesystem::path("Ranked") : parent / "Ranked").make_preferred();
}

static uint64_t fnv1a64(const std::string& data)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : data)
    {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

static void close_module(l_RankedModule& module)
{
    if (module.Handle != nullptr)
    {
        CoreCloseLibrary(module.Handle);
    }
    module = l_RankedModule();
}

static bool open_module(const std::filesystem::path& path, l_RankedModule& module, std::string& error)
{
    module = l_RankedModule();
    module.Handle = CoreOpenLibrary(path);
    if (module.Handle == nullptr)
    {
        error = "failed to open '" + path.string() + "': " + CoreGetLibraryError();
        return false;
    }

    module.GetInfo               = (ptr_RankedModule_GetInfo)CoreGetLibrarySymbol(module.Handle, "RankedModule_GetInfo");
    module.Create                = (ptr_RankedModule_Create)CoreGetLibrarySymbol(module.Handle, "RankedModule_Create");
    module.GetRulesetDescription = (ptr_RankedModule_GetRulesetDescription)CoreGetLibrarySymbol(module.Handle, "RankedModule_GetRulesetDescription");
    module.OnFrame               = (ptr_RankedModule_OnFrame)CoreGetLibrarySymbol(module.Handle, "RankedModule_OnFrame");
    module.OnConfirmedFrame      = (ptr_RankedModule_OnConfirmedFrame)CoreGetLibrarySymbol(module.Handle, "RankedModule_OnConfirmedFrame");
    module.GetStatusText         = (ptr_RankedModule_GetStatusText)CoreGetLibrarySymbol(module.Handle, "RankedModule_GetStatusText");
    module.Destroy               = (ptr_RankedModule_Destroy)CoreGetLibrarySymbol(module.Handle, "RankedModule_Destroy");

    if (module.GetInfo == nullptr || module.Create == nullptr ||
        module.GetRulesetDescription == nullptr || module.OnFrame == nullptr ||
        module.OnConfirmedFrame == nullptr || module.GetStatusText == nullptr ||
        module.Destroy == nullptr)
    {
        error = "'" + path.string() + "' is not a ranked module (missing exports)";
        close_module(module);
        return false;
    }

    RankedModuleInfo info{};
    info.size = sizeof(info);
    if (module.GetInfo(&info) == 0 || info.apiVersion != RANKED_MODULE_API_VERSION ||
        info.id == nullptr || info.name == nullptr || info.version == nullptr)
    {
        error = "'" + path.string() + "' has an incompatible ranked module API version";
        close_module(module);
        return false;
    }

    module.Info.Id      = info.id;
    module.Info.Name    = info.name;
    module.Info.Version = info.version;
    module.Info.Path    = path;
    return true;
}

static uint32_t to_rdram_offset(uint32_t address, uint32_t length, bool& valid)
{
    uint32_t physical = address & 0x1FFFFFFF;
    valid = l_Rdram != nullptr && physical < kRdramSize && (kRdramSize - physical) >= length;
    return physical;
}

//
// Host API
//

static uint8_t host_read8(void*, uint32_t address)
{
    bool valid;
    uint32_t offset = to_rdram_offset(address, 1, valid);
    return valid ? l_Rdram[offset ^ 3] : 0;
}

static uint16_t host_read16(void*, uint32_t address)
{
    bool valid;
    uint32_t offset = to_rdram_offset(address & ~1u, 2, valid);
    if (!valid)
    {
        return 0;
    }
    uint16_t value;
    std::memcpy(&value, l_Rdram + (offset ^ 2), sizeof(value));
    return value;
}

static uint32_t host_read32(void*, uint32_t address)
{
    bool valid;
    uint32_t offset = to_rdram_offset(address & ~3u, 4, valid);
    if (!valid)
    {
        return 0;
    }
    uint32_t value;
    std::memcpy(&value, l_Rdram + offset, sizeof(value));
    return value;
}

static void invalidate_code(unsigned int address, unsigned int size)
{
    using ptr_RmgkInvalidateCachedCode = void (*)(unsigned int, unsigned int);
    static ptr_RmgkInvalidateCachedCode invalidate = nullptr;
    static bool resolved = false;
    if (!resolved)
    {
        invalidate = (ptr_RmgkInvalidateCachedCode)CoreGetLibrarySymbol(
            (CoreLibraryHandle)m64p::Core.GetHandle(), "RmgkInvalidateCachedCode");
        resolved = true;
        if (invalidate == nullptr)
        {
            ranked_log(CoreDebugMessageType::Warning,
                "core lacks RmgkInvalidateCachedCode; code patches may not apply");
        }
    }
    if (invalidate != nullptr)
    {
        invalidate(address, size);
    }
}

// writes to RDRAM, returns whether memory changed
static bool write_memory(uint8_t kind, uint32_t address, uint32_t value, uint32_t* before)
{
    return CoreRankedWriteMemory(l_Rdram, kind, address, value, invalidate_code, before);
}

static void module_write(uint8_t kind, uint32_t address, uint32_t value)
{
    // replays apply the recorded journal instead
    if (l_Replay.load())
    {
        return;
    }
    // The journal keeps each frame's net effect per 32-bit word: a module may
    // set a value and then override it in the same frame (a profile, then the
    // ruleset), which must not cost a journal write every frame.
    const uint32_t word = 0x80000000u | ((address & 0x1FFFFFFF) & ~3u);
    uint32_t wordBefore = 0;
    const bool haveWord = CoreRankedReadMemory(l_Rdram, kCoreRankedJournalWrite32, word, wordBefore);
    if (!write_memory(kind, address, value, nullptr))
    {
        return;
    }
    if (l_FrameWrites != nullptr && haveWord)
    {
        auto [it, inserted] = l_FrameWords.try_emplace(word, l_FrameWrites->size());
        if (inserted)
        {
            l_FrameWrites->push_back({kCoreRankedJournalWrite32, word, 0, wordBefore});
        }
        if (kind == kCoreRankedJournalWriteCode32)
        {
            (*l_FrameWrites)[it->second].Kind = kCoreRankedJournalWriteCode32;
        }
    }
    else if (l_InModuleCall && !l_JournalOutsideFrameWarned)
    {
        l_JournalOutsideFrameWarned = true;
        ranked_log(CoreDebugMessageType::Warning,
            "module wrote memory outside OnFrame; replays of this session may desync");
    }
}

static void host_write8(void*, uint32_t address, uint8_t value)
{
    module_write(kCoreRankedJournalWrite8, address, value);
}

static void host_write16(void*, uint32_t address, uint16_t value)
{
    module_write(kCoreRankedJournalWrite16, address, value);
}

static void host_write32(void*, uint32_t address, uint32_t value)
{
    module_write(kCoreRankedJournalWrite32, address, value);
}

static void host_write_code32(void*, uint32_t address, uint32_t value)
{
    module_write(kCoreRankedJournalWriteCode32, address, value);
}

static void host_log(void*, RankedLogLevel level, const char* message)
{
    CoreDebugMessageType type = CoreDebugMessageType::Info;
    switch (level)
    {
    case RankedLog_Error:   type = CoreDebugMessageType::Error;   break;
    case RankedLog_Warning: type = CoreDebugMessageType::Warning; break;
    case RankedLog_Verbose: type = CoreDebugMessageType::Verbose; break;
    default: break;
    }
    ranked_log(type, "[" + l_Module.Info.Id + "] " + (message ? message : ""));
}

static std::string build_result_json(const RankedEvent& setEvent)
{
    std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx",
        static_cast<unsigned long long>(l_RulesetHash.load()));

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema\": 1,\n";
    json << "  \"module\": {\"id\": \"" << json_escape(l_Module.Info.Id)
         << "\", \"version\": \"" << json_escape(l_Module.Info.Version) << "\"},\n";
    json << "  \"ruleset\": {\"id\": \"" << json_escape(l_RulesetId)
         << "\", \"hash\": \"" << hash << "\"},\n";
    json << "  \"romMd5\": \"" << json_escape(l_RomMd5) << "\",\n";
    json << "  \"bestOf\": " << l_Session.BestOf << ",\n";

    json << "  \"players\": [";
    for (size_t i = 0; i < l_Session.Ports.size(); i++)
    {
        int port = l_Session.Ports[i];
        std::string name = (port >= 1 && static_cast<size_t>(port) <= l_Session.PlayerNames.size()) ?
            l_Session.PlayerNames[static_cast<size_t>(port - 1)] : std::string();
        json << (i ? ", " : "") << "{\"port\": " << port << ", \"name\": \"" << json_escape(name) << "\"}";
    }
    json << "],\n";

    json << "  \"games\": [";
    for (size_t i = 0; i < l_Games.size(); i++)
    {
        const RankedEvent& game = l_Games[i];
        json << (i ? "," : "") << "\n    {\"game\": " << game.gameNumber
             << ", \"winnerPort\": " << game.winnerPort
             << ", \"winnerTeam\": " << game.winnerTeam
             << ", \"frame\": " << game.frame
             << ", \"stage\": " << game.stage
             << ", \"players\": [";
        bool first = true;
        for (int port : l_Session.Ports)
        {
            if (port < 1 || port > RANKED_MAX_PLAYERS)
            {
                continue;
            }
            const RankedPlayerStats& stats = game.players[port - 1];
            json << (first ? "" : ", ") << "{\"port\": " << port
                 << ", \"character\": " << stats.character
                 << ", \"stocksLeft\": " << stats.stocksLeft
                 << ", \"extra\": " << stats.extra << "}";
            first = false;
        }
        json << "]}";
    }
    json << (l_Games.empty() ? "" : "\n  ") << "],\n";

    json << "  \"voidedGames\": " << l_VoidedGames << ",\n";
    json << "  \"winnerPort\": " << setEvent.winnerPort << ",\n";
    // team sets: each port's team (1 or 2, team 1 holds the lowest port)
    json << "  \"winnerTeam\": " << setEvent.winnerTeam << ",\n";
    json << "  \"teams\": [";
    for (int i = 0; i < RANKED_MAX_PLAYERS; i++)
    {
        json << (i ? ", " : "") << setEvent.teams[i];
    }
    json << "],\n";
    json << "  \"score\": [";
    for (int i = 0; i < RANKED_MAX_PLAYERS; i++)
    {
        json << (i ? ", " : "") << setEvent.score[i];
    }
    json << "],\n";
    json << "  \"integrity\": \"" << (l_IntegrityIssue.empty() ? "ok" : json_escape(l_IntegrityIssue)) << "\",\n";
    json << "  \"finishedAtUtc\": \"" << timestamp << "\"\n";
    json << "}\n";
    return json.str();
}

static void write_result_file(const std::string& json)
{
    std::error_code errorCode;
    std::filesystem::path directory = CoreGetUserDataDirectory();
    directory /= "Ranked";
    directory /= "Results";
    std::filesystem::create_directories(directory, errorCode);

    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::path path = directory / (l_Module.Info.Id + "-" + std::to_string(millis) + ".json");

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        ranked_log(CoreDebugMessageType::Warning, "failed to write result file '" + path.string() + "'");
        return;
    }
    file << json;
    ranked_log(CoreDebugMessageType::Info, "set result written to '" + path.string() + "'");
}

static int current_record_count(void)
{
#ifdef RMGK_HAVE_P2P_TRANSPORT
    return n02::recordingFrameCount();
#else
    return 0;
#endif
}

static int current_playback_record(void)
{
#ifdef RMGK_HAVE_P2P_TRANSPORT
    return n02::playbackGetCurrentFrame();
#else
    return 0;
#endif
}

// position of `frame` in the recording (see l_JournalFrame), false while
// no record at or after it is written yet. Needs l_JournalMutex.
static bool record_position(int64_t frame, int& record, uint32_t& offset)
{
    if (!l_RollbackDriven)
    { // without rollback the frame's input is recorded as it runs
        record = current_record_count();
        offset = 0;
        return true;
    }
    int64_t previousFrame = l_HistoryPrevFrame;
    for (const auto& [recordFrame, index] : l_RecordHistory)
    {
        if (recordFrame >= frame)
        {
            record = index;
            offset = static_cast<uint32_t>(frame - previousFrame - 1);
            return true;
        }
        previousFrame = recordFrame;
    }
    return false;
}

static void add_bookmark(int64_t frame, const std::string& label, const std::string& kind, int game)
{
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    l_Bookmark mark;
    mark.Frame  = frame;
    mark.Label  = label;
    mark.Kind   = kind;
    mark.Game   = game;
    uint32_t offset;
    if (!record_position(frame, mark.Record, offset))
    {
        mark.Record = -1;
    }
    l_Bookmarks.push_back(std::move(mark));
}

// commits a confirmed frame's writes (emulation thread)
static void commit_journal_frame(int64_t frame, const std::vector<l_JournalWrite>& writes)
{
    if (writes.empty() && l_LastJournalFrame >= 0 &&
        frame - l_LastJournalFrame < kJournalCheckpointFrames)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    l_JournalFrame entry;
    entry.Frame  = frame;
    if (!record_position(frame, entry.Record, entry.Offset))
    {
        entry.Record = -1;
    }
    entry.Writes = writes;
    l_Journal.push_back(std::move(entry));
    while (l_JournalResolved < l_Journal.size() && l_Journal[l_JournalResolved].Record >= 0)
    {
        l_JournalResolved++;
    }
    l_LastJournalFrame = frame;
}

static std::string base64_encode(const std::string& data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        uint32_t chunk = static_cast<uint8_t>(data[i]) << 16;
        if (i + 1 < data.size()) chunk |= static_cast<uint8_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) chunk |= static_cast<uint8_t>(data[i + 2]);
        out.push_back(table[(chunk >> 18) & 63]);
        out.push_back(table[(chunk >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? table[chunk & 63] : '=');
    }
    return out;
}

// the session part of the replay metadata, captured when the session ends
static std::string build_replay_header_json(void)
{
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx",
        static_cast<unsigned long long>(l_RulesetHash.load()));

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema\": 1,\n";
    json << "  \"module\": {\"id\": \"" << json_escape(l_Module.Info.Id)
         << "\", \"version\": \"" << json_escape(l_Module.Info.Version) << "\"},\n";
    json << "  \"ruleset\": {\"id\": \"" << json_escape(l_RulesetId)
         << "\", \"hash\": \"" << hash << "\"},\n";
    json << "  \"romMd5\": \"" << json_escape(l_RomMd5) << "\",\n";
    json << "  \"bestOf\": " << l_Session.BestOf << ",\n";
    json << "  \"ports\": [";
    for (size_t i = 0; i < l_Session.Ports.size(); i++)
    {
        json << (i ? ", " : "") << l_Session.Ports[i];
    }
    json << "],\n";
    // the teams the lobby required, so the replay judges games the same way
    if (std::any_of(l_Session.Teams.begin(), l_Session.Teams.end(), [](int team) { return team != 0; }))
    {
        json << "  \"teams\": [";
        for (size_t i = 0; i < l_Session.Teams.size(); i++)
        {
            json << (i ? ", " : "") << l_Session.Teams[i];
        }
        json << "],\n";
    }
    json << "  \"games\": [";
    for (size_t i = 0; i < l_Games.size(); i++)
    {
        json << (i ? ", " : "") << "{\"game\": " << l_Games[i].gameNumber
             << ", \"winnerPort\": " << l_Games[i].winnerPort << "}";
    }
    json << "],\n";
    json << "  \"integrity\": \"" << (l_IntegrityIssue.empty() ? "ok" : json_escape(l_IntegrityIssue)) << "\",\n";
    return json.str();
}

// completes the replay metadata with the bookmarks and the journal. Needs
// l_JournalMutex; records not written yet count as the end of the recording.
static std::string build_replay_json(void)
{
    std::ostringstream json;
    json << l_ReplayHeaderJson;
    json << "  \"bookmarks\": [";
    bool first = true;
    for (const l_Bookmark& mark : l_Bookmarks)
    {
        json << (first ? "" : ",") << "\n    {\"record\": " << (mark.Record < 0 ? l_RecordsWritten : mark.Record)
             << ", \"label\": \"" << json_escape(mark.Label)
             << "\", \"kind\": \"" << json_escape(mark.Kind)
             << "\", \"game\": " << mark.Game << "}";
        first = false;
    }
    json << (first ? "" : "\n  ") << "],\n";
    json << "  \"journal\": {\"format\": 3, \"frames\": " << l_Journal.size()
         << ", \"data\": \"" << base64_encode(CoreRankedEncodeJournal(l_Journal, 0, l_Journal.size(), l_RecordsWritten)) << "\"}\n";
    json << "}\n";
    return json.str();
}

static void host_emit_event(void*, const RankedEvent* moduleEvent)
{
    // modules that predate the team fields send a shorter event
    if (moduleEvent == nullptr || moduleEvent->size < RANKED_EVENT_SIZE_V1)
    {
        return;
    }
    RankedEvent copy{};
    std::memcpy(&copy, moduleEvent, std::min<size_t>(moduleEvent->size, sizeof(RankedEvent)));
    const RankedEvent* event = &copy;
    if (!l_InConfirmedFrame)
    {
        ranked_log(CoreDebugMessageType::Error,
            "module emitted an event outside OnConfirmedFrame, ignoring it");
        return;
    }
    if (l_SetEnded)
    {
        return;
    }

    CoreRankedEvent coreEvent;
    coreEvent.Type       = static_cast<CoreRankedEventType>(event->type);
    coreEvent.Frame      = event->frame;
    coreEvent.GameNumber = event->gameNumber;
    coreEvent.WinnerPort = event->winnerPort;
    coreEvent.Score.assign(event->score, event->score + RANKED_MAX_PLAYERS);
    coreEvent.Detail     = std::string(event->detail, strnlen(event->detail, sizeof(event->detail)));
    if (event->winnerTeam != 0 || event->teams[0] + event->teams[1] + event->teams[2] + event->teams[3] != 0)
    {
        coreEvent.Teams.assign(event->teams, event->teams + RANKED_MAX_PLAYERS);
        coreEvent.WinnerTeam = event->winnerTeam;
    }

    std::ostringstream message;
    message << "frame " << event->frame << " game " << event->gameNumber;
    switch (event->type)
    {
    case RankedEvent_GameStarted:
        message << " started";
        add_bookmark(event->frame, "Game " + std::to_string(event->gameNumber), "game_start", event->gameNumber);
        break;
    case RankedEvent_GameEnded:
        if (event->winnerTeam != 0)
        {
            message << " won by team " << event->winnerTeam;
        }
        else
        {
            message << " won by port " << event->winnerPort;
        }
        l_Games.push_back(*event);
        add_bookmark(event->frame, "Game " + std::to_string(event->gameNumber) + " end", "game_end", event->gameNumber);
        break;
    case RankedEvent_GameVoided:
        message << " voided";
        l_VoidedGames++;
        add_bookmark(event->frame, "Game " + std::to_string(event->gameNumber) + " (voided)", "game_end", event->gameNumber);
        break;
    case RankedEvent_SetEnded:
        if (event->winnerTeam != 0)
        {
            message << " set won by team " << event->winnerTeam;
        }
        else
        {
            message << " set won by port " << event->winnerPort;
        }
        break;
    default:
        ranked_log(CoreDebugMessageType::Warning, "module emitted an unknown event type");
        return;
    }
    if (!coreEvent.Detail.empty())
    {
        message << " (" << coreEvent.Detail << ")";
    }
    ranked_log(CoreDebugMessageType::Info, message.str());

    if (l_Replay.load() && event->type == RankedEvent_GameEnded)
    {
        const size_t index = static_cast<size_t>(std::max(0, event->gameNumber - 1));
        if (index < l_Session.ExpectedWinners.size() && l_Session.ExpectedWinners[index] != event->winnerPort)
        {
            CoreRankedEvent mismatch;
            mismatch.Type       = CoreRankedEventType::ReplayMismatch;
            mismatch.Frame      = event->frame;
            mismatch.GameNumber = event->gameNumber;
            mismatch.WinnerPort = event->winnerPort;
            mismatch.Detail     = "game " + std::to_string(event->gameNumber) + " was won by P" +
                std::to_string(l_Session.ExpectedWinners[index]) + " in the recording";
            ranked_log(CoreDebugMessageType::Warning, "replay desynced: " + mismatch.Detail);
            std::lock_guard<std::mutex> lock(l_EventMutex);
            l_Events.push_back(std::move(mismatch));
        }
    }

    if (event->type == RankedEvent_SetEnded && l_Replay.load())
    {
        l_SetEnded = true;
    }
    else if (event->type == RankedEvent_SetEnded)
    {
        l_SetEnded = true;
        std::string json = build_result_json(*event);
        write_result_file(json);
        std::lock_guard<std::mutex> lock(l_EventMutex);
        l_LastResultJson = json;
    }

    std::lock_guard<std::mutex> lock(l_EventMutex);
    l_Events.push_back(std::move(coreEvent));
}

static RankedHostApi l_HostApi =
{
    sizeof(RankedHostApi),
    RANKED_MODULE_API_VERSION,
    nullptr,
    host_read8,
    host_read16,
    host_read32,
    host_write8,
    host_write16,
    host_write32,
    host_log,
    host_emit_event,
    host_write_code32,
};

static void flag_integrity_issue(const std::string& issue)
{
    if (l_IntegrityIssue.empty())
    {
        l_IntegrityIssue = issue;
        ranked_log(CoreDebugMessageType::Error, "integrity issue: " + issue +
            " - this set's result will be marked unverified");
    }
}

static void reset_session_state(void)
{
    l_ObservationRing.assign(static_cast<size_t>(kObservationRingSize), l_ObservationSlot());
    l_NextConfirmFrame = -1;
    l_MaxObservedFrame = -1;
    l_InConfirmedFrame = false;
    l_SetEnded = false;
    l_IntegrityIssue.clear();
    l_Games.clear();
    l_VoidedGames = 0;
    l_Rdram = nullptr;
}

// rewinds the replay to the start of its journal (the journal itself stays:
// a spectator's may have arrived before emulation started)
static void reset_replay_cursor(void)
{
    std::lock_guard<std::mutex> lock(l_ReplayMutex);
    CoreRankedJournalReset(l_ReplayPlayer);
}

static void reset_journal(bool live)
{
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    l_JournalLive = live;
    l_Journal.clear();
    l_JournalResolved = 0;
    l_LastJournalFrame = -1;
    l_Bookmarks.clear();
    l_RecordHistory.clear();
    l_HistoryPrevFrame = -1;
    l_JournalStreamed = 0;
    l_RecordsWritten = 0;
    l_ReplayHeaderJson.clear();
    l_JournalOutsideFrameWarned = false;
}

static void confirm_frame(int64_t frame)
{
    const l_ObservationSlot& slot = l_ObservationRing[static_cast<size_t>(frame % kObservationRingSize)];
    if (slot.Frame != frame)
    {
        flag_integrity_issue("frame " + std::to_string(frame) + " was never observed");
        return;
    }

    if (l_Instance == nullptr)
    {
        return;
    }
    l_InConfirmedFrame = true;
    l_InModuleCall = true;
    l_Module.OnConfirmedFrame(l_Instance, static_cast<uint32_t>(frame), slot.Data.data());
    l_InModuleCall = false;
    l_InConfirmedFrame = false;
}

static bool ensure_rdram(void)
{
    if (l_Rdram != nullptr)
    {
        return true;
    }
    static ptr_DebugMemGetPointer getPointer = nullptr;
    if (getPointer == nullptr)
    {
        getPointer = (ptr_DebugMemGetPointer)CoreGetLibrarySymbol(
            (CoreLibraryHandle)m64p::Core.GetHandle(), "DebugMemGetPointer");
    }
    if (getPointer != nullptr)
    {
        l_Rdram = static_cast<uint8_t*>(getPointer(M64P_DBG_PTR_RDRAM));
    }
    return l_Rdram != nullptr;
}

static void push_mismatch(uint32_t frame, const std::string& detail)
{
    CoreRankedEvent mismatch;
    mismatch.Type   = CoreRankedEventType::ReplayMismatch;
    mismatch.Frame  = frame;
    mismatch.Detail = detail;
    ranked_log(CoreDebugMessageType::Warning, "replay: " + detail);
    std::lock_guard<std::mutex> lock(l_EventMutex);
    l_Events.push_back(std::move(mismatch));
}

//
// Exported Functions
//

CORE_EXPORT std::vector<CoreRankedModuleInfo> CoreGetRankedModules(void)
{
    std::vector<CoreRankedModuleInfo> modules;
    std::error_code errorCode;
    std::filesystem::path directory = get_ranked_directory();

    if (!std::filesystem::is_directory(directory, errorCode))
    {
        return modules;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, errorCode))
    {
        if (!entry.is_regular_file() || entry.path().extension() != CORE_LIBRARY_EXT_STR)
        {
            continue;
        }

        l_RankedModule module;
        std::string error;
        if (!open_module(entry.path(), module, error))
        {
            ranked_log(CoreDebugMessageType::Warning, error);
            continue;
        }
        modules.push_back(module.Info);
        close_module(module);
    }

    return modules;
}

CORE_EXPORT bool CoreRankedStageSession(const CoreRankedSessionSettings& settings)
{
    if (settings.ModuleId.empty())
    {
        CoreSetError("CoreRankedStageSession: no module id given");
        return false;
    }
    if (settings.BestOf < 1 || (settings.BestOf % 2) == 0)
    {
        CoreSetError("CoreRankedStageSession: best-of must be a positive odd number");
        return false;
    }
    if (settings.Ports.size() < 2 || settings.Ports.size() > RANKED_MAX_PLAYERS)
    {
        CoreSetError("CoreRankedStageSession: a ranked set needs 2 to 4 players");
        return false;
    }
    for (int port : settings.Ports)
    {
        if (port < 1 || port > RANKED_MAX_PLAYERS ||
            std::count(settings.Ports.begin(), settings.Ports.end(), port) != 1)
        {
            CoreSetError("CoreRankedStageSession: invalid or duplicate controller port");
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(l_StageMutex);
    l_StagedSession = settings;
    l_HasStagedSession = true;
    return true;
}

CORE_EXPORT void CoreRankedClearStagedSession(void)
{
    std::lock_guard<std::mutex> lock(l_StageMutex);
    l_StagedSpectator = false;
    l_HasStagedSession = false;
    l_StagedSession = CoreRankedSessionSettings();
}

CORE_EXPORT bool CoreRankedIsActive(void)
{
    return l_Active.load();
}

CORE_EXPORT bool CoreRankedIsReplay(void)
{
    return l_Replay.load();
}

CORE_EXPORT std::vector<CoreRankedEvent> CoreRankedTakeEvents(void)
{
    std::lock_guard<std::mutex> lock(l_EventMutex);
    std::vector<CoreRankedEvent> events;
    events.swap(l_Events);
    return events;
}

CORE_EXPORT std::string CoreRankedGetStatusText(void)
{
    std::lock_guard<std::mutex> lock(l_InstanceMutex);
    if (l_Instance == nullptr)
    {
        return std::string();
    }

    char buffer[256] = {0};
    l_Module.GetStatusText(l_Instance, buffer, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

CORE_EXPORT uint64_t CoreRankedGetRulesetHash(void)
{
    return l_RulesetHash.load();
}

CORE_EXPORT void CoreRankedStageSpectatorJournal(void)
{
    {
        std::lock_guard<std::mutex> lock(l_ReplayMutex);
        l_ReplayPlayer = CoreRankedJournalPlayer();
    }
    std::lock_guard<std::mutex> lock(l_StageMutex);
    l_StagedSpectator = true;
}

CORE_EXPORT bool CoreRankedAppendReplayJournal(const std::string& data)
{
    std::lock_guard<std::mutex> lock(l_ReplayMutex);
    if (!CoreRankedJournalAppend(l_ReplayPlayer, data))
    {
        ranked_log(CoreDebugMessageType::Warning, "dropped a malformed write journal chunk");
        return false;
    }
    return true;
}

CORE_EXPORT bool CoreRankedTakeJournalUpdate(std::string& data, size_t maxBytes)
{
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    size_t end = l_JournalStreamed;
    size_t bytes = 0;
    while (end < l_JournalResolved)
    {
        const size_t frameBytes = 14 + l_Journal[end].Writes.size() * 13;
        if (end > l_JournalStreamed && bytes + frameBytes > maxBytes)
        {
            break;
        }
        bytes += frameBytes;
        end++;
    }
    if (end == l_JournalStreamed)
    {
        return false;
    }
    data = CoreRankedEncodeJournal(l_Journal, l_JournalStreamed, end, l_RecordsWritten);
    l_JournalStreamed = end;
    return true;
}

CORE_EXPORT std::string CoreRankedGetLastReplayJson(void)
{
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    if (l_ReplayHeaderJson.empty())
    {
        return {};
    }
    return build_replay_json();
}

CORE_EXPORT std::string CoreRankedGetLastResultJson(void)
{
    std::lock_guard<std::mutex> lock(l_EventMutex);
    return l_LastResultJson;
}

//
// Internal Functions (emulation thread)
//

bool CoreRankedBeginEmulation(bool netplay, bool rollbackDriven)
{
    {
        // the previous session's records are all written by now
        std::lock_guard<std::mutex> lock(l_JournalMutex);
        l_JournalLive = false;
    }

    CoreRankedSessionSettings settings;
    {
        std::lock_guard<std::mutex> lock(l_StageMutex);
        if (l_StagedSpectator)
        { // live spectator: applies the broadcast's journal as it arrives
            l_StagedSpectator = false;
            l_HasStagedSession = false;
            reset_journal(false);
            reset_session_state();
            reset_replay_cursor();
            l_Session        = CoreRankedSessionSettings();
            l_RollbackDriven = false;
            l_Replay         = true;
            l_Active         = true;
            return true;
        }
        if (l_HasStagedSession)
        {
            settings = l_StagedSession;
            l_HasStagedSession = false;
        }
        else if (!netplay && !CoreSettingsGetStringValue(SettingsID::Ranked_DevModuleId).empty())
        { // offline development/testing session from settings
            settings.ModuleId  = CoreSettingsGetStringValue(SettingsID::Ranked_DevModuleId);
            settings.RulesetId = CoreSettingsGetStringValue(SettingsID::Ranked_DevRulesetId);
            settings.BestOf    = CoreSettingsGetIntValue(SettingsID::Ranked_DevBestOf);
            settings.Ports     = CoreSettingsGetIntListValue(SettingsID::Ranked_DevPorts);
        }
        else
        {
            return true;
        }
    }

    auto fail = [](const std::string& reason)
    {
        CoreSetError("Ranked: " + reason);
        ranked_log(CoreDebugMessageType::Error, reason);
        return false;
    };

    if (settings.BestOf < 1 || (settings.BestOf % 2) == 0)
    {
        return fail("best-of must be a positive odd number");
    }
    if (settings.Ports.size() < 2 || settings.Ports.size() > RANKED_MAX_PLAYERS)
    {
        return fail("a ranked set needs 2 to 4 players");
    }

    reset_journal(!settings.Replay);
    if (settings.Replay)
    {
        reset_session_state();
        reset_replay_cursor();
        std::lock_guard<std::mutex> lock(l_ReplayMutex);
        if (!CoreRankedDecodeJournal(settings.Journal, l_ReplayPlayer.Frames))
        {
            return fail("the replay's write journal is damaged");
        }
    }

    // find the module by id
    std::filesystem::path modulePath;
    for (const CoreRankedModuleInfo& info : CoreGetRankedModules())
    {
        if (info.Id == settings.ModuleId)
        {
            modulePath = info.Path;
            break;
        }
    }
    // a replay only loads the very version it was recorded with, and plays
    // back from the journal alone without it
    std::string installedVersion;
    for (const CoreRankedModuleInfo& info : CoreGetRankedModules())
    {
        if (info.Id == settings.ModuleId)
        {
            installedVersion = info.Version;
        }
    }
    if (settings.Replay && (modulePath.empty() || installedVersion != settings.ModuleVersion))
    {
        l_Session        = settings;
        l_RollbackDriven = false;
        l_Replay         = true;
        l_Active         = true;
        ranked_log(CoreDebugMessageType::Info, "replaying " + std::to_string(l_ReplayPlayer.Frames.size()) +
            " journal frames without the module (" + settings.ModuleId + " " + settings.ModuleVersion +
            (installedVersion.empty() ? " not installed)" : " differs from the installed " + installedVersion + ")"));
        return true;
    }
    if (modulePath.empty())
    {
        return fail("ranked module '" + settings.ModuleId + "' not found in '" +
            get_ranked_directory().string() + "'");
    }

    CoreRomSettings romSettings;
    if (!CoreGetCurrentRomSettings(romSettings))
    {
        return fail("failed to retrieve the ROM settings");
    }

    l_RankedModule module;
    std::string error;
    if (!open_module(modulePath, module, error))
    {
        return fail(error);
    }

    std::string dataDirectory = (modulePath.parent_path() / modulePath.stem()).make_preferred().string();

    RankedSessionConfig config{};
    config.size          = sizeof(config);
    config.rulesetId     = settings.RulesetId.c_str();
    config.bestOf        = settings.BestOf;
    config.romMd5        = romSettings.MD5.c_str();
    config.dataDirectory = dataDirectory.c_str();
    config.numPlayers    = static_cast<int32_t>(settings.Ports.size());
    for (size_t i = 0; i < settings.Ports.size(); i++)
    {
        config.ports[i] = settings.Ports[i];
    }
    for (size_t i = 0; i < settings.Teams.size() && i < RANKED_MAX_PLAYERS; i++)
    {
        config.teams[i] = settings.Teams[i];
    }

    // the host API is valid before the first frame, but memory
    // reads return 0 until emulation has started
    if (!settings.Replay)
    {
        reset_session_state();
    }
    l_Replay = settings.Replay; // writes during Create are dropped in replays
    l_Module = module;
    ranked_log(CoreDebugMessageType::Info, "loading " + module.Info.Name + " " +
        module.Info.Version + " from '" + modulePath.string() + "'");

    char createError[512] = {0};
    l_InModuleCall = true;
    RankedModuleInstance* instance = module.Create(&l_HostApi, &config, createError, sizeof(createError));
    l_InModuleCall = false;
    if (instance == nullptr)
    {
        createError[sizeof(createError) - 1] = '\0';
        close_module(l_Module);
        return fail(std::string("module refused the session: ") +
            (createError[0] ? createError : "unknown reason"));
    }

    // canonical ruleset description -> hash compared between peers
    uint32_t needed = module.GetRulesetDescription(instance, nullptr, 0);
    std::vector<char> buffer(needed + 1, '\0');
    module.GetRulesetDescription(instance, buffer.data(), static_cast<uint32_t>(buffer.size()));
    std::string description(buffer.data(), needed);
    l_RulesetHash = fnv1a64(module.Info.Id + "|" + module.Info.Version + "|" + description);

    {
        std::lock_guard<std::mutex> lock(l_InstanceMutex);
        l_Instance = instance;
    }
    {
        std::lock_guard<std::mutex> lock(l_EventMutex);
        l_Events.clear();
    }

    l_Session        = settings;
    l_RulesetId      = settings.RulesetId;
    l_RomMd5         = romSettings.MD5;
    l_RollbackDriven = rollbackDriven;
    l_Replay         = settings.Replay;
    l_Active         = true;

    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(l_RulesetHash.load()));
    if (settings.Replay && !settings.ExpectedRulesetHash.empty() && settings.ExpectedRulesetHash != hash)
    {
        push_mismatch(0, "recorded with a different ruleset");
    }
    ranked_log(CoreDebugMessageType::Info, "ranked session started: best of " +
        std::to_string(settings.BestOf) + ", ruleset hash " + hash + ", " +
        (rollbackDriven ? "rollback driven" : "frame callback driven"));
    return true;
}

void CoreRankedEndEmulation(void)
{
    if (!l_Active.load())
    {
        return;
    }

    if (!l_SetEnded)
    {
        ranked_log(CoreDebugMessageType::Info, l_Replay.load() ? "replay ended" :
            "emulation ended before the set was decided");
    }
    if (l_Replay.load())
    {
        std::lock_guard<std::mutex> lock(l_ReplayMutex);
        ranked_log(CoreDebugMessageType::Info, "replay applied " + std::to_string(l_ReplayPlayer.Applied) +
            " of " + std::to_string(l_ReplayPlayer.Frames.size()) + " journal frames" +
            (l_ReplayPlayer.MismatchReported ? " (diverged)" : ""));
    }

    if (!l_Replay.load())
    {
        // frames still inside the rollback window were simulated for the
        // last time: their records are written when the session closes
        for (int64_t frame = std::max<int64_t>(l_NextConfirmFrame, 0);
             l_NextConfirmFrame >= 0 && frame <= l_MaxObservedFrame; frame++)
        {
            const l_ObservationSlot& slot = l_ObservationRing[static_cast<size_t>(frame % kObservationRingSize)];
            if (slot.Frame == frame)
            {
                commit_journal_frame(frame, slot.Writes);
            }
        }
        std::string header = build_replay_header_json();
        std::lock_guard<std::mutex> lock(l_JournalMutex);
        l_ReplayHeaderJson = std::move(header);
    }

    l_Active = false;
    {
        std::lock_guard<std::mutex> lock(l_InstanceMutex);
        if (l_Instance != nullptr)
        {
            l_Module.Destroy(l_Instance);
        }
        l_Instance = nullptr;
    }
    if (l_Module.Handle != nullptr)
    {
        close_module(l_Module);
    }
    l_Module = l_RankedModule();
    reset_session_state();
    l_RulesetHash = 0;
}

void CoreRankedOnFrame(uint32_t frame)
{
    if (!l_Active.load())
    {
        return;
    }

    if (!ensure_rdram() || l_Instance == nullptr)
    {
        return;
    }

    const int64_t frame64 = static_cast<int64_t>(frame);
    l_ObservationSlot& slot = l_ObservationRing[static_cast<size_t>(frame64 % kObservationRingSize)];
    slot.Data.fill(0);
    slot.Writes.clear();

    // always run the module so enforcement writes stay identical to peers;
    // a resimulated frame replaces its earlier writes
    l_FrameWrites = &slot.Writes;
    l_FrameWords.clear();
    l_InModuleCall = true;
    l_Module.OnFrame(l_Instance, frame, slot.Data.data());
    l_InModuleCall = false;
    l_FrameWrites = nullptr;

    // keep the words that ended the frame changed, with their final value
    size_t kept = 0;
    for (l_JournalWrite& write : slot.Writes)
    {
        CoreRankedReadMemory(l_Rdram, kCoreRankedJournalWrite32, write.Address, write.Value);
        if (write.Value != write.Before)
        {
            slot.Writes[kept++] = write;
        }
    }
    slot.Writes.resize(kept);

    if (l_NextConfirmFrame >= 0 && frame64 < l_NextConfirmFrame)
    {
        flag_integrity_issue("confirmed frame " + std::to_string(frame64) + " was re-simulated");
        slot.Frame = -1;
        return;
    }

    slot.Frame = frame64;
    if (l_NextConfirmFrame < 0)
    {
        l_NextConfirmFrame = frame64;
    }
    l_MaxObservedFrame = std::max(l_MaxObservedFrame, frame64);

    // confirmation fell too far behind, the ring would overwrite
    // unconfirmed frames. Should never happen with a sane horizon.
    if (l_MaxObservedFrame - l_NextConfirmFrame >= kObservationRingSize - 1)
    {
        flag_integrity_issue("confirmation fell behind the observation buffer");
        CoreRankedConfirmThrough(l_MaxObservedFrame - (kObservationRingSize / 2));
    }
}

void CoreRankedConfirmThrough(int64_t frame)
{
    if (!l_Active.load() || l_NextConfirmFrame < 0)
    {
        return;
    }

    frame = std::min(frame, l_MaxObservedFrame);
    while (l_NextConfirmFrame <= frame)
    {
        if (!l_SetEnded)
        {
            confirm_frame(l_NextConfirmFrame);
        }
        if (!l_Replay.load())
        {
            const l_ObservationSlot& slot =
                l_ObservationRing[static_cast<size_t>(l_NextConfirmFrame % kObservationRingSize)];
            if (slot.Frame == l_NextConfirmFrame)
            {
                commit_journal_frame(l_NextConfirmFrame, slot.Writes);
            }
        }
        l_NextConfirmFrame++;
    }
}

void CoreRankedOnCoreFrame(uint32_t frame)
{
    if (!l_Active.load() || l_RollbackDriven)
    {
        return;
    }

    // replays apply the module's recorded writes at the same boundary the
    // module made them, before it runs (read-only) to follow the set. A
    // spectator waits for its keyframe: until then it's booting a state
    // the keyframe replaces.
    if (l_Replay.load() && ensure_rdram() && !CoreSpectateKeyframeAwaitingLoad())
    {
        std::lock_guard<std::mutex> lock(l_ReplayMutex);
        std::string mismatch;
        CoreRankedJournalAdvance(l_ReplayPlayer, current_playback_record(), l_Rdram, invalidate_code, &mismatch);
        if (!mismatch.empty())
        {
            push_mismatch(0, mismatch);
        }
    }

    // frame callback fires once `frame` finished, i.e. at the
    // boundary before frame + 1, without any rollback
    CoreRankedOnFrame(frame + 1);
    CoreRankedConfirmThrough(static_cast<int64_t>(frame) + 1);
}

void CoreRankedOnRecordWritten(int64_t frame, int recordIndex)
{
    std::lock_guard<std::mutex> lock(l_JournalMutex);
    if (!l_JournalLive)
    {
        return;
    }

    const int64_t previousFrame = l_RecordHistory.empty() ? l_HistoryPrevFrame : l_RecordHistory.back().first;
    l_RecordHistory.emplace_back(frame, recordIndex);
    if (l_RecordHistory.size() > kRecordHistorySize)
    {
        l_HistoryPrevFrame = l_RecordHistory.front().first;
        l_RecordHistory.pop_front();
    }
    l_RecordsWritten = recordIndex + 1;

    while (l_JournalResolved < l_Journal.size() && l_Journal[l_JournalResolved].Frame <= frame)
    {
        l_JournalFrame& entry = l_Journal[l_JournalResolved];
        if (entry.Record < 0)
        {
            entry.Record = recordIndex;
            entry.Offset = static_cast<uint32_t>(entry.Frame - previousFrame - 1);
        }
        l_JournalResolved++;
    }
    for (l_Bookmark& mark : l_Bookmarks)
    {
        if (mark.Record < 0 && mark.Frame <= frame)
        {
            mark.Record = recordIndex;
        }
    }
}

void CoreRankedReplayRebase(int record)
{
    std::lock_guard<std::mutex> lock(l_ReplayMutex);
    l_ReplayPlayer.Rebase = record;
}

void CoreRankedOnStateLoaded(void)
{
    if (!l_Active.load() || !l_Replay.load() || !ensure_rdram())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(l_ReplayMutex);
    if (l_ReplayPlayer.Rebase < 0)
    {
        return;
    }

    // the reader was rewound so its local record 0 is the keyframe's record
    const int record = l_ReplayPlayer.Rebase;
    std::string mismatch;
    CoreRankedJournalRebase(l_ReplayPlayer, record, l_Rdram, invalidate_code, &mismatch);
    if (!mismatch.empty())
    {
        push_mismatch(0, mismatch);
    }
    ranked_log(CoreDebugMessageType::Info, "replay journal realigned to the keyframe at input " +
        std::to_string(record));
}

void CoreRankedReportDesync(const std::string& detail)
{
    if (l_Active.load())
    {
        flag_integrity_issue("desync detected: " + detail);
    }
}
