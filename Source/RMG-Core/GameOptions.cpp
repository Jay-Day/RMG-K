#define CORE_INTERNAL
#include "GameOptions.hpp"
#include "Directories.hpp"
#include "Callback.hpp"
#include "Archive.hpp"
#include "Library.hpp"
#include "String.hpp"
#include "Error.hpp"
#include "File.hpp"
#include "Rom.hpp"

#include "m64p/Api.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

//
// Local Variables
//

namespace
{
constexpr char kSectionMagic[8] = { 'R', 'M', 'G', 'K', 'G', 'O', 'P', '1' };
constexpr int kPackFormat = 1;
// The cartridge's address space (see device.c); no ROM is larger.
constexpr uint64_t kMaxRomBytes = 0x0FC00000;
// Packs are a few kilobytes; anything near this isn't one.
constexpr std::uintmax_t kMaxPackFileBytes = 4 * 1024 * 1024;
constexpr int kMaxJsonDepth = 32;

struct LoadedOption
{
    std::string Id;
    std::string Label;
    std::string Description;
    // language code -> (label, description), either may be empty
    std::map<std::string, std::pair<std::string, std::string>> Translations;
    std::vector<CoreGameOptionPatch> Patches;
    // "serverOnly": never a host's switch, only the lobby's to set
    bool ServerOnly = false;
};

struct LoadedPack
{
    std::string Id;
    std::string Game;
    std::vector<std::string> RomMd5; // upper case
    std::vector<LoadedOption> Options;
    std::filesystem::path File;
};

bool l_PacksLoaded = false;
std::vector<LoadedPack> l_Packs;

CoreNetplayGameOptions l_StagedGameOptions;

//
// A small JSON reader for the packs. RMG-Core has no JSON library, and the
// packs only need objects, arrays, strings, numbers and booleans.
//

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    // String: the decoded text. Number: the literal, so integers stay exact.
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(std::string_view key) const
    {
        for (const auto& member : object)
        {
            if (member.first == key)
            {
                return &member.second;
            }
        }
        return nullptr;
    }
};

class JsonReader
{
public:
    explicit JsonReader(std::string_view text) : m_text(text)
    {
    }

    bool parse(JsonValue& out, std::string& error)
    {
        // a UTF-8 byte order mark
        if (m_text.substr(0, 3) == "\xEF\xBB\xBF")
        {
            m_pos = 3;
        }
        if (!value(out, 0))
        {
            error = m_error + " at byte " + std::to_string(m_pos);
            return false;
        }
        skip_space();
        if (m_pos != m_text.size())
        {
            error = "unexpected text after the JSON at byte " + std::to_string(m_pos);
            return false;
        }
        return true;
    }

private:
    bool fail(const char* what)
    {
        if (m_error.empty())
        {
            m_error = what;
        }
        return false;
    }

