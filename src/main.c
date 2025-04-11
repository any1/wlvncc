/*
 * Copyright (c) 2020 - 2022 Andri Yngvason
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

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <aml.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <fcntl.h>

#include "inhibitor.h"
#include "viewporter-v1.h"
#include "single-pixel-buffer-v1.h"
#include "pixman.h"
#include "xdg-shell.h"
#include "shm.h"
#include "seat.h"
#include "pointer.h"
#include "keyboard.h"
#include "vnc.h"
#include "pixels.h"
#include "region.h"
#include "buffer.h"
#include "renderer.h"
#include "renderer-egl.h"
#include "linux-dmabuf-v1.h"
#include "time-util.h"
#include "output.h"

#define CANARY_TICK_PERIOD INT64_C(100000) // us
#define CANARY_LETHALITY_LEVEL INT64_C(8000) // us
#define BUFFERING 2

struct point {
	double x, y;
};

struct window {
	struct wl_shm* wl_bg_pixels; // optional
	struct wl_buffer* wl_bg_buffer;
	struct wl_surface* wl_bg_surface;
	struct xdg_surface* xdg_bg_surface;
	struct xdg_toplevel* xdg_bg_toplevel;
	struct wp_viewport* wp_bg_viewport;

	struct wl_surface* wl_surface;
	struct wl_subsurface* wl_subsurface;
	struct wp_viewport* wp_viewport;

	int preferred_buffer_scale;
	int width, height;
	int32_t scale;

	struct buffer* buffers[BUFFERING];
	struct buffer* back_buffer;
	int buffer_index;

	struct pixman_region16 current_damage;

	struct vnc_client* vnc;
	void* vnc_fb;

	bool is_frame_committed;
};

struct format_table_entry {
	uint32_t format;
	uint32_t padding;
	uint64_t modifier;
} __attribute__((packed));

static void register_frame_callback(void);

static struct wl_display* wl_display;
static struct wl_registry* wl_registry;
struct wl_compositor* wl_compositor = NULL;
struct wl_shm* wl_shm = NULL;
struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf_v1 = NULL;
struct format_table_entry* format_table = NULL;
uint32_t format_table_size = -1;
struct gbm_device* gbm_device = NULL;
static struct xdg_wm_base* xdg_wm_base;
static struct wl_list seats;
static struct wl_list outputs;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibitor;
struct pointer_collection* pointers;
struct keyboard_collection* keyboards;
static dev_t dma_dev;
static int drm_fd = -1;
static uint64_t last_canary_tick;
struct shortcuts_inhibitor* inhibitor;
struct wl_subcompositor* subcompositor;
struct wp_viewporter* viewporter;
struct wp_single_pixel_buffer_manager_v1* single_pixel_manager;

static bool have_egl = false;
static bool shortcut_inhibit = false;

static uint32_t shm_format = DRM_FORMAT_INVALID;
static uint32_t dmabuf_format = DRM_FORMAT_INVALID;

static bool do_run = true;

struct window* window = NULL;
const char* app_id = "wlvncc";

const char* tls_cert_path = NULL;
const char* auth_command = NULL;

static void on_seat_capability_change(struct seat* seat)
{
	if (seat->capabilities & WL_SEAT_CAPABILITY_POINTER) {
		// TODO: Make sure this only happens once
		struct wl_pointer* wl_pointer =
			wl_seat_get_pointer(seat->wl_seat);
		pointer_collection_add_wl_pointer(pointers, wl_pointer, seat);
	} else {
		// TODO Remove
	}

	if (seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		// TODO: Make sure this only happens once
		struct wl_keyboard* wl_keyboard =
			wl_seat_get_keyboard(seat->wl_seat);
		keyboard_collection_add_wl_keyboard(keyboards, wl_keyboard, seat);
	} else {
		// TODO Remove
	}
}

static void registry_add(void* data, struct wl_registry* registry, uint32_t id,
		const char* interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0) {
		if (version >= 6)
			wl_compositor = wl_registry_bind(registry, id,
					&wl_compositor_interface,  6);
		else
			wl_compositor = wl_registry_bind(registry, id,
						&wl_compositor_interface,  4);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		zwp_linux_dmabuf_v1 = wl_registry_bind(registry, id,
				&zwp_linux_dmabuf_v1_interface, 4);
	} else if (strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
		if (!shortcut_inhibit)
			return;
		keyboard_shortcuts_inhibitor = wl_registry_bind(registry, id,
				&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
		inhibitor = inhibitor_new(keyboard_shortcuts_inhibitor);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		single_pixel_manager = wl_registry_bind(registry, id, &wp_single_pixel_buffer_manager_v1_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		struct wl_seat* wl_seat;
		wl_seat = wl_registry_bind(registry, id, &wl_seat_interface, 5);

		struct seat* seat = seat_new(wl_seat, id);
		if (!seat) {
			wl_seat_destroy(wl_seat);
			return;
		}

		wl_list_insert(&seats, &seat->link);
		seat->on_capability_change = on_seat_capability_change;

		// TODO remove seat when we bind events on them
		inhibitor_add_seat(inhibitor, seat);
	} else if (strcmp(interface, "wl_output") == 0) {
		struct wl_output* wl_output;
		wl_output = wl_registry_bind(registry, id, &wl_output_interface, 2);

		struct output* output = output_new(wl_output, id);
		if (!output) {
			wl_output_destroy(wl_output);
			return;
		}

		wl_list_insert(&outputs, &output->link);
	}
}

void registry_remove(void* data, struct wl_registry* registry, uint32_t id)
{
	struct seat* seat = seat_find_by_id(&seats, id);
	if (seat) {
		wl_list_remove(&seat->link);
		seat_destroy(seat);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_add,
	.global_remove = registry_remove,
};

static void handle_shm_format(void* data, struct wl_shm* shm, uint32_t format)
{
	(void)data;
	(void)wl_shm;

	if (shm_format != DRM_FORMAT_INVALID)
		return;

	uint32_t drm_format = drm_format_from_wl_shm(format);

	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
		shm_format = drm_format;
	}

	// TODO: Support more formats
}

static const struct wl_shm_listener shm_listener = {
	.format = handle_shm_format,
};

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* shell,
		uint32_t serial)
{
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void handle_dmabuf_formats_done(void* data,
		struct zwp_linux_dmabuf_feedback_v1* zwp_linux_dmabuf_feedback_v1)
{
	(void)data;
	(void)zwp_linux_dmabuf_feedback_v1;

	if (format_table != MAP_FAILED) {
		munmap(format_table, format_table_size);
		format_table = NULL;
		format_table_size = -1;
	}
}

static void handle_format_table(void* data,
		struct zwp_linux_dmabuf_feedback_v1* zwp_linux_dmabuf_feedback_v1,
		int32_t fd, uint32_t size)
{
	(void)data;
	(void)zwp_linux_dmabuf_feedback_v1;

	format_table = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (format_table == MAP_FAILED)
		return;

	format_table_size = size;
}

static void handle_main_device(void* data,
		struct zwp_linux_dmabuf_feedback_v1* zwp_linux_dmabuf_feedback_v1,
		struct wl_array* device)
{
	if (device->size != sizeof(dev_t))
		return;

	memcpy(&dma_dev, device->data, device->size);
}

static void handle_tranche_formats(void* data,
		struct zwp_linux_dmabuf_feedback_v1* zwp_linux_dmabuf_feedback_v1, struct wl_array* indices)
{
	(void)data;
	(void)zwp_linux_dmabuf_feedback_v1;

	if (dmabuf_format != DRM_FORMAT_INVALID)
		return;

	if (format_table == MAP_FAILED)
		return;

	uint16_t* index;
	uint16_t max_index = format_table_size / sizeof(struct format_table_entry);
	wl_array_for_each(index, indices) {
		if (*index >= max_index)
			continue;

		switch (format_table[*index].format) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_XBGR8888:
			if (format_table[*index].modifier == DRM_FORMAT_MOD_INVALID)
				dmabuf_format = format_table[*index].format;
		}
	}
}

static void noop()
{
	/* noop */
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
	.done = handle_dmabuf_formats_done,
	.format_table = handle_format_table,
	.main_device = handle_main_device,
	.tranche_done = noop,
	.tranche_target_device = noop,
	.tranche_formats = handle_tranche_formats,
	.tranche_flags = noop,
};

