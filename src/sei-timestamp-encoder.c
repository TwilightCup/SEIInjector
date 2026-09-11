/******************************************************************************
    SEIInjector - Timestamp-injecting OBS video encoder
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    Registers two custom OBS video encoders ("SEI Timestamp (H.264)" and
    "SEI Timestamp (HEVC/H.265)"). Each wraps an FFmpeg encoder that already
    exists inside the OBS process (libx264 / h264_nvenc / hevc_amf / ...) and
    rewrites every encoded access unit to carry one per-frame SEI message with
    the frame's real-time (NTP-corrected) stamp.

    Approach follows the same OBS plugin pattern as the SEI-Stamper project:
      * legacy obs_encoder_info.encode() path (no PASS_TEXTURE),
      * get_video_info() advertises NV12,
      * AV_CODEC_FLAG_GLOBAL_HEADER gives OBS containers their avcC/hvcC
        sequence header, while the parameter sets are additionally re-inserted
        inline on keyframes (as Annex-B) so MPEG-TS/SRT receivers that join
        mid-stream can still decode.
******************************************************************************/

#include "sei-timestamp-encoder.h"

#include "h26x-util.h"
#include "realtime-clock.h"
#include "sei-payload.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>

#include <util/bmem.h>
#include <util/platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define encoder_log(level, enc, format, ...)                                   \
	blog(level, "[SEI Timestamp: '%s'] " format,                           \
	     obs_encoder_get_name((enc)->context), ##__VA_ARGS__)

/*
 * Hard-crash-safe tracer: writes to %TEMP%/sei_trace.txt with an explicit
 * flush each call, so the last line survives a hard crash where OBS's blog
 * FILE buffer does not.
 */
static void sei_trace(struct stamp_encoder *enc, const char *fmt, ...)
{
	(void)enc;
	static unsigned n;
	if (n++ >= 100)
		return;
	const char *dir = getenv("TEMP");
	if (!dir)
		dir = "/tmp";
	char path[512];
	snprintf(path, sizeof path, "%s/sei_trace.txt", dir);
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
	fclose(f);
}

/* obs_data_get_string never returns NULL in practice once defaults exist, but
 * old profiles may lack keys -- guard anyway. */
static const char *cfg_str(obs_data_t *d, const char *key)
{
	const char *s = obs_data_get_string(d, key);
	return s ? s : "";
}

/* ------------------------------------------------------------------ */
/* settings keys                                                       */
/* ------------------------------------------------------------------ */

#define SET_CODEC_NAME "codec_name"
#define SET_BITRATE "bitrate"
#define SET_KEYINT "keyint_sec"
#define SET_BFRAMES "bframes"
#define SET_PRESET "preset"
#define SET_PROFILE "profile"
#define SET_TUNE "tune"
#define SET_RC "rate_control"
#define SET_NTP_ENABLED "ntp_enabled"
#define SET_NTP_SERVER "ntp_server"
#define SET_NTP_PORT "ntp_port"
#define SET_NTP_INTERVAL "ntp_interval_ms"

#define DEFAULT_NTP_SERVER "pool.ntp.org"
#define DEFAULT_NTP_PORT 123
#define DEFAULT_NTP_INTERVAL_MS 60000

/* ------------------------------------------------------------------ */
/* per-frame submit-time ring (pts -> epoch us)                        */
/* ------------------------------------------------------------------ */

#define STAMP_PTS_RING_CAP 4096u

struct pts_time_entry {
	int64_t pts;
	int64_t epoch_us;
};

struct stamp_encoder {
	obs_encoder_t *context;
	enum stamp_codec codec;

	/* settings */
	char codec_name[64];
	int bitrate;    /* kbps */
	int keyint_sec; /* seconds */
	int bframes;
	char preset[32];
	char profile[32];
	char tune[32];
	bool rate_control_cbr;

	/* OBS video info */
	uint32_t width, height;
	uint32_t fps_num, fps_den;

	/* ffmpeg encoder */
	const AVCodec *av_codec;
	AVCodecContext *codec_context;
	AVFrame *frame;
	AVPacket *packet;

	/* sequence header / inline parameter sets */
	uint8_t *extra_data;
	size_t extra_data_size;
	uint8_t *inline_headers; /* Annex-B param sets */
	size_t inline_headers_size;

	/* scratch output buffer (grows to fit the largest packet) */
	uint8_t *out_buffer;
	size_t out_buffer_size;

	/* realtime clock (epoch + NTP offset) */
	realtime_clock_t *clock;
	bool stamping_ntp; /* stamps carry NTP-corrected time */

	/* coded frame counter put into SEI */
	uint64_t frame_seq;

	/* pts -> submit time ring */
	struct pts_time_entry *pts_ring;
	size_t pts_ring_len;
	size_t pts_ring_head;

	uint64_t log_counter;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *codec_display_codec(enum stamp_codec codec)
{
	return codec == STAMP_CODEC_H264 ? "h264" : "hevc";
}

static void put_entry(struct stamp_encoder *enc, int64_t pts, int64_t epoch_us)
{
	if (enc->pts_ring_len >= STAMP_PTS_RING_CAP) {
		/* drop the oldest; its packet (if still pending) will fall
		 * back to "now" which is fine for a pathological delay */
		enc->pts_ring_head = (enc->pts_ring_head + 1) % STAMP_PTS_RING_CAP;
		enc->pts_ring_len--;
	}
	size_t slot = (enc->pts_ring_head + enc->pts_ring_len) % STAMP_PTS_RING_CAP;
	enc->pts_ring[slot].pts = pts;
	enc->pts_ring[slot].epoch_us = epoch_us;
	enc->pts_ring_len++;
}

/* returns the epoch_us recorded for pts, or -1 */
static int64_t take_entry(struct stamp_encoder *enc, int64_t pts)
{
	for (size_t i = 0; i < enc->pts_ring_len; i++) {
		size_t idx = (enc->pts_ring_head + i) % STAMP_PTS_RING_CAP;
		if (enc->pts_ring[idx].pts == pts) {
			int64_t us = enc->pts_ring[idx].epoch_us;
			/* remove entry (shift everything after it) */
			for (size_t j = i; j + 1 < enc->pts_ring_len; j++) {
				size_t a = (enc->pts_ring_head + j) % STAMP_PTS_RING_CAP;
				size_t b = (enc->pts_ring_head + j + 1) % STAMP_PTS_RING_CAP;
				enc->pts_ring[a] = enc->pts_ring[b];
			}
			enc->pts_ring_len--;
			return us;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* FFmpeg codec discovery & option mapping                             */
/* ------------------------------------------------------------------ */

struct codec_candidate {
	const char *name;
	const char *label;
};

static const struct codec_candidate h264_candidates[] = {
	{"libx264", "Software (libx264)"},
	{"h264_nvenc", "NVIDIA NVENC"},
	{"h264_amf", "AMD AMF"},
	{"h264_qsv", "Intel Quick Sync"},
	{"h264_videotoolbox", "Apple VideoToolbox"},
	{"h264_mf", "Windows Media Foundation"},
};

static const struct codec_candidate hevc_candidates[] = {
	{"hevc_nvenc", "NVIDIA NVENC"},    {"hevc_amf", "AMD AMF"},
	{"hevc_qsv", "Intel Quick Sync"},  {"hevc_videotoolbox", "Apple VideoToolbox"},
	{"libx265", "Software (libx265)"},
};

#define N_H264_CANDIDATES                                                     \
	(sizeof(h264_candidates) / sizeof(h264_candidates[0]))
#define N_HEVC_CANDIDATES                                                     \
	(sizeof(hevc_candidates) / sizeof(hevc_candidates[0]))

/* first available ffmpeg encoder for the codec */
static const char *pick_default_codec_name(enum stamp_codec codec)
{
	if (codec == STAMP_CODEC_H264) {
		for (size_t i = 0; i < N_H264_CANDIDATES; i++)
			if (avcodec_find_encoder_by_name(h264_candidates[i].name))
				return h264_candidates[i].name;
	} else {
		for (size_t i = 0; i < N_HEVC_CANDIDATES; i++)
			if (avcodec_find_encoder_by_name(hevc_candidates[i].name))
				return hevc_candidates[i].name;
	}
	return NULL;
}

static bool is_name(const char *haystack, const char *needle)
{
	return strstr(haystack, needle) != NULL;
}

/*
 * Maps the UI "speed" preset (veryfast/fast/medium/slow/veryslow) onto the
 * preset vocabulary of the underlying FFmpeg encoder. Returns NULL when the
 * encoder takes no preset knob.
 */
static const char *map_preset(const char *codec_name, const char *preset)
{
	if (!preset || !*preset)
		return NULL;

	const char *p = preset;

	if (is_name(codec_name, "nvenc")) {
		/* modern NVENC SDK presets are P1..P7 */
		if (strcmp(p, "ultrafast") == 0 || strcmp(p, "veryfast") == 0)
			return "p1";
		if (strcmp(p, "fast") == 0)
			return "p2";
		if (strcmp(p, "medium") == 0)
			return "p4";
		if (strcmp(p, "slow") == 0)
			return "p6";
		if (strcmp(p, "veryslow") == 0)
			return "p7";
		return NULL;
	}

	if (is_name(codec_name, "amf")) {
		if (strstr(p, "fast") || strcmp(p, "ultrafast") == 0)
			return "speed";
		if (strcmp(p, "slow") == 0 || strcmp(p, "veryslow") == 0)
			return "quality";
		return "balanced";
	}

	/* libx264 / qsv / others accept the conventional names directly */
	if (strcmp(p, "veryfast") == 0 || strcmp(p, "faster") == 0 || strcmp(p, "fast") == 0 ||
	    strcmp(p, "medium") == 0 || strcmp(p, "slow") == 0 || strcmp(p, "slower") == 0 ||
	    strcmp(p, "veryslow") == 0 || strcmp(p, "ultrafast") == 0 || strcmp(p, "superfast") == 0 ||
	    strcmp(p, "placebo") == 0)
		return p;

	/* videotoolbox / mf have no preset option */
	return NULL;
}

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

static void stamp_encoder_destroy_data(void *data)
{
	struct stamp_encoder *enc = data;
	if (!enc)
		return;

	encoder_log(LOG_INFO, enc, "Destroying encoder (seq=%llu)", (unsigned long long)enc->frame_seq);

	if (enc->codec_context)
		avcodec_free_context(&enc->codec_context);
	if (enc->frame)
		av_frame_free(&enc->frame);
	if (enc->packet)
		av_packet_free(&enc->packet);

	bfree(enc->extra_data);
	/* h26x_extradata_to_annexb() returns a C `malloc`'d buffer -> free(), not
	 * OBS's bfree() (mismatched allocator corrupts the heap). */
	free(enc->inline_headers);
	bfree(enc->out_buffer);
	bfree(enc->pts_ring);

	if (enc->clock)
		realtime_clock_destroy(enc->clock);

	bfree(enc);
}

static void *stamp_encoder_create(obs_data_t *settings, obs_encoder_t *encoder, enum stamp_codec codec)
{
	struct stamp_encoder *enc = bzalloc(sizeof(*enc));
	enc->context = encoder;
	enc->codec = codec;

	const char *codec_name = cfg_str(settings, SET_CODEC_NAME);
	if (!codec_name || !*codec_name)
		codec_name = pick_default_codec_name(codec);
	if (!codec_name) {
		blog(LOG_ERROR,
		     "[SEI Timestamp] No matching %s FFmpeg encoder found in "
		     "this OBS build; the plugin cannot start.",
		     codec_display_codec(codec));
		stamp_encoder_destroy_data(enc);
		return NULL;
	}
	snprintf(enc->codec_name, sizeof(enc->codec_name), "%s", codec_name);

	enc->bitrate = (int)obs_data_get_int(settings, SET_BITRATE);
	enc->keyint_sec = (int)obs_data_get_int(settings, SET_KEYINT);
	enc->bframes = (int)obs_data_get_int(settings, SET_BFRAMES);
	snprintf(enc->preset, sizeof(enc->preset), "%s", cfg_str(settings, SET_PRESET));
	snprintf(enc->profile, sizeof(enc->profile), "%s", cfg_str(settings, SET_PROFILE));
	snprintf(enc->tune, sizeof(enc->tune), "%s", cfg_str(settings, SET_TUNE));
	const char *rc = cfg_str(settings, SET_RC);
	enc->rate_control_cbr = rc && strstr(rc, "CBR");

	video_t *video = obs_encoder_video(encoder);
	const struct video_output_info *voi = video_output_get_info(video);
	enc->width = (uint32_t)obs_encoder_get_width(encoder);
	enc->height = (uint32_t)obs_encoder_get_height(encoder);
	if (!enc->width || !enc->height) {
		enc->width = voi->width;
		enc->height = voi->height;
	}
	enc->fps_num = voi->fps_num;
	enc->fps_den = voi->fps_den;

	encoder_log(LOG_INFO, enc,
		    "Creating %s encoder wrapping '%s' "
		    "(%ux%u @ %u.%u fps, %d kbps)",
		    codec_display_codec(codec), enc->codec_name, enc->width, enc->height, enc->fps_num, enc->fps_den,
		    enc->bitrate);

	enc->av_codec = avcodec_find_encoder_by_name(enc->codec_name);
	if (!enc->av_codec) {
		encoder_log(LOG_ERROR, enc,
			    "FFmpeg encoder '%s' not found "
			    "(is FFmpeg in this OBS build compiled with it?)",
			    enc->codec_name);
		stamp_encoder_destroy_data(enc);
		return NULL;
	}

	enc->codec_context = avcodec_alloc_context3(enc->av_codec);
	if (!enc->codec_context) {
		encoder_log(LOG_ERROR, enc, "avcodec_alloc_context3 failed");
		stamp_encoder_destroy_data(enc);
		return NULL;
	}

	enc->codec_context->width = (int)enc->width;
	enc->codec_context->height = (int)enc->height;
	enc->codec_context->pix_fmt = AV_PIX_FMT_NV12;
	enc->codec_context->time_base = (AVRational){(int)enc->fps_den, (int)enc->fps_num};
	enc->codec_context->framerate = (AVRational){(int)enc->fps_num, (int)enc->fps_den};
	enc->codec_context->bit_rate = enc->bitrate * 1000LL;
	enc->codec_context->gop_size =
		(int)(((int64_t)enc->keyint_sec * enc->fps_num + enc->fps_den - 1) / enc->fps_den);
	if (enc->bframes > 0)
		enc->codec_context->max_b_frames = enc->bframes;

	/* CBR rate control plumbing (generic AVCodecContext level) */
	if (enc->rate_control_cbr) {
		/* rc_*_rate/rc_buffer_size are int, bit_rate is int64_t: cast
		 * explicitly to silence MSVC C4244 (treated as error on CI). */
		enc->codec_context->rc_min_rate = (int)enc->codec_context->bit_rate;
		enc->codec_context->rc_max_rate = (int)enc->codec_context->bit_rate;
		enc->codec_context->rc_buffer_size = (int)enc->codec_context->bit_rate;
	}

	/*
	 * Global header: OBS containers (FLV/RTMP, mp4) need an
	 * AVCDecoderConfigurationRecord / HEVCDecoderConfigurationRecord at
	 * stream start, which FFmpeg only produces in extradata when this flag
	 * is set. Parameter sets are then re-inserted inline into keyframes by
	 * us (Annex-B) so TS/SRT receivers can join mid-stream.
	 */
	enc->codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	/* Pass no option dictionary to avcodec_open2 (matches how StreamFX opens
	 * FFmpeg): per-codec preset/tune/profile often trip "Invalid argument" on
	 * some builds (e.g. libx264 on OBS.app), and x264's 'medium' is the default
	 * anyway. Configure through AVCodecContext fields only. */
	const char *preset = map_preset(enc->codec_name, enc->preset);
	if (preset)
		encoder_log(LOG_INFO, enc, "preset '%s' (NOTE: applied by codec default)", preset);
	else if (enc->preset[0])
		encoder_log(LOG_INFO, enc, "encoder takes no preset option ('%s' ignored)", enc->preset);

	char errbuf[128];
	int ret = avcodec_open2(enc->codec_context, enc->av_codec, NULL);

	if (ret < 0) {
		av_strerror(ret, errbuf, sizeof(errbuf));
		encoder_log(LOG_ERROR, enc, "avcodec_open2 failed: %s (%d)", errbuf, ret);
		stamp_encoder_destroy_data(enc);
		return NULL;
	}

	enc->frame = av_frame_alloc();
	enc->packet = av_packet_alloc();
	if (!enc->frame || !enc->packet) {
		encoder_log(LOG_ERROR, enc, "frame/packet allocation failed");
		stamp_encoder_destroy_data(enc);
		return NULL;
	}

	/* keep sequence header for obs (avcC/hvcC or raw annexb) */
	if (enc->codec_context->extradata_size > 0) {
		enc->extra_data_size = (size_t)enc->codec_context->extradata_size;
		enc->extra_data = bmemdup(enc->codec_context->extradata, enc->extra_data_size);
	} else {
		encoder_log(LOG_WARNING, enc,
			    "encoder produced no extradata; FLV/mp4 outputs "
			    "may not start");
	}

	/* Annex-B parameter sets for inline keyframe injection */
	enc->inline_headers = h26x_extradata_to_annexb(codec == STAMP_CODEC_H264 ? H26X_H264 : H26X_HEVC,
						       enc->extra_data, enc->extra_data_size,
						       &enc->inline_headers_size);
	if (enc->inline_headers && enc->inline_headers_size > 0) {
		encoder_log(LOG_INFO, enc, "inline parameter sets: %zu bytes (Annex-B)", enc->inline_headers_size);
	} else {
		encoder_log(LOG_WARNING, enc,
			    "could not build inline parameter sets from "
			    "extradata; mid-stream TS/SRT joiners may not "
			    "decode until the next sequence header");
	}

	/* realtime clock + NTP */
	bool ntp_enabled = obs_data_get_bool(settings, SET_NTP_ENABLED);
	const char *ntp_server = cfg_str(settings, SET_NTP_SERVER);
	if (ntp_enabled && ntp_server && *ntp_server) {
		uint16_t port = (uint16_t)obs_data_get_int(settings, SET_NTP_PORT);
		uint32_t interval = (uint32_t)obs_data_get_int(settings, SET_NTP_INTERVAL);
		enc->clock = realtime_clock_create(ntp_server, port, interval);
		if (!enc->clock)
			encoder_log(LOG_WARNING, enc,
				    "could not start NTP clock; using local "
				    "wall clock");
		else
			enc->stamping_ntp = true;
	}

	enc->pts_ring = bmalloc(STAMP_PTS_RING_CAP * sizeof(struct pts_time_entry));

	/* Pre-size the output packet buffer with headroom so its address stays
	 * stable for OBS (which may hold packet->data while muxing). */
	enc->out_buffer_size = 1u << 22; /* 4 MB */
	enc->out_buffer = bmalloc(enc->out_buffer_size);

	encoder_log(LOG_INFO, enc, "encoder ready (%s)", enc->stamping_ntp ? "NTP clock" : "local clock");

	return enc;
}

static void *h264_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return stamp_encoder_create(settings, encoder, STAMP_CODEC_H264);
}

static void *hevc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return stamp_encoder_create(settings, encoder, STAMP_CODEC_HEVC);
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

static bool stamp_encoder_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
				 bool *received_packet)
{
	struct stamp_encoder *enc = data;
	char errbuf[128];

	if (!frame || !packet || !received_packet)
		return false;
	*received_packet = false;

	/* verbose per-step tracing for the first frames, to pinpoint a crash to a
	 * single call in the log (no minidump is produced on a hard crash) */
	bool dbg = enc->log_counter < 40;

	if (dbg)
		encoder_log(LOG_INFO, enc, "enc enter pts=%lld", (long long)frame->pts);

	av_frame_unref(enc->frame);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc post frame_unref");

	if (enc->codec_context->pix_fmt != AV_PIX_FMT_NV12 || !frame->data[0] || !frame->data[1]) {
		encoder_log(LOG_ERROR, enc, "unexpected input format (expected NV12)");
		return false;
	}

	int64_t submit_us = enc->clock ? realtime_clock_now_us(enc->clock) : realtime_clock_local_now_us();
	put_entry(enc, frame->pts, submit_us);

	enc->frame->format = enc->codec_context->pix_fmt;
	enc->frame->width = enc->codec_context->width;
	enc->frame->height = enc->codec_context->height;
	enc->frame->pts = frame->pts;
	enc->frame->linesize[0] = frame->linesize[0];
	enc->frame->linesize[1] = frame->linesize[1];
	enc->frame->data[0] = frame->data[0];
	enc->frame->data[1] = frame->data[1];
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc frame fields set (%ux%u ls=%d/%d)", enc->codec_context->width,
			    enc->codec_context->height, frame->linesize[0], frame->linesize[1]);

	int ret = avcodec_send_frame(enc->codec_context, enc->frame);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc send_frame ret=%d", ret);
	av_frame_unref(enc->frame); /* we never attached buffers */

	if (ret < 0) {
		av_strerror(ret, errbuf, sizeof(errbuf));
		encoder_log(LOG_ERROR, enc, "avcodec_send_frame: %s (%d)", errbuf, ret);
		return false;
	}

	ret = avcodec_receive_packet(enc->codec_context, enc->packet);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc receive_packet ret=%d", ret);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		av_packet_unref(enc->packet);
		return true; /* no packet yet: normal */
	} else if (ret < 0) {
		av_strerror(ret, errbuf, sizeof(errbuf));
		encoder_log(LOG_ERROR, enc, "avcodec_receive_packet: %s (%d)", errbuf, ret);
		return false;
	}

	*received_packet = true;

	bool keyframe = (enc->packet->flags & AV_PKT_FLAG_KEY) != 0;
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc got packet key=%d sz=%d", keyframe ? 1 : 0, (int)enc->packet->size);

	/* --- build the per-frame SEI --------------------------------- */
	sei_ts_frame_info_t info;
	memset(&info, 0, sizeof(info));
	info.keyframe = keyframe;
	info.clock_ntp = enc->stamping_ntp && realtime_clock_ntp_synced(enc->clock);
	info.frame_seq = (uint32_t)enc->frame_seq++;
	info.media_pts = enc->packet->pts;

	int64_t submit_epoch = take_entry(enc, enc->packet->pts);
	info.realtime_us = submit_epoch >= 0
				   ? submit_epoch
				   : (enc->clock ? realtime_clock_now_us(enc->clock) : realtime_clock_local_now_us());

	uint8_t *sei_nal = NULL;
	size_t sei_nal_size = 0;
	if (!sei_ts_build_nal(enc->codec == STAMP_CODEC_H264 ? H26X_H264 /* codec ids align */
							     : H26X_HEVC,
			      &info, &sei_nal, &sei_nal_size)) {
		encoder_log(LOG_ERROR, enc, "SEI allocation failed");
		av_packet_unref(enc->packet);
		return false;
	}
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc SEI built (%zu B)", sei_nal_size);

	/* --- assemble: [prefix][inline hdr][SEI][rest] --------------- */
	size_t prefix = h26x_prefix_nal_bytes(enc->codec == STAMP_CODEC_H264 ? H26X_H264 : H26X_HEVC, enc->packet->data,
					      enc->packet->size);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc prefix=%zu pkt=%d", prefix, (int)enc->packet->size);

	size_t add = sei_nal_size + (keyframe ? enc->inline_headers_size : 0);
	size_t total = enc->packet->size + add;

	/* Reuse one output buffer whose address must stay stable once handed to
	 * OBS: OBS may hold packet->data asynchronously (interleaved flv/rtmp
	 * muxing), so we must never let this buffer MOVE. It is grown up front
	 * with headroom in create(); growing here is a rare fallback. */
	if (enc->out_buffer_size < total) {
		enc->out_buffer = brealloc(enc->out_buffer, total);
		enc->out_buffer_size = total;
	}
	sei_trace(enc, "ASSEMBLE add=%zu total=%zu cap=%zu key=%d", add, total, enc->out_buffer_size, keyframe ? 1 : 0);

	size_t o = 0;
	memcpy(enc->out_buffer, enc->packet->data, prefix);
	o += prefix;
	sei_trace(enc, "C1 prefix ok o=%zu", o);
	if (keyframe && enc->inline_headers && enc->inline_headers_size > 0) {
		memcpy(enc->out_buffer + o, enc->inline_headers, enc->inline_headers_size);
		o += enc->inline_headers_size;
	}
	sei_trace(enc, "C2 inline ok o=%zu", o);
	memcpy(enc->out_buffer + o, sei_nal, sei_nal_size);
	o += sei_nal_size;
	sei_trace(enc, "C3 sei ok o=%zu", o);
	sei_trace(enc, "C4 pkt=%p size=%d prefix=%zu o=%zu dst=%p len=%zu", enc->packet->data, (int)enc->packet->size,
		  prefix, o, (void *)(enc->out_buffer + o), enc->packet->size - prefix);
	memcpy(enc->out_buffer + o, enc->packet->data + prefix, enc->packet->size - prefix);
	free(sei_nal); /* sei_ts_build_nal() allocates with C malloc */
	sei_trace(enc, "C4 rest ok total=%zu", total);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc assembled (%zu B)", total);

	packet->data = enc->out_buffer;
	packet->size = total;
	packet->type = OBS_ENCODER_VIDEO;
	packet->pts = enc->packet->pts;
	packet->dts = enc->packet->dts;
	packet->keyframe = keyframe;

	av_packet_unref(enc->packet);
	if (dbg)
		encoder_log(LOG_INFO, enc, "enc packet handed to OBS (%zu B)", total);
	sei_trace(enc, "DONE handed to OBS size=%zu", total);

	if ((++enc->log_counter & 0x3FFu) == 0)
		encoder_log(LOG_DEBUG, enc,
			    "frame seq=%u pts=%lld realtime=%lldus key=%d "
			    "pkt=%zuB",
			    (unsigned)info.frame_seq, (long long)info.media_pts, (long long)info.realtime_us,
			    keyframe ? 1 : 0, total);

	return true;
}

/* ------------------------------------------------------------------ */
/* OBS glue                                                            */
/* ------------------------------------------------------------------ */

static bool stamp_encoder_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct stamp_encoder *enc = data;
	if (!enc || !enc->extra_data) {
		*extra_data = NULL;
		*size = 0;
		return false;
	}
	*extra_data = enc->extra_data;
	*size = enc->extra_data_size;
	return true;
}

