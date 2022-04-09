#pragma once

struct buffer;
struct image;

int egl_init(void);
void egl_finish(void);

void render_image_egl(struct buffer* dst, const struct image* src, double scale,
		int pos_x, int pos_y);

