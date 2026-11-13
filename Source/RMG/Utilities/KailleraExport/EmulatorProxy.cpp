#include "EmulatorProxy.hpp"
#include "VidExtProxy.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define LOAD_LIB(path) LoadLibraryA(path)
#define GET_PROC(handle, name) GetProcAddress((HMODULE)(handle), name)
#define FREE_LIB(handle) FreeLibrary((HMODULE)(handle))
#else
#include <dlfcn.h>
#define LOAD_LIB(path) dlopen(path, RTLD_NOW)
#define GET_PROC(handle, name) dlsym(handle, name)
#define FREE_LIB(handle) dlclose(handle)
#endif

namespace KailleraExport
{

static bool s_VerboseLogs = false;
static EmulatorLogCallback s_LogCallback;

void SetEmulatorLogCallback(EmulatorLogCallback callback)
{
    s_LogCallback = std::move(callback);
}

static void writeLog(int level, const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (s_LogCallback)
    {
        s_LogCallback(level, buffer);
        return;
    }

    if (s_VerboseLogs || level <= M64MSG_WARNING)
    {
        fprintf(stderr, "%s\n", buffer);
    }
}

static void debugCallback(void*, int level, const char* message)
{
    if (!s_VerboseLogs && level > M64MSG_WARNING)
    {
        return;
    }

    writeLog(level, "[m64p] %s", message);
}

static void stateCallback(void*, m64p_core_param, int)
{
}

bool EmulatorProxy::loadCore(const std::string& path)
{
    m_CoreHandle = LOAD_LIB(path.c_str());
    if (m_CoreHandle == nullptr)
    {
        writeLog(M64MSG_ERROR, "Failed to load core library: %s", path.c_str());
        return false;
    }

#define RESOLVE(symbol, type, name) \
    symbol = reinterpret_cast<type>(GET_PROC(m_CoreHandle, name)); \
    if (symbol == nullptr) \
    { \
        writeLog(M64MSG_ERROR, "Failed to resolve core symbol: %s", name); \
        return false; \
    }

    RESOLVE(m_CoreStartup, CoreStartupFn, "CoreStartup");
    RESOLVE(m_CoreShutdown, CoreShutdownFn, "CoreShutdown");
    RESOLVE(m_CoreAttachPlugin, CoreAttachPluginFn, "CoreAttachPlugin");
    RESOLVE(m_CoreDetachPlugin, CoreDetachPluginFn, "CoreDetachPlugin");
    RESOLVE(coreDoCommand, CoreDoCommandFn, "CoreDoCommand");
    RESOLVE(m_CoreOverrideVidExt, CoreOverrideVidExtFn, "CoreOverrideVidExt");
    RESOLVE(m_ConfigOpenSection, ConfigOpenSectionFn, "ConfigOpenSection");
    RESOLVE(m_ConfigSetParameter, ConfigSetParameterFn, "ConfigSetParameter");

#undef RESOLVE

    m_SetPifCallback = reinterpret_cast<SetPifSyncCallbackFn>(GET_PROC(m_CoreHandle, "set_pif_sync_callback"));
    m_ReadScreen2 = nullptr;
    return true;
}

bool EmulatorProxy::loadPlugin(const std::string& path, m64p_plugin_type type)
{
    const int index = static_cast<int>(type) - 1;
    if (index < 0 || index > 3)
    {
        return false;
    }

    m64p_dynlib_handle handle = LOAD_LIB(path.c_str());
    if (handle == nullptr)
    {
        writeLog(M64MSG_ERROR, "Failed to load plugin: %s", path.c_str());
        return false;
    }

    using PluginStartupFn = m64p_error (*)(m64p_dynlib_handle, void*, ptr_DebugCallback);
    auto startup = reinterpret_cast<PluginStartupFn>(GET_PROC(handle, "PluginStartup"));
    auto shutdown = reinterpret_cast<PluginShutdownFn>(GET_PROC(handle, "PluginShutdown"));
    if (startup == nullptr || shutdown == nullptr)
    {
        writeLog(M64MSG_ERROR, "Plugin is missing PluginStartup/PluginShutdown: %s", path.c_str());
        FREE_LIB(handle);
        return false;
    }

    const m64p_error result = startup(m_CoreHandle, nullptr, debugCallback);
    if (result != M64ERR_SUCCESS && result != M64ERR_ALREADY_INIT)
    {
        writeLog(M64MSG_ERROR, "PluginStartup failed for %s with error %d", path.c_str(), result);
        FREE_LIB(handle);
        return false;
    }

    if (type == M64PLUGIN_GFX)
    {
        m_ReadScreen2 = reinterpret_cast<ReadScreen2Fn>(GET_PROC(handle, "ReadScreen2"));
    }

    m_PluginHandles[index] = handle;
    m_PluginShutdowns[index] = shutdown;
    return true;
}

static bool readIniLines(const std::filesystem::path& path, std::vector<std::string>& lines)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return true;
}

