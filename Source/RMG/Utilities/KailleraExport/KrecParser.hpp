#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace KailleraExport
{

struct KrecHeader
{
    std::string magic;
    std::string appName;
    std::string gameName;
    uint32_t timestamp = 0;
    int32_t playerNumber = 0;
    int32_t numPlayers = 0;
    std::array<std::string, 4> playerNames;
};

struct KrecData
{
    KrecHeader header;
    std::vector<uint8_t> inputData;
    int totalInputFrames = 0;
    int delayFrames = 0;
};

bool ParseKrecFile(const std::filesystem::path& path, KrecData& outData, std::string* errorMessage);

// Ranked replay metadata (JSON, see CoreRankedGetLastReplayJson) appended to a
// finished .krec:
//
//   0x14 "" int32 localPlayer   drop record of the recording's own player,
//                               which ends playback in every krec player
//   "RMGKMETA" json uint32 jsonLength "RMGKEND1"
//
// Readers stop at the drop record, so the file stays a valid .krec.

// finds the metadata in a whole .krec; trailerStart = where the drop record
// begins (the end of the input records). False when there is none.
bool FindKrecMetadata(const std::string& krec, size_t& trailerStart, std::string* json);

// reads a .krec file's metadata, false when it has none
bool ReadKrecMetadata(const std::filesystem::path& path, std::string& json);

// appends metadata to a .krec file that has none yet
bool AppendKrecMetadata(const std::filesystem::path& path, const std::string& json, std::string* errorMessage);

} // namespace KailleraExport
