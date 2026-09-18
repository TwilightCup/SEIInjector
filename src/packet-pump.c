#include "packet-pump.h"
#include <libavutil/error.h>

static int drain(struct packet_pump *q, const struct packet_pump_ops *ops, void *ctx)
{
	for (;;) {
		AVPacket *p = av_packet_alloc();
		if (!p)
			return AVERROR(ENOMEM);
		int ret = ops->receive(ctx, p);
		if (ret < 0) {
			av_packet_free(&p);
			return ret == AVERROR(EAGAIN) ? 0 : ret;
		}
		/* Fail visibly rather than silently losing output or growing forever. */
		if (p->size <= 0 || q->count == PACKET_PUMP_CAPACITY ||
		    (size_t)p->size > PACKET_PUMP_MAX_BYTES - q->bytes) {
			av_packet_free(&p);
			return AVERROR(ENOBUFS);
		}
		q->packets[(q->head + q->count) % PACKET_PUMP_CAPACITY] = p;
		q->count++;
		q->bytes += (size_t)p->size;
	}
}

int packet_pump_submit(struct packet_pump *q, const struct packet_pump_ops *ops, void *ctx, const AVFrame *frame)
{
	int ret = ops->send(ctx, frame);
	if (ret == AVERROR(EAGAIN)) {
		/* FFmpeg guarantees progress on receive when send returns EAGAIN.
		 * Retry the SAME still-owned frame only after draining previous output. */
		ret = drain(q, ops, ctx);
		if (ret < 0)
			return ret;
		ret = ops->send(ctx, frame);
	}
	if (ret < 0)
		return ret;
	return drain(q, ops, ctx);
}

bool packet_pump_pop(struct packet_pump *q, AVPacket *p)
{
	if (!q->count)
		return false;
	AVPacket *front = q->packets[q->head];
	q->bytes -= (size_t)front->size;
	av_packet_unref(p);
	av_packet_move_ref(p, front);
	av_packet_free(&front);
	q->packets[q->head] = NULL;
	q->head = (q->head + 1) % PACKET_PUMP_CAPACITY;
	q->count--;
	return true;
}

void packet_pump_clear(struct packet_pump *q)
{
	for (size_t i = 0; i < q->count; ++i)
		av_packet_free(&q->packets[(q->head + i) % PACKET_PUMP_CAPACITY]);
	*q = (struct packet_pump){0};
}
