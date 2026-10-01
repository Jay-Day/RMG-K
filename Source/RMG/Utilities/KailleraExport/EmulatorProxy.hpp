#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C"
{

typedef void* m64p_handle;
typedef void (*m64p_function)(void);
typedef void (*m64p_frame_callback)(unsigned int frameIndex);

typedef enum
{
    M64TYPE_INT = 1,
    M64TYPE_FLOAT,
    M64TYPE_BOOL,
    M64TYPE_STRING
} m64p_type;

typedef enum
{
    M64MSG_ERROR = 1,
    M64MSG_WARNING,
    M64MSG_INFO,
    M64MSG_STATUS,
    M64MSG_VERBOSE
} m64p_msg_level;

typedef enum
{
    M64ERR_SUCCESS = 0,
    M64ERR_NOT_INIT,
    M64ERR_ALREADY_INIT,
    M64ERR_INCOMPATIBLE,
    M64ERR_INPUT_ASSERT,
    M64ERR_INPUT_INVALID,
    M64ERR_INPUT_NOT_FOUND,
    M64ERR_NO_MEMORY,
    M64ERR_FILES,
    M64ERR_INTERNAL,
    M64ERR_INVALID_STATE,
    M64ERR_PLUGIN_FAIL,
    M64ERR_SYSTEM_FAIL,
    M64ERR_UNSUPPORTED,
    M64ERR_WRONG_TYPE
} m64p_error;

typedef enum
{
    M64PLUGIN_NULL = 0,
    M64PLUGIN_RSP = 1,
    M64PLUGIN_GFX,
    M64PLUGIN_AUDIO,
    M64PLUGIN_INPUT,
    M64PLUGIN_CORE
} m64p_plugin_type;

typedef enum
{
    M64CORE_EMU_STATE = 1,
    M64CORE_VIDEO_MODE,
    M64CORE_SAVESTATE_SLOT,
    M64CORE_SPEED_FACTOR,
    M64CORE_SPEED_LIMITER,
    M64CORE_VIDEO_SIZE,
    M64CORE_AUDIO_VOLUME,
    M64CORE_AUDIO_MUTE,
    M64CORE_INPUT_GAMESHARK,
    M64CORE_STATE_LOADCOMPLETE,
    M64CORE_STATE_SAVECOMPLETE,
    M64CORE_SCREENSHOT_CAPTURED
} m64p_core_param;

typedef enum
{
    M64CMD_NOP = 0,
    M64CMD_ROM_OPEN,
    M64CMD_ROM_CLOSE,
    M64CMD_ROM_GET_HEADER,
    M64CMD_ROM_GET_SETTINGS,
    M64CMD_EXECUTE,
    M64CMD_STOP,
    M64CMD_PAUSE,
    M64CMD_RESUME,
    M64CMD_CORE_STATE_QUERY,
    M64CMD_STATE_LOAD,
    M64CMD_STATE_SAVE,
    M64CMD_STATE_SET_SLOT,
    M64CMD_SEND_SDL_KEYDOWN,
    M64CMD_SEND_SDL_KEYUP,
    M64CMD_SET_FRAME_CALLBACK,
    M64CMD_TAKE_NEXT_SCREENSHOT,
    M64CMD_CORE_STATE_SET,
    M64CMD_READ_SCREEN,
    M64CMD_RESET,
    M64CMD_ADVANCE_FRAME,
    M64CMD_SET_MEDIA_LOADER,
    M64CMD_NETPLAY_INIT,
    M64CMD_NETPLAY_CONTROL_PLAYER,
    M64CMD_NETPLAY_GET_VERSION,
    M64CMD_NETPLAY_CLOSE,
    M64CMD_PIF_OPEN,
    M64CMD_ROM_SET_SETTINGS,
    M64CMD_DISK_OPEN,
    M64CMD_DISK_CLOSE
} m64p_command;

typedef enum
{
    M64P_GL_DOUBLEBUFFER = 1,
    M64P_GL_BUFFER_SIZE,
    M64P_GL_DEPTH_SIZE,
    M64P_GL_RED_SIZE,
    M64P_GL_GREEN_SIZE,
    M64P_GL_BLUE_SIZE,
    M64P_GL_ALPHA_SIZE,
    M64P_GL_SWAP_CONTROL,
    M64P_GL_MULTISAMPLEBUFFERS,
    M64P_GL_MULTISAMPLESAMPLES,
    M64P_GL_CONTEXT_MAJOR_VERSION,
    M64P_GL_CONTEXT_MINOR_VERSION,
    M64P_GL_CONTEXT_PROFILE_MASK
} m64p_GLattr;

typedef enum
{
    M64P_GL_CONTEXT_PROFILE_CORE,
    M64P_GL_CONTEXT_PROFILE_COMPATIBILITY,
    M64P_GL_CONTEXT_PROFILE_ES
} m64p_GLContextType;

typedef enum
{
    M64P_RENDER_OPENGL = 0,
    M64P_RENDER_VULKAN
} m64p_render_mode;

typedef struct
{
    unsigned int uiWidth;
    unsigned int uiHeight;
} m64p_2d_size;

typedef struct
{
    unsigned int Functions;
    m64p_error (*VidExtFuncInit)(void);
    m64p_error (*VidExtFuncQuit)(void);
    m64p_error (*VidExtFuncListModes)(m64p_2d_size*, int*);
    m64p_error (*VidExtFuncListRates)(m64p_2d_size, int*, int*);
    m64p_error (*VidExtFuncSetMode)(int, int, int, int, int);
    m64p_error (*VidExtFuncSetModeWithRate)(int, int, int, int, int, int);
    m64p_function (*VidExtFuncGLGetProc)(const char*);
    m64p_error (*VidExtFuncGLSetAttr)(m64p_GLattr, int);
    m64p_error (*VidExtFuncGLGetAttr)(m64p_GLattr, int*);
    m64p_error (*VidExtFuncGLSwapBuf)(void);
    m64p_error (*VidExtFuncSetCaption)(const char*);
    m64p_error (*VidExtFuncToggleFS)(void);
    m64p_error (*VidExtFuncResizeWindow)(int, int);
    uint32_t (*VidExtFuncGLGetDefaultFramebuffer)(void);
    m64p_error (*VidExtFuncInitWithRenderMode)(m64p_render_mode);
    m64p_error (*VidExtFuncVKGetSurface)(void**, void*);
    m64p_error (*VidExtFuncVKGetInstanceExtensions)(const char**[], uint32_t*);
} m64p_video_extension_functions;

typedef void (*ptr_DebugCallback)(void* context, int level, const char* message);
typedef void (*ptr_StateCallback)(void* context, m64p_core_param paramType, int newValue);

struct pif_channel
{
    void* jbd;
    const void* ijbd;
    uint8_t* tx;
    uint8_t* tx_buf;
    uint8_t* rx;
    uint8_t* rx_buf;
};

struct pif
{
    uint8_t* base;
    uint8_t* ram;
    struct pif_channel channels[5];
};

typedef void (*pif_sync_callback_t)(struct pif*);

} // extern "C"

