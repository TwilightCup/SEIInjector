#include "pts-timeline.h"
#include <string.h>

bool pts_timeline_put(struct pts_timeline *t, struct stamp_time entry)
{
	if (t->count == STAMP_PTS_CAPACITY)
		return false;
	for (size_t i = 0; i < t->count; ++i)
		if (t->entries[i].pts == entry.pts)
			return false;
	t->entries[t->count++] = entry;
	return true;
}

bool pts_timeline_take(struct pts_timeline *t, int64_t pts, struct stamp_time *entry)
{
	for (size_t i = 0; i < t->count; ++i) {
		if (t->entries[i].pts != pts)
			continue;
		*entry = t->entries[i];
		memmove(t->entries + i, t->entries + i + 1, (t->count - i - 1) * sizeof(*t->entries));
		--t->count;
		return true;
	}
	return false;
}
