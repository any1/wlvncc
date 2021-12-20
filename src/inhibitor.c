#include <stdlib.h>

#include "inhibitor.h"
#include <stdio.h>

struct shortcuts_inhibitor* inhibitor_new(struct zwp_keyboard_shortcuts_inhibit_manager_v1* manager)
{
	struct shortcuts_inhibitor* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->manager = manager;
	self->inhibitor = NULL;
	self->surface = NULL;
	self->seat = NULL;
	return self;
}

bool inhibitor_init(struct shortcuts_inhibitor* self, struct wl_surface* surface,
		struct wl_seat* seat)
{
	if ( self->surface != NULL || self->seat != NULL)
		return false;

	self->surface = surface;
	self->seat = seat;
	return true;
}

void inhibitor_destroy(struct shortcuts_inhibitor* self)
{
	if (self->inhibitor)
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(self->inhibitor);
	if (self->manager)
		zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(self->manager);
}

bool inhibitor_is_inhibited(const struct shortcuts_inhibitor* self)
{
	return self->inhibitor != NULL;
}

void inhibitor_inhibit(struct shortcuts_inhibitor* self)
{
	// The compositor does not support the wlr shortcuts inhibitor protocol
	if (self->manager == NULL)
		return;
	if (!inhibitor_is_inhibited(self))
		self->inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
				self->manager, self->surface, self->seat);
}

void inhibitor_release(struct shortcuts_inhibitor* self)
{
	if (inhibitor_is_inhibited(self)) {
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(self->inhibitor);
		self->inhibitor = NULL;
	}
}

void inhibitor_toggle(struct shortcuts_inhibitor* self)
{
	if (inhibitor_is_inhibited(self)) {
		inhibitor_release(self);
	} else {
		inhibitor_inhibit(self);
	}
}
