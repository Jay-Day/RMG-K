#define CORE_INTERNAL
#include "NetplaySave.hpp"
#include "CachedRomHeaderAndSettings.hpp"
#include "Directories.hpp"
#include "Emulation.hpp"
#include "RomHeader.hpp"
#include "RomSettings.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Error.hpp"

#include "m64p/Api.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

//
// Local Variables
//

namespace
{
// Largest save of each kind, in CoreSaveDataKind order.
constexpr std::array<size_t, CoreSaveDataKindCount> kMaxKindBytes = { 0x800, 0x8000, 0x20000 };
constexpr const char* kKindExtensions[CoreSaveDataKindCount] = { ".eep", ".sra", ".fla" };

constexpr char kManifestMagic[8] = { 'R', 'M', 'G', 'K', 'S', 'A', 'V', '1' };
constexpr char kFileMagic[8]     = { 'R', 'M', 'G', 'K', 'S', 'A', 'V', 'E' };
constexpr uint32_t kFileVersion  = 1;

CoreNetplaySaveData l_StagedSaveData;

// Confirmed-save tracking (emulation thread). Each saved rollback state
// records the core's write count for its frame. A frame's count is final
// once GekkoNet confirms the frame, and the running saves still hold that
// frame's bytes while the count hasn't moved since.
constexpr int kTrackFrames = 512;
// Publish once the count has held for this many confirmed frames (about a
// second), so a save the game writes over several frames is copied whole.
constexpr int kSettleFrames = 60;
// A game that never stops writing still publishes every ~5 seconds.
constexpr int kOverdueFrames = 300;

struct TrackedFrame
{
    int frame = -1;
    uint32_t generation = 0;
};

std::array<TrackedFrame, kTrackFrames> l_Track;
uint32_t l_TrackPublishedGeneration = 0;
int l_TrackChangeFirstSeen = -1;

std::mutex l_PublishMutex;
CoreNetplaySaveData l_Published;
uint32_t l_PublishedGeneration = 0;
bool l_PublishedFresh = false;
}

//
// Local Functions
//

static void append_u32(std::string& out, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

static bool read_u32(const std::string& in, size_t& offset, uint32_t& value)
{
    if (offset + 4 > in.size())
    {
        return false;
    }

    value = 0;
    for (int i = 0; i < 4; i++)
    {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(in[offset + i])) << (i * 8);
    }
    offset += 4;
    return true;
}

static bool read_file(const std::filesystem::path& file, std::string& out)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return !stream.bad();
}

static int kind_for_extension(const std::filesystem::path& file)
{
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        if (extension == kKindExtensions[kind])
        {
            return kind;
        }
    }
    return -1;
}

// key=value lines; values can't hold a newline, so they're cut at one.
static std::string meta_line(const char* key, const std::string& value)
{
    std::string line = key;
    line += '=';
    line += value.substr(0, value.find_first_of("\r\n"));
    line += '\n';
    return line;
}

// The base name mupen64plus-core gives this ROM's save files (see
// get_save_filename in its main.c).
static std::string offline_save_basename(const CoreRomHeader& header, const CoreRomSettings& settings)
{
    std::string name;
    const std::string md5 = settings.MD5.substr(0, 8);

    if (CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat) == 0)
    {
        name = header.Name;
    }
    else if (settings.GoodName.find("(unknown rom)") == std::string::npos)
    {
        name = settings.GoodName.substr(0, 32) + "-" + md5;
    }
    else if (!header.Name.empty())
    {
        name = header.Name + "-" + md5;
    }
    else
    {
        name = "unknown-" + md5;
    }

    for (char& c : name)
    {
        if (std::strchr(":<>\"/\\|?*", c) != nullptr)
        {
            c = '_';
        }
    }
    return name;
}

