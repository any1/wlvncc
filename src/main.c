#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <aml.h>
#include <wayland-client.h>

#include "xdg-shell.h"

struct window {
	struct wl_surface* surface;
};

static struct wl_display* wl_display;
static struct wl_registry* wl_registry;
static struct wl_compositor* wl_compositor;
static struct wl_shm* wl_shm;
static struct xdg_wm_base* xdg_wm_base;

static bool do_run = true;

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

static void shm_format(void* data, struct wl_shm* shm, uint32_t format)
{
	(void)data;
	(void)wl_shm;

	// TODO: Check formats
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

	// TODO: Create window
	// TODO: Create buffer
	// TODO: Draw to buffer

//	wl_display_dispatch(wl_display);
	while (do_run) {
		wl_display_flush(wl_display);
		aml_poll(aml, -1);
		aml_dispatch(aml);
	}

	rc = 0;
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
