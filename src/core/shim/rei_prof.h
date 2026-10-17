// Core section profiler (active only with -DREI_PROFILE; otherwise macros are empty, no hash impact)
//
// The frontend supplies the time source via rei_core_config.clock_us.
// CPU run sections executed 2048 times/frame are timed once in 64 and scaled by 64 (limits overhead).
#ifndef REI_PROF_H
#define REI_PROF_H

#include <stdint.h>

#include "rei_me.h"

enum {
	REI_PROF_V33,      // main CPU run (VezRun, sample x64)
	REI_PROF_V35,      // sound CPU + BurnTimer (CPU_RUN_TIMER, sample x64)
	REI_PROF_LAYERS,   // tile layers (DrawLayers)
	REI_PROF_SPRITES,  // sprites (draw_sprites)
	REI_PROF_XFER,     // palette conversion transfer (BurnTransferCopy)
	REI_PROF_SOUND,    // end-of-frame YM2151 + GA20 render
	REI_PROF_FRAME,    // whole frame
	REI_PROF_YM,       // YM2151 synthesis (on register writes + end of frame, overlaps other sections)
	REI_PROF_SWAIT,    // time the sound side waited for main progress (threaded)
	REI_PROF_MLINE,    // total main line processing (excl. sound wait, incl. V33)
	REI_PROF_SLINE,    // total sound line processing (excl. main wait, incl. V35/timer)
	REI_PROF_GA20,     // end-of-frame GA20
	REI_PROF_YMEND,    // end-of-frame YM2151 (pending writes + remaining synthesis + resample)
	REI_PROF_HOOK,     // V33 run slices (nec_execute calls; a count: shown value = count/frame)
	REI_PROF_INS,      // instructions executed by the V33 interpreter (a count, not time: shown value = thousands/frame)
	REI_PROF_FAST,     // V33 hooks/fast paths (rei_hook, included in V33 section)
	REI_PROF_Z80,      // other boards: ZetRun (sample x16)
	REI_PROF_M6800,    // other boards: M6800/M6803 run (sample x16)
	REI_PROF_NEC,      // other boards: VezRun (V30/V33/V35, sample x16)
	REI_PROF_I8039,    // other boards: I8039 run (sample x16)
	REI_PROF_COUNT
};

#define REI_PROF_SAMPLE_SHIFT 6 // 1/64 sampling

// For CPU run functions (other boards, 1/16 sampling): static counter inside the function, mixed before selecting — plain n & 15
// aliases with the CPUs' alternation period and measures only one CPU. (one clock read ~1.3µs: timing every call inflates numbers)
// (REI_PROF_FRAME_ONLY: CPU sections are not timed — so clock reads (syscalls) add no cache misses when measuring with HW counters)
#if defined(REI_PROFILE) && defined(REI_PROF_CPU_ALL)
// (diag) time every call without sampling — exact share for rarely-called CPUs (hundreds/frame). Adds the clock read cost accordingly
#define REI_PROF_CPU_BEGIN(name) uint64_t rei_prof_t_##name = rei_prof_clock()
#define REI_PROF_CPU_END(name, id) \
	do { if (rei_prof_t_##name) REI_PROF_ACC[id] += rei_prof_clock() - rei_prof_t_##name; } while (0)
#elif defined(REI_PROFILE) && !defined(REI_PROF_FRAME_ONLY)
#define REI_PROF_CPU_BEGIN(name) \
	static unsigned rei_prof_n_##name; \
	uint64_t rei_prof_t_##name = (((++rei_prof_n_##name * 0x9e3779b1u) >> 28) == 0) ? rei_prof_clock() : 0
#define REI_PROF_CPU_END(name, id) \
	do { if (rei_prof_t_##name) REI_PROF_ACC[id] += (rei_prof_clock() - rei_prof_t_##name) << 4; } while (0)
#else
#define REI_PROF_CPU_BEGIN(name) ((void)0)
#define REI_PROF_CPU_END(name, id) ((void)0)
#endif

#ifdef REI_PROFILE

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t (*rei_prof_clock)(void);
extern uint64_t rei_prof_acc[REI_PROF_COUNT];
#ifdef __cplusplus
}
#endif

// Accumulators: the ME split build (PSP) shares them between main and ME, so uncached (only one side writes each section)
#define REI_PROF_ACC (REI_UNCACHED(&rei_prof_acc[0]))

#define REI_PROF_BEGIN(name)      uint64_t rei_prof_t_##name = rei_prof_clock()
// a clock of 0 means "not measured": no write to the accumulators
#define REI_PROF_END(name, id)    do { if (rei_prof_t_##name) REI_PROF_ACC[id] += rei_prof_clock() - rei_prof_t_##name; } while (0)

// time only when i hits the sampling period
#define REI_PROF_SAMPLE_BEGIN(name, i) \
	uint64_t rei_prof_t_##name = (((i) & ((1 << REI_PROF_SAMPLE_SHIFT) - 1)) == 0) ? rei_prof_clock() : 0
#define REI_PROF_SAMPLE_END(name, id) \
	do { if (rei_prof_t_##name) REI_PROF_ACC[id] += (rei_prof_clock() - rei_prof_t_##name) << REI_PROF_SAMPLE_SHIFT; } while (0)

#else

#define REI_PROF_BEGIN(name)            ((void)0)
#define REI_PROF_END(name, id)          ((void)0)
#define REI_PROF_SAMPLE_BEGIN(name, i)  ((void)0)
#define REI_PROF_SAMPLE_END(name, id)   ((void)0)

#endif

// Per-CPU PC histogram (cycle weighted). Only with -DREI_PCHIST. cpu 0 = V33, 1 = V35
#ifdef REI_PCHIST
#ifdef __cplusplus
extern "C" {
#endif
void rei_pchist_add(int cpu, unsigned pc, int cycles);
#ifdef __cplusplus
}
#endif
#define REI_PCHIST_PC(var)            unsigned var = PC(nec_state)
#define REI_PCHIST_ADD(cpu, var, cyc) rei_pchist_add((cpu), (var), (cyc))
#else
#define REI_PCHIST_PC(var)            ((void)0)
#define REI_PCHIST_ADD(cpu, var, cyc) ((void)0)
#endif

#endif
