// Timers (for Yamaha FM chips and generic)
#include "burnint.h"
#include "timer.h"

#define MAX_TIMER_VALUE ((1 << 30) - 65536)
// (ReiREM) Cache tick/cycle conversion results — same input, same value (identical results). On MIPS32 (PSP) 64-bit division is a library
// call, and this conversion, redone for the same boundary every slice, ran ~3000 times per frame in the M72-family Z80 step (hharry 1.2M cycles)
#ifndef REI_TIMER_NOMEMO
#ifdef REI_TIMER_COUNT // (diag) actual division count
extern "C" { unsigned long long rei_timer_conv[2]; }
#define REI_TM_COUNT(i) (rei_timer_conv[i]++)
#else
#define REI_TM_COUNT(i) ((void)0)
#endif
#define REI_TM_N 2048
struct rei_tm_ent { INT64 n; INT64 v; INT32 m; };
static struct rei_tm_ent rei_tm_ticks[REI_TM_N], rei_tm_cyc[REI_TM_N]; // (m == 0: empty)
static inline UINT32 rei_tm_k(INT64 n, INT64 m) { const UINT32 x = (UINT32)n ^ (UINT32)(n >> 32) * 0x9e3779b1u ^ (UINT32)m; return (x ^ (x >> 9)) & (REI_TM_N - 1); }
static inline INT64 rei_make_timer_ticks(INT64 n, INT64 m)
{
	struct rei_tm_ent *e = &rei_tm_ticks[rei_tm_k(n, m)];
	if (e->m == (INT32)m && e->n == n && m != 0) return e->v;
	REI_TM_COUNT(0);
	const INT64 v = n * TIMER_TICKS_PER_SECOND / m;
	e->n = n; e->m = (INT32)m; e->v = v;
	return v;
}
static inline INT64 rei_make_cpu_cycles(INT64 n, INT64 m)
{
	struct rei_tm_ent *e = &rei_tm_cyc[rei_tm_k(n, m)];
	if (e->m == (INT32)m && e->n == n && m != 0) return e->v;
	REI_TM_COUNT(1);
	const INT64 v = n * m / TIMER_TICKS_PER_SECOND;
	e->n = n; e->m = (INT32)m; e->v = v;
	return v;
}
#undef MAKE_TIMER_TICKS
#undef MAKE_CPU_CYLES
#define MAKE_TIMER_TICKS(n, m) rei_make_timer_ticks((INT64)(n), (INT64)(m))
#define MAKE_CPU_CYLES(n, m) rei_make_cpu_cycles((INT64)(n), (INT64)(m))
#endif

double dTime;									// Time elapsed since the emulated machine was started

static INT32 nIndex;

#define TIMER_MAX 8
static INT32 nTimerCount[TIMER_MAX], nTimerStart[TIMER_MAX];

static INT32 nTimerChips[TIMER_MAX];

// Callbacks
typedef INT32 (*tpTimerOverCallback)(INT32, INT32);
static tpTimerOverCallback pTimerOverCallback[TIMER_MAX];

static double (*pTimerTimeCallback)();

INT32 BurnTimerCPUClockspeed = 0;
INT32 (*BurnTimerCPUTotalCycles)() = NULL;

static INT32 (*pCPURun)(INT32) = NULL;
static void (*pCPURunEnd)() = NULL;

// ---------------------------------------------------------------------------
// Running time

static double BurnTimerTimeCallbackDummy()
{
	return 0.0;
}

extern "C" double BurnTimerGetTime()
{
	return dTime + pTimerTimeCallback();
}

// ---------------------------------------------------------------------------
// Update timers

static INT32 nTicksTotal, nTicksDone, nTicksExtra;

// ReiREM port: CPU cycle where BurnTimerUpdate would next cut for a timer (earliest timer), for d_m92.cpp sound
// spans, which only skip the per-line cuts where no timer is near
INT32 BurnTimerNextCycles()
{
	INT32 m = nTimerCount[0];
	for (INT32 i = 1; i < TIMER_MAX; i++)
		if (nTimerCount[i] < m) m = nTimerCount[i];
	const INT64 c = MAKE_CPU_CYLES((INT64)m + nTicksExtra, BurnTimerCPUClockspeed);
	return c > 0x7fffffff ? 0x7fffffff : (INT32)c;
}

