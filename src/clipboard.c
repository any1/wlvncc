/*
 * Copyright (c) 2026
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <aml.h>
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
#include "ext-data-control-v1.h"
#include "seat.h"

#define CLIPBOARD_MAX_READ (16 * 1024 * 1024)

struct clipboard_blob {
	char* data;
	size_t len;
};

struct clipboard_source {
	struct clipboard* clipboard;
	bool is_ext;
	bool is_primary;
	struct wl_data_source* source;
	struct ext_data_control_source_v1* ext_source;
	struct clipboard_blob utf8;
	struct clipboard_blob latin1;
};

struct clipboard_offer {
	struct wl_list link;
	bool is_ext;
	struct wl_data_offer* offer;
	struct ext_data_control_offer_v1* ext_offer;
	char* mime;
};

struct clipboard_read {
	int fd;
	struct aml_handler* handler;
	bool started;
	struct clipboard_offer* offer;
	struct clipboard_blob buf;
};

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
	struct ext_data_control_manager_v1* ext_manager;
	struct wl_data_device* data_device;
	struct ext_data_control_device_v1* ext_device;
	struct seat* seat;

	struct clipboard_source* source;
	struct clipboard_source* primary_source;
	struct wl_list offers;
	struct wl_list writes;
	struct clipboard_read read;

	struct clipboard_blob last_remote_latin1;
	struct clipboard_blob last_sent_latin1;

	bool owns_selection;
	bool owns_primary;

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

	if (source->is_ext) {
		if (source->ext_source)
			ext_data_control_source_v1_destroy(source->ext_source);
	} else if (source->source) {
		wl_data_source_destroy(source->source);
	}

	blob_clear(&source->utf8);
	blob_clear(&source->latin1);
	free(source);
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

static void clipboard_source_send(struct clipboard_source* src,
		const char* mime_type, int32_t fd)
{
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

static void clipboard_source_cancelled(struct clipboard_source* src)
{
	struct clipboard* clipboard = src->clipboard;

	if (clipboard->source == src) {
		clipboard->source = NULL;
		clipboard->owns_selection = false;
	}

	if (clipboard->primary_source == src) {
		clipboard->primary_source = NULL;
		clipboard->owns_primary = false;
	}

	clipboard_source_destroy(src);
}

static void data_source_target(void* data, struct wl_data_source* source,
		const char* mime_type)
{
	(void)data;
	(void)source;
	(void)mime_type;
}

static void data_source_send(void* data, struct wl_data_source* source,
		const char* mime_type, int32_t fd)
{
	(void)source;
	clipboard_source_send(data, mime_type, fd);
}

static void data_source_cancelled(void* data, struct wl_data_source* source)
{
	(void)source;
	clipboard_source_cancelled(data);
}

static const struct wl_data_source_listener data_source_listener = {
	.target = data_source_target,
	.send = data_source_send,
	.cancelled = data_source_cancelled,
};

static void ext_source_send(void* data, struct ext_data_control_source_v1* source,
		const char* mime_type, int32_t fd)
{
	(void)source;
	clipboard_source_send(data, mime_type, fd);
}

static void ext_source_cancelled(void* data,
		struct ext_data_control_source_v1* source)
{
	(void)source;
	clipboard_source_cancelled(data);
}

static const struct ext_data_control_source_v1_listener ext_source_listener = {
	.send = ext_source_send,
	.cancelled = ext_source_cancelled,
};

static void clipboard_offer_destroy(struct clipboard_offer* offer)
{
	if (!offer)
		return;

	wl_list_remove(&offer->link);
	free(offer->mime);

	if (offer->is_ext) {
		if (offer->ext_offer)
			ext_data_control_offer_v1_destroy(offer->ext_offer);
	} else if (offer->offer) {
		wl_data_offer_destroy(offer->offer);
	}

	free(offer);
}

static void clipboard_offer_receive(struct clipboard_offer* offer,
		const char* mime, int32_t fd)
{
	if (offer->is_ext)
		ext_data_control_offer_v1_receive(offer->ext_offer, mime, fd);
	else
		wl_data_offer_receive(offer->offer, mime, fd);
}

static struct clipboard_offer* clipboard_find_offer(struct clipboard* self,
		bool is_ext, void* wl_offer)
{
	struct clipboard_offer* offer;
	wl_list_for_each(offer, &self->offers, link) {
		if (offer->is_ext != is_ext)
			continue;
		void* candidate = is_ext ? (void*)offer->ext_offer :
			(void*)offer->offer;
		if (candidate == wl_offer)
			return offer;
	}
	return NULL;
}

static void clipboard_offer_add_mime(struct clipboard_offer* offer,
		const char* mime_type)
{
	if (mime_is_utf8_text(mime_type)) {
		free(offer->mime);
		offer->mime = strdup(mime_type);
	} else if (!offer->mime && mime_is_text_plain(mime_type)) {
		offer->mime = strdup(mime_type);
	}
}

static void data_offer_offer(void* data, struct wl_data_offer* wl_offer,
		const char* mime_type)
{
	(void)wl_offer;
	clipboard_offer_add_mime(data, mime_type);
}

static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
};

static void ext_offer_offer(void* data,
		struct ext_data_control_offer_v1* ext_offer, const char* mime_type)
{
	(void)ext_offer;
	clipboard_offer_add_mime(data, mime_type);
}

static const struct ext_data_control_offer_v1_listener ext_offer_listener = {
	.offer = ext_offer_offer,
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

static void clipboard_use_selection(struct clipboard* self,
		struct clipboard_offer* offer)
{
	if (self->read.offer == offer)
		return;

	clipboard_read_cancel(self);

	if (!offer)
		return;

	if (self->owns_selection || !offer->mime) {
		clipboard_offer_destroy(offer);
		return;
	}

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		clipboard_offer_destroy(offer);
		return;
	}

	int flags = fcntl(fds[0], F_GETFL, 0);
	if (flags >= 0)
		fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	clipboard_offer_receive(offer, offer->mime, fds[1]);
	close(fds[1]);
	wl_display_flush(self->display);

	self->read.offer = offer;
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

static void clipboard_drop_stale_offers(struct clipboard* self)
{
	struct clipboard_offer* offer;
	struct clipboard_offer* tmp;
	wl_list_for_each_safe(offer, tmp, &self->offers, link) {
		if (offer != self->read.offer)
			clipboard_offer_destroy(offer);
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

	offer->is_ext = false;
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

	struct clipboard_offer* offer =
		clipboard_find_offer(self, false, wl_offer);
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

	struct clipboard_offer* offer = wl_offer ?
		clipboard_find_offer(self, false, wl_offer) : NULL;

	clipboard_use_selection(self, offer);
	clipboard_drop_stale_offers(self);

	if (wl_offer && !offer)
		wl_data_offer_destroy(wl_offer);
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection,
};

static void ext_device_data_offer(void* data,
		struct ext_data_control_device_v1* device,
		struct ext_data_control_offer_v1* ext_offer)
{
	(void)device;
	struct clipboard* self = data;
	struct clipboard_offer* offer = calloc(1, sizeof(*offer));
	if (!offer) {
		ext_data_control_offer_v1_destroy(ext_offer);
		return;
	}

	offer->is_ext = true;
	offer->ext_offer = ext_offer;
	wl_list_insert(&self->offers, &offer->link);
	ext_data_control_offer_v1_add_listener(ext_offer, &ext_offer_listener,
			offer);
}

static void ext_device_selection(void* data,
		struct ext_data_control_device_v1* device,
		struct ext_data_control_offer_v1* ext_offer)
{
	(void)device;
	struct clipboard* self = data;

	struct clipboard_offer* offer = ext_offer ?
		clipboard_find_offer(self, true, ext_offer) : NULL;

	clipboard_use_selection(self, offer);
	clipboard_drop_stale_offers(self);

	if (ext_offer && !offer)
		ext_data_control_offer_v1_destroy(ext_offer);
}

static void ext_device_finished(void* data,
		struct ext_data_control_device_v1* device)
{
	(void)data;
	(void)device;
}

static void ext_device_primary_selection(void* data,
		struct ext_data_control_device_v1* device,
		struct ext_data_control_offer_v1* ext_offer)
{
	struct clipboard* self = data;
	(void)device;

	if (!ext_offer)
		return;

	struct clipboard_offer* offer =
		clipboard_find_offer(self, true, ext_offer);
	if (offer)
		clipboard_offer_destroy(offer);
	else
		ext_data_control_offer_v1_destroy(ext_offer);
}

static struct clipboard_source* clipboard_source_new(struct clipboard* self,
		const char* text, size_t len, bool is_primary)
{
	struct clipboard_source* source = calloc(1, sizeof(*source));
	if (!source)
		return NULL;

	source->clipboard = self;
	source->is_primary = is_primary;

	if (blob_set(&source->latin1, text, len) < 0)
		goto failure;

	source->utf8.data = latin1_to_utf8(text, len, &source->utf8.len);
	if (!source->utf8.data)
		goto failure;

	if (self->ext_device) {
		source->is_ext = true;
		source->ext_source = ext_data_control_manager_v1_create_data_source(
				self->ext_manager);
		if (!source->ext_source)
			goto failure;

		ext_data_control_source_v1_add_listener(source->ext_source,
				&ext_source_listener, source);
		ext_data_control_source_v1_offer(source->ext_source,
				"text/plain;charset=utf-8");
		ext_data_control_source_v1_offer(source->ext_source,
				"text/plain");
		ext_data_control_source_v1_offer(source->ext_source,
				"UTF8_STRING");
		ext_data_control_source_v1_offer(source->ext_source, "STRING");
	} else {
		source->source = wl_data_device_manager_create_data_source(
				self->manager);
		if (!source->source)
			goto failure;

		wl_data_source_add_listener(source->source, &data_source_listener,
				source);
		wl_data_source_offer(source->source,
				"text/plain;charset=utf-8");
		wl_data_source_offer(source->source, "text/plain");
		wl_data_source_offer(source->source, "UTF8_STRING");
		wl_data_source_offer(source->source, "STRING");
	}

	return source;

failure:
	clipboard_source_destroy(source);
	return NULL;
}

static void clipboard_source_publish(struct clipboard* self,
		struct clipboard_source* source)
{
	if (self->ext_device) {
		if (source->is_primary)
			ext_data_control_device_v1_set_primary_selection(
					self->ext_device, source->ext_source);
		else
			ext_data_control_device_v1_set_selection(self->ext_device,
					source->ext_source);
	} else if (!source->is_primary) {
		wl_data_device_set_selection(self->data_device, source->source,
				self->seat->last_serial);
	}
}

static const struct ext_data_control_device_v1_listener ext_device_listener = {
	.data_offer = ext_device_data_offer,
	.selection = ext_device_selection,
	.finished = ext_device_finished,
	.primary_selection = ext_device_primary_selection,
};

struct clipboard* clipboard_new(struct wl_display* display,
		struct wl_data_device_manager* manager,
		struct ext_data_control_manager_v1* ext_manager,
		struct seat* seat,
		clipboard_local_selection_fn on_local_selection, void* userdata)
{
	if (!display || (!manager && !ext_manager) || !seat)
		return NULL;

	struct clipboard* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->display = display;
	self->manager = manager;
	self->ext_manager = ext_manager;
	self->seat = seat;
	self->on_local_selection = on_local_selection;
	self->userdata = userdata;
	self->read.fd = -1;
	wl_list_init(&self->offers);
	wl_list_init(&self->writes);

	if (ext_manager) {
		self->ext_device =
			ext_data_control_manager_v1_get_data_device(ext_manager,
					seat->wl_seat);
		if (!self->ext_device)
			goto failure;
		ext_data_control_device_v1_add_listener(self->ext_device,
				&ext_device_listener, self);
		return self;
	}

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

	if (self->ext_device)
		ext_data_control_device_v1_destroy(self->ext_device);

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

	if (self->primary_source) {
		clipboard_source_destroy(self->primary_source);
		self->primary_source = NULL;
		self->owns_primary = false;
	}

	if (!self->ext_device && !self->seat->last_serial) {
		fprintf(stderr, "Ignoring remote clipboard: no Wayland input serial yet\n");
		return;
	}

	struct clipboard_source* source =
		clipboard_source_new(self, text, len, false);
	if (!source)
		return;

	struct clipboard_source* primary = NULL;
	if (self->ext_device)
		primary = clipboard_source_new(self, text, len, true);

	blob_set(&self->last_remote_latin1, text, len);

	self->source = source;
	self->owns_selection = true;
	clipboard_source_publish(self, source);

	if (primary) {
		self->primary_source = primary;
		self->owns_primary = true;
		clipboard_source_publish(self, primary);
	}

	wl_display_flush(self->display);
}
