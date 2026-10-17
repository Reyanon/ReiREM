// V33 dynamic recompiler: block compilation and per-instruction MIPS code (included by nec_drc.inc, ADR-020)
//
// Generated code conventions
// - s0 = nec_state, s1 = icount, s2 = prefetch_count (value at instruction start), s3 = read page table, s4 = write page table,
//   s5 = icount at instruction start (rei_ins_ic for handlers), s6 = a handler was called in this instruction (exit check at end).
//   V33 registers, flags and ip live in memory (nec_state). Inside a block ip is a compile-time value, stored only on exit or handler calls.
// - Per-instruction temporaries are t0..t7 (preserved even if the slow path calls C), t8 = runtime-determined cycle sum of this instruction,
//   at, v0, v1, a0..a3 are short-lived scratch (not live across slow paths).
// - icount is subtracted at the same points as the interpreter handlers (handlers read VezTotalCycles). Prefetch at instruction end matches rei_prefetch1.
// - prefetch_reset is 0 at instruction start (dispatcher enters blocks only when it is 0, and end-of-instruction math always resets it to 0).

#define OFS(f) ((int)offsetof(nec_state_t, f))
#ifdef REI_DRC_V25
// (V35) Registers/segments are the current bank in internal RAM (ram is at the start of the state): use the compiled block's bank (drc_rbw, block key)
#define OFW(r) (2 * (drc_rbw + (r)))
#define OFB(r) (2 * drc_rbw + (r))
#define OFSR(s) (2 * (drc_rbw + (s)))
#else
#define OFW(r) (OFS(regs) + 2 * (r))
#define OFB(r) (OFS(regs) + (r))
#define OFSR(s) (OFS(sregs) + 2 * (s))
#endif

// CK(v30, v33): the value for the chip being compiled, from the interpreter handler's CLK macro (necinstr.c) (drc_v30, nec_drc.inc)
static inline int CK(int v30, int v33) { return drc_v30 ? v30 : v33; }

static const int drc_wreg[8] = { AW, CW, DW, BW, SP, BP, IX, IY };
static const int drc_breg[8] = { AL, CL, DL, BL, AH, CH, DH, BH };

#define FR_T(i) (16 + 4 * (i)) // t0..t8 save slots (slow path)
#define FR_S(i) (56 + 4 * (i)) // s0..s7
#define FR_V0 52
#define FR_RA 88
#define FR_HRA 92 // ra for the shared routines' slow path
#define FR_FP 96  // fp (= SS << 4)
static const int drc_saved_t[9] = { rT0, rT1, rT2, rT3, rT4, rT5, rT6, rT7, rT8 };
static inline int drc_tslot(int r) { return r == rT8 ? 8 : r - rT0; }


// ---------------------------------------------------------------------------
// Compile-time state

enum { DST_EXIT, DST_GEXIT, DST_LINK };
struct drc_stub {
	UINT8 kind;
	INT8 s2k;         // prefetch count to put in s2 on exit (-1 = register already correct)
	UINT32 *br;       // branches leading to this exit
	UINT32 *back;     // (CHK) return point when cycles remain
	UINT32 ip, pc;    // ip to store (0xffffffff = don't store), linear address to return
};
#define DRC_MAX_STUBS 512
static struct drc_stub drc_stubs[DRC_MAX_STUBS];
static int drc_nstubs;

static struct {
	int dconst;   // cycles subtracted in this instruction (constant part)
	int dreg;     // runtime-determined part is in t8
	int dalt;     // with dreg: dalt if t8 is one of {0, dalt}, -1 if arbitrary
	int mem;      // has memory access (slow path possible)
	int pf_done;  // instruction code already did the end-of-instruction prefetch (Jcc/LOOP: before the exit)
	UINT32 ip, ipn, pc; // instruction start ip, next ip, linear address
} dc;

// Is the prefetch count known at compile time (block key holds the entry value)? If so, s2 is not updated; it is set on exit
static int pf_set;  // possible prefetch counts at instruction start (bit set; at block entry, the single key value)
static int s2_ok;   // s2 holds the actual value (always true when the set has several members)
static inline int pf_single(void) { return (pf_set & (pf_set - 1)) == 0 ? __builtin_ctz(pf_set) : -1; }

static struct drc_stub *drc_new_stub(int kind)
{
	if (drc_nstubs >= DRC_MAX_STUBS) {
		drc_overflow = 1;
		return &drc_stubs[0];
	}
	struct drc_stub *s = &drc_stubs[drc_nstubs++];
	memset(s, 0, sizeof(*s));
	s->kind = (UINT8)kind;
	s->s2k = (INT8)((pf_single() >= 0 && !s2_ok) ? pf_single() : -1);
	return s;
}

// Merge if the previous instruction is addiu s1,s1,x with no branch target in between (order unchanged)
static void e_sub_s1(int c)
{
	const UINT32 hdr = (0x09u << 26) | ((UINT32)rS1 << 21) | ((UINT32)rS1 << 16);
	if (drc_ptr > drc_buf && drc_ptr != drc_nomerge && (drc_ptr[-1] & 0xffff0000u) == hdr && !drc_overflow) {
		const int v = (INT16)(drc_ptr[-1] & 0xffff) - c;
		if (v >= -32768 && v < 32768) {
			drc_ptr[-1] = hdr | ((UINT32)v & 0xffff);
			return;
		}
	}
	e_addiu(rS1, rS1, -c);
}

// Cycle subtraction (at the same points as the interpreter)
static void drc_clk(int c)
{
	if (c) {
		e_sub_s1(c);
		dc.dconst += c;
	}
}
static void drc_clk_reg(int r) // r = runtime-determined cycles (arbitrary)
{
	e_subu(rS1, rS1, r);
	if (!dc.dreg) {
		e_move(rT8, r);
		dc.dreg = 1;
		dc.dalt = -1;
	} else {
		e_addu(rT8, rT8, r);
		dc.dalt = -1;
	}
}
// Runtime cycles 0 or delta: delta if rcond != 0 (once per instruction)
static void drc_clk_alt(int rcond, int delta)
{
	e_li(rV1, (UINT32)delta);
	e_movz(rV1, rZERO, rcond);
	e_subu(rS1, rS1, rV1);
	if (!dc.dreg) {
		e_move(rT8, rV1);
		dc.dreg = 1;
		dc.dalt = delta;
	} else {
		e_addu(rT8, rT8, rV1);
		dc.dalt = -1;
	}
}
// By address parity (memory side of CLKW/CLKR): odd / even cycles, decided by bit 0 of register rparity
static void drc_clk_parity(int rparity, int odd, int even)
{
	drc_clk(even);
	if (odd != even) {
		const int delta = odd - even;
		e_andi(rAT, rparity, 1);
		if ((delta & (delta - 1)) == 0 && !dc.dreg) { // v1 = bit << log2(delta)
			int sh = 0;
			while ((1 << sh) < delta) sh++;
			if (sh) e_sll(rV1, rAT, sh); else e_move(rV1, rAT);
			e_subu(rS1, rS1, rV1);
			e_move(rT8, rV1);
			dc.dreg = 1;
			dc.dalt = delta;
		} else
			drc_clk_alt(rAT, delta);
	}
}

// ---------------------------------------------------------------------------
// Shared routines (built once at the start of the code cache, called by blocks via jal)

static UINT32 *H_pro, *H_epi, *H_exit_ip, *H_exit_link, *H_iexit, *H_jc, *H_rb, *H_rw, *H_wb, *H_ww, *H_in[2], *H_out[2];
// Memory routine variants (NEC): address in t5/t6, data in t0/t1 kept in registers. Call site is jal + delay slot, 2 words (saves the
// 3 words moving address/value to a0/a1 and receiving v0). Smaller generated code means fewer icache misses (about 1/3 of dsoccr94 block time,
// PSP hardware counters 2026-10-17). [width][address t5/t6][data t0/t1]
static UINT32 *H_rv[2][2][2], *H_wv[2][2][2], *H_rs[2][2], *H_rslow[2], *H_wslow[2];
#ifndef DRC_MEMV
#define DRC_MEMV 0 // (1: use variants. Block code shrank 7% but the shared routines grew and icache misses rose - dsoccr94 PSP 2026-10-17)
#endif
static UINT32 *H_add[2], *H_adc[2], *H_sub[2], *H_sbb[2], *H_log[2], *H_inc[2], *H_dec[2], *H_szp[2];

// Lazy flags (NEC instance only, nec_state lz_k/lz_a/lz_b): ADD/SUB/logic/INC/DEC write only CarryVal immediately and record which
// shared routine (H_add etc.) computes O/A/S/Z/P plus its inputs (t0/t1). Evaluation (H_mat) happens before instructions that read or write those flags, before
// interpreter steps, and on block exit (H_epi). Linked exits enter the next block with flags still pending (next block starts in the unknown state).
// H_mat preserves CarryVal (CLC/STC/CMC may have changed only CarryVal in between - the value written directly and the one the routine rewrites
// are the same). Conditional branches skip evaluation when they can decide from the recorded inputs (drc_cond)
#ifndef REI_DRC_V25
#define DRC_LAZY 1
#endif
// lz_k = ((kind*2 + width + 1) << 8) | pending groups. Groups are O, A, S/Z/P - instructions that redefine only some (shifts, TEST imm, NEG)
// drop just those groups from the pending set (drc_lz_drop, even from the unknown state at block start). Eval routines exist per group set (all / O+A / A)
enum { LZB_SZP = 1, LZB_A = 2, LZB_O = 4, LZB_ALL = 7 };
enum { LZK_ADD, LZK_SUB, LZK_LOG, LZK_INC, LZK_DEC };
// lz_mask: groups possibly pending (0 = none, all at block start), lz_known: set by an earlier instruction in this block (lz_kind/lz_w/lz_plain valid),
// lz_plain: SUB not SBB (for signed compares), lz_bad: touched possibly-pending flags directly (misclassified)
static int lz_mask, lz_known, lz_kind, lz_w, lz_plain, lz_bad;
static UINT32 *H_mat, *drc_hbase;
// Shared routines that leave lazy flags (t0 = dst, t1 = src -> t0 = result, CarryVal, lz_a/lz_b/lz_k): call site is 2 words -
// inlining into block code ran fewer instructions but the larger code lost in icache and code cache (dsoccr94, 2026-10-17)
static UINT32 *H_lzadd[2], *H_lzadc[2], *H_lzsub[2], *H_lzsbb[2], *H_lzlog[2], *H_lzinc[2], *H_lzdec[2];
static UINT32 drc_lz_tab[11 * 8]; // (lz_k >> 8) * 8 + groups -> eval routine
#if !defined(__psp__)
static int drc_prof_cur = -1, drc_cur_op; // (diag REI_DRC_PROF: block/instruction being built - counts eval sites)
static void drc_prof_flush_note(int why);
#endif
static inline void drc_lz_direct(int bits) // code that reads or writes those groups directly: they must not be pending (drc_lz_flush before the instruction)
{
	if (lz_mask & bits) lz_bad = 1;
}

static inline void e_jal(const UINT32 *t) { E((0x03u << 26) | (((UINT32)(uintptr_t)t >> 2) & 0x3ffffff)); }
static inline int drc_is_branch(UINT32 w)
{
	const UINT32 op = w >> 26;
	return op == 1 || (op >= 2 && op <= 7) || (op == 0 && ((w & 63) == 8 || (w & 63) == 9));
}
static UINT32 *drc_blk_start; // start of the block being built (earlier block code is left alone)
// jal + delay slot: move the previous instruction into the delay slot if possible (not a branch, no ra use, not in a delay slot, not a branch target)
static void e_jal_ds(const UINT32 *t)
{
	if (!drc_ds_slot && !drc_overflow && drc_ptr - 2 >= drc_blk_start && drc_ptr != drc_nomerge) {
		const UINT32 w = drc_ptr[-1];
		const int ra_use = ((w >> 21) & 31) == 31 || ((w >> 16) & 31) == 31 || ((w >> 11) & 31) == 31;
		if (w != 0 && !drc_is_branch(w) && !drc_is_branch(drc_ptr[-2]) && !ra_use) {
			drc_ptr[-1] = (0x03u << 26) | (((UINT32)(uintptr_t)t >> 2) & 0x3ffffff);
			E(w);
			return;
		}
	}
	e_jal(t);
	e_nop();
}
static inline void e_j(const UINT32 *t) { E((0x02u << 26) | (((UINT32)(uintptr_t)t >> 2) & 0x3ffffff)); }

static void drc_mask(int rd, int rs) { e_ext(rd, rs, 0, 20); }

#if !defined(__psp__)
// (diag REI_DRC_SLOWSTAT: slow-path counts per 4KB page, reads/writes - top 8 each time nCurrentFrame changes)
static void drc_slowstat(UINT32 a, int wr)
{
	static int on = -1;
	static unsigned long long n[2][512];
	static int last;
	if (on < 0) { // (value 35 = V35 instances only, anything else = NEC instances only)
		const char *e = getenv("REI_DRC_SLOWSTAT");
#ifdef REI_DRC_V25
		on = e && atoi(e) == 35;
#else
		on = e && atoi(e) != 35;
#endif
	}
	if (!on) return;
	n[wr][(a >> 12) & 0x1ff]++; // (0x100 and above = port + 0x100)
	if (++last % 20000 == 0) {
		(void)0;
		fprintf(stderr, "slowstat n%d:", last);
		for (int w = 0; w < 2; w++)
			for (int k = 0; k < 512; k++)
				if (n[w][k]) fprintf(stderr, k >= 256 ? " %sport%02x %llu" : " %s%02x000 %llu", w ? "W" : "R", k & 0xff, n[w][k]);
		fprintf(stderr, "\n");
		memset(n, 0, sizeof(n));
	}
}
#else
#define drc_slowstat(a, w) ((void)0)
#endif
// Slow-path helpers (same as the interpreter's read_mem_* / write_mem_*)
#ifdef REI_DRC_V25
// (V35: interpreter read_mem_* / write_mem_* that see internal RAM/SFR - the fast-path page tables leave those pages empty)
static UINT32 drc_h_rb(UINT32 a) { drc_slowstat(a, 0); return v25_read_byte(sChipsPtr, a & 0xfffff); }
static UINT32 drc_h_rw(UINT32 a) { drc_slowstat(a, 0); return v25_read_word(sChipsPtr, a & 0xfffff); }
static void drc_h_wb(UINT32 a, UINT32 v) { drc_slowstat(a, 1); v25_write_byte(sChipsPtr, a & 0xfffff, (UINT8)v); }
static void drc_h_ww(UINT32 a, UINT32 v) { drc_slowstat(a, 1); v25_write_word(sChipsPtr, a & 0xfffff, (UINT16)v); }
#else
// (profile build: slow-path handler time sampled 1/16 - display names z80 = memory read, m6800 = memory write, i8039 = port)
static UINT32 drc_h_rb(UINT32 a) { drc_slowstat(a, 0); REI_PROF_CPU_BEGIN(hr); const UINT32 r = rei_nec_rd(a); REI_PROF_CPU_END(hr, REI_PROF_Z80); return r; }
static UINT32 drc_h_rw(UINT32 a)
{
	drc_slowstat(a, 0);
	REI_PROF_CPU_BEGIN(hr);
	const UINT32 lo = rei_nec_rd(a), r = lo | ((UINT32)rei_nec_rd(a + 1) << 8);
	REI_PROF_CPU_END(hr, REI_PROF_Z80);
	return r;
}
static void drc_h_wb(UINT32 a, UINT32 v) { drc_slowstat(a, 1); REI_PROF_CPU_BEGIN(hw); rei_nec_wr(a, (UINT8)v); REI_PROF_CPU_END(hw, REI_PROF_M6800); }
static void drc_h_ww(UINT32 a, UINT32 v)
{
	drc_slowstat(a, 1);
	REI_PROF_CPU_BEGIN(hw);
	rei_nec_wr(a, (UINT8)v); rei_nec_wr(a + 1, (UINT8)(v >> 8));
	REI_PROF_CPU_END(hw, REI_PROF_M6800);
}
#endif
// Ports (same as the interpreter's read_port_* / write_port_*)
static UINT32 drc_h_in8(UINT32 a) { drc_slowstat(0x100000 | (a & 0xff) << 12, 0); REI_PROF_CPU_BEGIN(hp); const UINT32 r = read_port_byte(a); REI_PROF_CPU_END(hp, REI_PROF_I8039); return r; }
static UINT32 drc_h_in16(UINT32 a) { drc_slowstat(0x100000 | (a & 0xff) << 12, 0); REI_PROF_CPU_BEGIN(hp); const UINT32 r = (UINT16)read_port_word(a); REI_PROF_CPU_END(hp, REI_PROF_I8039); return r; }
static void drc_h_out8(UINT32 a, UINT32 v) { drc_slowstat(0x100000 | (a & 0xff) << 12, 1); REI_PROF_CPU_BEGIN(hp); write_port_byte(a, (UINT8)v); REI_PROF_CPU_END(hp, REI_PROF_I8039); }
static void drc_h_out16(UINT32 a, UINT32 v) { drc_slowstat(0x100000 | (a & 0xff) << 12, 1); REI_PROF_CPU_BEGIN(hp); write_port_word(a, (unsigned short)v); REI_PROF_CPU_END(hp, REI_PROF_I8039); }
// End-of-instruction icount exit table: bltzal return address (ra) -> ip | length << 16 | (prefetch count + 1) << 20 (0 = keep s2).
// Appended sequentially in the code cache, so sorted by ra (binary search)
static UINT32 drc_iexit(nec_state_t *s, UINT32 ra)
{
	const int seg = (drc_nsegs > 1 && ra >= (UINT32)(uintptr_t)drc_segs[1].code_start &&
			 ra < (UINT32)(uintptr_t)drc_segs[1].code_end) ? 1 : 0;
	const UINT32 *xra = seg == drc_cur ? drc_xt_ra : drc_segs[seg].xt_ra;
	const UINT32 *xinfo = seg == drc_cur ? drc_xt_info : drc_segs[seg].xt_info;
	int lo = 0, hi = (seg == drc_cur ? drc_xt_n : drc_segs[seg].xt_n) - 1;
	while (lo < hi) {
		const int mid = (lo + hi) >> 1;
		if (xra[mid] < ra) lo = mid + 1; else hi = mid;
	}
	const UINT32 info = xinfo[lo]; // (ra is always present)
	const UINT32 ip = info & 0xffff, len = (info >> 16) & 15;
	s->ip = (UINT16)(ip + len);
#ifdef REI_DRC_V25
	return ((((UINT32)s->ram.w[s->RBW + PS] << 4) + ip) & 0xfffff) | (info & 0xf00000);
#else
	return ((((UINT32)s->sregs[PS] << 4) + ip) & 0xfffff) | (info & 0xf00000);
#endif
}

