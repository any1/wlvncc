#pragma once

#include <stdbool.h>
#include <pixman.h>
#include <wayland-client.h>
#include <rfb/rfbclient.h>

struct vnc_client {
	rfbClient* client;

	int (*alloc_fb)(struct vnc_client*);
	void (*update_fb)(struct vnc_client*);

	void* userdata;
	struct pixman_region16 damage;
};

struct vnc_client* vnc_client_create(void);
void vnc_client_destroy(struct vnc_client* self);

int vnc_client_connect(struct vnc_client* self, const char* address, int port);

int vnc_client_set_pixel_format(struct vnc_client* self,
		enum wl_shm_format format);

int vnc_client_get_fd(const struct vnc_client* self);
int vnc_client_get_width(const struct vnc_client* self);
int vnc_client_get_height(const struct vnc_client* self);
int vnc_client_get_stride(const struct vnc_client* self);
void* vnc_client_get_fb(const struct vnc_client* self);
void vnc_client_set_fb(struct vnc_client* self, void* fb);
const char* vnc_client_get_desktop_name(const struct vnc_client* self);
int vnc_client_process(struct vnc_client* self);
void vnc_client_send_pointer_event(struct vnc_client* self, int x, int y,
		uint32_t button_mask);
void vnc_client_send_keyboard_event(struct vnc_client* self, uint32_t symbol,
		bool is_pressed);