INT32 BurnTimerUpdate(INT32 nCycles)
{
	INT32 nIRQStatus = 0;

	nTicksTotal = MAKE_TIMER_TICKS(nCycles, BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T(" -- Ticks: %08X, cycles %i\n"), nTicksTotal, nCycles);

	while (nTicksDone < nTicksTotal) {
		INT32 nTimer, nFirstTimer, nCyclesSegment, nTicksSegment;

		// Determine which timer fires first
		nFirstTimer = 0;
		for (INT32 i = 0; i < TIMER_MAX; i++) {
			if (nTimerCount[i] < nTimerCount[nFirstTimer]) {
				nFirstTimer = i;
			}
		}

		nTicksSegment = nTimerCount[nFirstTimer];

		if (nTicksSegment > nTicksTotal) {
			nTicksSegment = nTicksTotal;
		}

		nCyclesSegment = MAKE_CPU_CYLES(nTicksSegment + nTicksExtra, BurnTimerCPUClockspeed);
//		bprintf(PRINT_NORMAL, _T("  - Timer: %08X, %08X, %08X, cycles %i, %i\n"), nTicksDone, nTicksSegment, nTicksTotal, nCyclesSegment, BurnTimerCPUTotalCycles());

#ifdef REI_TIMER_TRACE
		{ extern int rei_trace_frame; const INT32 t0 = BurnTimerCPUTotalCycles(); pCPURun(nCyclesSegment - t0); fprintf(stderr, "run f%d %d->%d got %d\n", rei_trace_frame, t0, nCyclesSegment, BurnTimerCPUTotalCycles()); }
#else
		pCPURun(nCyclesSegment - BurnTimerCPUTotalCycles());
#endif

		nTicksDone = MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles() + 1, BurnTimerCPUClockspeed) - 1;
//		bprintf(PRINT_NORMAL, _T("  - ticks done -> %08X cycles -> %i\n"), nTicksDone, BurnTimerCPUTotalCycles());

		nTimer = 0;

		for (INT32 i = 0; i < TIMER_MAX; i++) {
			if (nTicksDone >= nTimerCount[i]) {
				if (nTimerStart[i] == MAX_TIMER_VALUE) {
					nTimerCount[i] = MAX_TIMER_VALUE;
				} else {
					nTimerCount[i] += nTimerStart[i];
				}
				//bprintf(PRINT_NORMAL, _T("  - timer %d fired\n"), i);

//				bprintf(0, _T("over_callback[%d](%d, %d)\n"), i>>1,nTimerChips[i>>1], i&1);
#ifdef REI_TIMER_TRACE
				{ extern int rei_trace_frame; fprintf(stderr, "fire f%d t%d T%d D%d\n", rei_trace_frame, i, BurnTimerCPUTotalCycles(), nTicksDone); }
#endif
				nIRQStatus |= pTimerOverCallback[i>>1](nTimerChips[i>>1], i&1);
			}
		}
	}

	return nIRQStatus;
}