// Must we exit at the next instruction boundary after a handler call (icount is checked by the block separately)?
static int drc_h_check(nec_state_t *s)
{
	rei_drc_stats[12]++; // (slow path = handler call count)
#ifdef REI_DRC_V25
	return s->stop_run || s->no_interrupt || s->halted || ((s->pending_irq & s->unmasked_irq) && ((s->pending_irq & NMI_IRQ) || s->IF)) ||
	       drc_map_dirty || rei_tim[s - sChips].dirty || drc_v25_idb != s->IDB; // (timer/IDB changed: go to the dispatcher)
#else
	return s->stop_run || s->no_interrupt || s->halted || (s->pending_irq && ((s->pending_irq & NMI_IRQ) || s->IF)) ||
	       drc_map_dirty;
#endif
}

// Memory routine slow path: save t0..t8/ra, icount/rei_ins_ic to memory, C handler, s6 = 1
// Memory routine slow path: save t0..t8/ra, icount/rei_ins_ic to memory, C handler. If the handler requires an exit
// (stop_run, interrupt, map change): s6 = DRC_BIAS, s1 -= DRC_BIAS so the end-of-instruction icount check (blez s1) exits at that boundary,
// and the exit routine restores s1 + s6
#define DRC_BIAS 0x40000000
static void drc_gen_slow_d(const void *fn, int dslot);
static void drc_gen_slow(const void *fn) { drc_gen_slow_d(fn, -1); }
// dslot >= 0: store the read value (v0) in that t save slot - restoring hands the value to that register (memory read variant)
static void drc_gen_slow_d(const void *fn, int dslot)
{
	for (int k = 0; k < 9; k++)
		e_sw(drc_saved_t[k], FR_T(k), rSP);
	e_sw(rRA, FR_HRA, rSP);
	e_addu(rAT, rS1, rS6);
	e_addiu(rAT, rAT, 1); // (s1 = icount - 1 inside blocks)
	e_sw(rAT, OFS(icount), rS0);
	e_li(rAT, (UINT32)(uintptr_t)&rei_ins_ic);
	e_sw(rS5, 0, rAT);
	e_call(fn);
	e_sw(rV0, dslot >= 0 ? FR_T(dslot) : FR_V0, rSP);
	e_move(rA0, rS0);
	e_call((const void *)drc_h_check);
	UINT32 *stay = e_beq(rV0, rZERO);
	e_nop();
	UINT32 *already = e_bne(rS6, rZERO);
	e_nop();
	e_lui(rS6, DRC_BIAS >> 16);
	e_subu(rS1, rS1, rS6);
	e_patch(stay, drc_ptr);
	e_patch(already, drc_ptr);
	if (dslot < 0) e_lw(rV0, FR_V0, rSP);
	for (int k = 0; k < 9; k++)
		e_lw(drc_saved_t[k], FR_T(k), rSP);
	e_lw(rRA, FR_HRA, rSP);
	e_jr(rRA);
	e_nop();
}

// Flag routines: t0 = dst, t1 = src -> t0 = result (masked), flags stored (same values as interpreter necmacro.h). Uses at, v1, t2
static void drc_gen_flag_tail(int word, int arith) // t2 = result
{
	const UINT32 sbit = word ? 0x8000 : 0x80;
	(void)sbit;
	if (arith) { // AuxVal = (res ^ (src ^ dst)) & 0x10
		e_xor(rAT, rT1, rT0); e_xor(rAT, rAT, rT2); e_andi(rAT, rAT, 0x10); e_sw(rAT, OFS(AuxVal), rS0);
	} else {
		e_sw(rZERO, OFS(CarryVal), rS0);
		e_sw(rZERO, OFS(OverVal), rS0);
		e_sw(rZERO, OFS(AuxVal), rS0);
	}
	if (word) e_seh(rAT, rT2); else e_seb(rAT, rT2);
	e_sw(rAT, OFS(SignVal), rS0);
	e_sw(rAT, OFS(ZeroVal), rS0);
	e_sw(rAT, OFS(ParityVal), rS0);
	e_jr(rRA);
	e_andi(rT0, rT2, word ? 0xffff : 0xff);
}

#ifdef REI_DRC_V25
// (V35 auto idle) Record mapped memory writes in the write ring (v25_wl_*) like interpreter rei_v25_wr: a0 = address (masked),
// value = low byte of vreg >> shift. Lets block-run iterations be compared for "same writes with same values" - no need to
// record that iteration through the interpreter. Uses at, v1, and a0 if shift is set
static void drc_emit_wlog(int vreg, int shift)
{
	const UINT32 P = (UINT32)(uintptr_t)&v25_wl_pos, A = (UINT32)(uintptr_t)v25_wl_a, V = (UINT32)(uintptr_t)v25_wl_v;
	e_lui(rAT, (int)(((P + 0x8000) >> 16) & 0xffff));
	e_lw(rV1, (int)(INT16)(P & 0xffff), rAT);
	e_addiu(rV1, rV1, 1);
	e_sw(rV1, (int)(INT16)(P & 0xffff), rAT);
	e_addiu(rV1, rV1, -1);
	e_andi(rV1, rV1, V25_WL - 1);
	e_sll(rV1, rV1, 2);
	e_lui(rAT, (int)(((A + 0x8000) >> 16) & 0xffff));
	e_addu(rAT, rAT, rV1);
	e_sw(rA0, (int)(INT16)(A & 0xffff), rAT);
	e_srl(rV1, rV1, 2);
	e_lui(rAT, (int)(((V + 0x8000) >> 16) & 0xffff));
	e_addu(rAT, rAT, rV1);
	if (shift) {
		e_srl(rA0, vreg, shift);
		e_sb(rA0, (int)(INT16)(V & 0xffff), rAT);
	} else {
		e_sb(vreg, (int)(INT16)(V & 0xffff), rAT);
	}
}
#endif

static void drc_gen_helpers(void)
{
	drc_ptr = drc_buf;
	// Block entry: the block lowers sp and saves ra before calling
	H_pro = drc_ptr;
	for (int i = 0; i < 8; i++)
		e_sw(rS0 + i, FR_S(i), rSP);
	e_sw(rFP, FR_FP, rSP);
	e_move(rS0, rA0);
#ifdef REI_DRC_V25
	e_lbu(rAT, OFS(RBW), rS0); e_sll(rAT, rAT, 1); e_addu(rAT, rAT, rS0); // (current bank)
	e_lhu(rV1, 2 * DS0, rAT);
	e_sll(rS7, rV1, 4);
	e_lhu(rV1, 2 * SS_, rAT);
	e_sll(rFP, rV1, 4);
#else
	e_lhu(rAT, OFSR(DS0), rS0);
	e_sll(rS7, rAT, 4);
	e_lhu(rAT, OFSR(SS_), rS0);
	e_sll(rFP, rAT, 4);
#endif
	e_lw(rS1, OFS(icount), rS0);
	e_addiu(rS1, rS1, -1); // s1 = icount - 1 inside blocks: "icount <= 0" becomes "s1 < 0" (single bltzal)
	e_lb(rS2, OFS(prefetch_count), rS0);
	e_li(rAT, (UINT32)(uintptr_t)&rei_nec_pg_rd);
	e_lw(rS3, 0, rAT);
	e_li(rAT, (UINT32)(uintptr_t)&rei_nec_pg_wr);
	e_lw(rS4, 0, rAT);
	e_jr(rRA);
	e_move(rS6, rZERO);
	drc_hbase = drc_buf;
#ifdef DRC_LAZY
	// Pending flag eval: if lz_k is set, call that routine with lz_a/lz_b. Uses at, v1, t0, t1, t2, t9, a2, a3 (only at instruction boundaries/exits)
	H_mat = drc_ptr;
	{
		e_lw(rT9, OFS(lz_k), rS0);
		e_andi(rAT, rT9, LZB_ALL);
		UINT32 *go = e_bne(rAT, rZERO);
		e_nop();
		e_jr(rRA);
		e_sw(rZERO, OFS(lz_k), rS0);
		e_patch(go, drc_ptr);
		e_srl(rV1, rT9, 8);
		e_sll(rV1, rV1, 3);
		e_addu(rV1, rV1, rAT);
		e_sll(rV1, rV1, 2);
		e_li(rT9, (UINT32)(uintptr_t)drc_lz_tab);
		e_addu(rT9, rT9, rV1);
		e_lw(rT9, 0, rT9);
		e_lw(rT0, OFS(lz_a), rS0);
		e_lw(rT1, OFS(lz_b), rS0);
		e_lw(rA2, OFS(CarryVal), rS0);
		e_move(rA3, rRA);
		e_jalr(rT9);
		e_sw(rZERO, OFS(lz_k), rS0);
		e_sw(rA2, OFS(CarryVal), rS0);
		e_jr(rA3);
		e_nop();
	}
#endif
	// Exit via a linkable exit (drc_emit_link): report the exit site to the dispatcher, then continue to H_exit_ip
	H_exit_link = drc_ptr;
	e_addiu(rV1, rRA, -36);
	e_li(rA1, (UINT32)(uintptr_t)&drc_last_stub);
	e_sw(rV1, 0, rA1);
	// Exit: at = ip to store (H_exit_ip), v0 = linear address of the last instruction
	H_exit_ip = drc_ptr;
	e_sh(rAT, OFS(ip), rS0);
	H_epi = drc_ptr;
#ifdef DRC_LAZY
	{ // (the interpreter knows nothing of lazy flags: finish pending eval before exiting. H_mat leaves v0 = last address untouched)
		e_lw(rT9, OFS(lz_k), rS0);
		UINT32 *none = e_beq(rT9, rZERO);
		e_nop();
		e_jal(H_mat);
		e_nop();
		e_patch(none, drc_ptr);
	}
#endif
	e_addu(rAT, rS1, rS6); // (undo bias)
	e_addiu(rAT, rAT, 1);
	e_sw(rAT, OFS(icount), rS0);
	e_sb(rS2, OFS(prefetch_count), rS0);
	e_lw(rRA, FR_RA, rSP);
	e_lw(rFP, FR_FP, rSP);
	for (int i = 0; i < 8; i++)
		e_lw(rS0 + i, FR_S(i), rSP);
	e_jr(rRA);
	e_addiu(rSP, rSP, DRC_FRAME);

	// Exit from end-of-instruction icount check (bltzal s1 -> block-end j H_iexit): look up ra in the exit table for ip, prefetch count, last address
	UINT32 *br, *br2;
	H_iexit = drc_ptr;
	e_move(rA0, rS0);
	e_move(rA1, rRA);
	e_call((const void *)drc_iexit);
	e_srl(rAT, rV0, 20);
	br = e_beq(rAT, rZERO);
	e_nop();
	e_addiu(rS2, rAT, -1);
	e_patch(br, drc_ptr);
	e_ext(rV0, rV0, 0, 20);
	e_j(H_epi);
	e_nop();

	// Jump cache (runtime target at block end, ip/PS in memory, prefetch 0): v0 = last instruction address. If found and icount > 0, jump to
	// that block's body, else H_epi
	{
		const UINT32 jc = (UINT32)(uintptr_t)&drc_jc[0][0];
		H_jc = drc_ptr;
		e_addiu(rS2, rZERO, 0); // (all callers are control transfers that flushed prefetch: s2 = 0 on exit too)
		e_lhu(rT0, OFS(ip), rS0);
#ifdef REI_DRC_V25
		e_lbu(rAT, OFS(RBW), rS0); e_sll(rAT, rAT, 1); e_addu(rAT, rAT, rS0);
		e_lhu(rT1, 2 * PS, rAT);
#else
		e_lhu(rT1, OFSR(PS), rS0);
#endif
		e_sll(rT1, rT1, 16);
		e_or(rT2, rT1, rT0);           // key PS << 16 | ip
		e_srl(rAT, rT2, 10);
		e_xor(rAT, rAT, rT2);
		e_andi(rAT, rAT, DRC_JC - 1);
		e_sll(rAT, rAT, 3);
		e_lui(rV1, (int)((jc + 0x8000) >> 16));
		e_addu(rV1, rV1, rAT);
		e_lw(rAT, (int)(INT16)(jc & 0xffff), rV1);
		UINT32 *m1 = e_bne(rAT, rT2);
		e_lw(rV1, (int)(INT16)(jc & 0xffff) + 4, rV1);
		UINT32 *m2 = e_bltz(rS1);
		e_addiu(rS2, rZERO, 0);
		e_jr(rV1);
		e_nop();
		e_patch(m1, drc_ptr);
		e_patch(m2, drc_ptr);
		e_j(H_epi);
		e_nop();
	}

	// Memory: a0 = linear address (unmasked), a1 = value to write -> v0 = value read. Uses at, v1, a0; t registers preserved
	H_rb = drc_ptr;
	drc_mask(rA0, rA0);
	e_srl(rAT, rA0, 9); e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
	br = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
	e_jr(rRA);
	e_lbu(rV0, 0, rAT);
	e_patch(br, drc_ptr);
	H_rslow[0] = drc_ptr;
	drc_gen_slow((const void *)drc_h_rb);

	// Word: even address = one halfword access (never crosses a page; mapped buffers are even-aligned - VezMapArea checks), odd = two bytes
	UINT32 *brf;
	H_rw = drc_ptr;
	drc_mask(rA0, rA0);
	e_andi(rV1, rA0, 1);
	br = e_bne(rV1, rZERO);
	e_srl(rAT, rA0, 9);
	e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
	br2 = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
	e_jr(rRA);
	e_lhu(rV0, 0, rAT);
	e_patch(br, drc_ptr);
	brf = br2; // (to the slow path - linked below)
	e_andi(rV1, rA0, 0x1ff); e_xori(rV1, rV1, 0x1ff);
	br = e_beq(rV1, rZERO);
	e_srl(rAT, rA0, 9);
	e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
	br2 = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
	e_lbu(rV0, 0, rAT); e_lbu(rV1, 1, rAT); e_sll(rV1, rV1, 8);
	e_jr(rRA);
	e_or(rV0, rV0, rV1);
	e_patch(br, drc_ptr); e_patch(br2, drc_ptr); e_patch(brf, drc_ptr);
	H_rslow[1] = drc_ptr;
	drc_gen_slow((const void *)drc_h_rw);

	H_wb = drc_ptr;
	drc_mask(rA0, rA0);
	e_srl(rAT, rA0, 9); e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
	br = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
#ifdef REI_DRC_V25
	e_sb(rA1, 0, rAT);
	drc_emit_wlog(rA1, 0); // (auto-idle write log - same as interpreter rei_v25_wr)
	e_jr(rRA);
	e_nop();
#else
	e_jr(rRA);
	e_sb(rA1, 0, rAT);
#endif
	e_patch(br, drc_ptr);
	H_wslow[0] = drc_ptr;
	drc_gen_slow((const void *)drc_h_wb);

	H_ww = drc_ptr;
	drc_mask(rA0, rA0);
#ifndef REI_DRC_V25
	e_andi(rV1, rA0, 1);
	br = e_bne(rV1, rZERO);
	e_srl(rAT, rA0, 9);
	e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
	br2 = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
	e_jr(rRA);
	e_sh(rA1, 0, rAT);
	e_patch(br, drc_ptr);
	brf = br2; // (to the slow path - linked below)
#endif
	e_andi(rV1, rA0, 0x1ff); e_xori(rV1, rV1, 0x1ff);
	br = e_beq(rV1, rZERO);
	e_srl(rAT, rA0, 9);
	e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
	br2 = e_beq(rAT, rZERO);
	e_addu(rAT, rAT, rA0);
	e_sb(rA1, 0, rAT); e_srl(rV1, rA1, 8);
#ifdef REI_DRC_V25
	e_sb(rV1, 1, rAT);
	drc_emit_wlog(rA1, 0);     // (a, low byte)
	e_addiu(rA0, rA0, 1);
	drc_emit_wlog(rA1, 8);     // (a + 1, high byte)
	e_jr(rRA);
	e_nop();
#else
	e_jr(rRA);
	e_sb(rV1, 1, rAT);
#endif
	e_patch(br, drc_ptr); e_patch(br2, drc_ptr);
#ifndef REI_DRC_V25
	e_patch(brf, drc_ptr);
#endif
	H_wslow[1] = drc_ptr;
	drc_gen_slow((const void *)drc_h_ww);

#if !defined(REI_DRC_V25) && DRC_MEMV
	// Memory variants (same fast path as above; slow path sets a0/a1 and joins the shared slow path - reads one per destination register)
	for (int w = 0; w < 2; w++)
		for (int d = 0; d < 2; d++) {
			H_rs[w][d] = drc_ptr;
			drc_gen_slow_d(w ? (const void *)drc_h_rw : (const void *)drc_h_rb, d);
		}
	for (int w = 0; w < 2; w++)
		for (int ai = 0; ai < 2; ai++)
			for (int di = 0; di < 2; di++) {
				const int ra = ai ? rT6 : rT5, rd = di ? rT1 : rT0;
				UINT32 *sl[3] = { NULL, NULL, NULL };
				// read
				H_rv[w][ai][di] = drc_ptr;
				drc_mask(rA0, ra);
				if (w) {
					e_andi(rV1, rA0, 1);
					UINT32 *odd = e_bne(rV1, rZERO);
					e_srl(rAT, rA0, 9);
					e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
					sl[0] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_jr(rRA);
					e_lhu(rd, 0, rAT);
					e_patch(odd, drc_ptr);
					e_andi(rV1, rA0, 0x1ff); e_xori(rV1, rV1, 0x1ff);
					sl[1] = e_beq(rV1, rZERO);
					e_srl(rAT, rA0, 9);
					e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
					sl[2] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_lbu(rd, 0, rAT); e_lbu(rV1, 1, rAT); e_sll(rV1, rV1, 8);
					e_jr(rRA);
					e_or(rd, rd, rV1);
				} else {
					e_srl(rAT, rA0, 9); e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS3); e_lw(rAT, 0, rAT);
					sl[0] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_jr(rRA);
					e_lbu(rd, 0, rAT);
				}
				for (int k = 0; k < 3; k++) e_patch(sl[k], drc_ptr);
				e_j(H_rs[w][di]);
				e_nop();
				// write
				sl[0] = sl[1] = sl[2] = NULL;
				H_wv[w][ai][di] = drc_ptr;
				drc_mask(rA0, ra);
				if (w) {
					e_andi(rV1, rA0, 1);
					UINT32 *odd = e_bne(rV1, rZERO);
					e_srl(rAT, rA0, 9);
					e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
					sl[0] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_jr(rRA);
					e_sh(rd, 0, rAT);
					e_patch(odd, drc_ptr);
					e_andi(rV1, rA0, 0x1ff); e_xori(rV1, rV1, 0x1ff);
					sl[1] = e_beq(rV1, rZERO);
					e_srl(rAT, rA0, 9);
					e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
					sl[2] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_sb(rd, 0, rAT); e_srl(rV1, rd, 8);
					e_jr(rRA);
					e_sb(rV1, 1, rAT);
				} else {
					e_srl(rAT, rA0, 9); e_sll(rAT, rAT, 2); e_addu(rAT, rAT, rS4); e_lw(rAT, 0, rAT);
					sl[0] = e_beq(rAT, rZERO);
					e_addu(rAT, rAT, rA0);
					e_jr(rRA);
					e_sb(rd, 0, rAT);
				}
				for (int k = 0; k < 3; k++) e_patch(sl[k], drc_ptr);
				e_j(H_wslow[w]);
				e_move(rA1, rd);
			}
