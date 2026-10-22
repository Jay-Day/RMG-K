#include "FrameCapture.hpp"

#include "PifReplay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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
static int s_QueueHead = 0;
static int s_QueueTail = 0;
static int s_QueueCount = 0;
static std::thread s_EncodeThread;
static std::mutex s_EncodeMutex;
static std::condition_variable s_EncodeWorkCv;
static std::condition_variable s_EncodeSpaceCv;
static PortLabelConfig s_PortLabelConfig;

using GlyphRows = std::array<uint8_t, 7>;

static GlyphRows glyphForChar(char c)
{
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c))))
    {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    default: return {0x1F, 0x01, 0x02, 0x04, 0x04, 0x00, 0x04};
    }
}

static void blendPixel(uint8_t* rgb, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
    const int inverseAlpha = 255 - alpha;
    rgb[0] = static_cast<uint8_t>((rgb[0] * inverseAlpha + red * alpha) / 255);
    rgb[1] = static_cast<uint8_t>((rgb[1] * inverseAlpha + green * alpha) / 255);
    rgb[2] = static_cast<uint8_t>((rgb[2] * inverseAlpha + blue * alpha) / 255);
}

static void fillRect(uint8_t* frame,
                     int width,
                     int height,
                     int x,
                     int y,
                     int rectWidth,
                     int rectHeight,
                     uint8_t red,
                     uint8_t green,
                     uint8_t blue,
                     uint8_t alpha)
{
    const int startX = std::clamp(x, 0, width);
    const int startY = std::clamp(y, 0, height);
    const int endX = std::clamp(x + rectWidth, 0, width);
    const int endY = std::clamp(y + rectHeight, 0, height);

    for (int py = startY; py < endY; ++py)
    {
        for (int px = startX; px < endX; ++px)
        {
            blendPixel(frame + (static_cast<size_t>(py) * width + px) * 3U, red, green, blue, alpha);
        }
    }
}

static int textWidth(const std::string& text, int scale)
{
    if (text.empty())
    {
        return 0;
    }
    return static_cast<int>(text.size()) * 6 * scale - scale;
}

static void drawText(uint8_t* frame,
                     int width,
                     int height,
                     int x,
                     int y,
                     const std::string& text,
                     int scale,
                     uint8_t red,
                     uint8_t green,
                     uint8_t blue)
{
    int cursorX = x;
    for (char c : text)
    {
        const GlyphRows glyph = glyphForChar(c);
        for (int row = 0; row < 7; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                if ((glyph[static_cast<size_t>(row)] & (1U << (4 - col))) == 0)
                {
                    continue;
                }
                fillRect(frame, width, height, cursorX + col * scale, y + row * scale,
                         scale, scale, red, green, blue, 255);
            }
        }
        cursorX += 6 * scale;
    }
}

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

static int chooseLabelTextScale(const std::string& label, int normalScale, int maxTextWidth)
{
    const int compactScale = std::max(1, normalScale - 1);
    if (textWidth(label, normalScale) <= maxTextWidth || compactScale == normalScale)
    {
        return normalScale;
    }

    return compactScale;
}

static void truncateLabelToFit(std::string& label, int scale, int maxTextWidth)
{
    const size_t maxChars = std::max<size_t>(1, static_cast<size_t>((maxTextWidth + scale) / (6 * scale)));
    if (label.size() > maxChars)
    {
        label.resize(maxChars);
    }
}

static void drawPortLabels(uint8_t* frame, int width, int height)
{
    if (!s_PortLabelConfig.enabled || s_PortLabelConfig.playerCount <= 0)
    {
        return;
    }

    const int playerCount = std::clamp(s_PortLabelConfig.playerCount, 1, 4);
    const int scale = std::max(2, height / 240);
    const int labelHeight = 15 * scale;
    const int labelY = std::max(0, height - labelHeight - 4 * scale);

    for (int i = 0; i < playerCount; ++i)
    {
        std::string label = normalizedLabelName(s_PortLabelConfig.playerNames[static_cast<size_t>(i)], i);
        const int portWidth = width / 4;
        const int portCenterX = portWidth * i + portWidth / 2 + scaledPortLabelOffset(i, height);
        const int maxTextWidth = std::max(scale, portWidth - 8 * scale);
        const int textScale = chooseLabelTextScale(label, scale, maxTextWidth);
        truncateLabelToFit(label, textScale, maxTextWidth);

        const int labelWidth = std::min(portWidth - 4 * scale, textWidth(label, textScale) + 8 * scale);
        const int labelX = std::clamp(portCenterX - labelWidth / 2, 0, std::max(0, width - labelWidth));
        fillRect(frame, width, height, labelX, labelY, labelWidth, labelHeight, 0, 0, 0, 170);

        const int textX = labelX + std::max(0, (labelWidth - textWidth(label, textScale)) / 2);
        const int textY = labelY + std::max(0, (labelHeight - 7 * textScale) / 2);
        drawText(frame, width, height, textX, textY, label, textScale, 255, 255, 255);
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
                            const PortLabelConfig& portLabelConfig)
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

    if (!queueFilledSlot(slotIndex))
    {
        s_Emulator->stop();
        return;
    }

    s_SubmittedFrames++;
}

} // namespace KailleraExport