void on_wayland_event(void* obj)
{
	int rc = wl_display_prepare_read(wl_display);
	assert(rc == 0);

	if (wl_display_read_events(wl_display) < 0) {
		if (errno == EPIPE) {
			fprintf(stderr, "Compositor has gone away. Exiting...\n");
			do_run = false;
		} else {
			fprintf(stderr, "Failed to read wayland events: %m\n");
		}
	}

	if (wl_display_dispatch_pending(wl_display) < 0)
		fprintf(stderr, "Failed to dispatch pending\n");
}

static int init_wayland_event_handler(void)
{
	struct aml_handler* handler;
	handler = aml_handler_new(wl_display_get_fd(wl_display),
			on_wayland_event, NULL, NULL);
	if (!handler)
		return -1;

	int rc = aml_start(aml_get_default(), handler);
	aml_unref(handler);
	return rc;
}

static void on_signal(void* obj)
{
	do_run = false;
}

static int init_signal_handler(void)
{
	struct aml_signal* sig;
	sig = aml_signal_new(SIGINT, on_signal, NULL, NULL);
	if (!sig)
		return -1;

	int rc = aml_start(aml_get_default(), sig);
	aml_unref(sig);
	return rc;
}

static void window_attach(struct window* w)
{
	wl_surface_attach(w->wl_bg_surface, w->wl_bg_buffer, 0, 0);
	w->back_buffer->is_attached = true;
	wl_surface_attach(w->wl_surface, w->back_buffer->wl_buffer, 0, 0);
}

