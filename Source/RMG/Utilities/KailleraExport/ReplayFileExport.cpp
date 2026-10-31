#include "ReplayFileExport.hpp"

#include "EmulatorProxy.hpp"
#include "KrecParser.hpp"
#include "PifReplay.hpp"
#include "ReplayHook.hpp"

#include <QCommandLineParser>
#include <QTemporaryDir>

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/File.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace KailleraExport
{

namespace
{

struct ReplayFileExportOptions
{
    std::filesystem::path krecPath;
    std::filesystem::path romPath;
    std::filesystem::path outputPath;
    bool verbose = false;
};

static constexpr const char* kExportKrecOption = "export-krec";
static constexpr const char* kExportRomOption = "export-rom";
static constexpr const char* kExportRmgrOutputOption = "export-rmgr-output";
static constexpr const char* kExportVerboseOption = "export-verbose";

// This feature's memory offsets (ReplayMemory.cpp, in RMG-Core) were only
// ever derived/verified against Smash Remix 2.0.1 - same restriction as the
// live-recording path (see Replay.cpp's DetermineGameFamily()). Checked
// here too, against the krec's own stored game name, so a mismatched
// recording fails fast with a clear reason instead of spinning up a whole
// headless emulator to produce nothing.
static constexpr const char* kSupportedGoodName = "SmashRemix2.0.1";

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

// Mirrors KrecMp4Export.cpp's prepareConfigDirectory() - kept as its own
// small copy here rather than shared, since the two export pipelines are
// otherwise independent and this is the only piece they'd share.
static void prepareConfigDirectory(const std::filesystem::path& configDirectory)
{
    const std::filesystem::path userConfigDirectory = CoreGetUserConfigDirectory();
    const std::filesystem::path sharedDataDirectory = CoreGetSharedDataDirectory();
    copyFileIfPresent(sharedDataDirectory / "mupen64plus.ini", configDirectory / "mupen64plus.ini");
}

static std::filesystem::path getPluginPath(const char* category, const char* fileName)
{
    std::filesystem::path pluginDirectory = CoreGetPluginDirectory();
    pluginDirectory /= category;
    pluginDirectory /= fileName;
    return pluginDirectory;
}

static bool requireExistingFile(const std::filesystem::path& path, const char* label, std::string* errorMessage)
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

static bool parseOptions(const QCommandLineParser& parser, ReplayFileExportOptions& outOptions, std::string* errorMessage)
{
    const QString krecValue = parser.value(kExportKrecOption);
    const QString romValue = parser.value(kExportRomOption);
    const QString outputValue = parser.value(kExportRmgrOutputOption);

    if (krecValue.isEmpty() || romValue.isEmpty() || outputValue.isEmpty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Missing replay file export arguments";
        }
        return false;
    }

    outOptions.krecPath = std::filesystem::path(krecValue.toStdString());
    outOptions.romPath = std::filesystem::path(romValue.toStdString());
    outOptions.outputPath = std::filesystem::path(outputValue.toStdString());
    outOptions.verbose = parser.isSet(kExportVerboseOption);
    return true;
}

// Frame callback for the headless export core (see EmulatorProxy::setFrameCallback()).
// Advances the krec replay, drives Replay::OnFrame() once per real frame
// (same cadence Emulation.cpp uses for a normal session), and reports
// progress in the exact text format KailleraPlaybackDialog's export-process
// output parser already understands (shared with MP4 export, see
// processExportOutputLine() in KailleraPlaybackDialog.cpp).
static EmulatorProxy* s_Emulator = nullptr;
static int s_ExpectedFrameCount = 0;
static int s_ProcessedFrames = 0;
static bool s_SpeedApplied = false;

static void applyExportSpeed()
{
    if (s_Emulator == nullptr || s_SpeedApplied)
    {
        return;
    }
    // No encoder backpressure to react to here (unlike MP4 export's
    // adaptive governor) - .rmgr recording only reads emulated memory once
    // per frame, so a single high fixed speed is enough. 2000 is the
    // core's own hard ceiling (see main_speedset() in mupen64plus-core's
    // main.c, raised from its original 1000 specifically for this) -
    // anything above that is silently rejected and leaves the speed
    // factor unchanged.
    int value = 2000;
    s_Emulator->coreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &value);
    s_SpeedApplied = true;
    fprintf(stderr, "Using replay export speed target: %d%%\n", value);
}

static void ReplayFileExportFrameCallback(unsigned int)
{
    if (s_Emulator == nullptr)
    {
        return;
    }

    ResetPifReplayFrameSync();
    if (IsPifReplayFinished())
    {
        s_Emulator->stop();
        return;
    }

    if (s_ExpectedFrameCount > 0 && s_ProcessedFrames >= (s_ExpectedFrameCount + 120))
    {
        fprintf(stderr,
                "Replay export reached safety frame limit (%d processed, %d expected), stopping.\n",
                s_ProcessedFrames,
                s_ExpectedFrameCount);
        s_Emulator->stop();
        return;
    }

    applyExportSpeed();

    ReplayOnFrame();
    s_ProcessedFrames++;

    if ((s_ProcessedFrames % 60) == 0)
    {
        if (s_ExpectedFrameCount > 0)
        {
            fprintf(stderr, "Captured %d / %d frames...\n", s_ProcessedFrames, s_ExpectedFrameCount);
        }
        else
        {
            fprintf(stderr, "Captured %d frames...\n", s_ProcessedFrames);
        }
    }
}

