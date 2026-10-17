// ReiREM PSP port: sound side on the Media Engine (psp/docs/DECISIONS.md PSP-ADR-003).
//
// The ME and the main CPU do not share caches. With REI_ME_SPLIT (PSP builds):
// - REI_ME_DATA: data written only by the sound side (V35, YM2151, GA20, timers). The PSP link script
//   (psp/me.ld) collects it, with the whole .data/.bss of the sound-only objects, into one 64-byte
//   aligned block so no cache line holds both ME-written and main-written bytes.
// - REI_ME_SHARED + REI_SH(x): variables both sides use (event rings, line counters, stop flag).
//   They live in their own aligned block and are only ever accessed through the uncached alias
//   (address | 0x40000000), so neither cache holds them.
// Without REI_ME_SPLIT everything is plain memory (Vita, PC: one coherent cache).
#ifndef REI_ME_H
#define REI_ME_H

#include <stdint.h>

#ifdef REI_ME_SPLIT
#define REI_ME_DATA   __attribute__((section(".rei_me_data"), aligned(64)))
#define REI_ME_SHARED __attribute__((section(".rei_me_shared"), aligned(64)))
#ifdef REI_ME_PCTEST
// PC check of the split code paths (tools/harness -DREI_ME_SPLIT -DREI_ME_PCTEST): same macros, plain addresses
#define REI_UNCACHED(p) (p)
#define REI_ME_SYNC() __asm__ volatile("mfence" ::: "memory")
#else
#define REI_UNCACHED(p) ((__typeof__(p))((uintptr_t)(p) | 0x40000000u))
#define REI_ME_SYNC() __asm__ volatile("sync" ::: "memory")
#endif
#define REI_SH(x)     (*(volatile __typeof__(x) *)REI_UNCACHED(&(x)))
#else
#define REI_ME_DATA
#define REI_ME_SHARED
#define REI_UNCACHED(p) (p)
#define REI_SH(x)     (x)
#define REI_ME_SYNC() ((void)0)
#endif

#endif
