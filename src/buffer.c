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
#include "linux-dmabuf-unstable-v1.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <gbm.h>
#include <assert.h>

/* Origin: main.c */
extern struct wl_shm* wl_shm;
extern struct gbm_device* gbm_device;
extern struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf_v1;

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

struct buffer* buffer_create_shm(int width, int height, int stride,
		uint32_t format)
{
	assert(wl_shm);

	struct buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = BUFFER_WL_SHM;
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

struct buffer* buffer_create_dmabuf(int width, int height, uint32_t format)
{
	assert(gbm_device && zwp_linux_dmabuf_v1);

	struct buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->type = BUFFER_DMABUF;
	self->width = width;
	self->height = height;
	self->format = format;

	pixman_region_init_rect(&self->damage, 0, 0, width, height);

	self->bo = gbm_bo_create(gbm_device, width, height, format,
			GBM_BO_USE_RENDERING);
	if (!self->bo)
		goto bo_failure;

	struct zwp_linux_buffer_params_v1* params;
	params = zwp_linux_dmabuf_v1_create_params(zwp_linux_dmabuf_v1);
	if (!params)
		goto params_failure;

	uint32_t offset = gbm_bo_get_offset(self->bo, 0);
	uint32_t stride = gbm_bo_get_stride(self->bo);
	uint64_t mod = gbm_bo_get_modifier(self->bo);
	int fd = gbm_bo_get_fd(self->bo);
	if (fd < 0)
		goto fd_failure;

	zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
			mod >> 32, mod & 0xffffffff);
	self->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, width,
			height, format, /* flags */ 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);

	if (!self->wl_buffer)
		goto buffer_failure;

	wl_buffer_add_listener(self->wl_buffer, &buffer_listener, self);

	return self;

buffer_failure:
fd_failure:
	zwp_linux_buffer_params_v1_destroy(params);
params_failure:
	gbm_bo_destroy(self->bo);
bo_failure:
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

	switch (self->type) {
	case BUFFER_WL_SHM:
		munmap(self->pixels, self->size);
		break;
	case BUFFER_DMABUF:
		gbm_bo_destroy(self->bo);
		break;
	default:
		abort();
		break;
	}

	free(self);
}
