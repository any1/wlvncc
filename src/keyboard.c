/*
 * Copyright (c) 2020 - 2024 Andri Yngvason
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
#include <stdint.h>
#include <assert.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <aml.h>

#include "keyboard.h"
#include "seat.h"

#define KEYBOARD_REPEAT_DEFAULT_RATE 25
#define KEYBOARD_REPEAT_DEFAULT_DELAY 660
#define KEYBOARD_REPEAT_MIN_PERIOD 10000

enum keyboard_led_state {
	KEYBOARD_LED_SCROLL_LOCK = 1 << 0,
	KEYBOARD_LED_NUM_LOCK = 1 << 1,
	KEYBOARD_LED_CAPS_LOCK = 1 << 2,
};

struct keyboard* keyboard_new(struct wl_keyboard* wl_keyboard,
		struct seat* seat)
{
	struct keyboard* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->wl_keyboard = wl_keyboard;
	self->seat = seat;
	self->context = xkb_context_new(0);

	return self;
}

uint32_t* keyboard_get_pressed_key(struct keyboard* keyboard, uint32_t key)
{
	for (int i = 0; i < PRESSED_KEYS_MAX; i += 1) {
		if (key == keyboard->pressed_keys[i]) {
			return &keyboard->pressed_keys[i];
		};
	}
	return NULL;
}

void keyboard_add_pressed_key(struct keyboard* keyboard, uint32_t key)
{
	if (keyboard_get_pressed_key(keyboard, key))
		return;
	uint32_t* pressed_key = keyboard_get_pressed_key(keyboard, 0);
	if (!pressed_key) {
		fprintf(stderr, "Oops, pressed too many keys.\n");
		return;
	}
	*pressed_key = key;
}

void keyboard_remove_pressed_key(struct keyboard* keyboard, uint32_t key)
{
	uint32_t* pressed_key = keyboard_get_pressed_key(keyboard, key);
	if (!pressed_key)
		return;
	*pressed_key = 0;
}

void keyboard_destroy(struct keyboard* self)
{
	if (self->state)
		xkb_state_unref(self->state);

	if (self->keymap)
		xkb_keymap_unref(self->keymap);

	xkb_context_unref(self->context);
	wl_keyboard_destroy(self->wl_keyboard);
	free(self);
}

static bool key_is_modifier(xkb_keysym_t symbol)
{
	switch (symbol) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Shift_Lock:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Level5_Shift:
	case XKB_KEY_Mode_switch:
	case XKB_KEY_Num_Lock:
		return true;
	}

	return false;
}

static uint64_t keyboard_repeat_period_us(int32_t rate)
{
	if (rate <= 0)
		return 0;

	uint64_t period = 1000000 / (uint64_t)rate;
	return period < KEYBOARD_REPEAT_MIN_PERIOD ?
		KEYBOARD_REPEAT_MIN_PERIOD : period;
}

static void keyboard_repeat_stop(struct keyboard_collection* self)
{
	self->repeat_keyboard = NULL;
	self->repeat_key = 0;

	struct aml* aml = aml_get_default();
	if (aml_is_started(aml, self->repeat_timer))
		aml_stop(aml, self->repeat_timer);
}

static void keyboard_repeat_tick(struct aml_timer* timer)
{
	struct keyboard_collection* self = aml_get_userdata(timer);

	uint64_t period = keyboard_repeat_period_us(self->repeat_rate);
	if (!period || !self->repeat_keyboard || !self->repeat_key) {
		keyboard_repeat_stop(self);
		return;
	}

	self->on_event(self, self->repeat_keyboard, self->repeat_key, true);

	struct aml* aml = aml_get_default();
	if (aml_is_started(aml, timer))
		aml_stop(aml, timer);
	aml_set_duration(timer, period);
	aml_start(aml, timer);
}

static void keyboard_repeat_start(struct keyboard_collection* self,
		struct keyboard* keyboard, uint32_t key)
{
	uint64_t period = keyboard_repeat_period_us(self->repeat_rate);
	if (!period) {
		keyboard_repeat_stop(self);
		return;
	}

	struct aml* aml = aml_get_default();
	if (aml_is_started(aml, self->repeat_timer))
		aml_stop(aml, self->repeat_timer);

	self->repeat_keyboard = keyboard;
	self->repeat_key = key;

	int32_t delay = self->repeat_delay > 0 ? self->repeat_delay : 1;
	aml_set_duration(self->repeat_timer, (uint64_t)delay * 1000);
	aml_start(aml, self->repeat_timer);
}

struct keyboard_collection* keyboard_collection_new(void)
{
	struct keyboard_collection* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	wl_list_init(&self->keyboards);
	self->repeat_rate = KEYBOARD_REPEAT_DEFAULT_RATE;
	self->repeat_delay = KEYBOARD_REPEAT_DEFAULT_DELAY;

	self->repeat_timer = aml_timer_new(
			(uint64_t)KEYBOARD_REPEAT_DEFAULT_DELAY * 1000,
			keyboard_repeat_tick, self, NULL);
	if (!self->repeat_timer) {
		free(self);
		return NULL;
	}

	return self;
}

void keyboard_collection_destroy(struct keyboard_collection* self)
{
	keyboard_repeat_stop(self);
	aml_unref(self->repeat_timer);

	struct keyboard* keyboard;
	struct keyboard* tmp;

	wl_list_for_each_safe(keyboard, tmp, &self->keyboards, link) {
		wl_list_remove(&keyboard->link);
		keyboard_destroy(keyboard);
	}

	free(self);
}

struct keyboard* keyboard_collection_find_wl_keyboard(
		struct keyboard_collection* self, struct wl_keyboard* wl_keyboard)
{
	struct keyboard* keyboard;
	wl_list_for_each(keyboard, &self->keyboards, link)
		if (keyboard->wl_keyboard == wl_keyboard)
			return keyboard;

	return NULL;
}

static void keyboard_keymap(void* data, struct wl_keyboard* wl_keyboard,
		enum wl_keyboard_keymap_format format, int32_t fd,
		uint32_t size)
{
	struct keyboard_collection* self = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(self, wl_keyboard);
	assert(keyboard);

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		return;

	char* buffer = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	assert(buffer);

	if (keyboard->state)
		xkb_state_unref(keyboard->state);

	if (keyboard->keymap)
		xkb_keymap_unref(keyboard->keymap);

	keyboard->keymap = xkb_keymap_new_from_string(keyboard->context,
			buffer, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	assert(keyboard->keymap);

	munmap(buffer, size);

	keyboard->state = xkb_state_new(keyboard->keymap);
	assert(keyboard->state);
}

static void handle_key(struct keyboard_collection* self, struct keyboard* keyboard,
		uint32_t key, enum xkb_key_direction dir)
{
	xkb_state_update_key(keyboard->state, key, dir);

	if (dir == XKB_KEY_DOWN) {
		keyboard_add_pressed_key(keyboard, key);

		xkb_keysym_t symbol =
			xkb_state_key_get_one_sym(keyboard->state, key);
		if (!key_is_modifier(symbol))
			keyboard_repeat_start(self, keyboard, key);
	} else {
		keyboard_remove_pressed_key(keyboard, key);

		if (self->repeat_keyboard == keyboard &&
				self->repeat_key == key)
			keyboard_repeat_stop(self);
	}

	self->on_event(self, keyboard, key, dir == XKB_KEY_DOWN);
}

static void keyboard_enter(void* data, struct wl_keyboard* wl_keyboard,
		uint32_t serial, struct wl_surface* surface,
		struct wl_array* keys)
{
	struct keyboard_collection* collection = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(collection, wl_keyboard);
	keyboard->seat->last_serial = serial;
	keyboard->waiting_for_modifiers = true;

	uint32_t* key;
	wl_array_for_each(key, keys) {
		handle_key(data, keyboard, *key + 8, XKB_KEY_DOWN);
	}
}

static void keyboard_leave(void* data, struct wl_keyboard* wl_keyboard,
		uint32_t serial, struct wl_surface* surface)
{
	struct keyboard_collection* collection = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(collection, wl_keyboard);

	for (int i = 0; i < PRESSED_KEYS_MAX; i += 1) {
		if (!keyboard->pressed_keys[i])
			continue;
		handle_key(collection, keyboard, keyboard->pressed_keys[i], XKB_KEY_UP);
	};
}

static enum xkb_key_direction xbk_key_direction_from_wl_keyboard_key_state(
		enum wl_keyboard_key_state state)
{
	switch (state) {
	case WL_KEYBOARD_KEY_STATE_PRESSED:
		return XKB_KEY_DOWN;
	case WL_KEYBOARD_KEY_STATE_RELEASED:
		return XKB_KEY_UP;
#ifdef WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION
	case WL_KEYBOARD_KEY_STATE_REPEATED:
		break;
#endif
	}

	abort();
	return 0;
}

static void keyboard_key(void* data, struct wl_keyboard* wl_keyboard,
		uint32_t serial, uint32_t t, uint32_t key,
		enum wl_keyboard_key_state state)
{
	struct keyboard_collection* self = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(self, wl_keyboard);
	assert(keyboard);
	keyboard->seat->last_serial = serial;

#ifdef WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION
	if (state == WL_KEYBOARD_KEY_STATE_REPEATED) {
		self->on_event(self, keyboard, key + 8, true);
		return;
	}
#endif

	enum xkb_key_direction dir =
		xbk_key_direction_from_wl_keyboard_key_state(state);
	handle_key(self, keyboard, key + 8, dir);
}

static void keyboard_toggle_key(struct keyboard* self, uint32_t code)
{
	struct keyboard_collection* collection = self->collection;
	collection->on_event(collection, self, code, 1);
	collection->on_event(collection, self, code, 0);
}

static void keyboard_sync_led_state(struct keyboard* self)
{
	uint32_t leds = 0;
	if (xkb_state_led_name_is_active(self->state, XKB_LED_NAME_SCROLL))
		leds |= KEYBOARD_LED_SCROLL_LOCK;
	if (xkb_state_led_name_is_active(self->state, XKB_LED_NAME_NUM))
		leds |= KEYBOARD_LED_NUM_LOCK;
	if (xkb_state_led_name_is_active(self->state, XKB_LED_NAME_CAPS))
		leds |= KEYBOARD_LED_CAPS_LOCK;

	uint32_t diff = self->led_state ^ leds;
	self->led_state = leds;
	if (!self->waiting_for_modifiers || !diff)
		return;

	if (diff & KEYBOARD_LED_SCROLL_LOCK)
		keyboard_toggle_key(self, KEY_SCROLLLOCK + 8);
	if (diff & KEYBOARD_LED_NUM_LOCK)
		keyboard_toggle_key(self, KEY_NUMLOCK + 8);
	if (diff & KEYBOARD_LED_CAPS_LOCK)
		keyboard_toggle_key(self, KEY_CAPSLOCK + 8);
}

static void keyboard_modifiers(void* data, struct wl_keyboard* wl_keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group)
{
	struct keyboard_collection* self = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(self, wl_keyboard);
	assert(keyboard);
	keyboard->seat->last_serial = serial;

	xkb_state_update_mask(keyboard->state, depressed, latched, locked, 0, 0,
			group);

	keyboard_sync_led_state(keyboard);
	keyboard->waiting_for_modifiers = false;
}

static void keyboard_repeat_info(void* data, struct wl_keyboard* wl_keyboard,
		int32_t rate, int32_t delay)
{
	struct keyboard_collection* self = data;
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(self, wl_keyboard);

	if (!keyboard)
		return;

	self->repeat_rate = rate;
	self->repeat_delay = delay;

	if (self->repeat_keyboard == keyboard && self->repeat_key &&
			keyboard_repeat_period_us(rate))
		keyboard_repeat_start(self, keyboard, self->repeat_key);
	else
		keyboard_repeat_stop(self);
}

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

int keyboard_collection_add_wl_keyboard(struct keyboard_collection* self,
		struct wl_keyboard* wl_keyboard, struct seat* seat)
{
	struct keyboard* keyboard = keyboard_new(wl_keyboard, seat);
	if (!keyboard)
		return -1;

	keyboard->collection = self;
	wl_list_insert(&self->keyboards, &keyboard->link);
	wl_keyboard_add_listener(keyboard->wl_keyboard, &keyboard_listener, self);
	return 0;
}

void keyboard_collection_remove_wl_keyboard(struct keyboard_collection* self,
	struct wl_keyboard* wl_keyboard)
{
	struct keyboard* keyboard =
		keyboard_collection_find_wl_keyboard(self, wl_keyboard);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}
