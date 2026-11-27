#define CORE_INTERNAL
#include "GameOptions.hpp"
#include "StageList.hpp"
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
#include <iterator>
#include <map>
#include <memory>
#include <set>
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

// language code -> (label, description), either may be empty
using Translations = std::map<std::string, std::pair<std::string, std::string>>;

// An option's "stageList" block: the list's rules and stages in English, the
// translations of its headings and presets by id, and where it goes.
struct LoadedStageList
{
    CoreStageListInfo Info;
    std::map<std::string, Translations> GroupText;
    std::map<std::string, Translations> KindText;
    std::map<std::string, Translations> PresetText;
    CoreStageListLayout Layout;
};

struct LoadedOption
{
    std::string Id;
    std::string Label;
    std::string Description;
    Translations Text;
    // With a stage list, its three ranges are in here too, holding the
    // unpatched bytes: the list's bytes replace them when it resolves.
    std::vector<CoreGameOptionPatch> Patches;
    std::string Group;
    // Ids of other options in the same pack. Conflicts are made symmetric
    // when the pack loads.
    std::vector<std::string> Requires;
    std::vector<std::string> Conflicts;
    bool BlankSave = false;
    bool Hidden = false;
    std::shared_ptr<const LoadedStageList> StageList;
    // Checked once every option is parsed (link_options).
    std::vector<CoreGameOptionAlias> Aliases;
};

struct LoadedGroup
{
    std::string Id;
    std::string Label;
    Translations Text;
};

struct LoadedPreset
{
    std::string Id;
    std::string Label;
    std::string Description;
    Translations Text;
    // In pack order.
    std::vector<std::string> Options;
    // Stage list option id -> the share code of the stage list preset named.
    std::map<std::string, std::string> Lists;
    // Fewest players first, each in pack order; WithPlayersText holds each
    // entry's translations.
    std::vector<CoreGameOptionPresetExtra> WithPlayers;
    std::vector<Translations> WithPlayersText;
    std::vector<std::string> Aliases;
};

struct LoadedPack
{
    std::string Id;
    std::string Game;
    std::vector<std::string> RomMd5; // upper case
    std::vector<LoadedOption> Options;
    std::vector<LoadedGroup> Groups;
    std::vector<LoadedPreset> Presets;
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

// An entry's "translations": { "ja": { "label": ..., "description": ... } }.
// Bad entries are left out; the English text stands in for them.
static void parse_translations(const JsonValue& json, Translations& out)
{
    const JsonValue* translations = json.find("translations");
    if (translations == nullptr || translations->type != JsonValue::Type::Object)
    {
        return;
    }
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
        out[CoreLowerString(language)] = std::move(text);
    }
}

// The translation for language ("ja_JP" falls back to "ja"), if there is one.
static const std::pair<std::string, std::string>* find_translation(const Translations& text, const std::string& language)
{
    const std::string exact = CoreLowerString(language);
    if (exact.empty())
    {
        return nullptr;
    }
    auto translation = text.find(exact);
    if (translation == text.end())
    {
        translation = text.find(exact.substr(0, exact.find_first_of("_-")));
    }
    return translation != text.end() ? &translation->second : nullptr;
}

// "requires" or "conflicts": option ids. Anything that can't be an id is
// logged and left out; whether the ids name options is checked later.
static std::vector<std::string> parse_id_list(const JsonValue& json, std::string_view key, const std::string& optionId,
                                              const std::filesystem::path& file)
{
    std::vector<std::string> ids;
    const JsonValue* list = json.find(key);
    if (list == nullptr)
    {
        return ids;
    }
    if (list->type != JsonValue::Type::Array)
    {
        log_pack(file, "option \"" + optionId + "\": " + std::string(key) + " isn't a list, ignored");
        return ids;
    }
    for (const JsonValue& entry : list->array)
    {
        if (entry.type != JsonValue::Type::String || !valid_option_id(entry.text))
        {
            log_pack(file, "option \"" + optionId + "\": ignored a " + std::string(key) + " entry that isn't an option id");
            continue;
        }
        if (std::find(ids.begin(), ids.end(), entry.text) == ids.end())
        {
            ids.push_back(entry.text);
        }
    }
    return ids;
}

// An option's "aliases": old ids, each "id" or { "id", "stageList", "with" }.
// Their shape is checked here, what they name once every option is parsed
// (link_options); a bad entry is logged and left out.
static std::vector<CoreGameOptionAlias> parse_option_aliases(const JsonValue& json, const std::string& optionId,
                                                             const std::filesystem::path& file)
{
    std::vector<CoreGameOptionAlias> aliases;
    const JsonValue* list = json.find("aliases");
    if (list == nullptr)
        return aliases;
    if (list->type != JsonValue::Type::Array)
    {
        log_pack(file, "option \"" + optionId + "\": aliases isn't a list, ignored");
        return aliases;
    }
    for (const JsonValue& entry : list->array)
    {
        CoreGameOptionAlias alias;
        const std::string* id = entry.type == JsonValue::Type::String ? &entry.text : string_member(entry, "id");
        bool ok = id != nullptr && valid_option_id(*id) && *id != optionId &&
            (entry.type == JsonValue::Type::String || entry.type == JsonValue::Type::Object);
        if (ok && entry.type == JsonValue::Type::Object)
        {
            if (const JsonValue* stageList = entry.find("stageList"))
            {
                ok = stageList->type == JsonValue::Type::String && valid_option_id(stageList->text);
                if (ok)
                    alias.StageListPreset = stageList->text;
            }
            if (const JsonValue* with = entry.find("with"); ok && with != nullptr)
            {
                ok = with->type == JsonValue::Type::Array;
                for (size_t i = 0; ok && i < with->array.size(); i++)
                {
                    const JsonValue& other = with->array[i];
                    ok = other.type == JsonValue::Type::String && valid_option_id(other.text);
                    if (ok && std::find(alias.With.begin(), alias.With.end(), other.text) == alias.With.end())
                        alias.With.push_back(other.text);
                }
            }
        }
        if (!ok)
        {
            log_pack(file, "option \"" + optionId + "\": ignored an alias that isn't valid");
            continue;
        }
        alias.Id = *id;
        aliases.push_back(std::move(alias));
    }
    return aliases;
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
    parse_translations(json, option.Text);

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

// A whole number from min to max, written as a JSON number.
static bool parse_int(const JsonValue* value, int min, int max, int& out)
{
    if (value == nullptr || value->type != JsonValue::Type::Number)
        return false;
    int parsed = 0;
    const std::string& text = value->text;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed < min || parsed > max)
        return false;
    out = parsed;
    return true;
}