    void skip_space()
    {
        while (m_pos < m_text.size() &&
            (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
        {
            m_pos++;
        }
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word)
        {
            return fail("invalid literal");
        }
        m_pos += word.size();
        return true;
    }

    bool value(JsonValue& out, int depth)
    {
        if (depth > kMaxJsonDepth)
        {
            return fail("nested too deeply");
        }
        skip_space();
        if (m_pos >= m_text.size())
        {
            return fail("unexpected end");
        }

        const char c = m_text[m_pos];
        if (c == '{')
        {
            out.type = JsonValue::Type::Object;
            m_pos++;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == '}')
            {
                m_pos++;
                return true;
            }
            for (;;)
            {
                skip_space();
                std::string key;
                if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !string(key))
                {
                    return fail("expected a key");
                }
                skip_space();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                {
                    return fail("expected ':'");
                }
                m_pos++;
                JsonValue member;
                if (!value(member, depth + 1))
                {
                    return false;
                }
                out.object.emplace_back(std::move(key), std::move(member));
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',')
                {
                    m_pos++;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}')
                {
                    m_pos++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            out.type = JsonValue::Type::Array;
            m_pos++;
            skip_space();
            if (m_pos < m_text.size() && m_text[m_pos] == ']')
            {
                m_pos++;
                return true;
            }
            for (;;)
            {
                JsonValue element;
                if (!value(element, depth + 1))
                {
                    return false;
                }
                out.array.push_back(std::move(element));
                skip_space();
                if (m_pos < m_text.size() && m_text[m_pos] == ',')
                {
                    m_pos++;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']')
                {
                    m_pos++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"')
        {
            out.type = JsonValue::Type::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f')
        {
            out.type = JsonValue::Type::Bool;
            out.boolean = (c == 't');
            return literal(out.boolean ? "true" : "false");
        }
        if (c == 'n')
        {
            out.type = JsonValue::Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            out.type = JsonValue::Type::Number;
            return number(out.text);
        }
        return fail("unexpected character");
    }

    bool number(std::string& out)
    {
        const size_t start = m_pos;
        auto digits = [this]() {
            const size_t from = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
            {
                m_pos++;
            }
            return m_pos > from;
        };

        if (m_text[m_pos] == '-')
        {
            m_pos++;
        }
        if (!digits())
        {
            return fail("invalid number");
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.')
        {
            m_pos++;
            if (!digits())
            {
                return fail("invalid number");
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        {
            m_pos++;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
            {
                m_pos++;
            }
            if (!digits())
            {
                return fail("invalid number");
            }
        }
        out.assign(m_text.substr(start, m_pos - start));
        return true;
    }

    bool hex4(uint32_t& out)
    {
        if (m_pos + 4 > m_text.size())
        {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; i++)
        {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return fail("invalid \\u escape");
        }
        return true;
    }

    static void append_utf8(std::string& out, uint32_t codepoint)
    {
        if (codepoint < 0x80)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    // m_pos is at the opening quote.
    bool string(std::string& out)
    {
        out.clear();
        m_pos++;
        while (m_pos < m_text.size())
        {
            const unsigned char c = static_cast<unsigned char>(m_text[m_pos++]);
            if (c == '"')
            {
                return true;
            }
            if (c < 0x20)
            {
                return fail("control character in a string");
            }
            if (c != '\\')
            {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (m_pos >= m_text.size())
            {
                break;
            }
            const char escape = m_text[m_pos++];
            switch (escape)
            {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u':
            {
                uint32_t codepoint = 0;
                if (!hex4(codepoint))
                {
                    return false;
                }
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                {
                    uint32_t low = 0;
                    if (m_text.substr(m_pos, 2) != "\\u")
                    {
                        return fail("unpaired surrogate");
                    }
                    m_pos += 2;
                    if (!hex4(low))
                    {
                        return false;
                    }
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        return fail("unpaired surrogate");
                    }
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                {
                    return fail("unpaired surrogate");
                }
                append_utf8(out, codepoint);
                break;
            }
            default:
                return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    std::string_view m_text;
    size_t m_pos = 0;
    std::string m_error;
};
}

//
// Local Functions
//

static void append_u16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

static void append_u32(std::string& out, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

static bool read_u16(const std::string& in, size_t& offset, uint16_t& value)
{
    if (offset > in.size() || in.size() - offset < 2)
    {
        return false;
    }
    value = static_cast<uint16_t>(static_cast<unsigned char>(in[offset]) |
        (static_cast<unsigned char>(in[offset + 1]) << 8));
    offset += 2;
    return true;
}

static bool read_u32(const std::string& in, size_t& offset, uint32_t& value)
{
    if (offset > in.size() || in.size() - offset < 4)
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

static bool valid_option_id(std::string_view id)
{
    return !id.empty() && id.size() <= CoreGameOptionMaxIdLength &&
        std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
}

static bool valid_pack_id(std::string_view id)
{
    return !id.empty() && id.size() <= 64 &&
        std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        });
}

// Well-formed UTF-8 with no control characters: labels end up in the UI.
static bool valid_label(std::string_view text)
{
    if (text.empty() || text.size() > CoreGameOptionMaxLabelLength)
    {
        return false;
    }
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80)
        {
            if (lead < 0x20 || lead == 0x7f)
            {
                return false;
            }
            i++;
            continue;
        }
        size_t length = 0;
        uint32_t codepoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            codepoint = lead & 0x1F;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            codepoint = lead & 0x0F;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            codepoint = lead & 0x07;
        }
        else
        {
            return false;
        }
        if (text.size() - i < length)
        {
            return false;
        }
        for (size_t k = 1; k < length; k++)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80)
            {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if ((length == 3 && (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF))) ||
            (length == 4 && (codepoint < 0x10000 || codepoint > 0x10FFFF)) ||
            (codepoint >= 0x80 && codepoint < 0xA0))
        {
            return false;
        }
        i += length;
    }
    return true;
}

static std::string upper_md5(std::string md5)
{
    std::transform(md5.begin(), md5.end(), md5.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return md5;
}

static bool valid_md5(const std::string& md5)
{
    return md5.size() == 32 && std::all_of(md5.begin(), md5.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

// Hex digits, with any whitespace between bytes for readability.
static bool parse_hex_bytes(const std::string& text, std::vector<uint8_t>& out)
{
    out.clear();
    int high = -1;
    for (unsigned char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (high >= 0)
            {
                return false;
            }
            continue;
        }
        int nibble;
        if (c >= '0' && c <= '9')
            nibble = c - '0';
        else if (c >= 'a' && c <= 'f')
            nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            nibble = c - 'A' + 10;
        else
            return false;
        if (high < 0)
        {
            high = nibble;
        }
        else
        {
            out.push_back(static_cast<uint8_t>((high << 4) | nibble));
            high = -1;
        }
    }
    return high < 0 && !out.empty();
}

// A hex string ("0x00123456" or "00123456") or a non-negative integer.
static bool parse_offset(const JsonValue& value, uint32_t& out)
{
    uint64_t parsed = 0;
    if (value.type == JsonValue::Type::String)
    {
        std::string_view text = value.text;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            text.remove_prefix(2);
        }
        if (text.empty() || text.size() > 8)
        {
            return false;
        }
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
        {
            return false;
        }
    }
    else if (value.type == JsonValue::Type::Number)
    {
        const std::string& text = value.text;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
        {
            return false; // negative, fractional or too large
        }
    }
    else
    {
        return false;
    }
    if (parsed >= kMaxRomBytes)
    {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

static const std::string* string_member(const JsonValue& object, std::string_view key)
{
    const JsonValue* member = object.find(key);
    return (member != nullptr && member->type == JsonValue::Type::String) ? &member->text : nullptr;
}

static bool patches_overlap(const CoreGameOptionPatch& a, const CoreGameOptionPatch& b)
{
    const uint64_t aEnd = uint64_t(a.Offset) + a.Bytes.size();
    const uint64_t bEnd = uint64_t(b.Offset) + b.Bytes.size();
    return a.Offset < bEnd && b.Offset < aEnd;
}

static void log_pack(const std::filesystem::path& file, const std::string& message)
{
    CoreAddCallbackMessage(CoreDebugMessageType::Warning,
        "Game options: " + file.filename().string() + ": " + message);
}

static bool parse_option(const JsonValue& json, LoadedOption& option, std::string& error)
{
    if (json.type != JsonValue::Type::Object)
    {
        error = "an option isn't an object";
        return false;
    }
    const std::string* id = string_member(json, "id");
    const std::string* label = string_member(json, "label");
    const std::string* description = string_member(json, "description");
    if (id == nullptr || !valid_option_id(*id))
    {
        error = (id == nullptr ? std::string("an option has no id") : "option id \"" + *id + "\" isn't valid") +
            " ([a-z0-9-], up to 32)";
        return false;
    }
    option.Id = *id;
    if (label == nullptr || !valid_label(*label))
    {
        error = "option \"" + option.Id + "\" has no valid label";
        return false;
    }
    option.Label = *label;
    if (description != nullptr)
    {
        option.Description = *description;
    }
    if (const JsonValue* serverOnly = json.find("serverOnly");
        serverOnly != nullptr && serverOnly->type == JsonValue::Type::Bool)
    {
        option.ServerOnly = serverOnly->boolean;
    }

    if (const JsonValue* translations = json.find("translations");
        translations != nullptr && translations->type == JsonValue::Type::Object)
    {
        for (const auto& [language, entry] : translations->object)
        {
            if (entry.type != JsonValue::Type::Object || language.empty())
            {
                continue;
            }
            const std::string* translatedLabel = string_member(entry, "label");
            const std::string* translatedDescription = string_member(entry, "description");
            std::pair<std::string, std::string> text;
            if (translatedLabel != nullptr && valid_label(*translatedLabel))
            {
                text.first = *translatedLabel;
            }
            if (translatedDescription != nullptr)
            {
                text.second = *translatedDescription;
            }
            option.Translations[CoreLowerString(language)] = std::move(text);
        }
    }

    const JsonValue* patches = json.find("patches");
    if (patches == nullptr || patches->type != JsonValue::Type::Array || patches->array.empty())
    {
        error = "option \"" + option.Id + "\" has no patches";
        return false;
    }
    if (patches->array.size() > size_t(CoreGameOptionMaxPatches))
    {
        error = "option \"" + option.Id + "\" has more than " + std::to_string(CoreGameOptionMaxPatches) + " patches";
        return false;
    }

    size_t totalBytes = 0;
    for (const JsonValue& entry : patches->array)
    {
        CoreGameOptionPatch patch;
        const JsonValue* offset = entry.type == JsonValue::Type::Object ? entry.find("offset") : nullptr;
        const std::string* expect = string_member(entry, "expect");
        const std::string* bytes = string_member(entry, "bytes");
        if (offset == nullptr || !parse_offset(*offset, patch.Offset))
        {
            error = "option \"" + option.Id + "\" has a patch without a valid offset";
            return false;
        }
        if (bytes == nullptr || !parse_hex_bytes(*bytes, patch.Bytes) ||
            expect == nullptr || !parse_hex_bytes(*expect, patch.Expect) ||
            patch.Bytes.size() != patch.Expect.size())
        {
            error = "option \"" + option.Id + "\" has a patch whose expect and bytes aren't equal-length hex";
            return false;
        }
        if (uint64_t(patch.Offset) + patch.Bytes.size() > kMaxRomBytes)
        {
            error = "option \"" + option.Id + "\" has a patch past the end of any ROM";
            return false;
        }
        totalBytes += patch.Bytes.size();
        if (totalBytes > CoreGameOptionsMaxPatchBytes)
        {
            error = "option \"" + option.Id + "\" changes more than 64 KiB";
            return false;
        }
        for (const CoreGameOptionPatch& other : option.Patches)
        {
            if (patches_overlap(patch, other))
            {
                error = "option \"" + option.Id + "\" has patches that overlap";
                return false;
            }
        }
        option.Patches.push_back(std::move(patch));
    }

    // At boot IPL3 checksums ROM 0x1000-0x101000 against the header's CRC
    // words (0x10-0x17) and stops the game on a mismatch, so an option that
    // changes that code has to set new CRCs too.
    const CoreGameOptionPatch bootCode = { 0x1000, std::vector<uint8_t>(0x100000), {} };
    const bool changesBootCode = std::any_of(option.Patches.begin(), option.Patches.end(),
        [&bootCode](const CoreGameOptionPatch& patch) { return patches_overlap(patch, bootCode); });
    if (changesBootCode)
    {
        for (uint32_t crcByte = 0x10; crcByte < 0x18; crcByte++)
        {
            const bool covered = std::any_of(option.Patches.begin(), option.Patches.end(),
                [crcByte](const CoreGameOptionPatch& patch) {
                    return crcByte >= patch.Offset && crcByte < uint64_t(patch.Offset) + patch.Bytes.size();
                });
            if (!covered)
            {
                error = "option \"" + option.Id + "\" changes the boot code (0x1000-0x101000) without "
                    "setting the header CRCs (0x10-0x17), so the game wouldn't start";
                return false;
            }
        }
    }
    return true;
}

static bool parse_pack(const std::string& text, LoadedPack& pack, std::string& error)
{
    JsonValue root;
    if (!JsonReader(text).parse(root, error))
    {
        return false;
    }
    if (root.type != JsonValue::Type::Object)
    {
        error = "isn't a JSON object";
        return false;
    }

    const JsonValue* format = root.find("format");
    if (format == nullptr || format->type != JsonValue::Type::Number || format->text != std::to_string(kPackFormat))
    {
        error = "has an unknown format (this version reads format " + std::to_string(kPackFormat) + ")";
        return false;
    }

    const std::string* id = string_member(root, "id");
    if (id == nullptr || !valid_pack_id(*id))
    {
        error = "has no valid id ([a-z0-9._-], up to 64)";
        return false;
    }
    pack.Id = *id;
    const std::string* game = string_member(root, "game");
    pack.Game = (game != nullptr && valid_label(*game)) ? *game : pack.Id;

    // One MD5, or a list of them.
    const JsonValue* romMd5 = root.find("romMd5");
    std::vector<const JsonValue*> md5Values;
    if (romMd5 != nullptr && romMd5->type == JsonValue::Type::Array)
    {
        for (const JsonValue& value : romMd5->array)
            md5Values.push_back(&value);
    }
    else if (romMd5 != nullptr)
    {
        md5Values.push_back(romMd5);
    }
    for (const JsonValue* value : md5Values)
    {
        if (value->type != JsonValue::Type::String || !valid_md5(value->text))
        {
            error = "has a romMd5 that isn't 32 hex digits";
            return false;
        }
        pack.RomMd5.push_back(upper_md5(value->text));
    }
    if (pack.RomMd5.empty())
    {
        error = "names no ROM (romMd5)";
        return false;
    }

    const JsonValue* options = root.find("options");
    if (options == nullptr || options->type != JsonValue::Type::Array)
    {
        error = "has no options";
        return false;
    }
    for (const JsonValue& entry : options->array)
    {
        LoadedOption option;
        std::string optionError;
        if (!parse_option(entry, option, optionError))
        {
            log_pack(pack.File, "skipped an option: " + optionError);
            continue;
        }
        const bool duplicate = std::any_of(pack.Options.begin(), pack.Options.end(),
            [&option](const LoadedOption& other) { return other.Id == option.Id; });
        if (duplicate)
        {
            log_pack(pack.File, "skipped a second option \"" + option.Id + "\"");
            continue;
        }
        pack.Options.push_back(std::move(option));
    }
    if (pack.Options.empty())
    {
        error = "has no usable options";
        return false;
    }
    return true;
}

static void load_packs_from(const std::filesystem::path& directory, std::vector<LoadedPack>& packs)
{
    std::error_code errorCode;
    if (!std::filesystem::is_directory(directory, errorCode))
    {
        return;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(directory, errorCode), end; !errorCode && it != end; it.increment(errorCode))
    {
        std::error_code fileError;
        if (it->is_regular_file(fileError) && CoreLowerString(it->path().extension().string()) == ".json")
        {
            files.push_back(it->path());
        }
    }
    // a stable order, so the same packs always list the same way
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& file : files)
    {
        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(file, sizeError);
        if (sizeError || size > kMaxPackFileBytes)
        {
            log_pack(file, "skipped: too large or unreadable");
            continue;
        }

        std::ifstream stream(file, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        if (!stream.is_open() || stream.bad())
        {
            log_pack(file, "skipped: unreadable");
            continue;
        }

        LoadedPack pack;
        std::string error;
        pack.File = file;
        if (!parse_pack(buffer.str(), pack, error))
        {
            log_pack(file, "skipped: " + error);
            continue;
        }

        auto existing = std::find_if(packs.begin(), packs.end(),
            [&pack](const LoadedPack& other) { return other.Id == pack.Id; });
        if (existing != packs.end())
        {
            // the user's copy of a pack replaces the shipped one
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "Game options: " + file.filename().string() + " replaces pack \"" + pack.Id + "\"");
            *existing = std::move(pack);
        }
        else
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Info,
                "Game options: loaded \"" + pack.Id + "\" (" + std::to_string(pack.Options.size()) + " options)");
            packs.push_back(std::move(pack));
        }
    }
}

static void ensure_packs_loaded(void)
{
    if (!l_PacksLoaded)
    {
        CoreReloadGameOptionPacks();
    }
}

// The options offered for this ROM, first pack first; a later pack's option
// with an id already taken is left out. Server-only options are a host's
// only with includeServerOnly.
static std::vector<std::pair<const LoadedPack*, const LoadedOption*>> offered_options(const std::string& romMd5,
                                                                                      bool includeServerOnly = false)
{
    std::vector<std::pair<const LoadedPack*, const LoadedOption*>> offered;
    const std::string md5 = upper_md5(romMd5);
    for (const LoadedPack& pack : l_Packs)
    {
        if (std::find(pack.RomMd5.begin(), pack.RomMd5.end(), md5) == pack.RomMd5.end())
        {
            continue;
        }
        for (const LoadedOption& option : pack.Options)
        {
            if (option.ServerOnly && !includeServerOnly)
            {
                continue;
            }
            const bool taken = std::any_of(offered.begin(), offered.end(),
                [&option](const auto& entry) { return entry.second->Id == option.Id; });
            if (!taken)
            {
                offered.emplace_back(&pack, &option);
            }
        }
    }
    return offered;
}

// The ROM file's bytes in .z64 order, read the way CoreOpenRom reads it.
static bool read_rom_image(const std::filesystem::path& rom, std::vector<char>& image)
{
    const std::string extension = CoreLowerString(rom.has_extension() ? rom.extension().string() : std::string());
    if (extension == ".zip" || extension == ".7z")
    {
        std::filesystem::path extracted;
        bool isDisk = false;
        if (!CoreReadArchiveFile(rom, extracted, isDisk, image))
        {
            return false;
        }
        if (isDisk)
        {
            CoreSetError("Game options only apply to cartridge games");
            return false;
        }
    }
    else if (extension == ".d64" || extension == ".ndd")
    {
        CoreSetError("Game options only apply to cartridge games");
        return false;
    }
    else if (!CoreReadFile(rom, image))
    {
        return false;
    }

    if (image.size() < 4)
    {
        CoreSetError("The ROM file isn't an N64 ROM");
        return false;
    }
    const unsigned char* head = reinterpret_cast<const unsigned char*>(image.data());
    if (head[0] == 0x80 && head[1] == 0x37 && head[2] == 0x12 && head[3] == 0x40)
    {
        return true;
    }
    if (head[0] == 0x37 && head[1] == 0x80 && head[2] == 0x40 && head[3] == 0x12 && image.size() % 2 == 0)
    {
        for (size_t i = 0; i < image.size(); i += 2)
        {
            std::swap(image[i], image[i + 1]);
        }
        return true;
    }
    if (head[0] == 0x40 && head[1] == 0x12 && head[2] == 0x37 && head[3] == 0x80 && image.size() % 4 == 0)
    {
        for (size_t i = 0; i < image.size(); i += 4)
        {
            std::swap(image[i], image[i + 3]);
            std::swap(image[i + 1], image[i + 2]);
        }
        return true;
    }
    CoreSetError("The ROM file isn't an N64 ROM");
    return false;
}

static std::string hex_offset(uint32_t offset)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(offset));
    return buffer;
}

// Everything a match may carry, whichever way it arrived.
static bool check_limits(const CoreNetplayGameOptions& options, std::string& error)
{
    if (options.Options.size() > size_t(CoreGameOptionsMaxOptions))
    {
        error = "more than " + std::to_string(CoreGameOptionsMaxOptions) + " game options";
        return false;
    }
    size_t totalBytes = 0;
    for (size_t i = 0; i < options.Options.size(); i++)
    {
        const CoreNetplayGameOption& option = options.Options[i];
        if (!valid_option_id(option.Id) || !valid_label(option.Label))
        {
            error = "a game option with an invalid id or label";
            return false;
        }
        for (size_t j = 0; j < i; j++)
        {
            if (options.Options[j].Id == option.Id)
            {
                error = "game option \"" + option.Id + "\" twice";
                return false;
            }
        }
        if (option.Patches.empty() || option.Patches.size() > size_t(CoreGameOptionMaxPatches))
        {
            error = "game option \"" + option.Id + "\" with no or too many patches";
            return false;
        }
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            if (patch.Bytes.empty() || uint64_t(patch.Offset) + patch.Bytes.size() > kMaxRomBytes ||
                (!patch.Expect.empty() && patch.Expect.size() != patch.Bytes.size()))
            {
                error = "game option \"" + option.Id + "\" with an invalid patch";
                return false;
            }
            totalBytes += patch.Bytes.size();
        }
    }
    if (totalBytes > CoreGameOptionsMaxPatchBytes)
    {
        error = "game options that change more than 64 KiB";
        return false;
    }
    return true;
}

// Two options, or one option's own patches, changing the same bytes, with
// CoreSetError() saying which.
static bool check_overlaps(const CoreNetplayGameOptions& options)
{
    for (size_t i = 0; i < options.Options.size(); i++)
    {
        for (size_t j = 0; j <= i; j++)
        {
            const std::vector<CoreGameOptionPatch>& a = options.Options[i].Patches;
            const std::vector<CoreGameOptionPatch>& b = options.Options[j].Patches;
            for (size_t m = 0; m < a.size(); m++)
            {
                for (size_t n = 0; n < (i == j ? m : b.size()); n++)
                {
                    if (!patches_overlap(a[m], b[n]))
                    {
                        continue;
                    }
                    CoreSetError(i == j
                        ? "\"" + options.Options[i].Label + "\" changes some bytes twice."
                        : "\"" + options.Options[j].Label + "\" and \"" + options.Options[i].Label +
                              "\" change the same part of the game, so they can't be on together.");
                    return false;
                }
            }
        }
    }
    return true;
}

// Every patch's Expect against the ROM file, with CoreSetError() saying why
// one doesn't fit.
static bool check_rom_bytes(const std::filesystem::path& rom, const CoreNetplayGameOptions& options)
{
    std::vector<char> image;
    if (!read_rom_image(rom, image))
    {
        const std::string why = CoreGetError();
        CoreSetError("Couldn't check the game options against the ROM: " + why);
        return false;
    }
    for (const CoreNetplayGameOption& option : options.Options)
    {
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            if (uint64_t(patch.Offset) + patch.Bytes.size() > image.size())
            {
                CoreSetError("\"" + option.Label + "\" doesn't fit this ROM (it changes bytes at " +
                    hex_offset(patch.Offset) + ", past the ROM's end). Is it the version of the game its pack is for?");
                return false;
            }
            if (std::memcmp(image.data() + patch.Offset, patch.Expect.data(), patch.Expect.size()) != 0)
            {
                CoreSetError("\"" + option.Label + "\" doesn't fit this ROM (it expects other bytes at " +
                    hex_offset(patch.Offset) + "). Is it the version of the game its pack is for?");
                return false;
            }
        }
    }
    return true;
}

//
// Exported Functions
//

CORE_EXPORT bool CoreNetplayGameOptions::IsEmpty(void) const
{
    return Options.empty();
}

CORE_EXPORT size_t CoreNetplayGameOptions::TotalPatchBytes(void) const
{
    size_t total = 0;
    for (const CoreNetplayGameOption& option : Options)
    {
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            total += patch.Bytes.size();
        }
    }
    return total;
}

CORE_EXPORT void CoreReloadGameOptionPacks(void)
{
    std::vector<LoadedPack> packs;
    load_packs_from(CoreGetSharedDataDirectory() / "GameOptions", packs);
    load_packs_from(CoreGetUserConfigDirectory() / "GameOptions", packs);
    l_Packs = std::move(packs);
    l_PacksLoaded = true;
}

CORE_EXPORT std::vector<CoreGameOptionInfo> CoreGetGameOptions(const std::string& romMd5, const std::string& language,
                                                               bool includeServerOnly)
{
    std::vector<CoreGameOptionInfo> infos;
    ensure_packs_loaded();

    // "ja_JP" falls back to "ja"
    const std::string exact = CoreLowerString(language);
    const std::string base = exact.substr(0, exact.find_first_of("_-"));
    for (const auto& [pack, option] : offered_options(romMd5, includeServerOnly))
    {
        CoreGameOptionInfo info;
        info.Id = option->Id;
        info.EnglishLabel = option->Label;
        info.EnglishDescription = option->Description;
        info.Label = option->Label;
        info.Description = option->Description;
        info.PackId = pack->Id;
        info.Game = pack->Game;
        info.ServerOnly = option->ServerOnly;

        auto translation = option->Translations.find(exact);
        if (translation == option->Translations.end() && !base.empty())
        {
            translation = option->Translations.find(base);
        }
        if (!exact.empty() && translation != option->Translations.end())
        {
            if (!translation->second.first.empty())
                info.Label = translation->second.first;
            if (!translation->second.second.empty())
                info.Description = translation->second.second;
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

CORE_EXPORT bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                                        const std::vector<std::string>& ids, CoreNetplayGameOptions& options)
{
    options = {};
    if (ids.empty())
    {
        return true;
    }
    ensure_packs_loaded();

    // In pack order, so the same choice always makes the same manifest.
    const auto offered = offered_options(romMd5);
    for (const std::string& id : ids)
    {
        const bool known = std::any_of(offered.begin(), offered.end(),
            [&id](const auto& entry) { return entry.second->Id == id; });
        if (!known)
        {
            CoreSetError("This game has no game option \"" + id + "\". Is its game options pack installed?");
            return false;
        }
    }
    for (const auto& [pack, option] : offered)
    {
        if (std::find(ids.begin(), ids.end(), option->Id) != ids.end())
        {
            options.Options.push_back({ option->Id, option->Label, option->Patches });
        }
    }

    std::string error;
    if (!check_limits(options, error))
    {
        options = {};
        CoreSetError("These game options can't be used together: " + error);
        return false;
    }
    if (!check_overlaps(options) || !check_rom_bytes(rom, options))
    {
        options = {};
        return false;
    }
    return true;
}

CORE_EXPORT bool CoreVerifyGameOptions(const std::filesystem::path& rom, const CoreNetplayGameOptions& options)
{
    if (options.IsEmpty())
    {
        return true;
    }

    std::string error;
    if (!check_limits(options, error))
    {
        CoreSetError("Invalid game options: " + error);
        return false;
    }
    for (const CoreNetplayGameOption& option : options.Options)
    {
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            if (patch.Expect.size() != patch.Bytes.size())
            {
                CoreSetError("\"" + option.Label + "\" doesn't say which bytes it replaces at " +
                    hex_offset(patch.Offset) + ".");
                return false;
            }
        }
    }
    return check_overlaps(options) && check_rom_bytes(rom, options);
}

CORE_EXPORT void CoreSetNetplayGameOptions(const CoreNetplayGameOptions& options)
{
    l_StagedGameOptions = options;
}

CORE_EXPORT void CoreClearNetplayGameOptions(void)
{
    l_StagedGameOptions = {};
}

CORE_EXPORT const CoreNetplayGameOptions& CoreGetNetplayGameOptions(void)
{
    return l_StagedGameOptions;
}

CORE_EXPORT bool CoreSerializeNetplayGameOptions(const CoreNetplayGameOptions& options, std::string& out)
{
    std::string error;
    out.clear();
    if (!check_limits(options, error))
    {
        CoreSetError("CoreSerializeNetplayGameOptions: " + error);
        return false;
    }

    out.assign(kSectionMagic, sizeof(kSectionMagic));
    append_u32(out, static_cast<uint32_t>(options.Options.size()));
    for (const CoreNetplayGameOption& option : options.Options)
    {
        append_u16(out, static_cast<uint16_t>(option.Id.size()));
        out += option.Id;
        append_u16(out, static_cast<uint16_t>(option.Label.size()));
        out += option.Label;
        append_u32(out, static_cast<uint32_t>(option.Patches.size()));
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            append_u32(out, patch.Offset);
            append_u32(out, static_cast<uint32_t>(patch.Bytes.size()));
            out.append(reinterpret_cast<const char*>(patch.Bytes.data()), patch.Bytes.size());
        }
    }
    return true;
}

CORE_EXPORT bool CoreDeserializeNetplayGameOptions(const std::string& in, CoreNetplayGameOptions& options)
{
    size_t offset = sizeof(kSectionMagic);
    uint32_t count = 0;
    size_t totalBytes = 0;

    options = {};
    if (in.size() < sizeof(kSectionMagic) ||
        std::memcmp(in.data(), kSectionMagic, sizeof(kSectionMagic)) != 0 ||
        !read_u32(in, offset, count) || count > uint32_t(CoreGameOptionsMaxOptions))
    {
        CoreSetError("CoreDeserializeNetplayGameOptions: invalid game options");
        return false;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        CoreNetplayGameOption option;
        uint16_t idLength = 0;
        uint16_t labelLength = 0;
        uint32_t patchCount = 0;
        if (!read_u16(in, offset, idLength) || idLength == 0 || idLength > CoreGameOptionMaxIdLength ||
            in.size() - offset < idLength)
        {
            options = {};
            CoreSetError("CoreDeserializeNetplayGameOptions: truncated game option");
            return false;
        }
        option.Id.assign(in, offset, idLength);
        offset += idLength;
        if (!read_u16(in, offset, labelLength) || labelLength == 0 || labelLength > CoreGameOptionMaxLabelLength ||
            in.size() - offset < labelLength)
        {
            options = {};
            CoreSetError("CoreDeserializeNetplayGameOptions: truncated game option");
            return false;
        }
        option.Label.assign(in, offset, labelLength);
        offset += labelLength;
        if (!read_u32(in, offset, patchCount) || patchCount == 0 || patchCount > uint32_t(CoreGameOptionMaxPatches))
        {
            options = {};
            CoreSetError("CoreDeserializeNetplayGameOptions: invalid patch count");
            return false;
        }

        for (uint32_t j = 0; j < patchCount; j++)
        {
            CoreGameOptionPatch patch;
            uint32_t length = 0;
            if (!read_u32(in, offset, patch.Offset) || !read_u32(in, offset, length) || length == 0 ||
                length > CoreGameOptionsMaxPatchBytes - totalBytes || in.size() - offset < length ||
                uint64_t(patch.Offset) + length > kMaxRomBytes)
            {
                options = {};
                CoreSetError("CoreDeserializeNetplayGameOptions: invalid patch");
                return false;
            }
            patch.Bytes.assign(in.begin() + offset, in.begin() + offset + length);
            offset += length;
            totalBytes += length;
            option.Patches.push_back(std::move(patch));
        }
        options.Options.push_back(std::move(option));
    }

    std::string error;
    if (offset != in.size() || !check_limits(options, error))
    {
        options = {};
        CoreSetError(offset != in.size() ? "CoreDeserializeNetplayGameOptions: trailing data"
                                         : "CoreDeserializeNetplayGameOptions: " + error);
        return false;
    }
    return true;
}

bool CoreApplyNetplayGameOptions(bool enabled)
{
    std::string error;
    CoreRomType type;

    if (!enabled || l_StagedGameOptions.IsEmpty())
    {
        return true;
    }
    if (!m64p::Core.IsHooked())
    {
        return false;
    }
    if (!CoreGetRomType(type) || type != CoreRomType::Cartridge)
    {
        CoreSetError("This match uses game options, which only apply to cartridge games.");
        return false;
    }

    std::vector<m64p_rom_patch> patches;
    std::string names;
    for (const CoreNetplayGameOption& option : l_StagedGameOptions.Options)
    {
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            patches.push_back({ patch.Offset, static_cast<unsigned int>(patch.Bytes.size()), patch.Bytes.data(),
                patch.Expect.empty() ? nullptr : patch.Expect.data() });
        }
        names += (names.empty() ? "" : ", ") + option.Label;
    }

    const m64p_error ret = m64p::Core.DoCommand(M64CMD_ROM_PATCH, static_cast<int>(patches.size()), patches.data());
    if (ret != M64ERR_SUCCESS)
    {
        if (ret == M64ERR_INPUT_INVALID)
            error = "This match's game options (" + names + ") don't fit this ROM: a change lies past its end.";
        else if (ret == M64ERR_INCOMPATIBLE)
            error = "This match's game options (" + names + ") don't match this ROM. Is it the version of the game they're for?";
        else
            error = "CoreApplyNetplayGameOptions m64p::Core.DoCommand(M64CMD_ROM_PATCH) Failed: " + std::string(m64p::Core.ErrorMessage(ret));
        CoreSetError(error);
        return false;
    }

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        "Game options on: " + names + " (" + std::to_string(patches.size()) + " patches, " +
        std::to_string(l_StagedGameOptions.TotalPatchBytes()) + " bytes)");
    return true;
}
