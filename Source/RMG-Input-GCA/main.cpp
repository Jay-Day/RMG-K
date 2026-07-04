/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define INPUT_PLUGIN_API_VERSION 0x020100

#define M64P_PLUGIN_PROTOTYPES 1
#include <RMG-Core/m64p/api/m64p_common.h>
#include <RMG-Core/m64p/api/m64p_plugin.h>
#include <RMG-Core/m64p/api/m64p_custom.h>
#include <RMG-Core/m64p/api/m64p_types.h>

#include <RMG-Core/Settings.hpp>
#include <libusb.h>

#include "Adapter.hpp"
#include "GCInput.hpp"
#include "UserInterface/MainDialog.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <array>
#include <mutex>
#include <sstream>

//
// Local Defines
//

#define NUM_CONTROLLERS    4
#define GCA_VENDOR_ID  0x057e
#define GCA_PRODUCT_ID 0x0337

#define GCA_ENDPOINT_IN  0x81
#define GCA_ENDPOINT_OUT 0x02

#define GCA_COMMAND_POLL 0x13

//
// Local Structures
//

struct SettingsProfile
{
    double DeadzoneValue = 0.09;
    double SensitivityValue = 1.0;

    double TriggerTreshold = 0.5;
    double CButtonTreshold = 0.4;

    bool PortEnabled[NUM_CONTROLLERS] = {true, true, true, true};

    GCButtonMapping Mapping;
};

//
// Local variables
//

// libusb variables
static bool l_UsbInitialized = false;

// GCA variables
static libusb_device_handle* l_DeviceHandle = nullptr;

// Interrupt endpoint addresses, discovered from the USB descriptor in gca_init().
// Default to the genuine WUP-028 adapter's values; clones / alt-firmware (e.g. the
// HHL GC Pocket / RP2040) present 057e:0337 but enumerate their endpoints at
// different addresses, so we read them from the descriptor rather than assuming.
static uint8_t l_EndpointIn  = GCA_ENDPOINT_IN;
static uint8_t l_EndpointOut = GCA_ENDPOINT_OUT;
static std::atomic<bool> l_PollThreadRunning;
static std::atomic<bool> l_PolledState;
static std::mutex l_ControllerStateMutex;
static std::array<GameCubeAdapterControllerState, 4> l_ControllerState;
static std::thread l_PollThread;
static SettingsProfile l_Settings = {0};

// Maps Control index (0-3) to physical adapter port index (0-3).
// -1 means no controller mapped to this Control slot.
static std::array<int, NUM_CONTROLLERS> l_ControlToPort = {-1, -1, -1, -1};

// Config polling flag (true when config dialog started the poll thread)
static bool l_ConfigPollingStarted = false;

// mupen64plus debug callback
static void (*l_DebugCallback)(void *, int, const char *) = nullptr;
static void *l_DebugCallContext                           = nullptr;

//
// Custom Internal Plugin Functions
//

void PluginDebugMessage(int level, std::string message)
{
    if (l_DebugCallback == nullptr)
    {
        return;
    }

    l_DebugCallback(l_DebugCallContext, level, message.c_str());
}

//
// Internal Functions
//

static bool usb_init(void)
{
    std::string debugMessage;

    int ret = libusb_init(nullptr);
    if (ret != LIBUSB_SUCCESS)
    {
        debugMessage = "usb_init(): failed to initialize libusb: ";
        debugMessage += libusb_error_name(ret);
        PluginDebugMessage(M64MSG_ERROR, debugMessage);
        return false;
    }

    l_UsbInitialized = true;
    return true;
}

static void usb_quit(void)
{
    if (l_UsbInitialized)
    {
        libusb_exit(nullptr);
        l_UsbInitialized = false;
    }
}

static void gca_reset_state(void)
{
    l_ControllerStateMutex.lock();
    l_ControllerState = {0};
    l_ControllerStateMutex.unlock();
}