// ReiREM port: the same as calling BurnTimerUpdate((k + 1) * nTotal / nInterleave) for k = k0..k1 (in order), but without
// stopping the CPU at the slice targets where nothing would happen. A stop at slice target t (run ends at T, the first
// instruction end >= t) only matters through the timer check after it: a timer whose count C is above ticks(t) (so its
// own cut is after t) fires there early when C <= ticks(T + 1) - 1. That needs the cut of C (about C * clock / ticks/s)
// within REI_TIMER_OV cycles after t; such targets are kept as stops. Counts change only at stops (fires re-arm, and
// setting a timer ends the run), so the next stop is chosen again after each one. Splitting a CPU run is otherwise
// invisible (the run continues from the same state; drivers using this keep their per-slice work on stops).
#ifndef REI_TIMER_OV
#define REI_TIMER_OV 128
#endif
#define REI_TIMER_OV_NOTE // (comfortably above the max cycles a CPU can overrun its target: Z80 interrupt + longest instruction)
INT32 (*BurnTimerSliceHold)() = NULL; // nonzero: the next CPU run is not split-invisible (ZetCyclesDelayedPending) — stop it at the slice target
void (*BurnTimerAfterStop)() = NULL; // called after each run's timer check (a driver finishes work that belongs after that stop, d_m72 m72b)
INT32 BurnTimerUpdateSlices(INT32 k0, INT32 k1, INT32 nInterleave, INT32 nTotal)
{
	INT32 nIRQStatus = 0;
	INT32 k = k0;
	for (;;) {
		while (k <= k1 && nTicksDone >= MAKE_TIMER_TICKS((k + 1) * nTotal / nInterleave, BurnTimerCPUClockspeed)) k++;
		if (k > k1) break;

		// cycle windows where a slice target would see an early fire: [cut - OV - 2, cut + 1] around each timer's cut
		INT32 lo[TIMER_MAX], hi[TIMER_MAX], n = 0;
		for (INT32 i = 0; i < TIMER_MAX; i++) {
			if (nTimerCount[i] >= MAX_TIMER_VALUE) continue;
			const INT32 f = (INT32)MAKE_CPU_CYLES(nTimerCount[i], BurnTimerCPUClockspeed);
			lo[n] = f - REI_TIMER_OV - 2;
			hi[n++] = f + 1;
		}
		INT32 kk = k;
		if (BurnTimerSliceHold && BurnTimerSliceHold()) n = -1; // (skip the loop below)
#if defined REI_TIMER_NOMERGE
		for (; kk < k1 && 0; kk++) {
#elif defined REI_TIMER_ONLYIDLE
		for (; kk < k1 && n == 0; kk++) {
#elif 0
		for (; kk < k1 && 0; kk++) {
#else
		for (; kk < k1 && n >= 0; kk++) {
#endif
			const INT32 t = (kk + 1) * nTotal / nInterleave;
			INT32 risky = 0;
			for (INT32 i = 0; i < n; i++) if (t >= lo[i] && t <= hi[i]) { risky = 1; break; }
			if (risky) break;
		}
		nTicksTotal = MAKE_TIMER_TICKS((kk + 1) * nTotal / nInterleave, BurnTimerCPUClockspeed);

		// one pass of the BurnTimerUpdate loop body
		INT32 nFirstTimer = 0;
		for (INT32 i = 0; i < TIMER_MAX; i++) {
			if (nTimerCount[i] < nTimerCount[nFirstTimer]) {
				nFirstTimer = i;
			}
		}
		INT32 nTicksSegment = nTimerCount[nFirstTimer];
		if (nTicksSegment > nTicksTotal) {
			nTicksSegment = nTicksTotal;
		}
		const INT32 nCyclesSegment = MAKE_CPU_CYLES(nTicksSegment + nTicksExtra, BurnTimerCPUClockspeed);
#ifdef REI_TIMER_TRACE
		{ extern int rei_trace_frame; const INT32 t0 = BurnTimerCPUTotalCycles(); pCPURun(nCyclesSegment - t0); fprintf(stderr, "run f%d %d->%d got %d\n", rei_trace_frame, t0, nCyclesSegment, BurnTimerCPUTotalCycles()); }
#else
		pCPURun(nCyclesSegment - BurnTimerCPUTotalCycles());
#endif
		nTicksDone = MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles() + 1, BurnTimerCPUClockspeed) - 1;
		for (INT32 i = 0; i < TIMER_MAX; i++) {
			if (nTicksDone >= nTimerCount[i]) {
				if (nTimerStart[i] == MAX_TIMER_VALUE) {
					nTimerCount[i] = MAX_TIMER_VALUE;
				} else {
					nTimerCount[i] += nTimerStart[i];
				}
#ifdef REI_TIMER_TRACE
				{ extern int rei_trace_frame; fprintf(stderr, "fire f%d t%d T%d D%d\n", rei_trace_frame, i, BurnTimerCPUTotalCycles(), nTicksDone); }
#endif
				nIRQStatus |= pTimerOverCallback[i>>1](nTimerChips[i>>1], i&1);
			}
		}
		if (BurnTimerAfterStop) BurnTimerAfterStop();
	}
	nTicksTotal = MAKE_TIMER_TICKS((k1 + 1) * nTotal / nInterleave, BurnTimerCPUClockspeed);

	return nIRQStatus;
}

void BurnTimerEndFrame(INT32 nCycles)
{
	INT32 nTicks = MAKE_TIMER_TICKS(nCycles, BurnTimerCPUClockspeed);

	BurnTimerUpdate(nCycles);

	for (INT32 i = 0; i < TIMER_MAX; i++) {
		if (nTimerCount[i] < MAX_TIMER_VALUE) {
			nTimerCount[i] -= nTicks;
		}
	}

	nTicksDone -= nTicks;
	if (nTicksDone < 0) {
//		bprintf(PRINT_ERROR, _T(" -- ticks done -> %08X\n"), nTicksDone);
		nTicksDone = 0;
	}
}

void BurnTimerUpdateEnd()
{
//	bprintf(PRINT_NORMAL, _T("  - end %i\n"), BurnTimerCPUTotalCycles());

	pCPURunEnd();

	nTicksTotal = 0;
}


// ---------------------------------------------------------------------------
// Callbacks for the sound cores
/*
static INT32 BurnTimerExtraCallbackDummy()
{
	return 0;
}
*/
void BurnOPLTimerCallback(INT32 n, INT32 c, double period)
{
	pCPURunEnd();

	if (period == 0.0) {
		nTimerCount[(n << 1) + c] = MAX_TIMER_VALUE;
//		bprintf(PRINT_NORMAL, _T("  - timer %i stopped\n"), c);
		return;
	}

	nTimerCount[(n << 1) + c]  = (INT32)(period * (double)TIMER_TICKS_PER_SECOND);
	nTimerCount[(n << 1) + c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T("  - timer %i started, %08X ticks (fires in %lf seconds)\n"), c, nTimerCount[c], period);
}

void BurnOPMTimerCallback(INT32 n, INT32 c, double period) // ym2151
{
	pCPURunEnd();
#ifdef REI_TIMER_TRACE
	{ extern int rei_trace_frame; fprintf(stderr, "set f%d c%d T%d p%g\n", rei_trace_frame, c, BurnTimerCPUTotalCycles(), period); }
#endif
	
	if (period == 0.0) {
		nTimerCount[(n << 1) + c] = MAX_TIMER_VALUE;
		return;
	}

	nTimerCount[(n << 1) + c]  = (INT32)(period * (double)TIMER_TICKS_PER_SECOND);
	nTimerCount[(n << 1) + c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);
}

void BurnOPNTimerCallback(INT32 n, INT32 c, INT32 cnt, double stepTime) // ym2203, ym2610, ym2612, ym2608
{
	pCPURunEnd();
	
	if (cnt == 0) {
		nTimerCount[(n << 1) + c] = MAX_TIMER_VALUE;

//		bprintf(PRINT_NORMAL, _T("  - timer %i stopped\n"), c);

		return;
	}

	nTimerCount[(n << 1) + c]  = (INT32)(stepTime * cnt * (double)TIMER_TICKS_PER_SECOND);
	nTimerCount[(n << 1) + c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);
	//bprintf(PRINT_NORMAL, _T("  - chip %i timer %i started, %08X ticks (fires in %lf seconds)\n"), n, c, nTimerCount[(n << 1) + c], stepTime * cnt);
}

void BurnYMFTimerCallback(INT32 /* n */, INT32 c, double period)
{
	pCPURunEnd();

	if (period == 0.0) {
		nTimerStart[c] = nTimerCount[c] = MAX_TIMER_VALUE;

//		bprintf(PRINT_NORMAL, _T("  - timer %i stopped\n"), c);

		return;
	}

	nTimerStart[c]  = nTimerCount[c] = (INT32)(period * (double)(TIMER_TICKS_PER_SECOND / 1000000));
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T("  - timer %i started, %08X ticks (fires in %lf seconds)\n"), c, nTimerCount[c], period);
}

void BurnYMF262TimerCallback(INT32 /* n */, INT32 c, double period)
{
	pCPURunEnd();

	if (period == 0.0) {
		nTimerCount[c] = MAX_TIMER_VALUE;
		return;
	}

	nTimerCount[c]  = (INT32)(period * (double)TIMER_TICKS_PER_SECOND);
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T("  - timer %i started, %08X ticks (fires in %lf seconds)\n"), c, nTimerCount[c], period);
}

