#include "FfmpegEncoder.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <locale.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif

namespace KailleraExport
{

namespace
{

#ifndef _WIN32
static const int READ_PIPE = 0;
static const int WRITE_PIPE = 1;
#endif


struct EncoderCandidate
{
    const char* codec;
    bool hardware;
};

static constexpr EncoderCandidate kPreferredVideoEncoders[] = {
    { "h264_nvenc", true },
    { "h264_qsv", true },
    { "h264_amf", true },
    { "libx264", false },
};

static bool fail(std::string* errorMessage, const std::string& message)
{
    if (errorMessage != nullptr)
    {
        *errorMessage = message;
    }
    return false;
}

#ifdef _WIN32
static bool runCommandWithCapturedOutput(const std::string& command,
                                         DWORD timeoutMs,
                                         bool requireOutput,
                                         DWORD* exitCode)
{
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, 0))
    {
        return false;
    }

    SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = writeHandle;
    startupInfo.hStdError = writeHandle;

    PROCESS_INFORMATION processInfo = {};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    const BOOL created = CreateProcessA(nullptr,
                                        mutableCommand.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startupInfo,
                                        &processInfo);
    CloseHandle(writeHandle);

    if (!created)
    {
        CloseHandle(readHandle);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);
    if (waitResult != WAIT_OBJECT_0)
    {
        TerminateProcess(processInfo.hProcess, 1);
    }

