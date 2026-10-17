#ifndef REI_PLATFORM_TIMER_H
#define REI_PLATFORM_TIMER_H

#include <stdint.h>
#include <psp2/kernel/processmgr.h>

// Process-relative monotonic time (microseconds)
static inline uint64_t rei_time_us(void)
{
	return (uint64_t)sceKernelGetProcessTimeWide();
}

#endif
