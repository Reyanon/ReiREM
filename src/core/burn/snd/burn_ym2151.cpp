// FBAlpha YM-2151 sound core interface
#include "burnint.h"
#include "burn_ym2151.h"
#include "rei_gain.h"
#include "rei_dmul.h" // (ReiREM: non-k/5 volume double multiply done in integer, same result)
#include "rei_prof.h" // ReiREM port: section profiler (no-op unless REI_PROFILE)
#ifdef REI_YM_ME
#include <malloc.h>
#include "rei_me.h"
#include "rei_defer.h" // (REI_RDR: ME-only section)
#endif
#ifdef REI_YM_SPLIT
#include "rei_me.h"
#include "rei_ym_split.h"
extern "C" volatile int rei_stop; // burn_shim.cpp: set when the threads are being stopped
#endif

// Irq Callback timing notes.. (when not using BurnTimer!)
// Due to the way the internal timing of the ym2151 works, BurnYM2151Render()
// should not be called more than ~65 times per frame.  See DrvFrame() in
// drv/konami/d_surpratk.cpp for a simple and effective work-around.
// Note: using BurnTimer mode avoids this mess completely.

static INT32 bYM2151_MultiChip = 0;

static UINT32 nBurnCurrentYM2151Register[2];

static INT32 nBurnYM2151SoundRate;

static INT16* pBuffer;
static INT16* pYM2151Buffer[4];

static INT32 bYM2151AddSignal;

static INT32 nYM2151Position;
static UINT32 nSampleSize;
static INT32 nFractionalPosition;

static double YM2151Volumes[2][2];
static INT32 YM2151VolK[2][2]; // ReiREM port: YM2151Volumes == k/5 exactly (rei_gain.h), else -1
static INT32 YM2151RouteDirs[2][2];

static INT32 YM2151BurnTimer = 0;

static INT32 bBurnYM2151IsBuffered = 0;
static INT32 (*BurnYM2151StreamCallback)(INT32 nSoundRate) = NULL;

#ifdef REI_YM_LAZY
// ReiREM PSP port: synthesis-register writes are logged with their sample position instead of
// rendering up to that position right away; the log is replayed (render to the position, then write) when
// the sound CPU has to wait for the main CPU anyway (rei_ym_idle_step from the wait hook), before any
// timer/IRQ/port register write, and at the end of the frame. YM2151 synthesis does not feed back into the
// sound CPU here (status = BurnTimer flags, busy flag not emulated) and rendering is per sample, so the
// output is the same. CSM (timer A key-on) would make it depend on render boundaries: then logging stops.
#define REI_YM_LOG 512
struct rei_ym_ev { INT32 pos; UINT8 chip, reg, val; };
static struct rei_ym_ev rei_ym_log[REI_YM_LOG];
static INT32 rei_ym_r, rei_ym_w;
static INT32 rei_ym_csm;
static void YM2151Render(INT32 nSegmentLength);

#ifdef REI_YM_SPLIT
// ReiREM PSP port: channel split. The sound side (this file, on the Media Engine) synthesizes the
// channels not in the main CPU's mask into rei_ym_pa (unshifted sums), forwards every register write it
// applies (except timer registers) with its sample position, and at the end of the frame asks the main
// CPU to finish its share (rei_ymf_pb, ym2151_main.c) up to the frame's last sample, adds the two
// (YM2151Combine = the end of YM2151UpdateOne's sample loop) and resamples as before. Same output.
struct rei_ymf_ev rei_ymf_ring[REI_YMF_RING] REI_ME_SHARED;
UINT32 rei_ymf_w REI_ME_SHARED, rei_ymf_r REI_ME_SHARED, rei_ymf_done REI_ME_SHARED;
INT32 rei_ymf_pb[2][4096] REI_ME_SHARED;
extern "C" INT32 rei_ym_split_on = 0; // read by the main CPU's line loop (set before the sound side starts)
static INT32 *rei_ym_pa;
static UINT32 rei_ymf_wl, rei_ymf_tag;
static INT32 rei_ym_frame_start;
static INT32 rei_ym_clock;

static void rei_ymf_put(UINT8 type, INT32 pos, UINT8 reg, UINT8 val)
{
	while (rei_ymf_wl - REI_YMF_LOAD(rei_ymf_r) >= REI_YMF_RING && !REI_SH(rei_stop)) { } // the main CPU drains it while it runs or waits
	volatile struct rei_ymf_ev *e = (volatile struct rei_ymf_ev *)REI_UNCACHED(&rei_ymf_ring[rei_ymf_wl & (REI_YMF_RING - 1)]);
	e->pos = pos;
	e->type = type;
	e->reg = reg;
	e->val = val;
	rei_ymf_wl++;
	REI_YMF_STORE(rei_ymf_w, rei_ymf_wl);
}
#endif

static void rei_ym_apply_one()
{
	const struct rei_ym_ev *e = &rei_ym_log[rei_ym_r++];
	if (e->pos >= 0) YM2151Render(e->pos);
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) rei_ymf_put(REI_YMF_WRITE, nYM2151Position, e->reg, e->val);
#endif
	YM2151WriteReg(e->chip, e->reg, e->val);
}

static void rei_ym_flush()
{
	while (rei_ym_r < rei_ym_w) rei_ym_apply_one();
	rei_ym_r = rei_ym_w = 0;
}

// one logged write (called while the sound side waits). Returns 1 if there was one
extern "C" int rei_ym_idle_step()
{
	if (rei_ym_r >= rei_ym_w) return 0;
	rei_ym_apply_one();
	if (rei_ym_r >= rei_ym_w) rei_ym_r = rei_ym_w = 0;
	return 1;
}

static inline INT32 rei_ym_immediate(INT32 reg)
{
	return reg == 0x10 || reg == 0x11 || reg == 0x12 || reg == 0x14 || reg == 0x1b;
}
#endif

#if !defined(__psp__) && !defined(REI_YM_LAZY)
// ReiREM (Vita/harness, other boards): YM2151 synthesis on a worker thread (CPU1). Main queues synth register writes with sample positions
// into a ring (timer registers 0x10-0x14 are written by main directly - no overlap with synth state); the worker, in the same order,
// does "synthesize up to that position → write". At frame end main waits for the worker to finish synthesis, then resamples.
// Same synthesis order/positions, so identical results. CSM (timer A key-on) interferes with synthesis; once enabled the worker is no longer used
#define REI_YM_THREAD 1
static void YM2151Render(INT32 nSegmentLength);
extern "C" volatile int rei_stop;
extern "C" void (*rei_wait_hook)(void);
struct rei_ymt_ev { INT32 pos; UINT8 kind, chip, reg, val; };
#define REI_YMT_RING 4096
static struct rei_ymt_ev rei_ymt_ring[REI_YMT_RING];
static UINT32 rei_ymt_w, rei_ymt_r, rei_ymt_done, rei_ymt_tag; // w: main, r/done: worker
static int rei_ymt_on;
static inline void rei_ymt_wait(void) { if (rei_wait_hook) rei_wait_hook(); }
static void rei_ymt_put(UINT8 kind, INT32 pos, UINT8 chip, UINT8 reg, UINT8 val)
{
	while (rei_ymt_w - __atomic_load_n(&rei_ymt_r, __ATOMIC_ACQUIRE) >= REI_YMT_RING && !rei_stop)
		rei_ymt_wait();
	struct rei_ymt_ev *e = &rei_ymt_ring[rei_ymt_w & (REI_YMT_RING - 1)];
	e->pos = pos; e->kind = kind; e->chip = chip; e->reg = reg; e->val = val;
	__atomic_store_n(&rei_ymt_w, rei_ymt_w + 1, __ATOMIC_RELEASE);
}
static void rei_ymt_drain(void) // until the worker drains the ring (before main touches state)
{
	while (__atomic_load_n(&rei_ymt_r, __ATOMIC_ACQUIRE) != rei_ymt_w && !rei_stop)
		rei_ymt_wait();
}
// Worker body (called by the frontend on a CPU1 thread, until rei_stop)
extern "C" void BurnYM2151WorkerRun(void)
{
	while (!rei_stop) {
		const UINT32 r = rei_ymt_r;
		if (r == __atomic_load_n(&rei_ymt_w, __ATOMIC_ACQUIRE)) {
			rei_ymt_wait();
			continue;
		}
		const struct rei_ymt_ev *e = &rei_ymt_ring[r & (REI_YMT_RING - 1)];
		YM2151Render(e->pos);
		if (e->kind == 0)
			YM2151WriteReg(e->chip, e->reg, e->val);
		else if (e->kind == 1)
			__atomic_store_n(&rei_ymt_done, (UINT32)e->reg << 8 | e->val, __ATOMIC_RELEASE);
		// (kind 2: synthesis only - timer write slot)
		__atomic_store_n(&rei_ymt_r, r + 1, __ATOMIC_RELEASE);
	}
}
// Can the worker thread be used (one buffered-mode chip); enable (before starting the thread)
extern "C" int BurnYM2151WorkerSupported(void);
extern "C" void BurnYM2151WorkerEnable(int on)
{
	if (!on && rei_ymt_on) rei_ymt_drain();
	rei_ymt_on = on && BurnYM2151WorkerSupported();
}
#endif

