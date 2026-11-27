#define CORE_INTERNAL
#include "StageList.hpp"
#include "Library.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>

//
// Local Variables
//

namespace
{
// Crockford's base32: no I, L, O or U, so a code can't be misread or spell much.
constexpr char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int kCodeVersion = 1;
constexpr int kVersionBits = 4;
constexpr int kCountBits = 7;
constexpr int kEntryBits = 10;
constexpr int kCrcBits = 16;
constexpr int kGroupLength = 4;

// The stage icon file is a quarter of a megabyte; nothing in the file table
// of a ROM the pack fits is near this.
constexpr size_t kMaxFileBytes = 4 * 1024 * 1024;

// A string of '0'/'1': codes are at most a few hundred bits.
using Bits = std::string;

void append_bits(Bits& bits, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--)
        bits.push_back(((value >> i) & 1) ? '1' : '0');
}

uint32_t read_bits(const Bits& bits, size_t at, int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; i++)
        value = (value << 1) | (bits[at + i] == '1' ? 1u : 0u);
    return value;
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final xor)
// over the salt and then the bits, zero-padded to whole bytes.
uint16_t code_crc(const std::string& salt, const Bits& bits)
{
    uint32_t crc = 0xFFFF;
    const auto feed = [&crc](uint8_t byte) {
        crc ^= uint32_t(byte) << 8;
        for (int i = 0; i < 8; i++)
            crc = ((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1)) & 0xFFFF;
    };
    for (unsigned char c : salt)
        feed(c);
    for (size_t i = 0; i < bits.size(); i += 8)
    {
        uint8_t byte = 0;
        for (size_t k = 0; k < 8; k++)
            byte = uint8_t((byte << 1) | ((i + k < bits.size() && bits[i + k] == '1') ? 1 : 0));
        feed(byte);
    }
    return uint16_t(crc);
}

uint32_t be32(const std::vector<char>& data, size_t at)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data()) + at;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t be32(const std::vector<uint8_t>& data, size_t at)
{
    const uint8_t* p = data.data() + at;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t be16(const std::vector<uint8_t>& data, size_t at)
{
    return uint16_t((data[at] << 8) | data[at + 1]);
}

//
// vpk0, the game's compression (a port of syDmaDecodeVpk0 in the original
// game's decompilation): Huffman-coded literals and back references, read
// most significant bit first from big-endian 16-bit words.
//

class Vpk0Reader
{
  public:
    Vpk0Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size)
    {
    }

    bool bits(int count, uint32_t& out)
    {
        while (m_count < count)
        {
            if (m_pos + 2 > m_size)
                return false;
            m_buffer = ((m_buffer << 16) | (uint64_t(m_data[m_pos]) << 8) | m_data[m_pos + 1]) & 0xFFFFFFFFFFFFull;
            m_pos += 2;
            m_count += 16;
        }
        m_count -= count;
        out = uint32_t((m_buffer >> m_count) & ((uint64_t(1) << count) - 1));
        return true;
    }

  private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    uint64_t m_buffer = 0;
    int m_count = 0;
};

struct Vpk0Node
{
    int Left = -1;
    int Right = -1;
    // A leaf's value: how many bits to read next.
    int Value = 0;
};

// Builds a tree into nodes and returns its root, or -1 for an empty one.
bool vpk0_tree(Vpk0Reader& reader, std::vector<Vpk0Node>& nodes, int& root)
{
    std::vector<int> stack;
    for (;;)
    {
        uint32_t bit = 0;
        if (!reader.bits(1, bit))
            return false;
        if (bit == 1)
        {
            if (stack.size() < 2)
                break;
            const int right = stack.back();
            stack.pop_back();
            const int left = stack.back();
            stack.pop_back();
            nodes.push_back({ left, right, 0 });
            stack.push_back(int(nodes.size()) - 1);
        }
        else
        {
            uint32_t value = 0;
            if (!reader.bits(8, value))
                return false;
            nodes.push_back({ -1, -1, int(value) });
            stack.push_back(int(nodes.size()) - 1);
        }
        if (nodes.size() > 4096)
            return false;
    }
    root = stack.empty() ? -1 : stack.front();
    return true;
}

