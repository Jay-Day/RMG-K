/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "VidExt.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/VidExt.hpp>
#include <RMG-Core/Video.hpp>

#include "OnScreenDisplay.hpp"

#include <QVulkanInstance>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QApplication>
#include <QByteArray>
#include <QThread>
#include <QScreen>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include <string>
#endif

//
// Local Variables
//

static Thread::EmulationThread* l_EmuThread             = nullptr;
static UserInterface::MainWindow* l_MainWindow          = nullptr;
static UserInterface::Widget::OGLWidget** l_OGLWidget   = nullptr;
static UserInterface::Widget::VKWidget** l_VulkanWidget = nullptr;
static QThread* l_RenderThread                          = nullptr;
static bool l_OpenGLInitialized                         = false;
static bool l_OsdInitialized                            = false;
static QSurfaceFormat l_SurfaceFormat;
static m64p_render_mode l_RenderMode;

#ifdef _WIN32
static constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
static constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
static constexpr int WGL_CONTEXT_PROFILE_MASK_ARB  = 0x9126;
static constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
static constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;

using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI *)(int);

struct NativeWglState
{
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC context = nullptr;
    int width = 0;
    int height = 0;
    bool displayModeChanged = false;
    std::wstring displayDevice;
};

static NativeWglState l_NativeWgl;
static bool l_NativeWglStopRequested = false;
static bool l_NativeWglRefocusRequested = false;

static bool VidExt_NativeWglShouldIgnoreAltEnter(void)
{
    return CoreHasInitNetplay() ||
        (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode());
}

static LRESULT CALLBACK VidExt_NativeWglWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        l_NativeWglStopRequested = true;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (GetForegroundWindow() != hwnd)
        {
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (wParam != FALSE)
        {
            l_NativeWglRefocusRequested = true;
        }
        else
        {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_F12)
        {
            l_NativeWglStopRequested = true;
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN)
        {
            if (VidExt_NativeWglShouldIgnoreAltEnter())
            {
                return 0;
            }
            l_NativeWglStopRequested = true;
            return 0;
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static std::wstring VidExt_Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

static bool VidExt_NativeWglRegisterClass(void)
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = VidExt_NativeWglWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RMGKNativeWglFullscreen";

    if (RegisterClassExW(&wc) == 0)
    {
        return false;
    }

    registered = true;
    return true;
}

static RECT VidExt_NativeWglTargetRect(LPCWSTR deviceName)
{
    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devmode))
    {
        return {
            devmode.dmPosition.x,
            devmode.dmPosition.y,
            devmode.dmPosition.x + static_cast<LONG>(devmode.dmPelsWidth),
            devmode.dmPosition.y + static_cast<LONG>(devmode.dmPelsHeight)
        };
    }

    return {
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)
    };
}

static bool VidExt_NativeWglApplyDisplayMode(LPCWSTR deviceName)
{
    std::string resolution = CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenResolution);
    int refreshRate = CoreSettingsGetIntValue(SettingsID::GUI_ExclusiveFullscreenRefreshRate);
    if (resolution.empty() && refreshRate == 0)
    {
        return true;
    }

    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);
    if (!EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devmode))
    {
        return false;
    }

    if (!resolution.empty())
    {
        size_t xPos = resolution.find('x');
        if (xPos != std::string::npos)
        {
            devmode.dmPelsWidth  = static_cast<DWORD>(std::stoul(resolution.substr(0, xPos)));
            devmode.dmPelsHeight = static_cast<DWORD>(std::stoul(resolution.substr(xPos + 1)));
            devmode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
        }
    }

    if (refreshRate > 0)
    {
        devmode.dmDisplayFrequency = static_cast<DWORD>(refreshRate);
        devmode.dmFields |= DM_DISPLAYFREQUENCY;
    }

    LONG result = (deviceName != nullptr) ?
        ChangeDisplaySettingsExW(deviceName, &devmode, nullptr, CDS_FULLSCREEN, nullptr) :
        ChangeDisplaySettingsW(&devmode, CDS_FULLSCREEN);
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        return false;
    }

    l_NativeWgl.displayModeChanged = true;
    if (deviceName != nullptr)
    {
        l_NativeWgl.displayDevice = deviceName;
    }
    return true;
}

