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

#pragma once

#include "buffer.h"

#include <stdbool.h>
#include <stdint.h>

struct buffer_pool;
struct buffer;

struct buffer_pool* buffer_pool_create(enum buffer_type type, uint16_t width,
		uint16_t height, uint32_t format, uint16_t stride, int scale);
void buffer_pool_destroy(struct buffer_pool* self);

bool buffer_pool_resize(struct buffer_pool* self, uint16_t width,
		uint16_t height, uint32_t format, uint16_t stride, int scale);

struct buffer* buffer_pool_acquire(struct buffer_pool* self);

void buffer_pool_damage_all(struct buffer_pool* self,
		struct pixman_region16* damage);
