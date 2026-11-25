#pragma once

#include <cstdint>
#include <string>

namespace LobbyButtonHints
{
    // The emulation thread owns the session and predictions. The adapter worker
    // can only publish a provisional sample for its currently armed ticket.
    void reset(std::uint64_t match, int epoch, std::uint64_t peer);
    void receive(int currentFrame);
    void arm(int currentFrame, int delay, std::uint32_t input, bool liveInput, int control);
    void predict(int frame, int lastReal, unsigned char* input);
    void confirm(int frame, const unsigned char* actual);
    std::string summary();
}
