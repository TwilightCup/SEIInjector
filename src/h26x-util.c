/******************************************************************************
    SEIInjector - H.264/H.265 bitstream helpers
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#include "h26x-util.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* start-code handling                                                */
/* ------------------------------------------------------------------ */

bool h26x_has_start_code(const uint8_t *data, size_t size)
{
	if (!data || size < 3)
		return false;
	if (data[0] == 0 && data[1] == 0 && data[2] == 1)
		return true;
	return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

/*
 * Finds the next start code at or after "from". Returns its offset relative
 * to "data" (the position of the 0x00 run), or SIZE_MAX when none found.
 * "*sc_size" receives 3 or 4.
 */
static size_t find_next_start_code(const uint8_t *data, size_t size, size_t from, size_t *sc_size)
{
	for (size_t i = from; i + 3 <= size; i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			*sc_size = 3;
			return i;
		}
		if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
			*sc_size = 4;
			return i;
		}
	}
	return SIZE_MAX;
}

/* NAL unit type of the NAL starting at nal (after its start code) */
static uint8_t nal_type_h264(const uint8_t *nal)
{
	return nal[0] & 0x1F;
}

static uint8_t nal_type_h265(const uint8_t *nal)
{
	return (nal[0] >> 1) & 0x3F;
}

static bool is_prefix_nal(enum h26x_codec codec, uint8_t type)
{
	if (codec == H26X_H264) {
		/* AUD, SPS, PPS, SEI */
		return type == 9 || type == 7 || type == 8 || type == 6;
	} else {
		/* AUD, VPS, SPS, PPS, PREFIX/SUFFIX SEI */
		return type == 35 || type == 32 || type == 33 || type == 34 || type == 39 || type == 40;
	}
}