#ifdef _WIN32
#include <windows.h>
typedef HMODULE m64p_dynlib_handle;
#else
typedef void* m64p_dynlib_handle;
#endif

namespace KailleraExport
{

using EmulatorLogCallback = std::function<void(int level, const char* message)>;

struct EmulatorConfig
{
    std::string corePath;
    std::string configDir;
    std::string dataDir;
    std::string gfxPluginPath;
    std::string rspPluginPath;
    std::string inputPluginPath;
    std::string audioPluginPath;
    int renderWidth = 640;
    int renderHeight = 480;
    int msaa = 0;
    int aniso = 0;
    bool verbose = false;
};

void SetEmulatorLogCallback(EmulatorLogCallback callback);

class EmulatorProxy
{
public:
    bool init(const EmulatorConfig& config);
    bool openRom(const std::string& romPath);
    bool attachPlugins();
    void applyDeterministicSettings();
    void configureControllersForReplay(int numPlayers);
    void setPifCallback(pif_sync_callback_t callback);
    void setFrameCallback(m64p_frame_callback callback);
    m64p_error execute();
    void stop();
    void readScreen(void* destination, int* width, int* height);
    void shutdown();
    m64p_dynlib_handle getAudioPluginHandle() const;
    // Lets a caller elsewhere in this process (e.g. RMG-Core's m64p::Core
    // wrapper, via its own Hook()) resolve function pointers against the
    // SAME loaded core module this proxy is driving, without this class
    // needing to know anything about RMG-Core itself.
    m64p_dynlib_handle getCoreHandle() const;

    using CoreDoCommandFn = m64p_error (*)(m64p_command, int, void*);
    CoreDoCommandFn coreDoCommand = nullptr;

private:
    bool loadCore(const std::string& path);
    bool loadPlugin(const std::string& path, m64p_plugin_type type);
    void detachPlugins();
    void configureGlideN64();
    void disableGlideN64Osd();

    m64p_dynlib_handle m_CoreHandle = nullptr;
    m64p_dynlib_handle m_PluginHandles[4] = {};
    using PluginShutdownFn = m64p_error (*)(void);
    PluginShutdownFn m_PluginShutdowns[4] = {};

    using CoreStartupFn = m64p_error (*)(int, const char*, const char*, void*, ptr_DebugCallback, void*, ptr_StateCallback);
    using CoreShutdownFn = m64p_error (*)(void);
    using CoreAttachPluginFn = m64p_error (*)(m64p_plugin_type, m64p_dynlib_handle);
    using CoreDetachPluginFn = m64p_error (*)(m64p_plugin_type);
    using CoreOverrideVidExtFn = m64p_error (*)(m64p_video_extension_functions*);
    using ConfigOpenSectionFn = m64p_error (*)(const char*, m64p_handle*);
    using ConfigSetParameterFn = m64p_error (*)(m64p_handle, const char*, m64p_type, const void*);
    using SetPifSyncCallbackFn = void (*)(pif_sync_callback_t);
    using ReadScreen2Fn = void (*)(void* dest, int* width, int* height, int front);

    CoreStartupFn m_CoreStartup = nullptr;
    CoreShutdownFn m_CoreShutdown = nullptr;
    CoreAttachPluginFn m_CoreAttachPlugin = nullptr;
    CoreDetachPluginFn m_CoreDetachPlugin = nullptr;
    CoreOverrideVidExtFn m_CoreOverrideVidExt = nullptr;
    ConfigOpenSectionFn m_ConfigOpenSection = nullptr;
    ConfigSetParameterFn m_ConfigSetParameter = nullptr;
    SetPifSyncCallbackFn m_SetPifCallback = nullptr;
    ReadScreen2Fn m_ReadScreen2 = nullptr;

    bool m_RomOpen = false;
    bool m_PluginsAttached = false;
    std::string m_ConfigDir;
    int m_RenderWidth = 640;
    int m_RenderHeight = 480;
    int m_Msaa = 0;
    int m_Aniso = 0;
};

} // namespace KailleraExport
