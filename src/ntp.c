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

#include "ntp.h"
#include "time-util.h"

#include <assert.h>
#include <aml.h>
#include <stdio.h>

static inline int32_t clamp_to_zero(int32_t v)
{
	return v >= 0 ? v : 0;
}

void ntp_client__tick(void* handler)
{
	struct ntp_client* self = aml_get_userdata(handler);
	assert(self);
	assert(self->send_ping);

	uint32_t t0 = gettime_us();
	self->send_ping(self, t0, 0, 0, 0);
}

int ntp_client_init(struct ntp_client* self, ntp_client_ping_fn send_ping,
		void* userdata)
{
	self->send_ping = send_ping;
	self->userdata = userdata;

	struct aml_ticker* ticker = aml_ticker_new(NTP_SAMPLE_PERIOD,
			ntp_client__tick, self, NULL);
	if (!ticker)
		return -1;

	int rc = aml_start(aml_get_default(), ticker);
	if (rc >= 0)
		self->ping_ticker = ticker;
	else
		aml_unref(ticker);

	return rc;
}

void ntp_client_deinit(struct ntp_client* self)
{
	if (self->ping_ticker) {
		aml_stop(aml_get_default(), self->ping_ticker);
		aml_unref(self->ping_ticker);
	}
}

void ntp_client_process_pong(struct ntp_client* self, uint32_t t0, uint32_t t1,
                uint32_t t2, uint32_t t3)
{
	t3 = gettime_us();

	int32_t theta = ((int32_t)(t1 - t0) + (int32_t)(t2 - t3)) / 2;
	uint32_t delta = clamp_to_zero((int32_t)(t3 - t0) - (int32_t)(t2 - t1));

	struct ntp_sample sample = {
		.theta = theta,
		.delta = delta,
	};

	self->samples[self->sample_index] = sample;
	self->sample_index = (self->sample_index + 1) % NTP_SAMPLE_SIZE;

	if (self->sample_count < NTP_SAMPLE_SIZE)
		self->sample_count++;

//	printf("%.3f %.3f\n", delta / 1e3, theta / 1e3);
}

bool ntp_client_get_best_sample(const struct ntp_client* self,
		struct ntp_sample* out)
{
	if (self->sample_count < NTP_MIN_SAMPLE_COUNT)
		return false;

	struct ntp_sample result = {
		.theta = 0,
		.delta = UINT32_MAX,
	};

	for (int i = 0; i < self->sample_count; ++i) {
		const struct ntp_sample *sample = &self->samples[i];
		if (sample->delta < result.delta) {
			result = *sample;
		}
	}

	*out = result;
	return true;
}

bool ntp_client_translate_server_time(const struct ntp_client* self,
		uint32_t* dst, const uint32_t t)
{
	struct ntp_sample sample;
	if (!ntp_client_get_best_sample(self, &sample))
		return false;

	*dst = (int32_t)t - sample.theta;
	return true;
}

uint32_t ntp_client_get_jitter(const struct ntp_client* self)
{
	uint32_t max_delta = 0;
	for (int i = 0; i < self->sample_count; ++i) {
		const struct ntp_sample *sample = &self->samples[i];
		if (sample->delta > max_delta) {
			max_delta = sample->delta;
		}
	}
	return max_delta;
}
