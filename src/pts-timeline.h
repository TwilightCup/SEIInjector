/* Per-submission timestamps, independent of encoder output order. */
#ifndef PTS_TIMELINE_H
#define PTS_TIMELINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STAMP_PTS_CAPACITY 4096u
struct stamp_time {
	int64_t pts;
	int64_t epoch_us;
	bool ntp;
};
struct pts_timeline {
	struct stamp_time entries[STAMP_PTS_CAPACITY];
	size_t count;
};
/* Duplicate PTS/full storage fail without evicting an outstanding frame. */
bool pts_timeline_put(struct pts_timeline *timeline, struct stamp_time entry);
bool pts_timeline_take(struct pts_timeline *timeline, int64_t pts, struct stamp_time *entry);
#endif
