// ReiREM PSP port: code that runs for every scanline of the main side (line loop, end-of-line work, raster
// interrupt entry and its fast path) in one block of its own sections, so screens with an interrupt on every
// line do not refill the instruction cache for it each line (psp/me.ld, CMake REI_PSP_LINE_ORDER, PSP perf PP19).
// Empty unless REI_LINE_ORDER (PSP build option): no effect on other builds.
#ifndef REI_LINE_H
#define REI_LINE_H

#if defined(__psp__) && defined(REI_LINE_ORDER)
#define REI_LINE_FN(n) __attribute__((section(".nbbline_text." #n)))
#define REI_LINE_FAST_FN(n) __attribute__((noinline, section(".nbbline_text." #n)))
#else
#define REI_LINE_FN(n)
#define REI_LINE_FAST_FN(n) REI_FAST_FN
#endif

#endif