int32_t window_get_scale(struct window* w)
{
	if (w->preferred_buffer_scale)
		return w->preferred_buffer_scale;

	return output_list_get_max_scale(&outputs);
}

static struct point surface_coord_to_buffer_coord(double x, double y)
{
	double scale = window_get_scale(window);

	struct point result = {
		.x = round(x * scale),
		.y = round(y * scale),
	};

	return result;
}

static void window_calculate_buffer(struct window* w, double* scale,
		int* width, int* height)
{
	double src_width = vnc_client_get_width(w->vnc);
	double src_height = vnc_client_get_height(w->vnc);
	double dst_width = w->width * w->scale;
	double dst_height = w->height * w->scale;

	double hratio = (double)dst_width / (double)src_width;
	double vratio = (double)dst_height / (double)src_height;
	*scale = fmin(hratio, vratio);

	if (hratio < vratio + 0.01 && hratio > vratio - 0.01) {
		*width = dst_width;
		*height = dst_height;
	} else if (hratio < vratio) {
		*width = dst_width;
		*height = src_height * *scale;
	} else {
		*width = src_width * *scale;
		*height = dst_height;
	}
}

static void window_transfer_pixels(struct window* w)
{
	if (w->vnc->n_av_frames != 0) {
		assert(have_egl);

		render_av_frames_egl(w->back_buffer, w->vnc->av_frames,
				w->vnc->n_av_frames);
		return;
	}

	struct image image = {
		.pixels = w->vnc_fb,
		.width = vnc_client_get_width(w->vnc),
		.height = vnc_client_get_height(w->vnc),
		.stride = vnc_client_get_stride(w->vnc),
		// TODO: Get the format from the vnc module
		.format = w->back_buffer->format,
		.damage = &w->current_damage,
	};

	if (have_egl)
		render_image_egl(w->back_buffer, &image);
	else
		render_image(w->back_buffer, &image);
}

static void window_commit(struct window* w)
{
	wl_surface_commit(w->wl_bg_surface);
	wl_surface_commit(w->wl_surface);
}

static void window_swap(struct window* w)
{
	w->buffer_index = (w->buffer_index + 1) % BUFFERING;
	w->back_buffer = w->buffers[w->buffer_index];
}

static void window_damage_buffer(struct window* w, int x, int y, int width, int height)
{
	wl_surface_damage_buffer(w->wl_surface, x, y, width, height);
}

static void window_configure(struct window* w)
{
	// What to do here?
}

static void xdg_surface_configure(void* data, struct xdg_surface* surface,
		uint32_t serial)
{
	struct window* w = data;
	xdg_surface_ack_configure(surface, serial);
	window_configure(w);
	// dunno why it does not work with the subsurface
	inhibitor_init(inhibitor, w->wl_bg_surface, &seats);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void window_resize(struct window* w, int width, int height)
{
	int32_t scale = window_get_scale(window);

	if (width == 0 || height == 0 || scale == 0)
		return;
	if (w->width == width && w->height == height && w->scale == scale)
		return;

	w->width = width;
	w->height = height;
	w->scale = scale;

	double new_scale;
	int new_width, new_height;
	window_calculate_buffer(window, &new_scale, &new_width, &new_height);

	new_width /= scale;
	new_height /= scale;

	wp_viewport_set_destination(w->wp_bg_viewport, width, height);
	wp_viewport_set_destination(w->wp_viewport, new_width, new_height);
	wl_subsurface_set_position(w->wl_subsurface,
		(width - new_width) / 2 , (height - new_height) / 2);
	window_commit(w);
}

static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
		int32_t width, int32_t height, struct wl_array* state)
{
	window_resize(data, width, height);
}

