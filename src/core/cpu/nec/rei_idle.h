// ReiREM Vita port: NEC wait-loop skipping (idle skip)
//
// When a game spins in a wait loop that "checks a memory flag and branches back if equal",
// and the per-iteration cycle cost d is the same twice in a row, treat the state as periodic and
// consume k*d cycles at once. icount stays >= 1 after the skip, so icount > 0 held at every instruction boundary of the
// skipped iterations, and the result (registers, cycles, interrupt timing) matches the original interpreter.
// (originally left 2 iterations, more than needed. V35 with short slices spun 4 loops per line — measured on PSP)
// When an interrupt arrives the caller resets mark to 0 to restart measurement.
//
// Also measures iterations spanning slice boundaries (one VezRun) (PSP: re-measuring one loop per slice cost ~20% of V35 execution).
// Cycles run after the last visit at the end of the previous slice (tail) plus cycles to the first visit in this slice = one boundary-spanning iteration.
// An interrupt in between discards tail. This assumes that anything changing between slices is signaled only by interrupts:
// this game's two wait loops (V33 0x5a4, V35 0x8e2) watch memory bytes written only by interrupt handlers.
#ifndef REI_IDLE_H
#define REI_IDLE_H

#define REI_IDLE_MAX_ITER 64 // longer iterations are not treated as wait loops

typedef struct {
	unsigned pc;  // linear address of the loop's first instruction (0 = unused)
	int mark;     // icount at previous visit
	int iter;     // last measured iteration cost
	unsigned long long visits;  // stats: times the loop's first instruction was reached
	unsigned long long skipped; // stats: total skipped cycles
	int last_d;   // stats: last measured d (diagnostics)
	int tail;     // cycles run after the last visit in the previous slice (-1 = none)
	int start;    // icount at the start of this slice
} rei_idle_t;

// slice start/end (beginning and end of VezRun)
static inline void rei_idle_begin(rei_idle_t *s, int icount)
{
	s->mark = 0;
	s->start = icount;
}
static inline void rei_idle_end(rei_idle_t *s, int icount)
{
	s->tail = (s->mark > 0) ? s->mark - icount : -1;
}
// restart measurement (interrupts etc.)
static inline void rei_idle_restart(rei_idle_t *s)
{
	s->mark = 0;
	s->tail = -1;
}

// Called when the loop's first instruction is reached. limit = max cycles that may be skipped now.
// Returns the number of cycles skipped.
static inline int rei_idle_visit(rei_idle_t *s, int *icount, int limit)
{
	int now = *icount;
	int skipped = 0;
	s->visits++;
	int d = -1;
	if (s->mark > 0)
		d = s->mark - now;
	else if (s->tail >= 0)
		d = s->tail + (s->start - now); // one iteration spanning the slice boundary
	s->tail = -1;
	if (d >= 0) {
		s->last_d = d;
		if (d > 0 && d <= REI_IDLE_MAX_ITER && d == s->iter) {
			int avail = now - 1;
			if (avail > limit)
				avail = limit;
			if (avail > 0) {
				int k = avail / d;
				skipped = k * d;
				*icount -= skipped;
				now = *icount;
				s->skipped += (unsigned long long)skipped;
			}
		}
		s->iter = d;
	}
	s->mark = now;
	return skipped;
}

#endif
