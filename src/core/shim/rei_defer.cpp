// Deferred drawing (rei_defer.h, ADR-018)
//
// Rotates 3 frame slots (main writing, render drawing, spare). Each slot holds MAX_SEG segment states + 1 end-of-frame state.
// Main waits for a free slot, render waits for a filled one (each on its own core, so spin-waits, same as the M92 render thread).
#include "burnint.h"
#include "rei_defer.h"
#include "tiles_generic.h" // (pTransDraw: PSP index mode)
#include "rei_prof.h" // (profile: segment state copy time - "m6800" slot for other boards)

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define SLOTS   3
#define MAX_SEG 16 // draw segments per frame (one per raster interrupt line; overflow merges into the last segment)

extern "C" {
extern volatile int rei_stop;
extern void (*rei_wait_hook)(void);
}

// (PSP ME render) slots sit on separate 64-byte cache lines - sharing a line with other globals the main CPU writes often made ME
// misread slots, crawling or hanging (wilytowr, exposed when one added global shifted the layout, 2026-10-13 on hardware)
typedef struct __attribute__((aligned(64))) {
	int nseg;
	int start[MAX_SEG], finish[MAX_SEG];
	UINT8 *buf; // (MAX_SEG + 1) * seg_size, last one is the end-of-frame state
} slot_t;

static rei_defer_ops s_ops;
static int s_registered;
int rei_defer_on = 0;
int rei_render_noalloc = 0;
int rei_defer_vlog = 0; // (diag) log frames with no draw segments (PSP host0:/rei_vlog, harness --vlog)
void (*rei_defer_flush_hook)(void) = NULL;
void (*rei_render_done_hook)(void) = NULL;
static slot_t s_slot[SLOTS];
// frames fully written by main, frames fully drawn by render. On PSP render is the ME, so uncached only (rei_me.h)
#ifdef REI_ME_SPLIT
#include "rei_me.h"
static UINT32 s_prod REI_ME_SHARED, s_cons REI_ME_SHARED;
#define DEF_LOAD(x) ({ REI_ME_SYNC(); REI_SH(x); })
#define DEF_STORE(x, v) do { REI_ME_SYNC(); REI_SH(x) = (v); REI_ME_SYNC(); } while (0)
#define DEF_STOP() ({ REI_ME_SYNC(); REI_SH(rei_stop); })
#else
static UINT32 s_prod, s_cons;
#define DEF_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define DEF_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define DEF_STOP() (rei_stop)
#endif
// (PSP: s_prod/s_cons/rei_stop are always read uncached too - a cached read could return a stale line instead of the new uncached
//  write, freezing frame counts with main and ME waiting on each other. When the stale line got evicted depended on layout, so one global toggled it)
static int s_open;            // has main started writing the current slot (s_prod % SLOTS)
static int s_overflow_logged;

