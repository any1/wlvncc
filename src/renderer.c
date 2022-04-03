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

#include "renderer.h"
#include "buffer.h"
#include "pixels.h"

#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <pixman.h>
#include <assert.h>

void render_image(struct buffer* dst, const struct image* src, double scale,
		int x_pos, int y_pos)
{
	bool ok __attribute__((unused));

	pixman_format_code_t dst_fmt = 0;
	ok = drm_format_to_pixman_fmt(&dst_fmt, dst->format);
	assert(ok);

	pixman_format_code_t src_fmt = 0;
	ok = drm_format_to_pixman_fmt(&src_fmt, src->format);
	assert(ok);

	pixman_image_t* dstimg = pixman_image_create_bits_no_clear(dst_fmt,
			dst->width, dst->height, dst->pixels, dst->stride);

	pixman_image_t* srcimg = pixman_image_create_bits_no_clear(src_fmt,
			src->width, src->height, src->pixels, src->stride);

	pixman_fixed_t src_scale = pixman_double_to_fixed(1.0 / scale);

	pixman_transform_t xform;
	pixman_transform_init_scale(&xform, src_scale, src_scale);
	pixman_image_set_transform(srcimg, &xform);

	pixman_image_set_clip_region(dstimg, &dst->damage);

	pixman_image_composite(PIXMAN_OP_OVER, srcimg, NULL, dstimg,
			0, 0,
			0, 0,
			x_pos, y_pos,
			dst->width, dst->height);

	pixman_image_unref(srcimg);
	pixman_image_unref(dstimg);

	pixman_region_clear(&dst->damage);
}

