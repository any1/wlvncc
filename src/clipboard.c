/*
 * Copyright (c) 2026
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <aml.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-client.h>

#include "clipboard.h"
#include "seat.h"

#define CLIPBOARD_MAX_READ (16 * 1024 * 1024)

struct clipboard_blob {
	char* data;
	size_t len;
};

struct clipboard_source {
	struct clipboard* clipboard;
	struct wl_data_source* source;
	struct clipboard_blob utf8;
	struct clipboard_blob latin1;
};

struct clipboard_offer {
	struct wl_list link;
	struct wl_data_offer* offer;
	char* mime;
};

struct clipboard_read {
	int fd;
	struct aml_handler* handler;
	bool started;
	struct clipboard_offer* offer;
	struct clipboard_blob buf;
};

/* An in-flight asynchronous write serving one consumer's paste request.
 * Owns a private copy of the payload so it can outlive the data source
 * (which may be cancelled/replaced while a slow consumer is still reading). */
struct clipboard_write {
	struct wl_list link;
	int fd;
	struct aml_handler* handler;
	char* data;
	size_t len;
	size_t offset;
};

struct clipboard {
	struct wl_display* display;
	struct wl_data_device_manager* manager;
	struct wl_data_device* data_device;
	struct seat* seat;

	struct clipboard_source* source;
	struct wl_list offers;
	struct wl_list writes;
	struct clipboard_read read;

	struct clipboard_blob last_remote_latin1;
	struct clipboard_blob last_sent_latin1;

	/* True while our own data source is the active selection. Driven by
	 * set_selection (set) and wl_data_source.cancelled (cleared), so it
	 * tracks real protocol ownership rather than assuming a single echo
	 * follows each set_selection. This lets us skip our own selection
	 * echoes without ever swallowing a genuine external selection. */
	bool owns_selection;

	clipboard_local_selection_fn on_local_selection;
	void* userdata;
};

static void blob_clear(struct clipboard_blob* blob)
{
	free(blob->data);
	blob->data = NULL;
	blob->len = 0;
}

static int blob_set(struct clipboard_blob* blob, const char* data, size_t len)
{
	char* copy = NULL;
	if (len) {
		copy = malloc(len);
		if (!copy)
			return -1;
		memcpy(copy, data, len);
	}

	free(blob->data);
	blob->data = copy;
	blob->len = len;
	return 0;
}

static bool blob_equals(const struct clipboard_blob* blob,
		const char* data, size_t len)
{
	return blob->len == len && (len == 0 || memcmp(blob->data, data, len) == 0);
}

static char* latin1_to_utf8(const char* in, size_t len, size_t* out_len)
{
	size_t n = 0;
	for (size_t i = 0; i < len; ++i)
		n += ((unsigned char)in[i] < 0x80) ? 1 : 2;

	char* out = malloc(n ? n : 1);
	if (!out)
		return NULL;

	char* p = out;
	for (size_t i = 0; i < len; ++i) {
		unsigned char c = in[i];
		if (c < 0x80) {
			*p++ = c;
		} else {
			*p++ = 0xc0 | (c >> 6);
			*p++ = 0x80 | (c & 0x3f);
		}
	}

	*out_len = n;
	return out;
}