bool vpk0_value(Vpk0Reader& reader, const std::vector<Vpk0Node>& nodes, int root, uint32_t& out)
{
    if (root < 0)
        return false;
    int node = root;
    while (nodes[size_t(node)].Left >= 0)
    {
        uint32_t bit = 0;
        if (!reader.bits(1, bit))
            return false;
        node = bit ? nodes[size_t(node)].Right : nodes[size_t(node)].Left;
    }
    const int width = nodes[size_t(node)].Value;
    if (width > 32)
        return false;
    if (width == 0)
    {
        out = 0;
        return true;
    }
    return reader.bits(width, out);
}

bool vpk0_decode(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    Vpk0Reader reader(data, size);
    uint32_t magic = 0, outSize = 0, method = 0;
    if (size < 9 || std::memcmp(data, "vpk0", 4) != 0 || !reader.bits(32, magic) || !reader.bits(32, outSize) ||
        !reader.bits(8, method) || outSize > kMaxFileBytes)
        return false;
    std::vector<Vpk0Node> nodes;
    int offsets = -1, lengths = -1;
    if (!vpk0_tree(reader, nodes, offsets) || !vpk0_tree(reader, nodes, lengths))
        return false;

    out.clear();
    out.reserve(outSize);
    while (out.size() < outSize)
    {
        uint32_t flag = 0;
        if (!reader.bits(1, flag))
            return false;
        if (flag == 0)
        {
            uint32_t literal = 0;
            if (!reader.bits(8, literal))
                return false;
            out.push_back(uint8_t(literal));
            continue;
        }
        int64_t from = 0;
        uint32_t value = 0;
        if (!vpk0_value(reader, nodes, offsets, value))
            return false;
        if (method != 0)
        {
            uint32_t extra = 0;
            if (value <= 2)
            {
                extra = value + 1;
                if (!vpk0_value(reader, nodes, offsets, value))
                    return false;
            }
            from = int64_t(out.size()) - int64_t(value) * 4 - int64_t(extra) + 8;
        }
        else
        {
            from = int64_t(out.size()) - int64_t(value);
        }
        uint32_t length = 0;
        if (!vpk0_value(reader, nodes, lengths, length))
            return false;
        // Overlapping copies repeat what they just wrote, byte by byte.
        for (uint32_t k = 0; k < length; k++)
        {
            const int64_t at = from + k;
            if (at < 0 || at >= int64_t(out.size()))
                return false;
            out.push_back(out[size_t(at)]);
        }
    }
    out.resize(outSize);
    return true;
}

//
// The game's file table: 12-byte big-endian entries, u32 data offset (bit
// 31: vpk0-compressed) from the end of the table, u16 internal relocation
// chain start, u16 stored size, u16 external chain start, u16 size, the
// sizes and chain starts in 32-bit words (0xFFFF: no chain).
//

struct GameFile
{
    std::vector<uint8_t> Data;
    // Pointer words that point into another file: offset -> file id.
    std::map<uint32_t, uint32_t> Externs;
};

bool load_game_file(const std::vector<char>& rom, const CoreStageListLayout& layout, uint32_t id, GameFile& file)
{
    if (id >= layout.FileCount)
        return false;
    const uint64_t entry = uint64_t(layout.FileTable) + uint64_t(id) * 12;
    const uint64_t dataBase = uint64_t(layout.FileTable) + (uint64_t(layout.FileCount) + 1) * 12;
    if (entry + 12 > rom.size())
        return false;
    const uint32_t word = be32(rom, size_t(entry));
    const auto u16at = [&rom](uint64_t at) {
        return uint16_t((uint8_t(rom[size_t(at)]) << 8) | uint8_t(rom[size_t(at) + 1]));
    };
    const uint16_t externChain = u16at(entry + 8);
    const uint64_t storedBytes = uint64_t(u16at(entry + 6)) * 4;
    const uint64_t sizeBytes = uint64_t(u16at(entry + 10)) * 4;
    const uint64_t start = dataBase + (word & 0x7FFFFFFF);
    if (start + storedBytes > rom.size() || sizeBytes > kMaxFileBytes)
        return false;

    const auto* stored = reinterpret_cast<const uint8_t*>(rom.data()) + start;
    if (word & 0x80000000)
    {
        if (!vpk0_decode(stored, size_t(storedBytes), file.Data))
            return false;
    }
    else
    {
        file.Data.assign(stored, stored + storedBytes);
    }
    if (file.Data.size() != sizeBytes)
        return false;

    // The file ids of the external pointers follow the stored data, in chain order.
    uint64_t idsAt = start + storedBytes;
    std::set<uint32_t> seen;
    for (uint32_t at = externChain; at != 0xFFFF;)
    {
        const uint32_t offset = at * 4;
        if (offset + 4 > file.Data.size() || idsAt + 2 > rom.size() || !seen.insert(offset).second)
            return false;
        file.Externs[offset] = u16at(idsAt);
        idsAt += 2;
        at = be16(file.Data, offset);
    }
    return true;
}

