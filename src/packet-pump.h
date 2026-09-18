/* Drain FFmpeg completely while preserving OBS's one-packet callback contract. */
#ifndef PACKET_PUMP_H
#define PACKET_PUMP_H
#include <libavcodec/avcodec.h>
#include <stdbool.h>
#include <stddef.h>
#define PACKET_PUMP_CAPACITY 64u
#define PACKET_PUMP_MAX_BYTES (64u * 1024u * 1024u)
struct packet_pump {
	AVPacket *packets[PACKET_PUMP_CAPACITY];
	size_t head, count, bytes;
};
/* Injected operations also permit deterministic EAGAIN tests without a GPU. */
struct packet_pump_ops {
	int (*send)(void *, const AVFrame *);
	int (*receive)(void *, AVPacket *);
};
int packet_pump_submit(struct packet_pump *q, const struct packet_pump_ops *ops, void *ctx, const AVFrame *frame);
bool packet_pump_pop(struct packet_pump *q, AVPacket *packet);
void packet_pump_clear(struct packet_pump *q);
#endif
