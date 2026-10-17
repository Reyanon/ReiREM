// ReiREM PSP port: YM2151 channel split between the sound side (Media Engine) and the main CPU.
// See burn_ym2151.cpp (sound side) and ym2151_main.c (main CPU side). REI_YM_SPLIT builds only.
#ifndef REI_YM_SPLIT_H
#define REI_YM_SPLIT_H

#define REI_YMF_RING 1024 // power of two

// shared counters: uncached plain accesses + sync on the PSP (rei_me.h), C11-style atomics elsewhere
// (PC harness threads)
#ifdef REI_ME_SPLIT
#define REI_YMF_LOAD(x)     REI_SH(x)
#define REI_YMF_STORE(x, v) do { REI_ME_SYNC(); REI_SH(x) = (v); REI_ME_SYNC(); } while (0)
#else
#define REI_YMF_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define REI_YMF_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#endif
enum { REI_YMF_WRITE, REI_YMF_END, REI_YMF_START, REI_YMF_RESET };
struct rei_ymf_ev {
	INT32 pos;            // sample position in the frame's YM2151 buffer
	UINT8 type, reg, val, pad;
};

#ifdef __cplusplus
extern "C" {
#endif
// shared (uncached only on the PSP): sound side -> main ring, main's read position and frame tag,
// main's partial channel sums (same layout as the sound side's buffer: 4 carried samples + frame)
extern struct rei_ymf_ev rei_ymf_ring[REI_YMF_RING];
extern UINT32 rei_ymf_w, rei_ymf_r, rei_ymf_done;
extern INT32 rei_ymf_pb[2][4096];

int rei_ym_main_init(int clock, int rate, UINT8 mask);
void rei_ym_main_exit(void);
int rei_ym_main_poll(void);
void rei_ym_main_resync(const void *state, int pos); // (after loading, while the sound side is stopped)
#ifdef __cplusplus
}
#endif

#endif