#endif

	for (int w = 0; w < 2; w++) {
		const UINT32 cbit = w ? 0x10000 : 0x100, sbit = w ? 0x8000 : 0x80;
		// ADC -> ADD (src += CF, then fall through)
		H_adc[w] = drc_ptr;
		e_lw(rAT, OFS(CarryVal), rS0); e_sltu(rAT, rZERO, rAT); e_addu(rT1, rT1, rAT);
		H_add[w] = drc_ptr;
		e_addu(rT2, rT0, rT1);
		e_li(rV1, cbit); e_and(rAT, rT2, rV1); e_sw(rAT, OFS(CarryVal), rS0);
		e_xor(rAT, rT2, rT1); e_xor(rV1, rT2, rT0); e_and(rAT, rAT, rV1); e_andi(rAT, rAT, sbit); e_sw(rAT, OFS(OverVal), rS0);
		drc_gen_flag_tail(w, 1);
		// SBB -> SUB (CMP is SUB too, just without storing the result)
		H_sbb[w] = drc_ptr;
		e_lw(rAT, OFS(CarryVal), rS0); e_sltu(rAT, rZERO, rAT); e_addu(rT1, rT1, rAT);
		H_sub[w] = drc_ptr;
		e_subu(rT2, rT0, rT1);
		e_li(rV1, cbit); e_and(rAT, rT2, rV1); e_sw(rAT, OFS(CarryVal), rS0);
		e_xor(rAT, rT0, rT1); e_xor(rV1, rT0, rT2); e_and(rAT, rAT, rV1); e_andi(rAT, rAT, sbit); e_sw(rAT, OFS(OverVal), rS0);
		drc_gen_flag_tail(w, 1);
		// OR/AND/XOR: result in t0 (computed by the caller)
		H_log[w] = drc_ptr;
		e_move(rT2, rT0);
		drc_gen_flag_tail(w, 0);
		// INC/DEC: t0 -> t0, CF unchanged
		for (int dec = 0; dec < 2; dec++) {
			(dec ? H_dec : H_inc)[w] = drc_ptr;
			e_addiu(rT2, rT0, dec ? -1 : 1);
			e_xori(rAT, rT0, dec ? (w ? 0x8000 : 0x80) : (w ? 0x7fff : 0x7f));
			e_sltiu(rAT, rAT, 1);
			e_sw(rAT, OFS(OverVal), rS0);
			e_xori(rAT, rT0, 1); e_xor(rAT, rAT, rT2); e_andi(rAT, rAT, 0x10); e_sw(rAT, OFS(AuxVal), rS0);
			if (w) e_seh(rAT, rT2); else e_seb(rAT, rT2);
			e_sw(rAT, OFS(SignVal), rS0); e_sw(rAT, OFS(ZeroVal), rS0); e_sw(rAT, OFS(ParityVal), rS0);
			e_jr(rRA);
			e_andi(rT0, rT2, w ? 0xffff : 0xff);
		}
		// SZP only: t0
		H_szp[w] = drc_ptr;
		if (w) e_seh(rAT, rT0); else e_seb(rAT, rT0);
		e_sw(rAT, OFS(SignVal), rS0); e_sw(rAT, OFS(ZeroVal), rS0);
		e_jr(rRA);
		e_sw(rAT, OFS(ParityVal), rS0);
	}
#ifdef DRC_LAZY
	for (int w = 0; w < 2; w++) {
		const UINT32 mask = w ? 0xffff : 0xff;
		for (int sub = 0; sub < 2; sub++) { // ADC/SBB: src += CF then ADD/SUB (same inputs as routines H_adc/H_sbb)
			(sub ? H_lzsbb : H_lzadc)[w] = drc_ptr;
			e_lw(rAT, OFS(CarryVal), rS0); e_sltu(rAT, rZERO, rAT); e_addu(rT1, rT1, rAT);
			(sub ? H_lzsub : H_lzadd)[w] = drc_ptr;
			e_sw(rT0, OFS(lz_a), rS0);
			e_sw(rT1, OFS(lz_b), rS0);
			if (sub) e_subu(rT2, rT0, rT1); else e_addu(rT2, rT0, rT1);
			e_li(rV1, w ? 0x10000 : 0x100); e_and(rAT, rT2, rV1); e_sw(rAT, OFS(CarryVal), rS0);
			e_ori(rAT, rZERO, (((sub ? LZK_SUB : LZK_ADD) * 2 + w + 1) << 8) | LZB_ALL);
			e_sw(rAT, OFS(lz_k), rS0);
			e_jr(rRA);
			e_andi(rT0, rT2, mask);
		}
		H_lzlog[w] = drc_ptr; // (t0 = result computed in the caller's delay slot)
		e_andi(rT0, rT0, mask);
		e_sw(rZERO, OFS(CarryVal), rS0);
		e_sw(rT0, OFS(lz_a), rS0);
		e_ori(rAT, rZERO, ((LZK_LOG * 2 + w + 1) << 8) | LZB_ALL);
		e_jr(rRA);
		e_sw(rAT, OFS(lz_k), rS0);
		for (int dec = 0; dec < 2; dec++) {
			(dec ? H_lzdec : H_lzinc)[w] = drc_ptr;
			e_sw(rT0, OFS(lz_a), rS0);
			e_addiu(rT2, rT0, dec ? -1 : 1);
			e_ori(rAT, rZERO, (((dec ? LZK_DEC : LZK_INC) * 2 + w + 1) << 8) | LZB_ALL);
			e_sw(rAT, OFS(lz_k), rS0);
			e_jr(rRA);
			e_andi(rT0, rT2, mask);
		}
	}
	// Lazy flag eval table: "all" uses the routines above, partial (O+A / A) only those groups (same formulas as above)
	for (int w = 0; w < 2; w++) {
		const UINT32 sbit = w ? 0x8000 : 0x80;
		UINT32 *const full[5] = { H_add[w], H_sub[w], H_log[w], H_inc[w], H_dec[w] };
		for (int kind = 0; kind < 5; kind++) {
			UINT32 *const row = drc_lz_tab + (kind * 2 + w + 1) * 8;
			row[LZB_ALL] = (UINT32)(uintptr_t)full[kind];
			for (int var = 0; var < 2; var++) { // 0: O·A, 1: A
				row[var ? LZB_A : (LZB_O | LZB_A)] = (UINT32)(uintptr_t)drc_ptr;
				if (kind == LZK_LOG) {
					if (!var) e_sw(rZERO, OFS(OverVal), rS0);
					e_jr(rRA);
					e_sw(rZERO, OFS(AuxVal), rS0);
					continue;
				}
				if (kind == LZK_ADD || kind == LZK_SUB) {
					if (kind == LZK_ADD) e_addu(rT2, rT0, rT1); else e_subu(rT2, rT0, rT1);
					if (!var) {
						if (kind == LZK_ADD) { e_xor(rAT, rT2, rT1); e_xor(rV1, rT2, rT0); }
						else { e_xor(rAT, rT0, rT1); e_xor(rV1, rT0, rT2); }
						e_and(rAT, rAT, rV1); e_andi(rAT, rAT, sbit); e_sw(rAT, OFS(OverVal), rS0);
					}
					e_xor(rAT, rT1, rT0); e_xor(rAT, rAT, rT2); e_andi(rAT, rAT, 0x10);
				} else {
					const int dec = kind == LZK_DEC;
					e_addiu(rT2, rT0, dec ? -1 : 1);
					if (!var) {
						e_xori(rAT, rT0, dec ? (w ? 0x8000 : 0x80) : (w ? 0x7fff : 0x7f));
						e_sltiu(rAT, rAT, 1);
						e_sw(rAT, OFS(OverVal), rS0);
					}
					e_xori(rAT, rT0, 1); e_xor(rAT, rAT, rT2); e_andi(rAT, rAT, 0x10);
				}
				e_jr(rRA);
				e_sw(rAT, OFS(AuxVal), rS0);
			}
		}
	}
#endif
	// Ports: always via handler (same save/exit check as the memory slow path). a0 = port, a1 = value -> v0
	H_in[0] = drc_ptr; drc_gen_slow((const void *)drc_h_in8);
	H_in[1] = drc_ptr; drc_gen_slow((const void *)drc_h_in16);
	H_out[0] = drc_ptr; drc_gen_slow((const void *)drc_h_out8);
	H_out[1] = drc_ptr; drc_gen_slow((const void *)drc_h_out16);
	drc_code0 = drc_ptr;
}

// ---------------------------------------------------------------------------
// Memory access (calls shared routines). ra = address register, rd/rv = t registers

// Before an instruction's first memory access: s5 = icount at instruction start (handler rei_ins_ic) = current icount + cycles already subtracted
static void drc_mem_begin(void)
{
	if (dc.mem)
		return;
	dc.mem = 1;
	if (dc.dreg) { // (s1 = icount - 1)
		e_addu(rS5, rS1, rT8);
		e_addiu(rS5, rS5, dc.dconst + 1);
	} else
		e_addiu(rS5, rS1, dc.dconst + 1);
}

// Call the variant if usable (jal + delay slot: s5 calc on first memory access, otherwise the previous instruction)
static int drc_memv(UINT32 *const tab[2][2][2], int w, int rd, int ra)
{
#if !defined(REI_DRC_V25) && DRC_MEMV
	if ((ra == rT5 || ra == rT6) && (rd == rT0 || rd == rT1)) {
		UINT32 *h = tab[w][ra == rT6][rd == rT1];
		if (!dc.mem && !dc.dreg) {
			dc.mem = 1;
			e_jal(h);
			e_addiu(rS5, rS1, dc.dconst + 1);
		} else {
			drc_mem_begin();
			e_jal_ds(h);
		}
		return 1;
	}
#endif
	(void)tab; (void)w; (void)rd; (void)ra;
	return 0;
}

static void drc_rb(int rd, int ra)
{
	if (drc_memv(H_rv, 0, rd, ra)) return;
	drc_mem_begin();
	e_jal(H_rb);
	e_move(rA0, ra);
	e_move(rd, rV0);
}
static void drc_rw(int rd, int ra)
{
	if (drc_memv(H_rv, 1, rd, ra)) return;
	drc_mem_begin();
	e_jal(H_rw);
	e_move(rA0, ra);
	e_move(rd, rV0);
}
static void drc_wb(int rv, int ra)
{
	if (drc_memv(H_wv, 0, rv, ra)) return;
	drc_mem_begin();
	e_move(rA0, ra);
	e_jal(H_wb);
	e_move(rA1, rv);
}
static void drc_ww(int rv, int ra)
{
	if (drc_memv(H_wv, 1, rv, ra)) return;
	drc_mem_begin();
	e_move(rA0, ra);
	e_jal(H_ww);
	e_move(rA1, rv);
}

// ---------------------------------------------------------------------------
// Operands

static void drc_lw16(int rd, int wreg) { e_lhu(rd, OFW(wreg), rS0); }
static void drc_sw16(int rs, int wreg) { e_sh(rs, OFW(wreg), rS0); }
static void drc_lb8(int rd, int breg) { e_lbu(rd, OFB(breg), rS0); }
static void drc_sb8(int rs, int breg) { e_sb(rs, OFB(breg), rS0); }

struct drc_dec {
	UINT32 pc;
	int len, npre, seg;  // seg: segment prefix (-1 none)
	UINT8 op, m;         // opcode, ModRM
	int mod, reg, rm;
	int p;               // position of the byte after the opcode (relative to pc)
	int rep;             // REP(E/NE) prefix (MOVS/STOS only)
};

static inline UINT8 drc_b(const struct drc_dec *d, int i) { return drc_byte(d->pc + i); }
static inline UINT32 drc_w(const struct drc_dec *d, int i) { return drc_b(d, i) | (drc_b(d, i + 1) << 8); }

// Position after the ModRM displacement (immediate start)
static int drc_after_modrm(const struct drc_dec *d)
{
	return d->p + drc_modrm_len(d->m);
}

// Register holding the segment base (Sreg << 4): DS0 = s7, SS_ = fp (carried by the block), others loaded into at
static int drc_segbase(int seg)
{
	if (seg == DS0) return rS7;
	if (seg == SS_) return rFP;
	e_lhu(rAT, OFSR(seg), rS0);
	e_sll(rAT, rAT, 4);
	return rAT;
}

// EA: t7 = EO (16-bit), t6 = linear address (unmasked: memory routines clip to 0xfffff). (same as interpreter EA_xxx)
static void drc_ea(const struct drc_dec *d)
{
	const int mod = d->mod, rm = d->rm;
	int defseg = DS0;
	if (mod == 0 && rm == 6) {
		e_li(rT7, drc_w(d, d->p + 1));
	} else {
		switch (rm) {
		case 0: drc_lw16(rT7, BW); drc_lw16(rAT, IX); e_addu(rT7, rT7, rAT); break;
		case 1: drc_lw16(rT7, BW); drc_lw16(rAT, IY); e_addu(rT7, rT7, rAT); break;
		case 2: drc_lw16(rT7, BP); drc_lw16(rAT, IX); e_addu(rT7, rT7, rAT); defseg = SS_; break;
		case 3: drc_lw16(rT7, BP); drc_lw16(rAT, IY); e_addu(rT7, rT7, rAT); defseg = SS_; break;
		case 4: drc_lw16(rT7, IX); break;
		case 5: drc_lw16(rT7, IY); break;
		case 6: drc_lw16(rT7, BP); defseg = SS_; break;
		case 7: drc_lw16(rT7, BW); break;
		}
		int disp = 0;
		if (mod == 1) disp = (INT8)drc_b(d, d->p + 1);
		else if (mod == 2) disp = (INT16)drc_w(d, d->p + 1);
		if (disp) e_addiu(rT7, rT7, disp);
		if (disp || rm < 4) e_andi(rT7, rT7, 0xffff); // (single register is already 16-bit)
	}
	const int seg = d->seg >= 0 ? d->seg : defseg;
	e_addu(rT6, drc_segbase(seg), rT7); // (masking done by the memory routines)
}

// segment:offset registers -> masked linear address (DefaultBase: DS0/SS_ follow the prefix)
static void drc_segaddr(int rd, int seg, int roff, const struct drc_dec *d)
{
	if (d && d->seg >= 0 && (seg == DS0 || seg == SS_))
		seg = d->seg;
	e_addu(rd, drc_segbase(seg), roff);
}

// r/m read: value in rd. If memory, t6 = address (for write-back)
static void drc_get_rm(const struct drc_dec *d, int word, int rd)
{
	if (d->mod == 3) {
		if (word) drc_lw16(rd, drc_wreg[d->rm]);
		else drc_lb8(rd, drc_breg[d->rm]);
	} else {
		drc_ea(d);
		if (word) drc_rw(rd, rT6);
		else drc_rb(rd, rT6);
	}
}
// r/m write-back (EA already in t6)
static void drc_put_rm_back(const struct drc_dec *d, int word, int rs)
{
	if (d->mod == 3) {
		if (word) drc_sw16(rs, drc_wreg[d->rm]);
		else drc_sb8(rs, drc_breg[d->rm]);
	} else {
		if (word) drc_ww(rs, rT6);
		else drc_wb(rs, rT6);
	}
}
// r/m write (EA computed here)
static void drc_put_rm(const struct drc_dec *d, int word, int rs)
{
	if (d->mod != 3)
		drc_ea(d);
	drc_put_rm_back(d, word, rs);
}
static void drc_get_reg(const struct drc_dec *d, int word, int rd)
{
	if (word) drc_lw16(rd, drc_wreg[d->reg]);
	else drc_lb8(rd, drc_breg[d->reg]);
}
static void drc_put_reg(const struct drc_dec *d, int word, int rs)
{
	if (word) drc_sw16(rs, drc_wreg[d->reg]);
	else drc_sb8(rs, drc_breg[d->reg]);
}

// ---------------------------------------------------------------------------
// Flags (shared routines, same values as interpreter necmacro.h)

static void drc_szp(int r, int word) // SignVal = ZeroVal = ParityVal = (INT8/INT16)r
{
	drc_lz_direct(LZB_SZP);
	if (r != rT0) e_move(rT0, r);
	e_jal_ds(H_szp[word]);
}
static void drc_cf_to(int rd) // rd = CF (0/1)
{
	e_lw(rd, OFS(CarryVal), rS0);
	e_sltu(rd, rZERO, rd);
}

#ifdef DRC_LAZY
// Leave lazy flags: routine to evaluate (lz_k unchanged if the same was just left earlier in this block)
static void drc_lz_note(int kind, int word, int plain) // (H_lz* was called: compile state only)
{
	lz_mask = LZB_ALL;
	lz_known = 1;
	lz_kind = kind;
	lz_w = word;
	lz_plain = plain;
}
// Evaluate pending flags now (only at instruction start: uses t registers)
static void drc_lz_flush(void)
{
	if (!lz_mask) return;
#if !defined(__psp__)
	drc_prof_flush_note(lz_known ? 0 : 0x100);
#endif
	e_jal_ds(H_mat);
	lz_mask = 0;
	lz_known = 0;
}
// This instruction fully redefines those groups: drop them from pending (uses at. Only if it always writes them - no skip path allowed)
static void drc_lz_drop(int bits)
{
	if (!(lz_mask & bits)) return;
	e_lw(rAT, OFS(lz_k), rS0);
	e_andi(rAT, rAT, 0xff00 | (LZB_ALL & ~bits));
	e_sw(rAT, OFS(lz_k), rS0);
	lz_mask &= ~bits;
}
#else
static inline void drc_lz_drop(int bits) { (void)bits; }
#endif

// ALU ops 0..7 = ADD OR ADC SBB AND SUB XOR CMP. dst(t0) op src(t1) -> t0 = result (masked), flags stored
static void drc_alu(int aop, int word)
{
#ifdef DRC_LAZY
	switch (aop) {
	case 0: e_jal_ds(H_lzadd[word]); break;
	case 2: e_jal_ds(H_lzadc[word]); break;
	case 3: e_jal_ds(H_lzsbb[word]); break;
	case 5: case 7: e_jal_ds(H_lzsub[word]); break;
	case 1: e_jal(H_lzlog[word]); e_or(rT0, rT0, rT1); break;  // (computed in the delay slot)
	case 4: e_jal(H_lzlog[word]); e_and(rT0, rT0, rT1); break;
	case 6: e_jal(H_lzlog[word]); e_xor(rT0, rT0, rT1); break;
	}
	drc_lz_note(aop == 1 || aop == 4 || aop == 6 ? LZK_LOG : (aop == 3 || aop == 5 || aop == 7) ? LZK_SUB : LZK_ADD, word, aop == 5 || aop == 7);
	return;
#endif
	switch (aop) {
	case 0: e_jal_ds(H_add[word]); break;
	case 2: e_jal_ds(H_adc[word]); break;
	case 3: e_jal_ds(H_sbb[word]); break;
	case 5: case 7: e_jal_ds(H_sub[word]); break;
	case 1: e_jal(H_log[word]); e_or(rT0, rT0, rT1); break;  // (computed in the delay slot)
	case 4: e_jal(H_log[word]); e_and(rT0, rT0, rT1); break;
	case 6: e_jal(H_log[word]); e_xor(rT0, rT0, rT1); break;
	}
}

