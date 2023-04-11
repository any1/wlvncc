#pragma once

#define PERF_FRAME_LATENCY_SAMPLE_SIZE 60

struct perf_sample_stats {
	double min, max, average;
};

struct perf_sample_buffer {
	int length;
	int count;
	int index;
	double samples[];
};

struct perf {
	struct perf_sample_buffer* frame_latency;
};

struct perf_sample_buffer* perf_sample_buffer_create(int length);

void perf_sample_buffer_add(struct perf_sample_buffer* self, double sample);

void perf_sample_buffer_get_stats(const struct perf_sample_buffer* self,
		struct perf_sample_stats* stats);

void perf_init(struct perf*);
void perf_deinit(struct perf*);

void perf_dump_latency_report(const struct perf*);
