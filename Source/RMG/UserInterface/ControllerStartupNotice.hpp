#ifndef CONTROLLERSTARTUPNOTICE_HPP
#define CONTROLLERSTARTUPNOTICE_HPP

#include <algorithm>
#include <cstdint>

// One brief notice on the ROM list per application launch. Wait for measured
// slow polling, then expire after 15 seconds without rearming on reconnects.
struct ControllerStartupNotice
{
    bool shown = false;
    std::int64_t deadlineMs = 0;

    bool observe(bool slow, bool romListVisible, std::int64_t nowMs)
    {
        if (!slow)
        {
            deadlineMs = 0;
            return false;
        }
        if (!romListVisible) return false;
        if (!shown)
        {
            shown = true;
            deadlineMs = nowMs + 15000;
        }
        return nowMs < deadlineMs;
    }

    // Launching a game ends the startup notice for this application run.
    void dismiss() { shown = true; deadlineMs = 0; }

    std::int64_t nextChangeMs(std::int64_t nowMs) const
    {
        return std::max<std::int64_t>(0, deadlineMs - nowMs);
    }
};

#endif
