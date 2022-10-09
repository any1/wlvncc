/*
 * Copyright (c) 2020 - 2022 Andri Yngvason
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

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>
#include <libavutil/frame.h>

#include "rfbclient.h"
#include "vnc.h"
#include "open-h264.h"
#include "usdt.h"

#define RFB_ENCODING_OPEN_H264 50
#define RFB_ENCODING_PTS -1000

#define NO_PTS UINT64_MAX

extern const unsigned short code_map_linux_to_qnum[];
extern const unsigned int code_map_linux_to_qnum_len;

static uint64_t vnc_client_htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(x);
#else
	return x;
#endif
}

static bool vnc_client_lock_handler(struct vnc_client* self)
{
	if (self->handler_lock)
		return false;

	self->handler_lock = true;
	return true;
}

static void vnc_client_unlock_handler(struct vnc_client* self)
{
	assert(self->handler_lock);
	self->handler_lock = false;
}

static rfbBool vnc_client_alloc_fb(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	return self->alloc_fb(self) < 0 ? FALSE : TRUE;
}

static void vnc_client_update_box(rfbClient* client, int x, int y, int width,
		int height)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (self->current_rect_is_av_frame) {
		self->current_rect_is_av_frame = false;
		return;
	}

	pixman_region_union_rect(&self->damage, &self->damage, x, y, width,
			height);
}

void vnc_client_clear_av_frames(struct vnc_client* self)
{
	for (int i = 0; i < self->n_av_frames; ++i) {
		av_frame_unref(self->av_frames[i]->frame);
		av_frame_free(&self->av_frames[i]->frame);
		free(self->av_frames[i]);
	}
	self->n_av_frames = 0;
}

static void vnc_client_start_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	self->pts = NO_PTS;
	pixman_region_clear(&self->damage);
	vnc_client_clear_av_frames(self);

	self->is_updating = true;
}

static void vnc_client_cancel_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	self->is_updating = false;
}

static void vnc_client_finish_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	DTRACE_PROBE2(wlvncc, vnc_client_finish_update, client, self->pts);

	self->is_updating = false;

	self->update_fb(self);
}

static void vnc_client_got_cut_text(rfbClient* client, const char* text,
		int len)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (self->cut_text)
		self->cut_text(self, text, len);
}

static rfbBool vnc_client_handle_open_h264_rect(rfbClient* client,
		rfbFramebufferUpdateRectHeader* rect_header)
{
	if ((int)rect_header->encoding != RFB_ENCODING_OPEN_H264)
		return FALSE;

	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	if (!self->open_h264)
		self->open_h264 = open_h264_create(client);

	if (!self->open_h264)
		return false;

	AVFrame* frame = open_h264_decode_rect(self->open_h264, rect_header);
	if (!frame)
		return false;

	assert(self->n_av_frames < VNC_CLIENT_MAX_AV_FRAMES);

	struct vnc_av_frame* f = calloc(1, sizeof(*f));
	if (!f) {
		av_frame_unref(frame);
		av_frame_free(&frame);
		return false;
	}

	f->frame = frame;
	f->x = rect_header->r.x;
	f->y = rect_header->r.y;
	f->width = rect_header->r.w;
	f->height = rect_header->r.h;

	self->av_frames[self->n_av_frames++] = f;

	self->current_rect_is_av_frame = true;
	return true;
}

static void vnc_client_init_open_h264(void)
{
	static int encodings[] = { RFB_ENCODING_OPEN_H264, 0 };
	static rfbClientProtocolExtension ext = {
		.encodings = encodings,
		.handleEncoding = vnc_client_handle_open_h264_rect,
	};
	rfbClientRegisterExtension(&ext);
}

static rfbBool vnc_client_handle_pts_rect(rfbClient* client,
		rfbFramebufferUpdateRectHeader* rect_header)
{
	if ((int)rect_header->encoding != RFB_ENCODING_PTS)
		return FALSE;

	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	uint64_t pts_msg = 0;
	if (!ReadFromRFBServer(self->client, (char*)&pts_msg, sizeof(pts_msg)))
		return FALSE;

	self->pts = vnc_client_htonll(pts_msg);

	DTRACE_PROBE1(wlvncc, vnc_client_handle_pts_rect, self->pts);

	return TRUE;
}

static void vnc_client_init_pts_ext(void)
{
	static int encodings[] = { RFB_ENCODING_PTS, 0 };
	static rfbClientProtocolExtension ext = {
		.encodings = encodings,
		.handleEncoding = vnc_client_handle_pts_rect,
	};
	rfbClientRegisterExtension(&ext);
}

struct vnc_client* vnc_client_create(void)
{
	vnc_client_init_open_h264();
	vnc_client_init_pts_ext();

	struct vnc_client* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	/* These are defaults that can be changed with
	 * vnc_client_set_pixel_format().
	 */
	int bits_per_sample = 8;
	int samples_per_pixel = 3;
	int bytes_per_pixel = 4;

	rfbClient* client = rfbGetClient(bits_per_sample, samples_per_pixel,
			bytes_per_pixel);
	if (!client)
		goto failure;

	self->client = client;
	rfbClientSetClientData(client, NULL, self);

	client->MallocFrameBuffer = vnc_client_alloc_fb;
	client->GotFrameBufferUpdate = vnc_client_update_box;
	client->FinishedFrameBufferUpdate = vnc_client_finish_update;
	client->StartingFrameBufferUpdate = vnc_client_start_update;
	client->CancelledFrameBufferUpdate = vnc_client_cancel_update;
	client->GotXCutText = vnc_client_got_cut_text;

	self->pts = NO_PTS;

	return self;