static void publish_running_save_data(void)
{
    CoreNetplaySaveData data;
    uint32_t generation = 0;
    if (!CoreGetRunningSaveData(data, generation))
    {
        return;
    }

    // A kind the game never wrote is still formatted blank (all 0xFF), which
    // is also what a missing kind boots as: leave it out of the copy.
    for (std::vector<uint8_t>& kind : data.Kinds)
    {
        if (std::all_of(kind.begin(), kind.end(), [](uint8_t byte) { return byte == 0xff; }))
        {
            kind.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(l_PublishMutex);
        l_Published = std::move(data);
        l_PublishedGeneration = generation;
        l_PublishedFresh = true;
    }
    l_TrackPublishedGeneration = generation;
    l_TrackChangeFirstSeen = -1;
}

//
// Exported Functions
//

CORE_EXPORT bool CoreNetplaySaveData::IsEmpty(void) const
{
    return std::all_of(Kinds.begin(), Kinds.end(),
        [](const std::vector<uint8_t>& kind) { return kind.empty(); });
}

CORE_EXPORT size_t CoreNetplaySaveData::TotalBytes(void) const
{
    size_t total = 0;
    for (const std::vector<uint8_t>& kind : Kinds)
    {
        total += kind.size();
    }
    return total;
}

CORE_EXPORT void CoreSetNetplaySaveData(const CoreNetplaySaveData& data)
{
    l_StagedSaveData = data;
}

CORE_EXPORT void CoreClearNetplaySaveData(void)
{
    l_StagedSaveData = {};
}

CORE_EXPORT const CoreNetplaySaveData& CoreGetNetplaySaveData(void)
{
    return l_StagedSaveData;
}

bool CoreApplyNetplaySaveData(bool enabled)
{
    std::string error;
    m64p_error ret;
    m64p_save_data save = {};

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        std::vector<uint8_t>& bytes = l_StagedSaveData.Kinds[kind];
        save.data[kind] = bytes.empty() ? nullptr : bytes.data();
        save.size[kind] = static_cast<int>(bytes.size());
    }

    ret = m64p::Core.DoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, enabled ? &save : nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreApplyNetplaySaveData m64p::Core.DoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

    // A new match: its saves start at write count 0 again.
    CoreNetplaySaveTrackReset();
    l_TrackPublishedGeneration = 0;
    l_TrackChangeFirstSeen = -1;
    std::lock_guard<std::mutex> lock(l_PublishMutex);
    l_Published = {};
    l_PublishedGeneration = 0;
    l_PublishedFresh = false;
    return true;
}

CORE_EXPORT bool CoreGetRunningSaveData(CoreNetplaySaveData& data, uint32_t& generation)
{
    m64p_save_data save = {};

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        data.Kinds[kind].resize(kMaxKindBytes[kind]);
        save.data[kind] = data.Kinds[kind].data();
        save.size[kind] = static_cast<int>(kMaxKindBytes[kind]);
    }

    if (m64p::Core.DoCommand(M64CMD_ROLLBACK_GET_SAVE_DATA, 0, &save) != M64ERR_SUCCESS)
    {
        return false;
    }

    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        data.Kinds[kind].resize(static_cast<size_t>(std::max(save.size[kind], 0)));
    }
    generation = save.generation;
    return true;
}

CORE_EXPORT bool CoreGetRunningSaveGeneration(uint32_t& generation)
{
    m64p_save_data save = {};

    if (!m64p::Core.IsHooked() ||
        m64p::Core.DoCommand(M64CMD_ROLLBACK_GET_SAVE_DATA, 0, &save) != M64ERR_SUCCESS)
    {
        return false;
    }

    generation = save.generation;
    return true;
}

CORE_EXPORT bool CoreTakeConfirmedSaveData(CoreNetplaySaveData& data, uint32_t& generation)
{
    std::lock_guard<std::mutex> lock(l_PublishMutex);
    if (!l_PublishedFresh)
    {
        return false;
    }

    data = l_Published;
    generation = l_PublishedGeneration;
    l_PublishedFresh = false;
    return true;
}

void CoreNetplaySaveTrackReset(void)
{
    l_Track.fill(TrackedFrame{});
    l_TrackChangeFirstSeen = -1;
}

void CoreNetplaySaveTrackStateSaved(int frame)
{
    uint32_t generation = 0;
    if (frame < 0 || !CoreGetRunningSaveGeneration(generation))
    {
        return;
    }

    l_Track[frame % kTrackFrames] = { frame, generation };
}