static void VidExt_NativeWglRestoreDisplayMode(void)
{
    if (l_NativeWgl.displayModeChanged && !l_NativeWgl.displayDevice.empty())
    {
        ChangeDisplaySettingsExW(l_NativeWgl.displayDevice.c_str(), nullptr, nullptr, 0, nullptr);
    }
    else if (l_NativeWgl.displayModeChanged)
    {
        ChangeDisplaySettingsW(nullptr, 0);
    }

    l_NativeWgl.displayModeChanged = false;
    l_NativeWgl.displayDevice.clear();
}

static m64p_function VidExt_NativeWglGetProcAddress(const char* proc)
{
    PROC address = wglGetProcAddress(proc);
    if (address != nullptr)
    {
        return reinterpret_cast<m64p_function>(address);
    }

    HMODULE module = GetModuleHandleW(L"opengl32.dll");
    if (module == nullptr)
    {
        module = LoadLibraryW(L"opengl32.dll");
    }
    if (module == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<m64p_function>(GetProcAddress(module, proc));
}

static bool VidExt_NativeWglCreateContext(HDC hdc)
{
    HGLRC legacyContext = wglCreateContext(hdc);
    if (legacyContext == nullptr || !wglMakeCurrent(hdc, legacyContext))
    {
        if (legacyContext != nullptr)
        {
            wglDeleteContext(legacyContext);
        }
        return false;
    }

    auto createContextAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs != nullptr)
    {
        int profile = (l_SurfaceFormat.profile() == QSurfaceFormat::CoreProfile) ?
            WGL_CONTEXT_CORE_PROFILE_BIT_ARB :
            WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, l_SurfaceFormat.majorVersion(),
            WGL_CONTEXT_MINOR_VERSION_ARB, l_SurfaceFormat.minorVersion(),
            WGL_CONTEXT_PROFILE_MASK_ARB, profile,
            0
        };

        HGLRC context = createContextAttribs(hdc, nullptr, attribs);
        if (context != nullptr)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacyContext);
            if (!wglMakeCurrent(hdc, context))
            {
                wglDeleteContext(context);
                return false;
            }
            l_NativeWgl.context = context;
        }
    }

    if (l_NativeWgl.context == nullptr)
    {
        l_NativeWgl.context = legacyContext;
    }

    auto swapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
        wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval != nullptr)
    {
        swapInterval(0);
    }

    return true;
}

static void VidExt_NativeWglReassertFullscreen(void)
{
    if (l_NativeWgl.hwnd == nullptr || l_NativeWgl.hdc == nullptr || l_NativeWgl.context == nullptr)
    {
        return;
    }

    LPCWSTR deviceName = l_NativeWgl.displayDevice.empty() ? nullptr : l_NativeWgl.displayDevice.c_str();
    RECT rect = VidExt_NativeWglTargetRect(deviceName);
    SetWindowPos(l_NativeWgl.hwnd, HWND_TOPMOST,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    BringWindowToTop(l_NativeWgl.hwnd);
    SetForegroundWindow(l_NativeWgl.hwnd);
    SetActiveWindow(l_NativeWgl.hwnd);
    SetFocus(l_NativeWgl.hwnd);

    if (wglMakeCurrent(l_NativeWgl.hdc, l_NativeWgl.context))
    {
        glViewport(0, 0, l_NativeWgl.width, l_NativeWgl.height);
        auto swapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
            wglGetProcAddress("wglSwapIntervalEXT"));
        if (swapInterval != nullptr)
        {
            swapInterval(0);
        }
    }
}