static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel)
{
	do_run = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void wl_surface_enter(void* data, struct wl_surface* wl_surface,
		struct wl_output* wl_output)
{
	struct output* output = output_find_by_wl_output(&outputs, wl_output);
	assert(output);

	output->has_surface = true;

	window_resize(window, window->width, window->height);
}

static void wl_surface_leave(void* data, struct wl_surface* wl_surface,
		struct wl_output* wl_output)
{
	struct output* output = output_find_by_wl_output(&outputs, wl_output);
	assert(output);

	output->has_surface = false;

	window_resize(window, window->width, window->height);
}

static void wl_surface_preferred_buffer_scale(void* data,
		struct wl_surface* wl_surface, int factor)
{
	struct window * w = data;
	w->preferred_buffer_scale = factor;

	window_resize(window, window->width, window->height);
}

static void wl_surface_preferred_buffer_transform(void* data,
		struct wl_surface* wl_surface, unsigned int transform)
{ }

static const struct wl_surface_listener wl_surface_listener = {
	.enter = wl_surface_enter,
	.leave = wl_surface_leave,
	.preferred_buffer_scale = wl_surface_preferred_buffer_scale,
	.preferred_buffer_transform = wl_surface_preferred_buffer_transform,
};

struct wl_buffer* create_single_pixel_buffer_fallback(struct wl_shm** pixels,
		struct wl_buffer** wl_buffer, uint32_t format)
{
	int fd = shm_alloc_fd(4);
	if (fd < 0)
		return NULL;

	*pixels = mmap(NULL, 4, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (!*pixels)
		goto mmap_failure;

	struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, 4);
	if (!pool)
		goto pool_failure;

	*wl_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1,
			4, drm_format_to_wl_shm(format));
	wl_shm_pool_destroy(pool);
	if (!*wl_buffer)
		goto shm_failure;

	close(fd);

	return *wl_buffer;

shm_failure:
pool_failure:
	munmap(*pixels, 4);
mmap_failure:
	close(fd);
	return NULL;
}

static struct window* window_create(const char* app_id, const char* title)
{
	struct window* w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;

	w->preferred_buffer_scale = 0;
	pixman_region_init(&w->current_damage);

	if (single_pixel_manager)
		w->wl_bg_buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
				single_pixel_manager, 0, 0, 0, UINT32_MAX);
	else
		create_single_pixel_buffer_fallback(&w->wl_bg_pixels, &w->wl_bg_buffer,
			shm_format);
	assert(w->wl_bg_buffer);

	w->wl_bg_surface = wl_compositor_create_surface(wl_compositor);
	if (!w->wl_bg_surface)
		goto wl_bg_surface_failure;

	wl_surface_add_listener(w->wl_bg_surface, &wl_surface_listener, w);

	w->wp_bg_viewport = wp_viewporter_get_viewport(viewporter, w->wl_bg_surface);
	if (!w->wp_bg_viewport)
		goto wp_bg_viewport_failure;

	w->xdg_bg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, w->wl_bg_surface);
	if (!w->xdg_bg_surface)
		goto xdg_surface_failure;

	xdg_surface_add_listener(w->xdg_bg_surface, &xdg_surface_listener, w);

	w->xdg_bg_toplevel = xdg_surface_get_toplevel(w->xdg_bg_surface);
	if (!w->xdg_bg_toplevel)
		goto xdg_toplevel_failure;

	xdg_toplevel_add_listener(w->xdg_bg_toplevel, &xdg_toplevel_listener, w);

	xdg_toplevel_set_app_id(w->xdg_bg_toplevel, app_id);
	xdg_toplevel_set_title(w->xdg_bg_toplevel, title);

	w->wl_surface = wl_compositor_create_surface(wl_compositor);
	if (!w->wl_surface)
		goto wl_surface_failure;

	w->wl_subsurface = wl_subcompositor_get_subsurface(subcompositor, w->wl_surface, w->wl_bg_surface);
	if (!w->wl_subsurface)
		goto wl_subsurface_failure;

	wl_subsurface_set_desync(w->wl_subsurface);
	w->wp_viewport = wp_viewporter_get_viewport(viewporter, w->wl_surface);
	if (!w->wp_viewport)
		goto wp_viewport_failure;

	wl_surface_commit(w->wl_bg_surface);
	wl_surface_commit(w->wl_surface);

	return w;

wp_viewport_failure:
	wl_subsurface_destroy(w->wl_subsurface);
wl_subsurface_failure:
	wl_surface_destroy(w->wl_surface);
wl_surface_failure:
	xdg_toplevel_destroy(w->xdg_bg_toplevel);
xdg_toplevel_failure:
	xdg_surface_destroy(w->xdg_bg_surface);