#ifdef REI_YM_ME
// ReiREM PSP (other boards): YM2151 synthesis on the Media Engine (ME), sound output one frame late.
// Main's chip takes every register write and keeps handling timers/status/ports, but does not synthesize. The ME, with a chip copy
// (ym2151_main.c), repeats "synthesize → write" at the same positions/lengths main used to (timer registers only
// split the synthesis), and at frame end yields the resampled YM share as before. Main leaves the YM share at 0 at frame end, takes the other
// chips (DAC, GA20 - additive), then at the next frame end adds the ME's YM share on top: clip(YM + rest), same as the original
// (as long as the other chips' sum fits in 16 bits). Overwrite mode (bAdd 0), one timer-mode chip only. Main takes over when CSM is enabled
extern "C" volatile int rei_stop;
extern "C" int rei_ym_copy_init(int clock, int rate);
extern "C" void nbbm_YM2151Shutdown(void);
extern "C" void nbbm_YM2151ResetChip(int num);
extern "C" void nbbm_YM2151UpdateOne(int num, INT16 **buffers, int length);
extern "C" void nbbm_YM2151WriteReg(int n, int r, int v);
extern "C" void nbbm_YM2151StateGet(int n, void *dst);
extern "C" void nbbm_YM2151StateSet(int n, const void *src, int full);
extern "C" void nbbm_YM2151SetChannelMask(UINT8 mask);
extern "C" void nbbm_YM2151CopyChannels(int n, const void *src, UINT8 mask);
extern "C" void nbbm_YM2151SetPartialOut(INT32 *l, INT32 *r);
extern "C" void nbbm_YM2151Combine(INT16 *dl, INT16 *dr, const INT32 *al, const INT32 *ar, const volatile INT32 *bl, const volatile INT32 *br, int n);
// Channel-split synthesis (when the ME overruns, BurnYM2151MeSplit): main synthesizes rei_ymm_mmask channels, writes partial sums (chip rate, integer) to a frame slot
// and tags RENDER with slot/position. The ME synthesizes the rest and finishes by adding main's partial sums (YM2151Combine = same formula as the end of UpdateOne
// → identical results). Each copy skips envelope/phase of the other's channels, so when one side takes over everything or exports state,
// it fetches the other side's channels from there (YM2151CopyChannels). Main waits for the ME's previous frame at every frame end, so 4 slots never overlap
#define REI_YMM_MP_SLOTS 4
#define REI_YMM_MP_LEN 2048
static INT32 rei_ymm_mpart[REI_YMM_MP_SLOTS][2][REI_YMM_MP_LEN] REI_ME_SHARED;
static UINT8 rei_ymm_mmask;         // channels main synthesizes (0 = all on the ME)
static UINT32 rei_ymm_mp_slot, rei_ymm_mp_pos;
extern "C" int nbbm_YM2151StateSize(void);
#ifdef REI_ME_SPLIT
#define REI_YMM_LOAD(x)     REI_SH(x)
#define REI_YMM_STORE(x, v) do { REI_ME_SYNC(); REI_SH(x) = (v); REI_ME_SYNC(); } while (0)
#else
#define REI_YMM_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define REI_YMM_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#endif
// SEG: resample one output segment (buffered mode: one per frame, unbuffered (M90): per segment). FRAME: frame end marker (finish count +1)
// CHIN: copy val channels of the main chip state (rei_ymm_xfer) to the ME copy (when shrinking main's share in channel split)
enum { REI_YMM_RENDER, REI_YMM_WRITE, REI_YMM_SEG, REI_YMM_RESET, REI_YMM_OUT, REI_YMM_IN, REI_YMM_FRAME, REI_YMM_CHIN };
struct rei_ymm_ev { INT32 a, b, c; UINT8 type, reg, val, pad; };
#define REI_YMM_RING 2048
#define REI_YMM_MAXLEN 2048 // output per frame (stereo, so twice the values)
#define REI_YMM_CARRY 4096  // one chip buffer row (same stride as pBuffer)
// Shared (uncached only on PSP)
static struct rei_ymm_ev rei_ymm_ring[REI_YMM_RING] REI_ME_SHARED;
static UINT32 rei_ymm_w REI_ME_SHARED, rei_ymm_r REI_ME_SHARED, rei_ymm_done REI_ME_SHARED;
static INT16 rei_ymm_out[2][REI_YMM_MAXLEN] REI_ME_SHARED;
// Main (rei_ymm_on is below)
static INT32 rei_ymm_clock;
static UINT32 rei_ymm_wl, rei_ymm_frames;
static INT16 *rei_ymm_hold;       // [2][REI_YMM_MAXLEN]: this frame's other-chip share (YM slot 0)
static INT32 rei_ymm_hold_len[2];
static int rei_ymm_posted;        // END sent this frame
static int rei_ymm_have;          // previous frame's share pending
static UINT8 *rei_ymm_xfer;       // chip state + buffer exchange (uncached on both sides)
// ME side (main writes only before enable / after disable)
struct rei_ymm_me_t {
	INT16 buf[2][REI_YMM_CARRY];
	INT32 pos, frac;
	UINT32 r;
	UINT32 size;
	INT32 volk[2], dirs[2];
	rei_dmul_t vol[2];
	INT32 part[2][REI_YMM_MP_LEN]; // (channel split: ME share partial sums)
};
static struct rei_ymm_me_t rei_ymm_me REI_RDR __attribute__((aligned(64)));

extern "C" int BurnYM2151MePoll(void);
extern "C" int rei_ym_me_inline = 0; // (PC harness) main processes directly instead of the ME when it would wait
// If the ME stops responding (~0.5 s), main takes synthesis back - sound is briefly wrong but game/menu/HOME keep running
#define REI_YMM_SPIN_MAX 2000000u
static int rei_ymm_on;
static void rei_ymm_give_up(void); // (after rei_ymm_marks below)
static void rei_ymm_takeover(void); // (below)
// (diag) µs main waited for the ME: 0 ring full, 1 previous frame share (only if the frontend clock rei_defer_clock exists)
extern "C" { unsigned long long rei_ymm_wait_us[2]; }
extern "C" uint64_t (*rei_defer_clock)(void);
static void rei_ymm_put(UINT8 type, INT32 a, INT32 b, UINT8 reg, UINT8 val, INT32 c = 0)
{
	UINT32 spin = 0;
	if (rei_ymm_wl - REI_YMM_LOAD(rei_ymm_r) >= REI_YMM_RING) {
		const uint64_t t0 = rei_defer_clock ? rei_defer_clock() : 0;
		while (rei_ymm_wl - REI_YMM_LOAD(rei_ymm_r) >= REI_YMM_RING && !REI_SH(rei_stop)) {
			if (rei_ym_me_inline) BurnYM2151MePoll();
			else if (++spin > REI_YMM_SPIN_MAX) { rei_ymm_give_up(); return; }
		}
		if (rei_defer_clock) rei_ymm_wait_us[0] += rei_defer_clock() - t0;
	}
	volatile struct rei_ymm_ev *e = (volatile struct rei_ymm_ev *)REI_UNCACHED(&rei_ymm_ring[rei_ymm_wl & (REI_YMM_RING - 1)]);
	e->a = a; e->b = b; e->c = c; e->type = type; e->reg = reg; e->val = val;
	rei_ymm_wl++;
	REI_YMM_STORE(rei_ymm_w, rei_ymm_wl);
}
static UINT32 rei_ymm_marks; // finish marks sent (1 per END/OUT/IN; rei_ymm_done follows as the ME completes)
static void rei_ymm_give_up(void)
{
	rei_ymm_on = 0;
	bprintf(PRINT_IMPORTANT, _T("ym2151: ME did not answer (frame %d, written %u read %u, marks %u done %u), synthesis back on the main CPU\n"),
		(int)nCurrentFrame, (unsigned)rei_ymm_wl, (unsigned)REI_YMM_LOAD(rei_ymm_r), (unsigned)rei_ymm_marks, (unsigned)REI_YMM_LOAD(rei_ymm_done));
}
static int rei_ymm_wait(UINT32 n) // until the ME completes the n-th mark (END/OUT/IN). 0 if gave up because the ME did not respond
{
	UINT32 spin = 0;
	while ((INT32)(REI_YMM_LOAD(rei_ymm_done) - n) < 0 && !REI_SH(rei_stop)) {
		if (rei_ym_me_inline) BurnYM2151MePoll();
		else if (++spin > REI_YMM_SPIN_MAX) { rei_ymm_give_up(); return 0; }
	}
	return 1;
}

