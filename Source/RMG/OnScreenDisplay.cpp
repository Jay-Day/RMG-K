/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "OnScreenDisplay.hpp"

#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Settings.hpp>

#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>
#include <QLocale>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

//
// Local Variables
//

static bool l_Initialized     = false;
static bool l_Enabled         = false;
static bool l_RenderingPaused = false;

enum class OnScreenDisplayMessageType
{
    System,
    Chat
};

struct OnScreenDisplayMessageEntry
{
    std::string message;
    std::chrono::time_point<std::chrono::high_resolution_clock> time;
    OnScreenDisplayMessageType type;
    bool skipSlideIn = false;
    bool overflowEvicting = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> overflowEvictStart{};
    float lastWindowHeight = 0.0f;
};

static std::deque<OnScreenDisplayMessageEntry> l_MessageQueue;
static int         l_MessagePosition = 1;
static float       l_MessagePaddingX = 20.0f;
static float       l_MessagePaddingY = 20.0f;
static float       l_BackgroundRed   = 1.0f;
static float       l_BackgroundGreen = 1.0f;
static float       l_BackgroundBlue  = 1.0f;
static float       l_BackgroundAlpha = 1.0f;
static float       l_TextRed         = 1.0f;
static float       l_TextGreen       = 1.0f;
static float       l_TextBlue        = 1.0f;
static float       l_TextAlpha       = 1.0f;
static int         l_MessageDuration = 7;
static float       l_MessageScale    = 1.25f;
static size_t      l_KailleraChatMaxMessages = 5;
static bool        l_KailleraChatEnabled = true;
static bool        l_FontsDirty      = true;
static const float l_BaseFontSize    = 13.0f;
// font.ttf covers Latin, Greek and Cyrillic. Japanese, Chinese, Korean and emoji
// come from fonts already on the system. Those are 13-20 MB each, so they're only
// read once such a character shows up, and off the emulation thread so it can't
// stall a running match. Until then the character draws as a placeholder.
struct OnScreenDisplayFallbackFont
{
    std::vector<char> data;
    int index = 0;
};
static std::atomic<bool> l_FallbackFontsWanted = false;
static bool l_FallbackFontsAdded = false;
static std::future<std::vector<OnScreenDisplayFallbackFont>> l_FallbackFontLoad;
static std::vector<OnScreenDisplayFallbackFont> l_FallbackFonts;
static const float l_MessageFadeoutDurationSeconds = 0.26f;
static const float l_MessageSlideInDurationSeconds = 0.31f;
static bool        l_InputPromptActive = false;
static std::string l_InputPrompt;
static bool        l_KailleraPortLabelsEnabled = false;
static int         l_KailleraPortLabelPlayerCount = 0;
static std::array<std::string, 4> l_KailleraPortLabelPlayerNames;
static constexpr std::array<float, 4> l_KailleraPortLabelCenterOffset720p = { 19.0f, 0.0f, -19.0f, -70.0f };
// Big centered banner (e.g. "Stream is buffering...") drawn over everything while
// a spectator fast-forwards toward a broadcast's live edge. Empty = hidden.
static std::string l_CenterMessage;
// Persistent audience badge for broadcasters and spectators. Unlike ordinary
// system messages, it remains visible until explicitly cleared.
static bool l_LiveReplayStatusEnabled = false;
static std::string l_LiveReplayStatus;
static std::mutex l_LiveReplayStatusMutex;
// Port labels and the center message change on the UI thread during a match
// while the emulation thread draws them.
static std::mutex l_KailleraOverlayMutex;

static float OnScreenDisplayEaseOutCubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - (inv * inv * inv);
}

static float OnScreenDisplayEaseInFadeOutAlpha(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (t * t);
}

static float OnScreenDisplayGetOverflowEvictProgress(const OnScreenDisplayMessageEntry& entry,
    const std::chrono::time_point<std::chrono::high_resolution_clock>& currentTime)
{
    if (!entry.overflowEvicting)
    {
        return 0.0f;
    }

    if (l_MessageSlideInDurationSeconds <= 0.0f)
    {
        return 1.0f;
    }

    const float elapsed = std::chrono::duration<float>(currentTime - entry.overflowEvictStart).count();
    return std::clamp(elapsed / l_MessageSlideInDurationSeconds, 0.0f, 1.0f);
}

static void OnScreenDisplayPruneCompletedOverflowEvictions(const std::chrono::time_point<std::chrono::high_resolution_clock>& currentTime)
{
    while (!l_MessageQueue.empty())
    {
        const OnScreenDisplayMessageEntry& entry = l_MessageQueue.front();
        if (!entry.overflowEvicting || OnScreenDisplayGetOverflowEvictProgress(entry, currentTime) < 1.0f)
        {
            break;
        }
        l_MessageQueue.pop_front();
    }
}

