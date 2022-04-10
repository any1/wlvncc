#pragma once

struct buffer;
struct image;
struct vnc_av_frame;

int egl_init(void);
void egl_finish(void);

void render_image_egl(struct buffer* dst, const struct image* src, double scale,
		int pos_x, int pos_y);
void render_av_frames_egl(struct buffer* dst, struct vnc_av_frame** src,
		int n_av_frames, double scale, int x_pos, int y_pos);

