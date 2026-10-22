#include "KrecParser.hpp"

#include <cstdio>
#include <cstring>
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

bool AppendKrecMetadata(const std::filesystem::path& path, const std::string& json, std::string* errorMessage)
{
    std::string krec;
    if (!readWholeFile(path, krec) || krec.size() < 400 || krec.compare(0, 4, "KRC1") != 0)
    {
        return fail(errorMessage, "Cannot read the recording");
    }
    size_t trailerStart = 0;
    if (FindKrecMetadata(krec, trailerStart, nullptr))
    {
        return fail(errorMessage, "The recording already has metadata");
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
