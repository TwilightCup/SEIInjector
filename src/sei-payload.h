/******************************************************************************
    SEIInjector - SEI payload specification & (de)serialisation
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    This module is deliberately free of any OBS / FFmpeg dependency so that it
    can be unit-tested and reused by external tooling (see tools/).
******************************************************************************/

#ifndef SEI_PAYLOAD_H
#define SEI_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* SEI identification                                                 */
/* ------------------------------------------------------------------ */

/* UUID identifying the SEI injected by this plugin.
 * Canonical form:  7e57c2ee-0dd2-4b53-9b35-93edf97a12c1
 * All bytes big-endian / network byte order where multi-byte. */
extern const uint8_t SEI_TS_UUID[16];

/* Current payload format version */
#define SEI_TS_PAYLOAD_VERSION 1u

/* SEI payload type (H.264 / H.265 common) */
#define SEI_PAYLOAD_TYPE_USER_DATA_UNREGISTERED 5u

/* NAL unit types */
#define SEI_NAL_TYPE_H264 6u          /* sei_rbsp                                             */
#define SEI_NAL_TYPE_H265_PREFIX 39u  /* PREFIX_SEI_NUT                                       */
#define SEI_NAL_TYPE_H265_SUFFIX 40u  /* SUFFIX_SEI_NUT (unused)                              */

/* flags byte bit definitions (bit 0 = LSB of the flags octet) */
#define SEI_TS_FLAG_KEYFRAME 0x01u    /* frame carries the IDR / CRA coded picture           */
#define SEI_TS_FLAG_CLOCK_NTP 0x02u   /* realtime_us comes from an NTP-synchronised clock    */
#define SEI_TS_FLAG_CLOCK_LOCAL 0x00u /* realtime_us comes from the local wall clock         */

/*
 * On-wire payload layout (after the ISO user_data_unregistered UUID, i.e. the
 * 16 bytes of SEI_TS_UUID are *excluded* from this structure):
 *
 *   offset  size  field
 *   ------  ----  ------------------------------------------------------------
 *   0       1     version            = SEI_TS_PAYLOAD_VERSION
 *   1       1     flags              see SEI_TS_FLAG_* above
 *   2       4     frame_seq          monotonically increasing coded-frame counter (BE)
 *   6       8     media_pts          OBS/container presentation timestamp of this frame (BE)
 *   14      8     realtime_us        microseconds since the Unix epoch (BE)
 *   ------  ----  total payload after UUID = 22 bytes
 *
 * Note: a few versions of the design embedded extra fields for future use;
 * version byte exists so receivers can reject/adapt gracefully.
 */

typedef struct sei_ts_frame_info {
	bool keyframe;
	bool clock_ntp;
	uint32_t frame_seq;
	int64_t media_pts;
	int64_t realtime_us; /* epoch microseconds; negative == unknown */
} sei_ts_frame_info_t;

/* Maximum raw payload size we will ever build (uuid + fields) */
#define SEI_TS_RAW_PAYLOAD_MAX (16 + 22u)

/* ------------------------------------------------------------------ */
/* Construction                                                       */
/* ------------------------------------------------------------------ */

/*
 * Builds the complete SEI NAL unit (Annex-B start code included) for a
 * user_data_unregistered message carrying the timestamp info.
 *
 * codec: 0 = H.264, 1 = H.265/HEVC (prefix SEI)
 * Allocates *out (malloc). Returns false on allocation failure.
 */
bool sei_ts_build_nal(int codec_h264_hevc, const sei_ts_frame_info_t *info, uint8_t **out, size_t *out_size);

/* Convenience: payload = UUID(16) + fields(22). Allocated with malloc. */
bool sei_ts_build_raw_payload(const sei_ts_frame_info_t *info, uint8_t **out, size_t *out_size);

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

/*
 * Scans a full Annex-B access unit / NAL stream ("data", "size") for our
 * SEI NAL units and returns the info of the first one found.
 * (RBSP emulation-prevention bytes are removed before matching.)
 */
bool sei_ts_parse_from_access_unit(int codec_h264_hevc, const uint8_t *data, size_t size,
				   sei_ts_frame_info_t *info_out);

/*
 * Parses one SEI NAL *payload* (i.e. the bytes after "type 5" size field,
 * including the embedded 16-byte UUID).
 */
bool sei_ts_parse_raw_payload(const uint8_t *payload, size_t payload_size, sei_ts_frame_info_t *info_out);

#ifdef __cplusplus
}
#endif

#endif /* SEI_PAYLOAD_H */
