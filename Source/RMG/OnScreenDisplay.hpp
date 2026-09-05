/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef RMG_ONSCREENDISPLAY_HPP
#define RMG_ONSCREENDISPLAY_HPP

#include <array>
#include <string>

// attempts to initialize the OSD
bool OnScreenDisplayInit(void);

// shuts down the OSD when initialized
void OnScreenDisplayShutdown(void);

// loads settings for the OSD
void OnScreenDisplayLoadSettings(void);

// sets the display size for the OSD
bool OnScreenDisplaySetDisplaySize(int width, int height);

// adds a message to the OSD message queue
// (pass an empty string to clear queued system messages)
void OnScreenDisplaySetMessage(std::string message);

// adds a Kaillera in-game chat message to the OSD message queue
// (pass an empty string to clear queued chat messages)
void OnScreenDisplaySetKailleraChatMessage(std::string message);

// enables or disables rendering queued Kaillera chat messages
void OnScreenDisplaySetKailleraChatEnabled(bool enabled);

// adds a Kaillera in-game chat message without slide-in animation
// (pass an empty string to clear queued chat messages)
void OnScreenDisplaySetKailleraChatMessageImmediate(std::string message);

// sets the live input prompt message shown in the OSD message stack
// (pass an empty string to hide the input prompt)
void OnScreenDisplaySetInputPrompt(std::string message);

// sets live Kaillera port labels rendered by the OSD
void OnScreenDisplaySetKailleraPortLabels(int playerCount, const std::array<std::string, 4>& playerNames);

// clears live Kaillera port labels
void OnScreenDisplayClearKailleraPortLabels(void);

// Sets a large message box centered on screen, drawn over everything (e.g. the
// spectate "Stream is buffering..." banner). Pass an empty string to hide it.
void OnScreenDisplaySetCenterMessage(const std::string& message);

// Sets persistent live-replay audience text in the normal OSD message stack.
// Pass an empty string to hide it.
void OnScreenDisplaySetLiveReplayStatus(const std::string& message);

// renders the OSD
void OnScreenDisplayRender(void);

// pauses OSD rendering
void OnScreenDisplayPause(void);

// resumes OSD rendering
void OnScreenDisplayResume(void);

#endif // RMG_ONSCREENDISPLAY_HPP