class GameFiles
{
  public:
    GameFiles(const std::vector<char>& rom, const CoreStageListLayout& layout) : m_rom(rom), m_layout(layout)
    {
    }

    const GameFile* file(uint32_t id)
    {
        auto it = m_files.find(id);
        if (it == m_files.end())
        {
            std::optional<GameFile> loaded(std::in_place);
            if (!load_game_file(m_rom, m_layout, id, *loaded))
                loaded.reset();
            it = m_files.emplace(id, std::move(loaded)).first;
        }
        return it->second ? &*it->second : nullptr;
    }

    // Where the pointer word at offset in file id points: the file (the
    // same one, or the one an external pointer names) and the offset in it.
    bool deref(uint32_t id, uint32_t offset, uint32_t& targetFile, uint32_t& target)
    {
        const GameFile* f = file(id);
        if (!f || offset + 4 > f->Data.size())
            return false;
        target = (be32(f->Data, offset) & 0xFFFF) * 4;
        const auto ext = f->Externs.find(offset);
        targetFile = ext != f->Externs.end() ? ext->second : id;
        const GameFile* t = file(targetFile);
        return t && target < t->Data.size();
    }

  private:
    const std::vector<char>& m_rom;
    const CoreStageListLayout& m_layout;
    std::map<uint32_t, std::optional<GameFile>> m_files;
};

uint8_t scale5(uint32_t value)
{
    return uint8_t(value * 255 / 31);
}

// A libultra Sprite (0x44 bytes) with 16-bit RGBA bitmaps, which every stage
// icon is: width +0x04, height +0x06, attr +0x14, bitmap count +0x28, format
// and texel size +0x30/+0x31, Bitmap* +0x34. A Bitmap (0x10 bytes): width,
// image width, buffer pointer at +8, rows at +0xC. The bitmaps stack top to
// bottom. SP_TEXSHUF in attr: odd rows are stored with their 32-bit words
// swapped in each 64-bit pair, as TMEM wants them, so texel x of an odd row
// sits at x ^ 2.
bool decode_sprite(GameFiles& files, uint32_t fileId, uint32_t at, CoreStageIcon& icon)
{
    const GameFile* f = files.file(fileId);
    if (!f || uint64_t(at) + 0x44 > f->Data.size())
        return false;
    const std::vector<uint8_t>& d = f->Data;
    const uint16_t attr = be16(d, at + 0x14);
    const uint16_t bitmaps = be16(d, at + 0x28);
    const uint8_t format = d[at + 0x30];
    const uint8_t texel = d[at + 0x31];
    if (format != 0 || texel != 2 || bitmaps == 0 || bitmaps > 64)
        return false;
    uint32_t bitmapFile = 0, bitmapAt = 0;
    if (!files.deref(fileId, at + 0x34, bitmapFile, bitmapAt))
        return false;

    icon = {};
    for (uint32_t i = 0; i < bitmaps; i++)
    {
        const GameFile* bf = files.file(bitmapFile);
        const uint32_t entry = bitmapAt + i * 16;
        if (!bf || uint64_t(entry) + 16 > bf->Data.size())
            return false;
        const int width = int16_t(be16(bf->Data, entry));
        const int imageWidth = int16_t(be16(bf->Data, entry + 2));
        const int rows = int16_t(be16(bf->Data, entry + 12));
        uint32_t bufferFile = 0, bufferAt = 0;
        if (width <= 0 || width > 1024 || imageWidth < width || rows <= 0 || rows > 1024 ||
            (icon.Width != 0 && icon.Width != width) || !files.deref(bitmapFile, entry + 8, bufferFile, bufferAt))
            return false;
        const GameFile* buffer = files.file(bufferFile);
        if (!buffer || uint64_t(bufferAt) + uint64_t(rows) * imageWidth * 2 > buffer->Data.size())
            return false;
        icon.Width = width;
        for (int y = 0; y < rows; y++)
        {
            for (int x = 0; x < width; x++)
            {
                const int stored = ((attr & 0x200) && (y & 1)) ? (x ^ 2) : x;
                if (stored >= imageWidth)
                    return false;
                const uint16_t v = be16(buffer->Data, bufferAt + (uint32_t(y) * imageWidth + stored) * 2);
                icon.Rgba.push_back(scale5((v >> 11) & 31));
                icon.Rgba.push_back(scale5((v >> 6) & 31));
                icon.Rgba.push_back(scale5((v >> 1) & 31));
                icon.Rgba.push_back((v & 1) ? 255 : 0);
            }
        }
        icon.Height += rows;
    }
    return icon.Width > 0 && icon.Height > 0;
}
} // namespace

