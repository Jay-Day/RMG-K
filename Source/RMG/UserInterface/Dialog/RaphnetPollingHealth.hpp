#ifndef RAPHNETPOLLINGHEALTH_HPP
#define RAPHNETPOLLINGHEALTH_HPP

#include <cstdint>
#include <3rdParty/mupen64plus-input-raphnetraw/src/polling_health.h>

// Preview measurements supply latency warnings and an estimate of Automatic mode.
struct RaphnetPollingHealth
{
    static constexpr int WindowSize = 120;
    int samples = 0;
    std::int64_t totalUs = 0;
    std::int64_t maximumUs = 0;
    raphnet_polling_health connection = {};
    bool initialLatencyWarning = false;

    void reset() { *this = {}; }

    // An empty port is not evidence that a previously slow connection recovered.
    void missingResponse() { raphnet_health_break_window(&connection); }

    bool shouldWarn() const
    {
        return samples == 0 ? initialLatencyWarning : raphnet_average_exceeds_warning_cutoff(totalUs, samples);
    }

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