xdg_surface_failure:
	wp_viewport_destroy(w->wp_bg_viewport);
wp_bg_viewport_failure:
	wl_surface_destroy(w->wl_bg_surface);
wl_bg_surface_failure:
	free(w);
	return NULL;
}

static void window_destroy(struct window* w)
{
	for (int i = 0; i < BUFFERING; ++i)
		buffer_destroy(w->buffers[i]);
	if (w->wl_bg_pixels)
		munmap(w->wl_bg_pixels, 4);
	wl_buffer_destroy(w->wl_bg_buffer);

	free(w->vnc_fb);
	wp_viewport_destroy(w->wp_viewport);
	wl_subsurface_destroy(w->wl_subsurface);
	wl_surface_destroy(w->wl_surface);
	xdg_toplevel_destroy(w->xdg_bg_toplevel);
	xdg_surface_destroy(w->xdg_bg_surface);
	wp_viewport_destroy(w->wp_bg_viewport);
	wl_surface_destroy(w->wl_bg_surface);
	pixman_region_fini(&w->current_damage);
	free(w);
}

void on_pointer_event(struct pointer_collection* collection,
		struct pointer* pointer)
{
	struct vnc_client* client = collection->userdata;

	double scale;
	int width, height;
	window_calculate_buffer(window, &scale, &width, &height);

	struct point coord = surface_coord_to_buffer_coord(
			wl_fixed_to_double(pointer->x),
			wl_fixed_to_double(pointer->y));

	int x = round(coord.x / scale);
	int y = round(coord.y / scale);

	enum pointer_button_mask pressed = pointer->pressed;
	int vertical_steps = pointer->vertical_scroll_steps;
	int horizontal_steps = pointer->horizontal_scroll_steps;

	if (!vertical_steps && !horizontal_steps) {
		vnc_client_send_pointer_event(client, x, y, pressed);
		return;
	}

	enum pointer_button_mask scroll_mask = 0;
	if (vertical_steps < 0) {
		vertical_steps *= -1;
		scroll_mask |= POINTER_SCROLL_UP;
	} else if (vertical_steps > 0) {
		scroll_mask |= POINTER_SCROLL_DOWN;
	}

	if (horizontal_steps < 0) {
		horizontal_steps *= -1;
		scroll_mask |= POINTER_SCROLL_LEFT;
	} else if (horizontal_steps > 0) {
		scroll_mask |= POINTER_SCROLL_RIGHT;
	}

	while (horizontal_steps > 0 || vertical_steps > 0) {
		vnc_client_send_pointer_event(client, x, y, pressed | scroll_mask);
		vnc_client_send_pointer_event(client, x, y, pressed);

		if (--vertical_steps <= 0)
			scroll_mask &= ~(POINTER_SCROLL_UP | POINTER_SCROLL_DOWN);
		if (--horizontal_steps <= 0)
			scroll_mask &= ~(POINTER_SCROLL_LEFT | POINTER_SCROLL_RIGHT);
	}
}

bool pointer_handle_event(struct wl_surface* wl_surface)
{
	return window->wl_surface == wl_surface;
}

void on_keyboard_event(struct keyboard_collection* collection,
		struct keyboard* keyboard, uint32_t key, bool is_pressed)
{
	struct vnc_client* client = collection->userdata;

	// TODO handle multiple symbols
	xkb_keysym_t symbol = xkb_state_key_get_one_sym(keyboard->state, key);

	if (symbol == XKB_KEY_F12) {
		if (!is_pressed) {
			inhibitor_toggle(inhibitor, keyboard->seat);
		}
		return;
	}

	char name[256];
	xkb_keysym_get_name(symbol, name, sizeof(name));

	vnc_client_send_keyboard_event(client, symbol, key - 8, is_pressed);
}

int on_vnc_client_alloc_fb(struct vnc_client* client)
{
	int width = vnc_client_get_width(client);
	int height = vnc_client_get_height(client);
	int stride = vnc_client_get_stride(client);

	if (!window) {
		window = window_create(app_id, vnc_client_get_desktop_name(client));
		window->vnc = client;

		window_resize(window, width, height);
	}

	for (int i = 0; i < BUFFERING; ++i) {
		window->buffers[i] = have_egl
			? buffer_create_dmabuf(width, height, dmabuf_format)
			: buffer_create_shm(width, height, 4 * width, shm_format);
	}
	window->back_buffer = window->buffers[0];

	free(window->vnc_fb);
	window->vnc_fb = malloc(height * stride);
	assert(window->vnc_fb);

	vnc_client_set_fb(client, window->vnc_fb);
	return 0;
}

