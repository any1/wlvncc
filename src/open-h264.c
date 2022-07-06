/*
 * Copyright (c) 2022 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "open-h264.h"
#include "rfb/rfbclient.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavcodec/avcodec.h>

enum open_h264_flags {
	OPEN_H264_RESET_CONTEXT = 1 << 0,
	OPEN_H264_RESET_ALL_CONTEXTS = 1 << 1,
};

struct open_h264_msg_head {
	uint32_t length;
	uint32_t flags;
} __attribute__((packed));

struct open_h264_context {
	rfbRectangle rect;

	AVCodecParserContext* parser;
	AVCodecContext* codec_ctx;
	AVBufferRef* hwctx_ref;
};

struct open_h264 {
	rfbClient* client;

	struct open_h264_context* contexts[OPEN_H264_MAX_CONTEXTS];
	int n_contexts;
};

static bool are_rects_equal(const rfbRectangle* a, const rfbRectangle* b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static int find_context_index(const struct open_h264* self,
		const rfbRectangle* rect)
{
	for (int i = 0; i < self->n_contexts; ++i)
		if (are_rects_equal(&self->contexts[i]->rect, rect))
			return i;
	return -1;
}

static struct open_h264_context* find_context(
		const struct open_h264* self, const rfbRectangle* rect)
{
	int i = find_context_index(self, rect);
	return i >= 0 ? self->contexts[i] : NULL;
}

static struct open_h264_context* open_h264_context_create(
		struct open_h264* self, const rfbRectangle* rect)
{
	if (self->n_contexts >= OPEN_H264_MAX_CONTEXTS)
		return NULL;

	struct open_h264_context* context = calloc(1, sizeof(*context));
	if (!context)
		return NULL;

	memcpy(&context->rect, rect, sizeof(context->rect));

	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec)
		goto failure;

	context->parser = av_parser_init(codec->id);
	if (!context->parser)
		goto failure;

	context->codec_ctx = avcodec_alloc_context3(codec);
	if (!context->codec_ctx)
		goto failure;

	if (av_hwdevice_ctx_create(&context->hwctx_ref, AV_HWDEVICE_TYPE_VAAPI,
				NULL, NULL, 0) != 0)
		goto failure;

	context->codec_ctx->hw_device_ctx = av_buffer_ref(context->hwctx_ref);

	if (avcodec_open2(context->codec_ctx, codec, NULL) != 0)
		goto failure;

	self->contexts[self->n_contexts++] = context;
	return context;

failure:
	av_buffer_unref(&context->hwctx_ref);
	avcodec_free_context(&context->codec_ctx);
	av_parser_close(context->parser);
	free(context);
	return NULL;
}

static void open_h264_context_destroy(struct open_h264_context* context)
{
	av_buffer_unref(&context->hwctx_ref);
	avcodec_free_context(&context->codec_ctx);
	av_parser_close(context->parser);
	free(context);
}

static struct open_h264_context* get_context(struct open_h264* self,
		const rfbRectangle* rect)
{
	struct open_h264_context* context = find_context(self, rect);
	return context ? context : open_h264_context_create(self, rect);
}


static void reset_context(struct open_h264* self,
		const rfbRectangle* rect)
{
	int i = find_context_index(self, rect);
	if (i < 0)
		return;

	open_h264_context_destroy(self->contexts[i]);
	--self->n_contexts;

	memmove(&self->contexts[i], &self->contexts[i + 1],
			self->n_contexts - i);
}

static void reset_all_contexts(struct open_h264* self)
{
	for (int i = 0; i < self->n_contexts; ++i) {
		open_h264_context_destroy(self->contexts[i]);
		self->contexts[i] = NULL;
	}
}

struct open_h264* open_h264_create(rfbClient* client)
{
	struct open_h264* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->client = client;

	return self;
}

void open_h264_destroy(struct open_h264* self)
{
	if (!self)
		return;

	reset_all_contexts(self);
	free(self);
}

static bool decode_frame(struct open_h264_context* context, AVFrame* frame,
		AVPacket* packet)
{
	av_frame_unref(frame);

	int rc;

	rc = avcodec_send_packet(context->codec_ctx, packet);
	if (rc < 0)
		return false;

	struct AVFrame* vaapi_frame = av_frame_alloc();
	if (!vaapi_frame)
		return false;

	rc = avcodec_receive_frame(context->codec_ctx, vaapi_frame);
	if (rc < 0)
		return false;

	frame->format = AV_PIX_FMT_DRM_PRIME;

	rc = av_hwframe_map(frame, vaapi_frame, AV_HWFRAME_MAP_DIRECT);
	if (rc < 0) {
		av_frame_free(&vaapi_frame);
		return false;
	}

	av_frame_copy_props(frame, vaapi_frame);
	av_frame_free(&vaapi_frame);

	return true;
}

static int parse_elementary_stream(struct open_h264_context* context,
		AVPacket* packet, const uint8_t* src, uint32_t length)
{
	return av_parser_parse2(context->parser, context->codec_ctx,
			&packet->data, &packet->size, src, length,
			AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
}

struct AVFrame* open_h264_decode_rect(struct open_h264* self,
		rfbFramebufferUpdateRectHeader* msg)
{
	bool have_frame = false;

	struct open_h264_msg_head head = { 0 };
	if (!ReadFromRFBServer(self->client, (char*)&head, sizeof(head)))
		return NULL;

	uint32_t length = ntohl(head.length);
	enum open_h264_flags flags = ntohl(head.flags);

	if (flags & OPEN_H264_RESET_ALL_CONTEXTS) {
		reset_all_contexts(self);
	} else if (flags & OPEN_H264_RESET_CONTEXT) {
		reset_context(self, &msg->r);
	}

	struct open_h264_context* context = get_context(self, &msg->r);
	if (!context)
		return NULL;

	char* data = calloc(1, length + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!data)
		return NULL;

	AVFrame* frame = av_frame_alloc();
	if (!frame)
		goto failure;

	AVPacket* packet = av_packet_alloc();
	if (!packet)
		goto failure;

	if (!ReadFromRFBServer(self->client, data, length))
		goto failure;

	uint8_t* dp = (uint8_t*)data;

	while (length > 0) {
		int rc = parse_elementary_stream(context, packet, dp, length);
		if (rc < 0)
			goto failure;

		dp += rc;
		length -= rc;

		// The h264 elementary stream doesn't have end-markers, so the
		// parser doesn't know where the frame ends. This flushes it:
		if (packet->size == 0 && length == 0) {
			int rc = parse_elementary_stream(context, packet, dp,
					length);
			if (rc < 0)
				goto failure;
		}

		// If we get multiple frames per rect, there's no point in
		// rendering them all, so we just return the last one.
		if (packet->size != 0)
			have_frame = decode_frame(context, frame, packet);
	}

failure:
	av_packet_free(&packet);
	if (!have_frame)
		av_frame_unref(frame);
	free(data);
	return have_frame ? frame : NULL;
}
