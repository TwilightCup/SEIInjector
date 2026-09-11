/******************************************************************************
    SEIInjector - standalone core logic tests (no OBS / FFmpeg needed)
    Copyright (C) 2026 SEIInjector contributors

    SPDX-License-Identifier: GPL-2.0-or-later

    Run:  cc -I../src tests/test_sei.c ../src/sei-payload.c ../src/h26x-util.c
          && ./a.out
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h26x-util.h"
#include "sei-payload.h"

static int failures = 0;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		if (cond) {                                                    \
			printf("  ok   - " fmt "\n", ##__VA_ARGS__);           \
		} else {                                                       \
			printf("  FAIL - " fmt " (line %d)\n", ##__VA_ARGS__,  \
			       __LINE__);                                       \
			failures++;                                            \
		}                                                              \
	} while (0)

static void test_nal_roundtrip(int codec, const char *label)
{
	sei_ts_frame_info_t in;
	memset(&in, 0, sizeof(in));
	in.keyframe = true;
	in.clock_ntp = true;
	in.frame_seq = 0x11223344u;
	in.media_pts = 0x0102030405060708LL;
	in.realtime_us = 1767000000123456LL; /* 2026-ish */

	uint8_t *nal = NULL;
	size_t nal_size = 0;
	CHECK(sei_ts_build_nal(codec, &in, &nal, &nal_size), "%s: build_nal", label);
	CHECK(nal && nal_size > 8, "%s: nal allocated (%zu bytes)", label, nal ? nal_size : 0);
	CHECK(nal && h26x_has_start_code(nal, nal_size), "%s: has start code", label);

	sei_ts_frame_info_t out;
	memset(&out, 0, sizeof(out));
	CHECK(sei_ts_parse_from_access_unit(codec, nal, nal_size, &out), "%s: parse found our SEI", label);
	CHECK(out.keyframe == in.keyframe && out.clock_ntp == in.clock_ntp, "%s: flags", label);
	CHECK(out.frame_seq == in.frame_seq, "%s: frame_seq", label);
	CHECK(out.media_pts == in.media_pts, "%s: media_pts", label);
	CHECK(out.realtime_us == in.realtime_us, "%s: realtime_us", label);

	/* embedded in a larger "access unit" */
	uint8_t *au = malloc(nal_size + 16);
	uint8_t *au_pos = au;
	/* fake AUD prefix */
	au_pos[0] = 0;
	au_pos[1] = 0;
	au_pos[2] = 0;
	au_pos[3] = 1;
	au_pos[4] = 0x09; /* h264 AUD (harmless for parsing both codecs) */
	au_pos += 5;
	memcpy(au_pos, nal, nal_size);
	au_pos += nal_size;
	/* fake trailing garbage */
	memset(au_pos, 0xAA, 11);
	au_pos += 11;

	memset(&out, 0, sizeof(out));
	CHECK(sei_ts_parse_from_access_unit(codec, au, (size_t)(au_pos - au), &out), "%s: parse from larger AU", label);
	CHECK(out.frame_seq == in.frame_seq, "%s: AU seq preserved", label);

	free(au);
	free(nal);
}

static void test_payload_with_zeros(void)
{
	/* force a payload with lots of zero bytes so EPB escaping is really
	 * exercised (frame_seq 0, pts 0) */
	sei_ts_frame_info_t in;
	memset(&in, 0, sizeof(in));
	in.frame_seq = 0;
	in.media_pts = 0;
	in.realtime_us = 1;

	uint8_t *payload = NULL;
	size_t payload_size = 0;
	CHECK(sei_ts_build_raw_payload(&in, &payload, &payload_size), "zero payload build");
	CHECK(payload_size == 38, "payload size == 38 (got %zu)", payload_size);

	uint8_t *nal = NULL;
	size_t nal_size = 0;
	CHECK(sei_ts_build_nal(0 /* h264 */, &in, &nal, &nal_size), "zero-payload NAL build");

	/* verify no illegal byte sequence survives in the NAL body:
	 * within RBSP data, "00 00 [00|01|02]" must never appear unescaped.
	 * ("00 00 03" is exactly the legal EPB marker.) */
	int bad = 0;
	for (size_t i = 4; i + 2 < nal_size; i++) {
		if (nal[i] == 0 && nal[i + 1] == 0 &&
		    (nal[i + 2] == 0x00 || nal[i + 2] == 0x01 || nal[i + 2] == 0x02)) {
			bad = 1;
			break;
		}
	}
	CHECK(!bad, "no unescaped 00 00 [0..3] in NAL body");

	sei_ts_frame_info_t out;
	memset(&out, 0, sizeof(out));
	CHECK(sei_ts_parse_from_access_unit(0, nal, nal_size, &out), "zero payload parses back");
	CHECK(out.realtime_us == 1 && out.frame_seq == 0, "zero payload fields intact");

	free(nal);
	free(payload);
}

