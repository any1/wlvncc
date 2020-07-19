/*
 * Copyright (c) 2020 Andri Yngvason
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
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/mman.h>
#include <aml.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "pixman.h"
#include "xdg-shell.h"
#include "shm.h"
#include "seat.h"
#include "pointer.h"
#include "keyboard.h"
#include "vnc.h"

struct buffer {
	int width, height, stride;
	size_t size;
	enum wl_shm_format format;
	struct wl_buffer* wl_buffer;
	void* pixels;
};

struct window {
	struct wl_surface* wl_surface;
	struct xdg_surface* xdg_surface;
	struct xdg_toplevel* xdg_toplevel;

	struct buffer* buffer;
};

static struct wl_display* wl_display;
static struct wl_registry* wl_registry;
struct wl_compositor* wl_compositor = NULL;
struct wl_shm* wl_shm = NULL;
static struct xdg_wm_base* xdg_wm_base;
static struct wl_list seats;
struct pointer_collection* pointers;
struct keyboard_collection* keyboards;

static enum wl_shm_format wl_shm_format;
static bool have_format = false;

static bool do_run = true;

struct window* window = NULL;
const char* app_id = "wlvncc";

static void on_seat_capability_change(struct seat* seat)
{
	if (seat->capabilities & WL_SEAT_CAPABILITY_POINTER) {
		// TODO: Make sure this only happens once
		struct wl_pointer* wl_pointer =
			wl_seat_get_pointer(seat->wl_seat);
		pointer_collection_add_wl_pointer(pointers, wl_pointer);
	} else {
		// TODO Remove
	}

	if (seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		// TODO: Make sure this only happens once
		struct wl_keyboard* wl_keyboard =
			wl_seat_get_keyboard(seat->wl_seat);
		keyboard_collection_add_wl_keyboard(keyboards, wl_keyboard);
	} else {
		// TODO Remove
	}
}

static void registry_add(void* data, struct wl_registry* registry, uint32_t id,
		const char* interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0) {
		wl_compositor = wl_registry_bind(registry, id,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
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

static struct buffer* buffer_create(int width, int height, int stride,
		enum wl_shm_format format)
{
	struct buffer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->width = width;
	self->height = height;
	self->stride = stride;
	self->format = format;

	self->size = height * stride;
	int fd = shm_alloc_fd(self->size);
	if (fd < 0)
		goto failure;

	self->pixels = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (!self->pixels)
		goto mmap_failure;

	struct wl_shm_pool* pool = wl_shm_create_pool(wl_shm, fd, self->size);
	if (!pool)
		goto pool_failure;

	self->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
			stride, format);
	wl_shm_pool_destroy(pool);
	if (!self->wl_buffer)
		goto shm_failure;

	close(fd);
	return self;

shm_failure:
pool_failure:
	munmap(self->pixels, self->size);
mmap_failure:
	close(fd);
failure:
	free(self);
	return NULL;
}

static void buffer_destroy(struct buffer* self)
{
	wl_buffer_destroy(self->wl_buffer);
	munmap(self->pixels, self->size);
	free(self);
}

static void shm_format(void* data, struct wl_shm* shm, uint32_t format)
{
	(void)data;
	(void)wl_shm;

	if (have_format)
		return;

	switch (format) {
	case WL_SHM_FORMAT_XRGB8888:
		wl_shm_format = format;
		have_format = true;
	}

	// TODO: Support more formats
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* shell,
		uint32_t serial)
{
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
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

static void window_attach(struct window* w, int x, int y)
{
	wl_surface_attach(w->wl_surface, w->buffer->wl_buffer, x, y);
}

static void window_commit(struct window* w)
{
	wl_surface_commit(w->wl_surface);
}

static void window_damage(struct window* w, int x, int y, int width, int height)
{
	wl_surface_damage(w->wl_surface, x, y, width, height);
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
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
		int32_t width, int32_t height, struct wl_array* state)
{
	// What to do here?
}

static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel)
{
	do_run = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static struct window* window_create(const char* app_id, const char* title)
{
	struct window* w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;

	w->wl_surface = wl_compositor_create_surface(wl_compositor);
	if (!w->wl_surface)
		goto wl_surface_failure;

	w->xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, w->wl_surface);
	if (!w->xdg_surface)
		goto xdg_surface_failure;

	xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);

	w->xdg_toplevel = xdg_surface_get_toplevel(w->xdg_surface);
	if (!w->xdg_toplevel)
		goto xdg_toplevel_failure;

	xdg_toplevel_add_listener(w->xdg_toplevel, &xdg_toplevel_listener, w);

	xdg_toplevel_set_app_id(w->xdg_toplevel, app_id);
	xdg_toplevel_set_title(w->xdg_toplevel, title);
	wl_surface_commit(w->wl_surface);

	return w;

xdg_toplevel_failure:
	xdg_surface_destroy(w->xdg_surface);
xdg_surface_failure:
	wl_surface_destroy(w->wl_surface);
wl_surface_failure:
	free(w);
	return NULL;
}

static void window_destroy(struct window* w)
{
	if (w->buffer)
		buffer_destroy(w->buffer);

	xdg_toplevel_destroy(w->xdg_toplevel);
	xdg_surface_destroy(w->xdg_surface);
	wl_surface_destroy(w->wl_surface);
	free(w);
}

void on_pointer_event(struct pointer_collection* collection,
		struct pointer* pointer)
{
	struct vnc_client* client = collection->userdata;

	int x = wl_fixed_to_int(pointer->x);
	int y = wl_fixed_to_int(pointer->y);

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
	} else {
		scroll_mask |= POINTER_SCROLL_DOWN;
	}

	if (horizontal_steps < 0) {
		horizontal_steps *= -1;
		scroll_mask |= POINTER_SCROLL_LEFT;
	} else {
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

void on_keyboard_event(struct keyboard_collection* collection,
		struct keyboard* keyboard, uint32_t key, bool is_pressed)
{
	struct vnc_client* client = collection->userdata;

	// TODO handle multiple symbols
	xkb_keysym_t symbol = xkb_state_key_get_one_sym(keyboard->state, key);

	char name[256];
	xkb_keysym_get_name(symbol, name, sizeof(name));

	vnc_client_send_keyboard_event(client, symbol, is_pressed);
}

int on_vnc_client_alloc_fb(struct vnc_client* client)
{
	assert(!window); // TODO: Support resizing

	int width = vnc_client_get_width(client);
	int height = vnc_client_get_height(client);
	int stride = vnc_client_get_stride(client);

	window = window_create(app_id, vnc_client_get_desktop_name(client));
	if (!window)
		return -1;

	window->buffer = buffer_create(width, height, stride, wl_shm_format);
	vnc_client_set_fb(client, window->buffer->pixels);
	return 0;
}

void on_vnc_client_update_fb(struct vnc_client* client)
{
	if (!pixman_region_not_empty(&client->damage))
		return;

	// TODO: Make sure that the buffer is released at this point, or make
	// this a side-buffer and copy damaged regions into double buffers.
	window_attach(window, 0, 0);

	int n_rects = 0;
	struct pixman_box16* box = pixman_region_rectangles(&client->damage,
			&n_rects);

	for (int i = 0; i < n_rects; ++i) {
		int x = box[i].x1;
		int y = box[i].y1;
		int width = box[i].x2 - x;
		int height = box[i].y2 - y;

		window_damage(window, x, y, width, height);
	}

	window_commit(window);
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

static int usage(int r)
{
	fprintf(r ? stderr : stdout, "\
Usage: wlvncc <address> [port]\n\
\n\
    -a,--app-id=<name>       Set the app-id of the window. Default: wlvncc\n\
    -c,--compression         Compression level (0 - 9).\n\
    -e,--encodings=<list>    Set allowed encodings, comma separated list.\n\
                             Supported values: tight, zrle, ultra, copyrect,\n\
                             hextile, zlib, corre, rre, raw.\n\
    -h,--help                Get help.\n\
    -n,--hide-cursor         Hide the client-side cursor.\n\
    -q,--quality             Quality level (0 - 9).\n\
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
	static const char* shortopts = "a:q:c:e:hn";

	static const struct option longopts[] = {
		{ "app-id", required_argument, NULL, 'a' },
		{ "compression", required_argument, NULL, 'c' },
		{ "encodings", required_argument, NULL, 'e' },
		{ "help", no_argument, NULL, 'h' },
		{ "quality", required_argument, NULL, 'q' },
		{ "hide-cursor", no_argument, NULL, 'n' },
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

	struct aml* aml = aml_new();
	if (!aml)
		return 1;

	aml_set_default(aml);

	if (init_signal_handler() < 0)
		goto signal_handler_failure;

	wl_display = wl_display_connect(NULL);
	if (!wl_display)
		goto display_failure;

	if (init_wayland_event_handler() < 0)
		goto event_handler_failure;

	pointers = pointer_collection_new(cursor_type);
	if (!pointers)
		goto pointer_failure;

	pointers->on_frame = on_pointer_event;

	keyboards = keyboard_collection_new();
	if (!keyboards)
		goto keyboards_failure;

	keyboards->on_event = on_keyboard_event;

	wl_registry = wl_display_get_registry(wl_display);
	if (!wl_registry)
		goto registry_failure;

	wl_list_init(&seats);

	wl_registry_add_listener(wl_registry, &registry_listener, wl_display);
	wl_display_roundtrip(wl_display);

	assert(wl_compositor);
	assert(wl_shm);
	assert(xdg_wm_base);

	wl_shm_add_listener(wl_shm, &shm_listener, NULL);
	xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
	wl_display_roundtrip(wl_display);
	wl_display_roundtrip(wl_display);

	struct vnc_client* vnc = vnc_client_create();
	if (!vnc)
		goto vnc_failure;

	vnc->alloc_fb = on_vnc_client_alloc_fb;
	vnc->update_fb = on_vnc_client_update_fb;

	if (vnc_client_set_pixel_format(vnc, wl_shm_format) < 0) {
		fprintf(stderr, "Unsupported pixel format\n");
		goto vnc_setup_failure;
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

	pointers->userdata = vnc;
	keyboards->userdata = vnc;

	wl_display_dispatch(wl_display);

	while (do_run) {
		wl_display_flush(wl_display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	rc = 0;
	if (window)
		window_destroy(window);
vnc_setup_failure:
	vnc_client_destroy(vnc);
vnc_failure:
	seat_list_destroy(&seats);
	wl_compositor_destroy(wl_compositor);
	wl_shm_destroy(wl_shm);
	xdg_wm_base_destroy(xdg_wm_base);

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
