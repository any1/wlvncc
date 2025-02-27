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

#pragma once

#include <wayland-client.h>

struct wl_cursor_theme;

enum pointer_cursor_type {
	POINTER_CURSOR_NONE = 0,
	POINTER_CURSOR_LEFT_PTR,
};

enum pointer_button_mask {
	POINTER_BUTTON_LEFT = 1 << 0,
	POINTER_BUTTON_MIDDLE = 1 << 1,
	POINTER_BUTTON_RIGHT = 1 << 2,
	// TODO: More buttons
	POINTER_SCROLL_UP = 1 << 3,
	POINTER_SCROLL_DOWN = 1 << 4,
	POINTER_SCROLL_LEFT = 1 << 5,
	POINTER_SCROLL_RIGHT = 1 << 6,
};

struct pointer {
	struct wl_pointer* wl_pointer;
	struct wl_list link;

	struct seat* seat;

	uint32_t serial;
	bool handle_event;
	enum pointer_button_mask pressed;
	wl_fixed_t x, y;

	int vertical_scroll_steps;
	int horizontal_scroll_steps;

	double vertical_axis_value;
	double horizontal_axis_value;

	struct wl_cursor_theme* cursor_theme;
	struct wl_surface* cursor_surface;

	enum pointer_cursor_type cursor_type;
};

struct pointer_collection {
	struct wl_list pointers;
	void (*on_frame)(struct pointer_collection*, struct pointer*);
	bool (*handle_event)(struct wl_surface*);
	enum pointer_cursor_type cursor_type;
	void* userdata;
};

struct pointer_collection* pointer_collection_new(enum pointer_cursor_type);
void pointer_collection_destroy(struct pointer_collection*);

int pointer_collection_add_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer, struct seat*);
void pointer_collection_remove_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer);
