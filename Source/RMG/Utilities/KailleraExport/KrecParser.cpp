#include "KrecParser.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

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

bool ParseKrecFile(const std::filesystem::path& path, KrecData& outData, std::string* errorMessage)
{
    FILE* file = fopen(path.string().c_str(), "rb");
    if (file == nullptr)
    {
        return fail(errorMessage, "Cannot open recording file");
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return fail(errorMessage, "Failed to seek recording file");
    }

    const long fileLength = ftell(file);
    if (fileLength < 272)
    {
        fclose(file);
        return fail(errorMessage, "Recording file is too short");
    }

    rewind(file);

    std::vector<uint8_t> buffer(static_cast<size_t>(fileLength));
    if (fread(buffer.data(), 1, buffer.size(), file) != buffer.size())
    {
        fclose(file);
        return fail(errorMessage, "Failed to read recording file");
    }

    fclose(file);

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
            while (scan < end && *scan != 0)
            {
                ++scan;
            }
            if (scan < end)
            {
                ++scan;
            }
            while (scan < end && *scan != 0)
            {
                ++scan;
            }
            if (scan < end)
            {
                ++scan;
            }
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
