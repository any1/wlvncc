#pragma once

#include <wayland-client.h>

enum pointer_button_mask {
	POINTER_BUTTON_LEFT = 1 << 0,
	POINTER_BUTTON_MIDDLE = 1 << 1,
	POINTER_BUTTON_RIGHT = 1 << 2,
	// TODO: More buttons
};

struct pointer {
	struct wl_pointer* wl_pointer;
	struct wl_list link;

	enum pointer_button_mask pressed;
	wl_fixed_t x, y;
};

struct pointer_collection {
	struct wl_list pointers;
	void (*on_frame)(struct pointer_collection*, struct pointer*);
	void* userdata;
};

struct pointer_collection* pointer_collection_new(void);
void pointer_collection_destroy(struct pointer_collection*);

int pointer_collection_add_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer);
void pointer_collection_remove_wl_pointer(struct pointer_collection* self,
		struct wl_pointer* wl_pointer);