static void writeIniLines(const std::filesystem::path& path, const std::vector<std::string>& lines)
{
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open())
    {
        return;
    }

    for (const std::string& line : lines)
    {
        output << line << '\n';
    }
}

using IniOverrides = std::vector<std::pair<std::string, std::string>>;

// Replaces the value of every "key=" line that names an overridden key,
// from firstLine up to (not including) lastLine. Returns which keys it found.
static std::vector<bool> replaceIniValues(std::vector<std::string>& lines,
                                          size_t firstLine,
                                          size_t lastLine,
                                          const IniOverrides& overrides)
{
    std::vector<bool> found(overrides.size(), false);
    for (size_t i = firstLine; i < lastLine && i < lines.size(); i++)
    {
        for (size_t o = 0; o < overrides.size(); o++)
        {
            const std::string prefix = overrides[o].first + "=";
            if (lines[i].rfind(prefix, 0) == 0)
            {
                lines[i] = prefix + overrides[o].second;
                found[o] = true;
                break;
            }
        }
    }
    return found;
}

void EmulatorProxy::configureGlideN64()
{
    // GLideN64 shows its FPS counter and renders at 640x480 unless its
    // settings say otherwise, so every one of these has to end up in the
    // file, including when the user has never saved graphics settings.
    const IniOverrides osdOverrides = {
        { "onScreenDisplay\\showFPS", "0" },
        { "onScreenDisplay\\showVIS", "0" },
        { "onScreenDisplay\\showPercent", "0" },
        { "onScreenDisplay\\showInternalResolution", "0" },
        { "onScreenDisplay\\showRenderingResolution", "0" },
        { "onScreenDisplay\\showStatistics", "0" },
    };
    IniOverrides profileOverrides = {
        { "frameBufferEmulation\\nativeResFactor", std::to_string(std::max(1, m_RenderWidth / 320)) },
        { "video\\windowedWidth", std::to_string(m_RenderWidth) },
        { "video\\windowedHeight", std::to_string(m_RenderHeight) },
        { "video\\multisampling", std::to_string(m_Msaa) },
        { "video\\maxMultiSampling", std::to_string(m_Msaa) },
        { "texture\\anisotropy", std::to_string(m_Aniso) },
        { "texture\\maxAnisotropy", std::to_string(m_Aniso) },
    };
    profileOverrides.insert(profileOverrides.end(), osdOverrides.begin(), osdOverrides.end());

    // The per-game file only needs its on-screen display lines turned off.
    {
        const std::filesystem::path customPath = std::filesystem::path(m_ConfigDir) / "GLideN64.custom.ini";
        std::vector<std::string> lines;
        if (readIniLines(customPath, lines))
        {
            replaceIniValues(lines, 0, lines.size(), osdOverrides);
            writeIniLines(customPath, lines);
        }
    }

    // The main file keeps its settings in the section named by "profile".
    const std::filesystem::path iniPath = std::filesystem::path(m_ConfigDir) / "GLideN64.ini";
    std::vector<std::string> lines;
    readIniLines(iniPath, lines);

    std::string profile = "User";
    for (const std::string& currentLine : lines)
    {
        if (currentLine.rfind("profile=", 0) == 0 && currentLine.size() > 8)
        {
            profile = currentLine.substr(8);
            break;
        }
    }

    const std::string profileHeader = "[" + profile + "]";
    size_t headerLine = lines.size();
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i] == profileHeader)
        {
            headerLine = i;
            break;
        }
    }

    if (headerLine == lines.size())
    {
        // No usable settings (no file, or one without a profile): start a
        // new one. 23 is the first settings version with profiles; the
        // plugin loads these values and rewrites the file as its own version.
        profile = "User";
        lines = {
            "[General]",
            "version=23",
            "profile=User",
            "",
            "[User]",
            "version=23",
        };
        headerLine = 4;
    }

    size_t sectionEnd = headerLine + 1;
    while (sectionEnd < lines.size() && (lines[sectionEnd].empty() || lines[sectionEnd].front() != '['))
    {
        sectionEnd++;
    }

    const std::vector<bool> found = replaceIniValues(lines, headerLine + 1, sectionEnd, profileOverrides);
    std::vector<std::string> missing;
    for (size_t o = 0; o < profileOverrides.size(); o++)
    {
        if (!found[o])
        {
            missing.push_back(profileOverrides[o].first + "=" + profileOverrides[o].second);
        }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(headerLine) + 1, missing.begin(), missing.end());

    writeIniLines(iniPath, lines);
}