// ME: process queued work. 1 if any work was done
extern "C" int BurnYM2151MePoll(void)
{
	struct rei_ymm_me_t *m = &rei_ymm_me;
	const UINT32 w = REI_YMM_LOAD(rei_ymm_w);
	if (m->r == w) return 0;
	while (m->r != w) {
		const volatile struct rei_ymm_ev *e = (const volatile struct rei_ymm_ev *)REI_UNCACHED(&rei_ymm_ring[m->r & (REI_YMM_RING - 1)]);
		const INT32 a = e->a, b = e->b, c = e->c;
		const UINT8 type = e->type, reg = e->reg, val = e->val;
		switch (type) {
		case REI_YMM_RENDER: {
			INT16 *bufs[2] = { m->buf[0] + 4 + m->pos, m->buf[1] + 4 + m->pos };
			if (reg & 0x80) { // (channel split: val = main channels, main partial sums start at c in slot reg & 3)
				const volatile INT32 *ml = (const volatile INT32 *)REI_UNCACHED(&rei_ymm_mpart[reg & 3][0][c]);
				const volatile INT32 *mr = (const volatile INT32 *)REI_UNCACHED(&rei_ymm_mpart[reg & 3][1][c]);
				nbbm_YM2151SetChannelMask((UINT8)~val);
				nbbm_YM2151SetPartialOut(m->part[0], m->part[1]);
				nbbm_YM2151UpdateOne(0, bufs, b);
				nbbm_YM2151SetPartialOut(NULL, NULL);
				nbbm_YM2151SetChannelMask(0xff);
				nbbm_YM2151Combine(bufs[0], bufs[1], m->part[0], m->part[1], ml, mr, b);
			} else {
				nbbm_YM2151UpdateOne(0, bufs, b);
			}
			m->pos += b;
			break;
		}
		case REI_YMM_WRITE:
			nbbm_YM2151WriteReg(0, reg, val);
			break;
		case REI_YMM_SEG: { // a = nSamplesNeeded, b = nSegmentLength (value count), reg = output slot, c = position in frame (value count)
			volatile INT16 *out = (volatile INT16 *)REI_UNCACHED(&rei_ymm_out[reg][c]);
			INT16 *b0 = m->buf[0] + 4, *b1 = m->buf[1] + 4;
			INT32 f = m->frac;
			for (INT32 i = (f & 0xFFFF0000) >> 15; i < b; i += 2, f += m->size) {
				const INT32 p = f >> 16;
				INT32 l[4] = { 0, 0, 0, 0 }, r[4] = { 0, 0, 0, 0 };
				if (m->dirs[0] & BURN_SND_ROUTE_LEFT)  { l[0] += b0[p - 3]; l[1] += b0[p - 2]; l[2] += b0[p - 1]; l[3] += b0[p]; }
				if (m->dirs[0] & BURN_SND_ROUTE_RIGHT) { r[0] += b0[p - 3]; r[1] += b0[p - 2]; r[2] += b0[p - 1]; r[3] += b0[p]; }
				if (m->dirs[1] & BURN_SND_ROUTE_LEFT)  { l[0] += b1[p - 3]; l[1] += b1[p - 2]; l[2] += b1[p - 1]; l[3] += b1[p]; }
				if (m->dirs[1] & BURN_SND_ROUTE_RIGHT) { r[0] += b1[p - 3]; r[1] += b1[p - 2]; r[2] += b1[p - 1]; r[3] += b1[p]; }
				INT32 tl = INTERPOLATE4PS_16BIT((f >> 4) & 0x0fff, l[0], l[1], l[2], l[3]);
				INT32 tr = INTERPOLATE4PS_16BIT((f >> 4) & 0x0fff, r[0], r[1], r[2], r[3]);
				if (m->volk[0] >= 0) tl = BURN_SND_CLIP((tl * m->volk[0]) / 5);
				else { const INT32 v = rei_dmul(tl, &m->vol[0]); tl = BURN_SND_CLIP(v); }
				if (m->volk[1] >= 0) tr = BURN_SND_CLIP((tr * m->volk[1]) / 5);
				else { const INT32 v = rei_dmul(tr, &m->vol[1]); tr = BURN_SND_CLIP(v); }
				out[i + 0] = (INT16)tl;
				out[i + 1] = (INT16)tr;
			}
			const INT32 extra = a - (f >> 16);
			for (INT32 i = -4; i < extra; i++) {
				b0[i] = b0[(f >> 16) + i];
				b1[i] = b1[(f >> 16) + i];
			}
			m->frac = f & 0xFFFF;
			m->pos = extra;
			break;
		}
		case REI_YMM_FRAME:
			REI_YMM_STORE(rei_ymm_done, REI_YMM_LOAD(rei_ymm_done) + 1);
			break;
		case REI_YMM_RESET:
			nbbm_YM2151ResetChip(0);
			break;
		case REI_YMM_OUT: { // chip state and what was synthesized this frame → main
			UINT8 *x = (UINT8 *)REI_UNCACHED(rei_ymm_xfer);
			nbbm_YM2151StateGet(0, (void *)x); // (directly via uncached)
			const INT32 sz = (nbbm_YM2151StateSize() + 63) & ~63; // (same as rei_ymm_xfer_off)
			const INT32 n = 4 + m->pos > 64 ? 4 + m->pos : 64; // (states hold the first 64 - keep the remaining values identical too)
			memcpy(x + sz, m->buf[0], n * sizeof(INT16));
			memcpy(x + sz + REI_YMM_CARRY * sizeof(INT16), m->buf[1], n * sizeof(INT16));
			REI_YMM_STORE(rei_ymm_done, REI_YMM_LOAD(rei_ymm_done) + 1);
			break;
		}
		case REI_YMM_CHIN: { // main → operator state of val channels (channels main gives up in a split)
			nbbm_YM2151CopyChannels(0, (const void *)REI_UNCACHED(rei_ymm_xfer), val);
			REI_YMM_STORE(rei_ymm_done, REI_YMM_LOAD(rei_ymm_done) + 1);
			break;
		}
		case REI_YMM_IN: { // main → full chip state plus buffers/positions (after load)
			const UINT8 *x = (const UINT8 *)REI_UNCACHED(rei_ymm_xfer);
			const INT32 sz = (nbbm_YM2151StateSize() + 63) & ~63; // (same as rei_ymm_xfer_off)
			nbbm_YM2151StateSet(0, x, 1);
			memcpy(m->buf[0], x + sz, REI_YMM_CARRY * sizeof(INT16));
			memcpy(m->buf[1], x + sz + REI_YMM_CARRY * sizeof(INT16), REI_YMM_CARRY * sizeof(INT16));
			m->pos = b;
			m->frac = a;
			REI_YMM_STORE(rei_ymm_done, REI_YMM_LOAD(rei_ymm_done) + 1);
			break;
		}
		}
		m->r++;
		REI_YMM_STORE(rei_ymm_r, m->r);
	}
	return 1;
}
#endif

static void YM2151Render(INT32 nSegmentLength)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("YM2151Render called without init\n"));
#endif

	if (nYM2151Position >= nSegmentLength || !pBurnSoundOut) {
		return;
	}

	//bprintf(PRINT_NORMAL, _T("    YM2151 render %6i -> %6i\n"), nYM2151Position, nSegmentLength);

	nSegmentLength -= nYM2151Position;

	if (nSegmentLength < 1) return;

