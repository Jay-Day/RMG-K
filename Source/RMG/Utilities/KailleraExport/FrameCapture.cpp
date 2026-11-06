#include "FrameCapture.hpp"

#include "ChatOverlay.hpp"
#include "PifReplay.hpp"

#include <RMG-Core/Directories.hpp>

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace KailleraExport
{

namespace
{

enum class EncodeSlotState
{
    Free,
    Filling,
    Queued,
    Encoding,
};

static constexpr int kEncodeSlotCount = 2;
static constexpr std::array<int, 4> kPortLabelCenterOffset720p = { 19, 0, -19, -70 };

static EmulatorProxy* s_Emulator = nullptr;
static FfmpegEncoder* s_Encoder = nullptr;
static FfmpegEncoderConfig s_EncoderConfig;
static bool s_EncoderOpened = false;
static std::atomic<int> s_CapturedFrames = 0;
static int s_SubmittedFrames = 0;
static int s_ExpectedFrameCount = 0;
static bool s_SpeedLimiterDisabled = false;
static bool s_EncodeThreadStarted = false;
static bool s_EncodeShutdown = false;
static std::atomic<bool> s_EncodeFailed = false;
static std::string s_EncodeErrorMessage;
static int s_FrameWidth = 0;
static int s_FrameHeight = 0;
static EncodeSlotState s_SlotStates[kEncodeSlotCount] = {
    EncodeSlotState::Free,
    EncodeSlotState::Free,
};
static std::vector<uint8_t> s_RawBuffers[kEncodeSlotCount];
static std::vector<uint8_t> s_FlippedBuffer;
static int s_QueuedSlotIndices[kEncodeSlotCount] = { 0, 0 };
static int s_SlotFrames[kEncodeSlotCount] = { 0, 0 }; // output frame in each slot
static int s_QueueHead = 0;
static int s_QueueTail = 0;
static int s_QueueCount = 0;
static std::thread s_EncodeThread;
static std::mutex s_EncodeMutex;
static std::condition_variable s_EncodeWorkCv;
static std::condition_variable s_EncodeSpaceCv;
static PortLabelConfig s_PortLabelConfig;
static bool s_ChatOverlay = false;

// The labels for one frame size, drawn once. Qt lays the text out, so any script
// works and fonts it lacks are found on the system. Only the strip they sit in is kept.
static QImage s_PortLabelStrip;
static int s_PortLabelStripY = 0;
static int s_PortLabelFrameHeight = 0;

static std::string normalizedLabelName(const std::string& name, int playerIndex)
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

static int scaledPortLabelOffset(int playerIndex, int height)
{
    if (playerIndex < 0 || playerIndex >= static_cast<int>(kPortLabelCenterOffset720p.size()))
    {
        return 0;
    }

    const int offset = kPortLabelCenterOffset720p[static_cast<size_t>(playerIndex)];
    if (offset >= 0)
    {
        return (offset * height + 360) / 720;
    }

    return -((-offset * height + 360) / 720);
}

static QFont portLabelFont(int pixelSize)
{
    static const QString family = []() {
        const int id = QFontDatabase::addApplicationFont(
            QString::fromStdString((CoreGetSharedDataDirectory() / "font.ttf").string()));
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QFont().family() : families.first();
    }();

    QFont font(family);
    font.setPixelSize(pixelSize);
    return font;
}

static void renderPortLabelStrip(int width, int height)
{
    const int playerCount = std::clamp(s_PortLabelConfig.playerCount, 1, 4);
    const int scale = std::max(2, height / 240);
    const int labelHeight = 15 * scale;
    const int portWidth = width / 4;
    const int maxTextWidth = std::max(scale, portWidth - 8 * scale);

    s_PortLabelFrameHeight = height;
    s_PortLabelStripY = std::max(0, height - labelHeight - 4 * scale);
    s_PortLabelStrip = QImage(width, std::min(labelHeight, height - s_PortLabelStripY), QImage::Format_ARGB32_Premultiplied);
    s_PortLabelStrip.fill(Qt::transparent);

    QPainter painter(&s_PortLabelStrip);
    painter.setPen(Qt::white);
    for (int i = 0; i < playerCount; ++i)
    {
        QString label = QString::fromStdString(normalizedLabelName(s_PortLabelConfig.playerNames[static_cast<size_t>(i)], i));

        QFont font = portLabelFont(10 * scale);
        if (QFontMetrics(font).horizontalAdvance(label) > maxTextWidth)
        {
            font = portLabelFont(8 * scale);
            label = QFontMetrics(font).elidedText(label, Qt::ElideRight, maxTextWidth);
        }

        const int portCenterX = portWidth * i + portWidth / 2 + scaledPortLabelOffset(i, height);
        const int labelWidth = std::min(portWidth - 4 * scale, QFontMetrics(font).horizontalAdvance(label) + 8 * scale);
        const int labelX = std::clamp(portCenterX - labelWidth / 2, 0, std::max(0, width - labelWidth));
        const QRect box(labelX, 0, labelWidth, labelHeight);

        painter.fillRect(box, QColor(0, 0, 0, 170));
        painter.setFont(font);
        painter.drawText(box, Qt::AlignCenter, label);
    }
}

static void drawPortLabels(uint8_t* frame, int width, int height)
{
    if (!s_PortLabelConfig.enabled || s_PortLabelConfig.playerCount <= 0)
    {
        return;
    }

    if (s_PortLabelStrip.isNull() || s_PortLabelStrip.width() != width || s_PortLabelFrameHeight != height)
    {
        renderPortLabelStrip(width, height);
    }

    for (int y = 0; y < s_PortLabelStrip.height(); ++y)
    {
        const QRgb* source = reinterpret_cast<const QRgb*>(s_PortLabelStrip.constScanLine(y));
        uint8_t* rgb = frame + static_cast<size_t>(y + s_PortLabelStripY) * width * 3U;
        for (int x = 0; x < width; ++x, rgb += 3)
        {
            const int alpha = qAlpha(source[x]);
            if (alpha == 0)
            {
                continue;
            }
            // the strip's colors are premultiplied
            const int inverseAlpha = 255 - alpha;
            rgb[0] = static_cast<uint8_t>(qRed(source[x]) + rgb[0] * inverseAlpha / 255);
            rgb[1] = static_cast<uint8_t>(qGreen(source[x]) + rgb[1] * inverseAlpha / 255);
            rgb[2] = static_cast<uint8_t>(qBlue(source[x]) + rgb[2] * inverseAlpha / 255);
        }
    }
}

static bool allSlotsIdleLocked()
{
    if (s_QueueCount != 0)
    {
        return false;
    }

    for (int i = 0; i < kEncodeSlotCount; ++i)
    {
        if (s_SlotStates[i] != EncodeSlotState::Free)
        {
            return false;
        }
    }

    return true;
}

static void releaseSlot(int slotIndex)
{
    {
        std::lock_guard<std::mutex> lock(s_EncodeMutex);
        s_SlotStates[slotIndex] = EncodeSlotState::Free;
    }
    s_EncodeSpaceCv.notify_all();
}

static void notifyEncodeFailure(const std::string& message)
{
    bool shouldStopEmulator = false;
    {
        std::lock_guard<std::mutex> lock(s_EncodeMutex);
        if (!s_EncodeFailed.load())
        {
            s_EncodeFailed.store(true);
            s_EncodeShutdown = true;
            s_EncodeErrorMessage = message;
            s_QueueHead = 0;
            s_QueueTail = 0;
            s_QueueCount = 0;
            for (int i = 0; i < kEncodeSlotCount; ++i)
            {
                if (s_SlotStates[i] == EncodeSlotState::Queued)
                {
                    s_SlotStates[i] = EncodeSlotState::Free;
                }
            }
            shouldStopEmulator = true;
        }
    }

    s_EncodeWorkCv.notify_all();
    s_EncodeSpaceCv.notify_all();

    if (shouldStopEmulator && s_Emulator != nullptr)
    {
        s_Emulator->stop();
    }
}

static int reserveFreeSlot()
{
    std::unique_lock<std::mutex> lock(s_EncodeMutex);
    s_EncodeSpaceCv.wait(lock, []()
    {
        if (s_EncodeFailed.load())
        {
            return true;
        }

        for (int i = 0; i < kEncodeSlotCount; ++i)
        {
            if (s_SlotStates[i] == EncodeSlotState::Free)
            {
                return true;
            }
        }

        return false;
    });

    if (s_EncodeFailed.load())
    {
        return -1;
    }

    for (int i = 0; i < kEncodeSlotCount; ++i)
    {
        if (s_SlotStates[i] == EncodeSlotState::Free)
        {
            s_SlotStates[i] = EncodeSlotState::Filling;
            return i;
        }
    }

    return -1;
}

static bool queueFilledSlot(int slotIndex)
{
    {
        std::lock_guard<std::mutex> lock(s_EncodeMutex);
        if (s_EncodeFailed.load())
        {
            s_SlotStates[slotIndex] = EncodeSlotState::Free;
            return false;
        }

        s_SlotStates[slotIndex] = EncodeSlotState::Queued;
        s_QueuedSlotIndices[s_QueueTail] = slotIndex;
        s_QueueTail = (s_QueueTail + 1) % kEncodeSlotCount;
        s_QueueCount++;
    }

    s_EncodeWorkCv.notify_one();
    return true;
}

static void EncodeWorkerMain()
{
    for (;;)
    {
        int slotIndex = -1;
        {
            std::unique_lock<std::mutex> lock(s_EncodeMutex);
            s_EncodeWorkCv.wait(lock, []()
            {
                return s_EncodeShutdown || s_QueueCount > 0;
            });

            if (s_QueueCount == 0)
            {
                if (s_EncodeShutdown)
                {
                    return;
                }
                continue;
            }

            slotIndex = s_QueuedSlotIndices[s_QueueHead];
            s_QueueHead = (s_QueueHead + 1) % kEncodeSlotCount;
            s_QueueCount--;
            s_SlotStates[slotIndex] = EncodeSlotState::Encoding;
        }

        const int width = s_FrameWidth;
        const int height = s_FrameHeight;
        if (width <= 0 || height <= 0)
        {
            notifyEncodeFailure("Replay export encountered an invalid frame size");
            releaseSlot(slotIndex);
            return;
        }

        const int stride = width * 3;
        const size_t frameSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
        if (s_FlippedBuffer.size() < frameSize)
        {
            s_FlippedBuffer.resize(frameSize);
        }

        const std::vector<uint8_t>& rawBuffer = s_RawBuffers[slotIndex];
        for (int y = 0; y < height; ++y)
        {
            memcpy(s_FlippedBuffer.data() + y * stride,
                   rawBuffer.data() + (height - 1 - y) * stride,
                   static_cast<size_t>(stride));
        }

        drawPortLabels(s_FlippedBuffer.data(), width, height);
        if (s_ChatOverlay)
        {
            DrawChatOverlay(s_FlippedBuffer.data(), width, height, s_SlotFrames[slotIndex]);
        }

        std::string errorMessage;
        if (!s_Encoder->writeFrame(s_FlippedBuffer.data(), width, height, &errorMessage))
        {
            notifyEncodeFailure(errorMessage);
            releaseSlot(slotIndex);
            return;
        }

        const int capturedFrames = s_CapturedFrames.fetch_add(1) + 1;
        if ((capturedFrames % 60) == 0)
        {
            if (s_ExpectedFrameCount > 0)
            {
                fprintf(stderr,
                        "Captured %d / %d frames...\n",
                        capturedFrames,
                        s_ExpectedFrameCount);
            }
            else
            {
                fprintf(stderr, "Captured %d frames...\n", capturedFrames);
            }
        }

        releaseSlot(slotIndex);
    }
}

static void startEncodeThread()
{
    if (s_EncodeThreadStarted)
    {
        return;
    }

    s_EncodeShutdown = false;
    s_EncodeThread = std::thread(EncodeWorkerMain);
    s_EncodeThreadStarted = true;
}

static void waitForEncodeThread()
{
    std::unique_lock<std::mutex> lock(s_EncodeMutex);
    s_EncodeSpaceCv.wait(lock, []()
    {
        return s_EncodeFailed.load() || allSlotsIdleLocked();
    });
}

static void stopEncodeThread()
{
    if (!s_EncodeThreadStarted)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_EncodeMutex);
        s_EncodeShutdown = true;
    }
    s_EncodeWorkCv.notify_all();

    if (s_EncodeThread.joinable())
    {
        s_EncodeThread.join();
    }

    s_EncodeThreadStarted = false;
}

} // namespace