static void VidExt_NativeWglDestroy(void)
{
    if (l_NativeWgl.context != nullptr)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(l_NativeWgl.context);
        l_NativeWgl.context = nullptr;
    }

    if (l_NativeWgl.hdc != nullptr && l_NativeWgl.hwnd != nullptr)
    {
        ReleaseDC(l_NativeWgl.hwnd, l_NativeWgl.hdc);
        l_NativeWgl.hdc = nullptr;
    }

    if (l_NativeWgl.hwnd != nullptr)
    {
        DestroyWindow(l_NativeWgl.hwnd);
        l_NativeWgl.hwnd = nullptr;
    }
    l_NativeWgl.width = 0;
    l_NativeWgl.height = 0;
    l_NativeWglStopRequested = false;
    l_NativeWglRefocusRequested = false;
    qunsetenv("RMG_NATIVE_WGL_DRAWABLE_WIDTH");
    qunsetenv("RMG_NATIVE_WGL_DRAWABLE_HEIGHT");

    VidExt_NativeWglRestoreDisplayMode();
}

static void VidExt_NativeWglPollEvents(void)
{
    MSG message = {};
    while (PeekMessageW(&message, l_NativeWgl.hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (l_NativeWglStopRequested)
    {
        l_NativeWglStopRequested = false;
        CoreStopEmulation();
    }

    if (l_NativeWglRefocusRequested)
    {
        l_NativeWglRefocusRequested = false;
        VidExt_NativeWglReassertFullscreen();
    }
}

static bool VidExt_NativeWglSetup(void)
{
    if (l_NativeWgl.hwnd != nullptr)
    {
        return true;
    }

    if (!VidExt_NativeWglRegisterClass())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to register native WGL fullscreen window class");
        return false;
    }

    std::wstring deviceNameString = VidExt_Utf8ToWide(CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenMonitor));
    LPCWSTR deviceName = deviceNameString.empty() ? nullptr : deviceNameString.c_str();
    l_NativeWgl.displayDevice = deviceNameString;
    if (!VidExt_NativeWglApplyDisplayMode(deviceName))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning, "Failed to apply exclusive fullscreen display mode; using current mode");
    }

    RECT rect = VidExt_NativeWglTargetRect(deviceName);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"RMGKNativeWglFullscreen",
        L"RMG-K",
        WS_POPUP | WS_VISIBLE,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd == nullptr)
    {
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create native WGL fullscreen window");
        return false;
    }

    HDC hdc = GetDC(hwnd);
    if (hdc == nullptr)
    {
        DestroyWindow(hwnd);
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to get native WGL fullscreen device context");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = static_cast<BYTE>(l_SurfaceFormat.depthBufferSize() > 0 ? l_SurfaceFormat.depthBufferSize() : 24);
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (pixelFormat == 0 || !SetPixelFormat(hdc, pixelFormat, &pfd))
    {
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to set native WGL fullscreen pixel format");
        return false;
    }

    l_NativeWgl.hwnd = hwnd;
    l_NativeWgl.hdc = hdc;
    RECT clientRect = {};
    if (GetClientRect(hwnd, &clientRect))
    {
        l_NativeWgl.width = static_cast<int>(clientRect.right - clientRect.left);
        l_NativeWgl.height = static_cast<int>(clientRect.bottom - clientRect.top);
        qputenv("RMG_NATIVE_WGL_DRAWABLE_WIDTH", QByteArray::number(l_NativeWgl.width));
        qputenv("RMG_NATIVE_WGL_DRAWABLE_HEIGHT", QByteArray::number(l_NativeWgl.height));
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Native WGL drawable size " + std::to_string(l_NativeWgl.width) + "x" + std::to_string(l_NativeWgl.height));
    }
    if (!VidExt_NativeWglCreateContext(hdc))
    {
        VidExt_NativeWglDestroy();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create native WGL fullscreen context");
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Using native WGL exclusive fullscreen presenter");
    return true;
}

static bool VidExt_NativeWglActive(void)
{
    return l_NativeWgl.hwnd != nullptr && l_NativeWgl.context != nullptr;
}
#endif

static QVulkanInstance l_VulkanInstance;
static QVulkanInfoVector<QVulkanExtension> l_VulkanExtensions;
static QVector<const char*> l_VulkanExtensionList;

static void VidExt_UpdateOsdDisplaySize(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL || !l_OsdInitialized || l_OGLWidget == nullptr)
    {
        return;
    }

    QOpenGLContext* context = (*l_OGLWidget)->GetContext();
    if (context == nullptr)
    {
        return;
    }

    QOpenGLFunctions* functions = context->functions();
    if (functions == nullptr)
    {
        return;
    }

    GLint viewport[4] = {};
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0)
    {
        OnScreenDisplaySetDisplaySize(viewport[2], viewport[3]);
    }
}

