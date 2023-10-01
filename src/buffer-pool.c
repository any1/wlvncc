/*
 * Copyright (c) 2023 Andri Yngvason
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

#include "buffer-pool.h"
#include "buffer.h"

#include "sys/queue.h"

#include <stdlib.h>
#include <assert.h>

LIST_HEAD(buffer_list, buffer);
TAILQ_HEAD(bufferq, buffer);

struct buffer_pool {
	int ref;

	struct buffer_list registry;
	struct bufferq buffers;

	enum buffer_type type;
	uint16_t width;
	uint16_t height;
	int32_t stride;
	uint32_t format;
	int scale;
};

struct buffer_pool* buffer_pool_create(enum buffer_type type, uint16_t width,
		uint16_t height, uint32_t format, uint16_t stride, int scale)
{
	struct buffer_pool* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->ref = 1;

	LIST_INIT(&self->registry);
	TAILQ_INIT(&self->buffers);
	self->type = type;
	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = format;
	self->scale = scale;

	return self;
}

static void buffer_pool__unref_buffers(struct buffer_pool* self)
{
	while (!LIST_EMPTY(&self->registry)) {
		struct buffer* buffer = LIST_FIRST(&self->registry);
		LIST_REMOVE(buffer, registry_link);
		buffer_set_release_fn(buffer, NULL, NULL);
		buffer_unref(buffer);
	}
	TAILQ_INIT(&self->buffers);
}

void buffer_pool_destroy(struct buffer_pool* self)
{
	buffer_pool__unref_buffers(self);
	free(self);
}

bool buffer_pool_resize(struct buffer_pool* self, uint16_t width,
		uint16_t height, uint32_t format, uint16_t stride, int scale)
{
	if (width == self->width && height == self->height &&
			format == self->format && stride == self->stride &&
			scale == self->scale)
		return false;

	buffer_pool__unref_buffers(self);

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = format;
	self->scale = scale;

	return true;
}

static void buffer_pool__on_buffer_release(struct buffer* buffer, void* userdata)
{
	struct buffer_pool* self = userdata;

	if (buffer->width != self->width || buffer->height != self->height ||
			buffer->format != self->format ||
			(buffer->type == BUFFER_WL_SHM && buffer->stride != self->stride) ||
			buffer->scale != self->scale) {
		buffer_unref(buffer);
	} else {
		TAILQ_INSERT_TAIL(&self->buffers, buffer, pool_link);
	}
}

static struct buffer* buffer_pool__acquire_new(struct buffer_pool* self)
{
	struct buffer* buffer;
	switch (self->type) {
	case BUFFER_WL_SHM:
		buffer = buffer_create_shm(self->width, self->height,
				self->stride, self->format);
		break;
	case BUFFER_DMABUF:
		buffer = buffer_create_dmabuf(self->width, self->height,
				self->format);
		break;
	case BUFFER_UNSPEC:
		abort();
	}
	if (!buffer)
		return NULL;

	buffer->scale = self->scale;

	LIST_INSERT_HEAD(&self->registry, buffer, registry_link);
	buffer_set_release_fn(buffer, buffer_pool__on_buffer_release, self);

	return buffer;
}

static struct buffer* buffer_pool__acquire_from_list(struct buffer_pool* self)
{
	struct buffer* buffer = TAILQ_FIRST(&self->buffers);
	assert(buffer);

	TAILQ_REMOVE(&self->buffers, buffer, pool_link);

	return buffer;
}

struct buffer* buffer_pool_acquire(struct buffer_pool* self)
{
	return TAILQ_EMPTY(&self->buffers) ?
		buffer_pool__acquire_new(self) :
		buffer_pool__acquire_from_list(self);
}

void buffer_pool_damage_all(struct buffer_pool* self,
		struct pixman_region16* damage)
{
	struct buffer* buffer;
	LIST_FOREACH(buffer, &self->registry, registry_link) {
		pixman_region_union(&buffer->damage, damage, damage);
	}
}