static void OnScreenDisplayEnforceQueueLimitWithFade(const std::chrono::time_point<std::chrono::high_resolution_clock>& currentTime)
{
    OnScreenDisplayPruneCompletedOverflowEvictions(currentTime);

    size_t activeMessageCount = std::count_if(l_MessageQueue.begin(), l_MessageQueue.end(),
        [](const OnScreenDisplayMessageEntry& entry) {
            return !entry.overflowEvicting;
        });

    while (activeMessageCount > l_KailleraChatMaxMessages)
    {
        auto oldestActive = std::find_if(l_MessageQueue.begin(), l_MessageQueue.end(),
            [](const OnScreenDisplayMessageEntry& entry) {
                return !entry.overflowEvicting;
            });
        if (oldestActive == l_MessageQueue.end())
        {
            break;
        }

        // If chat input is open, one message slot is reserved for the prompt.
        // Messages outside that prompt-visible window should not re-appear during overflow handling.
        size_t promptVisibleLimit = l_KailleraChatMaxMessages;
        if (l_InputPromptActive && promptVisibleLimit > 0)
        {
            promptVisibleLimit -= 1;
        }
        const bool oldestWouldBeHiddenByPrompt = l_InputPromptActive && (activeMessageCount > promptVisibleLimit);
        if (oldestWouldBeHiddenByPrompt)
        {
            l_MessageQueue.erase(oldestActive);
            activeMessageCount--;
            continue;
        }

        oldestActive->overflowEvicting = true;
        oldestActive->overflowEvictStart = currentTime;
        activeMessageCount--;
    }
}

// Notes when text needs a font beyond font.ttf: from Hangul Jamo up, leaving out
// the punctuation and Latin extensions in between.
static void OnScreenDisplayCheckFallbackFonts(const std::string& text)
{
    if (l_FallbackFontsWanted.load(std::memory_order_relaxed))
    {
        return;
    }

    for (size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const bool hangulJamo = lead == 0xE1 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) >= 0x84 && static_cast<unsigned char>(text[i + 1]) <= 0x87;
        const bool symbols = lead == 0xE2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) >= 0x98; // U+2600 and up
        if (lead >= 0xE3 || hangulJamo || symbols)
        {
            l_FallbackFontsWanted.store(true, std::memory_order_relaxed);
            return;
        }
    }
}

// Variation selectors ask for a presentation the OSD can't do anyway, and the emoji
// font gives them a wide blank glyph, which leaves a gap after the emoji.
static void OnScreenDisplayDropVariationSelectors(std::string& text)
{
    for (const char* selector : {"ï¸", "ï¸"})
    {
        for (size_t at = text.find(selector); at != std::string::npos; at = text.find(selector, at))
        {
            text.erase(at, 3);
        }
    }
}

static std::vector<OnScreenDisplayFallbackFont> OnScreenDisplayReadFallbackFonts(void)
{
    struct Candidate
    {
        const char* path;
        int index;
    };
    using Candidates = std::vector<Candidate>;

#ifdef _WIN32
    const char* windowsDirectory = std::getenv("WINDIR");
    const std::filesystem::path directory =
        std::filesystem::path(windowsDirectory != nullptr ? windowsDirectory : "C:\\Windows") / "Fonts";
    const Candidates japanese = {{"YuGothM.ttc", 0}, {"meiryo.ttc", 0}, {"msgothic.ttc", 0}};
    const Candidates chinese = {{"msyh.ttc", 0}, {"msyh.ttf", 0}, {"msjh.ttc", 0}, {"simsun.ttc", 0}};
    const Candidates korean = {{"malgun.ttf", 0}};
    const Candidates emoji = {{"seguiemj.ttf", 0}, {"seguisym.ttf", 0}};
#else
    const std::filesystem::path directory = "/usr/share/fonts";
    // Noto Sans CJK puts every language in one file; the index picks which one wins.
    const char* notoPaths[] = {"opentype/noto/NotoSansCJK-Regular.ttc", "noto-cjk/NotoSansCJK-Regular.ttc",
                               "google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
                               "OTF/NotoSansCJK-Regular.ttc", "noto/NotoSansCJK-Regular.ttc"};
    const auto noto = [&notoPaths](int index) {
        Candidates candidates;
        for (const char* path : notoPaths)
        {
            candidates.push_back({path, index});
        }
        return candidates;
    };
    Candidates japanese = noto(0);
    japanese.push_back({"truetype/fonts-japanese-gothic.ttf", 0});
    Candidates chinese = noto(2);
    chinese.push_back({"truetype/wqy/wqy-microhei.ttc", 0});
    chinese.push_back({"truetype/wqy/wqy-zenhei.ttc", 0});
    Candidates korean = noto(1);
    korean.push_back({"truetype/nanum/NanumGothic.ttf", 0});
    const Candidates emoji = {{"truetype/noto/NotoEmoji-Regular.ttf", 0}, {"noto/NotoEmoji-Regular.ttf", 0},
                              {"truetype/dejavu/DejaVuSans.ttf", 0}};
#endif

    // The same Han character is drawn differently per language, so the player's
    // own language goes first. Everyone else gets Japanese first, the likeliest
    // non-Latin names in this community.
    const QLocale::Language language = QLocale::system().language();
    std::vector<const Candidates*> order = {&japanese, &chinese, &korean};
    if (language == QLocale::Chinese)
    {
        order = {&chinese, &japanese, &korean};
    }
    else if (language == QLocale::Korean)
    {
        order = {&korean, &japanese, &chinese};
    }
    order.push_back(&emoji);

    std::vector<OnScreenDisplayFallbackFont> fonts;
    for (const Candidates* candidates : order)
    {
        for (const Candidate& candidate : *candidates)
        {
            std::ifstream file(directory / candidate.path, std::ios::binary);
            if (!file)
            {
                continue;
            }
            OnScreenDisplayFallbackFont font;
            font.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            font.index = candidate.index;
            fonts.push_back(std::move(font));
            break;
        }
    }
    return fonts;
}

