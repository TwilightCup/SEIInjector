/******************************************************************************
    SEIInjector - OBS Studio plugin: per-frame real-time stamps in pushed video
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sei-timestamp", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Injects a frame-level real-time (NTP-corrected) timestamp SEI "
	       "into every encoded video frame so that a pulling application "
	       "can align the timelines of multiple independent streams.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "SEI Timestamp";
}

extern struct obs_encoder_info stamp_h264_encoder_info;
extern struct obs_encoder_info stamp_hevc_encoder_info;

bool obs_module_load(void)
{
	const unsigned codec_version = avcodec_version();
	const unsigned util_version = avutil_version();
	blog(LOG_INFO,
	     "[SEI Timestamp] v%s; build OBS API %u.%u.%u; "
	     "FFmpeg headers avcodec=%u.%u.%u avutil=%u.%u.%u; "
	     "runtime avcodec=%u.%u.%u avutil=%u.%u.%u (direct linking)",
	     SEI_TIMESTAMP_VERSION, (unsigned)LIBOBS_API_MAJOR_VER, (unsigned)LIBOBS_API_MINOR_VER,
	     (unsigned)LIBOBS_API_PATCH_VER, (unsigned)LIBAVCODEC_VERSION_MAJOR, (unsigned)LIBAVCODEC_VERSION_MINOR,
	     (unsigned)LIBAVCODEC_VERSION_MICRO, (unsigned)LIBAVUTIL_VERSION_MAJOR, (unsigned)LIBAVUTIL_VERSION_MINOR,
	     (unsigned)LIBAVUTIL_VERSION_MICRO, codec_version >> 16, (codec_version >> 8) & 255u,
	     codec_version & 255u, util_version >> 16, (util_version >> 8) & 255u, util_version & 255u);
	/* Check before encoder code accesses FFmpeg public structs. Matching majors
	 * are necessary, not a promise that an older runtime provides every API. */
	if ((codec_version >> 16) != LIBAVCODEC_VERSION_MAJOR || (util_version >> 16) != LIBAVUTIL_VERSION_MAJOR) {
		blog(LOG_ERROR, "[SEI Timestamp] FFmpeg ABI mismatch; rebuild against the target OBS SDK");
		return false;
	}

	obs_register_encoder(&stamp_h264_encoder_info);
	blog(LOG_INFO, "[SEI Timestamp] registered H.264 encoder");

	obs_register_encoder(&stamp_hevc_encoder_info);
	blog(LOG_INFO, "[SEI Timestamp] registered H.265/HEVC encoder");

	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[SEI Timestamp] plugin unloaded");
}
