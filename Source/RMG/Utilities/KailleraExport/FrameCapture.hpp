#pragma once

#include "EmulatorProxy.hpp"
#include "FfmpegEncoder.hpp"
#include "KrecParser.hpp"

#include <array>
#include <string>
#include <vector>

namespace KailleraExport
{

struct PortLabelConfig
{
    bool enabled = false;
    int playerCount = 0;
    std::array<std::string, 4> playerNames;
    // the names change as players drop or join late
    std::vector<KrecPortChange> changes;
};

void InitializeFrameCapture(EmulatorProxy* emulator,
                            FfmpegEncoder* encoder,
                            const FfmpegEncoderConfig& encoderConfig,
                            int expectedFrameCount,
                            const PortLabelConfig& portLabelConfig = {},
                            bool chatOverlay = false);
void FrameCaptureCallback(unsigned int frameIndex);
void FlushFrameCapture();
bool GetFrameCaptureError(std::string* errorMessage);
int GetCapturedFrameCount();

} // namespace KailleraExport