static void test_extradata_avcc(void)
{
	/* craft an avcC record with two SPS and one PPS */
	uint8_t sps1[] = {0x67, 0x42, 0x00, 0x1e, 0xab};
	uint8_t pps[] = {0x68, 0xce, 0x3c, 0x80};

	uint8_t avcc[64];
	memset(avcc, 0, sizeof(avcc));
	avcc[0] = 1;
	avcc[1] = 0x42;
	avcc[2] = 0x00;
	avcc[3] = 0x1e;
	avcc[4] = 0xFF; /* lengthSizeMinusOne = 3 */
	avcc[5] = 0xE1; /* 3 reserved + 1 SPS */
	size_t p = 6;
	avcc[p++] = 0;
	avcc[p++] = (uint8_t)sizeof(sps1);
	memcpy(avcc + p, sps1, sizeof(sps1));
	p += sizeof(sps1);
	avcc[p++] = 1; /* one PPS */
	avcc[p++] = 0;
	avcc[p++] = (uint8_t)sizeof(pps);
	memcpy(avcc + p, pps, sizeof(pps));
	p += sizeof(pps);

	size_t out_size = 0;
	uint8_t *out = h26x_extradata_to_annexb(H26X_H264, avcc, p, &out_size);
	CHECK(out != NULL, "avcC -> annexb");
	if (out) {
		CHECK(out_size == 4 + sizeof(sps1) + 4 + sizeof(pps), "avcC annexb size (%zu)", out_size);
		CHECK(h26x_has_start_code(out, out_size), "avcC annexb header");
		free(out);
	}

	/* annexb passthrough */
	size_t passthrough_size = 0;
	out = h26x_extradata_to_annexb(H26X_H264, avcc, p, &passthrough_size);
	free(out);
	(void)passthrough_size;
	uint8_t annex[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 1, 0x68, 9};
	out = h26x_extradata_to_annexb(H26X_H264, annex, sizeof(annex), &passthrough_size);
	CHECK(out && passthrough_size == sizeof(annex), "annexb passthrough (%zu)", passthrough_size);
	free(out);

	/* junk input */
	out = h26x_extradata_to_annexb(H26X_H264, (const uint8_t *)"XXXXXX", 6, &passthrough_size);
	CHECK(out == NULL, "junk avcC rejected");
}

static void test_extradata_hvcc(void)
{
	uint8_t vps[] = {0x40, 0x01, 0x0c, 0x01, 0xff};
	uint8_t sps[] = {0x42, 0x01, 0x01, 0x02, 0x20};
	uint8_t pps[] = {0x44, 0x01, 0xc0};

	uint8_t hvcc[128];
	memset(hvcc, 0, sizeof(hvcc));
	hvcc[0] = 1;
	/* profile bytes 1..12 */
	hvcc[1] = 0x01;
	hvcc[2] = 0x60;
	hvcc[3] = 0x00;
	hvcc[4] = 0x00;
	/* constraint flags 5..10 */
	/* level 11 */
	hvcc[11] = 0x3c;
	hvcc[12] = 0x00; /* level_idc part? keep simple */
	hvcc[13] = 0xf0;
	hvcc[14] = 0x00;
	hvcc[15] = 0x00;
	hvcc[16] = 0x01;
	hvcc[17] = 0x00;
	hvcc[18] = 0x00;
	hvcc[19] = 0x00;
	hvcc[20] = 0x00;
	hvcc[21] = 0x03; /* lengthSizeMinusOne=3 */
	hvcc[22] = 3;    /* three arrays */

	size_t p = 23;
	/* VPS array: type 32 */
	hvcc[p++] = 32;
	hvcc[p++] = 0;
	hvcc[p++] = 1;
	hvcc[p++] = 0;
	hvcc[p++] = (uint8_t)sizeof(vps);
	memcpy(hvcc + p, vps, sizeof(vps));
	p += sizeof(vps);
	/* SPS array: type 33 */
	hvcc[p++] = 33;
	hvcc[p++] = 0;
	hvcc[p++] = 1;
	hvcc[p++] = 0;
	hvcc[p++] = (uint8_t)sizeof(sps);
	memcpy(hvcc + p, sps, sizeof(sps));
	p += sizeof(sps);
	/* PPS array: type 34 */
	hvcc[p++] = 34;
	hvcc[p++] = 0;
	hvcc[p++] = 1;
	hvcc[p++] = 0;
	hvcc[p++] = (uint8_t)sizeof(pps);
	memcpy(hvcc + p, pps, sizeof(pps));
	p += sizeof(pps);

	size_t out_size = 0;
	uint8_t *out = h26x_extradata_to_annexb(H26X_HEVC, hvcc, p, &out_size);
	CHECK(out != NULL, "hvcC -> annexb");
	if (out) {
		size_t expected = 3 * 4 + sizeof(vps) + sizeof(sps) + sizeof(pps);
		CHECK(out_size == expected, "hvcC annexb size %zu == %zu", out_size, expected);
		CHECK(h26x_has_start_code(out, out_size), "hvcC annexb header");
		CHECK(memcmp(out + 4, vps, sizeof(vps)) == 0, "hvcC VPS first");
		free(out);
	}
}

