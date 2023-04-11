#include "performance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct perf_sample_buffer* perf_sample_buffer_create(int length)
{
	struct perf_sample_buffer* self = malloc(sizeof(*self)
			+ length * sizeof(self->samples[0]));
	self->count = 0;
	self->index = 0;
	self->length = length;
	return self;
}

void perf_sample_buffer_add(struct perf_sample_buffer* self, double sample)
{
	self->samples[self->index] = sample;
	self->index = (self->index + 1) % self->length;
	if (self->count < self->length)
		self->count += 1;
}

void perf_sample_buffer_get_stats(const struct perf_sample_buffer* self,
		struct perf_sample_stats* stats)
{
	memset(stats, 0, sizeof(*stats));

	double sum = 0;
	double minimum = INFINITY;
	double maximum = 0;

	for (int i = 0; i < self->count; ++i) {
		double sample = self->samples[i];

		sum += sample;

		if (sample < minimum)
			minimum = sample;

		if (sample > maximum)
			maximum = sample;
	}

	stats->min = minimum;
	stats->max = maximum;
	stats->average = sum / (double)self->count;
}

void perf_init(struct perf* self)
{
	memset(self, 0, sizeof(*self));

	self->frame_latency =
		perf_sample_buffer_create(PERF_FRAME_LATENCY_SAMPLE_SIZE);
}

void perf_deinit(struct perf* self)
{
	free(self->frame_latency);
}

void perf_dump_latency_report(const struct perf* self)
{
	struct perf_sample_stats stats;
	perf_sample_buffer_get_stats(self->frame_latency, &stats);

	printf("Latency report: frame-latency (min, avg, max): %.1f, %.1f, %.1f\n",
			stats.min / 1e3, stats.average / 1e3, stats.max / 1e3);
}
