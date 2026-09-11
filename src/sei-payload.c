/******************************************************************************
    SEIInjector - SEI payload implementation
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later
******************************************************************************/

#include "sei-payload.h"

#include <stdlib.h>
#include <string.h>

/* Canonical UUID: 7e57c2ee-0dd2-4b53-9b35-93edf97a12c1 */
const uint8_t SEI_TS_UUID[16] = {0x7e, 0x57, 0xc2, 0xee, 0x0d, 0xd2, 0x4b, 0x53,
				 0x9b, 0x35, 0x93, 0xed, 0xf9, 0x7a, 0x12, 0xc1};

/* Field size after the embedded UUID */
#define SEI_TS_FIELDS_SIZE 22u

#define SEI_TS_CODEC_H264 0
#define SEI_TS_CODEC_HEVC 1

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void put_u32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_u64be(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint32_t get_u32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_u64be(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

/*
 * Writes a SEI "more_rbsp_data / payload" length using the 0xFF chain coding
 * used by payloadSize/payloadType. Returns bytes written (>=1).
 */
static size_t write_sei_var_uint(uint8_t *out, size_t cap, size_t value)
{
	size_t written = 0;
	while (value >= 0xFF && written < cap) {
		out[written++] = 0xFF;
		value -= 0xFF;
	}
	if (written < cap)
		out[written++] = (uint8_t)value;
	return written;
}

/*
 * Reads one 0xFF-chain coded value. Returns bytes consumed or 0 on error.
 */
static size_t read_sei_var_uint(const uint8_t *in, size_t in_size, size_t *value_out)
{
	size_t value = 0;
	size_t i = 0;
	while (i < in_size && in[i] == 0xFF) {
		value += 0xFF;
		i++;
		if (value > 0x1000000) /* sanity */
			return 0;
	}
	if (i >= in_size)
		return 0;
	value += in[i];
	i++;
	*value_out = value;
	return i;
}

/*
 * Unescapes until "want" unescaped bytes have been produced (SEI payloadSize
 * is expressed in unescaped bytes) or the source is exhausted. Returns the
 * number of unescaped bytes written (may be < want).
 */
static size_t unescape_rbsp_limit(const uint8_t *src, size_t size, uint8_t *dst, size_t want)
{
	size_t o = 0;
	size_t zeros = 0;
	for (size_t i = 0; i < size && o < want; i++) {
		uint8_t b = src[i];
		if (zeros >= 2 && b == 0x03) {
			zeros = 0; /* drop the EPB */
			continue;
		}
		dst[o++] = b;
		if (b == 0)
			zeros++;
		else
			zeros = 0;
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* construction                                                       */
/* ------------------------------------------------------------------ */

bool sei_ts_build_raw_payload(const sei_ts_frame_info_t *info, uint8_t **out, size_t *out_size)
{
	uint8_t fields[SEI_TS_FIELDS_SIZE];

	if (!info || !out || !out_size)
		return false;

	fields[0] = SEI_TS_PAYLOAD_VERSION;
	fields[1] =
		(uint8_t)((info->keyframe ? SEI_TS_FLAG_KEYFRAME : 0) | (info->clock_ntp ? SEI_TS_FLAG_CLOCK_NTP : 0));
	put_u32be(fields + 2, info->frame_seq);
	put_u64be(fields + 6, (uint64_t)info->media_pts);
	put_u64be(fields + 14, (uint64_t)info->realtime_us);

	uint8_t *buf = malloc(16 + SEI_TS_FIELDS_SIZE);
	if (!buf)
		return false;

	memcpy(buf, SEI_TS_UUID, 16);
	memcpy(buf + 16, fields, SEI_TS_FIELDS_SIZE);
	*out = buf;
	*out_size = 16 + SEI_TS_FIELDS_SIZE;
	return true;
}

bool sei_ts_build_nal(int codec_h264_hevc, const sei_ts_frame_info_t *info, uint8_t **out, size_t *out_size)
{
	uint8_t *payload = NULL;
	size_t payload_size = 0;
	if (!sei_ts_build_raw_payload(info, &payload, &payload_size))
		return false;

	/*
	 * NAL (Annex B):
	 *   start code 4B
	 *   header      1B (H.264) / 2B (H.265 prefix SEI)
	 *   type (5)    1..4B
	 *   size        N bytes (raw payload length incl. UUID)
	 *   payload     payload_size bytes (escaped)
	 *   rbsp_trailing 1B (0x80)
	 */
	uint8_t header[2];
	size_t header_size;
	if (codec_h264_hevc == SEI_TS_CODEC_H264) {
		header[0] = SEI_NAL_TYPE_H264; /* forbidden=0, nri=0 */
		header_size = 1;
	} else {
		header[0] = (uint8_t)(SEI_NAL_TYPE_H265_PREFIX << 1);
		header[1] = 0x01; /* layer_id=0, temporal_id_plus1=1 */
		header_size = 2;
	}

	uint8_t type_buf[5];
	uint8_t size_buf[5];
	size_t type_len = write_sei_var_uint(type_buf, sizeof type_buf, SEI_PAYLOAD_TYPE_USER_DATA_UNREGISTERED);
	size_t size_len = write_sei_var_uint(size_buf, sizeof size_buf, payload_size);

	/* Worst-case escaping growth: insert 0x03 for every 0x00 byte. */
	size_t max_total = 4 + header_size + type_len + size_len + payload_size * 2 + 1;
	uint8_t *nal = malloc(max_total);
	if (!nal) {
		free(payload);
		return false;
	}

	size_t o = 0;
	nal[o++] = 0x00;
	nal[o++] = 0x00;
	nal[o++] = 0x00;
	nal[o++] = 0x01;
	memcpy(nal + o, header, header_size);
	o += header_size;
	memcpy(nal + o, type_buf, type_len);
	o += type_len;
	memcpy(nal + o, size_buf, size_len);
	o += size_len;

	/*
	 * Escape the payload, tracking zero-runs *across* the whole RBSP we
	 * just emitted (header/type/size bytes are also RBSP) so no illegal
	 * 00 00 [00|01|02|03] sequence can survive.
	 */
	size_t zeros = 0;
	for (size_t i = 0; i < o; i++) {
		if (nal[i] == 0)
			zeros++;
		else
			zeros = 0;
	}
	for (size_t i = 0; i < payload_size; i++) {
		uint8_t b = payload[i];
		if (zeros >= 2 && b <= 0x03) {
			nal[o++] = 0x03;
			zeros = 0;
		}
		nal[o++] = b;
		if (b == 0)
			zeros++;
		else
			zeros = 0;
	}

	nal[o++] = 0x80; /* rbsp_stop_one_bit */
	free(payload);

	*out = nal;
	*out_size = o;
	return true;
}

/* ------------------------------------------------------------------ */
/* parsing                                                            */
/* ------------------------------------------------------------------ */

bool sei_ts_parse_raw_payload(const uint8_t *payload, size_t payload_size, sei_ts_frame_info_t *info_out)
{
	sei_ts_frame_info_t info;
	if (!payload || !info_out || payload_size < 16 + SEI_TS_FIELDS_SIZE)
		return false;

	if (memcmp(payload, SEI_TS_UUID, 16) != 0)
		return false;
	/* Only the *first* SEI_TS_UUID occurrence is our record: consumer data
	 * following the UUID (ISO/IEC 14496-10 § 7.3.2.2) is ignored. */
	if (payload[16] != SEI_TS_PAYLOAD_VERSION)
		return false;

	info.keyframe = (payload[17] & SEI_TS_FLAG_KEYFRAME) != 0;
	info.clock_ntp = (payload[17] & SEI_TS_FLAG_CLOCK_NTP) != 0;
	info.frame_seq = get_u32be(payload + 18);
	info.media_pts = (int64_t)get_u64be(payload + 22);
	info.realtime_us = (int64_t)get_u64be(payload + 30);

	*info_out = info;
	return true;
}

/*
 * Scans "data/size" for NAL start codes. When found a SEI NAL, walks its
 * type/size fields and tests each user-data-unregistered payload against our
 * UUID. Handles H.264 and H.265 two-byte NAL headers.
 */
bool sei_ts_parse_from_access_unit(int codec_h264_hevc, const uint8_t *data, size_t size, sei_ts_frame_info_t *info_out)
{
	if (!data || !info_out)
		return false;

	const uint8_t *pos = data;
	size_t remaining = size;

	/* stack buffer for one unescaped message (uuid + fields + slack) */
	uint8_t unesc[16 + SEI_TS_FIELDS_SIZE + 8];

	while (remaining > 4) {
		/* find next start code */
		size_t sc = 0;
		if (pos[0] == 0 && pos[1] == 0 && pos[2] == 1) {
			sc = 3;
		} else if (pos[0] == 0 && pos[1] == 0 && pos[2] == 0 && pos[3] == 1) {
			sc = 4;
		} else {
			pos++;
			remaining--;
			continue;
		}

		const uint8_t *nal = pos + sc;
		size_t nal_rem = remaining - sc;
		if (nal_rem < 1)
			break;

		/* determine SEI type for the codec */
		bool is_sei = false;
		size_t hdr = 1;
		if (codec_h264_hevc == SEI_TS_CODEC_H264) {
			if ((nal[0] & 0x1F) == SEI_NAL_TYPE_H264)
				is_sei = true;
		} else {
			uint8_t t = (nal[0] >> 1) & 0x3F;
			if (t == SEI_NAL_TYPE_H265_PREFIX || t == SEI_NAL_TYPE_H265_SUFFIX) {
				is_sei = true;
				hdr = 2;
			}
		}

		if (is_sei && nal_rem > hdr) {
			/* Parse the first SEI message of this NAL (our injected
			 * NALs always carry exactly one message). */
			size_t p = hdr;
			size_t type = 0;
			size_t n = read_sei_var_uint(nal + p, nal_rem - p, &type);
			if (n == 0)
				goto next_nal;
			p += n;
			size_t msg_size = 0;
			n = read_sei_var_uint(nal + p, nal_rem - p, &msg_size);
			if (n == 0)
				goto next_nal;
			p += n;

			if (type == SEI_PAYLOAD_TYPE_USER_DATA_UNREGISTERED && msg_size >= 16 + SEI_TS_FIELDS_SIZE &&
			    msg_size <= sizeof(unesc)) {
				/* payloadSize counts unescaped bytes; the wire
				 * payload may be longer due to EPBs. */
				size_t got = unescape_rbsp_limit(nal + p, nal_rem - p, unesc, msg_size);
				if (got >= 16 + SEI_TS_FIELDS_SIZE &&
				    sei_ts_parse_raw_payload(unesc, msg_size, info_out))
					return true;
			}
		}

	next_nal:; /* jump past this NAL */
		size_t scan = 0;
		bool found_next = false;
		while (scan + 3 <= nal_rem) {
			if (nal[scan] == 0 && nal[scan + 1] == 0 && nal[scan + 2] == 1) {
				found_next = true;
				break;
			}
			if (scan + 4 <= nal_rem && nal[scan] == 0 && nal[scan + 1] == 0 && nal[scan + 2] == 0 &&
			    nal[scan + 3] == 1) {
				found_next = true;
				break;
			}
			scan++;
		}
		if (!found_next)
			break;
		pos = nal + scan;
		remaining = size - (pos - data);
	}

	return false;
}
