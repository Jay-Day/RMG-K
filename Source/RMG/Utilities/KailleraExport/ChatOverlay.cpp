#include "ChatOverlay.hpp"

#include <RMG-Core/Settings.hpp>

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace KailleraExport
{

namespace
{

// the OSD's timings (OnScreenDisplay.cpp)
static constexpr double kFadeOutSeconds = 0.26;
static constexpr double kSlideInSeconds = 0.31;
static constexpr double kStackSpacing = 1.5;
static constexpr double kBaseFontSize = 13.0;
static constexpr double kWindowPadding = 8.0;

struct ChatLine
{
    int record = 0;
    QString text;
    int shownAt = -1; // output frame, guarded by s_Mutex
    // encode thread only: the line's measured text size
    double textWidth = -1.0;
    double textHeight = 0.0;
};

static std::mutex s_Mutex;
static std::vector<ChatLine> s_Lines;
static size_t s_NextLine = 0; // first line not shown yet, guarded by s_Mutex
static double s_Fps = 60.0;
static bool s_PortLabels = false;

// the user's OSD settings
static int s_Position = 0;
static int s_PaddingX = 20;
static int s_PaddingY = 20;
static double s_DurationSeconds = 7.0;
static double s_Scale = 1.25;
static size_t s_MaxMessages = 5;
static QColor s_BackgroundColor(0, 0, 0, 180);
static QColor s_TextColor(255, 255, 255, 255);

// the application font, taken on the main thread; the sized copy is the
// encode thread's
static std::unique_ptr<QFont> s_BaseFont;
static std::unique_ptr<QFont> s_Font;

static double easeOutCubic(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

static double easeInFadeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - t * t;
}

static QColor settingsColor(SettingsID id, const QColor& fallback)
{
    const std::vector<int> rgba = CoreSettingsGetIntListValue(id);
    if (rgba.size() != 4)
    {
        return fallback;
    }
    return QColor(std::clamp(rgba[0], 0, 255), std::clamp(rgba[1], 0, 255),
                  std::clamp(rgba[2], 0, 255), std::clamp(rgba[3], 0, 255));
}

static QColor withAlpha(QColor color, double alpha)
{
    color.setAlphaF(static_cast<float>(color.alphaF() * std::clamp(alpha, 0.0, 1.0)));
    return color;
}

// a chat line as the in-game overlay shows it
static QString chatText(const KrecChatLine& line)
{
    QString message = QString::fromUtf8(line.message).trimmed();
    message.replace('\r', ' ');
    message.replace('\n', ' ');
    return "<" + QString::fromUtf8(line.nick).trimmed() + "> " + message;
}

} // namespace

void InitializeChatOverlay(const std::vector<KrecChatLine>& lines, double fps, bool portLabels)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Lines.clear();
    for (const KrecChatLine& line : lines)
    {
        ChatLine chatLine;
        chatLine.record = line.record;
        chatLine.text = chatText(line);
        s_Lines.push_back(std::move(chatLine));
    }
    s_NextLine = 0;
    s_Fps = fps > 0.0 ? fps : 60.0;
    s_PortLabels = portLabels;

    s_Position = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayLocation);
    s_PaddingX = std::max(0, CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingX));
    s_PaddingY = std::max(0, CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingY));
    s_DurationSeconds = std::max(0, CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayDuration));
    s_Scale = CoreSettingsGetFloatValue(SettingsID::GUI_OnScreenDisplayScale);
    if (s_Scale <= 0.1)
    {
        s_Scale = 1.0;
    }
    s_MaxMessages = static_cast<size_t>(std::max(1, CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayMaxMessages)));
    s_BackgroundColor = settingsColor(SettingsID::GUI_OnScreenDisplayBackgroundColor, QColor(0, 0, 0, 180));
    s_TextColor = settingsColor(SettingsID::GUI_OnScreenDisplayTextColor, QColor(255, 255, 255, 255));
    s_BaseFont = std::make_unique<QFont>(QApplication::font());
    s_Font.reset();
}

void AdvanceChatOverlay(int inputRecords, int frame)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    while (s_NextLine < s_Lines.size() && s_Lines[s_NextLine].record < inputRecords)
    {
        s_Lines[s_NextLine++].shownAt = frame;
    }
}