#ifdef REI_YM_ME
	if (rei_ymm_on && rei_ymm_mmask && rei_ymm_mp_pos + nSegmentLength > REI_YMM_MP_LEN)
		rei_ymm_takeover(); // (out of frame slots - consecutive frames with no sound output: main takes over everything)
	if (rei_ymm_on) { // (the ME synthesizes at the same positions/lengths)
		if (rei_ymm_mmask) { // (channel split: main share as partial sums - UpdateOne also counts unbuffered chip timers like TimersOnly)
			INT16 *bufs[2] = { pBuffer + 0 * 4096 + 4 + nYM2151Position, pBuffer + 1 * 4096 + 4 + nYM2151Position }; // (not written)
			const UINT32 s = rei_ymm_mp_slot, p = rei_ymm_mp_pos;
			YM2151SetChannelMask(rei_ymm_mmask);
			YM2151SetPartialOut((INT32 *)REI_UNCACHED(&rei_ymm_mpart[s][0][p]), (INT32 *)REI_UNCACHED(&rei_ymm_mpart[s][1][p]));
			YM2151UpdateOne(0, bufs, nSegmentLength);
			YM2151SetPartialOut(NULL, NULL);
			rei_ymm_put(REI_YMM_RENDER, nYM2151Position, nSegmentLength, (UINT8)(0x80 | s), rei_ymm_mmask, (INT32)p);
			rei_ymm_mp_pos += nSegmentLength;
		} else {
			if (!bBurnYM2151IsBuffered) YM2151TimersOnly(0, nSegmentLength); // (unbuffered: chip timers are counted in synth calls - main does timers only)
			rei_ymm_put(REI_YMM_RENDER, nYM2151Position, nSegmentLength, 0, 0);
		}
		nYM2151Position += nSegmentLength;
		return;
	}
#endif
	pYM2151Buffer[0] = pBuffer + 0 * 4096 + 4 + nYM2151Position;
	pYM2151Buffer[1] = pBuffer + 1 * 4096 + 4 + nYM2151Position;

	REI_PROF_BEGIN(ym);
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) YM2151SetPartialOut(rei_ym_pa + 4 + nYM2151Position, rei_ym_pa + 4096 + 4 + nYM2151Position);
#endif
	YM2151UpdateOne(0, &pYM2151Buffer[0], nSegmentLength);
	REI_PROF_END(ym, REI_PROF_YM);

	if (bYM2151_MultiChip) {
		pYM2151Buffer[2] = pBuffer + 2 * 4096 + 4 + nYM2151Position;
		pYM2151Buffer[3] = pBuffer + 3 * 4096 + 4 + nYM2151Position;

		YM2151UpdateOne(1, &pYM2151Buffer[2], nSegmentLength);
	}

	nYM2151Position += nSegmentLength;
}

void BurnYM2151UpdateRequest()
{
	if (bBurnYM2151IsBuffered) {
		YM2151Render(BurnYM2151StreamCallback(nBurnYM2151SoundRate));
	}
}

// ----------------------------------------------------------------------------
// Update the sound buffer

#define INTERPOLATE_ADD_SOUND_LEFT(chip, route, buffer)																	\
	if ((YM2151RouteDirs[chip][route] & BURN_SND_ROUTE_LEFT) == BURN_SND_ROUTE_LEFT) {									\
		nLeftSample[chip][0] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 3]/* * YM2151Volumes[route]*/);	\
		nLeftSample[chip][1] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 2]/* * YM2151Volumes[route]*/);	\
		nLeftSample[chip][2] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 1]/* * YM2151Volumes[route]*/);	\
		nLeftSample[chip][3] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 0]/* * YM2151Volumes[route]*/);	\
	}

#define INTERPOLATE_ADD_SOUND_RIGHT(chip, route, buffer)																	\
	if ((YM2151RouteDirs[chip][route] & BURN_SND_ROUTE_RIGHT) == BURN_SND_ROUTE_RIGHT) {									\
		nRightSample[chip][0] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 3]/* * YM2151Volumes[route]*/);	\
		nRightSample[chip][1] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 2]/* * YM2151Volumes[route]*/);	\
		nRightSample[chip][2] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 1]/* * YM2151Volumes[route]*/);	\
		nRightSample[chip][3] += (INT32)(pYM2151Buffer[buffer][(nFractionalPosition >> 16) - 0]/* * YM2151Volumes[route]*/);	\
	}

void BurnYM2151Render(INT16* pSoundBuf, INT32 nSegmentEnd)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151Update called without init\n"));
#endif

	if (nBurnSoundRate == 0 || pBurnSoundOut == NULL) {
		return;
	}

	if (bBurnYM2151IsBuffered && nSegmentEnd != nBurnSoundLen) {
		bprintf(0, _T("BurnYM2151Render() - once per frame, please!\n"));
		return;
	}

//	bprintf(PRINT_NORMAL, _T("    YM2151 render %6i -> %6i\n"), nYM2151Position, nSegmentEnd);

#ifdef REI_YM_THREAD
	if (rei_ymt_on) rei_ymt_drain(); // (synthesize pending writes so nYM2151Position matches the interpreter path, then compute)
#endif
	INT32 nSegmentLength = nSegmentEnd;
	INT32 nSamplesNeeded = nSegmentEnd * nBurnYM2151SoundRate / nBurnSoundRate + 1;

	if (nSamplesNeeded < nYM2151Position) {
		nSamplesNeeded = nYM2151Position;
	}

	if (nSegmentLength > nBurnSoundLen) {
		nSegmentLength = nBurnSoundLen;
	}
	nSegmentLength <<= 1;

#ifdef REI_YM_ME
	if (rei_ymm_on) {
		YM2151Render(nSamplesNeeded); // (synthesis request to the ME)
		// Main also tracks the resample position (next frame's write sample positions are set here). YM share stays 0, and in the next frame
		const INT32 i0 = (nFractionalPosition & 0xFFFF0000) >> 15;
		INT32 f = nFractionalPosition;
		if (nSegmentLength > i0) {
			f += (UINT32)((nSegmentLength - i0 + 1) >> 1) * nSampleSize;
			memset(pSoundBuf + i0, 0, (nSegmentLength - i0) * sizeof(INT16));
		}
		rei_ymm_put(REI_YMM_SEG, nSamplesNeeded, nSegmentLength, (UINT8)(rei_ymm_frames & 1), 0, (INT32)(pSoundBuf - pBurnSoundOut));
		rei_ymm_posted = 1;
		const INT32 extra = nSamplesNeeded - (f >> 16);
		nFractionalPosition = f & 0xFFFF;
		nYM2151Position = extra;
		return;
	}
#endif
#ifdef REI_YM_LAZY
	rei_ym_flush();
#endif
#ifdef REI_YM_THREAD
	if (rei_ymt_on) { // wait for the worker to synthesize up to frame end
		rei_ymt_tag = (rei_ymt_tag + 1) & 0xffff;
		rei_ymt_put(1, nSamplesNeeded, 0, (UINT8)(rei_ymt_tag >> 8), (UINT8)rei_ymt_tag);
		while (__atomic_load_n(&rei_ymt_done, __ATOMIC_ACQUIRE) != rei_ymt_tag && !rei_stop)
			rei_ymt_wait();
	} else
#endif
	YM2151Render(nSamplesNeeded);
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) {
		const INT32 end = nYM2151Position; // >= nSamplesNeeded
		rei_ymf_tag = (rei_ymf_tag + 1) & 0xffff;
		rei_ymf_put(REI_YMF_END, end, (UINT8)(rei_ymf_tag >> 8), (UINT8)rei_ymf_tag);
		while (REI_YMF_LOAD(rei_ymf_done) != rei_ymf_tag && !REI_SH(rei_stop)) { } // the main CPU finishes its share up to 'end'
		const INT32 s0 = rei_ym_frame_start;
		if (end > s0)
			YM2151Combine(pBuffer + 0 * 4096 + 4 + s0, pBuffer + 1 * 4096 + 4 + s0, rei_ym_pa + 4 + s0, rei_ym_pa + 4096 + 4 + s0,
				(const volatile INT32 *)REI_UNCACHED(&rei_ymf_pb[0][4 + s0]), (const volatile INT32 *)REI_UNCACHED(&rei_ymf_pb[1][4 + s0]), end - s0);
	}
