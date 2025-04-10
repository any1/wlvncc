#include <stdlib.h>

#include "inhibitor.h"
#include "seat.h"
#include <stdio.h>
#include <wayland-client.h>
#include <assert.h>

void inhibitor_active(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *zwp_keyboard_shortcuts_inhibitor_v1)
{
	struct shortcuts_seat_inhibitor* seat_inhibitor = data;
	seat_inhibitor->active = true;
}

void inhibitor_inactive(void *data,
		struct zwp_keyboard_shortcuts_inhibitor_v1 *zwp_keyboard_shortcuts_inhibitor_v1)
{
	struct shortcuts_seat_inhibitor* seat_inhibitor = data;
	seat_inhibitor->active = false;
}

static struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
	.active = inhibitor_active,
	.inactive = inhibitor_inactive,
};

struct shortcuts_inhibitor* inhibitor_new(struct zwp_keyboard_shortcuts_inhibit_manager_v1* manager)
{
	struct shortcuts_inhibitor* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->manager = manager;
	self->surface = NULL;
	wl_list_init(&self->seat_inhibitors);

	return self;
}

struct shortcuts_seat_inhibitor* seat_inhibitor_new(struct seat* seat)
{
	struct shortcuts_seat_inhibitor* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->seat = seat;
	self->inhibitor = NULL;
	self->active = true;

	return self;
}

struct shortcuts_seat_inhibitor* seat_inhibitor_find_by_seat(struct wl_list* list, struct seat* seat)
{
	struct shortcuts_seat_inhibitor* seat_inhibitor;

	wl_list_for_each(seat_inhibitor, list, link) {
		if (seat == seat_inhibitor->seat) {
			return seat_inhibitor;
		}
	}

	return NULL;
}

bool inhibitor_init(struct shortcuts_inhibitor* self, struct wl_surface* surface,
		struct wl_list* seats)
{
	if (!self)
		return false;
	if (self->surface)
		return false;

	self->surface = surface;

	struct shortcuts_seat_inhibitor* seat_inhibitor;
	wl_list_for_each(seat_inhibitor, &self->seat_inhibitors, link)
		inhibitor_inhibit(self, seat_inhibitor->seat);

	return true;
}

void seat_inhibitor_destroy(struct shortcuts_seat_inhibitor* self)
{
	if (!self)
		return;

	if (self->inhibitor)
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(self->inhibitor);
	free(self);
}

void inhibitor_destroy(struct shortcuts_inhibitor* self)
{
	if (!self)
		return;

	struct shortcuts_seat_inhibitor* seat_inhibitor;
	struct shortcuts_seat_inhibitor* tmp;

	wl_list_for_each_safe(seat_inhibitor, tmp, &self->seat_inhibitors, link)
		seat_inhibitor_destroy(seat_inhibitor);
	zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(self->manager);
	free(self);
}

bool inhibitor_is_inhibited(struct shortcuts_inhibitor* self,
		struct seat* seat)
{
	if (!self)
		return false;

	struct shortcuts_seat_inhibitor* seat_inhibitor = seat_inhibitor_find_by_seat(&self->seat_inhibitors, seat);
	assert(seat_inhibitor);

	return seat_inhibitor->inhibitor != NULL;
}

void inhibitor_inhibit(struct shortcuts_inhibitor* self, struct seat* seat)
{
	if (!self)
		return;
	if (inhibitor_is_inhibited(self, seat))
		return;

	struct shortcuts_seat_inhibitor* seat_inhibitor = seat_inhibitor_find_by_seat(&self->seat_inhibitors, seat);
	assert(seat_inhibitor);

	if (!seat_inhibitor->active)
		return;

	seat_inhibitor->inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
			self->manager, self->surface, seat_inhibitor->seat->wl_seat);
	zwp_keyboard_shortcuts_inhibitor_v1_add_listener(seat_inhibitor->inhibitor, &inhibitor_listener, seat_inhibitor);
}

void inhibitor_release(struct shortcuts_inhibitor* self, struct seat* seat)
{
	if (!self)
		return;
	if (!inhibitor_is_inhibited(self, seat))
		return;

	struct shortcuts_seat_inhibitor* seat_inhibitor = seat_inhibitor_find_by_seat(&self->seat_inhibitors, seat);
	assert(seat_inhibitor);

	if (!seat_inhibitor->active)
		return;

	zwp_keyboard_shortcuts_inhibitor_v1_destroy(seat_inhibitor->inhibitor);
	seat_inhibitor->inhibitor = NULL;
}

void inhibitor_toggle(struct shortcuts_inhibitor* self, struct seat* seat)
{
	if (!self)
		return;

	if (inhibitor_is_inhibited(self, seat)) {
		inhibitor_release(self, seat);
	} else {
		inhibitor_inhibit(self, seat);
	}
}

void inhibitor_add_seat(struct shortcuts_inhibitor* self, struct seat* seat)
{
	if (!self)
		return;

	struct shortcuts_seat_inhibitor* seat_inhibitor = seat_inhibitor_find_by_seat(&self->seat_inhibitors, seat);
	if (seat_inhibitor)
		return;

	seat_inhibitor = seat_inhibitor_new(seat);
	wl_list_insert(&self->seat_inhibitors, &seat_inhibitor->link);

	if (self->surface)
		inhibitor_inhibit(self, seat);
}

void inhibitor_remove_seat(struct shortcuts_inhibitor* self, struct seat* seat)
{
	if (!self)
		return;

	struct shortcuts_seat_inhibitor* seat_inhibitor = seat_inhibitor_find_by_seat(&self->seat_inhibitors, seat);
	if (!seat_inhibitor)
		return;

	wl_list_remove(&seat_inhibitor->link);
	seat_inhibitor_destroy(seat_inhibitor);
}