// A stage id: "0x06", or a number.
static bool parse_stage_id(const JsonValue* value, int& out)
{
    uint32_t parsed = 0;
    if (value == nullptr || !parse_offset(*value, parsed) || parsed > 0xFF)
        return false;
    out = int(parsed);
    return true;
}

// { "offset": ..., "expect": ... } of a stage list's range.
static bool parse_stage_list_range(const JsonValue& json, const char* key, uint32_t& offset,
                                   std::vector<uint8_t>& expect, std::string& error)
{
    const JsonValue* range = json.find(key);
    const JsonValue* at = range != nullptr && range->type == JsonValue::Type::Object ? range->find("offset") : nullptr;
    const std::string* bytes = range != nullptr ? string_member(*range, "expect") : nullptr;
    if (at == nullptr || !parse_offset(*at, offset) || bytes == nullptr || !parse_hex_bytes(*bytes, expect) ||
        uint64_t(offset) + expect.size() > kMaxRomBytes)
    {
        error = std::string("its ") + key + " has no valid offset and expect";
        return false;
    }
    return true;
}

// [[stage id, mode], ...]
static bool parse_stage_entries(const JsonValue* list, std::vector<CoreStageListEntry>& out)
{
    out.clear();
    if (list == nullptr || list->type != JsonValue::Type::Array)
        return false;
    for (const JsonValue& entry : list->array)
    {
        int id = 0, mode = 0;
        if (entry.type != JsonValue::Type::Array || entry.array.size() != 2 || !parse_stage_id(&entry.array[0], id) ||
            !parse_int(&entry.array[1], 0, 3, mode))
            return false;
        out.push_back({ uint8_t(id), uint8_t(mode) });
    }
    return true;
}

