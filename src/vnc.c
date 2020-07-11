#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <pixman.h>
#include <rfb/rfbclient.h>
#include <wayland-client.h>

#include "vnc.h"

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

	pixman_region_union_rect(&self->damage, &self->damage, x, y, width,
			height);
}

static void vnc_client_finish_update(rfbClient* client)
{
	struct vnc_client* self = rfbClientGetClientData(client, NULL);
	assert(self);

	self->update_fb(self);

	pixman_region_clear(&self->damage);
}

struct vnc_client* vnc_client_create(void)
{
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

	return self;

failure:
	free(self);
	return NULL;
}

void vnc_client_destroy(struct vnc_client* self)
{
	rfbClientCleanup(self->client);
	free(self);
}

int vnc_client_connect(struct vnc_client* self, const char* address, int port)
{
	rfbClient* client = self->client;

	if (!ConnectToRFBServer(client, address, port))
		return -1;

	if (!InitialiseRFBConnection(client))
		return -1;

	client->width = client->si.framebufferWidth;
	client->height = client->si.framebufferHeight;

	if (!client->MallocFrameBuffer(client))
		return -1;

	if (!SetFormatAndEncodings(client))
		return -1;

	if (client->updateRect.x < 0) {
		client->updateRect.x = client->updateRect.y = 0;
		client->updateRect.w = client->width;
		client->updateRect.h = client->height;
	}

	if (!SendFramebufferUpdateRequest(client,
				client->updateRect.x, client->updateRect.y,
				client->updateRect.w, client->updateRect.h,
				FALSE))
		return -1;

	return 0;
}

int vnc_client_set_pixel_format(struct vnc_client* self,
		enum wl_shm_format format)
{
	rfbPixelFormat* dst = &self->client->format;
	int bpp = -1;

	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_XRGB8888:
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
	return HandleRFBServerMessage(self->client) ? 0 : -1;
}

void vnc_client_send_pointer_event(struct vnc_client* self, int x, int y,
		uint32_t button_mask)
{
	SendPointerEvent(self->client, x, y, button_mask);
}

void vnc_client_send_keyboard_event(struct vnc_client* self, uint32_t symbol,
		bool is_pressed)
{
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
