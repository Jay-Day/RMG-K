#include "KrecParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

namespace KailleraExport
{

static std::string readCStringField(const uint8_t* source, size_t length)
{
    size_t end = 0;
    while (end < length && source[end] != 0)
    {
        ++end;
    }

    return std::string(reinterpret_cast<const char*>(source), end);
}

// Fallback for the rare case a .krec's own header.timestamp is missing/zero
// (a real recording always has one - see Source/n02/n02_client.cpp - so this
// only matters for a hand-edited or corrupted file). Parses the leading
// "YYMMDDHHMMSS" (12 digits, 2-digit year, local time) that n02_client.cpp
// derives every .krec's filename from - the same convention
// KailleraPlaybackDialog.cpp's recordings list already parses for display.
// Returns 0 if the filename doesn't start with 12 digits.
static uint32_t TryParseTimestampFromFilename(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    if (name.size() < 12)
    {
        return 0;
    }
    for (int i = 0; i < 12; i++)
    {
        if (!std::isdigit(static_cast<unsigned char>(name[static_cast<size_t>(i)])))
        {
            return 0;
        }
    }

    auto twoDigits = [&name](int offset) -> int {
        return (name[static_cast<size_t>(offset)] - '0') * 10 + (name[static_cast<size_t>(offset) + 1] - '0');
    };

    tm parsed{};
    const int year = twoDigits(0); // 2-digit year, e.g. "26" - krec predates any Y2.1K concern
    parsed.tm_year = year + 100;   // tm_year is years since 1900; futureproof-ish for "20xx"
    parsed.tm_mon = twoDigits(2) - 1;
    parsed.tm_mday = twoDigits(4);
    parsed.tm_hour = twoDigits(6);
    parsed.tm_min = twoDigits(8);
    parsed.tm_sec = twoDigits(10);
    parsed.tm_isdst = -1; // let mktime figure out DST for that local date

    const time_t result = mktime(&parsed);
    return result > 0 ? static_cast<uint32_t>(result) : 0;
}

static bool fail(std::string* errorMessage, const std::string& message)
{
    if (errorMessage != nullptr)
    {
        *errorMessage = message;
    }
    return false;
}

static constexpr char kMetaBegin[] = "RMGKMETA";
static constexpr char kMetaEnd[]    = "RMGKEND1";
static constexpr size_t kMarkerSize = 8;
static constexpr size_t kDropRecordSize = 6; // 0x14, empty nick, int32 player

bool FindKrecMetadata(const std::string& krec, size_t& trailerStart, std::string* json)
{
    const size_t footer = 4 + kMarkerSize;
    if (krec.size() < 400 + kDropRecordSize + kMarkerSize + footer ||
        std::memcmp(krec.data() + krec.size() - kMarkerSize, kMetaEnd, kMarkerSize) != 0)
    {
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, krec.data() + krec.size() - footer, sizeof(length));
    if (length > krec.size() - 400 - kDropRecordSize - kMarkerSize - footer)
    {
        return false;
    }
    const size_t jsonStart = krec.size() - footer - length;
    trailerStart = jsonStart - kMarkerSize - kDropRecordSize;
    if (static_cast<uint8_t>(krec[trailerStart]) != 0x14 || krec[trailerStart + 1] != 0 ||
        std::memcmp(krec.data() + jsonStart - kMarkerSize, kMetaBegin, kMarkerSize) != 0)
    {
        return false;
    }
    if (json != nullptr)
    {
        json->assign(krec, jsonStart, length);
    }
    return true;
}

static bool readWholeFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool ReadKrecMetadata(const std::filesystem::path& path, std::string& json)
{
    std::string krec;
    size_t trailerStart = 0;
    return readWholeFile(path, krec) && FindKrecMetadata(krec, trailerStart, &json);
}

bool WriteKrecMetadata(const std::filesystem::path& path, const std::string& json, std::string* errorMessage)
{
    std::string krec;
    if (!readWholeFile(path, krec))
    {
        return fail(errorMessage, "Cannot read the recording");
    }
    if (krec.size() < 400 || krec.compare(0, 4, "KRC1") != 0)
    {
        return fail(errorMessage, "Only KRC1 recordings can hold bookmarks");
    }

    // Only the trailer changes: cut the old one off and append the new one,
    // so an interrupted write can't damage the recorded inputs.
    size_t trailerStart = krec.size();
    if (FindKrecMetadata(krec, trailerStart, nullptr))
    {
        std::error_code errorCode;
        std::filesystem::resize_file(path, trailerStart, errorCode);
        if (errorCode)
        {
            return fail(errorMessage, "Cannot write the recording");
        }
    }

    std::string trailer;
    trailer.push_back(static_cast<char>(0x14));
    trailer.push_back('\0');
    trailer.append(krec, 264, 4); // the recording's local player
    trailer.append(kMetaBegin, kMarkerSize);
    trailer.append(json);
    const uint32_t length = static_cast<uint32_t>(json.size());
    trailer.append(reinterpret_cast<const char*>(&length), sizeof(length));
    trailer.append(kMetaEnd, kMarkerSize);

    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file.is_open() || !file.write(trailer.data(), static_cast<std::streamsize>(trailer.size())))
    {
        return fail(errorMessage, "Cannot write the recording");
    }
    return true;
}