#endif

	pYM2151Buffer[0] = pBuffer + 0 * 4096 + 4;
	pYM2151Buffer[1] = pBuffer + 1 * 4096 + 4;

	if (bYM2151_MultiChip) {
		pYM2151Buffer[2] = pBuffer + 2 * 4096 + 4;
		pYM2151Buffer[3] = pBuffer + 3 * 4096 + 4;
	}

	static rei_dmul_t rei_ym_vol[2][2]; // (some boards call this per segment, so re-prepare only when values change)
	for (INT32 c = 0; c < 2; c++)
		for (INT32 r = 0; r < 2; r++)
			rei_dmul_set(&rei_ym_vol[c][r], YM2151Volumes[c][r]);

	for (INT32 i = (nFractionalPosition & 0xFFFF0000) >> 15; i < nSegmentLength; i += 2, nFractionalPosition += nSampleSize) {
		INT32 nLeftSample[2][4] = { {0, 0, 0, 0}, {0, 0, 0, 0} };
		INT32 nRightSample[2][4] = { {0, 0, 0, 0}, {0, 0, 0, 0} };
		INT32 nTotalLeftSample[2] = { 0, 0 }, nTotalRightSample[2] = { 0, 0 };

		INTERPOLATE_ADD_SOUND_LEFT  (0, BURN_SND_YM2151_YM2151_ROUTE_1, 0)
		INTERPOLATE_ADD_SOUND_RIGHT (0, BURN_SND_YM2151_YM2151_ROUTE_1, 0)
		INTERPOLATE_ADD_SOUND_LEFT  (0, BURN_SND_YM2151_YM2151_ROUTE_2, 1)
		INTERPOLATE_ADD_SOUND_RIGHT (0, BURN_SND_YM2151_YM2151_ROUTE_2, 1)

		if (bYM2151_MultiChip) {
			INTERPOLATE_ADD_SOUND_LEFT  (1, BURN_SND_YM2151_YM2151_ROUTE_1, 2)
			INTERPOLATE_ADD_SOUND_RIGHT (1, BURN_SND_YM2151_YM2151_ROUTE_1, 2)
			INTERPOLATE_ADD_SOUND_LEFT  (1, BURN_SND_YM2151_YM2151_ROUTE_2, 3)
			INTERPOLATE_ADD_SOUND_RIGHT (1, BURN_SND_YM2151_YM2151_ROUTE_2, 3)
		}

		for (INT32 chip = 0; chip < (bYM2151_MultiChip + 1); chip++) {
			nTotalLeftSample[chip]  = INTERPOLATE4PS_16BIT((nFractionalPosition >> 4) & 0x0fff, nLeftSample[chip][0], nLeftSample[chip][1], nLeftSample[chip][2], nLeftSample[chip][3]);
			nTotalRightSample[chip] = INTERPOLATE4PS_16BIT((nFractionalPosition >> 4) & 0x0fff, nRightSample[chip][0], nRightSample[chip][1], nRightSample[chip][2], nRightSample[chip][3]);

			const INT32 k1 = YM2151VolK[chip][BURN_SND_YM2151_YM2151_ROUTE_1], k2 = YM2151VolK[chip][BURN_SND_YM2151_YM2151_ROUTE_2];
			// (non-k/5 volume: rei_dmul - same value as the double multiply. clip-then-truncate = truncate-then-clip)
			if (k1 >= 0) nTotalLeftSample[chip] = BURN_SND_CLIP((nTotalLeftSample[chip] * k1) / 5);
			else { const INT32 v = rei_dmul(nTotalLeftSample[chip], &rei_ym_vol[chip][BURN_SND_YM2151_YM2151_ROUTE_1]); nTotalLeftSample[chip] = BURN_SND_CLIP(v); }
			if (k2 >= 0) nTotalRightSample[chip] = BURN_SND_CLIP((nTotalRightSample[chip] * k2) / 5);
			else { const INT32 v = rei_dmul(nTotalRightSample[chip], &rei_ym_vol[chip][BURN_SND_YM2151_YM2151_ROUTE_2]); nTotalRightSample[chip] = BURN_SND_CLIP(v); }
		}

		if (bYM2151AddSignal) {
			pSoundBuf[i + 0] = BURN_SND_CLIP(pSoundBuf[i + 0] + nTotalLeftSample[0] + nTotalLeftSample[1]);
			pSoundBuf[i + 1] = BURN_SND_CLIP(pSoundBuf[i + 1] + nTotalRightSample[0] + nTotalRightSample[1]);
		} else {
			pSoundBuf[i + 0] = nTotalLeftSample[0] + nTotalLeftSample[1];
			pSoundBuf[i + 1] = nTotalRightSample[0] + nTotalRightSample[1];
		}
	}

	if (!bBurnYM2151IsBuffered || (bBurnYM2151IsBuffered && nSegmentEnd >= nBurnSoundLen)) {
		INT32 nExtraSamples = nSamplesNeeded - (nFractionalPosition >> 16);
		//if (nExtraSamples !=0 && nExtraSamples !=1)
		//	bprintf(0, _T("nSegmentEnd   %d    nExtraSamples   %d\n"),nSegmentEnd,nExtraSamples);
		for (INT32 i = -4; i < nExtraSamples; i++) {
			pYM2151Buffer[0][i] = pYM2151Buffer[0][(nFractionalPosition >> 16) + i];
			pYM2151Buffer[1][i] = pYM2151Buffer[1][(nFractionalPosition >> 16) + i];
			if (bYM2151_MultiChip) {
				pYM2151Buffer[2][i] = pYM2151Buffer[2][(nFractionalPosition >> 16) + i];
				pYM2151Buffer[3][i] = pYM2151Buffer[3][(nFractionalPosition >> 16) + i];
			}
		}

		nFractionalPosition &= 0xFFFF;

		nYM2151Position = nExtraSamples;
#ifdef REI_YM_SPLIT
		if (rei_ym_split_on) {
			rei_ym_frame_start = nExtraSamples;
			rei_ymf_put(REI_YMF_START, nExtraSamples, 0, 0);
		}
#endif
	}
}

#ifdef REI_YM_SPLIT
// main_mask: channels the main CPU synthesizes (0 = none). Before the sound side starts. 0 on success
extern "C" int BurnYM2151SplitInit(UINT8 main_mask)
{
	if (!main_mask || bYM2151_MultiChip || !bBurnYM2151IsBuffered) return main_mask ? 1 : 0;
	if (!rei_ym_pa) rei_ym_pa = (INT32 *)BurnMalloc(2 * 4096 * sizeof(INT32));
	if (!rei_ym_pa || rei_ym_main_init(rei_ym_clock, nBurnYM2151SoundRate, main_mask)) return 1;
	YM2151SetChannelMask((UINT8)~main_mask);
	memset(rei_ym_pa, 0, 2 * 4096 * sizeof(INT32));
	rei_ymf_wl = 0;
	rei_ymf_tag = 0;
	REI_YMF_STORE(rei_ymf_w, 0);
	REI_YMF_STORE(rei_ymf_r, 0);
	REI_YMF_STORE(rei_ymf_done, 0);
	rei_ym_frame_start = nYM2151Position;
	rei_ym_split_on = 1;
	return 0;
}
#endif

#ifdef REI_YM_ME
extern "C" int BurnYM2151MeSupported(void)
{
	// Buffered mode uses BurnTimer chips; unbuffered (M90: render per segment) uses in-chip timers - counted by main via YM2151TimersOnly
	return DebugSnd_YM2151Initted && (bBurnYM2151IsBuffered ? YM2151BurnTimer : !YM2151BurnTimer) && !bYM2151_MultiChip &&
		!bYM2151AddSignal && pBuffer && nBurnSoundLen > 0 && nBurnSoundLen * 2 <= REI_YMM_MAXLEN;
}
// chip state size + two buffer rows
static INT32 rei_ymm_xfer_size(void) { return ((YM2151StateSize() + 63) & ~63) + 2 * REI_YMM_CARRY * (INT32)sizeof(INT16); }
static INT32 rei_ymm_xfer_off(void) { return (YM2151StateSize() + 63) & ~63; }