//
// Exported Functions
//

CORE_EXPORT const CoreStageListStage* CoreStageListInfo::stage(int id) const
{
    for (const CoreStageListStage& s : Stages)
    {
        if (s.Id == id)
            return &s;
    }
    return nullptr;
}

CORE_EXPORT const CoreStageListPreset* CoreStageListInfo::preset(const std::string& id) const
{
    for (const CoreStageListPreset& p : Presets)
    {
        if (p.Id == id)
            return &p;
    }
    return nullptr;
}

CORE_EXPORT int CoreStageListInfo::pagesFor(int n) const
{
    return PageStages > 0 && n > 0 ? (n + PageStages - 1) / PageStages : 0;
}

CORE_EXPORT CoreStageListProblem CoreNormalizeStageList(const CoreStageListInfo& info,
                                                        std::vector<CoreStageListEntry>& entries)
{
    if (entries.empty())
        return CoreStageListProblem::Empty;
    if (int(entries.size()) > info.MaxStages)
        return CoreStageListProblem::TooMany;
    std::set<int> seen;
    for (CoreStageListEntry& entry : entries)
    {
        const CoreStageListStage* s = info.stage(entry.Stage);
        if (!s)
            return CoreStageListProblem::UnknownStage;
        if (!seen.insert(entry.Stage).second)
            return CoreStageListProblem::Repeated;
        entry.Mode &= uint8_t((s->Hazards ? CoreStageModeHazardsOff : 0) | (s->Movement ? CoreStageModeMovementOff : 0));
    }
    return CoreStageListProblem::None;
}

// Bits, most significant first: 4 bits version, 7 bits count, then 8 bits
// stage id and 2 bits mode per entry, a 16-bit checksum, and zero bits up to
// a whole base32 character. Written in groups of 4 after the prefix.
CORE_EXPORT std::string CoreEncodeStageList(const CoreStageListInfo& info, std::vector<CoreStageListEntry> entries)
{
    if (CoreNormalizeStageList(info, entries) != CoreStageListProblem::None || entries.size() >= (1u << kCountBits))
        return std::string();
    Bits bits;
    append_bits(bits, kCodeVersion, kVersionBits);
    append_bits(bits, uint32_t(entries.size()), kCountBits);
    for (const CoreStageListEntry& entry : entries)
    {
        append_bits(bits, entry.Stage, 8);
        append_bits(bits, entry.Mode, 2);
    }
    append_bits(bits, code_crc(info.CodeSalt, bits), kCrcBits);
    while (bits.size() % 5 != 0)
        bits.push_back('0');

    std::string chars;
    for (size_t i = 0; i < bits.size(); i += 5)
        chars.push_back(kCodeAlphabet[read_bits(bits, i, 5)]);
    std::string code = info.CodePrefix;
    for (size_t i = 0; i < chars.size(); i += kGroupLength)
        code += "-" + chars.substr(i, kGroupLength);
    return code;
}