static void stamp_encoder_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	/* we consume legacy CPU frames; ask for NV12 so the conversion cost
	 * lands where every other non-texture encoder pays it */
	info->format = VIDEO_FORMAT_NV12;
}

static bool stamp_encoder_update(void *data, obs_data_t *settings)
{
	/* bitrate/other runtime updates are not applied live; OBS recreates
	 * the encoder when the user re-applies output settings. */
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
	return true;
}

static const char *h264_getname(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Encoder.H264");
}

static const char *hevc_getname(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Encoder.HEVC");
}

/* candidate lists are discovered dynamically so a build without a given
 * hardware encoder simply does not advertise it */
static void populate_codec_list(obs_property_t *list, enum stamp_codec codec)
{
	if (codec == STAMP_CODEC_H264) {
		for (size_t i = 0; i < N_H264_CANDIDATES; i++) {
			if (avcodec_find_encoder_by_name(h264_candidates[i].name))
				obs_property_list_add_string(list, h264_candidates[i].label, h264_candidates[i].name);
		}
	} else {
		for (size_t i = 0; i < N_HEVC_CANDIDATES; i++) {
			if (avcodec_find_encoder_by_name(hevc_candidates[i].name))
				obs_property_list_add_string(list, hevc_candidates[i].label, hevc_candidates[i].name);
		}
	}
}