// Enable: before the ME runs (main fills ME-side state - the frontend flushes the data cache when starting the ME). 0 on success
// Disable: after the ME stops (synthesis state is discarded: only when quitting the game)
extern "C" int BurnYM2151MeEnable(int on)
{
	if (rei_ymm_on && on) return 0;
	if (rei_ymm_mmask) { // (end channel split - main chip synthesizes everything when enabling/disabling)
		rei_ymm_mmask = 0;
		YM2151SetChannelMask(0xff);
		YM2151SetPartialOut(NULL, NULL);
	}
	rei_ymm_mp_slot = rei_ymm_mp_pos = 0;
	if (!on) {
		if (rei_ymm_on) nbbm_YM2151Shutdown();
		rei_ymm_on = 0;
		return 0;
	}
	if (!BurnYM2151MeSupported()) return 1;
#ifdef REI_YM_LAZY
	rei_ym_flush();
#endif
	if (!rei_ymm_hold) rei_ymm_hold = (INT16 *)BurnMalloc(2 * REI_YMM_MAXLEN * sizeof(INT16));
	if (!rei_ymm_xfer) rei_ymm_xfer = (UINT8 *)memalign(64, (rei_ymm_xfer_size() + 63) & ~63);
	if (!rei_ymm_hold || !rei_ymm_xfer || rei_ym_copy_init(rei_ymm_clock, nBurnYM2151SoundRate)) return 1;
	// copy = main chip (state synthesized so far), ME buffer = main buffer
	YM2151StateGet(0, rei_ymm_xfer);
	nbbm_YM2151StateSet(0, rei_ymm_xfer, 1);
	struct rei_ymm_me_t *m = &rei_ymm_me;
	memcpy(m->buf[0], pBuffer + 0 * 4096, sizeof(m->buf[0]));
	memcpy(m->buf[1], pBuffer + 1 * 4096, sizeof(m->buf[1]));
	m->pos = nYM2151Position;
	m->frac = nFractionalPosition;
	m->r = 0;
	m->size = nSampleSize;
	for (INT32 r = 0; r < 2; r++) {
		m->volk[r] = YM2151VolK[0][r];
		m->dirs[r] = YM2151RouteDirs[0][r];
		rei_dmul_set(&m->vol[r], YM2151Volumes[0][r]);
	}
	rei_ymm_wl = 0;
	rei_ymm_marks = 0;
	REI_YMM_STORE(rei_ymm_w, 0);
	REI_YMM_STORE(rei_ymm_r, 0);
	REI_YMM_STORE(rei_ymm_done, 0);
	rei_ymm_frames = 0;
	rei_ymm_have = rei_ymm_posted = 0;
	rei_ymm_on = 1;
	return 0;
}

// ME synthesis state and this frame's buffer → main chip (before save, when main takes over for CSM)
static void rei_ymm_pull(void)
{
	rei_ymm_put(REI_YMM_OUT, 0, 0, 0, 0);
	if (!rei_ymm_on || !rei_ymm_wait(++rei_ymm_marks)) return;
	const UINT8 *x = (const UINT8 *)REI_UNCACHED(rei_ymm_xfer);
	if (rei_ymm_mmask) { // (channel split: main channels are correct on main - those channels in the ME copy were frozen)
		static UINT8 *own;
		if (!own) own = (UINT8 *)BurnMalloc(YM2151StateSize());
		if (!own) { rei_ymm_give_up(); return; }
		YM2151StateGet(0, own);
		YM2151StateSet(0, x, 0);
		YM2151CopyChannels(0, own, rei_ymm_mmask);
	} else {
		YM2151StateSet(0, x, 0);
	}
	const INT32 o = rei_ymm_xfer_off(), n = 4 + nYM2151Position > 64 ? 4 + nYM2151Position : 64;
	for (INT32 i = 0; i < n; i++) { // (uncached)
		pBuffer[0 * 4096 + i] = ((const volatile INT16 *)(x + o))[i];
		pBuffer[1 * 4096 + i] = ((const volatile INT16 *)(x + o))[REI_YMM_CARRY + i];
	}
}
// All main chip state, buffers, positions → ME copy (after load)
static void rei_ymm_push(void)
{
	UINT8 *x = (UINT8 *)REI_UNCACHED(rei_ymm_xfer);
	YM2151StateGet(0, x);
	const INT32 o = rei_ymm_xfer_off();
	memcpy(x + o, pBuffer + 0 * 4096, REI_YMM_CARRY * sizeof(INT16));
	memcpy(x + o + REI_YMM_CARRY * sizeof(INT16), pBuffer + 1 * 4096, REI_YMM_CARRY * sizeof(INT16));
	REI_ME_SYNC();
	rei_ymm_put(REI_YMM_IN, nFractionalPosition, nYM2151Position, 0, 0);
	if (rei_ymm_on) rei_ymm_wait(++rei_ymm_marks);
	rei_ymm_have = 0; // (previous frame share discarded)
}
// CSM: main synthesizes from now on (the late previous frame share is discarded)
static void rei_ymm_takeover(void)
{
	bprintf(PRINT_IMPORTANT, _T("ym2151: main CPU takes the synthesis over (frame %d)\n"), (int)nCurrentFrame);
	rei_ymm_pull();
	rei_ymm_on = 0;
	rei_ymm_have = 0;
	if (rei_ymm_mmask) { // (main chip owns everything again - the pull above fetched the ME channels)
		rei_ymm_mmask = 0;
		YM2151SetChannelMask(0xff);
		YM2151SetPartialOut(NULL, NULL);
	}
}

// Main: switch to channel-split synthesis (when the ME overruns - decided by the frontend, between frames). mask = channels for main. 0 on success.
// Fetch ME state into the main chip (main channels kept), align the newly taken channels, then split (sound continues).
// Main's share only grows - shrinking would need sending main channels to the ME (not done)
extern "C" int BurnYM2151MeSplit(UINT8 mask)
{
	if (!rei_ymm_on || mask == 0xff || mask == rei_ymm_mmask) return 1;
	if (rei_ymm_mmask & ~mask) { // shrink only (mask ⊂ current): send the state of channels main gives up to the ME copy (main's is correct)
		if (mask & ~rei_ymm_mmask) return 1;
		const UINT8 give = rei_ymm_mmask & ~mask;
		YM2151StateGet(0, (void *)REI_UNCACHED(rei_ymm_xfer));
		REI_ME_SYNC();
		rei_ymm_put(REI_YMM_CHIN, 0, 0, 0, give);
		if (!rei_ymm_on || !rei_ymm_wait(++rei_ymm_marks)) return 1;
		rei_ymm_mmask = mask;
		if (!mask) { // (back to all on the ME - main chip does timers only)
			YM2151SetChannelMask(0xff);
			YM2151SetPartialOut(NULL, NULL);
		}
	} else { // grow (mask ⊃ current): fetch ME state (main channels kept) and align the newly taken channels
		if (!mask) return 1;
		rei_ymm_pull();
		if (!rei_ymm_on) return 1;
		rei_ymm_mmask = mask;
	}
	bprintf(PRINT_IMPORTANT, _T("ym2151: channels %02x on the main CPU, the rest on the ME (frame %d)\n"), mask, (int)nCurrentFrame);
	return 0;
}
extern "C" int BurnYM2151MeSplitMask(void) { return rei_ymm_on ? rei_ymm_mmask : -1; }

// Main: main synthesizes from now on (when the ME overruns - decided by the frontend). Call between frames. One frame of sound is lost
extern "C" void BurnYM2151MeRelease(void)
{
	if (rei_ymm_on) rei_ymm_takeover();
}
// Main: after the driver frame. audio = this frame's other-chip share → replaced with the previous frame's finished sound
extern "C" void BurnYM2151MeFrameEnd(INT16 *audio, INT32 len)
{
	if (!rei_ymm_on) return;
	if (!rei_ymm_posted || !audio) { // (no YM output this frame)
		rei_ymm_have = 0;
		return;
	}
	rei_ymm_posted = 0;
	rei_ymm_put(REI_YMM_FRAME, 0, 0, 0, 0); // (after this frame's segments - finish count +1 when the ME gets here)
	rei_ymm_marks++;
	rei_ymm_mp_slot = (rei_ymm_mp_slot + 1) % REI_YMM_MP_SLOTS; // (channel split: next frame's main partial-sum slot)
	rei_ymm_mp_pos = 0;
	const UINT32 k = rei_ymm_frames++;
	INT16 *cur = rei_ymm_hold + (k & 1) * REI_YMM_MAXLEN;
	const INT32 n = len * 2;
	memcpy(cur, audio, n * sizeof(INT16));
	rei_ymm_hold_len[k & 1] = n;
	if (!rei_ymm_have) {
		rei_ymm_have = 1;
		memset(audio, 0, n * sizeof(INT16));
		return;
	}
	// the previous frame's (k-1) FRAME is the mark just before this frame's FRAME
	const uint64_t tw0 = rei_defer_clock ? rei_defer_clock() : 0;
	if (!rei_ymm_wait(rei_ymm_marks - 1)) { // (ME not responding: this frame goes out as is, without YM)
		rei_ymm_have = 0;
		return;
	}
	if (rei_defer_clock) rei_ymm_wait_us[1] += rei_defer_clock() - tw0;
	const INT16 *prev = rei_ymm_hold + ((k - 1) & 1) * REI_YMM_MAXLEN;
	const volatile INT32 *y = (const volatile INT32 *)REI_UNCACHED(&rei_ymm_out[(k - 1) & 1][0]);
	const INT32 pn = rei_ymm_hold_len[(k - 1) & 1] < n ? rei_ymm_hold_len[(k - 1) & 1] : n;
	for (INT32 i = 0; i < pn; i += 2) {
		const INT32 v = y[i >> 1]; // (left and right at once)
		audio[i + 0] = BURN_SND_CLIP(prev[i + 0] + (INT16)(v & 0xffff));
		audio[i + 1] = BURN_SND_CLIP(prev[i + 1] + (INT16)(v >> 16));
	}
	for (INT32 i = pn; i < n; i++) audio[i] = 0;
}
#endif

