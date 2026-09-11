/******************************************************************************
    SEIInjector - Timestamp-injecting OBS video encoder
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#ifndef SEI_TIMESTAMP_ENCODER_H
#define SEI_TIMESTAMP_ENCODER_H

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

enum stamp_codec {
	STAMP_CODEC_H264 = 0,
	STAMP_CODEC_HEVC = 1,
};

/*
 * Per-frame SEI NAL is inserted into *every* encoded video packet carrying:
 *   - realtime_us : NTP-corrected epoch microseconds of the moment OBS
 *                   submitted the source frame to this encoder,
 *   - media_pts   : the frame's PTS (identical to the value carried in the
 *                   muxed container for that frame),
 *   - frame_seq   : a per-stream coded-frame counter,
 * plus a UUID so receivers can distinguish our SEI from others.
 */

/* encoder context shared by the H.264 and HEVC encoder instances */
struct stamp_encoder;

extern struct obs_encoder_info stamp_h264_encoder_info;
extern struct obs_encoder_info stamp_hevc_encoder_info;

#ifdef __cplusplus
}
#endif

#endif /* SEI_TIMESTAMP_ENCODER_H */