// Read the active config descriptor and pick the interrupt IN/OUT endpoint
// addresses for the claimed interface (interface 0). Logs the full endpoint layout
// so an unexpected adapter (clone / alt-firmware) is visible in the plugin log.
// Leaves the genuine-adapter defaults in place if nothing usable is found.
static void gca_discover_endpoints(void)
{
    l_EndpointIn  = GCA_ENDPOINT_IN;
    l_EndpointOut = GCA_ENDPOINT_OUT;

    libusb_device* dev = libusb_get_device(l_DeviceHandle);
    libusb_config_descriptor* config = nullptr;
    if (dev == nullptr ||
        libusb_get_active_config_descriptor(dev, &config) != LIBUSB_SUCCESS ||
        config == nullptr)
    {
        PluginDebugMessage(M64MSG_WARNING,
            "gca_discover_endpoints(): could not read config descriptor; using default endpoints");
        return;
    }

    bool foundIn  = false;
    bool foundOut = false;
    for (int ifaceIdx = 0; ifaceIdx < config->bNumInterfaces; ifaceIdx++)
    {
        const libusb_interface& iface = config->interface[ifaceIdx];
        for (int alt = 0; alt < iface.num_altsetting; alt++)
        {
            const libusb_interface_descriptor& asd = iface.altsetting[alt];
            for (int ep = 0; ep < asd.bNumEndpoints; ep++)
            {
                const libusb_endpoint_descriptor& epd = asd.endpoint[ep];
                const bool isInterrupt =
                    (epd.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT;
                const bool isIn =
                    (epd.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

                std::ostringstream descMsg;
                descMsg << "gca_discover_endpoints(): iface=" << (int)asd.bInterfaceNumber
                        << " class=0x" << std::hex << (int)asd.bInterfaceClass
                        << " ep_addr=0x" << (int)epd.bEndpointAddress
                        << " attr=0x" << (int)epd.bmAttributes << std::dec
                        << (isInterrupt ? " interrupt" : "")
                        << (isIn ? " IN" : " OUT");
                PluginDebugMessage(M64MSG_INFO, descMsg.str());

                // Only adopt endpoints from the interface we claim (interface 0).
                if (asd.bInterfaceNumber != 0 || !isInterrupt)
                {
                    continue;
                }
                if (isIn)
                {
                    l_EndpointIn = epd.bEndpointAddress;
                    foundIn = true;
                }
                else
                {
                    l_EndpointOut = epd.bEndpointAddress;
                    foundOut = true;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);

    std::ostringstream epMsg;
    epMsg << "gca_discover_endpoints(): using in=0x" << std::hex << (int)l_EndpointIn
          << " out=0x" << (int)l_EndpointOut << std::dec
          << ((foundIn && foundOut) ? " (discovered)" : " (defaults)");
    PluginDebugMessage(M64MSG_INFO, epMsg.str());
}

static bool gca_init(void)
{
    std::string debugMessage;
    int ret;

    // reset state
    gca_reset_state();
    l_PolledState.store(false);
    l_PollThreadRunning.store(true);

    // attempt open device
    l_DeviceHandle = libusb_open_device_with_vid_pid(nullptr, GCA_VENDOR_ID, GCA_PRODUCT_ID);
    if (l_DeviceHandle == nullptr)
    {
        debugMessage = "gca_init(): failed to open adapter!";
        PluginDebugMessage(M64MSG_ERROR, debugMessage);
        return false;
    }

    // according to dolphin, this makes
    // the Nyko-brand adapters work
    libusb_control_transfer(l_DeviceHandle, 0x21, 11, 0x0001, 0, nullptr, 0, 1000);

    // only detach kernel driver when required
    if (libusb_kernel_driver_active(l_DeviceHandle, 0) == 1)
    {
        ret = libusb_detach_kernel_driver(l_DeviceHandle, 0);
        if (ret != LIBUSB_SUCCESS)
        {
            libusb_close(l_DeviceHandle);
            debugMessage = "gca_init(): failed to detach kernel driver: ";
            debugMessage += libusb_error_name(ret);
            PluginDebugMessage(M64MSG_ERROR, debugMessage);
            return false;
        }
    }

    ret = libusb_claim_interface(l_DeviceHandle, 0);
    if (ret != LIBUSB_SUCCESS)
    {
        libusb_close(l_DeviceHandle);
        debugMessage = "gca_init(): failed to claim interface: ";
        debugMessage += libusb_error_name(ret);
        PluginDebugMessage(M64MSG_ERROR, debugMessage);
        return false;
    }

    // Discover the real interrupt endpoint addresses before talking to the adapter.
    // The genuine WUP-028 uses IN 0x81 / OUT 0x02, but alt-firmware adapters (HHL GC
    // Pocket / RP2040) number them differently, which made the hard-coded OUT
    // transfer below fail with LIBUSB_ERROR_NOT_FOUND.
    gca_discover_endpoints();

    // attempt to begin polling
    uint8_t cmd = GCA_COMMAND_POLL;
    ret = libusb_interrupt_transfer(l_DeviceHandle, l_EndpointOut, &cmd, sizeof(cmd), nullptr, 16);
    if (ret != LIBUSB_SUCCESS)
    {
        libusb_release_interface(l_DeviceHandle, 0);
        libusb_close(l_DeviceHandle);
        debugMessage = "gca_init(): failed to send polling cmd: ";
        debugMessage += libusb_error_name(ret);
        PluginDebugMessage(M64MSG_ERROR, debugMessage);
        return false;
    }

    debugMessage = "gca_init(): successfully opened adapter";
    PluginDebugMessage(M64MSG_INFO, debugMessage);
    return true;
}

static void gca_quit(void)
{
    if (l_DeviceHandle != nullptr)
    {
        libusb_release_interface(l_DeviceHandle, 0);
        libusb_close(l_DeviceHandle);
        l_DeviceHandle = nullptr;
    }
}

static void gca_poll_thread(void)
{
    uint8_t readBuf[37] = {0};
    int transferred = 0;
    int ret;
    int offset;
    std::array<GameCubeAdapterControllerState, 4> state;
    std::string debugMessage;

    while (l_PollThreadRunning.load(std::memory_order_relaxed))
    {
        ret = libusb_interrupt_transfer(l_DeviceHandle, l_EndpointIn, readBuf, sizeof(readBuf), &transferred, 16);
        if (ret == LIBUSB_ERROR_NO_DEVICE)
        {
            debugMessage = "gca_poll_thread(): adapter disconnected, stopping polling thread";
            PluginDebugMessage(M64MSG_WARNING, debugMessage);

            // reset state
            gca_reset_state();

            // ensure that we don't get stuck in InitiateControllers(),
            // because that might be waiting on l_PolledState to be set
            l_PolledState.store(true);
            return;
        }
        else if (ret != LIBUSB_SUCCESS || transferred != sizeof(readBuf))
        {
            debugMessage = "gca_poll_thread(): failed to retrieve input buffer: ";
            debugMessage += libusb_error_name(ret);
            PluginDebugMessage(M64MSG_WARNING, debugMessage);
            continue;
        }

        for (int i = 0; i < NUM_CONTROLLERS; i++)
        {
            offset = (i * 9);
            state[i].Status       = readBuf[offset + 1];
            state[i].Buttons1     = readBuf[offset + 2];
            state[i].Buttons2     = readBuf[offset + 3];
            state[i].LeftStickX   = readBuf[offset + 4];
            state[i].LeftStickY   = readBuf[offset + 5];
            state[i].RightStickX  = readBuf[offset + 6];
            state[i].RightStickY  = readBuf[offset + 7];
            state[i].LeftTrigger  = readBuf[offset + 8];
            state[i].RightTrigger = readBuf[offset + 9];
        }

        l_ControllerStateMutex.lock();
        l_ControllerState = state;
        l_ControllerStateMutex.unlock();

        l_PolledState.store(true, std::memory_order_relaxed);

        // poll every 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void load_settings(void)
{
    l_Settings.DeadzoneValue = static_cast<double>(CoreSettingsGetIntValue(SettingsID::GCAInput_Deadzone)) / 100.0;
    l_Settings.SensitivityValue = GCASensitivityPercentToScale(CoreSettingsGetIntValue(SettingsID::GCAInput_Sensitivity));
    l_Settings.CButtonTreshold = static_cast<double>(CoreSettingsGetIntValue(SettingsID::GCAInput_CButtonTreshold)) / 100.0;
    l_Settings.TriggerTreshold = static_cast<double>(CoreSettingsGetIntValue(SettingsID::GCAInput_TriggerTreshold)) / 100.0;
    l_Settings.PortEnabled[0] = CoreSettingsGetBoolValue(SettingsID::GCAInput_Port1Enabled);
    l_Settings.PortEnabled[1] = CoreSettingsGetBoolValue(SettingsID::GCAInput_Port2Enabled);
    l_Settings.PortEnabled[2] = CoreSettingsGetBoolValue(SettingsID::GCAInput_Port3Enabled);
    l_Settings.PortEnabled[3] = CoreSettingsGetBoolValue(SettingsID::GCAInput_Port4Enabled);

    l_Settings.Mapping.A       = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_A));
    l_Settings.Mapping.B       = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_B));
    l_Settings.Mapping.Start   = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_Start));
    l_Settings.Mapping.Z       = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_Z));
    l_Settings.Mapping.Z2      = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_Z2));
    l_Settings.Mapping.L       = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_L));
    l_Settings.Mapping.R       = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_R));
    l_Settings.Mapping.DpadUp    = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_DpadUp));
    l_Settings.Mapping.DpadDown  = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_DpadDown));
    l_Settings.Mapping.DpadLeft  = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_DpadLeft));
    l_Settings.Mapping.DpadRight = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_DpadRight));
    l_Settings.Mapping.CUp     = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_CUp));
    l_Settings.Mapping.CDown   = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_CDown));
    l_Settings.Mapping.CLeft   = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_CLeft));
    l_Settings.Mapping.CRight  = static_cast<GCInput>(CoreSettingsGetIntValue(SettingsID::GCAInput_Map_CRight));
}