// INC/DEC: t0 = value -> t0 = result (masked). CF unchanged
static void drc_incdec(int dec, int word)
{
#ifdef DRC_LAZY
	e_jal_ds(dec ? H_lzdec[word] : H_lzinc[word]);
	drc_lz_note(dec ? LZK_DEC : LZK_INC, word, 0);
	return;
#endif
	e_jal_ds(dec ? H_dec[word] : H_inc[word]);
}

// Condition (jcc 70..7f) -> true if rd != 0
// Condition (jcc 70..7f) -> value in rd; returns 1: true if rd != 0, 0: true if rd == 0
#ifdef DRC_LAZY
// rd = lazy result (unmasked). Uses at
static void drc_lz_res(int rd)
{
	e_lw(rd, OFS(lz_a), rS0);
	switch (lz_kind) {
	case LZK_ADD: e_lw(rAT, OFS(lz_b), rS0); e_addu(rd, rd, rAT); break;
	case LZK_SUB: e_lw(rAT, OFS(lz_b), rS0); e_subu(rd, rd, rAT); break;
	case LZK_INC: e_addiu(rd, rd, 1); break;
	case LZK_DEC: e_addiu(rd, rd, -1); break;
	}
}
// Can the condition be decided without pending eval? (C is always the direct CarryVal, Z, C|Z, S from recorded inputs, signed compares only for SUB/CMP)
static inline int drc_lz_cond_need(int cc)
{
	const int c = cc >> 1;
	return c == 1 ? 0 : c == 0 ? LZB_O : c >= 6 ? (LZB_SZP | LZB_O) : LZB_SZP;
}
static int drc_lz_cond_ok(int cc)
{
	const int c = cc >> 1;
	if (!(lz_mask & drc_lz_cond_need(cc))) return 1; // (memory value as is)
	if (!(lz_known && lz_mask == LZB_ALL)) return 0;
	return c == 2 || c == 3 || c == 4 || ((c == 6 || c == 7) && lz_kind == LZK_SUB && lz_plain);
}
#endif

static int drc_cond(int cc, int rd)
{
#ifdef DRC_LAZY
	if (lz_mask & drc_lz_cond_need(cc)) {
		if (!drc_lz_cond_ok(cc)) {
			lz_bad = 1;
		} else {
			const UINT32 mask = lz_w ? 0xffff : 0xff;
			switch (cc >> 1) {
			case 2: drc_lz_res(rd); e_andi(rd, rd, mask); return cc & 1;                                  // Z: result == 0
			case 3: drc_lz_res(rd); e_andi(rd, rd, mask); e_sltiu(rd, rd, 1);                              // C|Z
				e_lw(rAT, OFS(CarryVal), rS0); e_or(rd, rd, rAT); return !(cc & 1);
			case 4: drc_lz_res(rd); if (lz_w) e_seh(rd, rd); else e_seb(rd, rd); e_slt(rd, rd, rZERO); return !(cc & 1); // S
			default: // (SUB/CMP) L: a < b, NL: !(a < b), LE: !(b < a), NLE: b < a (signed compare)
				e_lw(rd, OFS(lz_a), rS0); e_lw(rAT, OFS(lz_b), rS0);
				if (lz_w) { e_seh(rd, rd); e_seh(rAT, rAT); } else { e_seb(rd, rd); e_seb(rAT, rAT); }
				if (cc == 0x0c) { e_slt(rd, rd, rAT); return 1; }
				if (cc == 0x0d) { e_slt(rd, rd, rAT); return 0; }
				e_slt(rd, rAT, rd);
				return cc == 0x0f;
			}
		}
	}
#endif
	switch (cc >> 1) {
	case 0: e_lw(rd, OFS(OverVal), rS0); return !(cc & 1);                         // O / NO
	case 1: e_lw(rd, OFS(CarryVal), rS0); return !(cc & 1);                        // C / NC
	case 2: e_lw(rd, OFS(ZeroVal), rS0); return cc & 1;                            // Z: ZeroVal == 0
	case 3: e_lw(rd, OFS(CarryVal), rS0); e_lw(rAT, OFS(ZeroVal), rS0); e_sltiu(rAT, rAT, 1); e_or(rd, rd, rAT); return !(cc & 1); // C|Z
	case 4: e_lw(rd, OFS(SignVal), rS0); e_slt(rd, rd, rZERO); return !(cc & 1);   // S
	case 5: // P = parity_table[(BYTE)ParityVal]
		e_lbu(rAT, OFS(ParityVal), rS0);
		e_li(rV1, (UINT32)(uintptr_t)parity_table);
		e_addu(rAT, rAT, rV1);
		e_lbu(rd, 0, rAT);
		return !(cc & 1);
	default: { // L: (SF!=OF)&&!ZF, NL: ZF||(SF==OF), LE: ZF||(SF!=OF), NLE: (SF==OF)&&!ZF
		e_lw(rd, OFS(SignVal), rS0); e_slt(rd, rd, rZERO);
		e_lw(rAT, OFS(OverVal), rS0); e_sltu(rAT, rZERO, rAT);
		e_xor(rd, rd, rAT);                                            // SF != OF
		e_lw(rAT, OFS(ZeroVal), rS0); e_sltiu(rAT, rAT, 1);             // ZF
		if (cc == 0x0c) { e_xori(rAT, rAT, 1); e_and(rd, rd, rAT); return 1; }               // jl
		if (cc == 0x0d) { e_xori(rd, rd, 1); e_or(rd, rd, rAT); return 1; }                  // jnl
		if (cc == 0x0e) { e_or(rd, rd, rAT); return 1; }                                      // jle
		e_or(rd, rd, rAT); return 0;                                                          // jnle = !(jle)
	}
	}
}

// ---------------------------------------------------------------------------
// End of instruction: prefetch (same closed form as rei_prefetch1). n = bytes fetched, cycles = dconst (+ t8)

// Result of one instruction (c0 = count at start, D = cycles, sh = log2(cycles per byte)) -> penalty cycles, new count (do_prefetch closed form)
static void drc_pf_fold_sh(int c0, int n, int D, int reset, int sh, int *pen, int *nc)
{
	int c = c0 - n, k = -c, diff = D;
	*pen = 0;
	if (k > 0) {
		int m = diff > 0 ? (diff - 1) >> sh : 0;
		if (m > k) m = k;
		diff -= m << sh;
		*pen = (k - m) << sh;
		c = 0;
	}
	if (reset) { *nc = 0; return; }
	if (diff >= (1 << sh)) {
		const int nb = diff >> sh, room = 6 - c;
		if (room > 0) c += nb < room ? nb : room;
	}
	*nc = c;
}
static inline void drc_pf_fold(int c0, int n, int D, int reset, int *pen, int *nc) { drc_pf_fold_sh(c0, n, D, reset, drc_pfsh, pen, nc); }

// Table for unknown count: [sh][n][min(D,dmax)][c0] = penalty << 3 | new count. Result is the same for D >= (n+6) << sh:
// for n < 11 it saturates at 16 on V33 (sh 0), 32 on V30 (sh 1). Penalty is k << sh <= 15 << 1 = 30 (fits in 5 bits)
#define DRC_PF_DMAX 34
static UINT8 drc_pf_tab[2][16][DRC_PF_DMAX + 1][8];
static inline int drc_pf_dmax(void) { return drc_pfsh ? DRC_PF_DMAX : 16; }
static void drc_pf_tab_init(void)
{
	for (int sh = 0; sh < 2; sh++)
		for (int n = 0; n < 16; n++)
			for (int D = 0; D <= DRC_PF_DMAX; D++)
				for (int c = 0; c < 7; c++) {
					int pen, nc;
					drc_pf_fold_sh(c, n, D, 0, sh, &pen, &nc);
					drc_pf_tab[sh][n][D][c] = (UINT8)((pen > 31 ? 31 : pen) << 3 | nc);
				}
}

static void drc_pf_sync(void) // put a single known count in s2 (before code that needs the register)
{
	const int v = pf_single();
	if (v >= 0 && !s2_ok) {
		e_li(rS2, (UINT32)v);
		s2_ok = 1;
	}
}

// Runtime calculation (general): s2 = start count, D = dconst + t8
static void drc_pf_runtime(int n, int reset)
{
	e_addiu(rV0, rS2, -n);
	if (dc.dreg) {
		if (dc.dconst) e_addiu(rT8, rT8, dc.dconst);
		e_move(rA2, rT8);
	} else
		e_li(rA2, (UINT32)dc.dconst);
	UINT32 *skip = e_bgez(rV0);
	e_nop();
	e_subu(rV1, rZERO, rV0);          // k
	e_addiu(rA0, rA2, -1);            // diff-1
	e_slt(rAT, rA0, rZERO);
	e_movn(rA0, rZERO, rAT);          // max(diff-1, 0)
	if (drc_pfsh) e_srl(rA0, rA0, drc_pfsh); // (diff-1) >> sh
	e_slt(rAT, rV1, rA0);
	e_movn(rA0, rV1, rAT);            // m = min(k, .)
	e_subu(rA1, rV1, rA0);            // k - m
	if (drc_pfsh) { e_sll(rA1, rA1, drc_pfsh); e_sll(rA0, rA0, drc_pfsh); } // penalty (k-m) << sh, diff -= m << sh
	e_subu(rS1, rS1, rA1);
	e_subu(rA2, rA2, rA0);
	e_move(rV0, rZERO);
	e_patch(skip, drc_ptr);
	if (reset) {
		e_move(rS2, rZERO);
		return;
	}
	if (drc_pfsh) e_srl(rA2, rA2, drc_pfsh); // fetchable bytes = diff >> sh (0 if diff < 1 << sh)
	e_li(rA3, 6);
	e_subu(rA3, rA3, rV0);
	e_slt(rAT, rA2, rA3);
	e_movn(rA3, rA2, rAT);
	e_addu(rS2, rV0, rA3);
}

// End-of-instruction prefetch. Precompute results over the possible count set (pf_set); fold to no code if they converge
static void drc_prefetch(int n, int reset)
{
	int Ds[2], nd = 1;
	Ds[0] = dc.dconst;
	if (dc.dreg) {
		if (dc.dalt <= 0) { // arbitrary: general calculation
			drc_pf_sync();
			drc_pf_runtime(n, reset);
			pf_set = reset ? 1 : 0x7f;
			s2_ok = 1;
			return;
		}
		Ds[1] = dc.dconst + dc.dalt; // t8 = 0 or dalt
		nd = 2;
	}
	int pen_all = -1, pen_same = 1, nc_set = 0, pen_by_d[2] = { -1, -1 }, pen_d_ok = 1, nc_by_d[2] = { -1, -1 }, nc_d_ok = 1;
	for (int bits = pf_set; bits; bits &= bits - 1) { // (possible counts only - usually one)
		const int c = __builtin_ctz(bits);
		for (int j = 0; j < nd; j++) {
			int pen, nc;
			if (n < 11) { // table (past saturation equals dmax): instead of computing
				const int dm = drc_pf_dmax();
				const UINT8 t = drc_pf_tab[drc_pfsh][n][Ds[j] < dm ? Ds[j] : dm][c];
				pen = t >> 3;
				nc = reset ? 0 : (t & 7);
			} else
				drc_pf_fold(c, n, Ds[j], reset, &pen, &nc);
			if (pen_all < 0) pen_all = pen; else if (pen != pen_all) pen_same = 0;
			if (pen_by_d[j] < 0) pen_by_d[j] = pen; else if (pen != pen_by_d[j]) pen_d_ok = 0;
			if (nc_by_d[j] < 0) nc_by_d[j] = nc; else if (nc != nc_by_d[j]) nc_d_ok = 0;
			nc_set |= 1 << nc;
		}
	}
	const int nc_single = (nc_set & (nc_set - 1)) == 0;
	if (pen_same && nc_single) { // all results identical
		if (pen_all) e_sub_s1(pen_all);
		pf_set = nc_set;
		s2_ok = 0;
		return;
	}
	if (nd == 2 && pen_d_ok && nc_d_ok) { // independent of start count, depends only on cycles (t8): select
		if (pen_by_d[0] == pen_by_d[1]) {
			if (pen_by_d[0]) e_sub_s1(pen_by_d[0]);
		} else {
			e_li(rV1, (UINT32)pen_by_d[0]);
			e_li(rAT, (UINT32)pen_by_d[1]);
			e_movn(rV1, rAT, rT8);
			e_subu(rS1, rS1, rV1);
		}
		if (nc_by_d[0] == nc_by_d[1]) {
			s2_ok = 0;
		} else {
			e_li(rS2, (UINT32)nc_by_d[0]);
			e_li(rAT, (UINT32)nc_by_d[1]);
			e_movn(rS2, rAT, rT8);
			s2_ok = 1;
		}
		pf_set = nc_set;
		return;
	}
	drc_pf_sync(); // (reaching here means several counts, so s2 is already correct - a single one finished above)
	if (nd == 1 && n < 11) { // table (penalty independent of reset; saturation holds only for n < 11)
		const int dm = drc_pf_dmax();
		const UINT32 a = (UINT32)(uintptr_t)&drc_pf_tab[drc_pfsh][n][dc.dconst < dm ? dc.dconst : dm][0];
		e_lui(rAT, (int)((a + 0x8000) >> 16));
		e_addu(rAT, rAT, rS2);
		e_lbu(rAT, (int)(INT16)(a & 0xffff), rAT);
		if (!reset && !nc_single) e_andi(rS2, rAT, 7);
		if (!pen_same) {
			e_srl(rAT, rAT, 3);
			e_subu(rS1, rS1, rAT);
		} else if (pen_all)
			e_addiu(rS1, rS1, -pen_all);
		pf_set = nc_set;
		s2_ok = !(reset || nc_single);
		return;
	}
	drc_pf_runtime(n, reset);
	pf_set = nc_set;
	s2_ok = 1;
}

// Linkable exit (fixed 9 words). Initially exits to the dispatcher; once the dispatcher finds the next block it patches [0]/[2]
// into "if icount > 0 go to the next block's body" (drc_try_link). [1] is the prefetch count the next block expects
//   [0] nop -> blez s1,[4]   [1] li s2,k   [2] nop -> j next block body   [3] nop
//   [4] ori at,zero,ip  [5] lui v0,pc>>16  [6] ori v0,v0,pc  [7] jal H_exit_link  [8] nop
static void drc_emit_link(UINT32 ip, UINT32 pc, int k)
{
	drc_ds_flush(); // ([0] is patched into a branch later: must not be a delay slot)
	e_nop();
	e_addiu(rS2, rZERO, k);
	e_nop();
	e_nop();
	e_ori(rAT, rZERO, (int)(ip & 0xffff));
	e_lui(rV0, (int)(pc >> 16));
	e_ori(rV0, rV0, (int)(pc & 0xffff));
	e_jal(H_exit_link);
	e_nop();
}

// Block exit (store ip, v0 = pc): only a branch in the body, actual code after it. Linkable exit if the prefetch count is known
static void drc_exit_branch_link(UINT32 *br, UINT32 ip, UINT32 pc)
{
	struct drc_stub *s = drc_new_stub(pf_single() >= 0 ? DST_LINK : DST_EXIT);
	s->br = br; s->ip = ip; s->pc = pc;
	if (s->kind == DST_LINK) s->s2k = (INT8)pf_single();
}

static void drc_exit_branch(UINT32 *br, UINT32 ip, UINT32 pc)
{
	struct drc_stub *s = drc_new_stub(DST_EXIT);
	s->br = br; s->ip = ip; s->pc = pc;
}

// ---------------------------------------------------------------------------
// Stack (same as PUSH/POP macros: SP is changed first)

static void drc_push(int rv) // rv is t0..t3 (uses t4/t5)
{
	drc_lw16(rT4, SP);
	e_addiu(rT4, rT4, -2);
	e_andi(rT4, rT4, 0xffff);
	drc_sw16(rT4, SP);
	e_addu(rT5, rFP, rT4);
	drc_ww(rv, rT5);
}
static void drc_pop(int rd) // value in rd (t0..t3)
{
	drc_lw16(rT4, SP);
	e_addiu(rT4, rT4, 2);
	e_andi(rT4, rT4, 0xffff);
	drc_sw16(rT4, SP);
	e_addiu(rT5, rT4, -2);
	e_andi(rT5, rT5, 0xffff);
	e_addu(rT5, rFP, rT5);
	drc_rw(rd, rT5);
}

// When a control transfer ends the block: ip (and PS) to memory (so the exit-check slow path doesn't rewrite ip)
static int drc_end_ip_set; // this instruction already stored ip
static int drc_end_target; // static target ip of the block-ending control transfer (-1 = determined at runtime) - for linkable exits
static void drc_set_ip_const(UINT32 ip)
{
	drc_end_target = (int)(ip & 0xffff);
	e_li(rAT, ip & 0xffff);
	e_sh(rAT, OFS(ip), rS0);
	drc_end_ip_set = 1;
}
static void drc_set_ip_reg(int r)
{
	drc_end_target = -1;
	e_sh(r, OFS(ip), rS0);
	drc_end_ip_set = 1;
}

// GetnextRMWord: (EA & 0xf0000) | ((EA + 2) & 0xffff), EA = t6
static void drc_next_rm_word(int rd)
{
	e_addiu(rT5, rT6, 2);
	e_andi(rT5, rT5, 0xffff);
	e_lui(rAT, 0xf);
	e_and(rAT, rT6, rAT);
	e_or(rT5, rT5, rAT);
	drc_rw(rd, rT5);
}

