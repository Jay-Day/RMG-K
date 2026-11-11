#pragma once

#include "KrecParser.hpp"

#include <cstdint>
#include <vector>

namespace KailleraExport
{

// The recording's chat drawn over the exported video, the way the in-game
// on-screen display shows it (the user's OSD position, colours, duration and
// message limit), scaled to the video size. A line appears when the replay
// reads the input record it was recorded before, as in in-app playback.

// lines in record order; portLabels = keep the bottom free for the port
// labels when the chat sits at the bottom
void InitializeChatOverlay(const std::vector<KrecChatLine>& lines, double fps, bool portLabels);

// emulation thread: by output frame `frame` the replay has read
// `inputRecords` input records
void AdvanceChatOverlay(int inputRecords, int frame);

// encode thread: draws the chat visible at output frame `frame` onto a
// top-down RGB24 frame
void DrawChatOverlay(uint8_t* rgb, int width, int height, int frame);

// after the encode thread stopped
void ShutdownChatOverlay();

} // namespace KailleraExport
