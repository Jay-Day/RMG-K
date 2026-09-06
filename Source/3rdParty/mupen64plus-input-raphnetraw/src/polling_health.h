#ifndef RAPHNET_POLLING_HEALTH_H
#define RAPHNET_POLLING_HEALTH_H

#include <stdint.h>
#include <string.h>

/* Shared by the worker and raw input path under the USB lock. Times are monotonic.
 * A missing controller supplies no evidence about USB latency. Separate entry and
 * recovery thresholds, and a longer recovery period, prevent mode oscillation. */
typedef struct {
    int cached;
    int samples;
    int slow_samples;
    int fast_samples;
    int64_t started_us;
    int64_t last_valid_us;
    int64_t assessed_us;
} raphnet_polling_health;

static inline void raphnet_health_break_window(raphnet_polling_health *health)
{
    health->samples = health->slow_samples = health->fast_samples = 0;
    health->started_us = 0;
}

static inline void raphnet_health_observe(raphnet_polling_health *health,
                                         int valid, int64_t elapsed_us, int64_t now_us)
{
    if (!valid) {
        raphnet_health_break_window(health);
        return;
    }
    if (now_us - health->last_valid_us > 2000000)
        raphnet_health_break_window(health);
    health->last_valid_us = now_us;
    if (health->samples == 0) health->started_us = now_us;
    ++health->samples;
    if (elapsed_us >= 4000) ++health->slow_samples;
    if (elapsed_us <= 2000) ++health->fast_samples;

    if (!health->cached && health->samples >= 120 && now_us - health->started_us >= 2000000) {
        health->assessed_us = now_us;
        if (health->slow_samples * 5 >= health->samples * 4) health->cached = 1;
        raphnet_health_break_window(health);
    } else if (health->cached && health->samples >= 500 && now_us - health->started_us >= 10000000) {
        health->assessed_us = now_us;
        if (health->fast_samples * 10 >= health->samples * 9) health->cached = 0;
        raphnet_health_break_window(health);
    }
}

/* Cache only the standard four-byte input response. Status, reset, and pak
 * commands continue through the raw path, preserving accessory capabilities. */
static inline int raphnet_is_input_read(const unsigned char *command)
{
    return command && command[0] == 1 && (command[1] & 0x3f) == 4 && command[2] == 1;
}

#endif
