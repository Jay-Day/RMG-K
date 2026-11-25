#ifndef RAPHNET_DIRECT_HINT_TIMING_H
#define RAPHNET_DIRECT_HINT_TIMING_H

#include "polling_health.h"

enum {
    DIRECT_HINT_INTERVALS = 16,
    DIRECT_HINT_EARLIEST_US = 4500,
    DIRECT_HINT_LATEST_US = 8000,
    DIRECT_HINT_GUARD_US = 2000
};

struct direct_read_history {
    int64_t previous_request_us;
    int64_t gaps_us[DIRECT_HINT_INTERVALS];
    unsigned int next, count;
};

/* Use only successful foreground reads. Return the latest time a probe may
 * start, reserving a full fast transaction and another 2 ms before the
 * earliest next request suggested by the recent intervals. */
static inline int64_t direct_hint_deadline(struct direct_read_history *history,
    int valid, int64_t requested, int64_t completed)
{
    const int64_t gap = requested - history->previous_request_us;
    const int fast = valid && completed > requested && completed - requested <= RAPHNET_FAST_POLL_US;
    if (!fast || !history->previous_request_us || gap <= 0 || gap > 100000) {
        history->count = history->next = 0;
    } else {
        history->gaps_us[history->next] = gap;
        history->next = (history->next + 1) % DIRECT_HINT_INTERVALS;
        if (history->count < DIRECT_HINT_INTERVALS) ++history->count;
    }
    history->previous_request_us = fast ? requested : 0;
    if (history->count < DIRECT_HINT_INTERVALS) return 0;
    int64_t shortest = history->gaps_us[0];
    for (unsigned int i = 1; i < DIRECT_HINT_INTERVALS; ++i)
        if (history->gaps_us[i] < shortest) shortest = history->gaps_us[i];
    return requested + shortest - RAPHNET_FAST_POLL_US - DIRECT_HINT_GUARD_US;
}

struct direct_probe_stats {
    int64_t start_us, end_us, last_incident_us, next_probe_us;
    int64_t max_overlap_us, max_official_us, first_official_us, last_official_us;
    unsigned int probes, incidents, slow, failed, overlaps;
    unsigned int official_reads, qualified_reads, official_over_2ms, clean_trials;
    int cooldown_s, incident_this_probe, overlap_this_probe;
};

static inline void direct_hint_backoff(struct direct_probe_stats *s, int64_t now)
{
    if (s->incident_this_probe) return;
    s->incident_this_probe = 1;
    ++s->incidents;
    s->clean_trials = 0;
    if (!s->cooldown_s || now - s->last_incident_us >= 60000000)
        s->cooldown_s = 1;
    else if (s->cooldown_s < 30)
        s->cooldown_s = s->cooldown_s > 15 ? 30 : 2 * s->cooldown_s;
    s->last_incident_us = now;
    s->next_probe_us = now + (int64_t)s->cooldown_s * 1000000;
}

static inline void direct_hint_finished(struct direct_probe_stats *s, int valid, int64_t start, int64_t end)
{
    ++s->probes;
    s->start_us = start;
    s->end_us = end;
    s->incident_this_probe = s->overlap_this_probe = 0;
    const int slow = end - start > RAPHNET_FAST_POLL_US;
    s->slow += (unsigned int)slow;
    if (!valid) ++s->failed;
    if (!valid || slow) {
        direct_hint_backoff(s, end);
    } else if (s->clean_trials < 4) {
        ++s->clean_trials;
        if (s->clean_trials < 4) s->next_probe_us = end + 250000;
    }
}

/* Called with the USB lock just acquired by game I/O. Measure only the
 * intersection of its lock wait and the actual optional USB transaction. */
static inline void direct_hint_game_wait(struct direct_probe_stats *s, int64_t requested, int64_t acquired)
{
    const int64_t from = requested > s->start_us ? requested : s->start_us;
    const int64_t to = acquired < s->end_us ? acquired : s->end_us;
    if (to <= from) return;
    if (to - from > s->max_overlap_us) s->max_overlap_us = to - from;
    if (!s->overlap_this_probe) {
        s->overlap_this_probe = 1;
        ++s->overlaps;
        direct_hint_backoff(s, acquired);
    }
}

#endif