// Shifts/rotates (c0 c1 d0 d1 d2 d3). t0 = dst (= src), t3 = src
static void drc_shift(const struct drc_dec *d, int word)
{
	const UINT8 op = d->op;
	const int kind = d->reg;
	const UINT32 hbit = word ? 0x8000 : 0x80, cbit = word ? 0x10000 : 0x100;
	drc_get_rm(d, word, rT0);
	e_move(rT3, rT0);
	if (op == 0xd0 || op == 0xd1) { // once: CLKM(6,6,2,16,16,7) / CLKM(6,6,2,24,16,7)
		if (kind >= 4 && kind != 6) drc_lz_drop(LZB_O | LZB_SZP); // (shift: C/O/S/Z/P, rotate: C/O)
		else if (kind < 4) drc_lz_direct(LZB_O);
		drc_clk(d->mod == 3 ? CK(6, 2) : CK(16, 7));
		switch (kind) {
		case 0: e_andi(rT2, rT0, hbit); e_sll(rT0, rT0, 1); e_sltu(rAT, rZERO, rT2); e_addu(rT0, rT0, rAT); break;
		case 1: e_andi(rT2, rT0, 1); e_srl(rT0, rT0, 1); e_sltu(rAT, rZERO, rT2); e_sll(rAT, rAT, word ? 15 : 7); e_addu(rT0, rT0, rAT); break;
		case 2: drc_cf_to(rAT); e_sll(rT0, rT0, 1); e_addu(rT0, rT0, rAT); e_li(rV1, cbit); e_and(rT2, rT0, rV1); break;
		case 3: drc_cf_to(rAT); e_sll(rAT, rAT, word ? 16 : 8); e_addu(rT0, rAT, rT0); e_andi(rT2, rT0, 1); e_srl(rT0, rT0, 1); break;
		case 4: drc_clk(1); e_sll(rT0, rT0, 1); e_li(rV1, cbit); e_and(rT2, rT0, rV1); drc_szp(rT0, word); break;
		case 5: drc_clk(1); e_andi(rT2, rT0, 1); e_srl(rT0, rT0, 1); drc_szp(rT0, word); break;
		case 7:
			drc_clk(1);
			if (word) e_seh(rT0, rT0); else e_seb(rT0, rT0);
			e_andi(rT2, rT0, 1);
			e_sra(rT0, rT0, 1);
			drc_szp(rT0, word);
			break;
		default: return; // (6: undefined - cycles only)
		}
		e_sw(rT2, OFS(CarryVal), rS0);
		drc_put_rm_back(d, word, rT0);
		if (kind == 7) e_sw(rZERO, OFS(OverVal), rS0);
		else { e_xor(rAT, rT3, rT0); e_andi(rAT, rAT, hbit); e_sw(rAT, OFS(OverVal), rS0); }
		return;
	}
	// Count: c0/c1 immediate, d2/d3 CL. CLKM(7,7,2,19,19,6) / CLKM(7,7,2,27,19,6)
	drc_clk(d->mod == 3 ? CK(7, 2) : CK(19, 6));
	const int konst = (op == 0xc0 || op == 0xc1);
	const int c = konst ? drc_b(d, drc_after_modrm(d)) : -1;
	if (konst && c == 0) return;
	if (kind == 6) return; // (undefined: nothing regardless of c)
	if (kind >= 4 && konst) drc_lz_drop(LZB_SZP); // (shift: C/S/Z/P. CL count can be 0 and then writes nothing, so it can't be dropped)
	UINT32 *skip = NULL;
	if (konst) e_li(rT1, (UINT32)c);
	else {
		if (!dc.dreg) { // (CL = 0 skips the code below, so zero the runtime cycle sum first)
			e_move(rT8, rZERO);
			dc.dreg = 1;
		}
		drc_lb8(rT1, CL);
		skip = e_beq(rT1, rZERO);
		e_nop();
	}
	if (kind <= 3) { // rotate: CLK(1) per iteration
		if (kind >= 2) e_lw(rT2, OFS(CarryVal), rS0);
		if (konst) drc_clk(c); else drc_clk_reg(rT1);
		UINT32 *loop = drc_nomerge = drc_ptr;
		switch (kind) {
		case 0: e_andi(rT2, rT0, hbit); e_sll(rT0, rT0, 1); e_sltu(rAT, rZERO, rT2); e_addu(rT0, rT0, rAT); break;
		case 1: e_andi(rT2, rT0, 1); e_srl(rT0, rT0, 1); e_sltu(rAT, rZERO, rT2); e_sll(rAT, rAT, word ? 15 : 7); e_addu(rT0, rT0, rAT); break;
		case 2: e_sltu(rAT, rZERO, rT2); e_sll(rT0, rT0, 1); e_addu(rT0, rT0, rAT); e_li(rV1, cbit); e_and(rT2, rT0, rV1); break;
		case 3: e_sltu(rAT, rZERO, rT2); e_sll(rAT, rAT, word ? 16 : 8); e_addu(rT0, rAT, rT0); e_andi(rT2, rT0, 1); e_srl(rT0, rT0, 1); break;
		}
		e_addiu(rT1, rT1, -1);
		UINT32 *br = e_bgtz(rT1);
		e_nop();
		e_patch(br, loop);
		e_sw(rT2, OFS(CarryVal), rS0);
		drc_put_rm_back(d, word, rT0);
	} else { // shift: icount -= c first
		if (konst) drc_clk(c); else drc_clk_reg(rT1);
		if (kind == 4) {
			e_sllv(rT0, rT0, rT1);
			e_li(rV1, cbit); e_and(rT2, rT0, rV1);
		} else {
			e_addiu(rAT, rT1, -1);
			if (kind == 7) { if (word) e_seh(rT0, rT0); else e_seb(rT0, rT0); e_srav(rT0, rT0, rAT); }
			else e_srlv(rT0, rT0, rAT);
			e_andi(rT2, rT0, 1);
			if (kind == 7) { if (word) e_seh(rT0, rT0); else e_seb(rT0, rT0); e_sra(rT0, rT0, 1); }
			else e_srl(rT0, rT0, 1);
		}
		e_sw(rT2, OFS(CarryVal), rS0);
		drc_szp(rT0, word);
		drc_put_rm_back(d, word, rT0);
	}
	if (skip) e_patch(skip, drc_ptr);
}

// Single string op (a4..af): direction step = DF ? -1 : 1 (word +/-2) -> rd
static void drc_str_delta(int rd, int word)
{
	e_lbu(rAT, OFS(DF), rS0);
	e_sll(rAT, rAT, word ? 2 : 1);
	e_li(rd, word ? 2 : 1);
	e_subu(rd, rd, rAT);
}

// ---------------------------------------------------------------------------
// Per-instruction code. 1 = emitted, 0 = interpreter step. *end = 1 means this instruction ends the block (ip already in memory)

