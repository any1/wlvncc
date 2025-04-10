#pragma once

#include <stdbool.h>
#include "seat.h"

#include "keyboard-shortcuts-inhibit-unstable-v1.h"

struct shortcuts_inhibitor {
	//TODO add the inhibitor toggle key to the struct ?
	struct zwp_keyboard_shortcuts_inhibit_manager_v1* manager;

	struct wl_surface* surface;
	struct wl_list seat_inhibitors;
};

struct shortcuts_seat_inhibitor {
	struct wl_list link;

	struct seat* seat;
	bool active;

	struct zwp_keyboard_shortcuts_inhibitor_v1* inhibitor;
};

struct shortcuts_inhibitor* inhibitor_new(struct zwp_keyboard_shortcuts_inhibit_manager_v1*);
struct shortcuts_seat_inhibitor* seat_inhibitor_find_by_seat(struct wl_list*, struct seat*);
void inhibitor_destroy(struct shortcuts_inhibitor*);
bool inhibitor_init(struct shortcuts_inhibitor*, struct wl_surface*, struct wl_list*);
void inhibitor_add_seat(struct shortcuts_inhibitor*, struct seat*);
void inhibitor_remove_seat(struct shortcuts_inhibitor*, struct seat*);

bool inhibitor_is_inhibited(struct shortcuts_inhibitor*, struct seat* seat);
void inhibitor_inhibit(struct shortcuts_inhibitor*, struct seat* seat);
void inhibitor_release(struct shortcuts_inhibitor*, struct seat* seat);
void inhibitor_toggle(struct shortcuts_inhibitor*, struct seat* seat);
