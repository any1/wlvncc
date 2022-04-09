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

#define XSTR(s) STR(s)
#define STR(s) #s

#define EGL_EXTENSION_LIST \
X(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT) \
X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) \

#define GL_EXTENSION_LIST \
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

static void compile_shaders()
{
	GLuint vert = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vert, 1, &vertex_shader_src, NULL);
	glCompileShader(vert);

	GLint is_compiled = 0;
	glGetShaderiv(vert, GL_COMPILE_STATUS, &is_compiled);
	assert(is_compiled);

	GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(frag, 1, &fragment_shader_src, NULL);
	glCompileShader(frag);
	glGetShaderiv(frag, GL_COMPILE_STATUS, &is_compiled);
	assert(is_compiled);

	shader_program = glCreateProgram();

	glAttachShader(shader_program, vert);
	glAttachShader(shader_program, frag);

	glBindAttribLocation(shader_program, ATTR_INDEX_POS, "pos");
	glBindAttribLocation(shader_program, ATTR_INDEX_TEXTURE, "texture");

	glLinkProgram(shader_program);

	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint is_linked = 0;
	glGetProgramiv(shader_program, GL_LINK_STATUS, &is_linked);
	assert(is_linked);

	uniforms.u_tex = glGetUniformLocation(shader_program, "u_tex");
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

	compile_shaders();

	return 0;

failure:
	eglDestroyContext(egl_display, egl_context);
	return -1;
}

void egl_finish(void)
{
	if (texture)
		glDeleteTextures(1, &texture);
	if (shader_program)
		glDeleteProgram(shader_program);
	eglDestroyContext(egl_display, egl_context);
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

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

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

	glFinish();

	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glDeleteFramebuffers(1, &fbo.fbo);
	glDeleteRenderbuffers(1, &fbo.rbo);
}