void BurnTimerSetRetrig(INT32 c, double period)
{
	pCPURunEnd();

	if (period == 0.0) {
		nTimerStart[c] = nTimerCount[c] = MAX_TIMER_VALUE;

//		bprintf(PRINT_NORMAL, _T("  - timer %i stopped\n"), c);

		return;
	}

	nTimerStart[c]  = nTimerCount[c] = (INT32)(period * (double)(TIMER_TICKS_PER_SECOND));
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T("  - timer %i started, %08X ticks (fires in %lf seconds)\n"), c, nTimerCount[c], period);
}

void BurnTimerSetOneshot(INT32 c, double period)
{
	pCPURunEnd();

	if (period == 0.0) {
		nTimerStart[c] = nTimerCount[c] = MAX_TIMER_VALUE;

//		bprintf(PRINT_NORMAL, _T("  - timer %i stopped\n"), c);

		return;
	}

	nTimerCount[c]  = (INT32)(period * (double)(TIMER_TICKS_PER_SECOND));
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

//	bprintf(PRINT_NORMAL, _T("  - timer %i started, %08X ticks (fires in %lf seconds)\n"), c, nTimerCount[c], period / 1000000.0);
}

void BurnTimerSetRetrig(INT32 c, UINT64 timer_ticks)
{
	pCPURunEnd();

	if (timer_ticks == 0) {
		nTimerStart[c] = nTimerCount[c] = MAX_TIMER_VALUE;

		// bprintf(PRINT_NORMAL, L"  - timer %i stopped\n", c);

		return;
	}

	nTimerStart[c] = nTimerCount[c] = (UINT32)(timer_ticks);
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

	// bprintf(PRINT_NORMAL, L"  - timer %i started, %08X ticks (fires in %lf seconds)\n", c, nTimerCount[c], (double)(TIMER_TICKS_PER_SECOND) / timer_ticks);
}

