/* Actual software FFmpeg encoding: does not certify any GPU backend. */
#include "packet-pump.h"
#include "pts-timeline.h"
#include "h26x-util.h"
#include <libavutil/opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int send_frame(void *c, const AVFrame *f) { return avcodec_send_frame(c, f); }
static int receive_packet(void *c, AVPacket *p) { return avcodec_receive_packet(c, p); }
static int count, reordered;
static int64_t previous = -1;
static void check_packet(AVPacket *p, struct pts_timeline *t)
{
	struct stamp_time stamp;
	CHECK(pts_timeline_take(t, p->pts, &stamp));
	CHECK(stamp.epoch_us == 1000000 + p->pts * 1000);
	uint8_t *au; size_t size;
	CHECK(h26x_packet_to_annexb(H26X_H264, p->data, p->size, 0, &au, &size));
	CHECK(size > 0);
	free(au);
	if (p->pts < previous) reordered++;
	previous = p->pts;
	count++;
}
int main(void)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
	if (!codec) { puts("SKIP: FFmpeg has no libx264"); return 77; }
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	CHECK(ctx);
	ctx->width = 64; ctx->height = 64; ctx->pix_fmt = AV_PIX_FMT_NV12;
	ctx->time_base = (AVRational){1, 30}; ctx->framerate = (AVRational){30, 1};
	ctx->max_b_frames = 2; ctx->gop_size = 30; ctx->thread_count = 1;
	CHECK(av_opt_set(ctx->priv_data, "preset", "medium", 0) == 0);
	CHECK(av_opt_set(ctx->priv_data, "x264-params", "b-adapt=0:scenecut=0:slices=2", 0) == 0);
	CHECK(avcodec_open2(ctx, codec, NULL) == 0);
	AVFrame *frame = av_frame_alloc(); AVPacket *packet = av_packet_alloc();
	CHECK(frame && packet);
	frame->format = ctx->pix_fmt; frame->width = 64; frame->height = 64;
	CHECK(av_frame_get_buffer(frame, 32) == 0);
	struct packet_pump q = {0}; struct pts_timeline t = {0};
	const struct packet_pump_ops ops = {send_frame, receive_packet};
	int delayed = 0;
	for (int i = 0; i < 90; ++i) {
		CHECK(av_frame_make_writable(frame) == 0);
		for (int y = 0; y < 64; y++) memset(frame->data[0] + y * frame->linesize[0], 32 + i, 64);
		for (int y = 0; y < 32; y++) memset(frame->data[1] + y * frame->linesize[1], 128, 64);
		frame->pts = i;
		CHECK(pts_timeline_put(&t, (struct stamp_time){i, 1000000 + i * 1000, false}));
		CHECK(packet_pump_submit(&q, &ops, ctx, frame) == 0);
		if (packet_pump_pop(&q, packet)) check_packet(packet, &t);
		else delayed++;
	}
	CHECK(delayed > 0 && reordered > 0 && t.count > 0);
	printf("libx264 CPU NV12: submitted=90 delivered=%d pending=%zu reordered=%d delayed=%d\n",
	       count, t.count, reordered, delayed);
	/* A standalone caller CAN flush. OBS's destroy callback cannot deliver
	 * these packets. Verify the missing stop tail, not an invented OBS flush. */
	while (packet_pump_pop(&q, packet)) check_packet(packet, &t);
	CHECK(avcodec_send_frame(ctx, NULL) == 0);
	int ret;
	while ((ret = avcodec_receive_packet(ctx, packet)) == 0) check_packet(packet, &t);
	CHECK(ret == AVERROR_EOF && t.count == 0 && count == 90);
	puts("standalone explicit flush: all 90 PTS recovered; OBS stop delivery remains unsupported");
	packet_pump_clear(&q); av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&ctx);
	return 0;
}