static void test_prefix_bytes(void)
{
	/* AU: AUD(9) SPS(7) PPS(8) then IDR(5) */
	uint8_t au[] = {0, 0, 0, 1, 0x09, 0xaa, 0, 0, 1, 0x67, 0xbb, 0, 0, 1, 0x68, 0xcc, 0, 0, 0, 1, 0x65, 0xdd};
	size_t prefix = h26x_prefix_nal_bytes(H26X_H264, au, sizeof(au));
	CHECK(prefix == 16, "h264 prefix == 16 (got %zu)", prefix);

	/* AU with only a slice NAL */
	uint8_t au2[] = {0, 0, 1, 0x65, 0xee};
	prefix = h26x_prefix_nal_bytes(H26X_H264, au2, sizeof(au2));
	CHECK(prefix == 0, "h264 single-slice prefix == 0 (got %zu)", prefix);

	/* H.265: AUD(35) VPS(32) SPS(33) PPS(34) + TRAIL(1) */
	uint8_t au3[] = {0, 0,    0,    1, 0x46, 0x01, 0,    0,    1, 0x40, 0x01, 0,    0,
			 1, 0x42, 0x01, 0, 0,    1,    0x44, 0x01, 0, 0,    1,    0x02, 0x11};
	size_t p3 = h26x_prefix_nal_bytes(H26X_HEVC, au3, sizeof(au3));
	CHECK(p3 == 21, "h265 prefix == 21 (got %zu)", p3);

	/* HEVC NAL header check: type bits in high nibble (>>1) */
	uint8_t vps_nal[] = {0, 0, 1, 0x40, 0x01}; /* 0x40 >>1 == 32 */
	size_t p4 = h26x_prefix_nal_bytes(H26X_HEVC, vps_nal, sizeof(vps_nal));
	CHECK(p4 == sizeof(vps_nal), "h265 single VPS prefix == size (%zu)", p4);
}

static void test_parse_rejects_foreign(void)
{
	/* an ordinary H.264 access unit with an unrelated SEI (type 5 but a
	 * different UUID) and a slice must NOT report our SEI */
	sei_ts_frame_info_t in;
	memset(&in, 0, sizeof(in));
	in.frame_seq = 7;
	in.realtime_us = 123456789;
	uint8_t *our_nal = NULL;
	size_t our_size = 0;
	CHECK(sei_ts_build_nal(0, &in, &our_nal, &our_size), "build for reuse");

	/* craft an access unit where a *foreign* unregistered SEI comes first
	 * (other UUID) and ours second: parser scans whole AU */
	uint8_t au[256];
	size_t o = 0;
	au[o++] = 0;
	au[o++] = 0;
	au[o++] = 0;
	au[o++] = 1;
	au[o++] = 0x06; /* SEI */
	au[o++] = 5;    /* type 5 */
	au[o++] = 32;   /* size */
	/* foreign UUID 00..0f + garbage */
	memset(au + o, 0x11, 32);
	o += 32;
	au[o++] = 0x80;
	memcpy(au + o, our_nal, our_size);
	o += our_size;

	sei_ts_frame_info_t out;
	memset(&out, 0, sizeof(out));
	CHECK(sei_ts_parse_from_access_unit(0, au, o, &out), "ours found after foreign SEI");
	CHECK(out.frame_seq == 7, "correct record returned");
	free(our_nal);
}

int main(void)
{
	printf("sei-payload tests\n");
	test_nal_roundtrip(0, "h264");
	test_nal_roundtrip(1, "hevc");
	test_payload_with_zeros();
	test_extradata_avcc();
	test_extradata_hvcc();
	test_prefix_bytes();
	test_parse_rejects_foreign();

	if (failures == 0) {
		printf("\nALL TESTS PASSED\n");
		return 0;
	}
	printf("\n%d FAILURE(S)\n", failures);
	return 1;
}