void CoreNetplaySaveTrackEndFrame(int confirmedFrame)
{
    if (confirmedFrame < 0)
    {
        return;
    }

    const TrackedFrame& at = l_Track[confirmedFrame % kTrackFrames];
    if (at.frame != confirmedFrame)
    {
        return;
    }
    if (at.generation == l_TrackPublishedGeneration)
    {
        l_TrackChangeFirstSeen = -1;
        return;
    }
    if (l_TrackChangeFirstSeen < 0)
    {
        l_TrackChangeFirstSeen = confirmedFrame;
    }

    // Predicted frames past the confirmed one wrote again: the running bytes
    // aren't the confirmed ones, so wait for the confirmed frame to catch up.
    uint32_t current = 0;
    if (!CoreGetRunningSaveGeneration(current) || current != at.generation)
    {
        return;
    }

    bool settled = false;
    if (confirmedFrame >= kSettleFrames)
    {
        const TrackedFrame& before = l_Track[(confirmedFrame - kSettleFrames) % kTrackFrames];
        settled = before.frame == confirmedFrame - kSettleFrames && before.generation == at.generation;
    }
    const bool overdue = confirmedFrame - l_TrackChangeFirstSeen >= kOverdueFrames;
    if (settled || overdue)
    {
        publish_running_save_data();
    }
}

void CoreNetplaySaveTrackSessionEnd(int confirmedFrame)
{
    // The match is over: nothing can change the confirmed frame any more, so
    // its saves count as settled now.
    if (confirmedFrame < 0)
    {
        return;
    }

    const TrackedFrame& at = l_Track[confirmedFrame % kTrackFrames];
    uint32_t current = 0;
    if (at.frame == confirmedFrame && at.generation != l_TrackPublishedGeneration &&
        CoreGetRunningSaveGeneration(current) && current == at.generation)
    {
        publish_running_save_data();
    }
}

CORE_EXPORT bool CoreSerializeNetplaySaveData(const CoreNetplaySaveData& data, std::string& out)
{
    out.assign(kManifestMagic, sizeof(kManifestMagic));
    append_u32(out, CoreSaveDataKindCount);
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        const std::vector<uint8_t>& bytes = data.Kinds[kind];
        if (bytes.size() > kMaxKindBytes[kind])
        {
            CoreSetError("CoreSerializeNetplaySaveData: save is larger than the cartridge allows");
            return false;
        }
        append_u32(out, static_cast<uint32_t>(bytes.size()));
        out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return true;
}

CORE_EXPORT bool CoreDeserializeNetplaySaveData(const std::string& in, CoreNetplaySaveData& data)
{
    size_t offset = sizeof(kManifestMagic);
    uint32_t count = 0;

    data = {};
    if (in.size() < sizeof(kManifestMagic) ||
        std::memcmp(in.data(), kManifestMagic, sizeof(kManifestMagic)) != 0 ||
        !read_u32(in, offset, count) || count != CoreSaveDataKindCount)
    {
        CoreSetError("CoreDeserializeNetplaySaveData: invalid save data");
        return false;
    }

    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        uint32_t size = 0;
        if (!read_u32(in, offset, size) || size > kMaxKindBytes[kind] || offset + size > in.size())
        {
            CoreSetError("CoreDeserializeNetplaySaveData: truncated save data");
            return false;
        }
        data.Kinds[kind].assign(in.begin() + offset, in.begin() + offset + size);
        offset += size;
    }

    if (offset != in.size())
    {
        CoreSetError("CoreDeserializeNetplaySaveData: trailing save data");
        return false;
    }
    return true;
}

CORE_EXPORT bool CoreReadNetplaySaveFile(const std::filesystem::path& file, CoreNetplaySaveData& data, CoreNetplaySaveInfo& info)
{
    std::string contents;

    data = {};
    info = {};
    if (!read_file(file, contents))
    {
        CoreSetError("CoreReadNetplaySaveFile: couldn't read " + file.string());
        return false;
    }

    const int nativeKind = kind_for_extension(file);
    if (nativeKind >= 0)
    {
        if (contents.empty() || contents.size() > kMaxKindBytes[nativeKind])
        {
            CoreSetError("CoreReadNetplaySaveFile: " + file.filename().string() + " isn't a save of that kind");
            return false;
        }
        data.Kinds[nativeKind].assign(contents.begin(), contents.end());
        info.Name = file.stem().string();
        return true;
    }

    size_t offset = sizeof(kFileMagic);
    uint32_t version = 0;
    uint32_t metaLength = 0;
    uint32_t count = 0;
    if (contents.size() < sizeof(kFileMagic) ||
        std::memcmp(contents.data(), kFileMagic, sizeof(kFileMagic)) != 0 ||
        !read_u32(contents, offset, version) || version != kFileVersion ||
        !read_u32(contents, offset, metaLength) || offset + metaLength > contents.size())
    {
        CoreSetError("CoreReadNetplaySaveFile: " + file.filename().string() + " isn't a Dale64 netplay save");
        return false;
    }

    std::istringstream meta(contents.substr(offset, metaLength));
    offset += metaLength;
    for (std::string line; std::getline(meta, line);)
    {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "name")
            info.Name = value;
        else if (key == "goodName")
            info.GoodName = value;
        else if (key == "romMd5")
            info.RomMD5 = value;
        else if (key == "created")
            info.CreatedUnix = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "player")
            info.Players.push_back(value);
    }

    if (!read_u32(contents, offset, count) || count > CoreSaveDataKindCount)
    {
        CoreSetError("CoreReadNetplaySaveFile: " + file.filename().string() + " is damaged");
        return false;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t kind = 0;
        uint32_t size = 0;
        if (!read_u32(contents, offset, kind) || kind >= CoreSaveDataKindCount ||
            !read_u32(contents, offset, size) || size > kMaxKindBytes[kind] ||
            offset + size > contents.size())
        {
            CoreSetError("CoreReadNetplaySaveFile: " + file.filename().string() + " is damaged");
            return false;
        }
        data.Kinds[kind].assign(contents.begin() + offset, contents.begin() + offset + size);
        offset += size;
    }
    if (info.Name.empty())
    {
        info.Name = file.stem().string();
    }
    return true;
}