static int drc_emit_insn(const struct drc_dec *d, int *end)
{
	const UINT8 op = d->op;
	*end = 0;
	if (drc_native_off[op])
		return 0;
	const int word = op & 1;

	// ALU r/m forms: 00..3b (op&7 < 4), immediate forms op&7 = 4,5
	if (op < 0x40 && (op & 7) < 6) {
		const int aop = (op >> 3) & 7, form = op & 7;
		if (form == 4 || form == 5) { // AL/AW, imm
			if (form == 4) { drc_lb8(rT0, AL); e_li(rT1, drc_b(d, d->p)); }
			else { drc_lw16(rT0, AW); e_li(rT1, drc_w(d, d->p)); }
			drc_alu(aop, form == 5);
			if (aop != 7) { if (form == 4) drc_sb8(rT0, AL); else drc_sw16(rT0, AW); }
			drc_clk(CK(4, 2)); // CLKS(4,4,2)
			return 1;
		}
		const int w = form & 1;
		if (form < 2) { // r/m <- r/m op reg: CLKM(2,2,2,16,16,7) / CLKR(24,24,11,24,16,7,2), CMP CLKM(2,2,2,11,11,6) / CLKR(15,15,8,15,11,6,2)
			drc_get_rm(d, w, rT0);
			drc_get_reg(d, w, rT1);
			drc_alu(aop, w);
			if (aop != 7) drc_put_rm_back(d, w, rT0);
			if (d->mod == 3) drc_clk(2);
			else if (!w) drc_clk(aop == 7 ? CK(11, 6) : CK(16, 7));
			else if (aop == 7) drc_clk_parity(rT6, CK(15, 8), CK(11, 6));
			else drc_clk_parity(rT6, CK(24, 11), CK(16, 7));
		} else { // reg <- reg op r/m: CLKM(2,2,2,11,11,6) / CLKR(15,15,8,15,11,6,2)
			drc_get_reg(d, w, rT0);
			drc_get_rm(d, w, rT1);
			drc_alu(aop, w);
			if (aop != 7) drc_put_reg(d, w, rT0);
			if (d->mod == 3) drc_clk(2);
			else if (!w) drc_clk(CK(11, 6));
			else drc_clk_parity(rT6, CK(15, 8), CK(11, 6));
		}
		return 1;
	}

	switch (op) {
	case 0x80: case 0x81: case 0x82: case 0x83: {
		const int w = (op == 0x81 || op == 0x83), aop = d->reg;
		drc_get_rm(d, w, rT0);
		const int ii = drc_after_modrm(d);
		UINT32 imm;
		if (op == 0x81) imm = drc_w(d, ii);
		else if (op == 0x83) imm = (UINT16)(INT16)(INT8)drc_b(d, ii);
		else imm = drc_b(d, ii);
		e_li(rT1, imm);
		// CLKS(4,4,2) / CMP CLKS(13,13,6)·CLKW(17,17,8,17,13,6) / CLKS(18,18,7)·CLKW(26,26,11,26,18,7)
		if (d->mod == 3) drc_clk(CK(4, 2));
		else if (!w) drc_clk(aop == 7 ? CK(13, 6) : CK(18, 7));
		else if (aop == 7) drc_clk_parity(rT6, CK(17, 8), CK(13, 6));
		else drc_clk_parity(rT6, CK(26, 11), CK(18, 7));
		drc_alu(aop, w);
		if (aop != 7) drc_put_rm_back(d, w, rT0);
		return 1;
	}
	case 0x84: case 0x85: // TEST r/m, reg: CLKM(2,2,2,10,10,6) / CLKR(14,14,8,14,10,6,2)
		drc_get_rm(d, word, rT0);
		drc_get_reg(d, word, rT1);
		drc_alu(4, word);
		if (d->mod == 3) drc_clk(2);
		else if (!word) drc_clk(CK(10, 6));
		else drc_clk_parity(rT6, CK(14, 8), CK(10, 6));
		return 1;
	case 0x86: case 0x87: // XCHG: CLKM(3,3,3,16,18,8) / CLKR(24,24,12,24,16,8,3)
		drc_get_reg(d, word, rT1);
		drc_get_rm(d, word, rT0);
		drc_put_reg(d, word, rT0);
		drc_put_rm_back(d, word, rT1);
		if (d->mod == 3) drc_clk(3);
		else if (!word) drc_clk(CK(18, 8));
		else drc_clk_parity(rT6, CK(24, 12), CK(16, 8));
		return 1;
	case 0x88: case 0x89: // MOV r/m, reg: CLKM(2,2,2,9,9,3) / CLKR(13,13,5,13,9,3,2)
		drc_get_reg(d, word, rT1);
		drc_put_rm(d, word, rT1);
		if (d->mod == 3) drc_clk(2);
		else if (!word) drc_clk(CK(9, 3));
		else drc_clk_parity(rT6, CK(13, 5), CK(9, 3));
		return 1;
	case 0x8a: case 0x8b: // MOV reg, r/m: CLKM(2,2,2,11,11,5) / CLKR(15,15,7,15,11,5,2)
		drc_get_rm(d, word, rT1);
		drc_put_reg(d, word, rT1);
		if (d->mod == 3) drc_clk(2);
		else if (!word) drc_clk(CK(11, 5));
		else drc_clk_parity(rT6, CK(15, 7), CK(11, 5));
		return 1;
	case 0x8c: // MOV r/m, sreg: CLKR(14,14,5,14,10,3,2)
		if (d->reg > 3) return 0;
		e_lhu(rT1, OFSR(d->reg == 0 ? DS1 : d->reg == 1 ? PS : d->reg == 2 ? SS_ : DS0), rS0);
		drc_put_rm(d, 1, rT1);
		if (d->mod == 3) drc_clk(2);
		else drc_clk_parity(rT6, CK(14, 5), CK(10, 3));
		return 1;
	case 0x8d: // LEA: CLKS(4,4,2)
		if (d->mod == 3) return 0;
		drc_ea(d);
		drc_sw16(rT7, drc_wreg[d->reg]);
		drc_clk(CK(4, 2));
		return 1;
	case 0x8e: // MOV sreg, r/m (no_interrupt = 1: drc_straight decides block end): CLKR(15,15,7,15,11,5,2)
		if (d->reg == 1 || d->reg > 3) return 0;
		drc_get_rm(d, 1, rT1);
		if (d->mod == 3) drc_clk(2);
		else drc_clk_parity(rT6, CK(15, 7), CK(11, 5));
		e_sh(rT1, OFSR(d->reg == 0 ? DS1 : d->reg == 2 ? SS_ : DS0), rS0);
		if (d->reg == 2) e_sll(rFP, rT1, 4);
		if (d->reg == 3) e_sll(rS7, rT1, 4);
		e_li(rAT, 1);
		e_sb(rAT, OFS(no_interrupt), rS0);
		return 1;
	case 0x90: drc_clk(3); return 1;
	case 0x98: // CBW
		drc_lb8(rT0, AL); e_seb(rT0, rT0); e_srl(rT0, rT0, 8); drc_sb8(rT0, AH); drc_clk(2); return 1;
	case 0x99: // CWD
		drc_lb8(rT0, AH); e_seb(rT0, rT0); e_sra(rT0, rT0, 8); drc_sw16(rT0, DW); drc_clk(4); return 1;
	case 0xa0: case 0xa1: case 0xa2: case 0xa3: { // MOV AL/AW <-> [disp]
		const UINT32 addr = drc_w(d, d->p);
		e_li(rT7, addr);
		drc_segaddr(rT6, DS0, rT7, d);
		// CLKS(10,10,5) / CLKW(14,14,7,14,10,5) / CLKS(9,9,3) / CLKW(13,13,5,13,9,3)
		if (op == 0xa0) { drc_rb(rT1, rT6); drc_sb8(rT1, AL); drc_clk(CK(10, 5)); }
		else if (op == 0xa1) { drc_rw(rT1, rT6); drc_sw16(rT1, AW); drc_clk((addr & 1) ? CK(14, 7) : CK(10, 5)); }
		else if (op == 0xa2) { drc_lb8(rT1, AL); drc_wb(rT1, rT6); drc_clk(CK(9, 3)); }
		else { drc_lw16(rT1, AW); drc_ww(rT1, rT6); drc_clk((addr & 1) ? CK(13, 5) : CK(9, 3)); }
		return 1;
	}
	case 0xa8: drc_lb8(rT0, AL); e_li(rT1, drc_b(d, d->p)); drc_alu(4, 0); drc_clk(CK(4, 2)); return 1;
	case 0xa9: drc_lw16(rT0, AW); e_li(rT1, drc_w(d, d->p)); drc_alu(4, 1); drc_clk(CK(4, 2)); return 1;
	case 0xc6: // MOV r/m8, imm8
		e_li(rT1, drc_b(d, drc_after_modrm(d)));
		drc_put_rm(d, 0, rT1);
		drc_clk(d->mod == 3 ? 4 : 11);
		return 1;
	case 0xc7:
		e_li(rT1, drc_w(d, drc_after_modrm(d)));
		drc_put_rm(d, 1, rT1);
		drc_clk(d->mod == 3 ? 4 : 15);
		return 1;
	case 0xf5: drc_cf_to(rT0); e_xori(rT0, rT0, 1); e_sw(rT0, OFS(CarryVal), rS0); drc_clk(2); return 1; // CMC
	case 0xf8: e_sw(rZERO, OFS(CarryVal), rS0); drc_clk(2); return 1;
	case 0xf9: e_li(rT0, 1); e_sw(rT0, OFS(CarryVal), rS0); drc_clk(2); return 1;
	case 0xfc: e_sb(rZERO, OFS(DF), rS0); drc_clk(2); return 1;
	case 0xfd: e_li(rT0, 1); e_sb(rT0, OFS(DF), rS0); drc_clk(2); return 1;
	case 0xfe: // INC/DEC r/m8: CLKM(2,2,2,16,16,7)
		if (d->reg > 1) return 0;
		drc_get_rm(d, 0, rT0);
		drc_incdec(d->reg, 0);
		drc_put_rm_back(d, 0, rT0);
		drc_clk(d->mod == 3 ? 2 : CK(16, 7));
		return 1;
	case 0xff:
		if (d->reg <= 1) { // CLKM(2,2,2,24,16,7)
			drc_get_rm(d, 1, rT0);
			drc_incdec(d->reg, 1);
			drc_put_rm_back(d, 1, rT0);
			drc_clk(d->mod == 3 ? 2 : CK(16, 7));
			return 1;
		}
		break; // (CALL/JMP/PUSH below)
	}
	if (op >= 0x40 && op <= 0x4f) { // INC/DEC reg16
		const int r = drc_wreg[op & 7];
		drc_lw16(rT0, r);
		drc_incdec(op >= 0x48, 1);
		drc_sw16(rT0, r);
		drc_clk(2);
		return 1;
	}
	if (op >= 0x91 && op <= 0x97) { // XCHG AW, reg
		const int r = drc_wreg[op & 7];
		drc_lw16(rT0, r); drc_lw16(rT1, AW); drc_sw16(rT1, r); drc_sw16(rT0, AW);
		drc_clk(3);
		return 1;
	}
	if (op >= 0xb0 && op <= 0xb7) { e_li(rT0, drc_b(d, d->p)); drc_sb8(rT0, drc_breg[op & 7]); drc_clk(CK(4, 2)); return 1; }
	if (op >= 0xb8 && op <= 0xbf) { e_li(rT0, drc_w(d, d->p)); drc_sw16(rT0, drc_wreg[op & 7]); drc_clk(CK(4, 2)); return 1; }
	if (op >= 0x50 && op <= 0x57) { // PUSH reg (PUSH SP pushes the decremented value): CLKS(12,8,3)
		if (op == 0x54) { drc_lw16(rT0, SP); e_addiu(rT0, rT0, -2); e_andi(rT0, rT0, 0xffff); }
		else drc_lw16(rT0, drc_wreg[op & 7]);
		drc_push(rT0);
		drc_clk(CK(8, 3));
		return 1;
	}
	if (op >= 0x58 && op <= 0x5f) { // POP reg: CLKS(12,8,5)
		drc_pop(rT0);
		drc_sw16(rT0, drc_wreg[op & 7]);
		drc_clk(CK(8, 5));
		return 1;
	}
	if (op >= 0x70 && op <= 0x7f) { // Jcc: flushes prefetch -> taken goes to a block exit. V33 always 3 cycles, V30 10 if taken else 4
		if (d->npre) return 0;
		const int pol = drc_cond(op & 15, rT0);
		if (!drc_v30)
			drc_clk(3);
		else { // (JMP macro: when taken subtracts only table[1] = 10 and returns, else CLKS(4,4,3))
			drc_clk(4);
			if (pol) drc_clk_alt(rT0, 6);
			else { e_sltiu(rT1, rT0, 1); drc_clk_alt(rT1, 6); }
		}
		drc_prefetch(2, 1);
		dc.pf_done = 1;
		UINT32 *br = pol ? e_bne(rT0, rZERO) : e_beq(rT0, rZERO);
		e_nop();
		drc_exit_branch_link(br, (dc.ipn + (INT8)drc_b(d, d->p)) & 0xffff, dc.pc);
		return 1;
	}
	if (op >= 0xe0 && op <= 0xe3) { // LOOPNE LOOPE LOOP JCXZ: 6 if taken, else 3 (prefetch not flushed)
		if (d->npre) return 0;
		drc_lw16(rT1, CW);
		if (op != 0xe3) {
			e_addiu(rT1, rT1, -1);
			e_andi(rT1, rT1, 0xffff);
			drc_sw16(rT1, CW);
			e_sltu(rT0, rZERO, rT1);                       // CW != 0
			if (op != 0xe2) {
				drc_lz_direct(LZB_SZP);
				e_lw(rAT, OFS(ZeroVal), rS0);
				e_sltiu(rAT, rAT, 1);                     // ZF
				if (op == 0xe0) e_xori(rAT, rAT, 1);      // !ZF
				e_and(rT0, rT0, rAT);
			}
		} else
			e_sltiu(rT0, rT1, 1);                          // CW == 0
		// not taken CLKS(5,5,3), taken LOOPNE/LOOPE CLKS(14,14,6), LOOP/JCXZ CLKS(13,13,6)
		drc_clk(CK(5, 3));
		drc_clk_alt(rT0, CK(op <= 0xe1 ? 9 : 8, 3));
		drc_prefetch(2, 0);
		dc.pf_done = 1;
		UINT32 *br = e_bne(rT0, rZERO);
		e_nop();
		drc_exit_branch_link(br, (dc.ipn + (INT8)drc_b(d, d->p)) & 0xffff, dc.pc);
		return 1;
	}
	switch (op) {
	case 0x06: case 0x0e: case 0x16: case 0x1e: // PUSH sreg: CLKS(12,8,3)
		e_lhu(rT0, OFSR(op == 0x06 ? DS1 : op == 0x0e ? PS : op == 0x16 ? SS_ : DS0), rS0);
		drc_push(rT0);
		drc_clk(CK(8, 3));
		return 1;
	case 0x07: case 0x1f: // POP es/ds: CLKS(12,8,5)
		drc_pop(rT0);
		e_sh(rT0, OFSR(op == 0x07 ? DS1 : DS0), rS0);
		if (op == 0x1f) e_sll(rS7, rT0, 4);
		drc_clk(CK(8, 5));
		return 1;
	case 0x68: // PUSH imm16: CLKW(12,12,5,12,8,5,SP) -> V33 5, V30 odd 12 even 8 (SP after push)
		e_li(rT0, drc_w(d, d->p));
		drc_push(rT0);
		if (drc_v30) drc_clk_parity(rT4, 12, 8); else drc_clk(5);
		return 1;
	case 0x6a: // PUSH imm8 (sign-extended): CLKW(11,11,5,11,7,3,SP) -> odd 5 even 3 (SP after push)
		e_li(rT0, (UINT16)(INT16)(INT8)drc_b(d, d->p));
		drc_push(rT0);
		drc_clk_parity(rT4, CK(11, 5), CK(7, 3));
		return 1;
	case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
		const int w = op & 1;
		if (d->rep) { // REP MOVS/STOS: CLK(2), CW times (each with the same cycles as interpreter i_movs*/i_stos*), CW = 0
			drc_clk(2);
			drc_str_delta(rT3, w);
			if (!dc.dreg) { e_move(rT8, rZERO); dc.dreg = 1; }
			dc.dalt = -1;
			drc_lw16(rT2, CW);
			UINT32 *skip = e_beq(rT2, rZERO);
			e_nop();
			UINT32 *loop = drc_nomerge = drc_ptr;
			if (op == 0xa4 || op == 0xa5) {
				drc_lw16(rT7, IX); drc_segaddr(rT6, DS0, rT7, d);
				if (w) drc_rw(rT1, rT6); else drc_rb(rT1, rT6);
				drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
				if (w) drc_ww(rT1, rT6); else drc_wb(rT1, rT6);
				drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IY);
				drc_lw16(rT0, IX); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IX);
				const int c = w ? CK(16, 10) : CK(8, 6); // CLKS(16,16,10) / CLKS(8,8,6)
				e_addiu(rS1, rS1, -c);
				e_addiu(rT8, rT8, c);
			} else {
				drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
				if (w) { drc_lw16(rT1, AW); drc_ww(rT1, rT6); } else { drc_lb8(rT1, AL); drc_wb(rT1, rT6); }
				drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); e_andi(rT0, rT0, 0xffff); drc_sw16(rT0, IY);
				if (w) { // CLKW(8,8,5,8,4,3,IY): V33 odd 5 even 3, V30 odd 8 even 4 (IY after increment)
					e_andi(rAT, rT0, 1);
					e_sll(rAT, rAT, drc_v30 ? 2 : 1);
					e_addiu(rAT, rAT, CK(4, 3));
					e_subu(rS1, rS1, rAT);
					e_addu(rT8, rT8, rAT);
				} else { // CLKS(4,4,3)
					e_addiu(rS1, rS1, -CK(4, 3));
					e_addiu(rT8, rT8, CK(4, 3));
				}
			}
			e_addiu(rT2, rT2, -1);
			UINT32 *br = e_bgtz(rT2);
			e_nop();
			e_patch(br, loop);
			drc_sw16(rZERO, CW);
			e_patch(skip, drc_ptr);
			return 1;
		}
		drc_str_delta(rT3, w);
		if (op == 0xa4 || op == 0xa5) { // MOVS: [DS0:IX] → [DS1:IY]
			drc_lw16(rT7, IX); drc_segaddr(rT6, DS0, rT7, d);
			if (w) drc_rw(rT1, rT6); else drc_rb(rT1, rT6);
			drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
			if (w) drc_ww(rT1, rT6); else drc_wb(rT1, rT6);
			drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IY);
			drc_lw16(rT0, IX); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IX);
			drc_clk(w ? CK(16, 10) : CK(8, 6));
		} else if (op == 0xa6 || op == 0xa7) { // CMPS: src = [DS1:IY], dst = [DS0:IX]
			drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
			if (w) drc_rw(rT1, rT6); else drc_rb(rT1, rT6);
			drc_lw16(rT7, IX); drc_segaddr(rT6, DS0, rT7, d);
			if (w) drc_rw(rT0, rT6); else drc_rb(rT0, rT6);
			drc_alu(5, w);
			drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IY);
			drc_lw16(rT0, IX); e_addu(rT0, rT0, rT3); drc_sw16(rT0, IX);
			drc_clk(14);
		} else if (op == 0xaa || op == 0xab) { // STOS
			drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
			if (w) { drc_lw16(rT1, AW); drc_ww(rT1, rT6); } else { drc_lb8(rT1, AL); drc_wb(rT1, rT6); }
			drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); e_andi(rT0, rT0, 0xffff); drc_sw16(rT0, IY);
			if (w) drc_clk_parity(rT0, CK(8, 5), CK(4, 3)); else drc_clk(CK(4, 3)); // CLKW(8,8,5,8,4,3) / CLKS(4,4,3)
		} else if (op == 0xac || op == 0xad) { // LODS
			drc_lw16(rT7, IX); drc_segaddr(rT6, DS0, rT7, d);
			if (w) { drc_rw(rT1, rT6); drc_sw16(rT1, AW); } else { drc_rb(rT1, rT6); drc_sb8(rT1, AL); }
			drc_lw16(rT0, IX); e_addu(rT0, rT0, rT3); e_andi(rT0, rT0, 0xffff); drc_sw16(rT0, IX);
			if (w) drc_clk_parity(rT0, CK(8, 5), CK(4, 3)); else drc_clk(CK(4, 3));
		} else { // SCAS: src = [DS1:IY], dst = AL/AW
			drc_lw16(rT7, IY); drc_segaddr(rT6, DS1, rT7, d);
			if (w) { drc_rw(rT1, rT6); drc_lw16(rT0, AW); } else { drc_rb(rT1, rT6); drc_lb8(rT0, AL); }
			drc_alu(5, w);
			drc_lw16(rT0, IY); e_addu(rT0, rT0, rT3); e_andi(rT0, rT0, 0xffff); drc_sw16(rT0, IY);
			if (w) drc_clk_parity(rT0, CK(8, 5), CK(4, 3)); else drc_clk(CK(4, 3));
		}
		return 1;
	}
	case 0xe4: case 0xe5: case 0xec: case 0xed: { // IN AL/AW, port (imm8 / DW)
		if (d->npre) return 0;
		const int w = op & 1, dx = op >= 0xec;
#ifndef REI_DRC_V25
		{ // pure ports whose value the driver shadows (changes only between lines - rei_nec_port_shadow) read the shadow without a handler (same value)
			extern UINT8 *rei_nec_port_shadow;
			extern UINT32 rei_nec_pure_ports;
			const UINT32 p = drc_b(d, d->p), bits = w ? 3u : 1u;
			if (!dx && rei_nec_port_shadow && p + w < 8 && ((rei_nec_pure_ports >> p) & bits) == bits) {
				const UINT32 a = (UINT32)(uintptr_t)(rei_nec_port_shadow + p);
				e_lui(rAT, (int)((a + 0x8000) >> 16));
				e_lbu(rV0, (int)(INT16)(a & 0xffff), rAT);
				if (w) {
					e_lbu(rV1, (int)(INT16)((a & 0xffff) + 1), rAT);
					e_sll(rV1, rV1, 8);
					e_or(rV0, rV0, rV1);
					drc_sw16(rV0, AW);
					drc_clk((p & 1) ? CK(13, 7) : CK(9, 5));
				} else {
					drc_sb8(rV0, AL);
					drc_clk(CK(9, 5));
				}
				return 1;
			}
		}
#endif
		drc_mem_begin();
		if (dx) drc_lw16(rT0, DW); else e_li(rT0, drc_b(d, d->p));
		e_jal(H_in[w]);
		e_move(rA0, rT0);
		if (w) drc_sw16(rV0, AW); else drc_sb8(rV0, AL);
		// IN AL,imm CLKS(9,9,5), IN AL,DW CLKS(8,8,5), IN AW,imm CLKW(13,13,7,13,9,5), IN AW,DW CLKW(12,12,7,12,8,5)
		if (!w) drc_clk(dx ? CK(8, 5) : CK(9, 5));
		else if (dx) drc_clk_parity(rT0, CK(12, 7), CK(8, 5));
		else drc_clk((drc_b(d, d->p) & 1) ? CK(13, 7) : CK(9, 5));
		return 1;
	}
	case 0xe6: case 0xe7: case 0xee: case 0xef: { // OUT port, AL/AW
		if (d->npre) return 0;
		const int w = op & 1, dx = op >= 0xee;
		drc_mem_begin();
		if (dx) drc_lw16(rT0, DW); else e_li(rT0, drc_b(d, d->p));
		if (w) drc_lw16(rT1, AW); else drc_lb8(rT1, AL);
		e_move(rA0, rT0);
		e_jal(H_out[w]);
		e_move(rA1, rT1);
		// OUT imm/DW,AL CLKS(8,8,3), OUT imm,AW CLKW(12,12,5,12,8,3), OUT DW,AW CLKW(12,12,5,12,8,3)
		if (!w) drc_clk(CK(8, 3));
		else if (dx) drc_clk_parity(rT0, CK(12, 5), CK(8, 3));
		else drc_clk((drc_b(d, d->p) & 1) ? CK(12, 5) : CK(8, 3));
		return 1;
	}
	case 0x8f: // POP r/m: POP then PutRMWord (EA computed after), 21
		drc_pop(rT1);
		drc_put_rm(d, 1, rT1);
		drc_clk(21);
		return 1;
	case 0xd7: // TRANS
		drc_lw16(rT7, BW); drc_lb8(rAT, AL); e_addu(rT7, rT7, rAT); e_andi(rT7, rT7, 0xffff);
		drc_segaddr(rT6, DS0, rT7, d);
		drc_rb(rT1, rT6);
		drc_sb8(rT1, AL);
		drc_clk(CK(9, 5)); // CLKS(9,9,5)
		return 1;
	case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
		drc_shift(d, op & 1);
		return 1;
	case 0xc4: case 0xc5: // LES/LDS: CLKW(26,26,14,26,18,10)
		if (d->mod == 3) return 0;
		drc_get_rm(d, 1, rT1);
		drc_put_reg(d, 1, rT1);
		drc_next_rm_word(rT2);
		e_sh(rT2, OFSR(op == 0xc4 ? DS1 : DS0), rS0);
		if (op == 0xc5) e_sll(rS7, rT2, 4);
		drc_clk_parity(rT6, CK(26, 14), CK(18, 10));
		return 1;
	case 0xf6: case 0xf7: {
		const int w = op & 1;
		switch (d->reg) {
		case 0: // TEST imm (C/O/S/Z/P, A unchanged)
			drc_lz_drop(LZB_O | LZB_SZP);
			drc_get_rm(d, w, rT0);
			e_li(rT1, w ? drc_w(d, drc_after_modrm(d)) : drc_b(d, drc_after_modrm(d)));
			e_and(rT0, rT0, rT1);
			e_sw(rZERO, OFS(CarryVal), rS0);
			e_sw(rZERO, OFS(OverVal), rS0);
			drc_szp(rT0, w);
			drc_clk(d->mod == 3 ? 4 : 11);
			return 1;
		case 2: // NOT
			drc_get_rm(d, w, rT0);
			e_nor(rT0, rT0, rZERO);
			drc_put_rm_back(d, w, rT0);
			drc_clk(d->mod == 3 ? 2 : 16);
			return 1;
		case 3: // NEG: CarryVal = (tmp != 0), tmp = ~tmp + 1 (O/A unchanged)
			drc_lz_drop(LZB_SZP);
			drc_get_rm(d, w, rT0);
			e_sltu(rAT, rZERO, rT0);
			e_sw(rAT, OFS(CarryVal), rS0);
			e_subu(rT0, rZERO, rT0);
			drc_szp(rT0, w);
			drc_put_rm_back(d, w, rT0);
			drc_clk(d->mod == 3 ? 2 : 16);
			return 1;
		case 4: case 5: // MULU / MUL
			drc_lz_direct(LZB_O);
			drc_get_rm(d, w, rT0);
			if (!w) {
				drc_lb8(rT1, AL);
				if (d->reg == 5) { e_seb(rT1, rT1); e_seb(rT0, rT0); }
				e_mult(rT1, rT0);
				e_mflo(rT1);
				drc_sw16(rT1, AW);
				e_srl(rAT, rT1, 8); e_andi(rAT, rAT, 0xff); // AH
			} else {
				drc_lw16(rT1, AW);
				if (d->reg == 5) { e_seh(rT1, rT1); e_seh(rT0, rT0); }
				e_mult(rT1, rT0);
				e_mflo(rT1);
				drc_sw16(rT1, AW);
				e_srl(rAT, rT1, 16);
				drc_sw16(rAT, DW);
				e_andi(rAT, rAT, 0xffff); // DW
			}
			e_sltu(rAT, rZERO, rAT);
			e_sw(rAT, OFS(CarryVal), rS0);
			e_sw(rAT, OFS(OverVal), rS0);
			drc_clk(d->mod == 3 ? 30 : 36);
			return 1;
		}
		return 0;
	}
	case 0x0f: { // test1/clr1/set1/not1 (0f 10..1f)
		const UINT8 op2 = drc_b(d, d->p);
		if (op2 < 0x10 || op2 > 0x1f) return 0;
		struct drc_dec dd = *d;
		dd.p = d->p + 1;
		dd.m = drc_b(d, dd.p);
		dd.mod = dd.m >> 6; dd.reg = (dd.m >> 3) & 7; dd.rm = dd.m & 7;
		const int w = op2 & 1;
		drc_get_rm(&dd, w, rT0);
		// V33 4. V30: 10/11 3, 12/13 5, 14..17 4, 18/19 4, 1a/1b 6, 1c..1f 5 (CLKS(.., v30, 4))
		static const UINT8 v30c[16] = { 3, 3, 5, 5, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5, 5, 5 };
		drc_clk(CK(v30c[op2 & 15], 4));
		if (op2 >= 0x18) e_li(rT1, drc_b(d, drc_after_modrm(&dd)) & (w ? 0xf : 0x7));
		else { drc_lb8(rT1, CL); e_andi(rT1, rT1, w ? 0xf : 0x7); }
		e_li(rT2, 1);
		e_sllv(rT2, rT2, rT1); // 1 << tmp2
		switch ((op2 >> 1) & 3) {
		case 0: // TEST1: ZeroVal = bit ? 1 : 0, CarryVal = OverVal = 0
			drc_lz_direct(LZB_SZP | LZB_O);
			e_and(rAT, rT0, rT2); e_sltu(rAT, rZERO, rAT); e_sw(rAT, OFS(ZeroVal), rS0);
			e_sw(rZERO, OFS(CarryVal), rS0); e_sw(rZERO, OFS(OverVal), rS0);
			return 1;
		case 1: e_nor(rT2, rT2, rZERO); e_and(rT0, rT0, rT2); break; // CLR1
		case 2: e_or(rT0, rT0, rT2); break;                          // SET1
		case 3: e_xor(rT0, rT0, rT2); break;                         // NOT1
		}
		drc_put_rm_back(&dd, w, rT0);
		return 1;
	}
	// Block-ending control transfers (ip to memory, prefetch flushed)
	case 0xeb: // JMP short: 12, prefetch not flushed
		if (d->npre) return 0;
		drc_clk(12);
		drc_set_ip_const(dc.ipn + (INT8)drc_b(d, d->p));
		*end = 1;
		return 1;
	case 0xe9:
		if (d->npre) return 0;
		drc_clk(15);
		drc_set_ip_const(dc.ipn + (INT16)drc_w(d, d->p));
		*end = 2; // prefetch flushed
		return 1;
	case 0xe8: // CALL near
		if (d->npre) return 0;
		e_li(rT0, dc.ipn);
		drc_push(rT0);
		drc_set_ip_const(dc.ipn + (INT16)drc_w(d, d->p));
		drc_clk(24);
		*end = 2;
		return 1;
	case 0xc3: case 0xc2: // RET / RET imm
		if (d->npre) return 0;
		drc_pop(rT0);
		drc_set_ip_reg(rT0);
		if (op == 0xc2) { drc_lw16(rT1, SP); e_addiu(rT1, rT1, (int)drc_w(d, d->p)); drc_sw16(rT1, SP); }
		drc_clk(op == 0xc2 ? CK(24, 10) : CK(19, 10)); // CLKS(24,24,10) / CLKS(19,19,10)
		*end = 2;
		return 1;
	case 0xcb: case 0xca: // RETF / RETF imm
		if (d->npre) return 0;
		drc_pop(rT0);
		drc_pop(rT1);
		e_sh(rT1, OFSR(PS), rS0);
		drc_set_ip_reg(rT0); // (slow path writes ip as the next instruction, so do this after all reads)
		if (op == 0xca) { drc_lw16(rT1, SP); e_addiu(rT1, rT1, (int)drc_w(d, d->p)); drc_sw16(rT1, SP); }
		drc_clk(op == 0xca ? CK(32, 16) : CK(29, 16)); // CLKS(32,32,16) / CLKS(29,29,16)
		*end = 2;
		return 1;
	case 0x9a: // CALL far: PUSH(PS), PUSH(ip), CLKW(...,SP) odd 13 even 9
		if (d->npre) return 0;
		e_lhu(rT0, OFSR(PS), rS0);
		drc_push(rT0);
		e_li(rT0, dc.ipn);
		drc_push(rT0);
		drc_set_ip_const(drc_w(d, d->p));
		drc_end_target = -1; // (PS changes)
		e_li(rT0, drc_w(d, d->p + 2));
		e_sh(rT0, OFSR(PS), rS0);
		drc_clk_parity(rT4, CK(29, 13), CK(21, 9)); // CLKW(29,29,13,29,21,9,SP)
		*end = 2;
		return 1;
	case 0xea: // JMP far
		if (d->npre) return 0;
		e_li(rT0, drc_w(d, d->p + 2));
		e_sh(rT0, OFSR(PS), rS0);
		drc_set_ip_const(drc_w(d, d->p));
		drc_end_target = -1; // (PS changes)
		drc_clk(27);
		*end = 2;
		return 1;
	case 0xff:
		switch (d->reg) {
		case 2: // CALL near r/m
			drc_get_rm(d, 1, rT1);
			e_li(rT0, dc.ipn);
			drc_push(rT0);
			drc_set_ip_reg(rT1);
			drc_clk(d->mod == 3 ? 16 : 20);
			*end = 2;
			return 1;
		case 3: // CALL far m
			if (d->mod == 3) return 0;
			drc_get_rm(d, 1, rT1);
			e_lhu(rT0, OFSR(PS), rS0);
			drc_next_rm_word(rT2);
			e_sh(rT2, OFSR(PS), rS0);
			drc_push(rT0);
			e_li(rT0, dc.ipn);
			drc_push(rT0);
			drc_set_ip_reg(rT1);
			drc_clk(26);
			*end = 2;
			return 1;
		case 4: // JMP near r/m
			drc_get_rm(d, 1, rT1);
			drc_set_ip_reg(rT1);
			drc_clk(13);
			*end = 2;
			return 1;
		case 5: // JMP far m
			if (d->mod == 3) return 0;
			drc_get_rm(d, 1, rT1);
			drc_next_rm_word(rT2);
			e_sh(rT2, OFSR(PS), rS0);
			drc_set_ip_reg(rT1);
			drc_clk(15);
			*end = 2;
			return 1;
		case 6: // PUSH r/m
			drc_get_rm(d, 1, rT0);
			drc_push(rT0);
			drc_clk(4);
			return 1;
		}
		return 0;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Block compilation

