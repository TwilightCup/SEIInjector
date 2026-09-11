/******************************************************************************
    SEIInjector - wall-clock + optional SNTP correction
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

/* glibc hides getaddrinfo/struct addrinfo behind a feature-test macro; Darwin
 * exposes them unconditionally, so this must be set explicitly on Linux. Must
 * appear before any system header is included. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "realtime-clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* portability shims                                                   */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
typedef CRITICAL_SECTION rtc_mutex_t;
#define rtc_mutex_init(m) InitializeCriticalSection(m)
#define rtc_mutex_lock(m) EnterCriticalSection(m)
#define rtc_mutex_unlock(m) LeaveCriticalSection(m)
#define rtc_mutex_destroy(m) DeleteCriticalSection(m)
typedef HANDLE rtc_thread_t;
typedef DWORD(WINAPI *rtc_thread_fn)(LPVOID);
#else
typedef pthread_mutex_t rtc_mutex_t;
#define rtc_mutex_init(m) pthread_mutex_init(m, NULL)
#define rtc_mutex_lock(m) pthread_mutex_lock(m)
#define rtc_mutex_unlock(m) pthread_mutex_unlock(m)
#define rtc_mutex_destroy(m) pthread_mutex_destroy(m)
typedef pthread_t rtc_thread_t;
typedef void *(*rtc_thread_fn)(void *);
#endif

static bool rtc_thread_start(rtc_thread_t *t, rtc_thread_fn fn, void *arg)
{
#ifdef _WIN32
	*t = CreateThread(NULL, 0, fn, arg, 0, NULL);
	return *t != NULL;
#else
	return pthread_create(t, NULL, fn, arg) == 0;
#endif
}

static void rtc_thread_join(rtc_thread_t t)
{
#ifdef _WIN32
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
#else
	pthread_join(t, NULL);
#endif
}

#ifdef _WIN32
static void rtc_sleep_ms(uint32_t ms)
{
	Sleep(ms);
}
#else
static void rtc_sleep_ms(uint32_t ms)
{
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
#endif

/* epoch microseconds of the local wall clock */
int64_t realtime_clock_local_now_us(void)
{
#ifdef _WIN32
	FILETIME ft;
	ULARGE_INTEGER li;
	GetSystemTimeAsFileTime(&ft);
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;
	/* 100 ns ticks since 1601-01-01 -> epoch offset 11644473600 s */
	return (int64_t)(li.QuadPart / 10) - 11644473600000000LL;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
#endif
}

/* monotonic microseconds (used only for the resync scheduler) */
static int64_t rtc_mono_us(void)
{
#if defined(_WIN32)
	LARGE_INTEGER freq, cnt;
	if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&cnt))
		return (int64_t)((cnt.QuadPart * 1000000LL) / freq.QuadPart);
	return realtime_clock_local_now_us();
#elif defined(CLOCK_MONOTONIC)
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
	return realtime_clock_local_now_us();
#else
	return realtime_clock_local_now_us();
#endif
}

/* ------------------------------------------------------------------ */
/* NTP wire protocol                                                   */
/* ------------------------------------------------------------------ */

#define NTP_EPOCH_OFFSET 2208988800ULL /* seconds 1900 -> 1970 */
#define NTP_PACKET_SIZE 48

typedef struct ntp_ts {
	uint32_t seconds;  /* since 1900 */
	uint32_t fraction; /* 2^-32 s */
} ntp_ts_t;

#ifdef _WIN32
static bool rtc_winsock_started;
static void rtc_ensure_winsock(void)
{
	if (!rtc_winsock_started) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
			rtc_winsock_started = true;
	}
}
#endif

static int64_t ntp_to_epoch_us(const ntp_ts_t *t)
{
	uint64_t s = t->seconds;
	if (s >= NTP_EPOCH_OFFSET)
		s -= NTP_EPOCH_OFFSET;
	return (int64_t)(s * 1000000ULL + (((uint64_t)t->fraction * 1000000ULL) >> 32));
}

/*
 * One blocking SNTP round trip. Returns true on success; *offset_us receives
 * (serverTime - localTime) and *rtt_us the round-trip time.
 */