void DrawChatOverlay(uint8_t* rgb, int width, int height, int frame)
{
    struct VisibleLine
    {
        ChatLine* line;
        double alpha;
        double slide;
    };
    std::vector<VisibleLine> visible;
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        // newest first; the encoder may run a frame or two behind the replay
        std::vector<ChatLine*> shown;
        for (size_t i = s_NextLine; i-- > 0;)
        {
            ChatLine& line = s_Lines[i];
            if (line.shownAt > frame)
            {
                continue;
            }
            const double age = (frame - line.shownAt) / s_Fps;
            if (age > s_DurationSeconds + kFadeOutSeconds + kSlideInSeconds)
            {
                break; // and every older line
            }
            shown.push_back(&line);
        }
        for (size_t i = 0; i < shown.size(); i++)
        {
            const double age = (frame - shown[i]->shownAt) / s_Fps;
            double alpha = age <= s_DurationSeconds ? 1.0 : easeInFadeOut((age - s_DurationSeconds) / kFadeOutSeconds);
            // past the message limit a line fades out from when the line that
            // pushed it out appeared
            if (i >= s_MaxMessages)
            {
                const double evicted = (frame - shown[i - s_MaxMessages]->shownAt) / s_Fps;
                alpha *= easeInFadeOut(evicted / kSlideInSeconds);
            }
            if (alpha > 0.0)
            {
                visible.push_back({shown[i], alpha, easeOutCubic(age / kSlideInSeconds)});
            }
        }
    }
    if (visible.empty() || s_BaseFont == nullptr)
    {
        return;
    }

    // sized for the video: the in-game overlay's pixels at 720 lines
    const double k = height / 720.0;
    const int fontHeight = std::max(8, static_cast<int>(std::lround(kBaseFontSize * s_Scale * k)));
    if (s_Font == nullptr || s_Font->pixelSize() != fontHeight)
    {
        s_Font = std::make_unique<QFont>(*s_BaseFont);
        s_Font->setPixelSize(fontHeight);
        for (ChatLine& line : s_Lines)
        {
            line.textWidth = -1.0;
        }
    }
    const double padding = kWindowPadding * k;
    const double border = std::max(1.0, k);
    const double paddingX = s_PaddingX * k;
    const double paddingY = s_PaddingY * k;
    const double wrapWidth = std::max(1.0, width - paddingX * 2.0);

    // the port labels' strip (see FrameCapture.cpp's drawPortLabels)
    const int labelScale = std::max(2, height / 240);
    const double bottomReserve = s_PortLabels ? 19.0 * labelScale : 0.0;

    // 0 left bottom (also any unknown value, like the OSD), 1 left top,
    // 2 right top, 3 right bottom
    const bool anchorRight = s_Position == 2 || s_Position == 3;
    const bool anchorBottom = s_Position != 1 && s_Position != 2;
    const double baseX = anchorRight ? width - paddingX : paddingX;
    const double baseY = anchorBottom ? height - paddingY - bottomReserve : paddingY;

    QImage image(rgb, width, height, width * 3, QImage::Format_RGB888);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(*s_Font);
    const QFontMetricsF metrics(*s_Font);
    const QColor borderColor(110, 110, 128, 128); // ImGui's dark style border

    double offsetY = 0.0;
    for (const VisibleLine& entry : visible)
    {
        ChatLine& line = *entry.line;
        if (line.textWidth < 0.0)
        {
            const QRectF bounds = metrics.boundingRect(QRectF(0, 0, wrapWidth, 1e6), Qt::TextWordWrap, line.text);
            line.textWidth = std::ceil(bounds.width());
            line.textHeight = std::ceil(bounds.height());
        }
        const double boxWidth = line.textWidth + padding * 2.0;
        const double boxHeight = line.textHeight + padding * 2.0;
        const double animatedOffsetY = offsetY - (1.0 - entry.slide) * boxHeight;
        const double x = anchorRight ? baseX - boxWidth : baseX;
        const double y = anchorBottom ? baseY - animatedOffsetY - boxHeight : baseY + animatedOffsetY;
        const QRectF box(x, y, boxWidth, boxHeight);

        painter.fillRect(box, withAlpha(s_BackgroundColor, entry.alpha));
        painter.setPen(QPen(withAlpha(borderColor, entry.alpha), border));
        painter.drawRect(box.adjusted(border / 2, border / 2, -border / 2, -border / 2));
        painter.setPen(withAlpha(s_TextColor, entry.alpha));
        painter.drawText(QRectF(x + padding, y + padding, wrapWidth, line.textHeight + 1.0),
                         Qt::TextWordWrap, line.text);

        offsetY += boxHeight * kStackSpacing * std::min(entry.slide, entry.alpha);
    }
}

void ShutdownChatOverlay()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Lines.clear();
    s_NextLine = 0;
    s_Font.reset();
    s_BaseFont.reset();
}

} // namespace KailleraExport