// Exit code (after the block body)
static void drc_emit_stubs(void)
{
	for (int i = 0; i < drc_nstubs; i++) {
		struct drc_stub *s = &drc_stubs[i];
		e_patch(s->br, drc_ptr);
		switch (s->kind) {
		case DST_EXIT:
			if (s->s2k >= 0) e_li(rS2, (UINT32)s->s2k);
			e_li(rAT, s->ip);
			e_li(rV0, s->pc);
			e_j(H_exit_ip);
			e_nop();
			break;
		case DST_LINK:
			drc_emit_link(s->ip, s->pc, s->s2k);
			break;
		case DST_GEXIT: // exit after an interpreter step (ip, icount, prefetch already in memory and registers)
			e_li(rV0, s->pc);
			e_j(H_epi);
			e_nop();
			break;
		}
	}
}

static UINT32 rei_drc_idle_pc = 0xffffffffu; // set by the dispatcher (idle loop address applies only to a block's first instruction)

static void drc_cache_sync(UINT32 *from, UINT32 *to)
{
#ifdef REI_PROFILE
	const uint64_t t0 = rei_prof_clock();
#else
	const unsigned t0 = rei_drc_clock32 ? rei_drc_clock32() : 0;
#endif
	if (rei_drc_cache_flush)
		rei_drc_cache_flush(from, (int)((to - from) * 4));
#ifndef __psp__
	else
		__builtin___clear_cache((char *)from, (char *)to);
#endif
#ifdef REI_PROFILE
	rei_drc_stats[11] += rei_prof_clock() - t0;
#else
	if (rei_drc_clock32)
		rei_drc_stats[11] += rei_drc_clock32() - t0;
#endif
}

static int drc_decode(UINT32 pc, struct drc_dec *d)
{
	memset(d, 0, sizeof(*d));
	d->pc = pc;
	d->seg = -1;
	d->len = drc_insn_len(pc, &d->op, &d->npre);
	if (d->len <= 0)
		return 0;
	int nseg = 0, nrep = 0;
	for (int i = 0; i < d->npre; i++) {
		const UINT8 b = drc_opbyte(pc + i);
		if (b == 0x26) { d->seg = DS1; nseg++; }
		else if (b == 0x2e) { d->seg = PS; nseg++; }
		else if (b == 0x36) { d->seg = SS_; nseg++; }
		else if (b == 0x3e) { d->seg = DS0; nseg++; }
		else nrep++;
	}
	d->p = d->npre + 1;
	if (drc_is_modrm_op(d->op)) {
		d->m = drc_byte(pc + d->p);
		d->mod = d->m >> 6;
		d->reg = (d->m >> 3) & 7;
		d->rm = d->m & 7;
	}
	if (nrep == 1 && nseg <= 1) { // REP(E/NE) MOVS/STOS become a native loop (repeat condition is CW only)
		for (int i = 0; i < d->npre; i++) {
			const UINT8 b = drc_opbyte(pc + i);
			if (b == 0xf2 || b == 0xf3) d->rep = 1;
		}
		if (d->rep && (d->op == 0xa4 || d->op == 0xa5 || d->op == 0xaa || d->op == 0xab))
			return 1;
	}
	return (nseg <= 1 && nrep == 0) ? 1 : 2; // 2 = not compiled natively (multiple prefixes, REP)
}

// (ReiREM) If the cache keeps overflowing and getting flushed (4 times in 600 frames), fall back to the interpreter for this game - endless
// flush/rebuild was slower than interpreting (PSP kengo: V35 blocks exceed the cache, 44 flushes in 1500 frames, 43-55ms vs 25ms interp). Same results
static void drc_thrash_check(void)
{
	static int ov_n;
	static UINT32 ov_f0;
	if ((UINT32)(nCurrentFrame - ov_f0) > 600) { ov_f0 = nCurrentFrame; ov_n = 0; }
	if (++ov_n >= 4) {
		rei_drc_enabled = 0;
		bprintf(PRINT_IMPORTANT, _T("drc: code cache keeps filling up, using the interpreter\n"));
	}
}

#if !defined(__psp__)
// (diag) REI_DRC_PROF=1: insert an execution counter before each block body; at the end count executed MIPS instructions by class (main body path only,
// shared routines by call count). Harness only - counter code changes hash and speed
#define DRC_PROF_MAX 65536
static UINT32 drc_prof_cnt[DRC_PROF_MAX], drc_prof_pc[DRC_PROF_MAX], drc_prof_n;
static UINT32 *drc_prof_s[DRC_PROF_MAX], *drc_prof_e[DRC_PROF_MAX];
static UINT16 drc_prof_ni[DRC_PROF_MAX];
static void drc_prof_dump(void);
static UINT16 drc_prof_fl[DRC_PROF_MAX][6];
static UINT8 drc_prof_nfl[DRC_PROF_MAX];
static void drc_prof_flush_note(int why)
{
	if (drc_prof_cur < 0 || drc_prof_nfl[drc_prof_cur] >= 6) return;
	drc_prof_fl[drc_prof_cur][drc_prof_nfl[drc_prof_cur]++] = (UINT16)(why | drc_cur_op);
}
static int drc_prof_begin(UINT32 pc0)
{
	static int on = -1;
	drc_prof_cur = -1;
	if (on < 0) { on = getenv("REI_DRC_PROF") != NULL; if (on) atexit(drc_prof_dump); }
	if (!on || drc_prof_n >= DRC_PROF_MAX) return -1;
	const int k = (int)drc_prof_n++;
	drc_prof_cur = k;
	const UINT32 a = (UINT32)(uintptr_t)&drc_prof_cnt[k];
	drc_prof_pc[k] = pc0;
	e_lui(rAT, (int)(((a + 0x8000) >> 16) & 0xffff));
	e_lw(rV1, (int)(INT16)(a & 0xffff), rAT);
	e_addiu(rV1, rV1, 1);
	e_sw(rV1, (int)(INT16)(a & 0xffff), rAT);
	drc_prof_s[k] = drc_ptr;
	return k;
}
static void drc_prof_tick(void) // REI_DRC_PROF_FROM=frame: zero counters at that frame (to exclude boot test loops)
{
	static int from = -2;
	if (from == -2) { const char *f = getenv("REI_DRC_PROF_FROM"); from = f ? atoi(f) : -1; }
	if (from >= 0 && (int)nCurrentFrame >= from) {
		memset(drc_prof_cnt, 0, sizeof(drc_prof_cnt));
		from = -1;
	}
}
static void drc_prof_main_end(int k)
{
	if (k < 0) return; // (cache flush overwrites code, so keep a copy)
	const size_t n = (size_t)(drc_ptr - drc_prof_s[k]);
	UINT32 *cp = (UINT32 *)malloc(n * 4 + 4);
	memcpy(cp, drc_prof_s[k], n * 4);
	drc_prof_s[k] = cp;
	drc_prof_e[k] = cp + n;
}
static void drc_prof_dump(void)
{
	enum { C_IC, C_CHK, C_LD, C_ST, C_JAL, C_BR, C_PF, C_NOP, C_OTH, C_HLP, C_N };
	static const char *nm[C_N] = { "icount addiu", "bltzal", "load s0", "store s0", "jal", "branch", "prefetch s2", "nop", "other", "helper body" };
	double tot[C_N] = { 0 }, all = 0, insn = 0;
	static double oth[512];
	// by jal target
	static UINT32 jt[4096]; static double jc[4096]; int njt = 0;
	for (UINT32 k = 0; k < drc_prof_n; k++) {
		static const int stat_w = getenv("REI_DRC_PROF_STATIC") != NULL; // (static: 1 per executed block - code size)
		const double c = stat_w ? (drc_prof_cnt[k] ? 1.0 : 0.0) : drc_prof_cnt[k];
		if (!c || !drc_prof_e[k]) continue;
		for (UINT32 *w = drc_prof_s[k]; w < drc_prof_e[k]; w++) {
			const UINT32 x = *w, op = x >> 26, rs = (x >> 21) & 31, rt = (x >> 16) & 31;
			int cl = C_OTH;
			if (op == 0x09 && rs == rS1 && rt == rS1) cl = C_IC;
			else if (op == 0x01 && rs == rS1 && rt == 0x10) cl = C_CHK;
			else if ((op == 0x20 || op == 0x21 || op == 0x23 || op == 0x24 || op == 0x25) && rs == rS0) cl = C_LD;
			else if ((op == 0x28 || op == 0x29 || op == 0x2b) && rs == rS0) cl = C_ST;
			else if (op == 0x03) {
				cl = C_JAL;
				const UINT32 t = x & 0x3ffffff;
				int i = 0;
				while (i < njt && jt[i] != t) i++;
				if (i == njt && njt < 4096) jt[njt++] = t;
				if (i < 4096) jc[i] += c;
			} else if ((op >= 0x04 && op <= 0x07) || op == 0x01 || op == 0x02) cl = C_BR;
			else if (x == 0) cl = C_NOP;
			else if (op == 0x0c && rt == rS2 && rs == rAT && (x & 0xffff) == 7) { cl = C_PF; tot[C_OTH] -= 3 * c; tot[C_PF] += 3 * c; }
			else if ((op == 0 && ((x >> 11) & 31) == rS2) || ((op == 0x09 || op == 0x0d || op == 0x0f) && rt == rS2)) cl = C_PF;
			tot[cl] += c;
			all += c;
			if (cl == C_OTH) { // (other words: SPECIAL as fn, SPECIAL3 as 0x40|fn, rest as 0x80|op)
				const int key = op == 0x09 ? 0x100 | (int)rt : op == 0x0c ? 0x120 | (int)rt : op == 0 ? (int)(x & 63) : op == 0x1f ? 0x40 | (int)(x & 63) : 0x80 | (int)op;
				oth[key] += c;
				if (op == 0 && (x & 63) == 0x21 && ((x >> 16) & 31) == 0) oth[0x3f] += c, oth[0x21] -= c; // move
			}
		}
	}
	for (UINT32 k = 0; k < drc_prof_n; k++) insn += (double)drc_prof_cnt[k];
	{ // lazy flag eval sites (opcode, unknown = may be left by a previous block) x block executions
		static double fc[512];
		for (UINT32 k = 0; k < drc_prof_n; k++)
			for (int j = 0; j < drc_prof_nfl[k]; j++) fc[drc_prof_fl[k][j] & 511] += drc_prof_cnt[k];
		for (int r = 0; r < 14; r++) {
			int bi = -1;
			for (int i = 0; i < 512; i++) if (fc[i] > 0 && (bi < 0 || fc[i] > fc[bi])) bi = i;
			if (bi < 0) break;
			fprintf(stderr, "drcprof flush op %02x%s %12.0f\n", bi & 255, bi & 256 ? " (unknown)" : "", fc[bi]);
			fc[bi] = 0;
		}
	}
	for (int r = 0; r < 24; r++) {
		int bi = -1;
		for (int i = 0; i < 512; i++) if (oth[i] > 0 && (bi < 0 || oth[i] > oth[bi])) bi = i;
		if (bi < 0) break;
		fprintf(stderr, "drcprof other %s %02x %12.0f\n", bi >= 0x120 ? "andi rt" : bi >= 0x100 ? "addiu rt" : bi & 0x80 ? "op" : bi & 0x40 ? "sp3" : bi == 0x3f ? "move" : "fn", bi >= 0x100 ? (bi & 0x1f) : (bi & 0x3f), oth[bi]);
		oth[bi] = 0;
	}
	fprintf(stderr, "drcprof blocks %u entries %.0f mips-words %.0f\n", drc_prof_n, insn, all);
	for (int i = 0; i < C_HLP; i++) fprintf(stderr, "drcprof %-14s %12.0f %5.1f%%\n", nm[i], tot[i], 100 * tot[i] / (all ? all : 1));
	const struct { UINT32 **h; int n; const char *name; } hs[] = {
		{ &H_rb, 1, "rb" }, { &H_rw, 1, "rw" }, { &H_wb, 1, "wb" }, { &H_ww, 1, "ww" }, { H_add, 2, "add" }, { H_adc, 2, "adc" },
		{ H_sub, 2, "sub" }, { H_sbb, 2, "sbb" }, { H_log, 2, "log" }, { H_inc, 2, "inc" }, { H_dec, 2, "dec" }, { H_szp, 2, "szp" },
		{ H_in, 2, "in" }, { H_out, 2, "out" }, { &H_pro, 1, "pro" } };
	for (int i = 0; i < njt; i++) { // shared routine fast-path length (up to the first jr and its delay slot) x call count
		const UINT32 *h = (const UINT32 *)(uintptr_t)(((UINT32)(uintptr_t)drc_buf & 0xf0000000u) | (jt[i] << 2));
		int len = 0;
		while (len < 64 && (h[len] & 0xfc1fffffu) != 0x00000008u) len++;
		tot[C_HLP] += (double)(len + 2) * jc[i];
	}
	fprintf(stderr, "drcprof %-14s %12.0f (outside body)\n", nm[C_HLP], tot[C_HLP]);
	for (int i = 0; i < njt; i++) {
		const char *name = "?";
		for (unsigned h = 0; h < sizeof(hs) / sizeof(hs[0]); h++)
			for (int j = 0; j < hs[h].n; j++)
				if (((((UINT32)(uintptr_t)hs[h].h[j]) >> 2) & 0x3ffffff) == jt[i]) name = hs[h].name;
		const UINT32 *h = (const UINT32 *)(uintptr_t)(((UINT32)(uintptr_t)drc_buf & 0xf0000000u) | (jt[i] << 2));
		int len = 0;
		while (len < 64 && (h[len] & 0xfc1fffffu) != 0x00000008u) len++;
		fprintf(stderr, "drcprof jal %-4s %12.0f len %d words %.0f\n", name, jc[i], len + 2, jc[i] * (len + 2));
	}
	// hottest blocks (count x words)
	for (int r = 0; r < 12; r++) {
		double best = 0; int bk = -1;
		for (UINT32 k = 0; k < drc_prof_n; k++) {
			if (!drc_prof_e[k]) continue;
			const double v = (double)drc_prof_cnt[k] * (drc_prof_e[k] - drc_prof_s[k]);
			if (v > best && drc_prof_ni[k] != 0xffff) { best = v; bk = (int)k; }
		}
		if (bk < 0) break;
		fprintf(stderr, "drcprof top pc %05x cnt %u words %d (%.1f%%)\n", drc_prof_pc[bk], drc_prof_cnt[bk], (int)(drc_prof_e[bk] - drc_prof_s[bk]), 100 * best / (all ? all : 1));
		drc_prof_ni[bk] = 0xffff;
	}
}
#endif

#ifdef DRC_LAZY
// Instructions compilable with pending flag eval: those that leave lazy flags (fully redefine O/A/S/Z/P) and those that don't touch
// O/A/S/Z/P. Others evaluate before the instruction (misclassification is caught by drc_lz_direct and falls back to an interpreter step)
static int drc_lz_safe(const struct drc_dec *d)
{
	const UINT8 op = d->op;
	if (op < 0x40) return (op & 7) < 6 || op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e || op == 0x07 || op == 0x17 || op == 0x1f;
	if (op <= 0x5f) return 1;                        // INC DEC PUSH POP
	if (op >= 0x70 && op <= 0x7f) return drc_lz_cond_ok(op & 15);
	if (op >= 0x80 && op <= 0x8f) return 1;          // ALU imm, TEST, XCHG, MOV, LEA, POP r/m
	if (op >= 0x90 && op <= 0x99) return 1;          // XCHG, CBW, CWD
	if (op >= 0xa0 && op <= 0xbf) return 1;          // MOV moffs, string ops (CMPS/SCAS leave SUB), TEST imm, MOV imm
	switch (op) {
	case 0x9a: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xc6: case 0xc7: case 0xca: case 0xcb: case 0xd7:
	case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xe8: case 0xe9: case 0xea: case 0xeb:
	case 0xec: case 0xed: case 0xee: case 0xef: case 0xf5: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd:
		return 1;                                    // (CMC/CLC/STC touch only CarryVal: H_mat preserves it)
	case 0xc0: case 0xc1: return 1;                  // counted rotates touch only CarryVal, shifts drop S/Z/P
	case 0xd2: case 0xd3: return d->reg <= 3 || d->reg == 6 || !(lz_mask & LZB_SZP); // (CL shifts may run 0 times)
	case 0xd0: case 0xd1: return d->reg >= 4 || !(lz_mask & LZB_O); // shifts drop O/S/Z/P, rotates write O
	case 0xf6: case 0xf7: return d->reg == 0 || d->reg == 2 || d->reg == 3 || ((d->reg == 4 || d->reg == 5) && !(lz_mask & LZB_O)); // TEST NOT NEG MUL
	case 0xe0: case 0xe1: return !(lz_mask & LZB_SZP); // LOOPNE LOOPE
	case 0xfe: return d->reg <= 1;                   // INC DEC
	case 0xff: return d->reg <= 6;                   // INC DEC CALL JMP PUSH
	}
	return 0;
}
#endif

