#include "KrecMp4Export.hpp"

#include "EmulatorProxy.hpp"
#include "FfmpegEncoder.hpp"
#include "FrameCapture.hpp"
#include "KrecParser.hpp"
#include "PifReplay.hpp"
#include "OnScreenDisplay.hpp"

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <RMG-Core/Directories.hpp>
#include <RMG-Core/RankedJournal.hpp>
#include <RMG-Core/Settings.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace KailleraExport
{

namespace
{

struct ReplayExportOptions
{
    std::filesystem::path krecPath;
    std::filesystem::path romPath;
    std::filesystem::path outputPath;
    std::filesystem::path ffmpegPath;
    int renderWidth = 640;
    int renderHeight = 480;
    int crf = 23;
    double fps = 60.0;
    bool includeKailleraChat = true;
    bool labelPorts = false;
    bool verbose = false;
};

using AudioCaptureSetOutputFn = void (*)(const char*);
using AudioCaptureGetFrequencyFn = unsigned int (*)(void);
using AudioCaptureGetBytesWrittenFn = unsigned long long (*)(void);

static constexpr const char* kExportKrecOption = "export-krec";
static constexpr const char* kExportRomOption = "export-rom";
static constexpr const char* kExportOutputOption = "export-output";
static constexpr const char* kExportFfmpegOption = "export-ffmpeg";
static constexpr const char* kExportWidthOption = "export-width";
static constexpr const char* kExportHeightOption = "export-height";
static constexpr const char* kExportNoKailleraChatOption = "export-no-kaillera-chat";
static constexpr const char* kExportLabelPortsOption = "export-label-ports";
static constexpr const char* kExportVerboseOption = "export-verbose";

// Ranked recordings: the game module's memory writes are part of the game
// state, so the export applies the recording's write journal like playback
// does (see RMG-Core/RankedJournal.hpp).
static CoreRankedJournalPlayer s_RankedJournal;
static bool s_RankedJournalActive = false;
static uint8_t* s_RankedRdram = nullptr;
static void* (*s_DebugMemGetPointer)(int) = nullptr;
static CoreRankedInvalidateCodeFn s_InvalidateCode = nullptr;

static bool loadRankedJournal(const std::filesystem::path& krecPath)
{
    std::string json;
    if (!ReadKrecMetadata(krecPath, json))
    {
        return false;
    }
    const QJsonObject journal = QJsonDocument::fromJson(QByteArray::fromStdString(json))
        .object().value("journal").toObject();
    const QByteArray data = QByteArray::fromBase64(journal.value("data").toString().toLatin1());
    s_RankedJournal = CoreRankedJournalPlayer();
    return !journal.isEmpty() && CoreRankedDecodeJournal(data.toStdString(), s_RankedJournal.Frames);
}

static void exportFrameCallback(unsigned int frameIndex)
{
    if (s_RankedJournalActive)
    {
        if (s_RankedRdram == nullptr && s_DebugMemGetPointer != nullptr)
        {
            constexpr int kDebugPointerRdram = 1; // M64P_DBG_PTR_RDRAM
            s_RankedRdram = static_cast<uint8_t*>(s_DebugMemGetPointer(kDebugPointerRdram));
        }
        std::string mismatch;
        CoreRankedJournalAdvance(s_RankedJournal, GetPifReplayFrameIndex(), s_RankedRdram,
            s_InvalidateCode, &mismatch);
        if (!mismatch.empty())
        {
            fprintf(stderr, "Ranked replay %s; the export may not match the match\n", mismatch.c_str());
        }
    }
    FrameCaptureCallback(frameIndex);
}

static void printExportError(const std::string& message)
{
    fprintf(stderr, "Replay export failed: %s\n", message.c_str());
}

static bool copyFileIfPresent(const std::filesystem::path& source, const std::filesystem::path& target)
{
    std::error_code errorCode;
    if (!std::filesystem::exists(source, errorCode))
    {
        return false;
    }

    std::filesystem::create_directories(target.parent_path(), errorCode);
    std::filesystem::copy_file(
        source,
        target,
        std::filesystem::copy_options::overwrite_existing,
        errorCode);
    return !errorCode;
}

static void prepareConfigDirectory(const std::filesystem::path& configDirectory)
{
    const std::filesystem::path userConfigDirectory = CoreGetUserConfigDirectory();
    const std::filesystem::path sharedDataDirectory = CoreGetSharedDataDirectory();

    copyFileIfPresent(sharedDataDirectory / "mupen64plus.ini", configDirectory / "mupen64plus.ini");

    const bool copiedPrimary =
        copyFileIfPresent(userConfigDirectory / "GLideN64.ini", configDirectory / "GLideN64.ini") ||
        copyFileIfPresent(sharedDataDirectory / "GLideN64.ini", configDirectory / "GLideN64.ini");

    const bool copiedCustom =
        copyFileIfPresent(userConfigDirectory / "GLideN64.custom.ini", configDirectory / "GLideN64.custom.ini") ||
        copyFileIfPresent(sharedDataDirectory / "GLideN64.custom.ini", configDirectory / "GLideN64.custom.ini");

    if (!copiedPrimary && copiedCustom)
    {
        copyFileIfPresent(configDirectory / "GLideN64.custom.ini", configDirectory / "GLideN64.ini");
    }
    if (!copiedCustom && copiedPrimary)
    {
        copyFileIfPresent(configDirectory / "GLideN64.ini", configDirectory / "GLideN64.custom.ini");
    }
}

static std::filesystem::path resolveFfmpegPath(const QCommandLineParser& parser)
{
    if (parser.isSet(kExportFfmpegOption))
    {
        return std::filesystem::path(parser.value(kExportFfmpegOption).toStdString());
    }

    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString localFfmpeg = QDir(applicationDirectory).filePath("ffmpeg.exe");
    if (QFileInfo::exists(localFfmpeg))
    {
        return std::filesystem::path(localFfmpeg.toStdString());
    }

    const QString savedFfmpeg =
        QString::fromStdString(CoreSettingsGetStringValue(SettingsID::Kaillera_FfmpegPath));
    if (!savedFfmpeg.isEmpty() && QFileInfo::exists(savedFfmpeg))
    {
        return std::filesystem::path(savedFfmpeg.toStdString());
    }

    const QString foundExecutable = QStandardPaths::findExecutable("ffmpeg");
    if (!foundExecutable.isEmpty())
    {
        return std::filesystem::path(foundExecutable.toStdString());
    }

    const QString foundExecutableExe = QStandardPaths::findExecutable("ffmpeg.exe");
    if (!foundExecutableExe.isEmpty())
    {
        return std::filesystem::path(foundExecutableExe.toStdString());
    }

    return {};
}

static bool parseOptions(const QCommandLineParser& parser,
                         ReplayExportOptions& outOptions,
                         std::string* errorMessage)
{
    const QString krecValue = parser.value(kExportKrecOption);
    const QString romValue = parser.value(kExportRomOption);
    const QString outputValue = parser.value(kExportOutputOption);

    if (krecValue.isEmpty() || romValue.isEmpty() || outputValue.isEmpty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Missing replay export arguments";
        }
        return false;
    }

    outOptions.krecPath = std::filesystem::path(krecValue.toStdString());
    outOptions.romPath = std::filesystem::path(romValue.toStdString());
    outOptions.outputPath = std::filesystem::path(outputValue.toStdString());
    outOptions.ffmpegPath = resolveFfmpegPath(parser);
    outOptions.includeKailleraChat = !parser.isSet(kExportNoKailleraChatOption);
    outOptions.labelPorts = parser.isSet(kExportLabelPortsOption);
    outOptions.verbose = parser.isSet(kExportVerboseOption);

    bool widthOk = true;
    bool heightOk = true;
    if (parser.isSet(kExportWidthOption))
    {
        outOptions.renderWidth = parser.value(kExportWidthOption).toInt(&widthOk);
    }
    if (parser.isSet(kExportHeightOption))
    {
        outOptions.renderHeight = parser.value(kExportHeightOption).toInt(&heightOk);
    }
    if (!widthOk || !heightOk ||
        outOptions.renderWidth < 320 || outOptions.renderHeight < 240 ||
        (outOptions.renderWidth % 2) != 0 || (outOptions.renderHeight % 2) != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Invalid replay export resolution";
        }
        return false;
    }

    if (outOptions.ffmpegPath.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "FFmpeg was not found. Install FFmpeg or place ffmpeg.exe next to RMG-K.exe.";
        }
        return false;
    }

    return true;
}