static void big_copy_default(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
void (*rei_big_copy)(void *dst, const void *src, size_t n) = big_copy_default;
extern "C" void rei_big_copy_reset(void) { rei_big_copy = big_copy_default; }

void rei_defer_register(const rei_defer_ops *ops)
{
	s_ops = *ops;
	s_ops.seg_size = (s_ops.seg_size + 63) & ~(size_t)63; // (64-byte boundary per state: large copies via DMA - rei_big_copy)
	s_registered = 1;
}

int rei_defer_registered(void) { return s_registered; }

void rei_defer_unregister(void)
{
	rei_defer_disable();
	s_registered = 0;
}

int rei_defer_enable(void)
{
	if (!s_registered)
		return -1;
	for (int i = 0; i < SLOTS; i++) {
		s_slot[i].buf = (UINT8 *)memalign(64, (MAX_SEG + 1) * s_ops.seg_size);
		if (!s_slot[i].buf) {
			rei_defer_disable();
			return -1;
		}
	}
	if (s_ops.prepare)
		s_ops.prepare();
	rei_defer_reset();
	rei_defer_on = 1;
	return 0;
}

void rei_defer_disable(void)
{
	rei_defer_on = 0;
	for (int i = 0; i < SLOTS; i++) {
		free(s_slot[i].buf);
		s_slot[i].buf = NULL;
	}
}

void rei_defer_reset(void)
{
	DEF_STORE(s_prod, 0);
	DEF_STORE(s_cons, 0);
	s_open = 0;
}

// (PSP ME render) is a frame ready to draw - lets render poll without waiting (ME can't call the wait hook)
int rei_defer_pending(void)
{
	return DEF_LOAD(s_prod) != DEF_LOAD(s_cons);
}
unsigned rei_defer_rendered(void) { return DEF_LOAD(s_cons); }

UINT16 *rei_render_index REI_RDR, *rei_render_clut REI_RDR; // (PSP index mode: index buffer/palette slot render draws into, chosen by the render side)
uint64_t (*rei_defer_clock)(void) = NULL; // (diag) time main spent waiting for render - frontend clock (rei_core_config.clock_us)
unsigned long long rei_defer_wait_us;

static void wait_hook(void)
{
	if (rei_wait_hook)
		rei_wait_hook();
}

// Main: open the current frame's slot (wait if render is still drawing it)
static slot_t *open_slot(void)
{
	if (!s_open) {
		if (DEF_LOAD(s_prod) - DEF_LOAD(s_cons) >= SLOTS) { // (render fell behind: add wait time to rei_defer_wait_us)
			const uint64_t t0 = rei_defer_clock ? rei_defer_clock() : 0;
			while (DEF_LOAD(s_prod) - DEF_LOAD(s_cons) >= SLOTS && !DEF_STOP())
				wait_hook();
			if (rei_defer_clock)
				rei_defer_wait_us += rei_defer_clock() - t0;
		}
		s_slot[DEF_LOAD(s_prod) % SLOTS].nseg = 0;
		s_open = 1;
	}
	return &s_slot[DEF_LOAD(s_prod) % SLOTS];
}

void rei_defer_segment(int start, int finish)
{
	if (finish <= start)
		return;
	slot_t *s = open_slot();
	int i = s->nseg;
	if (i == MAX_SEG) {
		// overflow: extend the last segment and draw with current state (earlier lines may get later state - rare)
		i = MAX_SEG - 1;
		start = s->start[i];
		if (!s_overflow_logged) {
			bprintf(PRINT_ERROR, _T("defer: more than %d segments in a frame\n"), MAX_SEG);
			s_overflow_logged = 1;
		}
	} else {
		s->nseg++;
	}
	s->start[i] = start;
	s->finish[i] = finish;
#ifdef REI_PROFILE
	REI_PROF_BEGIN(cap);
#endif
	s_ops.capture(s->buf + (size_t)i * s_ops.seg_size);
#ifdef REI_PROFILE
	REI_PROF_END(cap, REI_PROF_M6800);
#endif
}

// (diag, harness/PSP log) frame count, segment count, max segments - main only
unsigned long long rei_defer_stats[3];

void rei_defer_frame(void)
{
	slot_t *s = open_slot();
	if (rei_defer_vlog && s->nseg == 0) // (diag) becomes an empty frame
		bprintf(PRINT_IMPORTANT, _T("defer: no segment, frame %d\n"), nCurrentFrame);
	rei_defer_stats[0]++;
	rei_defer_stats[1] += (unsigned)s->nseg;
	if ((unsigned)s->nseg > rei_defer_stats[2])
		rei_defer_stats[2] = (unsigned)s->nseg;
#ifdef REI_PROFILE
	REI_PROF_BEGIN(capf);
#endif
	(s_ops.capture_end ? s_ops.capture_end : s_ops.capture)(s->buf + (size_t)MAX_SEG * s_ops.seg_size);
#ifdef REI_PROFILE
	REI_PROF_END(capf, REI_PROF_M6800);
#endif
	s_open = 0;
	if (rei_defer_flush_hook) // (PSP: so render (ME) reads it from memory)
		rei_defer_flush_hook();
	DEF_STORE(s_prod, DEF_LOAD(s_prod) + 1);
}

#ifdef REI_ME_SPLIT
// (diag) where render (ME) is now: 0 slot, 1 stage (1 start 2 segment 3 end state 4 frame end 5 done), 2 segment count, 3 segment index,
// 4/5 first segment lines, 6 segment count re-read per segment, 7 current segment lines (start | end<<16). Main prints in hang log (uncached)
extern "C" { UINT32 rei_defer_diag[8] REI_ME_SHARED; }
#define DEF_DIAG(i, v) (REI_SH(rei_defer_diag[i]) = (UINT32)(v))
#else
#define DEF_DIAG(i, v) ((void)0)
#endif
// (diag, wrong output) render stage to skip: 1 segment drawing, 2 frame end (sprites/transfer). PSP host0:/rei_rskip - measures ME workload
extern "C" { UINT32 rei_defer_skip[16] REI_ME_SHARED; }

int rei_defer_render(unsigned char *fb, int pitch)
{
	while (DEF_LOAD(s_prod) == DEF_LOAD(s_cons)) {
		if (DEF_STOP())
			return 0;
		wait_hook();
	}
	const UINT32 cons = DEF_LOAD(s_cons);
	slot_t *s = &s_slot[cons % SLOTS];
	DEF_DIAG(0, cons); DEF_DIAG(1, 1); DEF_DIAG(2, s->nseg); DEF_DIAG(4, s->start[0]); DEF_DIAG(5, s->finish[0]);
	pBurnDraw = fb;
	nBurnPitch = pitch;
	// (PSP index mode) picture 0 = point pTransDraw at the buffer GE reads and draw there directly; transfer only the palette (tiles_generic)
	UINT16 *const idx = rei_render_index;
	UINT16 *idx_old = NULL, *trans_old = NULL;
	if (idx) {
		extern UINT16 *rei_xfer_clut;
		extern UINT16 *rei_bitmap0_swap(UINT16 *p);
		idx_old = rei_bitmap0_swap(idx);
		trans_old = pTransDraw;
		pTransDraw = idx;
		rei_xfer_clut = rei_render_clut;
	}
	if (s_ops.frame_begin)
		s_ops.frame_begin();
	const UINT32 skip = REI_SH(rei_defer_skip[0]);
	for (int i = 0; i < s->nseg && !(skip & 1); i++) {
		DEF_DIAG(1, 2); DEF_DIAG(3, i); DEF_DIAG(6, s->nseg); DEF_DIAG(7, s->start[i] | (s->finish[i] << 16)); // (re-read segment count, this segment's lines)
		s_ops.apply(s->buf + (size_t)i * s_ops.seg_size);
		s_ops.draw(s->start[i], s->finish[i]);
	}
	DEF_DIAG(1, 3);
	s_ops.apply(s->buf + (size_t)MAX_SEG * s_ops.seg_size);
	DEF_DIAG(1, 4);
	if (s_ops.frame_end && !(skip & 2))
		s_ops.frame_end();
	if (idx) {
		extern UINT16 *rei_xfer_clut;
		extern UINT16 *rei_bitmap0_swap(UINT16 *p);
		rei_bitmap0_swap(idx_old);
		pTransDraw = trans_old;
		rei_xfer_clut = NULL;
	}
	DEF_DIAG(1, 5);
	if (rei_render_done_hook) // (PSP ME: signal after flushing the drawn frame to memory)
		rei_render_done_hook();
	DEF_STORE(s_cons, cons + 1);
	return 1;
}
