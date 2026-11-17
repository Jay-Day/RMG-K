#include "VidExtProxy.hpp"

#include <SDL3/SDL.h>

namespace KailleraExport
{

static SDL_Window* s_Window = nullptr;
static SDL_GLContext s_GlContext = nullptr;
static bool s_Initialized = false;

static int s_GlDoubleBuffer = 1;
static int s_GlDepthSize = 24;
static int s_GlRedSize = 8;
static int s_GlGreenSize = 8;
static int s_GlBlueSize = 8;
static int s_GlAlphaSize = 8;
static int s_GlSwapInterval = 0;
static int s_GlMultisampleBuffers = 0;
static int s_GlMultisampleSamples = 0;
static int s_GlMajorVersion = 3;
static int s_GlMinorVersion = 3;
static int s_GlProfile = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;

static m64p_error vidExtInit(void)
{
    if (s_Initialized)
    {
        return M64ERR_ALREADY_INIT;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return M64ERR_SYSTEM_FAIL;
    }

    s_Initialized = true;
    return M64ERR_SUCCESS;
}

static m64p_error vidExtInitWithRenderMode(m64p_render_mode mode)
{
    if (mode == M64P_RENDER_VULKAN)
    {
        return M64ERR_UNSUPPORTED;
    }

    return vidExtInit();
}

static m64p_error vidExtQuit(void)
{
    if (s_GlContext != nullptr)
    {
        SDL_GL_DestroyContext(s_GlContext);
        s_GlContext = nullptr;
    }

    if (s_Window != nullptr)
    {
        SDL_DestroyWindow(s_Window);
        s_Window = nullptr;
    }

    s_Initialized = false;
    return M64ERR_SUCCESS;
}

static m64p_error vidExtListModes(m64p_2d_size*, int*)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error vidExtListRates(m64p_2d_size, int*, int*)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error vidExtSetMode(int width, int height, int, int, int)
{
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, s_GlDoubleBuffer);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, s_GlDepthSize);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, s_GlRedSize);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, s_GlGreenSize);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, s_GlBlueSize);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, s_GlAlphaSize);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, s_GlMultisampleBuffers);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, s_GlMultisampleSamples);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, s_GlMajorVersion);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, s_GlMinorVersion);

    SDL_GLProfile profile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
    if (s_GlProfile == M64P_GL_CONTEXT_PROFILE_CORE)
    {
        profile = SDL_GL_CONTEXT_PROFILE_CORE;
    }
    else if (s_GlProfile == M64P_GL_CONTEXT_PROFILE_ES)
    {
        profile = SDL_GL_CONTEXT_PROFILE_ES;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

    if (s_GlContext != nullptr)
    {
        SDL_GL_DestroyContext(s_GlContext);
        s_GlContext = nullptr;
    }

    if (s_Window != nullptr)
    {
        SDL_DestroyWindow(s_Window);
        s_Window = nullptr;
    }

    s_Window = SDL_CreateWindow("RMG-K Replay Export",
                                width,
                                height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (s_Window == nullptr)
    {
        s_Window = SDL_CreateWindow("RMG-K Replay Export",
                                    width,
                                    height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_MINIMIZED);
        if (s_Window == nullptr)
        {
            return M64ERR_SYSTEM_FAIL;
        }
    }

    s_GlContext = SDL_GL_CreateContext(s_Window);
    if (s_GlContext == nullptr)
    {
        SDL_DestroyWindow(s_Window);
        s_Window = nullptr;
        return M64ERR_SYSTEM_FAIL;
    }

    SDL_GL_MakeCurrent(s_Window, s_GlContext);
    SDL_GL_SetSwapInterval(0);
    return M64ERR_SUCCESS;
}

static m64p_error vidExtSetModeWithRate(int, int, int, int, int, int)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_function vidExtGlGetProc(const char* procName)
{
    return reinterpret_cast<m64p_function>(SDL_GL_GetProcAddress(procName));
}

static m64p_error vidExtGlSetAttr(m64p_GLattr attr, int value)
{
    switch (attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        s_GlDoubleBuffer = value;
        break;
    case M64P_GL_DEPTH_SIZE:
        s_GlDepthSize = value;
        break;
    case M64P_GL_RED_SIZE:
        s_GlRedSize = value;
        break;
    case M64P_GL_GREEN_SIZE:
        s_GlGreenSize = value;
        break;
    case M64P_GL_BLUE_SIZE:
        s_GlBlueSize = value;
        break;
    case M64P_GL_ALPHA_SIZE:
        s_GlAlphaSize = value;
        break;
    case M64P_GL_SWAP_CONTROL:
        s_GlSwapInterval = 0;
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        s_GlMultisampleBuffers = 0;
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        s_GlMultisampleSamples = 0;
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        s_GlMajorVersion = value;
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        s_GlMinorVersion = value;
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        s_GlProfile = value;
        break;
    default:
        break;
    }

    return M64ERR_SUCCESS;
}

static m64p_error vidExtGlGetAttr(m64p_GLattr attr, int* value)
{
    if (value == nullptr)
    {
        return M64ERR_INPUT_ASSERT;
    }

    switch (attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        *value = s_GlDoubleBuffer;
        break;
    case M64P_GL_BUFFER_SIZE:
        *value = s_GlRedSize + s_GlGreenSize + s_GlBlueSize + s_GlAlphaSize;
        break;
    case M64P_GL_DEPTH_SIZE:
        *value = s_GlDepthSize;
        break;
    case M64P_GL_RED_SIZE:
        *value = s_GlRedSize;
        break;
    case M64P_GL_GREEN_SIZE:
        *value = s_GlGreenSize;
        break;
    case M64P_GL_BLUE_SIZE:
        *value = s_GlBlueSize;
        break;
    case M64P_GL_ALPHA_SIZE:
        *value = s_GlAlphaSize;
        break;
    case M64P_GL_SWAP_CONTROL:
        *value = s_GlSwapInterval;
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        *value = s_GlMultisampleBuffers;
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        *value = s_GlMultisampleSamples;
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        *value = s_GlMajorVersion;
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        *value = s_GlMinorVersion;
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        *value = s_GlProfile;
        break;
    default:
        break;
    }

    return M64ERR_SUCCESS;
}

static m64p_error vidExtGlSwapBuf(void)
{
    // Nothing is shown, and the frame capture's glReadPixels already waits
    // for rendering to finish.
    return M64ERR_SUCCESS;
}

static m64p_error vidExtSetCaption(const char*)
{
    return M64ERR_SUCCESS;
}

static m64p_error vidExtToggleFullscreen(void)
{
    return M64ERR_SUCCESS;
}

static m64p_error vidExtResizeWindow(int, int)
{
    return M64ERR_SUCCESS;
}

static uint32_t vidExtGetDefaultFramebuffer(void)
{
    return 0;
}

static m64p_error vidExtVkGetSurface(void**, void*)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error vidExtVkGetInstanceExtensions(const char**[], uint32_t*)
{
    return M64ERR_UNSUPPORTED;
}

m64p_video_extension_functions GetVidExtFunctions()
{
    m64p_video_extension_functions functions = {};
    functions.Functions = 17;
    functions.VidExtFuncInit = vidExtInit;
    functions.VidExtFuncQuit = vidExtQuit;
    functions.VidExtFuncListModes = vidExtListModes;
    functions.VidExtFuncListRates = vidExtListRates;
    functions.VidExtFuncSetMode = vidExtSetMode;
    functions.VidExtFuncSetModeWithRate = vidExtSetModeWithRate;
    functions.VidExtFuncGLGetProc = vidExtGlGetProc;
    functions.VidExtFuncGLSetAttr = vidExtGlSetAttr;
    functions.VidExtFuncGLGetAttr = vidExtGlGetAttr;
    functions.VidExtFuncGLSwapBuf = vidExtGlSwapBuf;
    functions.VidExtFuncSetCaption = vidExtSetCaption;
    functions.VidExtFuncToggleFS = vidExtToggleFullscreen;
    functions.VidExtFuncResizeWindow = vidExtResizeWindow;
    functions.VidExtFuncGLGetDefaultFramebuffer = vidExtGetDefaultFramebuffer;
    functions.VidExtFuncInitWithRenderMode = vidExtInitWithRenderMode;
    functions.VidExtFuncVKGetSurface = vidExtVkGetSurface;
    functions.VidExtFuncVKGetInstanceExtensions = vidExtVkGetInstanceExtensions;
    return functions;
}

void ShutdownVidExt()
{
    vidExtQuit();
    SDL_Quit();
}

} // namespace KailleraExport