static std::filesystem::path getPluginPath(const char* category, const char* fileName)
{
    std::filesystem::path pluginDirectory = CoreGetPluginDirectory();
    pluginDirectory /= category;
    pluginDirectory /= fileName;
    return pluginDirectory;
}

static bool requireExistingFile(const std::filesystem::path& path,
                                const char* label,
                                std::string* errorMessage)
{
    std::error_code errorCode;
    if (std::filesystem::exists(path, errorCode))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        *errorMessage = std::string("Missing ") + label + ": " + path.string();
    }
    return false;
}

static void removeFileIfPresent(const std::filesystem::path& path)
{
    std::error_code errorCode;
    std::filesystem::remove(path, errorCode);
}

static bool runReplayExport(const ReplayExportOptions& options, std::string* errorMessage)
{
    const std::filesystem::path outputDirectory = options.outputPath.parent_path();
    if (!outputDirectory.empty())
    {
        std::error_code errorCode;
        std::filesystem::create_directories(outputDirectory, errorCode);
    }

    std::string ffmpegError;
    if (!CheckFfmpegExecutable(options.ffmpegPath.string(), &ffmpegError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = ffmpegError;
        }
        return false;
    }

    KrecData krecData;
    if (!ParseKrecFile(options.krecPath, krecData, errorMessage))
    {
        return false;
    }

    QTemporaryDir temporaryConfigDirectory;
    if (!temporaryConfigDirectory.isValid())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create temporary export config directory";
        }
        return false;
    }

    prepareConfigDirectory(std::filesystem::path(temporaryConfigDirectory.path().toStdString()));

    const std::filesystem::path corePath = CoreGetCoreDirectory() / "mupen64plus.dll";
    const std::filesystem::path inputPluginPath = getPluginPath("Input", "RMG-Input.dll");
    const std::filesystem::path rspPluginPath = getPluginPath("RSP", "mupen64plus-rsp-hle.dll");
    const std::filesystem::path gfxPluginPath = getPluginPath("GFX", "mupen64plus-video-GLideN64.dll");
    const std::filesystem::path audioPluginPath = getPluginPath("Audio", "RMG-Audio-Capture.dll");
    const std::filesystem::path dataDirectory = CoreGetSharedDataDirectory();

    if (!requireExistingFile(options.krecPath, "recording", errorMessage) ||
        !requireExistingFile(options.romPath, "ROM", errorMessage) ||
        !requireExistingFile(corePath, "core DLL", errorMessage) ||
        !requireExistingFile(inputPluginPath, "input plugin", errorMessage) ||
        !requireExistingFile(rspPluginPath, "RSP plugin", errorMessage) ||
        !requireExistingFile(gfxPluginPath, "graphics plugin", errorMessage) ||
        !requireExistingFile(audioPluginPath, "audio capture plugin", errorMessage))
    {
        return false;
    }

    EmulatorConfig emulatorConfig;
    emulatorConfig.corePath = corePath.string();
    emulatorConfig.configDir = temporaryConfigDirectory.path().toStdString();
    emulatorConfig.dataDir = dataDirectory.string();
    emulatorConfig.gfxPluginPath = gfxPluginPath.string();
    emulatorConfig.rspPluginPath = rspPluginPath.string();
    emulatorConfig.inputPluginPath = inputPluginPath.string();
    emulatorConfig.audioPluginPath = audioPluginPath.string();
    emulatorConfig.renderWidth = options.renderWidth;
    emulatorConfig.renderHeight = options.renderHeight;
    emulatorConfig.verbose = options.verbose;

    EmulatorProxy emulator;
    if (!emulator.init(emulatorConfig))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to initialize replay export emulator";
        }
        return false;
    }

    auto shutdownGuard = [&emulator]() { emulator.shutdown(); };

    AudioCaptureSetOutputFn setOutputFn = nullptr;
    AudioCaptureGetFrequencyFn getFrequencyFn = nullptr;
    AudioCaptureGetBytesWrittenFn getBytesWrittenFn = nullptr;

    m64p_dynlib_handle audioHandle = emulator.getAudioPluginHandle();
    if (audioHandle != nullptr)
    {
#ifdef _WIN32
        setOutputFn = reinterpret_cast<AudioCaptureSetOutputFn>(GetProcAddress((HMODULE)audioHandle, "audio_capture_set_output"));
        getFrequencyFn = reinterpret_cast<AudioCaptureGetFrequencyFn>(GetProcAddress((HMODULE)audioHandle, "audio_capture_get_frequency"));
        getBytesWrittenFn = reinterpret_cast<AudioCaptureGetBytesWrittenFn>(GetProcAddress((HMODULE)audioHandle, "audio_capture_get_bytes_written"));
#endif
    }

    if (!emulator.openRom(options.romPath.string()))
    {
        shutdownGuard();
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to open ROM for replay export";
        }
        return false;
    }

    emulator.configureControllersForReplay(krecData.header.numPlayers);
    if (!emulator.attachPlugins())
    {
        shutdownGuard();
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to attach replay export plugins";
        }
        return false;
    }

    emulator.applyDeterministicSettings();

    const std::filesystem::path tempVideoPath = options.outputPath.string() + ".tmp_v.mp4";
    const std::filesystem::path tempAudioPath = options.outputPath.string() + ".tmp_a.raw";

    if (setOutputFn != nullptr)
    {
        setOutputFn(tempAudioPath.string().c_str());
    }

    FfmpegEncoder encoder;
    FfmpegEncoderConfig encoderConfig;
    encoderConfig.ffmpegPath = options.ffmpegPath.string();
    encoderConfig.outputPath = tempVideoPath.string();
    encoderConfig.videoEncoder.clear();
    encoderConfig.width = options.renderWidth;
    encoderConfig.height = options.renderHeight;
    encoderConfig.fps = options.fps;
    encoderConfig.crf = options.crf;

    InitializePifReplay(&krecData);
    emulator.setPifCallback(PifReplayCallback);

    OnScreenDisplaySetKailleraChatEnabled(options.includeKailleraChat);

    PortLabelConfig portLabelConfig;
    portLabelConfig.enabled = options.labelPorts;
    portLabelConfig.playerCount = krecData.header.numPlayers;
    portLabelConfig.playerNames = krecData.header.playerNames;

    InitializeFrameCapture(&emulator, &encoder, encoderConfig, krecData.totalInputFrames, portLabelConfig);
    s_RankedJournalActive = loadRankedJournal(options.krecPath);
    if (s_RankedJournalActive)
    {
        s_RankedRdram = nullptr;
        s_DebugMemGetPointer = reinterpret_cast<void* (*)(int)>(emulator.getCoreSymbol("DebugMemGetPointer"));
        s_InvalidateCode = reinterpret_cast<CoreRankedInvalidateCodeFn>(
            emulator.getCoreSymbol("RmgkInvalidateCachedCode"));
        fprintf(stderr, "Ranked recording: applying %zu write journal frames\n", s_RankedJournal.Frames.size());
    }
    emulator.setFrameCallback(exportFrameCallback);

    const m64p_error executeResult = emulator.execute();
    FlushFrameCapture();
    encoder.close();

    std::string frameCaptureError;
    if (GetFrameCaptureError(&frameCaptureError))
    {
        shutdownGuard();
        if (errorMessage != nullptr)
        {
            *errorMessage = frameCaptureError;
        }
        removeFileIfPresent(tempVideoPath);
        removeFileIfPresent(tempAudioPath);
        return false;
    }

    unsigned int audioFrequency = 33600;
    unsigned long long audioBytes = 0;
    if (getFrequencyFn != nullptr)
    {
        audioFrequency = getFrequencyFn();
    }
    if (getBytesWrittenFn != nullptr)
    {
        audioBytes = getBytesWrittenFn();
    }

    shutdownGuard();

    if (executeResult != M64ERR_SUCCESS)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Replay export emulation failed";
        }
        removeFileIfPresent(tempVideoPath);
        removeFileIfPresent(tempAudioPath);
        return false;
    }

    const int capturedFrames = GetCapturedFrameCount();
    if (capturedFrames <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Replay export did not capture any frames";
        }
        removeFileIfPresent(tempVideoPath);
        removeFileIfPresent(tempAudioPath);
        return false;
    }

    std::error_code errorCode;
    std::filesystem::remove(options.outputPath, errorCode);
    errorCode.clear();

    if (audioBytes > 0)
    {
        std::string muxError;
        if (!MuxVideoAndAudio(options.ffmpegPath.string(),
                              tempVideoPath.string(),
                              tempAudioPath.string(),
                              audioFrequency,
                              audioBytes,
                              capturedFrames,
                              options.fps,
                              options.outputPath.string(),
                              &muxError))
        {
            fprintf(stderr,
                    "Audio mux failed, falling back to video-only MP4: %s\n",
                    muxError.c_str());

            std::filesystem::remove(options.outputPath, errorCode);
            errorCode.clear();
            std::filesystem::rename(tempVideoPath, options.outputPath, errorCode);
            if (errorCode)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Audio mux failed and video-only fallback could not be saved";
                }
                removeFileIfPresent(tempVideoPath);
                removeFileIfPresent(tempAudioPath);
                return false;
            }
        }
    }
    else
    {
        std::filesystem::rename(tempVideoPath, options.outputPath, errorCode);
        if (errorCode)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Failed to move exported video into place";
            }
            removeFileIfPresent(tempVideoPath);
            removeFileIfPresent(tempAudioPath);
            return false;
        }
    }

    removeFileIfPresent(tempVideoPath);
    removeFileIfPresent(tempAudioPath);
    return true;
}

} // namespace

bool IsReplayExportRequested(const QCommandLineParser& parser)
{
    return parser.isSet(kExportKrecOption);
}

int RunReplayExportFromCommandLine(const QCommandLineParser& parser)
{
#ifndef _WIN32
    (void)parser;
    printExportError("Replay export is only supported on Windows");
    return 1;
#else
    ReplayExportOptions options;
    std::string errorMessage;
    if (!parseOptions(parser, options, &errorMessage))
    {
        printExportError(errorMessage);
        return 1;
    }

    if (!runReplayExport(options, &errorMessage))
    {
        printExportError(errorMessage);
        return 1;
    }

    fprintf(stderr, "Replay export finished: %s\n", options.outputPath.string().c_str());
    return 0;
#endif
}

} // namespace KailleraExport
