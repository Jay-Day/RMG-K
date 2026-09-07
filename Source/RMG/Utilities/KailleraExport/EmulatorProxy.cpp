#include "EmulatorProxy.hpp"
#include "VidExtProxy.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void EmulatorProxy::configureGlideN64()
{
    std::vector<std::filesystem::path> iniFiles;
    iniFiles.push_back(std::filesystem::path(m_ConfigDir) / "GLideN64.ini");
    iniFiles.push_back(std::filesystem::path(m_ConfigDir) / "GLideN64.custom.ini");

    for (const auto& iniPath : iniFiles)
    {
        if (!std::filesystem::exists(iniPath))
        {
            continue;
        }

        std::ifstream input(iniPath);
        if (!input.is_open())
        {
            continue;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.push_back(line);
        }
        input.close();

        const int nativeResFactor = std::max(1, m_RenderWidth / 320);
        bool inUserSection = false;
        for (std::string& currentLine : lines)
        {
            if (!currentLine.empty() && currentLine.front() == '[')
            {
                inUserSection = (currentLine == "[User]");
                continue;
            }

            if (currentLine.rfind("onScreenDisplay\\showFPS=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showFPS=0";
            }
            else if (currentLine.rfind("onScreenDisplay\\showVIS=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showVIS=0";
            }
            else if (currentLine.rfind("onScreenDisplay\\showPercent=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showPercent=0";
            }
            else if (currentLine.rfind("onScreenDisplay\\showInternalResolution=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showInternalResolution=0";
            }
            else if (currentLine.rfind("onScreenDisplay\\showRenderingResolution=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showRenderingResolution=0";
            }
            else if (currentLine.rfind("onScreenDisplay\\showStatistics=", 0) == 0)
            {
                currentLine = "onScreenDisplay\\showStatistics=0";
            }

            if (!inUserSection)
            {
                continue;
            }

            if (currentLine.rfind("frameBufferEmulation\\nativeResFactor=", 0) == 0)
            {
                currentLine = "frameBufferEmulation\\nativeResFactor=" + std::to_string(nativeResFactor);
            }
            else if (currentLine.rfind("video\\windowedWidth=", 0) == 0)
            {
                currentLine = "video\\windowedWidth=" + std::to_string(m_RenderWidth);
            }
            else if (currentLine.rfind("video\\windowedHeight=", 0) == 0)
            {
                currentLine = "video\\windowedHeight=" + std::to_string(m_RenderHeight);
            }
            else if (currentLine.rfind("video\\multisampling=", 0) == 0)
            {
                currentLine = "video\\multisampling=" + std::to_string(m_Msaa);
            }
            else if (currentLine.rfind("video\\maxMultiSampling=", 0) == 0)
            {
                currentLine = "video\\maxMultiSampling=" + std::to_string(m_Msaa);
            }
            else if (currentLine.rfind("texture\\anisotropy=", 0) == 0)
            {
                currentLine = "texture\\anisotropy=" + std::to_string(m_Aniso);
            }
            else if (currentLine.rfind("texture\\maxAnisotropy=", 0) == 0)
            {
                currentLine = "texture\\maxAnisotropy=" + std::to_string(m_Aniso);
            }
        }

        std::ofstream output(iniPath, std::ios::trunc);
        if (!output.is_open())
        {
            continue;
        }

        for (const std::string& currentLine : lines)
        {
            output << currentLine << '\n';
        }
    }
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

m64p_dynlib_handle EmulatorProxy::getCoreHandle() const
{
    return m_CoreHandle;
}

} // namespace KailleraExport