// Takes `options` by reference (not const) - outputPath is rewritten in
// place to the actual collision-free path used, so the caller (which
// prints "Replay export finished: <path>") reports where the file really
// ended up, not just what was originally requested.
static bool runReplayFileExport(ReplayFileExportOptions& options, std::string* errorMessage)
{
    const std::filesystem::path outputDirectory = options.outputPath.parent_path();
    if (!outputDirectory.empty())
    {
        std::error_code errorCode;
        std::filesystem::create_directories(outputDirectory, errorCode);
    }
    options.outputPath = CoreFindCollisionFreePath(options.outputPath);

    KrecData krecData;
    if (!ParseKrecFile(options.krecPath, krecData, errorMessage))
    {
        return false;
    }

    if (krecData.header.gameName != kSupportedGoodName)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "This recording's game (" + krecData.header.gameName +
                ") isn't Smash Remix 2.0.1 - replay file export only supports that game";
        }
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
    // Reuses the capture-only audio plugin from MP4 export - not because its
    // captured audio is used here (it isn't), but because it's already a
    // lightweight, headless-safe plugin that doesn't need a real audio
    // device, and mupen64plus-core needs some audio plugin attached to run.
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
    // No video is captured, so any small size does - keeps the hidden
    // window's offscreen framebuffer cheap.
    emulatorConfig.renderWidth = 320;
    emulatorConfig.renderHeight = 240;
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

    // Gives RMG-Core's own m64p::Core wrapper (which ReplayMemory.cpp reads
    // emulated memory through) working function pointers into the SAME
    // loaded core module this proxy is driving - EmulatorProxy talks to the
    // core directly and never goes through RMG-Core's own Emulation.cpp, so
    // without this, Replay::OnFrame()'s memory reads would silently no-op.
    // (Also returns false outright on a non-GAME_STATS build - see
    // ReplayHook.cpp - which is exactly the right failure here, since this
    // whole feature has nothing to do without it.)
    if (!IsReplayCoreApiHooked())
    {
        if (!HookReplayCoreApi(emulator.getCoreHandle()))
        {
            shutdownGuard();
            if (errorMessage != nullptr)
            {
                *errorMessage = "Failed to hook RMG-Core's API against the replay export emulator";
            }
            return false;
        }
    }

    InitializePifReplay(&krecData);
    emulator.setPifCallback(PifReplayCallback);

    s_Emulator = &emulator;
    s_ExpectedFrameCount = krecData.totalInputFrames;
    s_ProcessedFrames = 0;
    s_SpeedApplied = false;
    emulator.setFrameCallback(ReplayFileExportFrameCallback);

    ReplaySetEnabledOverride(true);
    ReplaySetOutputPathOverride(options.outputPath.string());
    // Live recording sources player names from n02's recording_player_names
    // global, which only the in-app netplay/lobby UI ever populates - this
    // headless process never touches that code, so without this the
    // GameStart event's playerNames would be silently written as all zero
    // bytes. The krec being replayed already has its own player names, so
    // pass those through instead.
    ReplaySetPlayerNamesOverride(krecData.header.playerNames);
    // The .krec's own recording-start timestamp (header field, or a
    // filename-derived fallback - see ParseKrecFile), plus GetPifReplayFrameIndex
    // to track how far into the original session each match falls - see
    // Replay::SetRecordedAtBaseOverride's doc comment for why this replaces
    // the default time(nullptr) (which would otherwise reflect this headless
    // export's own wall-clock time, not the original recording's).
    ReplaySetRecordedAtBaseOverride(krecData.header.timestamp, &GetPifReplayFrameIndex);
    ReplayOnEmulationStart();

    const m64p_error executeResult = emulator.execute();

    ReplayOnEmulationStop();

    s_Emulator = nullptr;
    shutdownGuard();

    if (executeResult != M64ERR_SUCCESS)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Replay export emulation failed";
        }
        return false;
    }

    return true;
}

} // namespace

bool IsReplayFileExportRequested(const QCommandLineParser& parser)
{
    return parser.isSet(kExportRmgrOutputOption);
}

int RunReplayFileExportFromCommandLine(const QCommandLineParser& parser)
{
#if !defined(_WIN32) || !defined(RMGK_GAME_STATS)
    (void)parser;
    printExportError("Replay file export requires Windows and a GAME_STATS build");
    return 1;
#else
    // RMG-Core's own CoreAddCallbackMessage() (used throughout Replay.cpp
    // for its info/warning messages - "Replay: recording to <path>",
    // "Replay: enabled, but the loaded ROM isn't Smash Remix 2.0.1 - not
    // recording", etc.) is otherwise a black hole in this process: the
    // normal GUI wires it up to the Log viewer in MainWindow::Init(), but
    // this CLI-only headless path never reaches that code (see the early
    // return in main.cpp, before MainWindow is even constructed) and never
    // calls CoreSetupCallbacks() itself - every message silently queues
    // into RMG-Core's internal pending-message buffer and is discarded
    // when this process exits, with no indication anything was ever said.
    // Wire up a callback that does nothing itself - the actual printing
    // happens inside CoreDebugCallback() via CoreSetPrintDebugCallback
    // below - just to satisfy RMG-Core's "has anyone registered yet" gate.
    // stdout is captured by the GUI's QProcess (MergedChannels) and shown
    // in the export progress dialog, same as this file's existing
    // fprintf(stderr, ...) progress messages.
    CoreSetupCallbacks([](CoreDebugMessageType, std::string, std::string) {},
                        [](CoreStateCallbackType, int) {});
    CoreSetPrintDebugCallback(true);

    ReplayFileExportOptions options;
    std::string errorMessage;
    if (!parseOptions(parser, options, &errorMessage))
    {
        printExportError(errorMessage);
        return 1;
    }

    if (!runReplayFileExport(options, &errorMessage))
    {
        printExportError(errorMessage);
        return 1;
    }

    fprintf(stderr, "Replay export finished: %s\n", options.outputPath.string().c_str());
    return 0;
#endif
}

} // namespace KailleraExport
