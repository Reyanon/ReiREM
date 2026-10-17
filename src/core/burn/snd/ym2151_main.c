// ReiREM PSP port: the main CPU's share of a channel-split YM2151 (burn_ym2151.cpp, REI_YM_SPLIT).
//
// ym2151.c is compiled a second time here with its public functions renamed, so this copy has its own
// chip state and tables in main-CPU memory. The sound side (the Media Engine) forwards every register
// write it applies, with its sample position, through an uncached ring (rei_ymf_*); rei_ym_main_poll()
// replays them, synthesizing this copy's channels into an uncached partial buffer that the sound side
// adds to its own at the end of the frame (YM2151Combine). Timer registers are not forwarded: this copy
// never drives timers (its timer callback does nothing).
//
// REI_YM_ME (other boards on the PSP) uses the same copy the other way round: the Media Engine synthesizes with it
// (burn_ym2151.cpp), the main CPU's chip gets the register writes and drives the timers.
#if defined REI_YM_SPLIT || defined REI_YM_ME

#define ym2151_timer_over       nbbm_ym2151_timer_over
#define YM2151WriteReg          nbbm_YM2151WriteReg
#define YM2151ReadStatus        nbbm_YM2151ReadStatus
#define BurnYM2151Scan_int      nbbm_BurnYM2151Scan_int
#define YM2151SetTimerInterleave nbbm_YM2151SetTimerInterleave
#define YM2151Init              nbbm_YM2151Init
#define YM2151Shutdown          nbbm_YM2151Shutdown
#define YM2151ResetChip         nbbm_YM2151ResetChip
#define YM2151UpdateOne         nbbm_YM2151UpdateOne
#define YM2151SetIrqHandler     nbbm_YM2151SetIrqHandler
#define YM2151SetPortWriteHandler nbbm_YM2151SetPortWriteHandler
#define YM2151SetChannelMask    nbbm_YM2151SetChannelMask
#define YM2151SetPartialOut     nbbm_YM2151SetPartialOut
#define YM2151Combine           nbbm_YM2151Combine
#define YM2151StateSize         nbbm_YM2151StateSize
#define YM2151TimersOnly        nbbm_YM2151TimersOnly
#define YM2151StateGet          nbbm_YM2151StateGet
#define YM2151StateSet          nbbm_YM2151StateSet
#define YM2151CopyChannels      nbbm_YM2151CopyChannels

#include "ym2151.c"

#include "rei_me.h"
#include "rei_ym_split.h"

static void nbbm_timer_cb(INT32 n, INT32 c, double period)
{
	(void)n;
	(void)c;
	(void)period;
}

#ifdef REI_YM_ME
// the Media Engine's copy (REI_YM_ME): made on the main CPU before the ME starts
int rei_ym_copy_init(int clock, int rate)
{
	return nbbm_YM2151Init(1, 0, clock, rate, nbbm_timer_cb);
}
#endif

#ifdef REI_YM_SPLIT
static INT32 nbbm_pos;          // samples of this frame already synthesized by this copy
static UINT32 nbbm_r;           // ring read position (also published, uncached)
static INT16 nbbm_dummy[2][4];  // UpdateOne wants output pointers even in partial mode

int rei_ym_main_init(int clock, int rate, UINT8 mask)
{
	if (nbbm_YM2151Init(1, 0, clock, rate, nbbm_timer_cb))
		return 1;
	nbbm_YM2151SetChannelMask(mask);
	nbbm_YM2151ResetChip(0);
	nbbm_pos = 0;
	nbbm_r = 0;
	return 0;
}

void rei_ym_main_exit(void)
{
	nbbm_YM2151Shutdown();
}

// After loading a save state (sound side stopped, main CPU): sync this copy to the sound side's chip state (state, YM2151StateGet),
// drop unread writes (from before the load), and align the synthesis position to the sound side's buffer position (pos)
void rei_ym_main_resync(const void *state, int pos)
{
	nbbm_YM2151StateSet(0, state, 1);
	nbbm_r = REI_YMF_LOAD(rei_ymf_w);
	REI_YMF_STORE(rei_ymf_r, nbbm_r);
	nbbm_pos = pos;
}

static void nbbm_render_to(INT32 pos)
{
	if (pos <= nbbm_pos)
		return;
	INT32 *pl = (INT32 *)REI_UNCACHED(&rei_ymf_pb[0][0]);
	INT32 *pr = (INT32 *)REI_UNCACHED(&rei_ymf_pb[1][0]);
	INT16 *bufs[2] = { nbbm_dummy[0], nbbm_dummy[1] };
	nbbm_YM2151SetPartialOut(pl + 4 + nbbm_pos, pr + 4 + nbbm_pos);
	nbbm_YM2151UpdateOne(0, bufs, pos - nbbm_pos);
	nbbm_pos = pos;
}

// main CPU: replay what the sound side forwarded. Returns 1 if it did anything
int rei_ym_main_poll(void)
{
	UINT32 w = REI_YMF_LOAD(rei_ymf_w);
	if (nbbm_r == w)
		return 0;
	while (nbbm_r != w) {
		const volatile struct rei_ymf_ev *e = (const volatile struct rei_ymf_ev *)REI_UNCACHED(&rei_ymf_ring[nbbm_r & (REI_YMF_RING - 1)]);
		const INT32 pos = e->pos;
		switch (e->type) {
		case REI_YMF_WRITE:
			nbbm_render_to(pos);
			nbbm_YM2151WriteReg(0, e->reg, e->val);
			break;
		case REI_YMF_END:
			nbbm_render_to(pos);
			REI_YMF_STORE(rei_ymf_done, (UINT32)e->val | ((UINT32)e->reg << 8)); // frame tag
			break;
		case REI_YMF_START:
			nbbm_pos = pos;
			break;
		case REI_YMF_RESET:
			nbbm_YM2151ResetChip(0);
			nbbm_pos = pos;
			break;
		}
		nbbm_r++;
	}
	REI_YMF_STORE(rei_ymf_r, nbbm_r);
	return 1;
}

#endif
#endif