static void get_frame_damage(struct vnc_client* client,
		struct pixman_region16* damage)
{
	pixman_region_union(damage, damage, &client->damage);

	for (int i = 0; i < client->n_av_frames; ++i) {
		const struct vnc_av_frame* frame = client->av_frames[i];

		pixman_region_union_rect(damage, damage, frame->x, frame->y,
				frame->width, frame->height);
	}
}

static void apply_buffer_damage(struct pixman_region16* damage)
{
	for (int i = 0; i < BUFFERING; ++i)
		pixman_region_union(&window->buffers[i]->damage,
				&window->buffers[i]->damage, damage);
}

static void window_damage_region(struct window* w,
		struct pixman_region16* damage)
{
	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(damage, &n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int width = box[i].x2 - x;
		int height = box[i].y2 - y;

		window_damage_buffer(w, x, y, width, height);
	}
}

static void render_from_vnc(void)
{
	if (!pixman_region_not_empty(&window->current_damage) &&
			window->vnc->n_av_frames == 0)
		return;

	if (window->is_frame_committed)
		return;

	if (window->back_buffer->is_attached)
		fprintf(stderr, "Oops, back-buffer is still attached.\n");

	window_attach(window);

	apply_buffer_damage(&window->current_damage);
	window_damage_region(window, &window->current_damage);

	window_transfer_pixels(window);

	window->is_frame_committed = true;
	register_frame_callback();

	window_commit(window);
	window_swap(window);

	pixman_region_clear(&window->current_damage);
	vnc_client_clear_av_frames(window->vnc);
}

void on_vnc_client_update_fb(struct vnc_client* client)
{
	get_frame_damage(window->vnc, &window->current_damage);
	render_from_vnc();
}

static void handle_frame_callback(void* data, struct wl_callback* callback,
		uint32_t time)
{
	wl_callback_destroy(callback);
	window->is_frame_committed = false;

	if (!window->vnc->is_updating)
		render_from_vnc();
}

static const struct wl_callback_listener frame_listener = {
	.done = handle_frame_callback
};

static void register_frame_callback(void)
{
	struct wl_callback* callback = wl_surface_frame(window->wl_surface);
	wl_callback_add_listener(callback, &frame_listener, NULL);
}

void on_vnc_client_event(void* obj)
{
	struct vnc_client* client = aml_get_userdata(obj);
	if (vnc_client_process(client) < 0)
		do_run = false;
}

int init_vnc_client_handler(struct vnc_client* client)
{
	int fd = vnc_client_get_fd(client);

	struct aml_handler* handler;
	handler = aml_handler_new(fd, on_vnc_client_event, client, NULL);
	if (!handler)
		return -1;

	int rc = aml_start(aml_get_default(), handler);
	aml_unref(handler);
	return rc;
}

static int find_render_node(char *node, size_t maxlen) {
	bool r = -1;
	drmDevice *devices[64];

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		strncpy(node, dev->nodes[DRM_NODE_RENDER], maxlen);
		node[maxlen - 1] = '\0';
		r = 0;
		break;
	}

	drmFreeDevices(devices, n);
	return r;
}

static int render_node_from_dev_t(char* node, size_t maxlen, dev_t device)
{
	drmDevice *dev_ptr;

	if (drmGetDeviceFromDevId(device, 0, &dev_ptr) < 0)
		return -1;

	if (dev_ptr->available_nodes & (1 << DRM_NODE_RENDER))
		strlcpy(node, dev_ptr->nodes[DRM_NODE_RENDER], maxlen);

	drmFreeDevice(&dev_ptr);

	return 0;
}

static int init_gbm_device(void)
{
	int rc;

	char render_node[256];
	if (dma_dev) {
		rc = render_node_from_dev_t(render_node, sizeof(render_node),
				dma_dev);
	} else {
		rc = find_render_node(render_node, sizeof(render_node));
	}
	if (rc < 0)
		return -1;

	drm_fd = open(render_node, O_RDWR);
	if (drm_fd < 0)
		return 1;

	gbm_device = gbm_create_device(drm_fd);
	if (!gbm_device) {
		close(drm_fd);
		drm_fd = -1;
		return -1;
	}

	return 0;
}

