#include "pts-timeline.h"
#include "packet-pump.h"
#include "h26x-util.h"
#include "sei-payload.h"
#include <libavutil/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void test_timeline(void)
{
	struct pts_timeline t = {0};
	struct stamp_time e;
	int order[] = {0, 3, 1, 2}; /* coded order of I P B B */
	for (int i = 0; i < 4; ++i)
		REQUIRE(pts_timeline_put(&t, (struct stamp_time){i, 1000000 + i * 1000, i >= 2}));
	REQUIRE(!pts_timeline_put(&t, (struct stamp_time){1, 7, false}));
	REQUIRE(!pts_timeline_take(&t, 99, &e));
	REQUIRE(t.count == 4);
	for (int i = 0; i < 4; ++i) {
		REQUIRE(pts_timeline_take(&t, order[i], &e));
		REQUIRE(e.epoch_us == 1000000 + order[i] * 1000 && e.ntp == (order[i] >= 2));
		REQUIRE(!pts_timeline_take(&t, order[i], &e));
	}
	for (size_t i = 0; i < STAMP_PTS_CAPACITY; ++i)
		REQUIRE(pts_timeline_put(&t, (struct stamp_time){(int64_t)i, (int64_t)i + 50, false}));
	REQUIRE(!pts_timeline_put(&t, (struct stamp_time){99999, 1, false}));
	REQUIRE(pts_timeline_take(&t, 0, &e) && e.epoch_us == 50); /* no silent eviction */
	REQUIRE(pts_timeline_put(&t, (struct stamp_time){99999, 1, true}));
}

struct fake_codec {
	int sends, again, count, pos, add_on_send, receive_error;
	const AVFrame *first;
};
static int fake_send(void *opaque, const AVFrame *frame)
{
	struct fake_codec *f = opaque;
	++f->sends;
	if (!f->first)
		f->first = frame;
	REQUIRE(frame == f->first);
	if (f->again < 0 || (f->again && f->sends == 1))
		return AVERROR(EAGAIN);
	f->count += f->add_on_send;
	return 0;
}
static int fake_receive(void *opaque, AVPacket *p)
{
	struct fake_codec *f = opaque;
	if (f->receive_error)
		return f->receive_error;
	if (f->pos == f->count)
		return AVERROR(EAGAIN);
	REQUIRE(av_new_packet(p, 2) == 0);
	p->pts = f->pos++;
	p->data[0] = (uint8_t)p->pts;
	return 0;
}
static void test_pump(void)
{
	const struct packet_pump_ops ops = {fake_send, fake_receive};
	struct packet_pump q = {0};
	AVFrame *frame = av_frame_alloc();
	AVPacket *p = av_packet_alloc();
	REQUIRE(frame && p);
	/* send EAGAIN -> drain two OLD packets -> retry same frame -> one NEW. */
	struct fake_codec f = {.again = 1, .count = 2, .add_on_send = 1};
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == 0);
	REQUIRE(f.sends == 2 && q.count == 3);
	for (int i = 0; i < 3; ++i) {
		REQUIRE(packet_pump_pop(&q, p));
		REQUIRE(p->pts == i && p->data[0] == i);
	}
	REQUIRE(!packet_pump_pop(&q, p) && q.bytes == 0);
	/* Delayed encoder: accepting a frame without output is normal. */
	f = (struct fake_codec){0};
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == 0 && q.count == 0);
	f.add_on_send = 4;
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == 0 && q.count == 4);
	REQUIRE(packet_pump_pop(&q, p) && q.count == 3);
	packet_pump_clear(&q); /* release undeliverable stop tail, never pretend delivered */
	REQUIRE(q.count == 0 && q.bytes == 0);
	f = (struct fake_codec){.add_on_send = PACKET_PUMP_CAPACITY + 1};
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == AVERROR(ENOBUFS));
	REQUIRE(q.count == PACKET_PUMP_CAPACITY);
	packet_pump_clear(&q);
	f = (struct fake_codec){.receive_error = AVERROR(EIO)};
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == AVERROR(EIO));
	f = (struct fake_codec){.again = -1};
	REQUIRE(packet_pump_submit(&q, &ops, &f, frame) == AVERROR(EAGAIN));
	REQUIRE(f.sends == 2); /* broken no-progress backend must not spin */
	av_frame_free(&frame);
	av_packet_free(&p);
}

