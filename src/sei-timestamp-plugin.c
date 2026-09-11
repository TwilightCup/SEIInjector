/******************************************************************************
    SEIInjector - OBS Studio plugin: per-frame real-time stamps in pushed video
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#include <obs-module.h>

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
	blog(LOG_INFO, "[SEI Timestamp] plugin loading");

	/* The plugin is linked against the FFmpeg version the target OBS bundles
	 * (avcodec-62 / OBS 32); the Windows loader resolves it from OBS's app
	 * bin directory, exactly as OBS's own FFmpeg plugins do. If the build
	 * does not match the host OBS the module simply fails to load here. */
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
