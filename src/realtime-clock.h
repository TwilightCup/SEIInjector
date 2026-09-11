/******************************************************************************
    SEIInjector - wall-clock + optional SNTP correction
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    Pure C (libc only). Provides:
      - epoch microseconds on the local wall clock, and
      - a background thread that periodically re-queries an SNTP server and
        maintains a constant offset that is added on top of the local clock.

    This lets every sender stamp frames with the same *absolute* time base
    (modulo each machine's clock quality) so a receiving application can line
    up several independent streams frame-by-frame.
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
 * is disabled and only the local wall clock is used. interval_ms is the
 * background re-sync period (minimum 1000). Returns NULL on allocation
 * failure.
 *
 * A single *blocking* initial sync is performed synchronously (bounded by a
 * short socket timeout) so the very first stamped frames already carry an
 * NTP-corrected time; afterwards the worker thread keeps the offset fresh.
 * A failed initial sync never fails the encoder: stamps simply fall back to
 * the local clock until a background sync succeeds.
 */
realtime_clock_t *realtime_clock_create(const char *server, uint16_t port, uint32_t interval_ms);

void realtime_clock_destroy(realtime_clock_t *clock);

/*
 * Current absolute time in microseconds since the Unix epoch, corrected with
 * the latest NTP offset (or plain local time when NTP is off / not yet
 * synced). Never blocks.
 */
int64_t realtime_clock_now_us(realtime_clock_t *clock);

/* True when the last NTP exchange succeeded (and NTP is enabled). */
bool realtime_clock_ntp_synced(realtime_clock_t *clock);

/* Last measured offset (server - local) in microseconds. */
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