static int scale_axis(const double input, const double deadzone, const double n64Max)
{
    const double inputAbs = std::abs(input);

    if (inputAbs <= deadzone)
    {
        return 0;
    }

    const double deadzoneRelation = 1.0 / (1.0 - deadzone);
    const double scaled = (inputAbs - deadzone) * deadzoneRelation * n64Max;

    const double axisMax = std::min(n64Max, static_cast<double>(INT8_MAX));
    const int result = static_cast<int>(std::min(scaled, axisMax));
    return (input >= 0) ? result : -result;
}

//
// Adapter Accessor Functions (for config UI)
//

bool GCA_StartConfigPolling(void)
{
    // If poll thread is already running, nothing to do
    if (l_PollThreadRunning.load())
    {
        return true;
    }

    if (!l_UsbInitialized)
    {
        if (!usb_init())
        {
            return false;
        }
    }

    if (!gca_init())
    {
        return false;
    }

    l_PollThread = std::thread(gca_poll_thread);
    l_ConfigPollingStarted = true;

    // Wait for initial state
    while (!l_PolledState.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return true;
}

void GCA_StopConfigPolling(void)
{
    if (!l_ConfigPollingStarted)
    {
        return;
    }

    if (l_PollThread.joinable())
    {
        l_PollThreadRunning.store(false);
        l_PollThread.join();
    }

    gca_quit();
    l_ConfigPollingStarted = false;
}

GameCubeAdapterControllerState GCA_GetControllerState(int port)
{
    std::lock_guard<std::mutex> lock(l_ControllerStateMutex);
    return l_ControllerState[port];
}

//
// Basic Plugin Functions
//

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context, void (*DebugCallback)(void *, int, const char *))
{
    // setup debug callback
    l_DebugCallback    = DebugCallback;
    l_DebugCallContext = Context;

    if (!usb_init())
    {
        return M64ERR_SYSTEM_FAIL;
    }

    load_settings();
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    // clear debug callback
    l_DebugCallback    = nullptr;
    l_DebugCallContext = nullptr;

    usb_quit();

    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *pluginType, int *pluginVersion, 
    int *apiVersion, const char **pluginNamePtr, int *capabilities)
{
    if (pluginType != nullptr)
    {
        *pluginType = M64PLUGIN_INPUT;
    }

    if (pluginVersion != nullptr)
    {
        *pluginVersion = 0x010000;
    }

    if (apiVersion != nullptr)
    {
        *apiVersion = INPUT_PLUGIN_API_VERSION;
    }

    if (pluginNamePtr != nullptr)
    {
        *pluginNamePtr = "Rosalie's Mupen GUI - GameCube Adapter Input Plugin";
    }

    if (capabilities != nullptr)
    {
        *capabilities = 0;
    }

    return M64ERR_SUCCESS;
}