void BurnYM2151Reset()
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151Reset called without init\n"));
#endif

	if (YM2151BurnTimer)
		BurnTimerReset();

#ifdef REI_YM_LAZY
	rei_ym_flush();
	rei_ym_csm = 0;
#endif
#ifdef REI_YM_THREAD
	if (rei_ymt_on) rei_ymt_drain();
#endif
	YM2151ResetChip(0);
#ifdef REI_YM_ME
	if (rei_ymm_on) rei_ymm_put(REI_YMM_RESET, 0, 0, 0, 0);
#endif
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) {
		rei_ym_frame_start = nYM2151Position;
		rei_ymf_put(REI_YMF_RESET, nYM2151Position, 0, 0);
	}
#endif
}

void BurnYM2151Exit()
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151Exit called without init\n"));
#endif

	if (!DebugSnd_YM2151Initted) return;
#ifdef REI_YM_THREAD
	if (rei_ymt_on) rei_ymt_drain();
	rei_ymt_on = 0;
#endif

#ifdef REI_YM_ME
	BurnYM2151MeEnable(0); // (ME already stopped)
	BurnFree(rei_ymm_hold);
	rei_ymm_hold = NULL;
	if (rei_ymm_xfer) free(rei_ymm_xfer);
	rei_ymm_xfer = NULL;
#endif
	BurnYM2151SetIrqHandler(NULL);
	BurnYM2151SetPortHandler(NULL);

	YM2151Shutdown();

	if (YM2151BurnTimer) {
		BurnTimerExit();
		YM2151BurnTimer = 0;
	}

	BurnFree(pBuffer);
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) {
		rei_ym_main_exit();
		YM2151SetChannelMask(0xff);
		YM2151SetPartialOut(NULL, NULL);
		rei_ym_split_on = 0;
	}
	BurnFree(rei_ym_pa);
	rei_ym_pa = NULL;
#endif
#ifdef REI_YM_LAZY
	rei_ym_r = rei_ym_w = 0;
	rei_ym_csm = 0;
#endif

	bBurnYM2151IsBuffered = 0;
	BurnYM2151StreamCallback = NULL;
	bYM2151AddSignal = 0;

	bYM2151_MultiChip = 0;

	DebugSnd_YM2151Initted = 0;
}

#ifdef REI_YM_THREAD
extern "C" int BurnYM2151WorkerSupported(void)
{
	return DebugSnd_YM2151Initted && bBurnYM2151IsBuffered && !bYM2151_MultiChip;
}
#endif

void BurnYM2151SetMultiChip(INT32 bYes) // set before init!
{
	bYM2151_MultiChip = (bYes) ? 1 : 0;
}

void BurnYM2151InitBuffered(INT32 nClockFrequency, INT32 use_timer, INT32 (*StreamCallback)(INT32), INT32 bAdd)
{
	BurnYM2151Init(nClockFrequency, use_timer);

	if (use_timer && StreamCallback == NULL) {
		StreamCallback = BurnSynchroniseStream; // BurnTimer has us covered!
	}

	bBurnYM2151IsBuffered = (StreamCallback != NULL);
	BurnYM2151StreamCallback = StreamCallback;

	if (bBurnYM2151IsBuffered) {
		bprintf(0, _T("YM2151: Using Buffered-mode.\n"));
	}

	bYM2151AddSignal = bAdd;
}

INT32 BurnYM2151Init(INT32 nClockFrequency)
{
	return BurnYM2151Init(nClockFrequency, 0);
}

INT32 BurnYM2151Init(INT32 nClockFrequency, INT32 use_timer)
{
	DebugSnd_YM2151Initted = 1;

	bBurnYM2151IsBuffered = 0; // Can I recommend BurnYM2151InitBuffered()? :)
	BurnYM2151StreamCallback = NULL; // ""
	bYM2151AddSignal = 0; // ""

	// Set YM2151 core samplerate to match the hardware
	nBurnYM2151SoundRate = nClockFrequency >> 6;
	// Bring YM2151 core samplerate within usable range
	while (nBurnYM2151SoundRate > nBurnSoundRate * 3) {
		nBurnYM2151SoundRate >>= 1;
	}
#ifdef REI_YM_RATE_SHIFT
	// ReiREM PSP port (approved 2026-10-09): lower synthesis rate to cut sound-side cost (intended difference, PERF.md)
	nBurnYM2151SoundRate >>= REI_YM_RATE_SHIFT;
#endif

	INT32 timer_chipbase = 0;

	if (use_timer)
	{
		bprintf(0, _T("YM2151: Using FM-Timer.\n"));
		YM2151BurnTimer = 1;
		timer_chipbase = BurnTimerInit(&ym2151_timer_over, NULL, (bYM2151_MultiChip) ? 2 : 1);
		bprintf(0, _T("BurnTimer chip_base: %d\n"), timer_chipbase);
	}

	YM2151Init((bYM2151_MultiChip) ? 2 : 1, timer_chipbase, nClockFrequency, nBurnYM2151SoundRate, (YM2151BurnTimer) ? BurnOPMTimerCallback : NULL);
#ifdef REI_YM_SPLIT
	rei_ym_clock = nClockFrequency;
#endif
#ifdef REI_YM_ME
	rei_ymm_clock = nClockFrequency;
#endif

	pBuffer = (INT16*)BurnMalloc(65536 * 4 * sizeof(INT16));
	memset(pBuffer, 0, 65536 * 4 * sizeof(INT16));

	if (nBurnSoundRate) nSampleSize = (UINT32)nBurnYM2151SoundRate * (1 << 16) / nBurnSoundRate;
	nFractionalPosition = 0;
	nYM2151Position = 0;

	// default routes
	for (INT32 i = 0; i < 2; i++) {
		YM2151Volumes[i][BURN_SND_YM2151_YM2151_ROUTE_1] = 1.00;
		YM2151Volumes[i][BURN_SND_YM2151_YM2151_ROUTE_2] = 1.00;
		YM2151VolK[i][BURN_SND_YM2151_YM2151_ROUTE_1] = YM2151VolK[i][BURN_SND_YM2151_YM2151_ROUTE_2] = 5;
		YM2151RouteDirs[i][BURN_SND_YM2151_YM2151_ROUTE_1] = BURN_SND_ROUTE_BOTH;
		YM2151RouteDirs[i][BURN_SND_YM2151_YM2151_ROUTE_2] = BURN_SND_ROUTE_BOTH;
	}

	return 0;
}

// Some games make heavy use of the B timer, to increase the accuracy of
// this timer, call this with the number of times per frame BurnYM2151Render()
// is called.  See drv/dataeast/d_rohga for example usage.
void BurnYM2151SetInterleave(INT32 nInterleave)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151SetInterleave called without init\n"));
#endif

	YM2151SetTimerInterleave(nInterleave * (nBurnFPS / 100));
}

void BurnYM2151SetRoute(INT32 chip, INT32 nIndex, double nVolume, INT32 nRouteDir)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151SetRoute called without init\n"));
	if (nIndex < 0 || nIndex > 1) bprintf(PRINT_ERROR, _T("BurnYM2151SetRoute called with invalid index %i\n"), nIndex);
