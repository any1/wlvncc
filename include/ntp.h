/*
 * Copyright (c) 2023 Andri Yngvason
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

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define NTP_SAMPLE_PERIOD 1000000 // µs
#define NTP_MIN_SAMPLE_COUNT 3
#define NTP_SAMPLE_SIZE 16

struct aml_ticker;
struct ntp_client;

typedef void (*ntp_client_ping_fn)(struct ntp_client*, uint32_t t0, uint32_t t1,
		uint32_t t2, uint32_t t3);

struct ntp_sample {
	int32_t theta;
	uint32_t delta;
};

struct ntp_client {
	struct ntp_sample samples[NTP_SAMPLE_SIZE];
	int sample_index;
	int sample_count;

	struct aml_ticker* ping_ticker;

	ntp_client_ping_fn send_ping;
	void* userdata;
};

int ntp_client_init(struct ntp_client*, ntp_client_ping_fn send_ping,
                void* userdata);
void ntp_client_deinit(struct ntp_client*);

void ntp_client_process_pong(struct ntp_client*, uint32_t t0, uint32_t t1,
                uint32_t t2, uint32_t t3);

bool ntp_client_get_best_sample(const struct ntp_client* self,
		struct ntp_sample* sample);

bool ntp_client_translate_server_time(const struct ntp_client* self,
		uint32_t* dst, const uint32_t t);
