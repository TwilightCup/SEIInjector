/******************************************************************************
    SEIInjector - runtime FFmpeg binding (cross OBS/FFmpeg-version loader)
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#include "ffmpeg-bind.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#define FFB_DLOPEN_NAME "LoadLibrary"
#else
#include <dlfcn.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <wchar.h>
#endif

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static struct ffb_table g_ffb;
static bool g_loaded = false;
static bool g_load_attempted = false;
static int g_major = -1;
/* human-readable reason for the last failed ffb_load() (for plugin logging) */
static char g_err[192];

const char *ffb_error(void)
{
	return g_err[0] ? g_err : "ok";
}

const struct ffb_table *ffb_table(void)
{
	return &g_ffb;
}

bool ffb_ok(void)
{
	if (!g_loaded)
		ffb_load();
	return g_loaded;
}

int ffb_major(void)
{
	return g_major;
}

bool ffb_supported(void)
{
	if (!g_loaded)
		ffb_load();
	return g_loaded && g_major >= FFMPEG_BIND_MIN_MAJOR;
}

/* ------------------------------------------------------------------ */
/* platform symbol resolution                                          */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static void *ffb_resolve_symbol(void *h, const char *name)
{
	/* GetProcAddress returns FARPROC; cast through void* so it works on
	 * C89/C11 toolchains without pedantic warnings. */
	void *fn = NULL;
	*(void **)(&fn) = (void *)GetProcAddress((HMODULE)h, name);
	return fn;
}

/* Load one avcodec-NN.dll; returns handle or NULL. */
/* Scan the current process for a loaded avcodec-NN.dll and return its handle.
 * The plugin imports avcodec-62.dll (so it is guaranteed loaded); a name-scanned
 * lookup is more reliable than GetModuleHandleW, which can miss the module. */
static HMODULE ffb_find_loaded_avcodec(void)
{
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), NULL, 0, &needed))
		return NULL;
	size_t nmod = needed / sizeof(HMODULE);
	if (nmod == 0)
		return NULL;
	HMODULE *mods = malloc(nmod * sizeof(HMODULE));
	if (!mods)
		return NULL;
	DWORD got = 0;
	HMODULE found = NULL;
	if (EnumProcessModules(GetCurrentProcess(), mods, (DWORD)(nmod * sizeof(HMODULE)), &got)) {
		size_t nm = got / sizeof(HMODULE);
		wchar_t name[MAX_PATH];
		for (size_t i = 0; i < nm && !found; i++) {
			if (GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) {
				if (wcslen(name) > 8 && _wcsnicmp(name, L"avcodec-", 8) == 0) {
					int v = (int)wcstol(name + 8, NULL, 10);
					if (v >= FFMPEG_BIND_MIN_MAJOR && v <= 66)
						found = mods[i];
				}
			}
		}
	}
	free(mods);
	return found;
}

static HMODULE ffb_open_lib(int major)
{
	wchar_t name[48];
	swprintf(name, sizeof name / sizeof name[0], L"avcodec-%d.dll", major);

	/* Never LoadLibrary: bind only to an avcodec-NN.dll already in this
	 * process (OBS's own, or the copy our import table pulls in). This avoids
	 * every antivirus / sibling-dependency problem that manual loading hits. */
	HMODULE h = GetModuleHandleW(name);
	if (h)
		return h;
	h = ffb_find_loaded_avcodec();
	if (h)
		return h;

	snprintf(g_err, sizeof g_err, "avcodec-%d: not loaded in process yet (will retry lazily)", major);
	return NULL;
}

#else /* posix */

static void *ffb_resolve_symbol(void *h, const char *name)
{
	if (h)
		return dlsym(h, name);
	/* RTLD_DEFAULT: search every library already loaded (OBS's FFmpeg). */
	return dlsym(RTLD_DEFAULT, name);
}

static void *ffb_open_lib(int major)
{
	void *h;
#if defined(__APPLE__)
	char name[64];
	snprintf(name, sizeof name, "libavcodec.%d.dylib", major);
	h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#else
	char name[64];
	snprintf(name, sizeof name, "libavcodec.so.%d", major);
	h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif
	/* If it is already mapped (OBS), grab that mapping without loading a
	 * second copy. */
	if (!h) {
#if defined(__APPLE__)
		snprintf(name, sizeof name, "libavcodec.%d.dylib", major);
#else
		snprintf(name, sizeof name, "libavcodec.so.%d", major);
#endif
		h = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
	}
	return h;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* binding                                                             */
/* ------------------------------------------------------------------ */

#define FFB_BIND(slot, name)                                                  \
	do {                                                                  \
		void *p = ffb_resolve_symbol(h, name);                        \
		if (!p) {                                                      \
			snprintf(g_err, sizeof g_err,                          \
				 "avcodec-%d: symbol %s not found", major,   \
				 name);                                        \
			goto next_major;                                      \
		}                                                               \
		*(void **)(&g_ffb.slot) = p;                                  \
	} while (0)

bool ffb_load(void)
{
	/* Idempotent on success, but a transient failure (e.g. OBS's own FFmpeg
	 * not initialised yet) is retried on the next call. */
	if (g_loaded)
		return true;

	g_load_attempted = true;
	g_ffb = (struct ffb_table){0};
	g_major = -1;
	g_err[0] = 0;

	/* Probe a small window of majors upward from the ABI floor. The very
	 * first one with every symbol present wins. */
	for (int major = FFMPEG_BIND_MIN_MAJOR; major <= 66; major++) {
		void *h = ffb_open_lib(major);
		if (!h) {
#ifndef _WIN32
			/* Windows set g_err inside ffb_open_lib; backstop others. */
			snprintf(g_err, sizeof g_err, "open avcodec-%d failed", major);
#endif
			continue;
		}
		g_ffb = (struct ffb_table){0};
		FFB_BIND(avcodec_find_encoder_by_name, "avcodec_find_encoder_by_name");
		FFB_BIND(avcodec_alloc_context3, "avcodec_alloc_context3");
		FFB_BIND(avcodec_open2, "avcodec_open2");
		FFB_BIND(avcodec_free_context, "avcodec_free_context");
		FFB_BIND(avcodec_send_frame, "avcodec_send_frame");
		FFB_BIND(avcodec_receive_packet, "avcodec_receive_packet");
		FFB_BIND(av_frame_alloc, "av_frame_alloc");
		FFB_BIND(av_frame_free, "av_frame_free");
		FFB_BIND(av_frame_unref, "av_frame_unref");
		FFB_BIND(av_packet_alloc, "av_packet_alloc");
		FFB_BIND(av_packet_free, "av_packet_free");
		FFB_BIND(av_packet_unref, "av_packet_unref");
		FFB_BIND(av_dict_set, "av_dict_set");
		FFB_BIND(av_dict_free, "av_dict_free");
		FFB_BIND(av_strerror, "av_strerror");
		FFB_BIND(avcodec_version, "avcodec_version");

		unsigned ver = g_ffb.avcodec_version();
		g_major = (int)((ver >> 16) & 0xFFu);
		if (g_major >= FFMPEG_BIND_MIN_MAJOR) {
			snprintf(g_err, sizeof g_err, "loaded avcodec-%d", g_major);
			g_loaded = true;
			return true;
		}
		/* bound a very old FFmpeg by accident: keep probing */
	next_major:
		g_ffb = (struct ffb_table){0};
#ifndef _WIN32
		if (h && h != RTLD_DEFAULT)
			dlclose(h);
#endif
		continue;
	}

	return false;
}