#if !defined(REI_DRC_V25)
// (diag) REI_DRC_INTERP_STAT=1 (PSP: frontend sets rei_drc_interp_stat via host0:/rei_interpstat): reasons and 4KB pages of instructions
// the dispatcher ran in the interpreter, plus loop heads being recorded for auto idle (every 600 frames)
#if defined(__psp__)
extern "C" { int rei_drc_interp_stat = 0; }
#else
extern "C" { int rei_drc_interp_stat = -1; } // (-1: check the environment variable on first call)
#endif
static void drc_interp_note(nec_state_t *nec_state, int shadow, UINT32 pc)
{
	static int last;
	int &on = rei_drc_interp_stat;
	static unsigned long long why[8], pg[256];
#if !defined(__psp__)
	if (on < 0) on = getenv("REI_DRC_INTERP_STAT") != NULL;
#endif
	if (on <= 0) return;
	int w;
	if (shadow) w = 0;
	else if (rei_rec) w = 1;
	else if (nec_state->prefetch_reset) w = 2;
	else {
		const UINT32 key = ((UINT32)Sreg(PS) << 16) | nec_state->ip;
		struct drc_block *b = drc_find(key, nec_state->prefetch_count, rei_nec_pg_op[pc >> 9]);
		if (!b) w = 3;
		else if (b->fn) w = 7;
		else if (!rei_nec_pg_op[pc >> 9]) w = 4;
		else if (rei_nec_pg_wr[pc >> 9]) w = 5;
		else w = 6;
	}
	why[w]++;
	if (w >= 4) pg[(pc >> 12) & 255]++;
	static UINT32 hh[64]; static unsigned long long hc[64];
	if (w == 1) { // (per recorded loop head)
		const UINT32 h = rei_auto[0].head;
		int i = 0;
		while (i < 64 && hh[i] && hh[i] != h) i++;
		if (i < 64) { hh[i] = h; hc[i]++; }
	}
	if ((int)nCurrentFrame - last >= 600) {
		last = nCurrentFrame;
		fprintf(stderr, "interp f%d: shadow %llu rec %llu pfreset %llu noblock %llu noop %llu ram %llu other %llu fn %llu |", (int)nCurrentFrame,
			why[0], why[1], why[2], why[3], why[4], why[5], why[6], why[7]);
		for (int r = 0; r < 6; r++) {
			int bi = -1;
			for (int i = 0; i < 256; i++) if (pg[i] && (bi < 0 || pg[i] > pg[bi])) bi = i;
			if (bi < 0) break;
			fprintf(stderr, " %02x000:%llu", bi, pg[bi]);
			pg[bi] = 0;
		}
		static unsigned long long sl0, c0, p0;
		fprintf(stderr, " | slices %llu chkpages %llu (%llu pages) | rec heads", (unsigned long long)rei_nec_slice - sl0, drc_chk_calls - c0, drc_chk_pages - p0);
		sl0 = rei_nec_slice; c0 = drc_chk_calls; p0 = drc_chk_pages;
		for (int r = 0; r < 8; r++) {
			int bi = -1;
			for (int i = 0; i < 64; i++) if (hc[i] && (bi < 0 || hc[i] > hc[bi])) bi = i;
			if (bi < 0) break;
			const int k = (hh[bi] ^ (hh[bi] >> 6)) & (REI_AUTO_BAD - 1);
			fprintf(stderr, " %05x:%llu(bad %d)", hh[bi], hc[bi], rei_auto[0].bad_head[k] == hh[bi] ? rei_auto[0].bad_n[k] : -1);
			hc[bi] = 0;
		}
		memset(hh, 0, sizeof(hh));
		fputc(10, stderr);
		memset(why, 0, sizeof(why));
		memset(pg, 0, sizeof(pg));
	}
}
#endif

#if !defined(REI_DRC_V25)
// (diag, rei_drc_interp_stat) why the dispatcher entered a block: previous block left via a linkable exit (not linked), block is on a frequently
// modified page, or other (previous block left via H_epi/icount/jump cache miss, fragment start). Top entered block addresses (every 600 frames)
static void drc_entry_note(struct drc_block *b, int linkable, UINT32 pc)
{
	static int last;
	static unsigned long long n[4];
	static UINT32 ph[256]; static unsigned long long pcn[256];
	const int vol = drc_page_vol[b->pg0] || drc_page_vol[b->pg1];
	n[vol ? 2 : linkable ? 1 : 0]++;
	{
		const UINT32 h = (pc * 2654435761u) >> 24;
		if (ph[h] == pc || !pcn[h]) { ph[h] = pc; pcn[h]++; }
	}
	if ((int)nCurrentFrame - last >= 600) {
		last = nCurrentFrame;
		fprintf(stderr, "entry f%d: plain %llu linkable-failed %llu volatile %llu |", (int)nCurrentFrame, n[0], n[1], n[2]);
		for (int r = 0; r < 10; r++) {
			int bi = -1;
			for (int i = 0; i < 256; i++) if (pcn[i] && (bi < 0 || pcn[i] > pcn[bi])) bi = i;
			if (bi < 0) break;
			fprintf(stderr, " %05x:%llu", ph[bi], pcn[bi]);
			pcn[bi] = 0;
		}
		fputc(10, stderr);
		memset(n, 0, sizeof(n)); memset(pcn, 0, sizeof(pcn));
	}
}
#endif

static struct drc_block *drc_compile(nec_state_t *nec_state, UINT32 key, int cnt)
{
	const UINT32 ps = key >> 16, ip0 = key & 0xffff;
	const UINT32 pc0 = ((ps << 4) + ip0) & 0xfffff;
#if !defined(__psp__)
	if (getenv("REI_DRC_KEYSTAT")) { // (diag) compile counts: how many distinct linear addresses / keys / key+prefetch counts (every 64th compile)
		static UINT8 seen_pc[1 << 17], seen_key[1 << 20], seen_kc[1 << 20];
		static unsigned n, npc, nkey, nkc, nps[16];
		const UINT32 hk = (key * 2654435761u) >> 12, hkc = ((key ^ (cnt << 28)) * 2654435761u) >> 12;
		if (!(seen_pc[pc0 >> 3] & (1 << (pc0 & 7)))) { seen_pc[pc0 >> 3] |= 1 << (pc0 & 7); npc++; }
		if (!(seen_key[hk >> 3] & (1 << (hk & 7)))) { seen_key[hk >> 3] |= 1 << (hk & 7); nkey++; }
		if (!(seen_kc[hkc >> 3] & (1 << (hkc & 7)))) { seen_kc[hkc >> 3] |= 1 << (hkc & 7); nkc++; }
		nps[cnt & 15]++;
		if ((++n & 4095) == 0)
			fprintf(stderr, "keystat f%d compiles %u pcs %u keys %u key+cnt %u, cnt: %u %u %u %u %u %u %u\n", (int)nCurrentFrame, n, npc, nkey, nkc,
				nps[0], nps[1], nps[2], nps[3], nps[4], nps[5], nps[6]);
	}
#endif
	if (drc_end - drc_ptr < 4096 && !drc_next_seg()) { // (in words: a size no single block exceeds, 16KB)
		DRC_WHY(0);
		const int grew = drc_grow_seg1(); // (once, on first overflow: enlarge the second segment - boards with large game code, dsoccr94 3MB)
		drc_flush_all();
		if (!grew)
			drc_thrash_check();
	}
	struct drc_block *b = drc_insert(key, cnt);
	b->pc = pc0;
	b->pg0 = (UINT16)(pc0 >> 9);
	b->host0 = rei_nec_pg_op[pc0 >> 9];
	// Don't compile RAM (where a write page exists) or areas without an instruction page
	if (!b->host0 || rei_nec_pg_wr[pc0 >> 9] || nec_state->fetch_xor)
		return b;

	drc_overflow = 0;
	drc_nstubs = 0;
	const int xt0 = drc_xt_n;
	UINT32 *iexits[DRC_MAX_INSNS];
	int niexit = 0;
	pf_set = 1 << cnt;
	s2_ok = 1; // (on entry s2 = memory value = cnt)
	UINT32 *start = drc_ptr;
	drc_blk_start = start;
	drc_ds_slot = NULL;
#ifdef DRC_LAZY
	lz_mask = LZB_ALL; // (a chained-in block doesn't know what the previous block left pending)
	lz_known = 0;
#endif
	e_addiu(rSP, rSP, -DRC_FRAME);
	e_sw(rRA, FR_RA, rSP);
	e_jal(H_pro);
	e_nop();
#if !defined(__psp__)
	const int prof_k = drc_prof_begin(pc0);
#endif

	int n = 0;
	UINT32 ip = ip0, lastpc = pc0;
	int ip_stored = 0; // last instruction already stored ip to memory
	int last_end = 0, last_native = 0, last_shadow = 0; // last instruction: control transfer, native, sets no_interrupt
	for (;;) {
		const UINT32 pc = ((ps << 4) + ip) & 0xfffff;
		if (n > 0 && pc == rei_drc_idle_pc) break;
		struct drc_dec d;
		const unsigned tq0 = rei_drc_clock_fine ? rei_drc_clock_fine() : 0;
		const int dk = drc_decode(pc, &d);
		if (!dk || ip + d.len > 0x10000) break;
		const UINT32 last = ((ps << 4) + ip + d.len - 1) & 0xfffff;
		const UINT32 pg1 = (b->pg0 + 1) & 0x7ff;
		if (((pc >> 9) != b->pg0 && ((pc >> 9) != pg1 || !rei_nec_pg_op[pc >> 9] || rei_nec_pg_wr[pc >> 9])) ||
		    ((last >> 9) != b->pg0 && ((last >> 9) != pg1 || !rei_nec_pg_op[last >> 9] || rei_nec_pg_wr[last >> 9])))
			break; // at most two pages
		const int straight = drc_straight(pc, d.op, d.npre);
		const unsigned tq1 = rei_drc_clock_fine ? rei_drc_clock_fine() : 0;
		if (rei_drc_clock_fine) rei_drc_tph[0] += tq1 - tq0;

		memset(&dc, 0, sizeof(dc));
		dc.ip = ip;
		dc.ipn = (ip + d.len) & 0xffff;
		dc.pc = pc;
#if !defined(__psp__)
		drc_cur_op = d.op;
#endif
		if (last_shadow && last_native && n > 0) // (previous instruction is an in-block MOV sreg: the interpreter loop decrements no_interrupt before this instruction)
			e_sb(rZERO, OFS(no_interrupt), rS0);
#ifdef DRC_LAZY
		if (lz_mask && (dk != 1 || !drc_lz_safe(&d)))
			drc_lz_flush();
		const int lzm0 = lz_mask, lzn0 = lz_known, lzk0 = lz_kind, lzw0 = lz_w, lzp0 = lz_plain;
		lz_bad = 0;
#endif
		UINT32 *ins_start = drc_ptr;
		UINT32 *ds0 = drc_ds_slot;
		const int nst0 = drc_nstubs;
		const int pfs0 = pf_set, s2ok0 = s2_ok;
		int end = 0, native = 0;
		drc_end_target = -1;
		if (dk == 1) {
			if (d.seg >= 0) { // prefix: set prefix_base and CLK(2) first (interpreter i_es etc.)
				e_lhu(rAT, OFSR(d.seg), rS0);
				e_sll(rAT, rAT, 4);
				e_sw(rAT, OFS(prefix_base), rS0);
				drc_clk(2);
			}
			native = drc_emit_insn(&d, &end);
#ifdef DRC_LAZY
			if (lz_bad) native = 0; // (misclassified: use an interpreter step)
#endif
		}
		if (native) {
			if (!dc.pf_done)
				drc_prefetch(d.len, end == 2); // (end 2: control transfer flushed prefetch)
			if (!end && straight) { // like the interpreter loop, check icount > 0 before the next instruction (if s1 < 0, go to block-end j H_iexit)
				if (drc_xt_n >= drc_xt_cap || niexit >= DRC_MAX_INSNS) {
					drc_overflow = 1;
				} else {
					iexits[niexit++] = drc_ptr;
					I_(0x01, rS1, 0x10, 0); // bltzal s1 (delay slot gets the next instruction's first pure op)
					drc_ds_slot = drc_ptr;
					drc_xt_ra[drc_xt_n] = (UINT32)(uintptr_t)(drc_ptr + 1);
					const int k = pf_single() >= 0 && !s2_ok ? pf_single() : -1;
					drc_xt_info[drc_xt_n++] = (dc.ip & 0xffff) | ((UINT32)d.len << 16) | ((UINT32)(k + 1) << 20);
				}
			}
			ip_stored = end ? 1 : 0;
		} else {
			// Interpreter step: discard code/exits so far (including prefix handling) and call the handler directly
			drc_ptr = ins_start;
			drc_ds_slot = ds0;
			drc_nstubs = nst0;
			pf_set = pfs0;
			s2_ok = s2ok0;
#ifdef DRC_LAZY
			lz_mask = lzm0; lz_known = lzn0; lz_kind = lzk0; lz_w = lzw0; lz_plain = lzp0;
			drc_lz_flush(); // (the interpreter knows nothing of lazy flags)
#endif
			drc_pf_sync();
			e_addiu(rAT, rS1, 1);
			e_sw(rAT, OFS(icount), rS0);
			e_sb(rS2, OFS(prefetch_count), rS0);
			e_li(rAT, ip);
			e_sh(rAT, OFS(ip), rS0);
			e_move(rA0, rS0);
			e_call((const void *)drc_gstep);
			e_lw(rS1, OFS(icount), rS0);
			e_addiu(rS1, rS1, -1);
			e_lb(rS2, OFS(prefetch_count), rS0);
			e_lhu(rAT, OFSR(DS0), rS0);
			e_sll(rS7, rAT, 4);
			e_lhu(rAT, OFSR(SS_), rS0);
			e_sll(rFP, rAT, 4);
			pf_set = 0x7f;
			s2_ok = 1;
			if (straight) {
				UINT32 *br = e_bne(rV0, rZERO);
				e_nop();
				struct drc_stub *s = drc_new_stub(DST_GEXIT);
				s->br = br; s->pc = pc;
			}
			ip_stored = 1;
		}
		if (rei_drc_clock_fine) rei_drc_tph[1] += rei_drc_clock_fine() - tq1;
		n++;
		lastpc = pc;
		ip += d.len;
		last_end = end;
		last_native = native;
		last_shadow = (d.op == 0x8e || d.op == 0x17 || d.op == 0xf0);
		if (end || !straight || n >= DRC_MAX_INSNS || drc_overflow)
			break;
	}
	if (n == 0) {
		drc_ptr = start;
		drc_ds_slot = NULL;
		drc_xt_n = xt0;
		return b;
	}
	const unsigned tq2 = rei_drc_clock_fine ? rei_drc_clock_fine() : 0;
	b->ninsn = (UINT16)n;
	b->pg1 = (UINT16)(lastpc >> 9);
	b->host1 = rei_nec_pg_op[lastpc >> 9];
	drc_note_page(b->pg0, b->host0);
	drc_note_page(b->pg1, b->host1);
	// End: store the next ip (already stored after an interpreter step or control transfer) and exit with the last instruction address
	const int link_ip = !ip_stored ? (int)(ip & 0xffff) : (last_end && last_native) ? drc_end_target : -1;
	if (link_ip >= 0 && pf_single() >= 0 && !last_shadow) {
		drc_emit_link((UINT32)link_ip, lastpc, pf_single());
	} else {
		if (ip_stored && last_end && last_native && drc_end_target < 0 && pf_single() == 0) {
			// runtime-determined target (RET, indirect, far jump; prefetch flushed to 0): jump cache (H_jc, else exit via H_epi)
			e_li(rV0, lastpc);
			e_j(H_jc);
			e_nop();
		} else {
		drc_pf_sync();
		e_li(rV0, lastpc);
		if (!ip_stored) {
			e_li(rAT, ip & 0xffff);
			e_j(H_exit_ip);
		} else
			e_j(H_epi);
		e_nop();
		}
	}
#if !defined(__psp__)
	drc_prof_main_end(prof_k);
#endif
	drc_ds_flush();
	drc_emit_stubs();
	drc_ds_flush();
	if (niexit) { // where icount exits land
		for (int i = 0; i < niexit; i++)
			e_patch(iexits[i], drc_ptr);
		e_j(H_iexit);
		e_nop();
	}
	if (drc_overflow) {
		DRC_WHY(2);
		drc_flush_all();
		drc_thrash_check();
		return NULL; // so it retries
	}
	if (rei_drc_clock_fine) rei_drc_tph[2] += rei_drc_clock_fine() - tq2;
	drc_cache_sync(start, drc_ptr);
	b->fn = (UINT32 (*)(nec_state_t *))start;
#if !defined(__psp__)
	{ // (diag) REI_DRC_DUMP=dir: dump block code as <pc>_<cnt>.bin (objdump -b binary -m mips:isa32r2 -EL)
		static const char *dump = getenv("REI_DRC_DUMP");
		static int dumped;
		static const char *dpcs = getenv("REI_DRC_DUMP_PCS"); // (comma-separated hex linear addresses: only those blocks, plus V33 bytes as .v33)
		char hx[8];
		snprintf(hx, sizeof(hx), "%05x", pc0);
		if (dump && (dpcs ? strstr(dpcs, hx) != NULL : dumped < 200)) {
			char path[256];
			snprintf(path, sizeof(path), "%s/%05x_%d.bin", dump, pc0, cnt);
			if (FILE *f = fopen(path, "wb")) {
				fwrite(start, 4, drc_ptr - start, f);
				fclose(f);
				dumped++;
			}
			snprintf(path, sizeof(path), "%s/%05x.v33", dump, pc0);
			if (FILE *f = fopen(path, "wb")) {
				for (UINT32 i = 0; i < 160; i++) fputc(drc_byte((pc0 + i) & 0xfffff), f);
				fclose(f);
			}
		}
	}
#endif
	rei_drc_stats[3]++;
	rei_drc_stats[7] += n;
#if !defined(__psp__)
	{ // (diag) same-site blocks differing only in prefetch count
		extern unsigned long long rei_drc_variants;
		static UINT8 seen[1 << 17]; // (PS:ip hash bitmap, approximate)
		const UINT32 h = (key * 2654435761u) >> 15;
		if (seen[h >> 3] & (1 << (h & 7))) rei_drc_variants++;
		seen[h >> 3] |= (UINT8)(1 << (h & 7));
	}
#endif
	{
		const unsigned long long used = drc_cur == 0 ? (unsigned long long)(drc_ptr - drc_buf) * 4 :
			(unsigned long long)(drc_segs[0].code_end - drc_buf + (drc_ptr - drc_segs[1].code_start)) * 4;
		if (used > rei_drc_stats[6])
			rei_drc_stats[6] = used;
	}
	return b;
}
