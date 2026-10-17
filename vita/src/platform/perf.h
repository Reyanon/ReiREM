#ifndef REI_PLATFORM_PERF_H
#define REI_PLATFORM_PERF_H

#include <stdint.h>

#include "platform/timer.h"

// per-thread ms/frame measurement (docs/RULES.md section 7: average, max, 99th percentile)
//
// Each slot is written by one thread only (single producer). The HUD side computes stats
// read-only, so a sample may appear torn, which is acceptable for display.

typedef enum {
	REI_PERF_EMU,    // CPU0: V33 + video registers + input (excluding vsync wait)
	REI_PERF_SOUND,  // CPU1: V35 + YM2151 + GA20
	REI_PERF_RENDER, // CPU2: tile/sprite compositing
	REI_PERF_FRAME,  // frame interval (incl. vsync). 16.67ms at 60fps
	REI_PERF_COUNT
} rei_perf_slot;

#define REI_PERF_WINDOW 120 // sample count (2 s)

typedef struct {
	float avg_ms;
	float max_ms;
	float p99_ms;
} rei_perf_stats;

void rei_perf_record(rei_perf_slot slot, uint32_t us);
void rei_perf_stats_get(rei_perf_slot slot, rei_perf_stats *out);
const char *rei_perf_slot_name(rei_perf_slot slot);

// section timing helpers
//   uint64_t t0 = rei_time_us(); ... ; rei_perf_end(REI_PERF_EMU, t0);
static inline void rei_perf_end(rei_perf_slot slot, uint64_t t0)
{
	rei_perf_record(slot, (uint32_t)(rei_time_us() - t0));
}

#endif
