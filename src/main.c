#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <aml.h>
#include <wayland-client.h>
#include <rfb/rfbclient.h>

#include "xdg-shell.h"
#include "shm.h"

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
	bool is_attached;
};

static struct wl_display* wl_display;
static struct wl_registry* wl_registry;
static struct wl_compositor* wl_compositor;
static struct wl_shm* wl_shm;
static struct xdg_wm_base* xdg_wm_base;

static enum wl_shm_format wl_shm_format;
static bool have_format = false;

static bool do_run = true;

struct window* window = NULL;

static void registry_add(void* data, struct wl_registry* registry, uint32_t id,
		const char* interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0) {
		wl_compositor = wl_registry_bind(registry, id,
				&wl_compositor_interface, 1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	}
}

void registry_remove(void* data, struct wl_registry* registry, uint32_t id)
{
	// Nothing to do here
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
	case WL_SHM_FORMAT_ARGB8888:
	case WL_SHM_FORMAT_ABGR8888:
	case WL_SHM_FORMAT_RGBA8888:
	case WL_SHM_FORMAT_BGRA8888:
		wl_shm_format = format;
		have_format = true;
	}

	// TODO: Try to get a preferred format?
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

static struct window* window_create(const char* title)
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

rfbBool rfb_client_alloc_fb(rfbClient* cl)
{
	int stride = cl->width * 4; // TODO

	assert(!window); // TODO

	window = window_create(cl->desktopName);
	if (!window)
		return FALSE;

	window->buffer = buffer_create(cl->width, cl->height, stride,
			wl_shm_format);

	cl->frameBuffer = window->buffer->pixels;

	return TRUE;
}

void rfb_client_update_box(rfbClient* cl, int x, int y, int width, int height)
{
	if (!window->is_attached)
		window_attach(window, 0, 0);

	window_damage(window, x, y, width, height);
}

void rfb_client_finish_update(rfbClient* cl)
{
	window_commit(window);
}

void on_rfb_client_server_event(void* obj)
{
	rfbClient* cl = aml_get_userdata(obj);
	if (!HandleRFBServerMessage(cl))
		do_run = false;
}

static rfbClient* rfb_client_create(int* argc, char* argv[])
{
	int bits_per_sample = 8;
	int samples_per_pixel = 3;
	int bytes_per_pixel = 4;

	rfbClient* cl = rfbGetClient(bits_per_sample, samples_per_pixel,
			bytes_per_pixel);
	if (!cl)
		return NULL;

	// TODO: Set correct pixel format here

	cl->MallocFrameBuffer = rfb_client_alloc_fb;
	cl->GotFrameBufferUpdate = rfb_client_update_box;
	cl->FinishedFrameBufferUpdate = rfb_client_finish_update;

	if (!rfbInitClient(cl, argc, argv))
		goto failure;

	int fd = cl->sock;

	struct aml_handler* handler;
	handler = aml_handler_new(fd, on_rfb_client_server_event, cl, NULL);
	if (!handler)
		goto failure;

	int rc = aml_start(aml_get_default(), handler);
	aml_unref(handler);

	if (rc < 0)
		goto failure;

	return cl;

failure:
	rfbClientCleanup(cl);
	return NULL;
}

static void rfb_client_destroy(rfbClient* cl)
{
	rfbClientCleanup(cl);
}

int main(int argc, char* argv[])
{
	int rc = -1;

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

	wl_registry = wl_display_get_registry(wl_display);
	if (!wl_registry)
		goto registry_failure;

	wl_registry_add_listener(wl_registry, &registry_listener, wl_display);
	wl_display_roundtrip(wl_display);

	assert(wl_compositor);
	assert(wl_shm);
	assert(xdg_wm_base);

	wl_shm_add_listener(wl_shm, &shm_listener, NULL);
	xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
	wl_display_roundtrip(wl_display);

	rfbClient* vnc = rfb_client_create(&argc, argv);
	if (!vnc)
		goto vnc_failure;


	wl_display_dispatch(wl_display);

	while (do_run) {
		wl_display_flush(wl_display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	rc = 0;
	if (window)
		window_destroy(window);
	rfb_client_destroy(vnc);
vnc_failure:
	wl_compositor_destroy(wl_compositor);
	wl_shm_destroy(wl_shm);
	xdg_wm_base_destroy(xdg_wm_base);

	wl_registry_destroy(wl_registry);
registry_failure:
event_handler_failure:
	wl_display_disconnect(wl_display);
display_failure:
signal_handler_failure:
	aml_unref(aml);
	printf("Exiting...\n");
	return rc;
}