#endif
	
	YM2151Volumes[chip][nIndex] = nVolume;
	YM2151VolK[chip][nIndex] = rei_gain_k(nVolume);
	YM2151RouteDirs[chip][nIndex] = nRouteDir;
}

void BurnYM2151SetRoute(INT32 nIndex, double nVolume, INT32 nRouteDir)
{
	BurnYM2151SetRoute(0, nIndex, nVolume, nRouteDir);
}

void BurnYM2151SetIrqHandler(INT32 chip, void (*irq_cb)(INT32))
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151SetIrqHandler called without init\n"));
#endif

	YM2151SetIrqHandler(chip, irq_cb);
}

void BurnYM2151SetIrqHandler(void (*irq_cb)(INT32))
{
	BurnYM2151SetIrqHandler(0, irq_cb);
}

void BurnYM2151SetPortHandler(INT32 chip, write8_handler port_cb)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151SetPortHandler called without init\n"));
#endif

	YM2151SetPortWriteHandler(chip, port_cb);
}

void BurnYM2151SetPortHandler(write8_handler port_cb)
{
	BurnYM2151SetPortHandler(0, port_cb);
}

void BurnYM2151SetAllRoutes(double vol, INT32 route)
{
	BurnYM2151SetRoute(BURN_SND_YM2151_YM2151_ROUTE_1, vol, route);
	BurnYM2151SetRoute(BURN_SND_YM2151_YM2151_ROUTE_2, vol, route);
}

void BurnYM2151SetAllRoutes(INT32 chip, double vol, INT32 route)
{
	BurnYM2151SetRoute(chip, BURN_SND_YM2151_YM2151_ROUTE_1, vol, route);
	BurnYM2151SetRoute(chip, BURN_SND_YM2151_YM2151_ROUTE_2, vol, route);
}

void BurnYM2151Scan(INT32 nAction, INT32 *pnMin)
{
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151Scan called without init\n"));
#endif
	
	if ((nAction & ACB_DRIVER_DATA) == 0) {
		return;
	}

#ifdef REI_YM_LAZY
	rei_ym_flush(); // a saved state never holds logged writes (loading: nothing is pending either)
#endif
#ifdef REI_YM_THREAD
	if (rei_ymt_on) rei_ymt_drain();
#endif
#ifdef REI_YM_ME
	if (rei_ymm_on) rei_ymm_pull(); // (make main chip/buffers match the ME copy)
#endif
	SCAN_VAR(nBurnCurrentYM2151Register);

	// ReiREM Vita port: also keep the resampler phase and the samples carried over to the next frame
	// (buffer heads), so audio after loading a state is identical, not just close
	SCAN_VAR(nYM2151Position);
	SCAN_VAR(nFractionalPosition);
	if (pBuffer) {
		struct BurnArea ba;
		for (INT32 i = 0; i < (bYM2151_MultiChip ? 4 : 2); i++) {
			memset(&ba, 0, sizeof(ba));
			ba.Data = pBuffer + i * 4096;
			ba.nLen = 64 * sizeof(INT16);
			ba.szName = "YM2151 carry";
			BurnAcb(&ba);
		}
	}

	BurnYM2151Scan_int(nAction); // Scan the YM2151's internal registers

	if (YM2151BurnTimer)
		BurnTimerScan(nAction, pnMin);
#ifdef REI_YM_ME
	if (rei_ymm_on && (nAction & ACB_WRITE)) rei_ymm_push(); // (loaded: ME copy too)
#endif
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on && (nAction & ACB_WRITE)) { // (loaded: main-share copy to this chip state/buffer position - so old instruments don't linger)
		static UINT8 *st;
		if (!st) st = (UINT8 *)malloc(YM2151StateSize()); // (survives game switches - BurnMalloc is freed on exit)
		if (st) {
			YM2151StateGet(0, st);
			rei_ym_main_resync(st, nYM2151Position);
			rei_ym_frame_start = nYM2151Position;
		}
	}
#endif
}

void BurnYM2151Write(INT32 chip, INT32 offset, const UINT8 nData)
{
#ifdef REI_EV_TRACE
	if (offset & 1) fprintf(stderr, "ym f%d r%02x v%02x p%d\n", (int)nCurrentFrame, nBurnCurrentYM2151Register[chip] & 0xff, nData,
		bBurnYM2151IsBuffered ? BurnYM2151StreamCallback(nBurnYM2151SoundRate) : -1);
#endif
#if defined FBNEO_DEBUG
	if (!DebugSnd_YM2151Initted) bprintf(PRINT_ERROR, _T("BurnYM2151Write called without init\n"));
#endif

	if (offset & 1) {
#ifdef REI_YM_ME
		if (rei_ymm_on) {
			const INT32 reg = nBurnCurrentYM2151Register[chip] & 0xff;
			if (reg == 0x14 && (nData & 0x80)) { // CSM: main from now on
				rei_ymm_takeover();
			} else {
				BurnYM2151UpdateRequest(); // (synthesis request to the ME up to this point)
				if (!(reg == 0x10 || reg == 0x11 || reg == 0x12 || reg == 0x14))
					rei_ymm_put(REI_YMM_WRITE, 0, 0, (UINT8)reg, nData);
				YM2151WriteReg(chip, reg, nData);
				return;
			}
		}
#endif
#ifdef REI_YM_THREAD
		if (rei_ymt_on) {
			const INT32 reg = nBurnCurrentYM2151Register[chip] & 0xff;
			if (reg == 0x14 && (nData & 0x80)) { // CSM: main from now on (drain the worker and stop using it)
				rei_ymt_drain();
				rei_ymt_on = 0;
			} else if (reg == 0x10 || reg == 0x11 || reg == 0x12 || reg == 0x14) {
				// Timers: main writes directly (no overlap with synth state). Synthesis is still split once at this position as before -
				// the split point changes synthesis results (silent-channel skipping etc., measured)
				rei_ymt_put(2, BurnYM2151StreamCallback(nBurnYM2151SoundRate), 0, 0, 0);
				YM2151WriteReg(chip, reg, nData);
				return;
			} else {
				rei_ymt_put(0, BurnYM2151StreamCallback(nBurnYM2151SoundRate), (UINT8)chip, (UINT8)reg, nData);
				return;
			}
		}
#endif
#ifdef REI_YM_LAZY
		const INT32 reg = nBurnCurrentYM2151Register[chip] & 0xff;
		if (reg == 0x14 && (nData & 0x80)) rei_ym_csm = 1;
		if (!rei_ym_csm && !rei_ym_immediate(reg)) {
			if (rei_ym_w >= REI_YM_LOG) rei_ym_flush();
			struct rei_ym_ev *e = &rei_ym_log[rei_ym_w++];
			e->pos = bBurnYM2151IsBuffered ? BurnYM2151StreamCallback(nBurnYM2151SoundRate) : -1;
			e->chip = (UINT8)chip;
			e->reg = (UINT8)reg;
			e->val = nData;
			return;
		}
		rei_ym_flush();
#endif
		BurnYM2151UpdateRequest();
#ifdef REI_YM_SPLIT
		{
			const INT32 r = nBurnCurrentYM2151Register[chip] & 0xff;
			if (rei_ym_split_on && !(r == 0x10 || r == 0x11 || r == 0x12 || r == 0x14))
				rei_ymf_put(REI_YMF_WRITE, nYM2151Position, (UINT8)r, nData);
		}
#endif
		YM2151WriteReg(chip, nBurnCurrentYM2151Register[chip], nData);
	} else {
		nBurnCurrentYM2151Register[chip] = nData;
	}
}

void BurnYM2151Write(INT32 offset, const UINT8 nData)
{
	BurnYM2151Write(0, offset, nData);
}

void BurnYM2151SelectRegister(const UINT8 nRegister)
{
	BurnYM2151Write(0, 0, nRegister);
}

void BurnYM2151WriteRegister(const UINT8 nValue)
{
	BurnYM2151Write(0, 1, nValue);
}

UINT8 BurnYM2151Read(INT32 chip)
{
	//  status contains: chip busy status (not emulated) and timer state.
	//  when using BurnTimer, this timer state is asynchronous to rendering
	//  state; therefore, updaterequest here is not needed.  Most games poll
	//  this status 800x+ per frame!
	//	BurnYM2151UpdateRequest();
	return YM2151ReadStatus(chip);
}

UINT8 BurnYM2151Read()
{
	return BurnYM2151Read(0);
}

