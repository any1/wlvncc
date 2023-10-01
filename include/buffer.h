/*
 * Copyright (c) 2022 - 2023 Andri Yngvason
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

#include "sys/queue.h"

#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <pixman.h>

struct wl_buffer;
struct gbm_bo;

enum buffer_type {
	BUFFER_UNSPEC = 0,
	BUFFER_WL_SHM,
	BUFFER_DMABUF,
};

struct buffer {
	int ref;
	int hold;

	LIST_ENTRY(buffer) registry_link;
	TAILQ_ENTRY(buffer) pool_link;

	void (*release_fn)(struct buffer*, void* ud);
	void* release_ud;

	enum buffer_type type;

	int width, height;
	int32_t scale;
	size_t size;
	uint32_t format;
	struct wl_buffer* wl_buffer;
	struct pixman_region16 damage;

	// wl_shm:
	void* pixels;
	int stride;

	// dmabuf:
	struct gbm_bo* bo;
};

struct buffer* buffer_create_shm(int width, int height, int stride, uint32_t format);
struct buffer* buffer_create_dmabuf(int width, int height, uint32_t format);
void buffer_destroy(struct buffer* self);

void buffer_ref(struct buffer*);
void buffer_unref(struct buffer*);

void buffer_set_release_fn(struct buffer*, void (*)(struct buffer*, void* ud),
		void* userdata);

void buffer_hold(struct buffer*);
void buffer_release(struct buffer*);
