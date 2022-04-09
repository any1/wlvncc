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

#pragma once

#include <stdbool.h>
#include <unistd.h>
#include <pixman.h>
#include <wayland-client.h>
#include <rfb/rfbclient.h>

#define VNC_CLIENT_MAX_AV_FRAMES 64

struct open_h264;
struct AVFrame;

struct vnc_av_frame {
	struct AVFrame* frame;
	int x, y, width, height;
};

struct vnc_client {
	rfbClient* client;

	struct open_h264* open_h264;
	bool current_rect_is_av_frame;
	struct vnc_av_frame* av_frames[VNC_CLIENT_MAX_AV_FRAMES];
	int n_av_frames;

	int (*alloc_fb)(struct vnc_client*);
	void (*update_fb)(struct vnc_client*);
	void (*cut_text)(struct vnc_client*, const char*, size_t);

	void* userdata;
	struct pixman_region16 damage;
};

struct vnc_client* vnc_client_create(void);
void vnc_client_destroy(struct vnc_client* self);

int vnc_client_connect(struct vnc_client* self, const char* address, int port);

int vnc_client_set_pixel_format(struct vnc_client* self, uint32_t format);

int vnc_client_get_fd(const struct vnc_client* self);
int vnc_client_get_width(const struct vnc_client* self);
int vnc_client_get_height(const struct vnc_client* self);
int vnc_client_get_stride(const struct vnc_client* self);
void* vnc_client_get_fb(const struct vnc_client* self);
void vnc_client_set_fb(struct vnc_client* self, void* fb);
const char* vnc_client_get_desktop_name(const struct vnc_client* self);
int vnc_client_process(struct vnc_client* self);
void vnc_client_send_pointer_event(struct vnc_client* self, int x, int y,
		uint32_t button_mask);
void vnc_client_send_keyboard_event(struct vnc_client* self, uint32_t symbol,
		uint32_t code, bool is_pressed);
void vnc_client_set_encodings(struct vnc_client* self, const char* encodings);
void vnc_client_set_quality_level(struct vnc_client* self, int value);
void vnc_client_set_compression_level(struct vnc_client* self, int value);
void vnc_client_send_cut_text(struct vnc_client* self, const char* text,
		size_t len);
