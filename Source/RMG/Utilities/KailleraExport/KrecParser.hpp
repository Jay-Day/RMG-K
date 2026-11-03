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
    // The save the match started from (its 0x16 record), empty when blank.
    std::string saveData;
};

bool ParseKrecFile(const std::filesystem::path& path, KrecData& outData, std::string* errorMessage);

} // namespace KailleraExport