void InitializeFrameCapture(EmulatorProxy* emulator,
                            FfmpegEncoder* encoder,
                            const FfmpegEncoderConfig& encoderConfig,
                            int expectedFrameCount,
                            const PortLabelConfig& portLabelConfig,
                            bool chatOverlay)
{
    s_Emulator = emulator;
    s_Encoder = encoder;
    s_EncoderConfig = encoderConfig;
    s_EncoderOpened = false;
    s_CapturedFrames.store(0);
    s_SubmittedFrames = 0;
    s_ExpectedFrameCount = expectedFrameCount;
    s_SpeedLimiterDisabled = false;
    s_EncodeThreadStarted = false;
    s_EncodeShutdown = false;
    s_EncodeFailed.store(false);
    s_EncodeErrorMessage.clear();
    s_FrameWidth = 0;
    s_FrameHeight = 0;
    s_QueueHead = 0;
    s_QueueTail = 0;
    s_QueueCount = 0;
    s_PortLabelConfig = portLabelConfig;
    s_PortLabelStrip = QImage();
    s_ChatOverlay = chatOverlay;
    for (int i = 0; i < kEncodeSlotCount; ++i)
    {
        s_SlotStates[i] = EncodeSlotState::Free;
        s_RawBuffers[i].clear();
    }
    s_FlippedBuffer.clear();
}

