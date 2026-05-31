/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "InputModule.hpp"
#include "../RMG-Core/Emulation.hpp"

#include <quickjs.h>

#include <cstdint>

namespace RMGScript {

struct ButtonInfo {
    const char* jsName;
    const char* label;
    uint16_t mask;
};

// Masks match mupen64plus BUTTONS.Value layout: b1 in high byte, b0 in low byte.
// b0: A(7) B(6) Z(5) Start(4) DpadUp(3) DpadDown(2) DpadLeft(1) DpadRight(0)
// b1: Res(7) Res(6) L(5) R(4) CUp(3) CDown(2) CLeft(1) CRight(0)
static const ButtonInfo BUTTON_INFO[] = {
    {"BUTTON_DPAD_RIGHT", "DpadRight", 0x0001u},
    {"BUTTON_DPAD_LEFT",  "DpadLeft",  0x0002u},
    {"BUTTON_DPAD_DOWN",  "DpadDown",  0x0004u},
    {"BUTTON_DPAD_UP",    "DpadUp",    0x0008u},
    {"BUTTON_START",      "Start",     0x0010u},
    {"BUTTON_Z",          "Z",         0x0020u},
    {"BUTTON_B",          "B",         0x0040u},
    {"BUTTON_A",          "A",         0x0080u},
    {"BUTTON_C_RIGHT",    "CRight",    0x0100u},
    {"BUTTON_C_LEFT",     "CLeft",     0x0200u},
    {"BUTTON_C_DOWN",     "CDown",     0x0400u},
    {"BUTTON_C_UP",       "CUp",       0x0800u},
    {"BUTTON_R",          "R",         0x1000u},
    {"BUTTON_L",          "L",         0x2000u},
};

static JSValue CreateButtonNameArray(JSContext* ctx, uint16_t buttons)
{
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& b : BUTTON_INFO) {
        if (buttons & b.mask)
            JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, b.label));
    }
    return array;
}

static bool GetPortIndex(JSContext* ctx, JSValue arg, uint32_t& outIndex)
{
    int32_t value;
    if (JS_ToInt32(ctx, &value, arg) != 0) return false;

    if (value >= 1 && value <= 4) { outIndex = static_cast<uint32_t>(value - 1); return true; }
    if (value >= 0 && value <= 3) { outIndex = static_cast<uint32_t>(value);     return true; }

    JS_ThrowTypeError(ctx, "input port must be 0-3 or 1-4");
    return false;
}

// Builds a controller state object from CoreGetControllerState data.
static JSValue MakePortObject(JSContext* ctx, uint32_t portIndex)
{
    bool connected = false, valid = false;
    uint8_t rx[4] = {};
    CoreGetControllerState(static_cast<int>(portIndex), connected, valid, rx);

    // buttons word: b1 in high byte, b0 in low byte — matches BUTTON_INFO masks.
    const uint16_t buttons = valid
        ? static_cast<uint16_t>((static_cast<uint16_t>(rx[1]) << 8) | rx[0])
        : 0u;
    const uint32_t raw = valid
        ? (static_cast<uint32_t>(rx[0]) << 24 | static_cast<uint32_t>(rx[1]) << 16 |
           static_cast<uint32_t>(rx[2]) << 8  | static_cast<uint32_t>(rx[3]))
        : 0u;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "port",           JS_NewInt32(ctx, static_cast<int32_t>(portIndex + 1)));
    JS_SetPropertyStr(ctx, obj, "connected",      JS_NewBool(ctx, connected));
    JS_SetPropertyStr(ctx, obj, "valid",          JS_NewBool(ctx, valid));
    JS_SetPropertyStr(ctx, obj, "raw",            JS_NewUint32(ctx, raw));
    JS_SetPropertyStr(ctx, obj, "buttons",        JS_NewUint32(ctx, buttons));
    JS_SetPropertyStr(ctx, obj, "buttonsPressed", CreateButtonNameArray(ctx, buttons));
    JS_SetPropertyStr(ctx, obj, "x",              JS_NewInt32(ctx, static_cast<int8_t>(rx[2])));
    JS_SetPropertyStr(ctx, obj, "y",              JS_NewInt32(ctx, static_cast<int8_t>(rx[3])));
    return obj;
}

static JSValue Input_GetPortState(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getPortState(port): expected a port number");
    uint32_t portIndex;
    if (!GetPortIndex(ctx, argv[0], portIndex)) return JS_EXCEPTION;
    return MakePortObject(ctx, portIndex);
}

static JSValue Input_GetPortStates(JSContext* ctx, JSValue, int, JSValue*)
{
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < 4; ++i)
        JS_SetPropertyUint32(ctx, array, i, MakePortObject(ctx, i));
    return array;
}

static JSValue Input_GetPressedButtons(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getPressedButtons(port): expected a port number");
    uint32_t portIndex;
    if (!GetPortIndex(ctx, argv[0], portIndex)) return JS_EXCEPTION;

    bool connected = false, valid = false;
    uint8_t rx[4] = {};
    CoreGetControllerState(static_cast<int>(portIndex), connected, valid, rx);
    const uint16_t buttons = valid
        ? static_cast<uint16_t>((static_cast<uint16_t>(rx[1]) << 8) | rx[0])
        : 0u;
    return CreateButtonNameArray(ctx, buttons);
}

static JSValue Input_DecodeButtons(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "decodeButtons(buttonMask): expected a button mask");
    int32_t mask;
    if (JS_ToInt32(ctx, &mask, argv[0]) != 0) return JS_EXCEPTION;
    return CreateButtonNameArray(ctx, static_cast<uint16_t>(mask));
}

void RegisterInputModule(JSContext* ctx)
{
    JSValue input = JS_NewObject(ctx);

#define SET(name, fn, nargs) \
    JS_SetPropertyStr(ctx, input, name, JS_NewCFunction(ctx, fn, name, nargs))

    SET("getPortState",      Input_GetPortState,      1);
    SET("getPortStates",     Input_GetPortStates,     0);
    SET("getPressedButtons", Input_GetPressedButtons, 1);
    SET("decodeButtons",     Input_DecodeButtons,     1);

#undef SET

    for (const auto& b : BUTTON_INFO)
        JS_SetPropertyStr(ctx, input, b.jsName, JS_NewUint32(ctx, b.mask));

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "input", input);
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
