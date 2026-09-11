/******************************************************************************
    SEIInjector - H.264/H.265 bitstream helpers (Annex-B, avcC/hvcC)
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    Pure C, no OBS/FFmpeg dependency (unit-testable).
******************************************************************************/

#ifndef H26X_UTIL_H
#define H26X_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum h26x_codec {
	H26X_H264 = 0,
	H26X_HEVC = 1,
};

/*
 * Converts an encoder extradata blob into an Annex-B byte stream containing
 * the parameter sets (H.264: SPS+PPS; H.265: VPS+SPS+PPS) ready to be
 * prepended inline to keyframes.
 *
 * Accepts:
 *   - extradata that already is Annex-B (start codes),
 *   - H.264 AVCDecoderConfigurationRecord (avcC),
 *   - H.265 HEVCDecoderConfigurationRecord (hvcC).
 *
 * Returns a malloc'd buffer (caller frees) or NULL if the blob is not
 * recognised. *out_size receives the byte count.
 */
uint8_t *h26x_extradata_to_annexb(enum h26x_codec codec, const uint8_t *extradata, size_t extradata_size,
				  size_t *out_size);

/*
 * Returns the number of leading bytes of an Annex-B access unit that belong
 * to "prefix" NAL units (AUD, parameter sets and existing SEI) which must
 * stay *before* any freshly injected SEI / parameter sets:
 *   H.264: AUD(9) SPS(7) PPS(8) SEI(6)
 *   H.265: AUD(35) VPS(32) SPS(33) PPS(34) SEI(39/40)
 * The returned offset is the byte position where the first non-prefix NAL
 * starts (== size when the AU only contains prefix NALs, e.g. a pure SEI
 * access unit).
 */
size_t h26x_prefix_nal_bytes(enum h26x_codec codec, const uint8_t *data, size_t size);

/*
 * True when "data" begins with a 3- or 4-byte Annex-B start code.
 */
bool h26x_has_start_code(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* H26X_UTIL_H */
