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
#include "renderer.h"
#include "renderer-egl.h"
#include "vnc.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>

#define XSTR(s) STR(s)
#define STR(s) #s

#define EGL_EXTENSION_LIST \
X(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT) \
X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) \

#define GL_EXTENSION_LIST \
X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) \
X(PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC, glEGLImageTargetRenderbufferStorageOES) \

#define X(t, n) static t n;
	EGL_EXTENSION_LIST
	GL_EXTENSION_LIST
#undef X

enum {
	ATTR_INDEX_POS = 0,
	ATTR_INDEX_TEXTURE,
};

struct fbo_info {
	GLuint fbo;
	GLuint rbo;
	int width, height;
};

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;

static GLuint shader_program = 0;
static GLuint shader_program_ext = 0;
static GLuint texture = 0;

static const char *vertex_shader_src =
"attribute vec2 pos;\n"
"attribute vec2 texture;\n"
"varying vec2 v_texture;\n"
"void main() {\n"
"	v_texture = vec2(texture.s, 1.0 - texture.t);\n"
"	gl_Position = vec4(pos, 0.0, 1.0);\n"
"}\n";

static const char *fragment_shader_src =
"precision mediump float;\n"
"uniform sampler2D u_tex;\n"
"varying vec2 v_texture;\n"
"void main() {\n"
"	vec4 colour = texture2D(u_tex, v_texture);\n"
"	gl_FragColor = vec4(colour.rgb, 1.0);\n"
"}\n";

static const char *fragment_shader_ext_src =
"#extension GL_OES_EGL_image_external: require\n\n"
"precision mediump float;\n"
"uniform samplerExternalOES u_tex;\n"
"varying vec2 v_texture;\n"
"void main() {\n"
"	gl_FragColor = texture2D(u_tex, v_texture);\n"
"}\n";

struct {
	GLuint u_tex;
} uniforms;

static int egl_load_egl_ext(void)
{
#define X(t, n) \
	n = (t)eglGetProcAddress(XSTR(n)); \
	if (!n) \
		return -1;

	EGL_EXTENSION_LIST
#undef X

	return 0;
}

static int egl_load_gl_ext(void)
{
#define X(t, n) \
	n = (t)eglGetProcAddress(XSTR(n)); \
	if (!n) \
		return -1;

	GL_EXTENSION_LIST
#undef X

	return 0;
}

static int compile_shaders(const char* vert_src, const char* frag_src)
{
	GLuint vert = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vert, 1, &vert_src, NULL);
	glCompileShader(vert);

	GLint is_compiled = 0;
	glGetShaderiv(vert, GL_COMPILE_STATUS, &is_compiled);
	assert(is_compiled);

	GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(frag, 1, &frag_src, NULL);
	glCompileShader(frag);
	glGetShaderiv(frag, GL_COMPILE_STATUS, &is_compiled);
	assert(is_compiled);

	int prog = glCreateProgram();

	glAttachShader(prog, vert);
	glAttachShader(prog, frag);

	glBindAttribLocation(prog, ATTR_INDEX_POS, "pos");
	glBindAttribLocation(prog, ATTR_INDEX_TEXTURE, "texture");

	glLinkProgram(prog);

	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint is_linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &is_linked);
	assert(is_linked);

	uniforms.u_tex = glGetUniformLocation(prog, "u_tex");

	return prog;
}

int egl_init(void)
{
	int rc;
	rc = eglBindAPI(EGL_OPENGL_ES_API);
	if (!rc)
		return -1;

	if (egl_load_egl_ext() < 0)
		return -1;

	egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
			EGL_DEFAULT_DISPLAY, NULL);
	if (egl_display == EGL_NO_DISPLAY)
		return -1;

	rc = eglInitialize(egl_display, NULL, NULL);
	if (!rc)
		goto failure;

	static const EGLint attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	egl_context = eglCreateContext(egl_display, EGL_NO_CONFIG_KHR,
			EGL_NO_CONTEXT, attribs);
	if (egl_context == EGL_NO_CONTEXT)
		goto failure;

	if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			egl_context))
		goto failure;

	if (egl_load_gl_ext() < 0)
		goto failure;

	shader_program = compile_shaders(vertex_shader_src,
			fragment_shader_src);
	shader_program_ext = compile_shaders(vertex_shader_src,
			fragment_shader_ext_src);

	return 0;

failure:
	eglDestroyContext(egl_display, egl_context);
	return -1;
}

void egl_finish(void)
{
	if (texture)
		glDeleteTextures(1, &texture);
	if (shader_program_ext)
		glDeleteProgram(shader_program_ext);
	if (shader_program)
		glDeleteProgram(shader_program);
	eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	eglDestroyContext(egl_display, egl_context);
	eglTerminate(egl_display);
}