static bool rtc_sntp_exchange(const char *server, uint16_t port, int64_t *offset_us, int64_t *rtt_us)
{
#ifdef _WIN32
	rtc_ensure_winsock();
#endif

	int sock = -1;
	struct addrinfo *ai = NULL;
	bool ok = false;
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if (getaddrinfo(server, port_str, &hints, &ai) != 0 || !ai)
		goto done;
	sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock < 0)
		goto done;

	/* short timeouts so a dead server can never stall the caller */
#ifdef _WIN32
	DWORD tv = 1200;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 200000};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	uint8_t pkt[NTP_PACKET_SIZE];
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = (3 << 3) | 3; /* NTP v3 client */

	/* populate transmit timestamp with our (approximate) epoch */
	int64_t local_now = realtime_clock_local_now_us();
	uint64_t ntp_sec = (uint64_t)(local_now / 1000000LL) + NTP_EPOCH_OFFSET;
	uint64_t frac_us = (uint64_t)(local_now % 1000000LL);
	pkt[40] = (uint8_t)(ntp_sec >> 24);
	pkt[41] = (uint8_t)(ntp_sec >> 16);
	pkt[42] = (uint8_t)(ntp_sec >> 8);
	pkt[43] = (uint8_t)ntp_sec;
	uint32_t ntp_frac = (uint32_t)((frac_us << 32) / 1000000ULL);
	pkt[44] = (uint8_t)(ntp_frac >> 24);
	pkt[45] = (uint8_t)(ntp_frac >> 16);
	pkt[46] = (uint8_t)(ntp_frac >> 8);
	pkt[47] = (uint8_t)ntp_frac;

	if (sendto(sock, (const char *)pkt, sizeof(pkt), 0, ai->ai_addr, (socklen_t)ai->ai_addrlen) < 0)
		goto done;

	uint8_t rsp[NTP_PACKET_SIZE];
	int64_t t4 = realtime_clock_local_now_us();
	/* recvfrom returns ssize_t on POSIX but int on Windows; `int` covers both
	 * and the 48-byte NTP read can never reach the negative range anyway. */
	int got = (int)recvfrom(sock, (char *)rsp, sizeof(rsp), 0, NULL, NULL);
	if (got < (int)NTP_PACKET_SIZE)
		goto done;

	ntp_ts_t t2 = {
		.seconds = ((uint32_t)rsp[32] << 24) | ((uint32_t)rsp[33] << 16) | ((uint32_t)rsp[34] << 8) | rsp[35],
		.fraction = ((uint32_t)rsp[36] << 24) | ((uint32_t)rsp[37] << 16) | ((uint32_t)rsp[38] << 8) | rsp[39]};
	ntp_ts_t t3 = {
		.seconds = ((uint32_t)rsp[40] << 24) | ((uint32_t)rsp[41] << 16) | ((uint32_t)rsp[42] << 8) | rsp[43],
		.fraction = ((uint32_t)rsp[44] << 24) | ((uint32_t)rsp[45] << 16) | ((uint32_t)rsp[46] << 8) | rsp[47]};

	int64_t t1 = local_now;
	int64_t t2_us = ntp_to_epoch_us(&t2);
	int64_t t3_us = ntp_to_epoch_us(&t3);
	/* offset = ((t2 - t1) + (t3 - t4)) / 2 */
	*offset_us = ((t2_us - t1) + (t3_us - t4)) / 2;
	*rtt_us = (t4 - t1) - (t3_us - t2_us);
	ok = true;

