#ifndef REI_PLATFORM_CLOCK_H
#define REI_PLATFORM_CLOCK_H

// Raise ARM to 444MHz, bus 222MHz, GPU 222MHz, GPU Xbar 166MHz.
void rei_clock_boost(void);

// Log current clocks over UDP. If measure != 0, also measure (about 45ms, startup only)
void rei_clock_log(int measure);

// Estimate the real ARM clock (MHz) with a fixed-cycle loop (about 45-180ms)
int rei_clock_measure_arm_mhz(void);

#ifdef REI_CLOCK_SWEEP
// Diagnostic clock step test (-DREI_CLOCK_SWEEP)
void rei_clock_sweep(void);
#endif

#endif