static int init_egl_renderer(void)
{
	if (!zwp_linux_dmabuf_v1) {
		printf("Missing linux-dmabuf-unstable-v1. Using software rendering.\n");
		return -1;
	}

	struct zwp_linux_dmabuf_feedback_v1* zwp_linux_dmabuf_feedback_v1 = NULL;
	zwp_linux_dmabuf_feedback_v1 = zwp_linux_dmabuf_v1_get_default_feedback(zwp_linux_dmabuf_v1);
	zwp_linux_dmabuf_feedback_v1_add_listener(zwp_linux_dmabuf_feedback_v1, &dmabuf_feedback_listener, NULL);
	wl_display_roundtrip(wl_display);

	if (dmabuf_format == DRM_FORMAT_INVALID) {
		printf("No supported dmabuf pixel format found. Using software rendering.\n");
		goto failure;
	}

	if (init_gbm_device() < 0) {
		printf("Failed to find render node. Using software rendering.\n");
		goto failure;
	}

	if (egl_init(gbm_device) < 0) {
		printf("Failed initialise EGL. Using software rendering.\n");
		goto failure;
	}

	printf("Using EGL for rendering...\n");

	return 0;

failure:
	if (zwp_linux_dmabuf_v1) {
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf_v1);
		zwp_linux_dmabuf_v1 = NULL;
	}

	return -1;
}

static void on_canary_tick(void* obj)
{
	(void)obj;

	uint64_t t = gettime_us();
	int64_t dt = t - last_canary_tick;
	last_canary_tick = t;

	int64_t delay = dt - CANARY_TICK_PERIOD;

	// Early ticks are just a result of late ticks...
	if (delay < CANARY_LETHALITY_LEVEL)
		return;

	fprintf(stderr, "WARNING: Long delays observed (%"PRIi64"). Something is blocking the main loop\n",
				delay);
}

static void create_canary_ticker(void)
{
	last_canary_tick = gettime_us();

	struct aml* aml = aml_get_default();
	struct aml_ticker* ticker = aml_ticker_new(CANARY_TICK_PERIOD,
			on_canary_tick, NULL, NULL);
	aml_start(aml, ticker);
	aml_unref(ticker);
}

void run_main_loop_once(void)
{
	struct aml* aml = aml_get_default();
	wl_display_flush(wl_display);
	aml_poll(aml, -1);
	aml_dispatch(aml);
}

static int usage(int r)
{
	fprintf(r ? stderr : stdout, "\
Usage: wlvncc <address> [port]\n\
\n\
    -a,--app-id=<name>       Set the app-id of the window. Default: wlvncc\n\
    -A,--auth-command=<cmd>  Run a command for authenticating.\n\
    -c,--compression         Compression level (0 - 9).\n\
    -e,--encodings=<list>    Set allowed encodings, comma separated list.\n\
                             Supported values: tight, zrle, ultra, copyrect,\n\
                             hextile, zlib, corre, rre, raw, open-h264.\n\
    -h,--help                Get help.\n\
    -n,--hide-cursor         Hide the client-side cursor.\n\
    -i,--shortcut-inhibit    Enable the shortcut inhibitor while being focused.\n\
    -q,--quality             Quality level (0 - 9).\n\
    -t,--tls-cert            Use given TLS cert for authenticating server.\n\
    -s,--use-sw-renderer     Use software rendering.\n\
\n\
");
	return r;
}