    bool gotOutput = false;
    char buffer[256];
    DWORD bytesRead = 0;
    while (ReadFile(readHandle, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        gotOutput = true;
    }
    CloseHandle(readHandle);

    DWORD localExitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &localExitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    if (exitCode != nullptr)
    {
        *exitCode = localExitCode;
    }

    if (waitResult != WAIT_OBJECT_0)
    {
        return false;
    }

    if (requireOutput && !gotOutput)
    {
        return false;
    }

    return localExitCode == 0;
}
#endif

#ifndef _WIN32
bool execWithArgs(const std::vector<std::string> args)
{
    std::vector<const char*> commandArgs;
    commandArgs.reserve(args.size() + 1);

    for (int i = 0; i < args.size(); i++)
    {
        commandArgs.push_back(args[i].c_str());
    }

    commandArgs.push_back(nullptr);

    //The first element in the array must always be the program name, and the last argument must be null
    return execvp(commandArgs[0], const_cast<char* const*>(commandArgs.data())) != -1;
}

static bool runCommandWithCapturedOutputPosix(const std::vector<std::string> arguments,
                                         int timeoutMs,
                                         bool requireOutput,
                                         int* exitCode)
{
    int rwPipe[2];

    if (pipe(rwPipe) == -1)
    {
        return false;
    }

    pid_t childPid = fork();

    if (childPid == -1)
    {
        return false;
    }

    if (childPid == 0)
    {
        //Replace stdout and stderr in the child with the pipes
        dup2(rwPipe[WRITE_PIPE], STDOUT_FILENO);
        dup2(rwPipe[WRITE_PIPE], STDERR_FILENO);
        close(rwPipe[READ_PIPE]);
        close(rwPipe[WRITE_PIPE]);

        if (execWithArgs(arguments) == false)
        {
            return false;
        }
    }
    //Parent process
    else {
        //Close unused write pipe
        close(rwPipe[WRITE_PIPE]);

        int fd = syscall(SYS_pidfd_open, childPid, 0);
        struct pollfd pfd = {fd, POLL_IN, 0};

        //Wait for the child process to exit in the given time
        int pollVal = poll(&pfd, 1, timeoutMs);

        //Timeout
        if (pollVal == 0)
        {
            //Kill process
            kill(childPid, SIGTERM);
            return false;
        }
        //Error
        else if (pollVal == -1)
        {
            return false;
        }

        //Wait for the child process to exit
        int childExitCode;
        waitpid(childPid, &childExitCode, 0);

        if (WIFEXITED(childExitCode))
        {
            int exitStatus = WEXITSTATUS(childExitCode);

            if (exitCode != nullptr)
            {
                *exitCode = exitStatus;
            }
        }

        bool gotOutput = false;

        //Read from the pipe
        char output[256];
        const int readBytes = read(rwPipe[READ_PIPE], &output, sizeof(output));

        if (readBytes > 0)
        {
            gotOutput = true;
        }

        //Close no longer needed pipe
        close(rwPipe[READ_PIPE]);

        if (requireOutput && !gotOutput)
        {
            return false;
        }

        return childExitCode == 0;
    }

    return true;
}
#endif

static bool probeVideoEncoder(const std::string& ffmpegPath, const char* codec)
{
#ifdef _WIN32
    // AMD AMF requires at least 128x96 to initialize; 64x64 silently fails on
    // AMD systems even when the driver is fine. 320x240 clears all known
    // hardware-encoder minimums (NVENC, QSV, AMF). yuv420p is forced because
    // the real export pipeline converts to it and some hardware encoders
    // refuse lavfi's default surface format. The 10s timeout covers AMF's
    // cold-init latency on first launch.
    char command[512];
    snprintf(command,
             sizeof(command),
             "\"%s\" -v error -f lavfi -i color=black:s=320x240:r=30:d=0.1 -frames:v 1 "
             "-pix_fmt yuv420p -c:v %s -f null -",
             ffmpegPath.c_str(),
             codec);

    return runCommandWithCapturedOutput(command, 10000, false, nullptr);
#else
    std::vector<std::string> args =
    {
      ffmpegPath, "-v", "error", "-f", "lavfi", "-i", "color=black:s=320x240:r=30:d=0.1", "-frames:v", "1",
      "-pix_fmt", "yuv420p", "-c:v", codec, "-f", "null", "-"
    };

    return runCommandWithCapturedOutputPosix(args, 10000, false, nullptr);
#endif
}

static std::string chooseVideoEncoder(const std::string& ffmpegPath, bool* hardwareAccelerated)
{
    for (const EncoderCandidate& candidate : kPreferredVideoEncoders)
    {
        if (probeVideoEncoder(ffmpegPath, candidate.codec))
        {
            if (hardwareAccelerated != nullptr)
            {
                *hardwareAccelerated = candidate.hardware;
            }
            return candidate.codec;
        }
    }

    if (hardwareAccelerated != nullptr)
    {
        *hardwareAccelerated = false;
    }
    return {};
}

#ifndef _WIN32
static std::vector<std::string> buildVideoEncoderFlagsPosix(const std::string& videoEncoder, int crf)
{
    const std::string crfStr = std::to_string(crf);

    if (videoEncoder == "h264_nvenc")
    {
        std::vector<std::string> args =
        {
            "-c:v", "h264_nvenc", "-preset", "fast", "-rc", "vbr", "-cq", crfStr, "-pix_fmt", "yuv420p"
        };
        return args;
    }

    if (videoEncoder == "h264_amf")
    {
        std::vector<std::string> args =
        {
            "-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", crfStr,
            "qp_p", crfStr, "-pix_fmt", "yuv420p"
        };
        return args;
    }

    if (videoEncoder == "h264_qsv")
    {
        std::vector<std::string> args = { "-c:v", "h264_qsv", "-global_quality", crfStr, "-look_ahead", "0" };
        return args;
    }

    std::vector<std::string> args = { "-c:v", "libx264", "-preset", "ultrafast", "-crf", crfStr, "-pix_fmt", "yuv420p" };
    return args;
}
#endif

#ifdef _WIN32
static std::string buildVideoEncoderFlags(const std::string& videoEncoder, int crf)
{
    char buffer[256];

    if (videoEncoder == "h264_nvenc")
    {
        snprintf(buffer,
                 sizeof(buffer),
                 "-c:v h264_nvenc -preset fast -rc vbr -cq %d -pix_fmt yuv420p",
                 crf);
        return buffer;
    }

    if (videoEncoder == "h264_amf")
    {
        snprintf(buffer,
                 sizeof(buffer),
                 "-c:v h264_amf -quality quality -rc cqp -qp_i %d -qp_p %d -pix_fmt yuv420p",
                 crf,
                 crf);
        return buffer;
    }

    if (videoEncoder == "h264_qsv")
    {
        snprintf(buffer,
                 sizeof(buffer),
                 "-c:v h264_qsv -global_quality %d -look_ahead 0",
                 crf);
        return buffer;
    }

    snprintf(buffer,
             sizeof(buffer),
             "-c:v libx264 -preset ultrafast -crf %d -pix_fmt yuv420p",
             crf);
    return buffer;
}
#endif

} // namespace

