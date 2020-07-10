/*
 * Copyright (c) 2020 Andri Yngvason
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
#include <stdint.h>
#include <assert.h>
#include <wayland-client.h>
#include <linux/input-event-codes.h>

#include "pointer.h"

struct pointer* pointer_new(struct wl_pointer* wl_pointer)
{
	struct pointer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->wl_pointer = wl_pointer;

	return self;
}

void pointer_destroy(struct pointer* self)
{
	wl_pointer_destroy(self->wl_pointer);
	free(self);
}

struct pointer_collection* pointer_collection_new(void)
{
	struct pointer_collection* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	wl_list_init(&self->pointers);

	return self;
}

void pointer_collection_destroy(struct pointer_collection* self)
{
	struct pointer* pointer;
	struct pointer* tmp;

	wl_list_for_each_safe(pointer, tmp, &self->pointers, link) {
		wl_list_remove(&pointer->link);
		pointer_destroy(pointer);
	}

	free(self);
}

struct pointer* pointer_collection_find_wl_pointer(
		struct pointer_collection* self, struct wl_pointer* wl_pointer)
{
	struct pointer* pointer;
	wl_list_for_each(pointer, &self->pointers, link)
		if (pointer->wl_pointer == wl_pointer)
			return pointer;

	return NULL;
}

static void pointer_enter(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, struct wl_surface* surface, wl_fixed_t x,
		wl_fixed_t y)
{
	// TODO
}

static void pointer_leave(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, struct wl_surface* surface)
{
	// TODO
}

static void pointer_motion(void* data, struct wl_pointer* wl_pointer,
		uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	pointer->x = x;
	pointer->y = y;
}

static void pointer_set_button_state(struct pointer* self,
		enum pointer_button_mask button,
		enum wl_pointer_button_state state)
{
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		self->pressed |= button;
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		self->pressed &= ~button;
	} else {
		abort();
	}
}

static void pointer_button(void* data, struct wl_pointer* wl_pointer,
		uint32_t serial, uint32_t t, uint32_t button,
		enum wl_pointer_button_state state)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	assert(pointer);

	switch (button) {
	case BTN_LEFT:
		pointer_set_button_state(pointer, POINTER_BUTTON_LEFT, state);
		break;
	case BTN_RIGHT:
		pointer_set_button_state(pointer, POINTER_BUTTON_RIGHT, state);
		break;
	case BTN_MIDDLE:
		pointer_set_button_state(pointer, POINTER_BUTTON_MIDDLE, state);
		break;

	// TODO: More buttons
	}
}

static void pointer_axis(void* data, struct wl_pointer* wl_pointer, uint32_t t,
		enum wl_pointer_axis axis, wl_fixed_t value)
{
	// TODO
}

static void pointer_axis_source(void* data, struct wl_pointer* wl_pointer,
		enum wl_pointer_axis_source source)
{
	// TODO
}

static void pointer_axis_stop(void* data, struct wl_pointer* wl_pointer,
		uint32_t t, enum wl_pointer_axis axis)
{
	// TODO
}

static void pointer_axis_discrete(void* data, struct wl_pointer* wl_pointer,
		enum wl_pointer_axis axis, int steps)
{
	// TODO
}

static void pointer_frame(void* data, struct wl_pointer* wl_pointer)
{
	struct pointer_collection* self = data;
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);

	self->on_frame(self, pointer);
}

static struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.frame = pointer_frame,
};

int pointer_collection_add_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer)
{
	struct pointer* pointer = pointer_new(wl_pointer);
	if (!pointer)
		return -1;

	wl_list_insert(&self->pointers, &pointer->link);
	wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener, self);
	return 0;
}

void pointer_collection_remove_wl_pointer(struct pointer_collection* self,
	struct wl_pointer* wl_pointer)
{
	struct pointer* pointer =
		pointer_collection_find_wl_pointer(self, wl_pointer);
	wl_list_remove(&pointer->link);
	free(pointer);
}