int main(int argc, char* argv[])
{
	int rc = -1;

	enum pointer_cursor_type cursor_type = POINTER_CURSOR_LEFT_PTR;
	const char* encodings = NULL;
	int quality = -1;
	int compression = -1;
	static const char* shortopts = "a:A:q:c:e:hnist:";
	bool use_sw_renderer = false;

	static const struct option longopts[] = {
		{ "app-id", required_argument, NULL, 'a' },
		{ "auth-command", required_argument, NULL, 'A' },
		{ "compression", required_argument, NULL, 'c' },
		{ "encodings", required_argument, NULL, 'e' },
		{ "hide-cursor", no_argument, NULL, 'n' },
		{ "shortcut-inhibit", no_argument, NULL, 'i' },
		{ "help", no_argument, NULL, 'h' },
		{ "quality", required_argument, NULL, 'q' },
		{ "tls-cert", required_argument, NULL, 't' },
		{ "use-sw-renderer", no_argument, NULL, 's' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'a':
			app_id = optarg;
			break;
		case 'A':
			auth_command = optarg;
			break;
		case 'q':
			quality = atoi(optarg);
			break;
		case 'c':
			compression = atoi(optarg);
			break;
		case 'e':
			encodings = optarg;
			break;
		case 'n':
			cursor_type = POINTER_CURSOR_NONE;
			break;
		case 'i':
			shortcut_inhibit = true;
			break;
		case 's':
			use_sw_renderer = true;
			break;
		case 't':
			tls_cert_path = optarg;
			break;
		case 'h':
			return usage(0);
		default:
			return usage(1);
		}
	}

	int n_args = argc - optind;

	if (n_args < 1)
		return usage(1);

	const char* address = argv[optind];
	int port = 5900;
	if (n_args >= 2)
		port = atoi(argv[optind + 1]);

	if (aml_unstable_abi_version != AML_UNSTABLE_API) {
		fprintf(stderr, "libaml is incompatible with current build of wlvncc!\n");
		abort();
	}

	struct aml* aml = aml_new();
	if (!aml)
		return 1;

	aml_set_default(aml);

	if (init_signal_handler() < 0)
		goto signal_handler_failure;

	wl_display = wl_display_connect(NULL);
	if (!wl_display) {
		fprintf(stderr, "Failed to connect to local wayland display\n");
		goto display_failure;
	}

	if (init_wayland_event_handler() < 0)
		goto event_handler_failure;

	pointers = pointer_collection_new(cursor_type);
	if (!pointers)
		goto pointer_failure;

	pointers->on_frame = on_pointer_event;
	pointers->handle_event = pointer_handle_event;

	keyboards = keyboard_collection_new();
	if (!keyboards)
		goto keyboards_failure;

	keyboards->on_event = on_keyboard_event;

	wl_registry = wl_display_get_registry(wl_display);
	if (!wl_registry)
		goto registry_failure;

	wl_list_init(&seats);
	wl_list_init(&outputs);

	wl_registry_add_listener(wl_registry, &registry_listener, wl_display);
	wl_display_roundtrip(wl_display);

	assert(wl_compositor);
	assert(wl_shm);
	assert(xdg_wm_base);
	assert(subcompositor);
	assert(viewporter);

	wl_shm_add_listener(wl_shm, &shm_listener, NULL);

	xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);

	if (!use_sw_renderer)
		have_egl = init_egl_renderer() == 0;

	wl_display_roundtrip(wl_display);
	wl_display_roundtrip(wl_display);

	struct vnc_client* vnc = vnc_client_create();
	if (!vnc)
		goto vnc_failure;

	vnc->alloc_fb = on_vnc_client_alloc_fb;
	vnc->update_fb = on_vnc_client_update_fb;

	uint32_t format = have_egl ? dmabuf_format : shm_format;

	if (vnc_client_set_pixel_format(vnc, format) < 0) {
		fprintf(stderr, "Unsupported pixel format\n");
		goto vnc_setup_failure;
	}

	if (encodings) {
		if (!have_egl && strstr(encodings, "open-h264")) {
			fprintf(stderr, "Open H.264 encoding won't work without EGL\n");
			goto vnc_setup_failure;
		}
	} else if (have_egl) {
		encodings = "open-h264,tight,zrle,ultra,copyrect,hextile,zlib"
			",corre,rre,raw";
	} else {
		encodings = "tight,zrle,ultra,copyrect,hextile,zlib,corre,rre,raw";
	}
	vnc_client_set_encodings(vnc, encodings);

	if (quality >= 0)
		vnc_client_set_quality_level(vnc, quality);

	if (compression >= 0)
		vnc_client_set_compression_level(vnc, compression);

	if (vnc_client_connect(vnc, address, port) < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		goto vnc_setup_failure;
	}

	if (init_vnc_client_handler(vnc) < 0)
		goto vnc_setup_failure;

	if (vnc_client_init(vnc) < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		goto vnc_setup_failure;
	}

	pointers->userdata = vnc;
	keyboards->userdata = vnc;

	wl_display_dispatch(wl_display);

	create_canary_ticker();

	while (do_run)
		run_main_loop_once();

	rc = 0;
	if (window)
		window_destroy(window);
vnc_setup_failure:
	vnc_client_destroy(vnc);
vnc_failure:
	output_list_destroy(&outputs);
	seat_list_destroy(&seats);
	wl_compositor_destroy(wl_compositor);
	wl_shm_destroy(wl_shm);
	xdg_wm_base_destroy(xdg_wm_base);
	egl_finish();
	if (zwp_linux_dmabuf_v1)
		zwp_linux_dmabuf_v1_destroy(zwp_linux_dmabuf_v1);
	if (gbm_device)
		gbm_device_destroy(gbm_device);
	if (drm_fd >= 0)
		close(drm_fd);

	wl_registry_destroy(wl_registry);
registry_failure:
	keyboard_collection_destroy(keyboards);
keyboards_failure:
	pointer_collection_destroy(pointers);
pointer_failure:
event_handler_failure:
	wl_display_disconnect(wl_display);
display_failure:
signal_handler_failure:
	aml_unref(aml);
	printf("Exiting...\n");
	return rc;
}