// ImGui sizes a font by its line height, and how much of that a font's glyphs fill
// varies a lot: Noto Sans CJK draws a quarter smaller than Yu Gothic at the same size.
// This reads a font's line height in ems from its header, so they can all be evened out.
static float OnScreenDisplayFontLineHeightInEms(const std::vector<char>& data, int index)
{
    const auto read16 = [&data](size_t at) -> int {
        if (at + 2 > data.size())
        {
            return 0;
        }
        return (static_cast<unsigned char>(data[at]) << 8) | static_cast<unsigned char>(data[at + 1]);
    };
    const auto read32 = [&read16](size_t at) -> size_t {
        return (static_cast<size_t>(read16(at)) << 16) | static_cast<size_t>(read16(at + 2));
    };

    size_t offset = 0;
    if (data.size() >= 4 && std::memcmp(data.data(), "ttcf", 4) == 0)
    {
        offset = read32(12 + 4 * static_cast<size_t>(index));
    }

    int unitsPerEm = 0;
    int ascent = 0;
    int descent = 0;
    const int tableCount = read16(offset + 4);
    for (int i = 0; i < tableCount; ++i)
    {
        const size_t record = offset + 12 + 16 * static_cast<size_t>(i);
        if (record + 16 > data.size())
        {
            break;
        }
        const size_t table = read32(record + 8);
        if (std::memcmp(data.data() + record, "head", 4) == 0)
        {
            unitsPerEm = read16(table + 18);
        }
        else if (std::memcmp(data.data() + record, "hhea", 4) == 0)
        {
            ascent = static_cast<int16_t>(read16(table + 4));
            descent = static_cast<int16_t>(read16(table + 6));
        }
    }

    if (unitsPerEm <= 0 || ascent <= descent)
    {
        return 1.0f;
    }
    return static_cast<float>(ascent - descent) / static_cast<float>(unitsPerEm);
}

static void OnScreenDisplayUpdateFonts(void)
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.Fonts->Fonts.empty())
    {
        const std::filesystem::path fontPath = CoreGetSharedDataDirectory() / "font.ttf";
        std::error_code error;
        if (std::filesystem::exists(fontPath, error))
        {
            io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), l_BaseFontSize);
        }
        if (io.Fonts->Fonts.empty())
        {
            io.Fonts->AddFontDefault();
        }
    }

    if (l_FontsDirty)
    {
        // glyphs are drawn on demand at each size in use, so nothing needs rebuilding
        ImGui::GetStyle().FontSizeBase = l_BaseFontSize * l_MessageScale;
        l_FontsDirty = false;
    }

    if (l_FallbackFontsAdded)
    {
        return;
    }
    if (!l_FallbackFontLoad.valid())
    {
        if (l_FallbackFontsWanted.load(std::memory_order_relaxed))
        {
            l_FallbackFontLoad = std::async(std::launch::async, OnScreenDisplayReadFallbackFonts);
        }
        return;
    }
    if (l_FallbackFontLoad.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }

    l_FallbackFonts = l_FallbackFontLoad.get();
    for (OnScreenDisplayFallbackFont& font : l_FallbackFonts)
    {
        ImFontConfig config;
        config.MergeMode = true;
        config.FontNo = font.index;
        config.FontDataOwnedByAtlas = false;
        // A merged font's size is relative to the base font's. 0.92 em sits well beside font.ttf.
        const float size = l_BaseFontSize * 0.92f * OnScreenDisplayFontLineHeightInEms(font.data, font.index);
        io.Fonts->AddFontFromMemoryTTF(font.data.data(), static_cast<int>(font.data.size()), size, &config);
    }
    l_FallbackFontsAdded = true;
}

