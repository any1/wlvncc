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

#include "buffer.h"
#include "shm.h"
#include "pixels.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <wayland-client.h>

extern struct wl_shm* wl_shm; /* Origin: main.c */

static void buffer_release(void* data, struct wl_buffer* wl_buffer)
{
	(void)wl_buffer;
	struct buffer* self = data;
	self->is_attached = false;

	if (self->please_clean_up) {
		buffer_destroy(self);
	}
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

struct buffer* buffer_create(int width, int height, int stride, uint32_t format)
{
	struct buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = format;

	pixman_region_init_rect(&self->damage, 0, 0, width, height);

	self->size = height * stride;
	int fd = shm_alloc_fd(self->size);
	if (fd < 0)
		goto failure;

	self->pixels = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (!self->pixels)
		goto mmap_failure;

	struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, self->size);
	if (!pool)
		goto pool_failure;

	self->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
			stride, drm_format_to_wl_shm(format));
	wl_shm_pool_destroy(pool);
	if (!self->wl_buffer)
		goto shm_failure;

	close(fd);

	wl_buffer_add_listener(self->wl_buffer, &buffer_listener, self);

	return self;

shm_failure:
pool_failure:
	munmap(self->pixels, self->size);
mmap_failure:
	close(fd);
failure:
	free(self);
	return NULL;
}

void buffer_destroy(struct buffer* self)
{
	if (!self)
		return;

	if (self->is_attached)
		self->please_clean_up = true;

	wl_buffer_destroy(self->wl_buffer);
	munmap(self->pixels, self->size);
	free(self);
}