//
// Custom Plugin Functions
//

EXPORT m64p_error CALL PluginConfig(void* parent)
{
    UserInterface::MainDialog dialog((QWidget*)parent);
    dialog.exec();

    // re-load settings
    load_settings();

    return M64ERR_SUCCESS;
}

//
// Input Plugin Functions
//

EXPORT void CALL ControllerCommand(int Control, unsigned char* Command)
{
}

EXPORT void CALL GetKeys(int Control, BUTTONS* Keys)
{
    if (Control < 0 || Control >= NUM_CONTROLLERS)
    {
        Keys->Value = 0;
        return;
    }

    // Use the port mapping to translate Control index to physical port
    int port = l_ControlToPort[Control];
    if (port < 0)
    {
        Keys->Value = 0;
        return;
    }

    l_ControllerStateMutex.lock();
    GameCubeAdapterControllerState state = l_ControllerState[port];
    l_ControllerStateMutex.unlock();

    if (!state.Status)
    {
        Keys->Value = 0;
        return;
    }

    const GCButtonMapping& map = l_Settings.Mapping;
    const double trigT = l_Settings.TriggerTreshold;
    const double cT = l_Settings.CButtonTreshold;

    Keys->A_BUTTON     = isGCInputActive(state, map.A, trigT, cT);
    Keys->B_BUTTON     = isGCInputActive(state, map.B, trigT, cT);
    Keys->START_BUTTON = isGCInputActive(state, map.Start, trigT, cT);
    Keys->Z_TRIG       = isGCInputActive(state, map.Z, trigT, cT) ||
                         (map.Z2 != GCInput::None && isGCInputActive(state, map.Z2, trigT, cT));
    Keys->L_TRIG       = isGCInputActive(state, map.L, trigT, cT);
    Keys->R_TRIG       = isGCInputActive(state, map.R, trigT, cT);
    Keys->U_DPAD       = isGCInputActive(state, map.DpadUp, trigT, cT);
    Keys->D_DPAD       = isGCInputActive(state, map.DpadDown, trigT, cT);
    Keys->L_DPAD       = isGCInputActive(state, map.DpadLeft, trigT, cT);
    Keys->R_DPAD       = isGCInputActive(state, map.DpadRight, trigT, cT);
    Keys->U_CBUTTON    = isGCInputActive(state, map.CUp, trigT, cT);
    Keys->D_CBUTTON    = isGCInputActive(state, map.CDown, trigT, cT);
    Keys->L_CBUTTON    = isGCInputActive(state, map.CLeft, trigT, cT);
    Keys->R_CBUTTON    = isGCInputActive(state, map.CRight, trigT, cT);

    // Analog stick (not remappable)
    const int8_t x = static_cast<int8_t>(state.LeftStickX + 128);
    const int8_t y = static_cast<int8_t>(state.LeftStickY + 128);

    const double inputX = static_cast<double>(x) / static_cast<double>(INT8_MAX);
    const double inputY = static_cast<double>(y) / static_cast<double>(INT8_MAX);
    const double n64Max = GCA_N64_AXIS_PEAK * l_Settings.SensitivityValue;

    Keys->X_AXIS = scale_axis(inputX, l_Settings.DeadzoneValue, n64Max);
    Keys->Y_AXIS = scale_axis(inputY, l_Settings.DeadzoneValue, n64Max);
}