void EmulatorProxy::disableGlideN64Osd()
{
    m64p_handle section = nullptr;
    if (m_ConfigOpenSection("Video-GLideN64", &section) != M64ERR_SUCCESS)
    {
        return;
    }

    int disabled = 0;
    m_ConfigSetParameter(section, "ShowFPS", M64TYPE_BOOL, &disabled);
    m_ConfigSetParameter(section, "ShowVIS", M64TYPE_BOOL, &disabled);
    m_ConfigSetParameter(section, "ShowPercent", M64TYPE_BOOL, &disabled);
    m_ConfigSetParameter(section, "ShowInternalResolution", M64TYPE_BOOL, &disabled);
    m_ConfigSetParameter(section, "ShowRenderingResolution", M64TYPE_BOOL, &disabled);
    m_ConfigSetParameter(section, "ShowStatistics", M64TYPE_BOOL, &disabled);
}

bool EmulatorProxy::init(const EmulatorConfig& config)
{
    s_VerboseLogs = config.verbose;
    m_ConfigDir = config.configDir;
    m_RenderWidth = config.renderWidth;
    m_RenderHeight = config.renderHeight;
    m_Msaa = config.msaa;
    m_Aniso = config.aniso;

    if (!loadCore(config.corePath))
    {
        return false;
    }

    if (m_SetPifCallback == nullptr)
    {
        writeLog(M64MSG_ERROR, "Core is missing set_pif_sync_callback support");
        return false;
    }

    const m64p_error startupResult = m_CoreStartup(
        0x020001,
        config.configDir.c_str(),
        config.dataDir.c_str(),
        nullptr,
        debugCallback,
        nullptr,
        stateCallback);
    if (startupResult != M64ERR_SUCCESS)
    {
        writeLog(M64MSG_ERROR, "CoreStartup failed with error %d", startupResult);
        return false;
    }

    m64p_video_extension_functions videoExtension = GetVidExtFunctions();
    const m64p_error vidExtResult = m_CoreOverrideVidExt(&videoExtension);
    if (vidExtResult != M64ERR_SUCCESS)
    {
        writeLog(M64MSG_ERROR, "CoreOverrideVidExt failed with error %d", vidExtResult);
        return false;
    }

    configureGlideN64();

    if (!loadPlugin(config.gfxPluginPath, M64PLUGIN_GFX) ||
        !loadPlugin(config.rspPluginPath, M64PLUGIN_RSP) ||
        !loadPlugin(config.audioPluginPath, M64PLUGIN_AUDIO) ||
        !loadPlugin(config.inputPluginPath, M64PLUGIN_INPUT))
    {
        return false;
    }

    disableGlideN64Osd();

    return true;
}

bool EmulatorProxy::openRom(const std::string& romPath)
{
    FILE* file = fopen(romPath.c_str(), "rb");
    if (file == nullptr)
    {
        writeLog(M64MSG_ERROR, "Cannot open ROM: %s", romPath.c_str());
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }

    const long length = ftell(file);
    rewind(file);

    std::vector<uint8_t> romData(static_cast<size_t>(length));
    if (fread(romData.data(), 1, romData.size(), file) != romData.size())
    {
        fclose(file);
        writeLog(M64MSG_ERROR, "Failed to read ROM");
        return false;
    }
    fclose(file);

    const m64p_error result = coreDoCommand(M64CMD_ROM_OPEN, static_cast<int>(length), romData.data());
    if (result != M64ERR_SUCCESS)
    {
        writeLog(M64MSG_ERROR, "M64CMD_ROM_OPEN failed with error %d", result);
        return false;
    }

    m_RomOpen = true;
    return true;
}

bool EmulatorProxy::attachPlugins()
{
    const m64p_plugin_type pluginTypes[] = {
        M64PLUGIN_GFX,
        M64PLUGIN_AUDIO,
        M64PLUGIN_INPUT,
        M64PLUGIN_RSP
    };

    for (m64p_plugin_type type : pluginTypes)
    {
        const int index = static_cast<int>(type) - 1;
        if (m_PluginHandles[index] == nullptr)
        {
            continue;
        }

        const m64p_error result = m_CoreAttachPlugin(type, m_PluginHandles[index]);
        if (result != M64ERR_SUCCESS)
        {
            writeLog(M64MSG_ERROR, "CoreAttachPlugin(%d) failed with error %d", type, result);
            return false;
        }
    }

    m_PluginsAttached = true;
    return true;
}

