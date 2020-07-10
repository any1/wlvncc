#pragma once

#include <stdbool.h>
#include <wayland-client.h>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct keyboard {
	struct wl_keyboard* wl_keyboard;
	struct wl_list link;

	struct xkb_context* context;
	struct xkb_keymap* keymap;
	struct xkb_state* state;
};

struct keyboard_collection {
	struct wl_list keyboards;
	void (*on_event)(struct keyboard_collection*, struct keyboard*,
			uint32_t key, bool is_pressed);
	void* userdata;
};

struct keyboard_collection* keyboard_collection_new(void);
void keyboard_collection_destroy(struct keyboard_collection*);

int keyboard_collection_add_wl_keyboard(struct keyboard_collection* self,
		struct wl_keyboard* wl_keyboard);
void keyboard_collection_remove_wl_keyboard(struct keyboard_collection* self,
		struct wl_keyboard* wl_keyboard);