EXPORT void CALL InitiateControllers(CONTROL_INFO ControlInfo)
{
    load_settings();

    if (!gca_init())
    {
        return;
    }

    // start polling thread
    l_PollThread = std::thread(gca_poll_thread);

    // wait for initial state to be polled
    while (!l_PolledState.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Map enabled physical ports to Control slots sequentially.
    // This keeps Control 0 stable for rollback even if the adapter reports
    // controller presence slightly after InitiateControllers().
    l_ControllerStateMutex.lock();
    l_ControlToPort = {-1, -1, -1, -1};
    int controlSlot = 0;
    for (int i = 0; i < NUM_CONTROLLERS; i++)
    {
        if (l_Settings.PortEnabled[i])
        {
            l_ControlToPort[controlSlot] = i;
            controlSlot++;
        }
    }

    for (int i = 0; i < NUM_CONTROLLERS; i++)
    {
        const int port = l_ControlToPort[i];
        ControlInfo.Controls[i].Present = (port >= 0 && l_ControllerState[port].Status > 0) ? 1 : 0;
    }
    l_ControllerStateMutex.unlock();
}

EXPORT void CALL ReadController(int Control, unsigned char *Command)
{
}

EXPORT int CALL RomOpen(void)
{
    return 1;
}

EXPORT void CALL RomClosed(void)
{
    // wait for polling thread to stop
    if (l_PollThread.joinable())
    {
        l_PollThreadRunning.store(false);
        l_PollThread.join();
    }

    gca_quit();
}

EXPORT void CALL SDL_KeyDown(int keymod, int keysym)
{
}

EXPORT void CALL SDL_KeyUp(int keymod, int keysym)
{
}
