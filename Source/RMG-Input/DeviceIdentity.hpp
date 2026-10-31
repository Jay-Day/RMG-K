#ifndef INPUT_DEVICE_IDENTITY_HPP
#define INPUT_DEVICE_IDENTITY_HPP

#include <SDL3/SDL.h>
#include <string>
#include <vector>

struct InputDeviceIdentity
{
    std::string name;
    std::string path;
    std::string serial;
    std::string guid;
};

inline std::string InputDeviceGuid(SDL_JoystickID id)
{
    char value[33]{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), value, sizeof(value));
    return std::string(value) == "00000000000000000000000000000000" ? "" : value;
}

// SDL's mapping name may be generic ("XInput Controller"). Keep it for old
// profiles, but display the underlying joystick name separately in the UI.
inline InputDeviceIdentity ReadInputDeviceIdentity(SDL_JoystickID id)
{
    const auto text = [](const char* value) { return value ? std::string(value) : std::string(); };
    const bool gamepad = SDL_IsGamepad(id);
    InputDeviceIdentity result{
        text(gamepad ? SDL_GetGamepadNameForID(id) : SDL_GetJoystickNameForID(id)),
        text(SDL_GetJoystickPathForID(id)), {}, InputDeviceGuid(id)};
    if (auto* joystick = SDL_OpenJoystick(id))
    {
        result.serial = text(SDL_GetJoystickSerial(joystick));
        SDL_CloseJoystick(joystick);
    }
    return result;
}

// Return a unique match, never the first device with a generic name. The GUID
// identifies a hardware model, not an individual unit; serial/path distinguish
// multiple copies. XInput slot paths are usable only with compatible identity.
inline int FindInputDevice(const InputDeviceIdentity& saved, const std::vector<InputDeviceIdentity>& devices)
{
    const auto unique = [&](const auto& matches)
    {
        int found = -1;
        for (int i = 0; i < static_cast<int>(devices.size()); ++i)
        {
            if (!matches(devices[i])) continue;
            if (found >= 0) return -1;
            found = i;
        }
        return found;
    };
    const auto compatible = [&](const InputDeviceIdentity& device)
    {
        return (saved.guid.empty() || device.guid == saved.guid) &&
            (saved.serial.empty() || device.serial.empty() || saved.serial == device.serial);
    };
    if (!saved.serial.empty())
    {
        const int found = unique([&](const auto& device) {
            return compatible(device) && device.serial == saved.serial;
        });
        if (found >= 0) return found;
    }
    if (!saved.path.empty())
    {
        const int found = unique([&](const auto& device) {
            return compatible(device) && device.path == saved.path &&
                (!saved.guid.empty() || device.name == saved.name);
        });
        if (found >= 0) return found;
    }
    if (!saved.guid.empty() && saved.serial.empty())
        return unique([&](const auto& device) { return device.guid == saved.guid; });
    // Legacy profiles without paths can still match a unique name. A stored
    // path that disappeared must not silently select a different controller.
    if (saved.guid.empty() && saved.path.empty() && saved.serial.empty())
        return unique([&](const auto& device) { return device.name == saved.name; });
    return -1;
}

#endif
