/*
 * Copyright (c) 2026
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#pragma once

#include <stddef.h>
#include <wayland-client.h>

struct clipboard;
struct seat;
struct ext_data_control_manager_v1;

typedef void (*clipboard_local_selection_fn)(void* userdata,
		const char* text, size_t len);

struct clipboard* clipboard_new(struct wl_display* display,
		struct wl_data_device_manager* manager,
		struct ext_data_control_manager_v1* ext_manager,
		struct seat* seat,
		clipboard_local_selection_fn on_local_selection, void* userdata);
void clipboard_destroy(struct clipboard* self);
void clipboard_set_selection_from_rfb(struct clipboard* self,
		const char* text, size_t len);