failure:
	free(self);
	return NULL;
}

void vnc_client_destroy(struct vnc_client* self)
{
	vnc_client_clear_av_frames(self);
	open_h264_destroy(self->open_h264);
	rfbClientCleanup(self->client);
	free(self);
}

int vnc_client_connect(struct vnc_client* self, const char* address, int port)
{
	rfbClient* client = self->client;

	return ConnectToRFBServer(client, address, port) ? 0 : -1;
}

int vnc_client_init(struct vnc_client* self)
{
	int rc = -1;
	rfbClient* client = self->client;

	vnc_client_lock_handler(self);

	if (!InitialiseRFBConnection(client))
		goto failure;

	client->width = client->si.framebufferWidth;
	client->height = client->si.framebufferHeight;

	if (!client->MallocFrameBuffer(client))
		goto failure;

	if (!SetFormatAndEncodings(client))
		goto failure;

	if (client->updateRect.x < 0) {
		client->updateRect.x = client->updateRect.y = 0;
		client->updateRect.w = client->width;
		client->updateRect.h = client->height;
	}

	if (!SendFramebufferUpdateRequest(client,
				client->updateRect.x, client->updateRect.y,
				client->updateRect.w, client->updateRect.h,
				FALSE))
		goto failure;

	SendIncrementalFramebufferUpdateRequest(client);
	SendIncrementalFramebufferUpdateRequest(client);

	rc = 0;
failure:
	vnc_client_unlock_handler(self);
	return rc;
}

int vnc_client_set_pixel_format(struct vnc_client* self, uint32_t format)
{
	rfbPixelFormat* dst = &self->client->format;
	int bpp = -1;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		dst->redShift = 16;
		dst->greenShift = 8;
		dst->blueShift = 0;
		bpp = 32;
		break;
	default:
		return -1;
	}

	switch (bpp) {
	case 32:
		dst->bitsPerPixel = 32;
		dst->depth = 24;
		dst->redMax = 0xff;
		dst->greenMax = 0xff;
		dst->blueMax = 0xff;
		break;
	default:
		abort();
	}

	dst->trueColour = 1;
	dst->bigEndian = FALSE;
	self->client->appData.requestedDepth = dst->depth;

	return 0;
}

int vnc_client_get_width(const struct vnc_client* self)
{
	return self->client->width;
}

int vnc_client_get_height(const struct vnc_client* self)
{
	return self->client->height;
}

int vnc_client_get_stride(const struct vnc_client* self)
{
	// TODO: What happens if bitsPerPixel == 24?
	return self->client->width * self->client->format.bitsPerPixel / 8;
}

void* vnc_client_get_fb(const struct vnc_client* self)
{
	return self->client->frameBuffer;
}

void vnc_client_set_fb(struct vnc_client* self, void* fb)
{
	self->client->frameBuffer = fb;
}

int vnc_client_get_fd(const struct vnc_client* self)
{
	return self->client->sock;
}

const char* vnc_client_get_desktop_name(const struct vnc_client* self)
{
	return self->client->desktopName;
}

int vnc_client_process(struct vnc_client* self)
{
	if (!ReadToBuffer(self->client))
		return -1;

	if (!vnc_client_lock_handler(self))
		return 0;

	int rc;
	while (self->client->buffered > 0) {
		rc = HandleRFBServerMessage(self->client) ? 0 : -1;
		if (rc < 0)
			break;
	}

	vnc_client_unlock_handler(self);
	return rc;
}

void vnc_client_send_pointer_event(struct vnc_client* self, int x, int y,
		uint32_t button_mask)
{
	SendPointerEvent(self->client, x, y, button_mask);
}

void vnc_client_send_keyboard_event(struct vnc_client* self, uint32_t symbol,
		uint32_t code, bool is_pressed)
{
	if (code >= code_map_linux_to_qnum_len)
		return;

	uint32_t qnum = code_map_linux_to_qnum[code];
	if (!qnum)
		qnum = code;

	if (!SendExtendedKeyEvent(self->client, symbol, qnum, is_pressed))
		SendKeyEvent(self->client, symbol, is_pressed);
}

void vnc_client_set_encodings(struct vnc_client* self, const char* encodings)
{
	self->client->appData.encodingsString = encodings;
}

void vnc_client_set_quality_level(struct vnc_client* self, int value)
{
	self->client->appData.qualityLevel = value;
}

void vnc_client_set_compression_level(struct vnc_client* self, int value)
{
	self->client->appData.compressLevel = value;
}

void vnc_client_send_cut_text(struct vnc_client* self, const char* text,
		size_t len)
{
	// libvncclient doesn't modify text, so typecast is OK.
	SendClientCutText(self->client, (char*)text, len);
}
