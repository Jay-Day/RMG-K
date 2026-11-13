#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

namespace KailleraExport
{

struct FfmpegEncoderConfig
{
    std::string ffmpegPath;
    std::string outputPath;
    std::string videoEncoder;
    int width = 640;
    int height = 480;
    double fps = 60.0;
    int crf = 23;
};

bool CheckFfmpegExecutable(const std::string& ffmpegPath, std::string* errorMessage);
bool MuxVideoAndAudio(const std::string& ffmpegPath,
                      const std::string& videoPath,
                      const std::string& audioPath,
                      unsigned int audioFrequency,
                      unsigned long long audioBytes,
                      int capturedFrames,
                      double fps,
                      const std::string& outputPath,
                      std::string* errorMessage);

class FfmpegEncoder
{
public:
    bool openEncoder(const FfmpegEncoderConfig& config, std::string* errorMessage);
    bool writeFrame(const uint8_t* rgbData, int width, int height, std::string* errorMessage);
    void closeEncoder();
    bool isHardwareAccelerated() const;
    const std::string& selectedVideoEncoder() const;

private:
    FILE* m_Pipe = nullptr;
    int m_FrameWidth = 0;
    int m_FrameHeight = 0;
    void* m_ProcessHandle = nullptr;
    std::string m_SelectedVideoEncoder;
    bool m_HardwareAccelerated = false;
};

} // namespace KailleraExport
