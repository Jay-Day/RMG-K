#ifndef RAPHNETPOLLINGHEALTH_HPP
#define RAPHNETPOLLINGHEALTH_HPP

#include <cstdint>
#include <3rdParty/mupen64plus-input-raphnetraw/src/polling_health.h>

// Preview measurements drive a notice only, using the plugin's sustained-latency
// thresholds. The input plugin remains responsible for pre-game mode selection.
struct RaphnetPollingHealth
{
    static constexpr int WindowSize = 120;
    int samples = 0;
    std::int64_t totalUs = 0;
    std::int64_t maximumUs = 0;
    raphnet_polling_health connection = {};

    void reset() { *this = {}; }

    void observe(std::int64_t elapsedUs, std::int64_t nowUs)
    {
        raphnet_health_observe(&connection, 1, elapsedUs, nowUs);
        if (samples == WindowSize) samples = totalUs = maximumUs = 0;
        ++samples;
        totalUs += elapsedUs;
        if (elapsedUs > maximumUs) maximumUs = elapsedUs;
    }
};

#endif
