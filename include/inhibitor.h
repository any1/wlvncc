#pragma once

#include <stdbool.h>

#include "keyboard-shortcuts-inhibit-unstable-v1.h"

struct shortcuts_inhibitor {
	//TODO add the inhibitor toggle key to the struct ?
	struct zwp_keyboard_shortcuts_inhibit_manager_v1* manager;
	struct zwp_keyboard_shortcuts_inhibitor_v1* inhibitor;

	struct wl_surface* surface;
	struct wl_seat* seat;
};

struct shortcuts_inhibitor* inhibitor_new(struct zwp_keyboard_shortcuts_inhibit_manager_v1*);
void inhibitor_destroy(struct shortcuts_inhibitor*);
bool inhibitor_init(struct shortcuts_inhibitor*, struct wl_surface*, struct wl_seat*);

bool inhibitor_is_inhibited(const struct shortcuts_inhibitor*);
void inhibitor_inhibit(struct shortcuts_inhibitor*);
void inhibitor_release(struct shortcuts_inhibitor*);
void inhibitor_toggle(struct shortcuts_inhibitor*);