static obs_properties_t *stamp_properties_common(void *data, enum stamp_codec codec)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, SET_CODEC_NAME, obs_module_text("Prop.Encoder"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_codec_list(list, codec);

	obs_property_t *rc = obs_properties_add_list(props, SET_RC, obs_module_text("Prop.RateControl"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(rc, obs_module_text("Rate.CBR"), "CBR");
	obs_property_list_add_string(rc, obs_module_text("Rate.VBR"), "VBR");

	obs_properties_add_int(props, SET_BITRATE, obs_module_text("Prop.Bitrate"), 100, 100000, 100);
	obs_properties_add_int(props, SET_KEYINT, obs_module_text("Prop.KeyInterval"), 1, 10, 1);
	obs_properties_add_int(props, SET_BFRAMES, obs_module_text("Prop.BFrames"), 0, 4, 1);

	obs_property_t *preset = obs_properties_add_list(props, SET_PRESET, obs_module_text("Prop.Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset, obs_module_text("Preset.VeryFast"), "veryfast");
	obs_property_list_add_string(preset, obs_module_text("Preset.Fast"), "fast");
	obs_property_list_add_string(preset, obs_module_text("Preset.Medium"), "medium");
	obs_property_list_add_string(preset, obs_module_text("Preset.Slow"), "slow");
	obs_property_list_add_string(preset, obs_module_text("Preset.VerySlow"), "veryslow");

	obs_properties_add_text(props, SET_PROFILE, obs_module_text("Prop.Profile"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, SET_TUNE, obs_module_text("Prop.Tune"), OBS_TEXT_DEFAULT);

	/* --- timestamp / NTP --- */
	obs_properties_t *ts = obs_properties_create();
	obs_property_t *group = obs_properties_add_group(props, obs_module_text("Prop.TimestampGroup"),
							 obs_module_text("Prop.TimestampGroup"), OBS_GROUP_NORMAL, ts);
	UNUSED_PARAMETER(group);
	obs_properties_add_bool(ts, SET_NTP_ENABLED, obs_module_text("Prop.EnableNTP"));
	obs_properties_add_text(ts, SET_NTP_SERVER, obs_module_text("Prop.NTPServer"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(ts, SET_NTP_PORT, obs_module_text("Prop.NTPPort"), 1, 65535, 1);
	obs_properties_add_int(ts, SET_NTP_INTERVAL, obs_module_text("Prop.ResyncInterval"), 1000, 600000, 1000);

	return props;
}

static obs_properties_t *h264_properties(void *data)
{
	return stamp_properties_common(data, STAMP_CODEC_H264);
}

static obs_properties_t *hevc_properties(void *data)
{
	return stamp_properties_common(data, STAMP_CODEC_HEVC);
}

static void stamp_defaults_common(obs_data_t *settings, enum stamp_codec codec)
{
	const char *default_codec = pick_default_codec_name(codec);
	obs_data_set_default_string(settings, SET_CODEC_NAME, default_codec ? default_codec : "");
	obs_data_set_default_string(settings, SET_RC, "CBR");
	obs_data_set_default_int(settings, SET_BITRATE, 6000);
	obs_data_set_default_int(settings, SET_KEYINT, 2);
	obs_data_set_default_int(settings, SET_BFRAMES, 2);
	obs_data_set_default_string(settings, SET_PRESET, "medium");
	obs_data_set_default_string(settings, SET_PROFILE, "");
	obs_data_set_default_string(settings, SET_TUNE, "");

	obs_data_set_default_bool(settings, SET_NTP_ENABLED, true);
	obs_data_set_default_string(settings, SET_NTP_SERVER, DEFAULT_NTP_SERVER);
	obs_data_set_default_int(settings, SET_NTP_PORT, DEFAULT_NTP_PORT);
	obs_data_set_default_int(settings, SET_NTP_INTERVAL, DEFAULT_NTP_INTERVAL_MS);
}

static void h264_defaults(obs_data_t *settings)
{
	stamp_defaults_common(settings, STAMP_CODEC_H264);
}

static void hevc_defaults(obs_data_t *settings)
{
	stamp_defaults_common(settings, STAMP_CODEC_HEVC);
}

struct obs_encoder_info stamp_h264_encoder_info = {
	.id = "sei_timestamp_encoder_h264",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = h264_getname,
	.create = h264_create,
	.destroy = stamp_encoder_destroy_data,
	.encode = stamp_encoder_encode,
	.get_defaults = h264_defaults,
	.get_properties = h264_properties,
	.update = stamp_encoder_update,
	.get_video_info = stamp_encoder_video_info,
	.get_extra_data = stamp_encoder_extra_data,
	.caps = 0,
};

struct obs_encoder_info stamp_hevc_encoder_info = {
	.id = "sei_timestamp_encoder_hevc",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = hevc_getname,
	.create = hevc_create,
	.destroy = stamp_encoder_destroy_data,
	.encode = stamp_encoder_encode,
	.get_defaults = hevc_defaults,
	.get_properties = hevc_properties,
	.update = stamp_encoder_update,
	.get_video_info = stamp_encoder_video_info,
	.get_extra_data = stamp_encoder_extra_data,
	.caps = 0,
};