static inline void append_attr(EGLint* dst, int* i, EGLint name, EGLint value)
{
	dst[*i] = name;
	i[0] += 1;
	dst[*i] = value;
	i[0] += 1;
}

static void fbo_from_gbm_bo(struct fbo_info* dst, struct gbm_bo* bo)
{
	memset(dst, 0, sizeof(*dst));

	int index = 0;
	EGLint attr[128];

	// Won't do multi-planar...
	int n_planes = gbm_bo_get_plane_count(bo);
	assert(n_planes == 1);

	int width = gbm_bo_get_width(bo);
	int height = gbm_bo_get_height(bo);

	append_attr(attr, &index, EGL_WIDTH, width);
	append_attr(attr, &index, EGL_HEIGHT, height);
	append_attr(attr, &index, EGL_LINUX_DRM_FOURCC_EXT,
			gbm_bo_get_format(bo));

	int fd = gbm_bo_get_fd(bo);

	// Plane 0:
	uint64_t mod = gbm_bo_get_modifier(bo);
	uint32_t mod_hi = mod >> 32;
	uint32_t mod_lo = mod & 0xffffffff;

	append_attr(attr, &index, EGL_DMA_BUF_PLANE0_FD_EXT, fd);
	append_attr(attr, &index, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			gbm_bo_get_offset(bo, 0));
	append_attr(attr, &index, EGL_DMA_BUF_PLANE0_PITCH_EXT,
			gbm_bo_get_stride(bo));
	append_attr(attr, &index, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, mod_lo);
	append_attr(attr, &index, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, mod_hi);

	attr[index++] = EGL_NONE;

	EGLImageKHR image = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(image != EGL_NO_IMAGE_KHR);

	GLuint rbo = 0;
	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, rbo);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	assert(status == GL_FRAMEBUFFER_COMPLETE);

	dst->fbo = fbo;
	dst->rbo = rbo;
	dst->width = width;
	dst->height = height;

	eglDestroyImageKHR(egl_display, image);
	close(fd);
}

#define X(lc, uc) \
static EGLint plane_ ## lc ## _key(int plane) \
{ \
	switch (plane) { \
	case 0: return EGL_DMA_BUF_PLANE0_ ## uc ## _EXT; \
	case 1: return EGL_DMA_BUF_PLANE1_ ## uc ## _EXT; \
	case 2: return EGL_DMA_BUF_PLANE2_ ## uc ## _EXT; \
	case 3: return EGL_DMA_BUF_PLANE3_ ## uc ## _EXT; \
	} \
	return EGL_NONE; \
}

X(fd, FD);
X(offset, OFFSET);
X(pitch, PITCH);
X(modifier_lo, MODIFIER_LO);
X(modifier_hi, MODIFIER_HI);
#undef X

static void dmabuf_attr_append_planes(EGLint* dst, int* index,
		struct AVDRMFrameDescriptor* desc)
{
	struct AVDRMPlaneDescriptor *plane;
	struct AVDRMObjectDescriptor *obj;

	for (int i = 0; i < desc->nb_layers; ++i) {
		assert(desc->layers[i].nb_planes == 1);

		plane = &desc->layers[i].planes[0];
		obj = &desc->objects[plane->object_index];

		append_attr(dst, index, plane_fd_key(i), obj->fd);
		append_attr(dst, index, plane_offset_key(i), plane->offset);
		append_attr(dst, index, plane_pitch_key(i), plane->pitch);
		append_attr(dst, index, plane_modifier_lo_key(i),
				obj->format_modifier);
		append_attr(dst, index, plane_modifier_hi_key(i),
				obj->format_modifier >> 32);
	}
}

static EGLint get_color_space_hint(const struct AVFrame* frame)
{
	switch (frame->colorspace) {
	case AVCOL_SPC_RGB: // Conversion coeffients are based on REC.601.
	case AVCOL_SPC_SMPTE170M:
		return EGL_ITU_REC601_EXT;
	case AVCOL_SPC_BT709:
		return EGL_ITU_REC709_EXT;
	default:;
	}
	return 0;
}

static EGLint get_sample_range_hint(const struct AVFrame* frame)
{
	return frame->color_range == AVCOL_RANGE_JPEG
		? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT;
}

