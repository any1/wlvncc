#pragma once

struct buffer;
struct image;
struct vnc_av_frame;
struct gbm_device;

int egl_init(struct gbm_device* gbm);
void egl_finish(void);

void render_image_egl(struct buffer* dst, const struct image* src);
void render_av_frames_egl(struct buffer* dst, struct vnc_av_frame** src,
		int n_av_frames);