//
// VidExt Functions
//

static bool VidExt_OglSetup(int screenMode)
{
#ifdef _WIN32
    if (screenMode == M64VIDEO_FULLSCREEN &&
        CoreSettingsGetBoolValue(SettingsID::GUI_ExclusiveFullscreen) &&
        CoreSettingsGetBoolValue(SettingsID::GUI_BetaFullscreenBackend) &&
        VidExt_NativeWglSetup())
    {
        l_OpenGLInitialized = true;
        return true;
    }
#endif

    l_EmuThread->on_VidExt_SetupOGL(l_SurfaceFormat, QThread::currentThread());

    while (!(*l_OGLWidget)->isVisible())
    {
        continue;
    }

    if (!(*l_OGLWidget)->GetContext()->isValid())
    {
        if (QSurfaceFormat::defaultFormat().renderableType() == QSurfaceFormat::OpenGLES)
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve valid OpenGL ES context");
        }
        else
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve valid OpenGL context");
        }
        return false;
    }

    if (!(*l_OGLWidget)->GetContext()->makeCurrent((*l_OGLWidget)))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to make OpenGL context current");
        return false;
    }

    l_OpenGLInitialized = true;
    return true;
}

static m64p_error VidExt_InitWithRenderMode(m64p_render_mode RenderMode)
{
    l_RenderMode = RenderMode;
    l_RenderThread = QThread::currentThread();

    if (RenderMode == M64P_RENDER_OPENGL)
    {
        l_SurfaceFormat = QSurfaceFormat::defaultFormat();
        l_SurfaceFormat.setOption(QSurfaceFormat::DeprecatedFunctions, 1);
        l_SurfaceFormat.setDepthBufferSize(24);
        l_SurfaceFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
        l_SurfaceFormat.setSwapInterval(0);
        if (l_SurfaceFormat.renderableType() != QSurfaceFormat::OpenGLES)
        {
            l_SurfaceFormat.setMajorVersion(3);
            l_SurfaceFormat.setMinorVersion(3);
        }
        else
        {
            l_SurfaceFormat.setMajorVersion(2);
            l_SurfaceFormat.setMinorVersion(0);
        }
    }

    l_EmuThread->on_VidExt_Init(RenderMode == M64P_RENDER_OPENGL ? VidExtRenderMode::OpenGL : VidExtRenderMode::Vulkan);

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_Init(void)
{
    return VidExt_InitWithRenderMode(M64P_RENDER_OPENGL);
}

static m64p_error VidExt_Quit(void)
{
    OnScreenDisplayShutdown();

    if (l_RenderMode == M64P_RENDER_OPENGL)
    {
#ifdef _WIN32
        if (VidExt_NativeWglActive())
        {
            VidExt_NativeWglDestroy();
        }
        else
#endif
        {
        // move OpenGL context back to the GUI thread
        (*l_OGLWidget)->MoveContextToThread(QApplication::instance()->thread());
        }
    }
    else
    {
        // remove vulkan instance from widget
        // and destroy the instance
        (*l_VulkanWidget)->setVulkanInstance(nullptr);
        if (l_VulkanInstance.isValid())
        {
            l_VulkanInstance.destroy();
        }
    }
    l_EmuThread->on_VidExt_Quit();

    l_OpenGLInitialized = false;
    l_OsdInitialized    = false;

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ListModes(m64p_2d_size *SizeArray, int *NumSizes)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_ListRates(m64p_2d_size Size, int *NumRates, int *Rates)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_SetMode(int Width, int Height, int BitsPerPixel, int ScreenMode, int Flags)
{
    // initialize OpenGL when render mode
    // is OpenGL and not Vulkan
    if (l_RenderMode == M64P_RENDER_OPENGL)
    {
        if (!l_OpenGLInitialized && !VidExt_OglSetup(ScreenMode))
        {
            return M64ERR_SYSTEM_FAIL;
        }

        // try to initialize the OSD
        // when opengl 3 is used
#ifdef _WIN32
        bool nativeWglActive = VidExt_NativeWglActive();
#else
        bool nativeWglActive = false;
#endif
        if (!nativeWglActive &&
            l_SurfaceFormat.majorVersion() == 3 &&
            !l_OsdInitialized)
        {
            if (!OnScreenDisplayInit())
            {
                CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to initialize OSD");
                return M64ERR_SYSTEM_FAIL;
            }

            OnScreenDisplayLoadSettings();
            l_OsdInitialized = true;
        }
    }

    switch (ScreenMode)
    {
        default:
        case M64VIDEO_NONE:
            return M64ERR_INPUT_INVALID;
        case M64VIDEO_WINDOWED:
#ifdef _WIN32
            if (VidExt_NativeWglActive())
            {
                CoreAddCallbackMessage(CoreDebugMessageType::Info, "Leaving native WGL fullscreen by stopping emulation");
                CoreStopEmulation();
                return M64ERR_SUCCESS;
            }
#endif
            l_EmuThread->on_VidExt_SetWindowedMode(Width, Height, BitsPerPixel, Flags);
            break;
        case M64VIDEO_FULLSCREEN:
#ifdef _WIN32
            if (VidExt_NativeWglActive())
            {
                if (l_NativeWgl.width > 0 && l_NativeWgl.height > 0)
                {
                    Width = l_NativeWgl.width;
                    Height = l_NativeWgl.height;
                    glViewport(0, 0, l_NativeWgl.width, l_NativeWgl.height);
                    OnScreenDisplaySetDisplaySize(l_NativeWgl.width, l_NativeWgl.height);
                }
                break;
            }
#endif
            l_EmuThread->on_VidExt_SetFullscreenMode(Width, Height, BitsPerPixel, Flags);
            break;
    }

    OnScreenDisplaySetDisplaySize(Width, Height);
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_SetModeWithRate(int Width, int Height, int RefreshRate, int BitsPerPixel, int ScreenMode, int Flags)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_function VidExt_GLGetProc(const char *Proc)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return nullptr;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        return VidExt_NativeWglGetProcAddress(Proc);
    }
#endif

    return (*l_OGLWidget)->GetContext()->getProcAddress(Proc);
}

static m64p_error VidExt_GLSetAttr(m64p_GLattr Attr, int Value)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    switch (Attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        if (Value == 1)
            l_SurfaceFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        else if (Value == 0)
            l_SurfaceFormat.setSwapBehavior(QSurfaceFormat::SingleBuffer);
        break;
    case M64P_GL_BUFFER_SIZE:
        break;
    case M64P_GL_DEPTH_SIZE:
        l_SurfaceFormat.setDepthBufferSize(Value);
        break;
    case M64P_GL_RED_SIZE:
        l_SurfaceFormat.setRedBufferSize(Value);
        break;
    case M64P_GL_GREEN_SIZE:
        l_SurfaceFormat.setGreenBufferSize(Value);
        break;
    case M64P_GL_BLUE_SIZE:
        l_SurfaceFormat.setBlueBufferSize(Value);
        break;
    case M64P_GL_ALPHA_SIZE:
        l_SurfaceFormat.setAlphaBufferSize(Value);
        break;
    case M64P_GL_SWAP_CONTROL: // vsync should be disabled during netplay
        l_SurfaceFormat.setSwapInterval((!CoreHasInitNetplay() && Value) ? 1 : 0);
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        l_SurfaceFormat.setSamples(Value);
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        l_SurfaceFormat.setMajorVersion(Value);
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        l_SurfaceFormat.setMinorVersion(Value);
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        switch (Value)
        {
        case M64P_GL_CONTEXT_PROFILE_CORE:
            l_SurfaceFormat.setProfile(QSurfaceFormat::CoreProfile);
            break;
        case M64P_GL_CONTEXT_PROFILE_COMPATIBILITY:
            l_SurfaceFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
            break;
        case M64P_GL_CONTEXT_PROFILE_ES:
            l_SurfaceFormat.setRenderableType(QSurfaceFormat::OpenGLES);
            break;
        }

        break;
    }

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLGetAttr(m64p_GLattr Attr, int *pValue)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    QSurfaceFormat::SwapBehavior SB = l_SurfaceFormat.swapBehavior();
    switch (Attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        if (SB == QSurfaceFormat::SingleBuffer)
            *pValue = 0;
        else
            *pValue = 1;
        break;
    case M64P_GL_BUFFER_SIZE:
        *pValue =
            l_SurfaceFormat.alphaBufferSize() + l_SurfaceFormat.redBufferSize() + l_SurfaceFormat.greenBufferSize() + l_SurfaceFormat.blueBufferSize();
        break;
    case M64P_GL_DEPTH_SIZE:
        *pValue = l_SurfaceFormat.depthBufferSize();
        break;
    case M64P_GL_RED_SIZE:
        *pValue = l_SurfaceFormat.redBufferSize();
        break;
    case M64P_GL_GREEN_SIZE:
        *pValue = l_SurfaceFormat.greenBufferSize();
        break;
    case M64P_GL_BLUE_SIZE:
        *pValue = l_SurfaceFormat.blueBufferSize();
        break;
    case M64P_GL_ALPHA_SIZE:
        *pValue = l_SurfaceFormat.alphaBufferSize();
        break;
    case M64P_GL_SWAP_CONTROL:
        *pValue = l_SurfaceFormat.swapInterval();
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        *pValue = l_SurfaceFormat.samples();
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        *pValue = l_SurfaceFormat.majorVersion();
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        *pValue = l_SurfaceFormat.minorVersion();
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        switch (l_SurfaceFormat.profile())
        {
        case QSurfaceFormat::CoreProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_CORE;
            break;
        case QSurfaceFormat::CompatibilityProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;
            break;
        case QSurfaceFormat::NoProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;
            break;
        }
        break;
    }
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLSwapBuf(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    if (l_RenderThread != QThread::currentThread())
    {
        return M64ERR_UNSUPPORTED;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        VidExt_NativeWglPollEvents();
        SwapBuffers(l_NativeWgl.hdc);
        return M64ERR_SUCCESS;
    }
#endif

    VidExt_UpdateOsdDisplaySize();
    OnScreenDisplayRender();

    (*l_OGLWidget)->GetContext()->swapBuffers((*l_OGLWidget));
    (*l_OGLWidget)->GetContext()->makeCurrent((*l_OGLWidget));

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_SetCaption(const char *Title)
{
    l_EmuThread->on_VidExt_SetCaption(QString(Title));
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ToggleFS(void)
{
#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        if (VidExt_NativeWglShouldIgnoreAltEnter())
        {
            return M64ERR_SUCCESS;
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "Leaving native WGL fullscreen by stopping emulation");
        CoreStopEmulation();
        return M64ERR_SUCCESS;
    }
#endif

    CoreVideoMode videoMode;

    if (!CoreGetVideoMode(videoMode))
    {
        return M64ERR_SYSTEM_FAIL;
    }

    if (QThread::currentThread() != l_RenderThread)
    {
        l_MainWindow->on_VidExt_ToggleFS((videoMode == CoreVideoMode::Windowed));
    }
    else
    {
        l_EmuThread->on_VidExt_ToggleFS((videoMode == CoreVideoMode::Windowed));
    }

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ResizeWindow(int Width, int Height)
{
    l_EmuThread->on_VidExt_ResizeWindow(Width, Height);
    OnScreenDisplaySetDisplaySize(Width, Height);
    return M64ERR_SUCCESS;
}

static uint32_t VidExt_GLGetDefaultFramebuffer(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return 0;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        return 0;
    }
#endif

    return (*l_OGLWidget)->GetContext()->defaultFramebufferObject();
}

static m64p_error VidExt_VK_GetSurface(void** Surface, void* Instance)
{
    if (l_RenderMode != M64P_RENDER_VULKAN)
    {
        return M64ERR_INVALID_STATE;
    }

    // we don't support receiving a null handle
    // for the VkInstance
    if ((VkInstance)Instance == VK_NULL_HANDLE)
    {
        return M64ERR_UNSUPPORTED;
    }

    // use VkInstance from plugin
    // when we don't have a VkInstance yet
    if (l_VulkanInstance.vkInstance() == VK_NULL_HANDLE)
    {
        l_VulkanInstance.setVkInstance((VkInstance)Instance);
        if (!l_VulkanInstance.create())
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create vulkan instance");
            return M64ERR_SYSTEM_FAIL;
        }
        (*l_VulkanWidget)->setVulkanInstance(&l_VulkanInstance);
    }

    // attempt to retrieve vulkan surface for window
    VkSurfaceKHR vulkanSurface = QVulkanInstance::surfaceForWindow((*l_VulkanWidget));
    if (vulkanSurface == VK_NULL_HANDLE)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve vulkan surface for window");
        return M64ERR_SYSTEM_FAIL;
    }

    *Surface = (void*)vulkanSurface;
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_VK_GetInstanceExtensions(const char** Extensions[], uint32_t* NumExtensions)
{
    if (l_RenderMode != M64P_RENDER_VULKAN)
    {
        return M64ERR_INVALID_STATE;
    }

    l_VulkanExtensions = l_VulkanInstance.supportedExtensions();
    l_VulkanExtensionList.clear();

    // only add surface extensions
    for (int i = 0; i < l_VulkanExtensions.size(); i++)
    {
        if (l_VulkanExtensions[i].name.startsWith("VK_KHR_") &&
            l_VulkanExtensions[i].name.endsWith("surface"))
        {
            l_VulkanExtensionList.append(l_VulkanExtensions[i].name.data());
        }
    }

    *Extensions    = l_VulkanExtensionList.data();
    *NumExtensions = l_VulkanExtensionList.size();
    return M64ERR_SUCCESS;
}

//
// Exported Functions
//

bool SetupVidExt(Thread::EmulationThread* emuThread, UserInterface::MainWindow* mainWindow, 
    UserInterface::Widget::OGLWidget** oglWidget, UserInterface::Widget::VKWidget** vulkanWidget)
{
    l_EmuThread    = emuThread;
    l_MainWindow   = mainWindow;
    l_OGLWidget    = oglWidget;
    l_VulkanWidget = vulkanWidget;

    m64p_video_extension_functions vidext_funcs;

    vidext_funcs.Functions = 17;
    vidext_funcs.VidExtFuncInit = &VidExt_Init;
    vidext_funcs.VidExtFuncInitWithRenderMode = &VidExt_InitWithRenderMode;
    vidext_funcs.VidExtFuncQuit = &VidExt_Quit;
    vidext_funcs.VidExtFuncListModes = &VidExt_ListModes;
    vidext_funcs.VidExtFuncListRates = &VidExt_ListRates;
    vidext_funcs.VidExtFuncSetMode = &VidExt_SetMode;
    vidext_funcs.VidExtFuncSetModeWithRate = &VidExt_SetModeWithRate;
    vidext_funcs.VidExtFuncGLGetProc = &VidExt_GLGetProc;
    vidext_funcs.VidExtFuncGLSetAttr = &VidExt_GLSetAttr;
    vidext_funcs.VidExtFuncGLGetAttr = &VidExt_GLGetAttr;
    vidext_funcs.VidExtFuncGLSwapBuf = &VidExt_GLSwapBuf;
    vidext_funcs.VidExtFuncSetCaption = &VidExt_SetCaption;
    vidext_funcs.VidExtFuncToggleFS = &VidExt_ToggleFS;
    vidext_funcs.VidExtFuncResizeWindow = &VidExt_ResizeWindow;
    vidext_funcs.VidExtFuncGLGetDefaultFramebuffer = &VidExt_GLGetDefaultFramebuffer;
    vidext_funcs.VidExtFuncVKGetSurface = &VidExt_VK_GetSurface;
    vidext_funcs.VidExtFuncVKGetInstanceExtensions = &VidExt_VK_GetInstanceExtensions;

    return CoreSetupVidExt(vidext_funcs);
}
