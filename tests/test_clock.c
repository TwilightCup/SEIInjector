/* Deterministic wall/monotonic inputs exercise the actual implementation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
static int64_t wall_us = 1772953200000000LL;
static int64_t mono_us = 1000000;
static int test_gettimeofday(struct timeval *tv, void *unused)
{
	(void)unused;
	tv->tv_sec = wall_us / 1000000;
	tv->tv_usec = wall_us % 1000000;
	return 0;
}
static clockid_t last_clock_id;
static int test_clock_gettime(clockid_t id, struct timespec *ts)
{
	last_clock_id = id;
	ts->tv_sec = mono_us / 1000000;
	ts->tv_nsec = (mono_us % 1000000) * 1000;
	return 0;
}
#define gettimeofday test_gettimeofday
#define clock_gettime test_clock_gettime
#include "../src/realtime-clock.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static realtime_clock_t *observed;
static bool fail_exchange;
static int64_t sample_error = 100000;
static bool fake_exchange(const char *server, uint16_t port, struct rtc_sample *sample)
{
	(void)server;
	(void)port;
	CHECK(pthread_mutex_trylock(&observed->lock) == 0);
	pthread_mutex_unlock(&observed->lock);
	if (fail_exchange)
		return false;
	bool synced;
	int64_t predicted = realtime_clock_snapshot(observed, &synced);
	*sample = (struct rtc_sample){predicted + sample_error, mono_us, 1000};
	return true;
}
static int64_t snapshot(bool *synced)
{
	return realtime_clock_snapshot(observed, synced);
}

int main(void)
{
	int64_t off, rtt;
	CHECK(rtc_measurement(1000000, 1120000, 1125000, 1045000, &off, &rtt));
	CHECK(off == 100000 && rtt == 40000);
	CHECK(!rtc_measurement(1000000, 1120000, 1125000, 1045000 + 3600000000LL, &off, &rtt));
	CHECK(!rtc_measurement(1000000, 1120000, 1125000, 1000000, &off, &rtt));
	CHECK(!rtc_measurement(10, 20, 19, 30, &off, &rtt));
	CHECK(rtc_wall_stable(1000000, 1045000, 0, 45000));
	CHECK(!rtc_wall_stable(1000000, 1045000 + 3600000000LL, 0, 45000));
	CHECK(!rtc_wall_stable(1000000, 1045000 - 3600000000LL, 0, 45000));
	/* Even a small backward step that leaves RTT positive must be rejected. */
	CHECK(!rtc_wall_stable(1000000, 1100000, 0, 200000));
	ntp_ts_t after_era = {1, 0};
	CHECK(ntp_to_epoch_us(&after_era, 2085978497000000LL) == 2085978497000000LL);

	/* Empty server prevents network/thread creation, not continuous time. */
	observed = realtime_clock_create("", 123, 60000);
	CHECK(observed);
	bool synced;
	int64_t start = snapshot(&synced);
	CHECK(start == wall_us && !synced);
#ifdef CLOCK_BOOTTIME
	CHECK(last_clock_id == CLOCK_BOOTTIME);
#else
	CHECK(last_clock_id == CLOCK_MONOTONIC);
#endif
	mono_us += 1000000;
	wall_us += 3600000000LL;
	CHECK(snapshot(&synced) == start + 1000000 && !synced);
	wall_us -= 7200000000LL;
	mono_us += 1000000;
	CHECK(snapshot(&synced) == start + 2000000);

	/* Successful calibration does not jump the current stamp. */
	strcpy(observed->server, "offline");
	int64_t before = snapshot(&synced);
	rtc_sync_once(observed, fake_exchange);
	CHECK(snapshot(&synced) == before && synced && observed->sync_count == 1);
	mono_us += 1000000;
	CHECK(snapshot(&synced) == before + 1020000); /* +2% slew */
	mono_us += 4000000;
	CHECK(snapshot(&synced) == before + 5100000 && observed->remaining_us == 0);

	/* A negative correction slows time, never steps it backward. */
	sample_error = -100000;
	before = snapshot(&synced);
	rtc_sync_once(observed, fake_exchange);
	CHECK(snapshot(&synced) == before);
	mono_us += 1000000;
	CHECK(snapshot(&synced) == before + 980000 && synced);
	mono_us += 4000000;
	CHECK(snapshot(&synced) == before + 4900000 && observed->remaining_us == 0);

	/* Outliers do not change the target or extend freshness. */
	int64_t last_good = observed->last_good_mono_us;
	sample_error = RTC_MAX_STEP_US + 1;
	before = snapshot(&synced);
	rtc_sync_once(observed, fake_exchange);
	CHECK(snapshot(&synced) == before && synced);
	CHECK(observed->last_good_mono_us == last_good && observed->remaining_us == 0);
	CHECK(observed->fail_count == 1);
	fail_exchange = true;
	rtc_sync_once(observed, fake_exchange);
	CHECK(observed->fail_count == 2 && observed->last_good_mono_us == last_good);

	/* Failure holdover expires trust, not output; subsequent success recovers. */
	mono_us = last_good + RTC_FRESH_US;
	before = snapshot(&synced);
	CHECK(synced);
	mono_us++;
	CHECK(snapshot(&synced) == before + 1 && !synced && observed->warning_pending);
	fail_exchange = false;
	sample_error = 0;
	before = snapshot(&synced);
	rtc_sync_once(observed, fake_exchange);
	CHECK(snapshot(&synced) == before && synced && !observed->warning_pending);
	CHECK(realtime_clock_ntp_synced(observed));

	/* A long slew also stays continuous within the four-second acceptance gate. */
	sample_error = RTC_MAX_STEP_US;
	before = snapshot(&synced);
	rtc_sync_once(observed, fake_exchange);
	CHECK(snapshot(&synced) == before);
	mono_us += 100000000;
	CHECK(snapshot(&synced) == before + 102000000 && synced);
	CHECK(observed->remaining_us == 2000000);

	/* Stale samples and a regressing monotonic clock cannot regain trust. */
	struct rtc_sample stale = {before, mono_us - RTC_MAX_RTT_US - 1, 1000};
	CHECK(!rtc_accept_locked(observed, &stale, mono_us));
	before = snapshot(&synced);
	mono_us--;
	CHECK(snapshot(&synced) == before && !synced);
	mono_us += 1000001;
	CHECK(snapshot(&synced) > before && !synced);
	rtc_mutex_lock(&observed->lock);
	observed->stop = true;
	rtc_mutex_unlock(&observed->lock);
	CHECK(rtc_should_stop(observed));
	realtime_clock_destroy(observed);
	puts("clock: anchor, slew +/- , outliers, failures, wall steps, expiry, recovery, era PASS");
	return 0;
}