static GLuint texture_from_av_frame(const struct AVFrame* frame)
{
	int index = 0;
	EGLint attr[128];

	AVDRMFrameDescriptor *desc = (void*)frame->data[0];

	append_attr(attr, &index, EGL_WIDTH, frame->width);
	append_attr(attr, &index, EGL_HEIGHT, frame->height);
	append_attr(attr, &index, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12);
	append_attr(attr, &index, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE);
	EGLint colorspace_hint = get_color_space_hint(frame);
	if (colorspace_hint)
		append_attr(attr, &index, EGL_YUV_COLOR_SPACE_HINT_EXT,
				colorspace_hint);
	if (frame->color_range != AVCOL_RANGE_UNSPECIFIED)
		append_attr(attr, &index, EGL_SAMPLE_RANGE_HINT_EXT,
				get_sample_range_hint(frame));
	dmabuf_attr_append_planes(attr, &index, desc);
	attr[index++] = EGL_NONE;

	EGLImageKHR image = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(image != EGL_NO_IMAGE_KHR);

	GLuint tex = 0;
	glGenTextures(1, &tex);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);

	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl_display, image);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	return tex;
}

void gl_draw(void)
{
	static const GLfloat s_vertices[4][2] = {
		{ -1.0, 1.0 },
		{ 1.0, 1.0 },
		{ -1.0, -1.0 },
		{ 1.0, -1.0 },
	};

	static const GLfloat s_positions[4][2] = {
		{ 0, 0 },
		{ 1, 0 },
		{ 0, 1 },
		{ 1, 1 },
	};

	glVertexAttribPointer(ATTR_INDEX_POS, 2, GL_FLOAT, GL_FALSE, 0,
			s_vertices);
	glVertexAttribPointer(ATTR_INDEX_TEXTURE, 2, GL_FLOAT, GL_FALSE, 0,
			s_positions);

	glEnableVertexAttribArray(ATTR_INDEX_POS);
	glEnableVertexAttribArray(ATTR_INDEX_TEXTURE);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(ATTR_INDEX_TEXTURE);
	glDisableVertexAttribArray(ATTR_INDEX_POS);
}

GLenum gl_format_from_drm(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		return GL_BGRA_EXT;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		return GL_RGBA;
	}

	return 0;
}

void import_image_with_damage(const struct image* src,
		struct pixman_region16* damage)
{
	GLenum fmt = gl_format_from_drm(src->format);

	int n_rects = 0;
	struct pixman_box16* rects =
		pixman_region_rectangles(damage, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int x = rects[i].x1;
		int y = rects[i].y1;
		int width = rects[i].x2 - x;
		int height = rects[i].y2 - y;

		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, x);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, y);

		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, fmt,
				GL_UNSIGNED_BYTE, src->pixels);
	}

	glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
}

void render_image_egl(struct buffer* dst, const struct image* src,
		double scale, int x_pos, int y_pos)
{
	struct fbo_info fbo;
	fbo_from_gbm_bo(&fbo, dst->bo);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);

	bool is_new_texture = !texture;

	if (!texture)
		glGenTextures(1, &texture);

	glBindTexture(GL_TEXTURE_2D, texture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, src->stride / 4);

	if (is_new_texture) {
		GLenum fmt = gl_format_from_drm(src->format);
		glTexImage2D(GL_TEXTURE_2D, 0, fmt, src->width, src->height, 0,
				fmt, GL_UNSIGNED_BYTE, src->pixels);
	} else {
		import_image_with_damage(src,
				(struct pixman_region16*)src->damage);
	}

	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

	int width = round((double)src->width * scale);
	int height = round((double)src->height * scale);
	glViewport(x_pos, y_pos, width, height);

	glUseProgram(shader_program);

	struct pixman_box16* ext = pixman_region_extents(&dst->damage);
	glScissor(ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1);
	glEnable(GL_SCISSOR_TEST);

	gl_draw();

	glDisable(GL_SCISSOR_TEST);

	glFlush();

	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glDeleteFramebuffers(1, &fbo.fbo);
	glDeleteRenderbuffers(1, &fbo.rbo);

	pixman_region_clear(&dst->damage);
}

void render_av_frames_egl(struct buffer* dst, struct vnc_av_frame** src,
		int n_av_frames, double scale, int x_pos, int y_pos)
{
	struct fbo_info fbo;
	fbo_from_gbm_bo(&fbo, dst->bo);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);

	struct pixman_box16* ext = pixman_region_extents(&dst->damage);
	glScissor(ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1);
	glEnable(GL_SCISSOR_TEST);

	glUseProgram(shader_program_ext);

	for (int i = 0; i < n_av_frames; ++i) {
		const struct vnc_av_frame* frame = src[i];

		int width = round((double)frame->width * scale);
		int height = round((double)frame->height * scale);
		glViewport(x_pos + frame->x, y_pos + frame->y, width, height);

		GLuint tex = texture_from_av_frame(frame->frame);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);

		gl_draw();

		glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	}

	glDisable(GL_SCISSOR_TEST);

	glFlush();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo.fbo);
	glDeleteRenderbuffers(1, &fbo.rbo);

	pixman_region_clear(&dst->damage);
}