size_t h26x_prefix_nal_bytes(enum h26x_codec codec, const uint8_t *data, size_t size)
{
	if (!data || size < 4 || !h26x_has_start_code(data, size))
		return 0;

	size_t pos = 0;
	while (pos + 4 <= size) {
		size_t sc = 0;
		size_t sc_at = find_next_start_code(data, size, pos, &sc);
		if (sc_at == SIZE_MAX)
			break;
		const uint8_t *nal = data + sc_at + sc;
		size_t nal_rem = size - (sc_at + sc);
		if (nal_rem < 1)
			break;

		uint8_t type = (codec == H26X_H264) ? nal_type_h264(nal) : nal_type_h265(nal);
		if (!is_prefix_nal(codec, type))
			return sc_at; /* first VCL (or other) NAL starts here */

		/* advance past this NAL */
		size_t next = find_next_start_code(data, size, sc_at + sc + 1, &sc);
		if (next == SIZE_MAX)
			return size; /* AU ends after prefix NALs */
		pos = next;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* extradata -> Annex-B parameter sets                                 */
/* ------------------------------------------------------------------ */

/*
 * Writes one NAL (without start code) with a 4-byte Annex-B start code into
 * *out (which must already point to a writable cursor). Returns bytes used.
 */
static size_t emit_nal(uint8_t *out, const uint8_t *nal, size_t len)
{
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	out[3] = 1;
	if (len > 0 && nal)
		memcpy(out + 4, nal, len);
	return 4 + len;
}

static bool avcc_is_annexb(const uint8_t *xd, size_t n)
{
	return n >= 4 && xd[0] == 0 && xd[1] == 0 && ((xd[2] == 0 && xd[3] == 1) || xd[2] == 1);
}

/*
 * avcC layout (ISO/IEC 14496-15):
 *   [0]    configurationVersion = 1
 *   [1..3] AVCProfileIndication / profile_compatibility / AVCLevelIndication
 *   [4]    (6 reserved) | lengthSizeMinusOne (2)
 *   [5]    (3 reserved) | numOfSequenceParameterSets (5)
 *   then numSPS x { 2-byte length, SPS } ; numPPS (1); numPPS x {2B,PPS}
 */
static uint8_t *parse_avcc(const uint8_t *xd, size_t n, size_t *out_size)
{
	*out_size = 0;
	if (n < 7 || xd[0] != 1)
		return NULL;

	size_t pos = 5;
	int num_sps = xd[pos++] & 0x1F;
	if (pos + 1 > n)
		return NULL;

	/* pass 1: validate + tally */
	size_t total = 0;
	size_t scan = pos;
	for (int i = 0; i < num_sps; i++) {
		if (scan + 2 > n)
			return NULL;
		uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		if (scan + len > n)
			return NULL;
		total += 4 + len;
		scan += len;
	}
	if (scan + 1 > n)
		return NULL;
	int num_pps = xd[scan++];
	for (int i = 0; i < num_pps; i++) {
		if (scan + 2 > n)
			return NULL;
		uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		if (scan + len > n)
			return NULL;
		total += 4 + len;
		scan += len;
	}
	if (total == 0)
		return NULL;

	uint8_t *out = malloc(total);
	if (!out)
		return NULL;
	uint8_t *wp = out;
	scan = pos;
	for (int i = 0; i < num_sps; i++) {
		uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		wp += emit_nal(wp, xd + scan, len);
		scan += len;
	}
	scan++; /* skip num_pps */
	for (int i = 0; i < num_pps; i++) {
		uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		wp += emit_nal(wp, xd + scan, len);
		scan += len;
	}

	*out_size = total;
	return out;
}

/*
 * hvcC layout (ISO/IEC 14496-15):
 *   [0]     configurationVersion = 1
 *   [1..12] general profile space/tier/profile_idc(1) +
 *           general_profile_compatibility_flags(4) +
 *           general_constraint_indicator_flags(6) + general_level_idc(1)
 *   [13..14] min_spatial_segmentation_idc (2)
 *   [15]    parallelismType (1)
 *   [16]    chromaFormat (1)
 *   [17]    bitDepthLumaMinus8 (1)
 *   [18]    bitDepthChromaMinus8 (1)
 *   [19..20] avgFrameRate (2)
 *   [21]    constantFrameRate(2)|numTemporalLayers(3)|temporalIdNested(1)|lengthSizeMinusOne(2)
 *   [22]    numOfArrays (1)
 *   then numArrays x { array: (1) completeness(1)+reserved(6)+NAL_unit_type(1);
 *                       (2) numNalus; then numNalus x { (2) length, NAL } }
 */
static uint8_t *parse_hvcc(const uint8_t *xd, size_t n, size_t *out_size)
{
	*out_size = 0;
	if (n < 23 || xd[0] != 1)
		return NULL;

	size_t pos = 22;
	int num_arrays = xd[pos++];

	/* pass 1: validate + tally (only VPS/SPS/PPS arrays are useful, but we
	 * keep every NAL that arrives to be safe) */
	size_t total = 0;
	size_t scan = pos;
	for (int a = 0; a < num_arrays; a++) {
		if (scan + 3 > n)
			return NULL;
		scan++; /* array header */
		int num_nalus = ((int)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		if (num_nalus <= 0 || num_nalus > 64)
			return NULL;
		for (int i = 0; i < num_nalus; i++) {
			if (scan + 2 > n)
				return NULL;
			uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
			scan += 2;
			if (scan + len > n)
				return NULL;
			total += 4 + len;
			scan += len;
		}
	}
	if (total == 0)
		return NULL;

	uint8_t *out = malloc(total);
	if (!out)
		return NULL;
	uint8_t *wp = out;
	scan = pos;
	for (int a = 0; a < num_arrays; a++) {
		scan++; /* skip array header */
		int num_nalus = ((int)xd[scan] << 8) | xd[scan + 1];
		scan += 2;
		for (int i = 0; i < num_nalus; i++) {
			uint16_t len = ((uint16_t)xd[scan] << 8) | xd[scan + 1];
			scan += 2;
			wp += emit_nal(wp, xd + scan, len);
			scan += len;
		}
	}

	*out_size = total;
	return out;
}

uint8_t *h26x_extradata_to_annexb(enum h26x_codec codec, const uint8_t *extradata, size_t extradata_size,
				  size_t *out_size)
{
	*out_size = 0;
	if (!extradata || extradata_size < 4)
		return NULL;

	if (avcc_is_annexb(extradata, extradata_size)) {
		uint8_t *out = malloc(extradata_size);
		if (!out)
			return NULL;
		memcpy(out, extradata, extradata_size);
		*out_size = extradata_size;
		return out;
	}

	if (codec == H26X_H264)
		return parse_avcc(extradata, extradata_size, out_size);
	else
		return parse_hvcc(extradata, extradata_size, out_size);
}

unsigned h26x_extradata_length_size(enum h26x_codec codec, const uint8_t *data, size_t size)
{
	if (!data || !size || data[0] != 1)
		return 0;
	if (codec == H26X_H264 && size >= 7)
		return (data[4] & 3u) + 1;
	if (codec == H26X_HEVC && size >= 23)
		return (data[21] & 3u) + 1;
	return 0;
}

static bool valid_packet_nal(enum h26x_codec codec, const uint8_t *nal, size_t size, bool *vcl)
{
	if (!size || (nal[0] & 0x80))
		return false;
	if (codec == H26X_H264) {
		unsigned type = nal[0] & 31;
		if (!type || type >= 24)
			return false;
		*vcl |= type >= 1 && type <= 5;
	} else {
		if (size < 2 || !(nal[1] & 7))
			return false;
		*vcl |= ((nal[0] >> 1) & 63) <= 31;
	}
	return true;
}

static bool valid_lengths(enum h26x_codec codec, const uint8_t *data, size_t size,
                          unsigned width, size_t *total)
{
	if (width < 1 || width > 4)
		return false;
	size_t pos = 0;
	bool vcl = false;
	*total = 0;
	while (pos < size) {
		if (size - pos < width)
			return false;
		uint32_t len = 0;
		for (unsigned j = 0; j < width; ++j)
			len = (len << 8) | data[pos++];
		if (len > size - pos || !valid_packet_nal(codec, data + pos, len, &vcl))
			return false;
		if (*total > SIZE_MAX - 4 || len > SIZE_MAX - 4 - *total)
			return false;
		*total += 4 + (size_t)len;
		pos += len;
	}
	return vcl;
}

bool h26x_packet_to_annexb(enum h26x_codec codec, const uint8_t *data, size_t size,
                          unsigned length_size, uint8_t **out, size_t *out_size)
{
	*out = NULL;
	*out_size = 0;
	if (!data || !size || length_size > 4)
		return false;
	bool annex = h26x_has_start_code(data, size), vcl = false;
	if (annex) {
		size_t pos = 0;
		while (pos < size) {
			size_t sc = 0;
			size_t at = find_next_start_code(data, size, pos, &sc);
			if (at != pos) {
				annex = false;
				break;
			}
			size_t ignored;
			size_t end = find_next_start_code(data, size, at + sc, &ignored);
			if (end == SIZE_MAX)
				end = size;
			if (!valid_packet_nal(codec, data + at + sc, end - at - sc, &vcl)) {
				annex = false;
				break;
			}
			pos = end;
		}
		annex &= vcl;
	}
	size_t total = 0;
	bool lengths = valid_lengths(codec, data, size, length_size, &total);
	if (annex && lengths)
		return false; /* never guess between two valid interpretations */
	if (annex) {
		/* Even without avcC/hvcC, reject a packet also parseable as lengths. */
		for (unsigned w = 1; w <= 4; ++w)
			if (valid_lengths(codec, data, size, w, &total))
				return false;
		*out = malloc(size);
		if (!*out)
			return false;
		memcpy(*out, data, size);
		*out_size = size;
		return true;
	}
	if (!lengths)
		return false;
	uint8_t *buf = malloc(total);
	if (!buf)
		return false;
	size_t pos = 0, written = 0;
	while (pos < size) {
		uint32_t len = 0;
		for (unsigned j = 0; j < length_size; ++j)
			len = (len << 8) | data[pos++];
		written += emit_nal(buf + written, data + pos, len);
		pos += len;
	}
	*out = buf;
	*out_size = written;
	return true;
}