void BurnTimerSetOneshot(INT32 c, UINT64 timer_ticks)
{
	pCPURunEnd();

	if (timer_ticks == 0) {
		nTimerStart[c] = nTimerCount[c] = MAX_TIMER_VALUE;

		// bprintf(PRINT_NORMAL, L"  - timer %i stopped\n", c);

		return;
	}

	nTimerCount[c] = (UINT32)(timer_ticks);
	nTimerCount[c] += MAKE_TIMER_TICKS(BurnTimerCPUTotalCycles(), BurnTimerCPUClockspeed);

	// bprintf(PRINT_NORMAL, L"  - timer %i started, %08X ticks (fires in %lf seconds)\n", c, nTimerCount[c], (double)(TIMER_TICKS_PER_SECOND) / timer_ticks);
}

// ------------------------------------ ---------------------------------------
// Initialisation etc.

void BurnTimerScan(INT32 nAction, INT32* pnMin)
{
	if (pnMin && *pnMin < 0x029521) {
		*pnMin = 0x029521;
	}

	if (nAction & ACB_DRIVER_DATA) {
		SCAN_VAR(nTimerCount);
		SCAN_VAR(nTimerStart);
		SCAN_VAR(dTime);

		SCAN_VAR(nTicksDone);
	}
}

void BurnTimerExit()
{
	BurnTimerCPUClockspeed = 0;
	BurnTimerCPUTotalCycles = NULL;
	pCPURun = NULL;
	pCPURunEnd = NULL;

	pTimerTimeCallback = NULL;

	return;
}