CORE_EXPORT CoreStageListProblem CoreDecodeStageList(const CoreStageListInfo& info, const std::string& code,
                                                     std::vector<CoreStageListEntry>& entries)
{
    entries.clear();
    std::string text;
    for (unsigned char c : code)
    {
        if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n')
            continue;
        text.push_back(char(std::toupper(c)));
    }
    std::string prefix = info.CodePrefix;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (!prefix.empty() && text.compare(0, prefix.size(), prefix) == 0)
        text.erase(0, prefix.size());

    Bits bits;
    for (char c : text)
    {
        if (c == 'O')
            c = '0';
        else if (c == 'I' || c == 'L')
            c = '1';
        const char* at = std::strchr(kCodeAlphabet, c);
        if (c == '\0' || at == nullptr)
            return CoreStageListProblem::NotACode;
        append_bits(bits, uint32_t(at - kCodeAlphabet), 5);
    }
    if (bits.size() < size_t(kVersionBits + kCountBits + kEntryBits + kCrcBits))
        return CoreStageListProblem::BadLength;
    if (read_bits(bits, 0, kVersionBits) != uint32_t(kCodeVersion))
        return CoreStageListProblem::BadVersion;
    const int n = int(read_bits(bits, kVersionBits, kCountBits));
    const size_t end = size_t(kVersionBits + kCountBits) + size_t(n) * kEntryBits;
    if (n < 1 || n > info.MaxStages || bits.size() < end + kCrcBits)
        return CoreStageListProblem::BadLength;
    // Exactly the padding a code of n stages has, all zero.
    const size_t padded = (end + kCrcBits + 4) / 5 * 5;
    if (bits.size() != padded || bits.find('1', end + kCrcBits) != Bits::npos)
        return CoreStageListProblem::BadLength;
    if (read_bits(bits, end, kCrcBits) != code_crc(info.CodeSalt, bits.substr(0, end)))
        return CoreStageListProblem::BadChecksum;

    for (int i = 0; i < n; i++)
    {
        const size_t at = size_t(kVersionBits + kCountBits) + size_t(i) * kEntryBits;
        entries.push_back({ uint8_t(read_bits(bits, at, 8)), uint8_t(read_bits(bits, at + 8, 2)) });
    }
    const CoreStageListProblem problem = CoreNormalizeStageList(info, entries);
    if (problem != CoreStageListProblem::None)
        entries.clear();
    return problem;
}

std::vector<CoreGameOptionPatch> CoreStageListPatches(const CoreStageListInfo& info, const CoreStageListLayout& layout,
                                                      const std::vector<CoreStageListEntry>& entries)
{
    std::vector<CoreGameOptionPatch> patches;

    // The stage select's icons, page by page: the list's stages in order on
    // each page's first PageStages icons, Random on the rest and once the
    // list runs out.
    CoreGameOptionPatch table{ layout.TableOffset, {}, layout.TableExpect };
    size_t next = 0;
    for (size_t slot = 0; slot < layout.TableExpect.size(); slot++)
    {
        const bool stageSlot = int(slot % size_t(info.PageIcons)) < info.PageStages;
        table.Bytes.push_back(stageSlot && next < entries.size() ? entries[next++].Stage : info.RandomStage);
    }
    patches.push_back(std::move(table));

    CoreGameOptionPatch pages{ layout.PagesOffset, std::vector<uint8_t>(layout.PagesExpect.size(), 0), layout.PagesExpect };
    uint32_t count = uint32_t(info.pagesFor(int(entries.size())));
    for (size_t i = pages.Bytes.size(); i-- > 0; count >>= 8)
        pages.Bytes[i] = uint8_t(count & 0xFF);
    patches.push_back(std::move(pages));

    // Every stage keeps the game's own byte unless the list has it.
    CoreGameOptionPatch modes{ layout.ModesOffset, layout.ModesExpect, layout.ModesExpect };
    for (const CoreStageListEntry& entry : entries)
    {
        if (entry.Stage < modes.Bytes.size())
            modes.Bytes[entry.Stage] = entry.Mode;
    }
    patches.push_back(std::move(modes));
    return patches;
}

bool CoreDecodeStageListIcons(const std::vector<char>& rom, const CoreStageListInfo& info,
                              const CoreStageListLayout& layout, std::map<int, CoreStageIcon>& icons,
                              std::string& error)
{
    icons.clear();
    GameFiles files(rom, layout);
    if (!files.file(layout.IconFile))
    {
        error = "the stage icon file couldn't be read";
        return false;
    }

    std::vector<int> ids;
    for (const CoreStageListStage& s : info.Stages)
        ids.push_back(s.Id);
    ids.push_back(info.RandomStage);
    for (const int id : ids)
    {
        const uint64_t at = id == info.RandomStage ? layout.RandomSprite : uint64_t(layout.Sprites) + uint64_t(id) * 4;
        if (at + 4 > rom.size())
            continue;
        const uint32_t word = be32(rom, size_t(at));
        const uint32_t index = word >> 24;
        uint32_t fileId = layout.IconFile;
        uint32_t offset = word & 0xFFFFFF;
        if (index != 0)
        {
            // The file whose base the icon file's n-th file pointer names.
            uint32_t base = 0;
            if (!files.deref(layout.IconFile, layout.FileBases + 4 * (index - 1), fileId, base))
                continue;
            offset += base;
        }
        CoreStageIcon icon;
        if (decode_sprite(files, fileId, offset, icon))
            icons[id] = std::move(icon);
    }
    if (icons.empty())
    {
        error = "no stage icons could be decoded";
        return false;
    }
    return true;
}