bool ParseKrecFile(const std::filesystem::path& path, KrecData& outData, std::string* errorMessage)
{
    // opened by its wide path: player names in file names can be in any script
    std::string contents;
    if (!readWholeFile(path, contents))
    {
        return fail(errorMessage, "Cannot open recording file");
    }
    if (contents.size() < 272)
    {
        return fail(errorMessage, "Recording file is too short");
    }

    const std::vector<uint8_t> buffer(contents.begin(), contents.end());

    const std::string magic = readCStringField(buffer.data(), 4);
    const bool isKrc1 = (magic == "KRC1");
    const bool isKrc0 = (magic == "KRC0");
    if (!isKrc0 && !isKrc1)
    {
        return fail(errorMessage, "Recording file is not a supported KRC0/KRC1 file");
    }

    const size_t headerSize = isKrc1 ? 400 : 272;
    if (buffer.size() < headerSize)
    {
        return fail(errorMessage, "Recording file header is incomplete");
    }

    outData = {};
    outData.header.magic = magic;
    outData.header.appName = readCStringField(buffer.data() + 4, 128);
    outData.header.gameName = readCStringField(buffer.data() + 132, 128);
    memcpy(&outData.header.timestamp, buffer.data() + 260, sizeof(uint32_t));
    memcpy(&outData.header.playerNumber, buffer.data() + 264, sizeof(int32_t));
    memcpy(&outData.header.numPlayers, buffer.data() + 268, sizeof(int32_t));
    if (outData.header.timestamp == 0)
    {
        outData.header.timestamp = TryParseTimestampFromFilename(path);
    }

    if (isKrc1)
    {
        for (int i = 0; i < 4; ++i)
        {
            outData.header.playerNames[static_cast<size_t>(i)] =
                readCStringField(buffer.data() + 272 + i * 32, 32);
        }
    }

    int numPlayers = outData.header.numPlayers;
    if (numPlayers < 1)
    {
        numPlayers = 1;
    }
    if (numPlayers > 4)
    {
        numPlayers = 4;
    }

    const int bytesPerFrame = numPlayers * 4;
    bool inDelayPeriod = true;

    const uint8_t* scan = buffer.data() + headerSize;
    const uint8_t* end = buffer.data() + buffer.size();

    while (scan + 1 < end)
    {
        const uint8_t type = *scan++;

        if (type == 0x12)
        {
            if (scan + 2 > end)
            {
                break;
            }

            uint16_t recordLength = 0;
            memcpy(&recordLength, scan, sizeof(uint16_t));
            scan += 2;

            if (recordLength > 0)
            {
                if (scan + recordLength > end)
                {
                    break;
                }

                inDelayPeriod = false;
                outData.inputData.insert(outData.inputData.end(), scan, scan + recordLength);
                scan += recordLength;
            }
            else
            {
                if (inDelayPeriod)
                {
                    outData.delayFrames++;
                }
                outData.inputData.insert(outData.inputData.end(), bytesPerFrame, 0);
            }

            outData.totalInputFrames++;
        }
        else if (type == 0x14)
        {
            while (scan < end && *scan != 0)
            {
                ++scan;
            }
            if (scan < end)
            {
                ++scan;
            }
            scan += 4;
        }
        else if (type == 0x08)
        {
            KrecChatLine line;
            line.record = outData.totalInputFrames;
            line.nick = readCStringField(scan, static_cast<size_t>(end - scan));
            scan += std::min(line.nick.size() + 1, static_cast<size_t>(end - scan));
            line.message = readCStringField(scan, static_cast<size_t>(end - scan));
            scan += std::min(line.message.size() + 1, static_cast<size_t>(end - scan));
            outData.chat.push_back(std::move(line));
        }
        else if (type == 0x16)
        {
            uint32_t length = 0;
            if (scan + 4 > end)
            {
                break;
            }
            memcpy(&length, scan, sizeof(uint32_t));
            scan += 4;
            if (static_cast<size_t>(end - scan) < length)
            {
                break;
            }
            outData.saveData.assign(reinterpret_cast<const char*>(scan), length);
            scan += length;
        }
        else
        {
            break;
        }
    }

    if (outData.totalInputFrames <= 0)
    {
        return fail(errorMessage, "Recording file does not contain any input frames");
    }

    return true;
}

} // namespace KailleraExport