static uint32_t utf8_decode_one(const unsigned char** p, const unsigned char* end)
{
	const unsigned char* s = *p;
	if (s >= end)
		return 0;

	uint32_t cp;
	if (s[0] < 0x80) {
		*p = s + 1;
		return s[0];
	}

	if ((s[0] & 0xe0) == 0xc0 && s + 1 < end &&
			(s[1] & 0xc0) == 0x80) {
		cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
		if (cp >= 0x80) {
			*p = s + 2;
			return cp;
		}
	}

	if ((s[0] & 0xf0) == 0xe0 && s + 2 < end &&
			(s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
		cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) |
			(s[2] & 0x3f);
		if (cp >= 0x800 && !(cp >= 0xd800 && cp <= 0xdfff)) {
			*p = s + 3;
			return cp;
		}
	}

	if ((s[0] & 0xf8) == 0xf0 && s + 3 < end &&
			(s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
			(s[3] & 0xc0) == 0x80) {
		cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
			((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
		if (cp >= 0x10000 && cp <= 0x10ffff) {
			*p = s + 4;
			return cp;
		}
	}

	*p = s + 1;
	return '?';
}

static char* utf8_to_latin1(const char* in, size_t len, size_t* out_len)
{
	char* out = malloc(len ? len : 1);
	if (!out)
		return NULL;

	const unsigned char* p = (const unsigned char*)in;
	const unsigned char* end = p + len;
	char* q = out;

	while (p < end) {
		uint32_t cp = utf8_decode_one(&p, end);
		*q++ = cp <= 0xff ? (char)cp : '?';
	}

	*out_len = q - out;
	return out;
}

static bool mime_is_utf8_text(const char* mime)
{
	return strcasecmp(mime, "text/plain;charset=utf-8") == 0 ||
		strcasecmp(mime, "text/plain;charset=UTF-8") == 0 ||
		strcmp(mime, "UTF8_STRING") == 0;
}

static bool mime_is_text_plain(const char* mime)
{
	return strcasecmp(mime, "text/plain") == 0;
}

static bool mime_is_string(const char* mime)
{
	return strcmp(mime, "STRING") == 0;
}

static void clipboard_source_destroy(struct clipboard_source* source)
{
	if (!source)
		return;

	if (source->source)
		wl_data_source_destroy(source->source);
	blob_clear(&source->utf8);
	blob_clear(&source->latin1);
	free(source);
}

static void data_source_target(void* data, struct wl_data_source* source,
		const char* mime_type)
{
	(void)data;
	(void)source;
	(void)mime_type;
}

static void clipboard_write_destroy(struct clipboard_write* w)
{
	if (!w)
		return;

	wl_list_remove(&w->link);
	if (w->handler) {
		aml_stop(aml_get_default(), w->handler);
		aml_unref(w->handler);
	}
	if (w->fd >= 0)
		close(w->fd);
	free(w->data);
	free(w);
}

/* Drain as much as the pipe will accept without blocking. Returns true when
 * the write is finished (all sent, or the consumer went away / errored). */
static bool clipboard_write_pump(struct clipboard_write* w)
{
	while (w->offset < w->len) {
		ssize_t n = write(w->fd, w->data + w->offset, w->len - w->offset);
		if (n > 0) {
			w->offset += n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return false;
		/* EPIPE or any other error: consumer is gone, give up. */
		return true;
	}
	return true;
}

static void clipboard_writable(struct aml_handler* handler)
{
	struct clipboard_write* w = aml_get_userdata(handler);
	if (clipboard_write_pump(w))
		clipboard_write_destroy(w);
}

static void data_source_send(void* data, struct wl_data_source* source,
		const char* mime_type, int32_t fd)
{
	(void)source;
	struct clipboard_source* src = data;
	struct clipboard* clipboard = src->clipboard;
	const struct clipboard_blob* blob = &src->utf8;

	if (mime_type && mime_is_string(mime_type))
		blob = &src->latin1;

	struct clipboard_write* w = calloc(1, sizeof(*w));
	if (!w) {
		close(fd);
		return;
	}

	w->fd = fd;
	w->len = blob->len;
	if (blob->len) {
		w->data = malloc(blob->len);
		if (!w->data) {
			free(w);
			close(fd);
			return;
		}
		memcpy(w->data, blob->data, blob->len);
	}

	wl_list_insert(&clipboard->writes, &w->link);

	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (clipboard_write_pump(w)) {
		clipboard_write_destroy(w);
		return;
	}

	w->handler = aml_handler_new(w->fd, clipboard_writable, w, NULL);
	if (!w->handler) {
		clipboard_write_destroy(w);
		return;
	}
	aml_set_event_mask(w->handler, AML_EVENT_WRITE);
	if (aml_start(aml_get_default(), w->handler) < 0) {
		aml_unref(w->handler);
		w->handler = NULL;
		clipboard_write_destroy(w);
	}
}

static void data_source_cancelled(void* data, struct wl_data_source* source)
{
	struct clipboard_source* src = data;
	struct clipboard* clipboard = src->clipboard;
	(void)source;

	if (clipboard->source == src) {
		clipboard->source = NULL;
		/* Another client (or NULL) replaced our selection, so we no
		 * longer own it; subsequent selection events are external. */
		clipboard->owns_selection = false;
	}

	src->source = NULL;
	clipboard_source_destroy(src);
}

static const struct wl_data_source_listener data_source_listener = {
	.target = data_source_target,
	.send = data_source_send,
	.cancelled = data_source_cancelled,
};

static void clipboard_offer_destroy(struct clipboard_offer* offer)
{
	if (!offer)
		return;

	wl_list_remove(&offer->link);
	free(offer->mime);
	if (offer->offer)
		wl_data_offer_destroy(offer->offer);
	free(offer);
}

static struct clipboard_offer* clipboard_find_offer(struct clipboard* self,
		struct wl_data_offer* wl_offer)
{
	struct clipboard_offer* offer;
	wl_list_for_each(offer, &self->offers, link) {
		if (offer->offer == wl_offer)
			return offer;
	}
	return NULL;
}

static void data_offer_offer(void* data, struct wl_data_offer* wl_offer,
		const char* mime_type)
{
	(void)wl_offer;
	struct clipboard_offer* offer = data;

	if (mime_is_utf8_text(mime_type)) {
		free(offer->mime);
		offer->mime = strdup(mime_type);
	} else if (!offer->mime && mime_is_text_plain(mime_type)) {
		offer->mime = strdup(mime_type);
	}
}

static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
};

static void clipboard_read_cancel(struct clipboard* self)
{
	if (self->read.handler) {
		if (self->read.started)
			aml_stop(aml_get_default(), self->read.handler);
		aml_unref(self->read.handler);
		self->read.handler = NULL;
		self->read.started = false;
	}

	if (self->read.fd >= 0) {
		close(self->read.fd);
		self->read.fd = -1;
	}

	if (self->read.offer) {
		clipboard_offer_destroy(self->read.offer);
		self->read.offer = NULL;
	}

	blob_clear(&self->read.buf);
}

static int clipboard_read_append(struct clipboard* self, const char* data,
		size_t len)
{
	if (self->read.buf.len + len > CLIPBOARD_MAX_READ)
		return -1;

	char* p = realloc(self->read.buf.data, self->read.buf.len + len);
	if (!p && self->read.buf.len + len)
		return -1;

	memcpy(p + self->read.buf.len, data, len);
	self->read.buf.data = p;
	self->read.buf.len += len;
	return 0;
}

static void clipboard_finish_read(struct clipboard* self)
{
	size_t latin1_len = 0;
	char* latin1 = utf8_to_latin1(self->read.buf.data, self->read.buf.len,
			&latin1_len);
	if (!latin1)
		return;

	if (blob_equals(&self->last_remote_latin1, latin1, latin1_len)) {
		free(latin1);
		return;
	}

	if (blob_set(&self->last_sent_latin1, latin1, latin1_len) < 0) {
		free(latin1);
		return;
	}

	if (self->on_local_selection)
		self->on_local_selection(self->userdata, latin1, latin1_len);

	free(latin1);
}

static void clipboard_readable(struct aml_handler* handler)
{
	struct clipboard* self = aml_get_userdata(handler);
	char buf[4096];

	while (true) {
		ssize_t n = read(self->read.fd, buf, sizeof(buf));
		if (n > 0) {
			if (clipboard_read_append(self, buf, n) < 0) {
				fprintf(stderr, "Clipboard selection is too large; dropping it\n");
				clipboard_read_cancel(self);
				return;
			}
			continue;
		}

		if (n == 0) {
			clipboard_finish_read(self);
			clipboard_read_cancel(self);
			return;
		}

		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		clipboard_read_cancel(self);
		return;
	}
}

static void data_device_data_offer(void* data,
		struct wl_data_device* data_device, struct wl_data_offer* wl_offer)
{
	(void)data_device;
	struct clipboard* self = data;
	struct clipboard_offer* offer = calloc(1, sizeof(*offer));
	if (!offer) {
		wl_data_offer_destroy(wl_offer);
		return;
	}

	offer->offer = wl_offer;
	wl_list_insert(&self->offers, &offer->link);
	wl_data_offer_add_listener(wl_offer, &data_offer_listener, offer);
}

static void data_device_enter(void* data, struct wl_data_device* data_device,
		uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
		struct wl_data_offer* wl_offer)
{
	struct clipboard* self = data;
	(void)data_device;
	(void)serial;
	(void)surface;
	(void)x;
	(void)y;

	/* Drag-and-drop is out of scope. Do not keep DnD offers around. */
	struct clipboard_offer* offer = clipboard_find_offer(self, wl_offer);
	if (offer)
		clipboard_offer_destroy(offer);
}

static void data_device_leave(void* data, struct wl_data_device* data_device)
{
	(void)data;
	(void)data_device;
}

static void data_device_motion(void* data, struct wl_data_device* data_device,
		uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)data_device;
	(void)time;
	(void)x;
	(void)y;
}

static void data_device_drop(void* data, struct wl_data_device* data_device)
{
	(void)data;
	(void)data_device;
}

static void data_device_selection(void* data,
		struct wl_data_device* data_device, struct wl_data_offer* wl_offer)
{
	(void)data_device;
	struct clipboard* self = data;

	if (self->read.offer && self->read.offer->offer == wl_offer)
		return;

	clipboard_read_cancel(self);

	if (!wl_offer)
		return;

	struct clipboard_offer* offer = clipboard_find_offer(self, wl_offer);

	/* If we still own the selection, this event echoes our own source;
	 * never read our own data back (and avoid a self-pipe deadlock for
	 * large payloads). Ownership is cleared via data_source_cancelled
	 * the moment any other client takes the selection, so a real
	 * external selection is never mistaken for our echo. */
	if (self->owns_selection) {
		if (offer)
			clipboard_offer_destroy(offer);
		else
			wl_data_offer_destroy(wl_offer);
		return;
	}

	if (!offer || !offer->mime) {
		if (offer)
			clipboard_offer_destroy(offer);
		else
			wl_data_offer_destroy(wl_offer);
		return;
	}

	self->read.offer = offer;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		clipboard_read_cancel(self);
		return;
	}

	int flags = fcntl(fds[0], F_GETFL, 0);
	if (flags >= 0)
		fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	wl_data_offer_receive(wl_offer, offer->mime, fds[1]);
	close(fds[1]);
	wl_display_flush(self->display);

	self->read.fd = fds[0];
	self->read.handler = aml_handler_new(fds[0], clipboard_readable, self, NULL);
	if (!self->read.handler) {
		clipboard_read_cancel(self);
		return;
	}

	if (aml_start(aml_get_default(), self->read.handler) < 0) {
		clipboard_read_cancel(self);
		return;
	}
	self->read.started = true;
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection,
};

struct clipboard* clipboard_new(struct wl_display* display,
		struct wl_data_device_manager* manager, struct seat* seat,
		clipboard_local_selection_fn on_local_selection, void* userdata)
{
	if (!display || !manager || !seat)
		return NULL;

	struct clipboard* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->display = display;
	self->manager = manager;
	self->seat = seat;
	self->on_local_selection = on_local_selection;
	self->userdata = userdata;
	self->read.fd = -1;
	wl_list_init(&self->offers);
	wl_list_init(&self->writes);

	self->data_device = wl_data_device_manager_get_data_device(manager,
			seat->wl_seat);
	if (!self->data_device)
		goto failure;

	seat->data_device = self->data_device;
	wl_data_device_add_listener(self->data_device, &data_device_listener, self);
	return self;

failure:
	clipboard_destroy(self);
	return NULL;
}

void clipboard_destroy(struct clipboard* self)
{
	if (!self)
		return;

	clipboard_read_cancel(self);

	struct clipboard_write* w;
	struct clipboard_write* w_tmp;
	wl_list_for_each_safe(w, w_tmp, &self->writes, link)
		clipboard_write_destroy(w);

	if (self->source)
		clipboard_source_destroy(self->source);

	struct clipboard_offer* offer;
	struct clipboard_offer* tmp;
	wl_list_for_each_safe(offer, tmp, &self->offers, link)
		clipboard_offer_destroy(offer);

	if (self->seat && self->seat->data_device == self->data_device)
		self->seat->data_device = NULL;

	if (self->data_device)
		wl_data_device_destroy(self->data_device);

	blob_clear(&self->last_remote_latin1);
	blob_clear(&self->last_sent_latin1);
	free(self);
}

void clipboard_set_selection_from_rfb(struct clipboard* self,
		const char* text, size_t len)
{
	if (!self)
		return;

	if (blob_equals(&self->last_sent_latin1, text, len))
		return;

	if (self->source) {
		clipboard_source_destroy(self->source);
		self->source = NULL;
		self->owns_selection = false;
	}

	if (!self->seat->last_serial) {
		fprintf(stderr, "Ignoring remote clipboard: no Wayland input serial yet\n");
		return;
	}

	struct clipboard_source* source = calloc(1, sizeof(*source));
	if (!source)
		return;

	source->clipboard = self;
	if (blob_set(&source->latin1, text, len) < 0)
		goto failure;

	source->utf8.data = latin1_to_utf8(text, len, &source->utf8.len);
	if (!source->utf8.data)
		goto failure;

	source->source = wl_data_device_manager_create_data_source(self->manager);
	if (!source->source)
		goto failure;

	wl_data_source_add_listener(source->source, &data_source_listener, source);
	wl_data_source_offer(source->source, "text/plain;charset=utf-8");
	wl_data_source_offer(source->source, "text/plain");
	wl_data_source_offer(source->source, "UTF8_STRING");
	wl_data_source_offer(source->source, "STRING");

	if (blob_set(&self->last_remote_latin1, text, len) < 0)
		goto failure;

	self->source = source;
	self->owns_selection = true;
	wl_data_device_set_selection(self->data_device, source->source,
			self->seat->last_serial);
	wl_display_flush(self->display);
	return;

failure:
	clipboard_source_destroy(source);
}