void BurnTimerReset()
{
	for (INT32 i = 0; i < TIMER_MAX; i++) {
		nTimerCount[i] = nTimerStart[i] = MAX_TIMER_VALUE;
	}

	dTime = 0.0;

	nTicksDone = 0;
}

// inits in BurnDrvInit()
void BurnTimerPreInit()
{
	BurnTimerExit();

	nIndex = 0;
}

// initted by soundcore[s], (or anything)
// Each chip gets 2 timers

// BurnTimerSetOneshot / BurnTimerSetRetrig, use (baseIndex << 1) + timer#
// for the first param.  baseIndex is the return value from BurnTimerInit()
// timer# can be 0, 1

INT32 BurnTimerInit(INT32 (*pOverCallback)(INT32, INT32), double (*pTimeCallback)(), INT32 nChips)
{
	INT32 nChipBaseIndex = nIndex;
	bprintf(0, _T("BurnTimerInit: base index %d, #chips %d\n"), nIndex, nChips);

	if (nIndex + (nChips * 2) >= TIMER_MAX) {
		bprintf(PRINT_ERROR, _T("BurnTimer: Init overflows, increase TIMER_MAX?\n"));
		return 0;
	}

	for (INT32 chipnum = 0; chipnum < nChips; chipnum++) {
		pTimerOverCallback[nIndex] = pOverCallback;
		nTimerChips[nIndex] = chipnum; // base chip#
		//bprintf(0, _T("nTimerChips[%d] = %d\n"), nIndex, chipnum);
		nIndex++;
	}

	if (pTimerTimeCallback == NULL || pTimerTimeCallback == BurnTimerTimeCallbackDummy) {
		// Make sure we don't overwrite a good callback with NULL in subsequent core init's (some soundcores..)
		pTimerTimeCallback = pTimeCallback ? pTimeCallback : BurnTimerTimeCallbackDummy;
	}

	BurnTimerReset();

	return nChipBaseIndex;
}

// Null CPU, for a FM timer that isn't attached to anything.
static INT32 NullCyclesTotal;

void NullNewFrame()
{
	NullCyclesTotal = 0;
}

INT32 NullTotalCycles()
{
	return NullCyclesTotal;
}

INT32 NullRun(const INT32 nCycles)
{
	NullCyclesTotal += nCycles;

	return nCycles;
}

void NullRunEnd()
{
}

INT32 BurnTimerAttach(cpu_core_config *ptr, INT32 nClockspeed)
{
	BurnTimerCPUClockspeed = nClockspeed;
	BurnTimerCPUTotalCycles = ptr->totalcycles;
	pCPURun = ptr->run;
	pCPURunEnd = ptr->runend;

	nTicksExtra = MAKE_TIMER_TICKS(1, BurnTimerCPUClockspeed) - 1;

//	bprintf(PRINT_NORMAL, _T("--- timer cpu speed %iHz, one cycle = %i ticks.\n"), nClockspeed, MAKE_TIMER_TICKS(1, BurnTimerCPUClockspeed));

	return 0;
}

INT32 BurnTimerAttachNull(INT32 nClockspeed)
{
	BurnTimerCPUClockspeed = nClockspeed;
	BurnTimerCPUTotalCycles = NullTotalCycles;
	pCPURun = NullRun;
	pCPURunEnd = NullRunEnd;

	nTicksExtra = MAKE_TIMER_TICKS(1, BurnTimerCPUClockspeed) - 1;

//	bprintf(PRINT_NORMAL, _T("--- timer cpu speed %iHz, one cycle = %i ticks.\n"), nClockspeed, MAKE_TIMER_TICKS(1, BurnTimerCPUClockspeed));

	return 0;
}
