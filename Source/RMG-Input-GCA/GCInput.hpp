/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef GCINPUT_HPP
#define GCINPUT_HPP

#include <QString>
#include "Adapter.hpp"

// Keep the practical default at 100%, with extra headroom up to 200%.
constexpr double GCA_N64_AXIS_PEAK = 85.0;
constexpr double GCA_SENSITIVITY_OFFSET = 0.90;
constexpr double GCA_SENSITIVITY_PER_PERCENT = 0.0045;

inline double GCASensitivityPercentToScale(int sensitivityPercent)
{
    return GCA_SENSITIVITY_OFFSET + (static_cast<double>(sensitivityPercent) * GCA_SENSITIVITY_PER_PERCENT);
}

enum class GCInput : int
{
    None = -1,
    A = 0,
    B,
    X,
    Y,
    Z,
    Start,
    L,
    R,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    CStickUp,
    CStickDown,
    CStickLeft,
    CStickRight,
    Count
};

#define N64_BUTTON_COUNT 15

struct GCButtonMapping
{
    GCInput A       = GCInput::A;
    GCInput B       = GCInput::B;
    GCInput Start   = GCInput::Start;
    GCInput Z       = GCInput::Z;
    GCInput Z2      = GCInput::None;
    GCInput L       = GCInput::LeftTrigger;
    GCInput R       = GCInput::RightTrigger;
    GCInput DpadUp    = GCInput::DpadUp;
    GCInput DpadDown  = GCInput::DpadDown;
    GCInput DpadLeft  = GCInput::DpadLeft;
    GCInput DpadRight = GCInput::DpadRight;
    GCInput CUp     = GCInput::CStickUp;
    GCInput CDown   = GCInput::CStickDown;
    GCInput CLeft   = GCInput::CStickLeft;
    GCInput CRight  = GCInput::CStickRight;
};

inline QString GCInputToString(GCInput input)
{
    switch (input)
    {
    case GCInput::A:            return "A";
    case GCInput::B:            return "B";
    case GCInput::X:            return "X";
    case GCInput::Y:            return "Y";
    case GCInput::Z:            return "Z";
    case GCInput::Start:        return "Start";
    case GCInput::L:            return "L";
    case GCInput::R:            return "R";
    case GCInput::DpadUp:       return "D-Pad Up";
    case GCInput::DpadDown:     return "D-Pad Down";
    case GCInput::DpadLeft:     return "D-Pad Left";
    case GCInput::DpadRight:    return "D-Pad Right";
    case GCInput::LeftTrigger:  return "L Trigger";
    case GCInput::RightTrigger: return "R Trigger";
    case GCInput::CStickUp:     return "C-Stick Up";
    case GCInput::CStickDown:   return "C-Stick Down";
    case GCInput::CStickLeft:   return "C-Stick Left";
    case GCInput::CStickRight:  return "C-Stick Right";
    default:                    return "None";
    }
}

inline bool isGCInputActive(const GameCubeAdapterControllerState& state, GCInput input, double triggerThreshold, double cStickThreshold)
{
    const int triggerThresh = static_cast<int>(INT8_MAX * triggerThreshold);
    const int cStickThresh  = static_cast<int>(INT8_MAX * cStickThreshold);
    const int8_t cX = static_cast<int8_t>(state.RightStickX + 128);
    const int8_t cY = static_cast<int8_t>(state.RightStickY + 128);

    switch (input)
    {
    case GCInput::A:            return state.A;
    case GCInput::B:            return state.B;
    case GCInput::X:            return state.X;
    case GCInput::Y:            return state.Y;
    case GCInput::Z:            return state.Z;
    case GCInput::Start:        return state.Start;
    case GCInput::L:            return state.L;
    case GCInput::R:            return state.R;
    case GCInput::DpadUp:       return state.DpadUp;
    case GCInput::DpadDown:     return state.DpadDown;
    case GCInput::DpadLeft:     return state.DpadLeft;
    case GCInput::DpadRight:    return state.DpadRight;
    case GCInput::LeftTrigger:  return state.LeftTrigger > triggerThresh;
    case GCInput::RightTrigger: return state.RightTrigger > triggerThresh;
    case GCInput::CStickUp:     return cY > cStickThresh;
    case GCInput::CStickDown:   return cY < -cStickThresh;
    case GCInput::CStickLeft:   return cX < -cStickThresh;
    case GCInput::CStickRight:  return cX > cStickThresh;
    default:                    return false;
    }
}

inline GCInput DetectGCInput(const GameCubeAdapterControllerState& prev, const GameCubeAdapterControllerState& curr, double triggerThreshold, double cStickThreshold)
{
    const int triggerThresh = static_cast<int>(INT8_MAX * triggerThreshold);
    const int cStickThresh  = static_cast<int>(INT8_MAX * cStickThreshold);

    // Check digital buttons
    if (!prev.A && curr.A) return GCInput::A;
    if (!prev.B && curr.B) return GCInput::B;
    if (!prev.X && curr.X) return GCInput::X;
    if (!prev.Y && curr.Y) return GCInput::Y;
    if (!prev.Z && curr.Z) return GCInput::Z;
    if (!prev.Start && curr.Start) return GCInput::Start;
    if (!prev.L && curr.L) return GCInput::L;
    if (!prev.R && curr.R) return GCInput::R;
    if (!prev.DpadUp && curr.DpadUp) return GCInput::DpadUp;
    if (!prev.DpadDown && curr.DpadDown) return GCInput::DpadDown;
    if (!prev.DpadLeft && curr.DpadLeft) return GCInput::DpadLeft;
    if (!prev.DpadRight && curr.DpadRight) return GCInput::DpadRight;

    // Check analog triggers (thresholded)
    if (prev.LeftTrigger <= triggerThresh && curr.LeftTrigger > triggerThresh) return GCInput::LeftTrigger;
    if (prev.RightTrigger <= triggerThresh && curr.RightTrigger > triggerThresh) return GCInput::RightTrigger;

    // Check C-Stick (thresholded)
    const int8_t prevCX = static_cast<int8_t>(prev.RightStickX + 128);
    const int8_t prevCY = static_cast<int8_t>(prev.RightStickY + 128);
    const int8_t currCX = static_cast<int8_t>(curr.RightStickX + 128);
    const int8_t currCY = static_cast<int8_t>(curr.RightStickY + 128);

    if (prevCY <= cStickThresh && currCY > cStickThresh) return GCInput::CStickUp;
    if (prevCY >= -cStickThresh && currCY < -cStickThresh) return GCInput::CStickDown;
    if (prevCX >= -cStickThresh && currCX < -cStickThresh) return GCInput::CStickLeft;
    if (prevCX <= cStickThresh && currCX > cStickThresh) return GCInput::CStickRight;

    return GCInput::None;
}

#endif // GCINPUT_HPP
