// Vita clock boost (docs/RULES.md 3: ARM 444MHz at app start, GPU/bus raised)
#include "platform/clock.h"

#include <psp2/power.h>

#include "platform/log.h"
#include "platform/timer.h"

// Target ARM clock. Acceptance criterion is 444 (docs/RULES.md 1). For testing, e.g. -DREI_ARM_MHZ=500
#ifndef REI_ARM_MHZ
#define REI_ARM_MHZ 444
#endif

void rei_clock_boost(void)
{
	int ra = scePowerSetArmClockFrequency(REI_ARM_MHZ);
	int rb = scePowerSetBusClockFrequency(222);
	int rg = scePowerSetGpuClockFrequency(222);
	int rx = scePowerSetGpuXbarClockFrequency(166);

	REI_LOG("clock: set(arm %d) ret arm=0x%08x bus=0x%08x gpu=0x%08x xbar=0x%08x", REI_ARM_MHZ, ra, rb, rg, rx);
	(void)ra; (void)rb; (void)rg; (void)rx;
	rei_clock_log(1);
}

// Estimate the real ARM clock (MHz). Plugins such as PSVshell hook scePowerGet* and
// may return wrong values, so time a loop with 1 cycle per iteration.
int rei_clock_measure_arm_mhz(void)
{
	const unsigned iters = 20000000u;
	uint64_t t0 = rei_time_us();
	unsigned n = iters;
	__asm__ volatile(
		"1:\n\t"
		"subs %0, %0, #1\n\t"
		"bne 1b\n\t"
		: "+r"(n) : : "cc");
	uint64_t dt = rei_time_us() - t0;
	return dt ? (int)(iters / dt) : 0;
}

// measure != 0 also measures the real clock. Holds the CPU for ~45ms, so use only at startup
// (periodic measurement stutters frames each time - PERF.md P6)
void rei_clock_log(int measure)
{
	REI_LOG("clock: arm=%d bus=%d gpu=%d xbar=%d (measured arm~%dMHz)",
		scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
		scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
		measure ? rei_clock_measure_arm_mhz() : 0);
	(void)measure;
}

#ifdef REI_CLOCK_SWEEP
#include <psp2/kernel/threadmgr.h>

// Diagnostic: step through ARM clocks, logging return value, read-back and measured value
void rei_clock_sweep(void)
{
	static const int steps[] = { 111, 222, 333, 444, 500 };
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		int r = scePowerSetArmClockFrequency(steps[i]);
		sceKernelDelayThread(100 * 1000);
		REI_LOG("sweep: set %d ret=0x%08x get=%d measured~%dMHz",
			steps[i], r, scePowerGetArmClockFrequency(), rei_clock_measure_arm_mhz());
	}
}
#endif