static void test_packets(void)
{
	/* Two slices plus parameter sets, both 3-byte and 4-byte start codes. */
	const uint8_t h264[] = {0,0,0,1,0x67,0x42, 0,0,1,0x68,0xce,
		0,0,0,1,0x65,0x80,0xaa, 0,0,1,0x65,0x40,0xbb};
	const uint8_t hevc[] = {0,0,0,1,0x40,1,0x11, 0,0,1,0x42,1,0x22,
		0,0,1,0x26,1,0x80,0xaa, 0,0,0,1,0x26,1,0x40,0xbb};
	const uint8_t *packets[] = {h264, hevc};
	size_t sizes[] = {sizeof(h264), sizeof(hevc)};
	for (int codec = 0; codec < 2; ++codec) {
		uint8_t *out = NULL;
		size_t n = 0;
		REQUIRE(h26x_packet_to_annexb(codec, packets[codec], sizes[codec], 0, &out, &n));
		REQUIRE(n == sizes[codec] && memcmp(out, packets[codec], n) == 0);
		free(out);
		/* All declared NAL-length widths; two slices in one packet. */
		for (unsigned width = 1; width <= 4; ++width) {
			uint8_t in[64] = {0};
			size_t pos = 0;
			/* A parameter-set NAL before the two VCL NALs. */
			in[width - 1] = 3;
			pos = width;
			in[pos++] = codec ? 0x42 : 0x67;
			in[pos++] = codec ? 1 : 0x42;
			in[pos++] = 0xaa;
			for (int slice = 0; slice < 2; ++slice) {
				in[pos + width - 1] = 4;
				pos += width;
				in[pos++] = codec ? 0x26 : 0x65;
				in[pos++] = codec ? 1 : (slice ? 0x40 : 0x80);
				in[pos++] = slice ? 0x40 : 0x80;
				in[pos++] = 0xaa;
			}
			REQUIRE(h26x_packet_to_annexb(codec, in, pos, width, &out, &n));
			REQUIRE(n == 23 && out[3] == 1 && out[10] == 1 && out[18] == 1);
			size_t prefix = h26x_prefix_nal_bytes(codec, out, n);
			REQUIRE(prefix == 7);
			/* Converted packet can carry our SEI without losing either slice. */
			sei_ts_frame_info_t stamp = {.media_pts = 42, .realtime_us = 1000};
			uint8_t *sei; size_t sn;
			REQUIRE(sei_ts_build_nal(codec, &stamp, &sei, &sn));
			uint8_t *au = malloc(sn + n);
			REQUIRE(au);
			memcpy(au, out, prefix); memcpy(au + prefix, sei, sn);
			memcpy(au + prefix + sn, out + prefix, n - prefix);
			sei_ts_frame_info_t parsed;
			REQUIRE(sei_ts_parse_from_access_unit(codec, au, sn + n, &parsed));
			REQUIRE(parsed.media_pts == 42 && memcmp(au, out, prefix) == 0);
			REQUIRE(memcmp(au + prefix + sn, out + prefix, n - prefix) == 0);
			free(au); free(sei); free(out);
			REQUIRE(!h26x_packet_to_annexb(codec, in, pos - 1, width, &out, &n));
			REQUIRE(!h26x_packet_to_annexb(codec, in, pos, 0, &out, &n));
		}
	}
	uint8_t avcc[7] = {1,0,0,0,0xff};
	uint8_t hvcc[23] = {1}; hvcc[21] = 0xfd;
	REQUIRE(h26x_extradata_length_size(H26X_H264, avcc, sizeof(avcc)) == 4);
	REQUIRE(h26x_extradata_length_size(H26X_HEVC, hvcc, sizeof(hvcc)) == 2);
	REQUIRE(h26x_extradata_length_size(H26X_H264, NULL, 0) == 0);
	const uint8_t headers[] = {0,0,1,0x67,0xaa};
	uint8_t *out; size_t n;
	REQUIRE(!h26x_packet_to_annexb(H26X_H264, headers, sizeof(headers), 0, &out, &n));
	const uint8_t bad[] = {0,0,0,8,0x65,0x80};
	REQUIRE(!h26x_packet_to_annexb(H26X_H264, bad, sizeof(bad), 4, &out, &n));
	/* 357-byte NAL length also looks like a 3-byte Annex-B start code.
	 * Reject both declared and undeclared ambiguous interpretations. */
	uint8_t ambiguous[361];
	memset(ambiguous, 0xaa, sizeof(ambiguous));
	ambiguous[0] = 0; ambiguous[1] = 0; ambiguous[2] = 1;
	ambiguous[3] = 0x65; ambiguous[4] = 0x65;
	REQUIRE(!h26x_packet_to_annexb(H26X_H264, ambiguous, sizeof(ambiguous), 4, &out, &n));
	REQUIRE(!h26x_packet_to_annexb(H26X_H264, ambiguous, sizeof(ambiguous), 0, &out, &n));
}
int main(void)
{
	test_timeline(); test_pump(); test_packets();
	puts("reliability: timeline, EAGAIN/drain/queue, H.264/HEVC packet conversion PASS");
	return 0;
}
