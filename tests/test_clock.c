/* Exercise the actual worker's sample/publish function with offline I/O. */
#include "../src/realtime-clock.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static realtime_clock_t *observed;
static bool fake_exchange(const char *server, uint16_t port, int64_t *offset, int64_t *rtt)
{
	(void)server; (void)port;
	/* A blocked network callback must not own the clock state lock. */
	CHECK(pthread_mutex_trylock(&observed->lock) == 0);
	pthread_mutex_unlock(&observed->lock);
	bool synced;
	(void)realtime_clock_snapshot(observed, &synced);
	CHECK(!synced);
	*offset = 5000; *rtt = 1000;
	return true;
}
static bool failed_exchange(const char *s, uint16_t p, int64_t *o, int64_t *r)
{
	(void)s; (void)p; (void)o; (void)r;
	CHECK(pthread_mutex_trylock(&observed->lock) == 0);
	pthread_mutex_unlock(&observed->lock);
	return false;
}
int main(void)
{
	int64_t off, rtt;
	/* 20 ms each direction, 5 ms server work, remote clock +100 ms. */
	CHECK(rtc_measurement(1000000, 1120000, 1125000, 1045000, &off, &rtt));
	CHECK(off == 100000 && rtt == 40000);
	CHECK(!rtc_measurement(1000000, 1120000, 1125000, 1000000, &off, &rtt));
	CHECK(!rtc_measurement(10, 20, 19, 30, &off, &rtt));
	struct realtime_clock c = {0};
	rtc_mutex_init(&c.lock);
	strcpy(c.server, "offline-test");
	observed = &c;
	rtc_sync_once(&c, fake_exchange);
	CHECK(c.offset_us == 5000 && c.ntp_synced && c.sync_count == 1);
	rtc_sync_once(&c, failed_exchange);
	CHECK(c.offset_us == 5000 && c.ntp_synced && c.fail_count == 1);
	rtc_mutex_lock(&c.lock); c.stop = true; rtc_mutex_unlock(&c.lock);
	CHECK(rtc_should_stop(&c));
	rtc_mutex_destroy(&c.lock);
	puts("clock: four timestamps, network outside mutex, snapshot and failed-refresh retention PASS");
	return 0;
}