// An option's "stageList": the stages a host can list, the rules of the
// stage select it fills, its share codes and where its icons are. The list's
// three ranges are added to the option's patches with their unpatched bytes,
// so the usual overlap and conflict checks cover them.
static bool parse_stage_list(const JsonValue& json, LoadedOption& option, const std::filesystem::path& file,
                             std::string& error)
{
    const JsonValue* block = json.find("stageList");
    if (block == nullptr)
        return true;
    const std::string where = "option \"" + option.Id + "\": stage list: ";
    if (block->type != JsonValue::Type::Object)
    {
        error = where + "isn't an object";
        return false;
    }
    auto list = std::make_shared<LoadedStageList>();
    CoreStageListInfo& info = list->Info;
    CoreStageListLayout& layout = list->Layout;
    info.OptionId = option.Id;

    // Share codes: the prefix can't start with a code's first character,
    // which is always a digit.
    const std::string* prefix = string_member(*block, "codePrefix");
    const std::string* salt = string_member(*block, "codeSalt");
    const auto upperLetters = [](const std::string& s) {
        return !s.empty() && s.size() <= 8 && std::all_of(s.begin(), s.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    };
    const auto printable = [](const std::string& s) {
        return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
    };
    if (prefix == nullptr || !upperLetters(*prefix) || salt == nullptr || !printable(*salt))
    {
        error = where + "codePrefix needs 1-8 letters A-Z and codeSalt 1-64 printable ASCII characters";
        return false;
    }
    info.CodePrefix = *prefix;
    info.CodeSalt = *salt;

    const JsonValue* table = block->find("table");
    int random = 0;
    if (!parse_stage_list_range(*block, "table", layout.TableOffset, layout.TableExpect, error) ||
        !parse_stage_list_range(*block, "pages", layout.PagesOffset, layout.PagesExpect, error) ||
        !parse_stage_list_range(*block, "modes", layout.ModesOffset, layout.ModesExpect, error))
    {
        error = where + error;
        return false;
    }
    if (!parse_int(table->find("pageIcons"), 1, 64, info.PageIcons) ||
        !parse_int(table->find("pageStages"), 1, info.PageIcons, info.PageStages) ||
        !parse_stage_id(table->find("random"), random) || layout.TableExpect.size() % size_t(info.PageIcons) != 0)
    {
        error = where + "its table needs pageIcons (1-64), pageStages (1 to pageIcons), a random stage id, "
            "and room for whole pages";
        return false;
    }
    info.RandomStage = uint8_t(random);
    if (!parse_int(table->find("pageColumns"), 1, info.PageIcons, info.PageColumns))
        info.PageColumns = info.PageIcons;
    info.MaxPages = int(layout.TableExpect.size() / size_t(info.PageIcons));
    const int fits = info.PageStages * info.MaxPages;
    // The code counts stages in 7 bits.
    if (!parse_int(block->find("maxStages"), 1, std::min(fits, 127), info.MaxStages))
    {
        error = where + "maxStages must be 1 to " + std::to_string(std::min(fits, 127));
        return false;
    }
    if (layout.PagesExpect.size() > 4 || (layout.PagesExpect.size() < 4 && info.MaxPages >= (1 << (8 * layout.PagesExpect.size()))))
    {
        error = where + "its page count is too narrow";
        return false;
    }

    if (const JsonValue* icons = block->find("icons"); icons != nullptr && icons->type == JsonValue::Type::Object)
    {
        int files = 0;
        const JsonValue* fileTable = icons->find("fileTable");
        const JsonValue* iconFile = icons->find("file");
        const JsonValue* sprites = icons->find("sprites");
        const JsonValue* randomSprite = icons->find("randomSprite");
        const JsonValue* fileBases = icons->find("fileBases");
        if (fileTable == nullptr || !parse_offset(*fileTable, layout.FileTable) || !parse_int(icons->find("files"), 1, 0xFFFF, files) ||
            iconFile == nullptr || !parse_offset(*iconFile, layout.IconFile) || sprites == nullptr ||
            !parse_offset(*sprites, layout.Sprites) || randomSprite == nullptr ||
            !parse_offset(*randomSprite, layout.RandomSprite) || fileBases == nullptr ||
            !parse_offset(*fileBases, layout.FileBases))
        {
            log_pack(file, where + "icons isn't complete, ignored (the list shows names only)");
            layout.FileCount = 0;
        }
        else
        {
            layout.FileCount = uint32_t(files);
        }
    }

    // Headings: groups, and the kinds of variant.
    if (const JsonValue* groups = block->find("groups"); groups != nullptr && groups->type == JsonValue::Type::Array)
    {
        for (const JsonValue& entry : groups->array)
        {
            const std::string* id = string_member(entry, "id");
            const std::string* label = string_member(entry, "label");
            if (id == nullptr || !valid_option_id(*id) || label == nullptr || !valid_label(*label))
                continue;
            const bool taken = std::any_of(info.Groups.begin(), info.Groups.end(),
                [id](const CoreStageListText& g) { return g.Id == *id; });
            if (taken)
                continue;
            info.Groups.push_back({ *id, *label, std::string() });
            parse_translations(entry, list->GroupText[*id]);
        }
    }
    if (const JsonValue* kinds = block->find("variantKinds"); kinds != nullptr && kinds->type == JsonValue::Type::Object)
    {
        for (const auto& [id, entry] : kinds->object)
        {
            const std::string* label = string_member(entry, "label");
            if (!valid_option_id(id) || label == nullptr || !valid_label(*label))
                continue;
            info.Kinds.push_back({ id, *label, std::string() });
            parse_translations(entry, list->KindText[id]);
        }
    }

    // The stages, as the editor lists them.
    const JsonValue* stages = block->find("stages");
    if (stages == nullptr || stages->type != JsonValue::Type::Array || stages->array.empty())
    {
        error = where + "has no stages";
        return false;
    }
    for (const JsonValue& entry : stages->array)
    {
        CoreStageListStage s;
        int id = 0;
        const std::string* name = entry.type == JsonValue::Type::Object ? string_member(entry, "name") : nullptr;
        if (!parse_stage_id(entry.find("id"), id) || name == nullptr || !valid_label(*name) || id == random ||
            size_t(id) >= layout.ModesExpect.size() || info.stage(id) != nullptr)
        {
            error = where + "a stage has no valid id or name, or its id is Random's, past the modes table or taken";
            return false;
        }
        s.Id = uint8_t(id);
        s.Name = *name;
        if (const JsonValue* base = entry.find("variantOf"))
        {
            int baseId = 0;
            if (!parse_stage_id(base, baseId) || baseId == id)
            {
                error = where + "stage \"" + s.Name + "\" has an invalid variantOf";
                return false;
            }
            s.VariantOf = baseId;
            const std::string* shortName = string_member(entry, "short");
            s.Short = shortName != nullptr && valid_label(*shortName) ? *shortName : s.Name;
            if (const std::string* kind = string_member(entry, "kind"))
                s.Kind = *kind;
        }
        else if (const std::string* group = string_member(entry, "group"))
        {
            s.Group = *group;
        }
        const JsonValue* hazards = entry.find("hazards");
        const JsonValue* movement = entry.find("movement");
        s.Hazards = hazards != nullptr && hazards->type == JsonValue::Type::Bool && hazards->boolean;
        s.Movement = movement != nullptr && movement->type == JsonValue::Type::Bool && movement->boolean;
        info.Stages.push_back(std::move(s));
    }
    for (CoreStageListStage& s : info.Stages)
    {
        if (s.VariantOf >= 0)
        {
            const CoreStageListStage* base = info.stage(s.VariantOf);
            if (base == nullptr || base->VariantOf >= 0)
            {
                error = where + "stage \"" + s.Name + "\" is a variant of a stage that isn't a base stage on the list";
                return false;
            }
            if (!s.Kind.empty() && std::none_of(info.Kinds.begin(), info.Kinds.end(),
                    [&s](const CoreStageListText& k) { return k.Id == s.Kind; }))
                s.Kind.clear();
        }
        else if (!s.Group.empty() && std::none_of(info.Groups.begin(), info.Groups.end(),
                     [&s](const CoreStageListText& g) { return g.Id == s.Group; }))
        {
            s.Group.clear();
        }
    }

    // Ready-made lists with names; one that breaks the rules is left out.
    // "user-" ids are a host's saved lists in a room.
    if (const JsonValue* presets = block->find("presets"); presets != nullptr && presets->type == JsonValue::Type::Array)
    {
        for (const JsonValue& entry : presets->array)
        {
            CoreStageListPreset preset;
            const std::string* id = string_member(entry, "id");
            const std::string* label = string_member(entry, "label");
            if (id == nullptr || !valid_option_id(*id) || id->rfind("user-", 0) == 0 || label == nullptr ||
                !valid_label(*label) || !parse_stage_entries(entry.find("stages"), preset.Stages) ||
                CoreNormalizeStageList(info, preset.Stages) != CoreStageListProblem::None)
            {
                log_pack(file, where + "skipped a preset list that isn't valid");
                continue;
            }
            const auto same = std::find_if(info.Presets.begin(), info.Presets.end(), [&](const CoreStageListPreset& other) {
                return other.Id == *id || other.Stages == preset.Stages;
            });
            if (same != info.Presets.end())
            {
                log_pack(file, where + "skipped preset list \"" + *id + "\": " +
                    (same->Id == *id ? std::string("a second one with that id") : "the same list as \"" + same->Id + "\""));
                continue;
            }
            preset.Id = *id;
            preset.Label = *label;
            preset.EnglishLabel = *label;
            if (const std::string* description = string_member(entry, "description"))
                preset.Description = *description;
            parse_translations(entry, list->PresetText[*id]);
            info.Presets.push_back(std::move(preset));
        }
    }

    // The list's ranges, holding the unpatched bytes until a list resolves.
    const std::vector<CoreGameOptionPatch> ranges = {
        { layout.TableOffset, layout.TableExpect, layout.TableExpect },
        { layout.PagesOffset, layout.PagesExpect, layout.PagesExpect },
        { layout.ModesOffset, layout.ModesExpect, layout.ModesExpect },
    };
    size_t totalBytes = 0;
    for (const CoreGameOptionPatch& patch : option.Patches)
        totalBytes += patch.Bytes.size();
    for (const CoreGameOptionPatch& range : ranges)
    {
        for (const CoreGameOptionPatch& other : option.Patches)
        {
            if (patches_overlap(range, other))
            {
                error = where + "its ranges overlap each other or the option's patches";
                return false;
            }
        }
        if (range.Offset < 0x101000 && uint64_t(range.Offset) + range.Bytes.size() > 0x1000)
        {
            error = where + "its ranges reach into the boot code (0x1000-0x101000)";
            return false;
        }
        totalBytes += range.Bytes.size();
        option.Patches.push_back(range);
    }
    if (option.Patches.size() > size_t(CoreGameOptionMaxPatches) || totalBytes > CoreGameOptionsMaxPatchBytes)
    {
        error = where + "with its ranges the option has too many patches or bytes";
        return false;
    }
    std::sort(option.Patches.begin(), option.Patches.end(),
        [](const CoreGameOptionPatch& a, const CoreGameOptionPatch& b) { return a.Offset < b.Offset; });
    option.StageList = std::move(list);
    return true;
}

// The stage list in a language: its headings' and presets' translations.
static CoreStageListInfo stage_list_in(const LoadedStageList& list, const std::string& language)
{
    CoreStageListInfo info = list.Info;
    const auto translate = [&language](const std::map<std::string, Translations>& text, CoreStageListText& entry) {
        const auto it = text.find(entry.Id);
        const auto* translation = it != text.end() ? find_translation(it->second, language) : nullptr;
        if (translation != nullptr && !translation->first.empty())
            entry.Label = translation->first;
        if (translation != nullptr && !translation->second.empty())
            entry.Description = translation->second;
    };
    for (CoreStageListText& group : info.Groups)
        translate(list.GroupText, group);
    for (CoreStageListText& kind : info.Kinds)
        translate(list.KindText, kind);
    for (CoreStageListPreset& preset : info.Presets)
    {
        CoreStageListText text{ preset.Id, preset.Label, preset.Description };
        translate(list.PresetText, text);
        preset.Label = text.Label;
        preset.Description = text.Description;
    }
    return info;
}

static bool contains(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

static bool options_overlap(const LoadedOption& a, const LoadedOption& b)
{
    for (const CoreGameOptionPatch& patchA : a.Patches)
    {
        for (const CoreGameOptionPatch& patchB : b.Patches)
        {
            if (patches_overlap(patchA, patchB))
            {
                return true;
            }
        }
    }
    return false;
}

static bool options_conflict(const LoadedOption& a, const LoadedOption& b)
{
    return contains(a.Conflicts, b.Id) || contains(b.Conflicts, a.Id);
}

// The rules for options that are on together: at most CoreGameOptionsMaxOptions
// of them and 64 KiB of changes in all, everything each one requires on too,
// and no two that conflict or change the same bytes. labels names options
// that aren't in set, for the message. The error is a sentence for the host.
static bool check_option_set(const std::vector<const LoadedOption*>& set,
                             const std::vector<const LoadedOption*>& labels, std::string& error)
{
    const auto labelOf = [&](const std::string& id) {
        for (const LoadedOption* option : labels)
        {
            if (option->Id == id)
                return option->Label;
        }
        return id;
    };

    if (set.size() > size_t(CoreGameOptionsMaxOptions))
    {
        error = "At most " + std::to_string(CoreGameOptionsMaxOptions) + " game options can be on at once.";
        return false;
    }
    size_t totalBytes = 0;
    for (size_t i = 0; i < set.size(); i++)
    {
        const LoadedOption& option = *set[i];
        for (const std::string& required : option.Requires)
        {
            const bool on = std::any_of(set.begin(), set.end(),
                [&required](const LoadedOption* other) { return other->Id == required; });
            if (!on)
            {
                error = "\"" + option.Label + "\" needs \"" + labelOf(required) + "\" on too.";
                return false;
            }
        }
        for (size_t j = 0; j < i; j++)
        {
            const LoadedOption& other = *set[j];
            if (options_conflict(option, other))
            {
                error = "\"" + other.Label + "\" and \"" + option.Label + "\" can't be on together.";
                return false;
            }
            if (options_overlap(option, other))
            {
                error = "\"" + other.Label + "\" and \"" + option.Label +
                    "\" change the same part of the game, so they can't be on together.";
                return false;
            }
        }
        for (const CoreGameOptionPatch& patch : option.Patches)
        {
            totalBytes += patch.Bytes.size();
        }
    }
    if (totalBytes > CoreGameOptionsMaxPatchBytes)
    {
        error = "Together these game options change more than 64 KiB of the game.";
        return false;
    }
    return true;
}

static bool parse_group(const JsonValue& json, LoadedGroup& group, std::string& error)
{
    const std::string* id = json.type == JsonValue::Type::Object ? string_member(json, "id") : nullptr;
    const std::string* label = json.type == JsonValue::Type::Object ? string_member(json, "label") : nullptr;
    if (id == nullptr || !valid_option_id(*id))
    {
        error = "a group has no valid id ([a-z0-9-], up to 32)";
        return false;
    }
    if (label == nullptr || !valid_label(*label))
    {
        error = "group \"" + *id + "\" has no valid label";
        return false;
    }
    group.Id = *id;
    group.Label = *label;
    parse_translations(json, group.Text);
    return true;
}

// The options' groups, requires and conflicts, once every option is parsed.
// options is the file's whole list, usable entries or not.
static void link_options(const JsonValue& options, LoadedPack& pack)
{
    std::vector<std::string> declared;
    for (const JsonValue& entry : options.array)
    {
        if (const std::string* id = string_member(entry, "id"))
            declared.push_back(*id);
    }
    const auto loaded = [&pack](const std::string& id) -> LoadedOption* {
        for (LoadedOption& option : pack.Options)
        {
            if (option.Id == id)
                return &option;
        }
        return nullptr;
    };

    for (LoadedOption& option : pack.Options)
    {
        const bool knownGroup = std::any_of(pack.Groups.begin(), pack.Groups.end(),
            [&option](const LoadedGroup& group) { return group.Id == option.Group; });
        if (!option.Group.empty() && !knownGroup)
        {
            log_pack(pack.File, "option \"" + option.Id + "\" names group \"" + option.Group + "\", which the pack doesn't list");
            option.Group.clear();
        }
        for (std::vector<std::string>* list : { &option.Requires, &option.Conflicts })
        {
            const std::string what = list == &option.Requires ? "requires" : "conflicts";
            list->erase(std::remove_if(list->begin(), list->end(), [&](const std::string& id) {
                if (id == option.Id || !contains(declared, id))
                {
                    log_pack(pack.File, "option \"" + option.Id + "\": ignored " + what + " \"" + id + "\" (" +
                        (id == option.Id ? std::string("itself") : std::string("no such option")) + ")");
                    return true;
                }
                // A skipped option can't be on, so there's nothing to conflict with.
                return list == &option.Conflicts && loaded(id) == nullptr;
            }), list->end());
        }
    }

    // Options that change the same bytes can't be on together, declared or not.
    for (size_t i = 0; i < pack.Options.size(); i++)
    {
        for (size_t j = 0; j < i; j++)
        {
            LoadedOption& a = pack.Options[i];
            LoadedOption& b = pack.Options[j];
            if (!options_conflict(a, b) && options_overlap(a, b))
            {
                log_pack(pack.File, "options \"" + b.Id + "\" and \"" + a.Id +
                    "\" change the same bytes but don't list each other in conflicts; treating them as conflicting");
                a.Conflicts.push_back(b.Id);
            }
        }
    }

    // An option that needs one that was skipped, or needs two that conflict,
    // can never be on. Dropping it can strand others, so repeat until stable.
    for (bool dropped = true; dropped;)
    {
        dropped = false;
        for (auto it = pack.Options.begin(); it != pack.Options.end(); ++it)
        {
            std::vector<const LoadedOption*> needed = { &*it };
            std::string reason;
            for (size_t k = 0; k < needed.size() && reason.empty(); k++)
            {
                for (const std::string& id : needed[k]->Requires)
                {
                    const LoadedOption* required = loaded(id);
                    if (required == nullptr)
                    {
                        reason = "it needs \"" + id + "\", which was skipped";
                        break;
                    }
                    if (std::find(needed.begin(), needed.end(), required) == needed.end())
                        needed.push_back(required);
                }
            }
            for (size_t a = 0; a < needed.size() && reason.empty(); a++)
            {
                for (size_t b = 0; b < a && reason.empty(); b++)
                {
                    if (options_conflict(*needed[a], *needed[b]))
                        reason = "it needs \"" + needed[a]->Id + "\" and \"" + needed[b]->Id + "\" on together, which conflict";
                }
            }
            if (!reason.empty())
            {
                log_pack(pack.File, "skipped option \"" + it->Id + "\": " + reason);
                pack.Options.erase(it);
                dropped = true;
                break;
            }
        }
    }

    // Conflicts go both ways.
    for (LoadedOption& option : pack.Options)
    {
        for (const std::string& id : option.Conflicts)
        {
            LoadedOption* other = loaded(id);
            if (other != nullptr && !contains(other->Conflicts, option.Id))
                other->Conflicts.push_back(option.Id);
        }
    }

    // Aliases. One that is an option's id (skipped or not) or another
    // alias's, names a preset list the option lacks, or makes a set that
    // can't be on is left out: the id then means nothing.
    std::vector<const LoadedOption*> all;
    for (const LoadedOption& option : pack.Options)
        all.push_back(&option);
    std::vector<std::string> aliasIds;
    for (LoadedOption& option : pack.Options)
    {
        std::vector<CoreGameOptionAlias> kept;
        for (CoreGameOptionAlias& alias : option.Aliases)
        {
            std::string why;
            if (contains(declared, alias.Id))
                why = "it's the id of an option";
            else if (contains(aliasIds, alias.Id))
                why = "another option has it";
            else if (option.StageList && option.StageList->Info.preset(alias.StageListPreset) == nullptr)
                why = "it names none of the option's preset lists (stageList)";
            else if (!option.StageList && !alias.StageListPreset.empty())
                why = "the option takes no stage list";
            std::vector<const LoadedOption*> set = { &option };
            for (const std::string& id : alias.With)
            {
                const LoadedOption* other = loaded(id);
                if (why.empty() && (other == nullptr || other == &option || other->StageList))
                    why = "with names \"" + id + "\", which isn't another usable option without a stage list";
                else if (other != nullptr)
                    set.push_back(other);
            }
            if (why.empty() && !check_option_set(set, all, why))
                why = "with it the options can't be on together: " + why;
            if (!why.empty())
            {
                log_pack(pack.File, "option \"" + option.Id + "\": ignored alias \"" + alias.Id + "\": " + why);
                continue;
            }
            std::vector<std::string> with;
            for (const LoadedOption& other : pack.Options)
            {
                if (contains(alias.With, other.Id))
                    with.push_back(other.Id);
            }
            alias.With = std::move(with);
            aliasIds.push_back(alias.Id);
            kept.push_back(std::move(alias));
        }
        option.Aliases = std::move(kept);
    }
}

static bool parse_preset(const JsonValue& json, const LoadedPack& pack, LoadedPreset& preset, std::string& error)
{
    const std::string* id = json.type == JsonValue::Type::Object ? string_member(json, "id") : nullptr;
    if (id == nullptr || !valid_option_id(*id))
    {
        error = "a preset has no valid id ([a-z0-9-], up to 32)";
        return false;
    }
    preset.Id = *id;
    const std::string* label = string_member(json, "label");
    if (label == nullptr || !valid_label(*label))
    {
        error = "preset \"" + preset.Id + "\" has no valid label";
        return false;
    }
    preset.Label = *label;
    if (const std::string* description = string_member(json, "description"))
        preset.Description = *description;
    parse_translations(json, preset.Text);

    const JsonValue* options = json.find("options");
    if (options == nullptr || options->type != JsonValue::Type::Array || options->array.empty())
    {
        error = "preset \"" + preset.Id + "\" turns no options on (the unchanged game needs no preset)";
        return false;
    }
    const auto optionOf = [&pack](const std::string& id) -> const LoadedOption* {
        for (const LoadedOption& option : pack.Options)
        {
            if (option.Id == id)
                return &option;
        }
        return nullptr;
    };
    // "stageLists": { "<option id>": "<one of its preset lists>" }, the list
    // each stage list option the preset turns on gets.
    std::map<std::string, std::string> listPresets;
    if (const JsonValue* stageLists = json.find("stageLists"); stageLists != nullptr)
    {
        if (stageLists->type != JsonValue::Type::Object)
        {
            error = "preset \"" + preset.Id + "\": stageLists isn't an object";
            return false;
        }
        for (const auto& [optionId, presetId] : stageLists->object)
        {
            const LoadedOption* option = optionOf(optionId);
            const CoreStageListPreset* list = option != nullptr && option->StageList != nullptr &&
                presetId.type == JsonValue::Type::String ? option->StageList->Info.preset(presetId.text) : nullptr;
            if (list == nullptr)
            {
                error = "preset \"" + preset.Id + "\": stageLists names \"" + optionId +
                    "\" with something that isn't one of its preset lists";
                return false;
            }
            listPresets[optionId] = presetId.text;
            preset.Lists[optionId] = CoreEncodeStageList(option->StageList->Info, list->Stages);
        }
    }
    std::vector<std::string> ids;
    for (const JsonValue& entry : options->array)
    {
        const LoadedOption* option = entry.type == JsonValue::Type::String ? optionOf(entry.text) : nullptr;
        if (option == nullptr)
        {
            error = "preset \"" + preset.Id + "\" names \"" +
                (entry.type == JsonValue::Type::String ? entry.text : std::string("?")) +
                "\", which isn't a usable option of this pack";
            return false;
        }
        // A stage list option needs its list, one of the option's preset lists.
        if (option->StageList && listPresets.count(entry.text) == 0)
        {
            error = "preset \"" + preset.Id + "\" names \"" + entry.text +
                "\", which takes a stage list, without one in stageLists";
            return false;
        }
        if (!contains(ids, entry.text))
            ids.push_back(entry.text);
    }
    for (const auto& [optionId, list] : listPresets)
    {
        if (!contains(ids, optionId))
        {
            error = "preset \"" + preset.Id + "\": stageLists names \"" + optionId + "\", which it doesn't turn on";
            return false;
        }
    }

    std::vector<const LoadedOption*> set;
    std::vector<const LoadedOption*> all;
    for (const LoadedOption& option : pack.Options)
    {
        all.push_back(&option);
        if (contains(ids, option.Id))
        {
            set.push_back(&option);
            preset.Options.push_back(option.Id);
        }
    }
    std::string why;
    if (!check_option_set(set, all, why))
    {
        error = "preset \"" + preset.Id + "\" can't be used: " + why;
        return false;
    }

    // "withPlayers": [ { "minPlayers": 4, "options": [...] } ], what it turns
    // on as well in bigger matches. Each player count's set has to be one
    // that can be on together, or the preset would fail at match start.
    if (const JsonValue* withPlayers = json.find("withPlayers"); withPlayers != nullptr)
    {
        if (withPlayers->type != JsonValue::Type::Array)
        {
            error = "preset \"" + preset.Id + "\": withPlayers isn't a list";
            return false;
        }
        std::vector<std::string> taken = ids;
        for (const JsonValue& entry : withPlayers->array)
        {
            const JsonValue* minPlayers = entry.type == JsonValue::Type::Object ? entry.find("minPlayers") : nullptr;
            const JsonValue* extra = entry.type == JsonValue::Type::Object ? entry.find("options") : nullptr;
            if (minPlayers == nullptr || minPlayers->type != JsonValue::Type::Number ||
                (minPlayers->text != "2" && minPlayers->text != "3" && minPlayers->text != "4"))
            {
                error = "preset \"" + preset.Id + "\": a withPlayers entry has no minPlayers of 2, 3 or 4";
                return false;
            }
            CoreGameOptionPresetExtra rule;
            rule.MinPlayers = minPlayers->text[0] - '0';
            const bool repeated = std::any_of(preset.WithPlayers.begin(), preset.WithPlayers.end(),
                [&rule](const CoreGameOptionPresetExtra& other) { return other.MinPlayers == rule.MinPlayers; });
            if (repeated || extra == nullptr || extra->type != JsonValue::Type::Array || extra->array.empty())
            {
                error = "preset \"" + preset.Id + "\": withPlayers for " + std::to_string(rule.MinPlayers) +
                    " players " + (repeated ? "is there twice" : "turns no options on");
                return false;
            }
            std::vector<std::string> extraIds;
            for (const JsonValue& id : extra->array)
            {
                const bool known = id.type == JsonValue::Type::String &&
                    std::any_of(pack.Options.begin(), pack.Options.end(),
                        [&id](const LoadedOption& option) { return option.Id == id.text && !option.StageList; });
                if (!known || contains(taken, id.text))
                {
                    error = "preset \"" + preset.Id + "\": withPlayers names \"" +
                        (id.type == JsonValue::Type::String ? id.text : std::string("?")) + "\", which " +
                        (known ? "it already turns on" : "isn't a usable option of this pack");
                    return false;
                }
                taken.push_back(id.text);
                extraIds.push_back(id.text);
            }
            for (const LoadedOption& option : pack.Options)
            {
                if (contains(extraIds, option.Id))
                    rule.Options.push_back(option.Id);
            }
            // What they do, for the Custom view's line about them.
            if (const std::string* description = string_member(entry, "description"); description != nullptr &&
                valid_label(*description))
                rule.Description = *description;
            Translations text;
            parse_translations(entry, text);
            preset.WithPlayers.push_back(std::move(rule));
            preset.WithPlayersText.push_back(std::move(text));
        }
        // Fewest players first, each entry's translations with it.
        std::vector<size_t> order(preset.WithPlayers.size());
        for (size_t i = 0; i < order.size(); i++)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&preset](size_t a, size_t b) {
            return preset.WithPlayers[a].MinPlayers < preset.WithPlayers[b].MinPlayers;
        });
        std::vector<CoreGameOptionPresetExtra> sortedRules;
        std::vector<Translations> sortedText;
        for (const size_t i : order)
        {
            sortedRules.push_back(std::move(preset.WithPlayers[i]));
            sortedText.push_back(std::move(preset.WithPlayersText[i]));
        }
        preset.WithPlayers = std::move(sortedRules);
        preset.WithPlayersText = std::move(sortedText);

        std::vector<std::string> bigger = ids;
        for (const CoreGameOptionPresetExtra& rule : preset.WithPlayers)
        {
            bigger.insert(bigger.end(), rule.Options.begin(), rule.Options.end());
            std::vector<const LoadedOption*> biggerSet;
            for (const LoadedOption& option : pack.Options)
            {
                if (contains(bigger, option.Id))
                    biggerSet.push_back(&option);
            }
            if (!check_option_set(biggerSet, all, why))
            {
                error = "preset \"" + preset.Id + "\" can't be used with " + std::to_string(rule.MinPlayers) +
                    " players: " + why;
                return false;
            }
        }
    }

    // "aliases": the ids it had before, for choices remembered by them.
    if (const JsonValue* aliases = json.find("aliases"); aliases != nullptr)
    {
        if (aliases->type != JsonValue::Type::Array)
        {
            log_pack(pack.File, "preset \"" + preset.Id + "\": aliases isn't a list, ignored");
        }
        else
        {
            for (const JsonValue& alias : aliases->array)
            {
                if (alias.type != JsonValue::Type::String || !valid_option_id(alias.text) || alias.text == preset.Id)
                {
                    log_pack(pack.File, "preset \"" + preset.Id + "\": ignored an alias that isn't another preset id");
                    continue;
                }
                if (!contains(preset.Aliases, alias.text))
                    preset.Aliases.push_back(alias.text);
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

    if (const JsonValue* groups = root.find("groups"); groups != nullptr)
    {
        if (groups->type != JsonValue::Type::Array)
        {
            log_pack(pack.File, "groups isn't a list, ignored");
        }
        else
        {
            for (const JsonValue& entry : groups->array)
            {
                LoadedGroup group;
                std::string groupError;
                if (!parse_group(entry, group, groupError))
                {
                    log_pack(pack.File, "skipped a group: " + groupError);
                    continue;
                }
                const bool duplicate = std::any_of(pack.Groups.begin(), pack.Groups.end(),
                    [&group](const LoadedGroup& other) { return other.Id == group.Id; });
                if (duplicate)
                {
                    log_pack(pack.File, "skipped a second group \"" + group.Id + "\"");
                    continue;
                }
                pack.Groups.push_back(std::move(group));
            }
        }
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
        if (!parse_option(entry, option, optionError) || !parse_stage_list(entry, option, pack.File, optionError))
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
        if (const JsonValue* group = entry.find("group"); group != nullptr)
        {
            if (group->type == JsonValue::Type::String && valid_option_id(group->text))
                option.Group = group->text;
            else
                log_pack(pack.File, "option \"" + option.Id + "\": group isn't a group id, ignored");
        }
        option.Requires = parse_id_list(entry, "requires", option.Id, pack.File);
        option.Conflicts = parse_id_list(entry, "conflicts", option.Id, pack.File);
        // "saveData": "blank" is the only value so far; anything else might
        // mean a rule this version doesn't know, so it's logged.
        if (const JsonValue* saveData = entry.find("saveData"); saveData != nullptr)
        {
            if (saveData->type == JsonValue::Type::String && saveData->text == "blank")
                option.BlankSave = true;
            else
                log_pack(pack.File, "option \"" + option.Id + "\": saveData isn't \"blank\", ignored");
        }
        if (const JsonValue* hidden = entry.find("hidden"); hidden != nullptr)
        {
            if (hidden->type == JsonValue::Type::Bool)
                option.Hidden = hidden->boolean;
            else
                log_pack(pack.File, "option \"" + option.Id + "\": hidden isn't true or false, ignored");
        }
        option.Aliases = parse_option_aliases(entry, option.Id, pack.File);
        pack.Options.push_back(std::move(option));
    }
    link_options(*options, pack);
    if (pack.Options.empty())
    {
        error = "has no usable options";
        return false;
    }

    if (const JsonValue* presets = root.find("presets"); presets != nullptr)
    {
        if (presets->type != JsonValue::Type::Array)
        {
            log_pack(pack.File, "presets isn't a list, ignored");
        }
        else
        {
            for (const JsonValue& entry : presets->array)
            {
                LoadedPreset preset;
                std::string presetError;
                if (!parse_preset(entry, pack, preset, presetError))
                {
                    log_pack(pack.File, "skipped a preset: " + presetError);
                    continue;
                }
                const auto same = std::find_if(pack.Presets.begin(), pack.Presets.end(), [&preset](const LoadedPreset& other) {
                    return other.Id == preset.Id || (other.Options == preset.Options && other.Lists == preset.Lists);
                });
                if (same != pack.Presets.end())
                {
                    log_pack(pack.File, "skipped preset \"" + preset.Id + "\": " +
                        (same->Id == preset.Id ? std::string("a second preset with that id")
                                               : "it turns on the same options as \"" + same->Id + "\""));
                    continue;
                }
                pack.Presets.push_back(std::move(preset));
            }
        }
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
                "Game options: loaded \"" + pack.Id + "\" (" + std::to_string(pack.Options.size()) + " options, " +
                std::to_string(pack.Presets.size()) + " presets)");
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
// with an id already taken is left out.
static std::vector<std::pair<const LoadedPack*, const LoadedOption*>> offered_options(const std::string& romMd5)
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

CORE_EXPORT std::vector<CoreGameOptionInfo> CoreGetGameOptions(const std::string& romMd5, const std::string& language)
{
    std::vector<CoreGameOptionInfo> infos;
    ensure_packs_loaded();

    const auto offered = offered_options(romMd5);
    const auto isOffered = [&offered](const std::string& id) {
        return std::any_of(offered.begin(), offered.end(), [&id](const auto& entry) { return entry.second->Id == id; });
    };
    for (const auto& [pack, option] : offered)
    {
        CoreGameOptionInfo info;
        info.Id = option->Id;
        info.EnglishLabel = option->Label;
        info.EnglishDescription = option->Description;
        info.Label = option->Label;
        info.Description = option->Description;
        info.PackId = pack->Id;
        info.Game = pack->Game;
        info.Group = option->Group;
        info.NeedsBlankSave = option->BlankSave;
        info.StageList = option->StageList != nullptr;
        info.Hidden = option->Hidden;
        std::copy_if(option->Requires.begin(), option->Requires.end(), std::back_inserter(info.Requires), isOffered);
        std::copy_if(option->Conflicts.begin(), option->Conflicts.end(), std::back_inserter(info.Conflicts), isOffered);
        // An alias never stands for an option that's offered, and only turns
        // on this pack's own options.
        const auto ownOption = [&, pack = pack](const std::string& id) {
            return std::any_of(offered.begin(), offered.end(),
                [&](const auto& entry) { return entry.first == pack && entry.second->Id == id; });
        };
        for (const CoreGameOptionAlias& alias : option->Aliases)
        {
            if (!isOffered(alias.Id) && std::all_of(alias.With.begin(), alias.With.end(), ownOption))
                info.Aliases.push_back(alias);
        }

        if (const auto* translation = find_translation(option->Text, language))
        {
            if (!translation->first.empty())
                info.Label = translation->first;
            if (!translation->second.empty())
                info.Description = translation->second;
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

CORE_EXPORT std::vector<CoreGameOptionGroupInfo> CoreGetGameOptionGroups(const std::string& romMd5, const std::string& language)
{
    std::vector<CoreGameOptionGroupInfo> infos;
    ensure_packs_loaded();

    const std::string md5 = upper_md5(romMd5);
    for (const LoadedPack& pack : l_Packs)
    {
        if (!contains(pack.RomMd5, md5))
        {
            continue;
        }
        for (const LoadedGroup& group : pack.Groups)
        {
            const bool taken = std::any_of(infos.begin(), infos.end(),
                [&group](const CoreGameOptionGroupInfo& info) { return info.Id == group.Id; });
            if (taken)
            {
                continue;
            }
            CoreGameOptionGroupInfo info;
            info.Id = group.Id;
            info.EnglishLabel = group.Label;
            info.Label = group.Label;
            if (const auto* translation = find_translation(group.Text, language);
                translation != nullptr && !translation->first.empty())
            {
                info.Label = translation->first;
            }
            infos.push_back(std::move(info));
        }
    }
    return infos;
}

CORE_EXPORT std::vector<CoreGameOptionPresetInfo> CoreGetGameOptionPresets(const std::string& romMd5, const std::string& language)
{
    std::vector<CoreGameOptionPresetInfo> infos;
    ensure_packs_loaded();

    const auto offered = offered_options(romMd5);
    const std::string md5 = upper_md5(romMd5);
    for (const LoadedPack& pack : l_Packs)
    {
        if (!contains(pack.RomMd5, md5))
        {
            continue;
        }
        for (const LoadedPreset& preset : pack.Presets)
        {
            // Its options as this pack has them: another pack that came first
            // may offer an option of the same id that does something else.
            const auto ownOption = [&](const std::string& id) {
                return std::any_of(offered.begin(), offered.end(),
                    [&](const auto& entry) { return entry.first == &pack && entry.second->Id == id; });
            };
            const bool ownOptions = std::all_of(preset.Options.begin(), preset.Options.end(), ownOption) &&
                std::all_of(preset.WithPlayers.begin(), preset.WithPlayers.end(), [&](const CoreGameOptionPresetExtra& rule) {
                    return std::all_of(rule.Options.begin(), rule.Options.end(), ownOption);
                });
            const bool taken = std::any_of(infos.begin(), infos.end(), [&preset](const CoreGameOptionPresetInfo& info) {
                return info.Id == preset.Id || (info.Options == preset.Options && info.Lists == preset.Lists);
            });
            if (!ownOptions || taken)
            {
                continue;
            }
            CoreGameOptionPresetInfo info;
            info.Id = preset.Id;
            info.EnglishLabel = preset.Label;
            info.EnglishDescription = preset.Description;
            info.Label = preset.Label;
            info.Description = preset.Description;
            info.Options = preset.Options;
            info.Lists = preset.Lists;
            info.WithPlayers = preset.WithPlayers;
            for (size_t i = 0; i < info.WithPlayers.size() && i < preset.WithPlayersText.size(); i++)
            {
                const auto* translation = find_translation(preset.WithPlayersText[i], language);
                if (translation != nullptr && !translation->second.empty())
                    info.WithPlayers[i].Description = translation->second;
            }
            info.Aliases = preset.Aliases;
            info.PackId = pack.Id;
            if (const auto* translation = find_translation(preset.Text, language))
            {
                if (!translation->first.empty())
                    info.Label = translation->first;
                if (!translation->second.empty())
                    info.Description = translation->second;
            }
            infos.push_back(std::move(info));
        }
    }
    return infos;
}

CORE_EXPORT bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                                        const std::vector<std::string>& ids, CoreNetplayGameOptions& options)
{
    return CoreResolveGameOptions(rom, romMd5, ids, {}, options);
}

CORE_EXPORT bool CoreResolveGameOptions(const std::filesystem::path& rom, const std::string& romMd5,
                                        const std::vector<std::string>& ids, const std::map<std::string, std::string>& lists,
                                        CoreNetplayGameOptions& options)
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
    std::vector<const LoadedOption*> chosen;
    std::vector<const LoadedOption*> all;
    for (const auto& [pack, option] : offered)
    {
        all.push_back(option);
        if (std::find(ids.begin(), ids.end(), option->Id) == ids.end())
        {
            continue;
        }
        chosen.push_back(option);
        CoreNetplayGameOption resolved{ option->Id, option->Label, option->Patches };
        if (option->StageList)
        {
            // The list's three ranges get the list's bytes; the rest is the
            // same for every list.
            const LoadedStageList& list = *option->StageList;
            const auto code = lists.find(option->Id);
            std::vector<CoreStageListEntry> entries;
            const CoreStageListProblem problem = code == lists.end() ? CoreStageListProblem::Empty
                : CoreDecodeStageList(list.Info, code->second, entries);
            if (problem != CoreStageListProblem::None)
            {
                options = {};
                CoreSetError(problem == CoreStageListProblem::Empty
                    ? "\"" + option->Label + "\" is on, but no stages were picked for it."
                    : "\"" + option->Label + "\" has a stage list code this version can't use.");
                return false;
            }
            for (const CoreGameOptionPatch& made : CoreStageListPatches(list.Info, list.Layout, entries))
            {
                for (CoreGameOptionPatch& patch : resolved.Patches)
                {
                    if (patch.Offset == made.Offset && patch.Bytes.size() == made.Bytes.size())
                        patch = made;
                }
            }
        }
        options.Options.push_back(std::move(resolved));
    }

    std::string error;
    if (!check_option_set(chosen, all, error))
    {
        options = {};
        CoreSetError(error);
        return false;
    }
    if (!check_limits(options, error))
    {
        options = {};
        CoreSetError("These game options can't be used together: " + error);
        return false;
    }

    std::vector<char> image;
    if (!read_rom_image(rom, image))
    {
        const std::string why = CoreGetError();
        options = {};
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
                options = {};
                return false;
            }
            if (std::memcmp(image.data() + patch.Offset, patch.Expect.data(), patch.Expect.size()) != 0)
            {
                CoreSetError("\"" + option.Label + "\" doesn't fit this ROM (it expects other bytes at " +
                    hex_offset(patch.Offset) + "). Is it the version of the game its pack is for?");
                options = {};
                return false;
            }
        }
    }
    return true;
}

static const LoadedStageList* offered_stage_list(const std::string& romMd5, const std::string& optionId)
{
    ensure_packs_loaded();
    for (const auto& [pack, option] : offered_options(romMd5))
    {
        if (option->Id == optionId)
            return option->StageList.get();
    }
    return nullptr;
}

CORE_EXPORT bool CoreGetStageList(const std::string& romMd5, const std::string& optionId,
                                  const std::string& language, CoreStageListInfo& info)
{
    const LoadedStageList* list = offered_stage_list(romMd5, optionId);
    if (list == nullptr)
    {
        info = {};
        return false;
    }
    info = stage_list_in(*list, language);
    return true;
}

CORE_EXPORT bool CoreReadStageListIcons(const std::filesystem::path& rom, const std::string& romMd5,
                                        const std::string& optionId, std::map<int, CoreStageIcon>& icons)
{
    icons.clear();
    const LoadedStageList* list = offered_stage_list(romMd5, optionId);
    if (list == nullptr || list->Layout.FileCount == 0)
    {
        CoreSetError("This game's stage list has no icons.");
        return false;
    }
    std::vector<char> image;
    if (!read_rom_image(rom, image))
    {
        return false;
    }
    // The ROM the pack is for, as far as the list's own ranges tell.
    const CoreStageListLayout& layout = list->Layout;
    const auto matches = [&image](uint32_t offset, const std::vector<uint8_t>& expect) {
        return uint64_t(offset) + expect.size() <= image.size() &&
            std::memcmp(image.data() + offset, expect.data(), expect.size()) == 0;
    };
    if (!matches(layout.TableOffset, layout.TableExpect) || !matches(layout.PagesOffset, layout.PagesExpect) ||
        !matches(layout.ModesOffset, layout.ModesExpect))
    {
        CoreSetError("The ROM isn't the version of the game its pack is for.");
        return false;
    }
    std::string error;
    if (!CoreDecodeStageListIcons(image, list->Info, layout, icons, error))
    {
        CoreSetError("Couldn't read the stage icons: " + error);
        return false;
    }
    return true;
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