void EmulatorProxy::applyDeterministicSettings()
{
    m64p_handle section = nullptr;
    if (m_ConfigOpenSection("Core", &section) == M64ERR_SUCCESS)
    {
        int value = 0;
        m_ConfigSetParameter(section, "RandomizeInterrupt", M64TYPE_BOOL, &value);

        value = 2;
        m_ConfigSetParameter(section, "R4300Emulator", M64TYPE_INT, &value);

        value = 0;
        m_ConfigSetParameter(section, "CountPerOp", M64TYPE_INT, &value);
        m_ConfigSetParameter(section, "CountPerOpDenomPot", M64TYPE_INT, &value);

        value = -1;
        m_ConfigSetParameter(section, "SiDmaDuration", M64TYPE_INT, &value);

        value = 0;
        m_ConfigSetParameter(section, "DisableExtraMem", M64TYPE_BOOL, &value);

        value = 1;
        m_ConfigSetParameter(section, "DisableSaveFileLoading", M64TYPE_BOOL, &value);
    }
}

void EmulatorProxy::configureControllersForReplay(int numPlayers)
{
    for (int i = 0; i < 4; ++i)
    {
        char sectionName[128];
        snprintf(sectionName, sizeof(sectionName), "Rosalie's Mupen GUI - Input Plugin Profile %d", i);

        m64p_handle section = nullptr;
        if (m_ConfigOpenSection(sectionName, &section) != M64ERR_SUCCESS)
        {
            continue;
        }

        int pluggedIn = (i < numPlayers) ? 1 : 0;
        m_ConfigSetParameter(section, "PluggedIn", M64TYPE_BOOL, &pluggedIn);

        int pluginType = 1;
        m_ConfigSetParameter(section, "Plugin", M64TYPE_INT, &pluginType);
    }
}

void EmulatorProxy::setPifCallback(pif_sync_callback_t callback)
{
    if (m_SetPifCallback != nullptr)
    {
        m_SetPifCallback(callback);
    }
}

void EmulatorProxy::setFrameCallback(m64p_frame_callback callback)
{
    coreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, reinterpret_cast<void*>(callback));
}

bool EmulatorProxy::setSaveData(const std::vector<std::vector<uint8_t>>& kinds)
{
    if (coreDoCommand == nullptr)
    {
        return false;
    }

    std::vector<std::vector<uint8_t>> copy = kinds;
    m64p_save_data save = {};
    for (int kind = 0; kind < M64SAVEDATA_KINDS && kind < static_cast<int>(copy.size()); kind++)
    {
        save.data[kind] = copy[kind].empty() ? nullptr : copy[kind].data();
        save.size[kind] = static_cast<int>(copy[kind].size());
    }
    return coreDoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, &save) == M64ERR_SUCCESS;
}

m64p_error EmulatorProxy::execute()
{
    return coreDoCommand(M64CMD_EXECUTE, 0, nullptr);
}

void EmulatorProxy::stop()
{
    if (coreDoCommand != nullptr)
    {
        coreDoCommand(M64CMD_STOP, 0, nullptr);
    }
}

void EmulatorProxy::readScreen(void* destination, int* width, int* height)
{
    if (m_ReadScreen2 != nullptr)
    {
        m_ReadScreen2(destination, width, height, 0);
    }
}

void EmulatorProxy::detachPlugins()
{
    if (!m_PluginsAttached)
    {
        return;
    }

    const m64p_plugin_type pluginTypes[] = {
        M64PLUGIN_GFX,
        M64PLUGIN_AUDIO,
        M64PLUGIN_INPUT,
        M64PLUGIN_RSP
    };

    for (m64p_plugin_type type : pluginTypes)
    {
        m_CoreDetachPlugin(type);
    }

    m_PluginsAttached = false;
}

void EmulatorProxy::shutdown()
{
    detachPlugins();

    if (m_RomOpen && coreDoCommand != nullptr)
    {
        coreDoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
        m_RomOpen = false;
    }

    for (int i = 0; i < 4; ++i)
    {
        if (m_PluginShutdowns[i] != nullptr)
        {
            m_PluginShutdowns[i]();
            m_PluginShutdowns[i] = nullptr;
        }

        if (m_PluginHandles[i] != nullptr)
        {
            FREE_LIB(m_PluginHandles[i]);
            m_PluginHandles[i] = nullptr;
        }
    }

    if (m_CoreShutdown != nullptr)
    {
        m_CoreShutdown();
    }

    if (m_CoreHandle != nullptr)
    {
        FREE_LIB(m_CoreHandle);
        m_CoreHandle = nullptr;
    }

    ShutdownVidExt();
}

m64p_dynlib_handle EmulatorProxy::getAudioPluginHandle() const
{
    return m_PluginHandles[2];
}

} // namespace KailleraExport