static std::string OnScreenDisplayNormalizePortLabelName(const std::string& name, int playerIndex)
{
    std::string label = name.empty()
        ? ("P" + std::to_string(playerIndex + 1))
        : name;

    for (char& c : label)
    {
        if (static_cast<unsigned char>(c) < 32)
        {
            c = ' ';
        }
    }
    return label;
}

static std::string OnScreenDisplayTruncateToWidth(std::string label, float textScale, float maxTextWidth)
{
    if (maxTextWidth <= 0.0f)
    {
        return {};
    }

    while (!label.empty())
    {
        const float textWidth = ImGui::CalcTextSize(label.c_str()).x * textScale;
        if (textWidth <= maxTextWidth)
        {
            return label;
        }
        // drop one whole character: its continuation bytes, then its lead byte
        while (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xC0) == 0x80)
        {
            label.pop_back();
        }
        if (!label.empty())
        {
            label.pop_back();
        }
    }

    return label;
}

static void OnScreenDisplayRenderKailleraPortLabels(void)
{
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    if (!l_KailleraPortLabelsEnabled || l_KailleraPortLabelPlayerCount <= 0)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const int playerCount = std::clamp(l_KailleraPortLabelPlayerCount, 1, 4);
    float contentX = 0.0f;
    float contentY = 0.0f;
    float contentWidth = io.DisplaySize.x;
    float contentHeight = io.DisplaySize.y;
    if (io.DisplaySize.x > io.DisplaySize.y * (4.0f / 3.0f))
    {
        contentWidth = io.DisplaySize.y * (4.0f / 3.0f);
        contentX = (io.DisplaySize.x - contentWidth) * 0.5f;
    }
    else
    {
        contentHeight = io.DisplaySize.x * (3.0f / 4.0f);
        contentY = (io.DisplaySize.y - contentHeight) * 0.5f;
    }

    const float outputScale720p = contentHeight / 720.0f;
    const float baseScale = std::max(1.0f, contentHeight / 240.0f);
    const float fontScale = (baseScale * 7.0f) / (l_BaseFontSize * std::max(l_MessageScale, 0.1f));
    const float compactFontScale = std::max(0.65f, fontScale * 0.82f);
    const float portWidth = contentWidth / 4.0f;
    const ImVec2 labelPadding(4.0f * baseScale, 2.0f * baseScale);
    const float labelHeight = (ImGui::GetTextLineHeight() * fontScale) + (labelPadding.y * 2.0f);
    const float y = contentY + contentHeight - (4.0f * baseScale) - (labelHeight * 0.5f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(l_BackgroundRed, l_BackgroundGreen, l_BackgroundBlue, l_BackgroundAlpha));
    ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(l_TextRed, l_TextGreen, l_TextBlue, l_TextAlpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, labelPadding);

    for (int i = 0; i < playerCount; ++i)
    {
        std::string label = OnScreenDisplayNormalizePortLabelName(l_KailleraPortLabelPlayerNames[static_cast<size_t>(i)], i);
        const float maxTextWidth = std::max(0.0f, portWidth - (8.0f * baseScale));
        float activeFontScale = fontScale;

        const float normalTextWidth = ImGui::CalcTextSize(label.c_str()).x * fontScale;
        if (normalTextWidth > maxTextWidth)
        {
            activeFontScale = compactFontScale;
            label = OnScreenDisplayTruncateToWidth(label, activeFontScale, maxTextWidth);
        }

        const float offsetX = l_KailleraPortLabelCenterOffset720p[static_cast<size_t>(i)] * outputScale720p;
        const float centerX = contentX + portWidth * static_cast<float>(i) + (portWidth / 2.0f) + offsetX;
        ImGui::SetNextWindowPos(ImVec2(centerX, y), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0.0f, 0.0f),
            ImVec2(std::max(1.0f, portWidth - (4.0f * baseScale)), io.DisplaySize.y));
        const std::string windowName = "Kaillera Port Label##" + std::to_string(i);
        ImGui::Begin(windowName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::SetWindowFontScale(activeFontScale);
        const ImVec2 textPos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(textPos.x + 1.0f, textPos.y));
        ImGui::Text("%s", label.c_str());
        ImGui::SetCursorPos(textPos);
        ImGui::Text("%s", label.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// A large message box centered on screen, drawn over everything else. Used for
// the spectate "Stream is buffering..." banner during fast-forward catch-up.
static void OnScreenDisplayRenderCenterMessage(void)
{
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    if (l_CenterMessage.empty())
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    // Size relative to the 4:3 content height, matching the port-label math so it
    // scales with the window. Larger multiplier than port labels — this is a hero
    // banner, not a per-port tag.
    float contentHeight = io.DisplaySize.y;
    if (io.DisplaySize.x <= io.DisplaySize.y * (4.0f / 3.0f))
        contentHeight = io.DisplaySize.x * (3.0f / 4.0f);
    const float baseScale = std::max(1.0f, contentHeight / 240.0f);
    const float fontScale = (baseScale * 9.0f) / (l_BaseFontSize * std::max(l_MessageScale, 0.1f));
    const ImVec2 padding(16.0f * baseScale, 11.0f * baseScale);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.66f));
    ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(l_TextRed, l_TextGreen, l_TextBlue, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * baseScale);

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Stream Buffering Banner", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(fontScale);
    ImGui::Text("%s", l_CenterMessage.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

//
// Exported Functions
//

bool OnScreenDisplayInit(void)
{
    if (l_Initialized)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplOpenGL3_Init())
    {
        return false;
    }

    l_FontsDirty = true;
    l_Initialized = true;
    return true;
}

void OnScreenDisplayShutdown(void)
{
    if (!l_Initialized)
    {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    if (l_FallbackFontLoad.valid())
    {
        l_FallbackFontLoad.wait();
        l_FallbackFontLoad = {};
    }
    l_FallbackFonts.clear();
    l_FallbackFontsAdded = false;
    l_FallbackFontsWanted.store(false, std::memory_order_relaxed);

    l_MessageQueue.clear();
    l_InputPromptActive = false;
    l_InputPrompt.clear();
    l_KailleraChatEnabled = true;
    {
        std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
        l_KailleraPortLabelPlayerCount = 0;
        l_KailleraPortLabelPlayerNames = {};
        l_CenterMessage.clear();
    }
    {
        std::lock_guard<std::mutex> lock(l_LiveReplayStatusMutex);
        l_LiveReplayStatus.clear();
    }
    l_Initialized     = false;
    l_RenderingPaused = false;
}

void OnScreenDisplayLoadSettings(void)
{
    l_Enabled         = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayEnabled);
    l_KailleraChatEnabled = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayChatEnabled);
    l_KailleraPortLabelsEnabled = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayKailleraPortLabels);
    l_LiveReplayStatusEnabled = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayLiveReplayStatus);
    l_MessagePosition = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayLocation);
    l_MessagePaddingX = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingX);
    l_MessagePaddingY = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingY);
    l_MessageDuration = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayDuration);
    float newMessageScale = CoreSettingsGetFloatValue(SettingsID::GUI_OnScreenDisplayScale);
    if (newMessageScale <= 0.1f)
    {
        newMessageScale = 1.0f;
    }
    if (std::abs(newMessageScale - l_MessageScale) > 0.001f)
    {
        l_MessageScale = newMessageScale;
        l_FontsDirty = true;
    }
    int maxChatMessages = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayMaxMessages);
    if (maxChatMessages < 1)
    {
        maxChatMessages = 1;
    }
    l_KailleraChatMaxMessages = static_cast<size_t>(maxChatMessages);
    if (!l_KailleraChatEnabled)
    {
        l_InputPromptActive = false;
        l_InputPrompt.clear();
        for (auto it = l_MessageQueue.begin(); it != l_MessageQueue.end();)
        {
            if (it->type == OnScreenDisplayMessageType::Chat)
            {
                it = l_MessageQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::vector<int> backgroundColor = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayBackgroundColor);
    std::vector<int> textColor       = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayTextColor);
    if (backgroundColor.size() == 4)
    {
        l_BackgroundRed   = backgroundColor.at(0) / 255.0f;
        l_BackgroundGreen = backgroundColor.at(1) / 255.0f;
        l_BackgroundBlue  = backgroundColor.at(2) / 255.0f;
        l_BackgroundAlpha = backgroundColor.at(3) / 255.0f;
    }
    if (textColor.size() == 4)
    {
        l_TextRed   = textColor.at(0) / 255.0f;
        l_TextGreen = textColor.at(1) / 255.0f;
        l_TextBlue  = textColor.at(2) / 255.0f;
        l_TextAlpha = textColor.at(3) / 255.0f;
    }

    OnScreenDisplayEnforceQueueLimitWithFade(std::chrono::high_resolution_clock::now());
}