bool CheckFfmpegExecutable(const std::string& ffmpegPath, std::string* errorMessage)
{
#ifdef _WIN32
    std::string command = "\"" + ffmpegPath + "\" -version";

    DWORD exitCode = 1;
    if (!runCommandWithCapturedOutput(command, 5000, true, &exitCode) || exitCode != 0)
#else
    std::vector<std::string> args = { ffmpegPath, "-version" };

    int exitCode = 1;
    if (!runCommandWithCapturedOutputPosix(args, 5000, true, &exitCode) || exitCode != 0)
#endif
    {
        return fail(errorMessage, "FFmpeg was not found");
    }

    if (chooseVideoEncoder(ffmpegPath, nullptr).empty())
    {
        return fail(errorMessage, "FFmpeg does not provide a usable H.264 encoder for MP4 export");
    }

    return true;
}

#ifdef _WIN32
static bool runFfmpegCommand(const std::string& command, std::string* errorMessage)
{
    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION processInfo = {};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    if (!CreateProcessA(nullptr,
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startupInfo,
                        &processInfo))
    {
        return fail(errorMessage, "Failed to run FFmpeg");
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    if (exitCode != 0)
    {
        return fail(errorMessage, "FFmpeg exited with an error");
    }

    return true;
}
#endif

#ifndef _WIN32
static bool runFfmpegCommandPosix(const std::vector<std::string> arguments, std::string* errorMessage)
{
    pid_t childPid = fork();

    if (childPid == -1)
    {
        return fail(errorMessage, "Failed to create fork for FFmpeg process");
    }

    if (childPid == 0)
    {
        if (execWithArgs(arguments) == false)
        {
            return fail(errorMessage, "Error executing FFmpeg process");
        }
    }
    else
    {
        int exitCode;
        if (waitpid(childPid, &exitCode, 0) == -1)
        {
            return fail(errorMessage, "Error waiting for FFmpeg process");
        }

        if (exitCode != 0)
        {
            return fail(errorMessage, "FFmpeg exited with an error");
        }
    }

    return true;
}
#endif

bool MuxVideoAndAudio(const std::string& ffmpegPath,
                      const std::string& videoPath,
                      const std::string& audioPath,
                      unsigned int audioFrequency,
                      unsigned long long audioBytes,
                      int capturedFrames,
                      double fps,
                      const std::string& outputPath,
                      std::string* errorMessage)
{
    const double audioDuration = static_cast<double>(audioBytes) /
                                 (static_cast<double>(audioFrequency) * 4.0);
    const double videoDuration = static_cast<double>(capturedFrames) / fps;
    const double timestampScale = (audioDuration > 0.0 && videoDuration > 0.0)
        ? audioDuration / videoDuration
        : 1.0;

#ifdef _WIN32
    char command[4096];
    snprintf(command,
             sizeof(command),
             "\"%s\" -v error -y -itsscale %g -i \"%s\" -f s16le -ar %u -ac 2 -i \"%s\" "
             "-c:v copy -c:a aac -strict -2 -b:a 192k -shortest \"%s\"",
             ffmpegPath.c_str(),
             timestampScale,
             videoPath.c_str(),
             audioFrequency,
             audioPath.c_str(),
             outputPath.c_str());

    return runFfmpegCommand(command, errorMessage);
#else
    char timeStampBuffer[64];
    char audioFreqBuffer[64];

    //std::format is not used for compatibility with older systems
    //Otherwise, std::locale and std::format would be simpler options
    auto currentLocale = setlocale(LC_ALL, NULL);

    //Use default C locale for formatting numbers
    setlocale(LC_NUMERIC, "C");

    //Print to buffers
    std::snprintf(timeStampBuffer, sizeof(timeStampBuffer), "%g", timestampScale);
    std::snprintf(audioFreqBuffer, sizeof(audioFreqBuffer), "%d", audioFrequency);

    //Set locale back
    setlocale(LC_ALL, currentLocale);

    std::string timestampStr(timeStampBuffer);
    std::string audioFreqStr(audioFreqBuffer);

    std::vector<std::string> arguments =
    {
        ffmpegPath, "-v", "error", "-y", "-itsscale", timestampStr, "-i", videoPath, "-f", "s16le",
        "-ar", audioFreqStr, "-ac", "2", "-i", audioPath, "-c:v", "copy", "-c:a", "aac", "-strict",
        "-2", "-b:a", "192k", "-shortest", outputPath
    };

    return runFfmpegCommandPosix(arguments, errorMessage);
#endif
}

bool FfmpegEncoder::openEncoder(const FfmpegEncoderConfig& config, std::string* errorMessage)
{
    m_FrameWidth = config.width;
    m_FrameHeight = config.height;
    m_ProcessHandle = nullptr;
    m_SelectedVideoEncoder.clear();
    m_HardwareAccelerated = false;

    m_SelectedVideoEncoder = config.videoEncoder.empty()
        ? chooseVideoEncoder(config.ffmpegPath, &m_HardwareAccelerated)
        : config.videoEncoder;
    if (m_SelectedVideoEncoder.empty())
    {
        return fail(errorMessage, "FFmpeg does not provide a usable H.264 encoder for MP4 export");
    }
    if (!config.videoEncoder.empty())
    {
        m_HardwareAccelerated = (config.videoEncoder != "libx264");
    }

#ifdef _WIN32
    const std::string encoderFlags = buildVideoEncoderFlags(m_SelectedVideoEncoder, config.crf);
    fprintf(stderr,
            "Using FFmpeg video encoder: %s%s\n",
            m_SelectedVideoEncoder.c_str(),
            m_HardwareAccelerated ? " (hardware)" : " (CPU)");

    char command[4096];
    snprintf(command,
             sizeof(command),
             "\"%s\" -v error -y -f rawvideo -pixel_format rgb24 -video_size %dx%d -framerate %g -i pipe:0 "
             "%s \"%s\"",
             config.ffmpegPath.c_str(),
             config.width,
             config.height,
             config.fps,
             encoderFlags.c_str(),
             config.outputPath.c_str());

    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    // Room for several frames, so writing one rarely waits for FFmpeg to read.
    constexpr DWORD stdinPipeSize = 16 << 20;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, stdinPipeSize))
    {
        return fail(errorMessage, "Failed to create FFmpeg stdin pipe");
    }

    SetHandleInformation(writeHandle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = readHandle;
    startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION processInfo = {};
    if (!CreateProcessA(nullptr,
                        command,
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startupInfo,
                        &processInfo))
    {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        return fail(errorMessage, "Failed to launch FFmpeg encoder");
    }

    CloseHandle(readHandle);
    CloseHandle(processInfo.hThread);
    m_ProcessHandle = processInfo.hProcess;

    const int fileDescriptor = _open_osfhandle(reinterpret_cast<intptr_t>(writeHandle), 0);
    if (fileDescriptor == -1)
    {
        CloseHandle(writeHandle);
        WaitForSingleObject(reinterpret_cast<HANDLE>(m_ProcessHandle), 1000);
        CloseHandle(reinterpret_cast<HANDLE>(m_ProcessHandle));
        m_ProcessHandle = nullptr;
        return fail(errorMessage, "Failed to open FFmpeg pipe");
    }

    m_Pipe = _fdopen(fileDescriptor, "wb");
    if (m_Pipe == nullptr)
    {
        _close(fileDescriptor);
        WaitForSingleObject(reinterpret_cast<HANDLE>(m_ProcessHandle), 1000);
        CloseHandle(reinterpret_cast<HANDLE>(m_ProcessHandle));
        m_ProcessHandle = nullptr;
        return fail(errorMessage, "Failed to wrap FFmpeg pipe");
    }

    setvbuf(m_Pipe, nullptr, _IOFBF, 1 << 20);
#else
    char videoSizeBuffer[16];
    char fpsBuffer[16];

    std::vector<std::string> encoderFlags = buildVideoEncoderFlagsPosix(m_SelectedVideoEncoder, config.crf);
    fprintf(stderr,
            "Using FFmpeg video encoder: %s%s\n",
            m_SelectedVideoEncoder.c_str(),
            m_HardwareAccelerated ? " (hardware)" : " (CPU)");

    auto currentLocale = setlocale(LC_ALL, NULL);

    //Use default C locale for formatting numbers
    setlocale(LC_NUMERIC, "C");

    //Print to buffers
    std::snprintf(videoSizeBuffer, sizeof(videoSizeBuffer), "%dx%d", config.width, config.height);
    std::snprintf(fpsBuffer, sizeof(fpsBuffer), "%g", config.fps);

    //Set locale back
    setlocale(LC_ALL, currentLocale);

    std::string videoSizeStr(videoSizeBuffer);
    std::string fpsStr(fpsBuffer);

    std::vector<std::string> command =
        {
          config.ffmpegPath, "-v", "error", "-y", "-f", "rawvideo", "-pixel_format", "rgb24",
          "-video_size", videoSizeStr, "-framerate", fpsStr, "-i", "pipe:0"
        };

    for (int i = 0; i < encoderFlags.size(); i++)
    {
        command.push_back(encoderFlags[i]);
    }
    command.push_back(config.outputPath);

    int rwPipe[2];

    if (pipe(rwPipe) == -1)
    {
        return fail(errorMessage, "Failed to create FFmpeg stdin pipe");
    }

    //As defined in /proc/sys/fs/pipe-max-size
    const int maxPipeSize = 16 << 16;

    //Set the write pipe's capacity to the max unprivileged size - the pipe will continue with the default value if it fails
    fcntl(rwPipe[WRITE_PIPE], F_SETPIPE_SZ, maxPipeSize);

    pid_t childPid = fork();

    if (childPid == -1)
    {
        close(rwPipe[READ_PIPE]);
        close(rwPipe[WRITE_PIPE]);
        return fail(errorMessage, "Failed to create fork for FFmpeg encoder");
    }

    //Child
    if (childPid == 0)
    {
        dup2(rwPipe[READ_PIPE], STDIN_FILENO);
        close(rwPipe[READ_PIPE]);
        close(rwPipe[WRITE_PIPE]);

        if (execWithArgs(command) == false)
        {
            return fail(errorMessage, "Failed to launch FFmpeg encoder");
        }
    }
    //Parent
    else
    {
        //Close unused read pipe
        close(rwPipe[READ_PIPE]);

        m_Pipe = fdopen(rwPipe[WRITE_PIPE], "w");

        if (m_Pipe == nullptr)
        {
            close(rwPipe[WRITE_PIPE]);
            return fail(errorMessage, "Failed to wrap FFmpeg pipe");
        }

        setvbuf(m_Pipe, nullptr, _IOFBF, 1 << 20);
    }
#endif

    return true;
}

bool FfmpegEncoder::writeFrame(const uint8_t* rgbData, int width, int height, std::string* errorMessage)
{
    if (m_Pipe == nullptr)
    {
        return fail(errorMessage, "FFmpeg encoder pipe is not open");
    }

    const size_t frameSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 3U;
    if (fwrite(rgbData, 1, frameSize, m_Pipe) != frameSize)
    {
        return fail(errorMessage, "Failed to write frame data to FFmpeg");
    }

    return true;
}

void FfmpegEncoder::closeEncoder()
{
    if (m_Pipe != nullptr)
    {
        fclose(m_Pipe);
        m_Pipe = nullptr;
    }

#ifdef _WIN32
    if (m_ProcessHandle != nullptr)
    {
        WaitForSingleObject(reinterpret_cast<HANDLE>(m_ProcessHandle), INFINITE);
        CloseHandle(reinterpret_cast<HANDLE>(m_ProcessHandle));
        m_ProcessHandle = nullptr;
    }
#endif
}

bool FfmpegEncoder::isHardwareAccelerated() const
{
    return m_HardwareAccelerated;
}

const std::string& FfmpegEncoder::selectedVideoEncoder() const
{
    return m_SelectedVideoEncoder;
}

} // namespace KailleraExport