done:
	if (ai)
		freeaddrinfo(ai);
	if (sock >= 0) {
#ifdef _WIN32
		closesocket(sock);
#else
		close(sock);
#endif
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* clock object                                                        */
/* ------------------------------------------------------------------ */

struct realtime_clock {
	char server[256];
	uint16_t port;
	uint32_t interval_us;

	rtc_mutex_t lock;
	int64_t offset_us; /* server - local */
	bool ntp_synced;
	uint32_t sync_count;
	uint32_t fail_count;

	volatile bool stop;
	rtc_thread_t thread;
	bool thread_started;
};

static void rtc_sync_once_locked(realtime_clock_t *c)
{
	/* take up to three samples and keep the lowest round-trip time */
	int64_t best_offset = 0;
	int64_t best_rtt = INT64_MAX;
	bool any = false;

	for (int i = 0; i < 3; i++) {
		int64_t off = 0, rtt = 0;
		if (rtc_sntp_exchange(c->server, c->port, &off, &rtt)) {
			any = true;
			if (rtt < best_rtt) {
				best_rtt = rtt;
				best_offset = off;
			}
			if (rtt < 20000) /* 20 ms: good enough, stop early */
				break;
		}
	}

	if (any) {
		c->offset_us = best_offset;
		c->ntp_synced = true;
		c->sync_count++;
	} else {
		c->fail_count++;
		/* keep the previous offset (may be 0 = local time) */
	}
}

#ifdef _WIN32
static DWORD WINAPI rtc_worker(LPVOID arg)
#else
static void *rtc_worker(void *arg)
#endif
{
	realtime_clock_t *c = arg;
	int64_t next = rtc_mono_us(); /* first sync happens right away */

	while (!c->stop) {
		rtc_sleep_ms(200);
		int64_t now = rtc_mono_us();
		if (now < next)
			continue;
		next = now + c->interval_us;

		rtc_mutex_lock(&c->lock);
		rtc_sync_once_locked(c);
		rtc_mutex_unlock(&c->lock);
	}

#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

realtime_clock_t *realtime_clock_create(const char *server, uint16_t port, uint32_t interval_ms)
{
	realtime_clock_t *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	if (server)
		snprintf(c->server, sizeof(c->server), "%s", server);
	c->port = port ? port : 123;
	uint32_t iv = interval_ms ? interval_ms : 60000;
	if (iv < 1000)
		iv = 1000;
	c->interval_us = iv * 1000u;

	rtc_mutex_init(&c->lock);
	c->stop = false;

	if (c->server[0]) {
		/* Non-blocking create: the worker performs the first sync about
		 * 200 ms after encoder start and every interval_us afterwards,
		 * so a dead NTP server can never stall "Start Streaming". */
		if (!rtc_thread_start(&c->thread, rtc_worker, c)) {
			/* worker thread failed; keep local-time mode */
		} else {
			c->thread_started = true;
		}
	}

	return c;
}

void realtime_clock_destroy(realtime_clock_t *c)
{
	if (!c)
		return;

	c->stop = true;
	if (c->thread_started)
		rtc_thread_join(c->thread);

	rtc_mutex_destroy(&c->lock);
	free(c);
}

int64_t realtime_clock_now_us(realtime_clock_t *c)
{
	int64_t local = realtime_clock_local_now_us();
	if (!c || !c->server[0])
		return local;

	int64_t offset;
	rtc_mutex_lock(&c->lock);
	offset = c->offset_us;
	rtc_mutex_unlock(&c->lock);
	return local + offset;
}

bool realtime_clock_ntp_synced(realtime_clock_t *c)
{
	if (!c || !c->server[0])
		return false;
	bool synced;
	rtc_mutex_lock(&c->lock);
	synced = c->ntp_synced;
	rtc_mutex_unlock(&c->lock);
	return synced;
}

int64_t realtime_clock_offset_us(realtime_clock_t *c)
{
	if (!c)
		return 0;
	int64_t o;
	rtc_mutex_lock(&c->lock);
	o = c->offset_us;
	rtc_mutex_unlock(&c->lock);
	return o;
}

uint32_t realtime_clock_sync_count(realtime_clock_t *c)
{
	if (!c)
		return 0;
	uint32_t n;
	rtc_mutex_lock(&c->lock);
	n = c->sync_count;
	rtc_mutex_unlock(&c->lock);
	return n;
}

uint32_t realtime_clock_fail_count(realtime_clock_t *c)
{
	if (!c)
		return 0;
	uint32_t n;
	rtc_mutex_lock(&c->lock);
	n = c->fail_count;
	rtc_mutex_unlock(&c->lock);
	return n;
}
