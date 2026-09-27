/******************************************************************************
    SEIInjector - wall-clock + optional SNTP correction
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    UTC epoch microseconds from an initial wall-clock anchor plus monotonic
    elapsed time. Optional SNTP corrections slew without discontinuities.
    Freshness is a confidence indicator, not an authenticated accuracy proof.
******************************************************************************/

#ifndef REALTIME_CLOCK_H
#define REALTIME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct realtime_clock realtime_clock_t;

/*
 * Allocates and starts a clock. server may be empty (""), in which case NTP
 * is disabled; the initial UTC anchor still advances monotonically.
 * interval_ms is the background re-sync period (minimum 1000). Returns NULL on allocation
 * failure or unavailable monotonic clock.
 *
 * Initial synchronization runs on the worker, not the caller. Before the
 * first successful sample, accuracy of the initial system clock is unknown.
 * DNS/receive never hold the state mutex. Destroy joins the worker and can still wait for DNS/I/O.
 */
realtime_clock_t *realtime_clock_create(const char *server, uint16_t port, uint32_t interval_ms);

void realtime_clock_destroy(realtime_clock_t *clock);

/*
 * Current absolute time in microseconds since the Unix epoch, corrected with
 * a bounded gradual SNTP slew. No per-frame wall-clock dependency. Loss of
 * calibration clears trust after 180 seconds but does not stop the clock.
 * Only briefly locks in-memory state; never waits for network I/O.
 */
int64_t realtime_clock_now_us(realtime_clock_t *clock);

/* Atomically snapshot the offset and its provenance for one submitted frame. */
int64_t realtime_clock_snapshot(realtime_clock_t *clock, bool *ntp_synced);

/* True only with a recently accepted sample (at most 180 seconds old). */
bool realtime_clock_ntp_synced(realtime_clock_t *clock);

/* Diagnostic server - wall offset at publication; not used for timestamps. */
int64_t realtime_clock_offset_us(realtime_clock_t *clock);

/* Successful / failed sync counts (informational). */
uint32_t realtime_clock_sync_count(realtime_clock_t *clock);
uint32_t realtime_clock_fail_count(realtime_clock_t *clock);

/* Raw local wall clock, no correction. */
int64_t realtime_clock_local_now_us(void);

#ifdef __cplusplus
}
#endif

#endif /* REALTIME_CLOCK_H */