CORE_EXPORT bool CoreWriteNetplaySaveFile(const std::filesystem::path& file, const CoreNetplaySaveData& data, const CoreNetplaySaveInfo& info)
{
    std::string contents(kFileMagic, sizeof(kFileMagic));
    std::string meta;
    std::error_code errorCode;

    append_u32(contents, kFileVersion);
    meta += meta_line("name", info.Name);
    meta += meta_line("goodName", info.GoodName);
    meta += meta_line("romMd5", info.RomMD5);
    meta += meta_line("created", std::to_string(info.CreatedUnix));
    for (const std::string& player : info.Players)
    {
        meta += meta_line("player", player);
    }
    append_u32(contents, static_cast<uint32_t>(meta.size()));
    contents += meta;

    uint32_t count = 0;
    for (const std::vector<uint8_t>& bytes : data.Kinds)
    {
        count += bytes.empty() ? 0 : 1;
    }
    append_u32(contents, count);
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        const std::vector<uint8_t>& bytes = data.Kinds[kind];
        if (bytes.empty())
        {
            continue;
        }
        append_u32(contents, static_cast<uint32_t>(kind));
        append_u32(contents, static_cast<uint32_t>(bytes.size()));
        contents.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), errorCode);
    }

    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            CoreSetError("CoreWriteNetplaySaveFile: couldn't write " + file.string());
            return false;
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        if (stream.fail())
        {
            std::filesystem::remove(temporary, errorCode);
            CoreSetError("CoreWriteNetplaySaveFile: couldn't write " + file.string());
            return false;
        }
    }

    std::filesystem::rename(temporary, file, errorCode);
    if (errorCode)
    {
        std::filesystem::remove(temporary, errorCode);
        CoreSetError("CoreWriteNetplaySaveFile: couldn't replace " + file.string());
        return false;
    }
    return true;
}

CORE_EXPORT bool CoreReadOfflineSaveData(const std::filesystem::path& rom, CoreNetplaySaveData& data, std::vector<std::filesystem::path>& files)
{
    CoreRomType type;
    CoreRomHeader header;
    CoreRomSettings defaultSettings;
    CoreRomSettings settings;

    data = {};
    files.clear();
    if (!CoreGetCachedRomHeaderAndSettings(rom, &type, &header, &defaultSettings, &settings) &&
        (CoreIsEmulationRunning() || !CoreUpdateCachedRomHeaderAndSettings(rom) ||
         !CoreGetCachedRomHeaderAndSettings(rom, &type, &header, &defaultSettings, &settings)))
    {
        return false;
    }

    const std::string basename = offline_save_basename(header, settings);
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        std::filesystem::path file = CoreGetSaveDirectory() / (basename + kKindExtensions[kind]);
        std::string contents;
        std::error_code errorCode;
        if (!std::filesystem::is_regular_file(file, errorCode) || !read_file(file, contents) ||
            contents.empty() || contents.size() > kMaxKindBytes[kind])
        {
            continue;
        }
        data.Kinds[kind].assign(contents.begin(), contents.end());
        files.push_back(file);
    }
    return !files.empty();
}

CORE_EXPORT std::filesystem::path CoreGetNetplaySaveDirectory(void)
{
    return CoreGetSaveDirectory() / "Netplay";
}