bool OnScreenDisplaySetDisplaySize(int width, int height)
{
    if (!l_Initialized)
    {
        return false;
    }

    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)width, (float)height);
    return true;
}

void OnScreenDisplaySetMessage(std::string message)
{
    if (!l_Initialized)
    {
        return;
    }

    if (message.empty())
    {
        for (auto it = l_MessageQueue.begin(); it != l_MessageQueue.end();)
        {
            if (it->type == OnScreenDisplayMessageType::System)
            {
                it = l_MessageQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return;
    }

    OnScreenDisplayCheckFallbackFonts(message);
    l_MessageQueue.push_back({std::move(message), std::chrono::high_resolution_clock::now(), OnScreenDisplayMessageType::System});
    OnScreenDisplayEnforceQueueLimitWithFade(std::chrono::high_resolution_clock::now());
}

void OnScreenDisplaySetKailleraChatMessage(std::string message)
{
    if (!l_Initialized)
    {
        return;
    }

    if (message.empty())
    {
        for (auto it = l_MessageQueue.begin(); it != l_MessageQueue.end();)
        {
            if (it->type == OnScreenDisplayMessageType::Chat)
            {
                it = l_MessageQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return;
    }

    if (!l_KailleraChatEnabled)
    {
        return;
    }

    OnScreenDisplayDropVariationSelectors(message);
    OnScreenDisplayCheckFallbackFonts(message);
    l_MessageQueue.push_back({std::move(message), std::chrono::high_resolution_clock::now(), OnScreenDisplayMessageType::Chat});
    OnScreenDisplayEnforceQueueLimitWithFade(std::chrono::high_resolution_clock::now());
}

void OnScreenDisplaySetKailleraChatEnabled(bool enabled)
{
    l_KailleraChatEnabled = enabled;
    if (enabled)
    {
        return;
    }

    l_InputPromptActive = false;
    l_InputPrompt.clear();

    for (auto it = l_MessageQueue.begin(); it != l_MessageQueue.end();)
    {
        if (it->type == OnScreenDisplayMessageType::Chat)
        {
            it = l_MessageQueue.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void OnScreenDisplaySetKailleraChatMessageImmediate(std::string message)
{
    if (!l_Initialized)
    {
        return;
    }

    if (message.empty())
    {
        for (auto it = l_MessageQueue.begin(); it != l_MessageQueue.end();)
        {
            if (it->type == OnScreenDisplayMessageType::Chat)
            {
                it = l_MessageQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return;
    }

    if (!l_KailleraChatEnabled)
    {
        return;
    }

    OnScreenDisplayDropVariationSelectors(message);
    OnScreenDisplayCheckFallbackFonts(message);
    l_MessageQueue.push_back({std::move(message), std::chrono::high_resolution_clock::now(), OnScreenDisplayMessageType::Chat, true});
    OnScreenDisplayEnforceQueueLimitWithFade(std::chrono::high_resolution_clock::now());
}

void OnScreenDisplaySetInputPrompt(std::string message)
{
    if (!l_Initialized)
    {
        return;
    }

    if (message.empty() || !l_KailleraChatEnabled)
    {
        l_InputPromptActive = false;
        l_InputPrompt.clear();
        return;
    }

    OnScreenDisplayDropVariationSelectors(message);
    OnScreenDisplayCheckFallbackFonts(message);
    l_InputPromptActive = true;
    l_InputPrompt = std::move(message);
}

void OnScreenDisplaySetKailleraPortLabels(int playerCount, const std::array<std::string, 4>& playerNames)
{
    for (const std::string& name : playerNames)
    {
        OnScreenDisplayCheckFallbackFonts(name);
    }
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    l_KailleraPortLabelPlayerCount = std::clamp(playerCount, 0, 4);
    l_KailleraPortLabelPlayerNames = playerNames;
}

void OnScreenDisplaySetKailleraPortLabelNames(const std::array<std::string, 4>& playerNames)
{
    for (const std::string& name : playerNames)
    {
        OnScreenDisplayCheckFallbackFonts(name);
    }
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    l_KailleraPortLabelPlayerNames = playerNames;
}

void OnScreenDisplayClearKailleraPortLabels(void)
{
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    l_KailleraPortLabelPlayerCount = 0;
    l_KailleraPortLabelPlayerNames = {};
}

void OnScreenDisplaySetCenterMessage(const std::string& message)
{
    OnScreenDisplayCheckFallbackFonts(message);
    std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
    l_CenterMessage = message;
}

void OnScreenDisplaySetLiveReplayStatus(const std::string& message)
{
    // Keep this even before ImGui initializes: a spectator may receive the
    // initial audience count before the streamed game has launched.
    std::lock_guard<std::mutex> lock(l_LiveReplayStatusMutex);
    l_LiveReplayStatus = message;
}

void OnScreenDisplayRender(void)
{
    if (!l_Initialized || l_RenderingPaused)
    {
        return;
    }

    const auto currentTime = std::chrono::high_resolution_clock::now();
    OnScreenDisplayPruneCompletedOverflowEvictions(currentTime);

    const float visibleDurationSeconds = static_cast<float>(std::max(l_MessageDuration, 0));

    auto getMessageAgeSeconds = [&currentTime](const OnScreenDisplayMessageEntry& entry) -> float
    {
        return std::chrono::duration<float>(currentTime - entry.time).count();
    };

    auto getMessageFadeAlpha = [&](float ageSeconds) -> float
    {
        if (l_InputPromptActive || ageSeconds <= visibleDurationSeconds)
        {
            return 1.0f;
        }

        const float fadeProgress = (ageSeconds - visibleDurationSeconds) / l_MessageFadeoutDurationSeconds;
        return OnScreenDisplayEaseInFadeOutAlpha(fadeProgress);
    };

    auto getMessageSlideProgress = [](float ageSeconds) -> float
    {
        const float slideProgress = ageSeconds / l_MessageSlideInDurationSeconds;
        return OnScreenDisplayEaseOutCubic(slideProgress);
    };

    auto getOverflowFadeAlpha = [&currentTime](const OnScreenDisplayMessageEntry& entry) -> float
    {
        const float evictProgress = OnScreenDisplayGetOverflowEvictProgress(entry, currentTime);
        return OnScreenDisplayEaseInFadeOutAlpha(evictProgress);
    };

    bool hasVisibleQueueMessage = false;
    for (const auto& messageEntry : l_MessageQueue)
    {
        const float alpha = getMessageFadeAlpha(getMessageAgeSeconds(messageEntry)) * getOverflowFadeAlpha(messageEntry);
        if (alpha > 0.0f &&
            (messageEntry.type != OnScreenDisplayMessageType::Chat || l_KailleraChatEnabled))
        {
            hasVisibleQueueMessage = true;
            break;
        }
    }

    std::string liveReplayStatus;
    {
        std::lock_guard<std::mutex> lock(l_LiveReplayStatusMutex);
        liveReplayStatus = l_LiveReplayStatus;
    }
    bool hasPortLabels = false;
    bool hasCenterMessage = false;
    {
        std::lock_guard<std::mutex> lock(l_KailleraOverlayMutex);
        hasPortLabels = l_KailleraPortLabelsEnabled && l_KailleraPortLabelPlayerCount > 0;
        hasCenterMessage = !l_CenterMessage.empty();
    }
    const bool hasLiveReplayStatus = l_LiveReplayStatusEnabled && !liveReplayStatus.empty();
    const bool hasMessages = l_Enabled && (hasVisibleQueueMessage || l_InputPromptActive ||
                                           hasPortLabels || hasCenterMessage || hasLiveReplayStatus);

    if (!hasMessages)
    {
        return;
    }

    OnScreenDisplayUpdateFonts();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    
    ImGuiIO& io = ImGui::GetIO();
    float maxWrapWidth = io.DisplaySize.x - (l_MessagePaddingX * 2.0f);
    if (maxWrapWidth < 0.0f)
    {
        maxWrapWidth = 0.0f;
    }

    size_t visibleQueueCount = l_KailleraChatMaxMessages;
    if (l_InputPromptActive && visibleQueueCount > 0)
    {
        visibleQueueCount -= 1;
    }
    const size_t overflowVisibleCount = std::count_if(l_MessageQueue.begin(), l_MessageQueue.end(),
        [&getOverflowFadeAlpha](const OnScreenDisplayMessageEntry& entry) {
            return entry.overflowEvicting && getOverflowFadeAlpha(entry) > 0.0f;
        });
    visibleQueueCount += overflowVisibleCount;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(l_BackgroundRed, l_BackgroundGreen, l_BackgroundBlue, l_BackgroundAlpha));
    ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(l_TextRed, l_TextGreen, l_TextBlue, l_TextAlpha));

    float baseX = 0.0f;
    float baseY = 0.0f;
    ImVec2 pivot(0.0f, 0.0f);
    switch (l_MessagePosition)
    {
    default:
    case 0: // left bottom
        baseX = l_MessagePaddingX;
        baseY = io.DisplaySize.y - l_MessagePaddingY;
        pivot = ImVec2(0.0f, 1.0f);
        break;
    case 1: // left top
        baseX = l_MessagePaddingX;
        baseY = l_MessagePaddingY;
        pivot = ImVec2(0.0f, 0.0f);
        break;
    case 2: // right top
        baseX = io.DisplaySize.x - l_MessagePaddingX;
        baseY = l_MessagePaddingY;
        pivot = ImVec2(1.0f, 0.0f);
        break;
    case 3: // right bottom
        baseX = io.DisplaySize.x - l_MessagePaddingX;
        baseY = io.DisplaySize.y - l_MessagePaddingY;
        pivot = ImVec2(1.0f, 1.0f);
        break;
    }

    // ImGui stacks windows by when each was first created, not by draw order, and
    // NoBringToFrontOnFocus creates a window behind all the others. Port labels
    // keep that flag and chat and the prompt don't, which puts chat on top however
    // the two came to be created.
    const ImGuiWindowFlags chatWindowFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

    const bool anchorBottom = (l_MessagePosition == 0 || l_MessagePosition == 3);
    const float stackSpacingFactor = 1.5f;
    float offsetY = 0.0f;
    int messageIndex = 0;

    if (hasLiveReplayStatus)
    {
        // The live audience badge has a stable home independent of the user's
        // ordinary notification/chat anchor.
        ImGui::SetNextWindowPos(ImVec2(l_MessagePaddingX, l_MessagePaddingY),
                                ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::Begin("OSD Live Replay", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%s", liveReplayStatus.c_str());
        const ImVec2 windowSize = ImGui::GetWindowSize();
        ImGui::End();
        // Stack ordinary top-left messages beneath the persistent badge. Other
        // configured anchors have their own space and need no offset.
        if (l_MessagePosition == 1)
            offsetY += windowSize.y * stackSpacingFactor;
    }

    if (l_InputPromptActive)
    {
        const float posY = anchorBottom ? (baseY - offsetY) : (baseY + offsetY);
        ImGui::SetNextWindowPos(ImVec2(baseX, posY), ImGuiCond_Always, pivot);

        ImGui::Begin("OSD Input", nullptr, chatWindowFlags);
        if (maxWrapWidth > 0.0f)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWrapWidth);
        }
        ImGui::Text("%s", l_InputPrompt.c_str());
        if (maxWrapWidth > 0.0f)
        {
            ImGui::PopTextWrapPos();
        }
        const ImVec2 windowSize = ImGui::GetWindowSize();
        ImGui::End();

        offsetY += windowSize.y * stackSpacingFactor;
    }

    size_t renderedQueueCount = 0;
    for (auto messageIter = l_MessageQueue.rbegin(); messageIter != l_MessageQueue.rend(); ++messageIter, ++messageIndex)
    {
        if (renderedQueueCount >= visibleQueueCount)
        {
            break;
        }
        if (messageIter->type == OnScreenDisplayMessageType::Chat && !l_KailleraChatEnabled)
        {
            continue;
        }

        const float messageAgeSeconds = getMessageAgeSeconds(*messageIter);
        const float overflowFadeAlpha = getOverflowFadeAlpha(*messageIter);
        const float messageAlpha = getMessageFadeAlpha(messageAgeSeconds) * overflowFadeAlpha;
        if (messageAlpha <= 0.0f)
        {
            continue;
        }

        const float slideProgress = messageIter->skipSlideIn ? 1.0f : getMessageSlideProgress(messageAgeSeconds);
        float messageHeight = messageIter->lastWindowHeight;
        if (messageHeight <= 0.0f)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            messageHeight = ImGui::GetFontSize() + (style.WindowPadding.y * 2.0f);
        }
        const float slideOffsetY = (1.0f - slideProgress) * messageHeight;
        const float animatedOffsetY = offsetY - slideOffsetY;
        const float posY = anchorBottom ? (baseY - animatedOffsetY) : (baseY + animatedOffsetY);
        ImGui::SetNextWindowPos(ImVec2(baseX, posY), ImGuiCond_Always, pivot);

        const std::string windowName = "OSD Message##" + std::to_string(messageIndex);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, messageAlpha);
        ImGui::Begin(windowName.c_str(), nullptr, chatWindowFlags);
        if (maxWrapWidth > 0.0f)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWrapWidth);
        }
        ImGui::Text("%s", messageIter->message.c_str());
        if (maxWrapWidth > 0.0f)
        {
            ImGui::PopTextWrapPos();
        }
        const ImVec2 windowSize = ImGui::GetWindowSize();
        messageIter->lastWindowHeight = windowSize.y;
        ImGui::End();
        ImGui::PopStyleVar();

        const float stackContribution = windowSize.y * stackSpacingFactor * std::min(slideProgress, messageAlpha);
        offsetY += stackContribution;
        renderedQueueCount++;
    }

    ImGui::PopStyleColor(2);

    OnScreenDisplayRenderKailleraPortLabels();
    OnScreenDisplayRenderCenterMessage();

    ImGui::Render();

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void OnScreenDisplayPause(void)
{
    l_RenderingPaused = true;
}

void OnScreenDisplayResume(void)
{
    l_RenderingPaused = false;
}
