/******************************************************************************
    SEIInjector - runtime FFmpeg binding (cross OBS/FFmpeg-version loader)
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    OBS bundles its own FFmpeg and the installed plugin distributes *without*
    linking libavcodec/libavutil. This module resolves the small set of FFmpeg
    symbols we need at runtime, from whatever FFmpeg is already loaded into the
    OBS process (or by probing common library names). That lets one binary ride
    multiple OBS versions instead of being tied to a single avcodec-XX.so/dll
    via its import table.

    The encoder still #includes libavcodec headers so it can read/write the
    (long ABI-stable) AVFrame / AVPacket public fields and the small leading
    set of AVCodecContext configuration fields; those layouts are compiled
    against the headers we build with, so loaders must reject a runtime
    FFmpeg whose *major* is outside the supported window (ffb_supported()).
******************************************************************************/

#ifndef FFMPEG_BIND_H
#define FFMPEG_BIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to FFmpeg types; only function pointers below are used
 * through this module, plus AVFrame/AVPacket/AVCodecContext fields which the
 * encoder accesses by #including the headers directly. */
typedef struct AVCodec AVCodec;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;
typedef struct AVDictionary AVDictionary;

/*
 * Lowest FFmpeg libavcodec *major* this binary will run against. Raised by the
 * build the headers match; code compiled against newer AVCodecContext layouts
 * must not be handed to an older libavcodec. ABI-safe ground floor we validate
 * in CI (OBS 30+ carries avcodec-major >= 60):
 *   avcodec-59 = FFmpeg 5 (OBS 28/29), 60..62 = FFmpeg 6..8 (OBS 30..32).
 * To also cover 59, drop this to 59 AND lock the layout via a CI matrix that
 * actually exercises it -- do not lower blindly.
 */
#define FFMPEG_BIND_MIN_MAJOR 60

/* The AVOptions-free dimension/config we set through plain struct fields: the
 * two signalled as an int pair for time base / frame rate. */
typedef struct ffb_rational {
	int num;
	int den;
} ffb_rational;

struct ffb_table {
	const AVCodec *(*avcodec_find_encoder_by_name)(const char *name);
	AVCodecContext *(*avcodec_alloc_context3)(const AVCodec *codec);
	int (*avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
	void (*avcodec_free_context)(AVCodecContext **avctx);
	int (*avcodec_send_frame)(AVCodecContext *avctx, const AVFrame *frame);
	int (*avcodec_receive_packet)(AVCodecContext *avctx, AVPacket *avpkt);

	AVFrame *(*av_frame_alloc)(void);
	void (*av_frame_free)(AVFrame **frame);
	void (*av_frame_unref)(AVFrame *frame);

	AVPacket *(*av_packet_alloc)(void);
	void (*av_packet_free)(AVPacket **pkt);
	void (*av_packet_unref)(AVPacket *pkt);

	int (*av_dict_set)(AVDictionary **pm, const char *key, const char *value, int flags);
	void (*av_dict_free)(AVDictionary **m);

	int (*av_strerror)(int errnum, char *errbuf, size_t errbuf_size);
	unsigned (*avcodec_version)(void);
};

/*
 * Tries to (re)load FFmpeg and bind every symbol in the table. Safe to call
 * more than once (no-op if already loaded). Returns true on success.
 * On failure the table stays zeroed and a reason string is logged.
 */
bool ffb_load(void);

/* Accessors. ffb_table() is only valid after a successful ffb_load(). */
bool ffb_ok(void);
int ffb_major(void); /* libavcodec major, or -1 */
const struct ffb_table *ffb_table(void);
bool ffb_supported(void);    /* ok() && major >= MIN */
const char *ffb_error(void); /* last load failure reason, or "ok" */

#ifdef __cplusplus
}
#endif

#endif /* FFMPEG_BIND_H */
