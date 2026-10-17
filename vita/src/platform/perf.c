#include "platform/perf.h"

#include <string.h>

typedef struct {
	volatile uint32_t samples[REI_PERF_WINDOW];
	volatile uint32_t count; // cumulative record count (monotonic)
} perf_ring;

static perf_ring s_rings[REI_PERF_COUNT];

static const char *const s_names[REI_PERF_COUNT] = {
	"emu", "sound", "render", "frame",
};

void rei_perf_record(rei_perf_slot slot, uint32_t us)
{
	perf_ring *r = &s_rings[slot];
	uint32_t n = r->count;
	r->samples[n % REI_PERF_WINDOW] = us;
	r->count = n + 1;
}

static void sort_u32(uint32_t *a, int n)
{
	for (int i = 1; i < n; i++) {
		uint32_t v = a[i];
		int j = i - 1;
		while (j >= 0 && a[j] > v) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = v;
	}
}

void rei_perf_stats_get(rei_perf_slot slot, rei_perf_stats *out)
{
	const perf_ring *r = &s_rings[slot];
	uint32_t total = r->count;
	int n = total < REI_PERF_WINDOW ? (int)total : REI_PERF_WINDOW;

	memset(out, 0, sizeof(*out));
	if (n == 0)
		return;

	uint32_t buf[REI_PERF_WINDOW];
	uint64_t sum = 0;
	for (int i = 0; i < n; i++) {
		buf[i] = r->samples[i];
		sum += buf[i];
	}
	sort_u32(buf, n);

	int p99 = (n * 99 + 99) / 100 - 1; // rounded-up rank
	out->avg_ms = (float)sum / (float)n / 1000.0f;
	out->max_ms = buf[n - 1] / 1000.0f;
	out->p99_ms = buf[p99] / 1000.0f;
}

const char *rei_perf_slot_name(rei_perf_slot slot)
{
	return s_names[slot];
}
