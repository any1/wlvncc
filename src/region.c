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

#include <math.h>
#include <pixman.h>

void region_scale(struct pixman_region16* dst, struct pixman_region16* src,
		double scale)
{
	if (scale == 1.0) {
		pixman_region_copy(dst, src);
		return;
	}

	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = floor((double)r->x1 * scale);
		int x2 = ceil((double)r->x2 * scale);
		int y1 = floor((double)r->y1 * scale);
		int y2 = ceil((double)r->y2 * scale);

		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}

void region_translate(struct pixman_region16* dst, struct pixman_region16* src,
		int x, int y)
{
	if (x == 0 && y == 0) {
		pixman_region_copy(dst, src);
		return;
	}
	
	pixman_region_fini(dst);
	pixman_region_init(dst);

	int n_rects = 0;
	pixman_box16_t* rects = pixman_region_rectangles(src, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		pixman_box16_t* r = &rects[i];

		int x1 = r->x1 + x;
		int x2 = r->x2 + x;
		int y1 = r->y1 + y;
		int y2 = r->y2 + y;

		pixman_region_union_rect(dst, dst, x1, y1, x2 - x1, y2 - y1);
	}
}