int GetCapturedFrameCount()
{
    return s_CapturedFrames.load();
}

void FlushFrameCapture()
{
    waitForEncodeThread();
    stopEncodeThread();
    for (int i = 0; i < kEncodeSlotCount; ++i)
    {
        s_RawBuffers[i].clear();
        s_SlotStates[i] = EncodeSlotState::Free;
    }
    s_FlippedBuffer.clear();
}

bool GetFrameCaptureError(std::string* errorMessage)
{
    std::lock_guard<std::mutex> lock(s_EncodeMutex);
    if (!s_EncodeFailed.load())
    {
        return false;
    }

    if (errorMessage != nullptr)
    {
        *errorMessage = s_EncodeErrorMessage;
    }
    return true;
}

void FrameCaptureCallback(unsigned int)
{
    if (s_Emulator == nullptr || s_Encoder == nullptr)
    {
        return;
    }

    if (s_EncodeFailed.load())
    {
        s_Emulator->stop();
        return;
    }

    ResetPifReplayFrameSync();
    if (IsPifReplayFinished())
    {
        s_Emulator->stop();
        return;
    }

    if (s_ExpectedFrameCount > 0 &&
        s_SubmittedFrames >= (s_ExpectedFrameCount + 120))
    {
        fprintf(stderr,
                "Replay export reached safety frame limit (%d captured, %d expected), stopping.\n",
                s_SubmittedFrames,
                s_ExpectedFrameCount);
        s_Emulator->stop();
        return;
    }

    int width = s_FrameWidth;
    int height = s_FrameHeight;
    if (width <= 0 || height <= 0)
    {
        s_Emulator->readScreen(nullptr, &width, &height);
        if (width <= 0 || height <= 0)
        {
            return;
        }
    }

    if (!s_EncoderOpened)
    {
        s_EncoderConfig.width = width;
        s_EncoderConfig.height = height;
        s_FrameWidth = width;
        s_FrameHeight = height;

        std::string errorMessage;
        if (!s_Encoder->open(s_EncoderConfig, &errorMessage))
        {
            fprintf(stderr, "%s\n", errorMessage.c_str());
            s_Emulator->stop();
            return;
        }
        startEncodeThread();
        s_EncoderOpened = true;
    }

    if (!s_SpeedLimiterDisabled)
    {
        // Run as fast as possible: waiting for a free encode slot below holds
        // emulation back whenever the encoder falls behind.
        int limiter = 0;
        s_Emulator->coreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &limiter);
        s_SpeedLimiterDisabled = true;
    }

    if (s_ChatOverlay)
    {
        AdvanceChatOverlay(GetPifReplayFrameIndex(), s_SubmittedFrames);
    }

    const int slotIndex = reserveFreeSlot();
    if (slotIndex < 0)
    {
        s_Emulator->stop();
        return;
    }

    const size_t frameSize = static_cast<size_t>(s_FrameWidth) * static_cast<size_t>(s_FrameHeight) * 3U;
    if (s_RawBuffers[slotIndex].size() < frameSize)
    {
        s_RawBuffers[slotIndex].resize(frameSize);
    }

    width = s_FrameWidth;
    height = s_FrameHeight;
    s_Emulator->readScreen(s_RawBuffers[slotIndex].data(), &width, &height);
    if (width <= 0 || height <= 0)
    {
        releaseSlot(slotIndex);
        return;
    }

    if (width != s_FrameWidth || height != s_FrameHeight)
    {
        releaseSlot(slotIndex);
        notifyEncodeFailure("Replay export render size changed unexpectedly");
        s_Emulator->stop();
        return;
    }

    s_SlotFrames[slotIndex] = s_SubmittedFrames;
    if (!queueFilledSlot(slotIndex))
    {
        s_Emulator->stop();
        return;
    }

    s_SubmittedFrames++;
}

} // namespace KailleraExport
