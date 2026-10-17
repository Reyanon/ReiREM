// V33 dynamic recompiler (MIPS32, ADR-020). Included by nec.cpp after the handlers and macros.
//
// Block: consecutive instructions within one PS compiled into one MIPS chunk. Entry happens after the dispatcher (nec_execute_drc) does the
// interpreter loop's instruction-boundary checks (interrupts, no_interrupt, idle-loop visit). Blocks are cut so the interpreter loop has
// nothing to do at in-block boundaries: after control transfers and IF/no_interrupt changes, before idle-loop addresses. After an insn that
// called a handler, exit on icount/stop_run/pending interrupt/no_interrupt/hlt/map change (drc_must_exit). Returns the linear address of the
// last executed insn (dispatcher auto_prev, idle-loop auto-detect).
//
// Stage 1: one interpreter step (drc_gstep) per insn. From stage 2, per-insn MIPS code (results must match the interpreter).
// (REI_DRC_TEST: on non-MIPS harness, compile only and execute via the interpreter, for compiler profiling)
#if defined(REI_DRC) && (defined(__mips__) || defined(REI_DRC_TEST))
#define REI_DRC_ACTIVE 1

#ifdef __psp__
#include <malloc.h>
#else
#include <sys/mman.h>
#endif

// (REI_DRC_V25: v25.cpp includes this again for V35; v25.cpp renames clashing globals to rei_drc35_*, while the code cache
//  sync function and the PSP fixed code region owner flag are shared)
#ifdef REI_DRC_V25
#define DRC_INSTANCE_ID 2
#else
#define DRC_INSTANCE_ID 1
#endif
extern "C" {
int rei_drc_enabled = 1;                         // harness --no-drc, frontend setting
#ifndef REI_DRC_V25
void (*rei_drc_cache_flush)(void *p, int len);   // platform: D-cache writeback + I-cache invalidate (default __builtin___clear_cache)
int rei_drc_fastdata_owner;                      // (PSP) instance using the fixed code region (1 NEC, 2 V35, 0 none)
unsigned (*rei_drc_clock32)(void);               // (diag, set by frontend) compile time us -> stat 10 (even without profile build)
unsigned rei_drc_tph[4];                         // (diag) compile phase us: 0 decode, 1 emit, 2 finish (exits, tables)
unsigned (*rei_drc_clock_fine)(void);           // (diag) per-insn phase time (one clock read ~1.3us inflates ~4us/insn: enable separately)
int rei_drc_idle_on;                               // idle-time compile (drc_idle_compile, enabled by frontend)
unsigned (*rei_drc_now_us)(void);                // clock used for it, us (frontend)
unsigned rei_drc_idle_n;                         // (diag) blocks built in idle time
unsigned (*rei_drc_blk_clock)(void);             // (diag, set by PSP dev build) time inside V33 blocks -> rei_drc_blk_acc (read twice per dispatch)
unsigned rei_drc_blk_acc;
#else
extern void (*rei_drc_cache_flush)(void *p, int len);
extern int rei_drc_fastdata_owner;
extern unsigned (*rei_drc_clock32)(void);
extern unsigned (*rei_drc_clock_fine)(void);
extern int rei_drc_idle_on;
extern unsigned (*rei_drc_now_us)(void);
// (Diag values are per instance: V35 compiling on the ME added into the V33 share (main .bss), and the ME wrote back stale values of the V33
//  insn page cache rei_fc_op/rei_fc_arg in the same cache line, killing main with a bus error; 2026-10-17 PSP nbbatman m92drc35, regression log 19)
#define rei_drc_tph    rei_drc35_tph
#define rei_drc_idle_n rei_drc35_idle_n
unsigned rei_drc_tph[4];
unsigned rei_drc_idle_n;
#endif
unsigned long long rei_drc_stats[13];             // 0 block entries, 1 block insns, 2 interpreted insns, 3 compiled blocks, 4 cache flushes, 5 invalidations (map),
                                                 // 6 code cache max bytes used, 7 compiled insns, 8 linked exits, 9 volatile pages,
                                                 // 10 compile time us, 11 cache sync us, 12 slow-path handler calls (10/11 need REI_PROFILE)
}

// ---------------------------------------------------------------------------
// MIPS32 instruction emitters

enum { rZERO = 0, rAT = 1, rV0 = 2, rV1 = 3, rA0 = 4, rA1 = 5, rA2 = 6, rA3 = 7, rT0 = 8, rT1 = 9, rT2 = 10, rT3 = 11, rT4 = 12, rT5 = 13,
       rT6 = 14, rT7 = 15, rS0 = 16, rS1 = 17, rS2 = 18, rS3 = 19, rS4 = 20, rS5 = 21, rS6 = 22, rS7 = 23, rT8 = 24, rT9 = 25, rSP = 29, rFP = 30,
       rRA = 31 };

// opcodes called straight from the interpreter (not emitted); harness REI_DRC_OFF="83,8b" disables them (for bisecting)
static UINT8 drc_native_off[256];

// Chip this dynarec runs: V33 (M92, M107) or V30 (M72, M81, M82, M84, M90, ADR-024). They differ in cycles (nec_drc_ops.inc CK) and
// prefetch (1 / 2 cycles per byte). Set by the dispatcher on entry (code cache is flushed on chip change)
static int drc_v30, drc_pfsh = 0; // drc_pfsh = log2(prefetch_cycles): V33 0, V30 1

static UINT32 *drc_buf, *drc_ptr, *drc_end;
static int drc_overflow; // cache filled while building this block

// Delay slot filling: put the next "side-effect free" insn (compute/load touching only temp regs) in the slot after bltzal (drc_ds_slot);
// if the branch is taken, it only clobbers disposable values. Otherwise nop
static UINT32 *drc_ds_slot;
static inline int drc_tmpreg(UINT32 r) { return (r >= 1 && r <= 15) || r == 24 || r == 25; }
static int drc_is_pure(UINT32 w)
{
	const UINT32 op = w >> 26, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
	if (w == 0) return 0;
	if (op == 0) // SPECIAL: arith/logic/shift/movz/movn (excluding jr, jalr, syscall, break, mult/div)
		return fn != 8 && fn != 9 && fn != 12 && fn != 13 && (fn < 0x10 || fn > 0x1b) && drc_tmpreg(rd);
	if (op == 0x1f) return drc_tmpreg((w & 63) == 0x20 ? rd : rt) && ((w & 63) == 0 || (w & 63) == 4 || (w & 63) == 0x20); // ext ins seb/seh
	if ((op >= 0x08 && op <= 0x0f) || (op >= 0x20 && op <= 0x25)) return drc_tmpreg(rt); // immediate compute/load
	return 0;
}
static __attribute__((noinline)) void E(UINT32 w) // (a call: inlining everywhere made compiling slower, PSP 12.8 -> 10.8us per insn)
{
	if (drc_ds_slot) {
		UINT32 *slot = drc_ds_slot;
		drc_ds_slot = NULL;
		if (slot == drc_ptr && !drc_is_pure(w) && drc_ptr < drc_end)
			*drc_ptr++ = 0; // (cannot fill: nop)
	}
	if (drc_ptr < drc_end)
		*drc_ptr++ = w;
	else
		drc_overflow = 1;
}
static inline void drc_ds_flush(void) // close the delay slot with a nop (before exit code patched later, etc.)
{
	if (drc_ds_slot) {
		UINT32 *slot = drc_ds_slot;
		drc_ds_slot = NULL;
		if (slot == drc_ptr) E(0);
	}
}
#define R_(rs, rt, rd, sa, fn) E(((UINT32)(rs) << 21) | ((UINT32)(rt) << 16) | ((UINT32)(rd) << 11) | ((UINT32)(sa) << 6) | (fn))
#define I_(op, rs, rt, imm) E(((UINT32)(op) << 26) | ((UINT32)(rs) << 21) | ((UINT32)(rt) << 16) | ((UINT32)(imm) & 0xffff))
static inline void e_addu(int d, int s, int t) { R_(s, t, d, 0, 0x21); }
static inline void e_subu(int d, int s, int t) { R_(s, t, d, 0, 0x23); }
static inline void e_and(int d, int s, int t) { R_(s, t, d, 0, 0x24); }
static inline void e_or(int d, int s, int t) { R_(s, t, d, 0, 0x25); }
static inline void e_xor(int d, int s, int t) { R_(s, t, d, 0, 0x26); }
static inline void e_nor(int d, int s, int t) { R_(s, t, d, 0, 0x27); }
static inline void e_slt(int d, int s, int t) { R_(s, t, d, 0, 0x2a); }
static inline void e_sltu(int d, int s, int t) { R_(s, t, d, 0, 0x2b); }
static inline void e_sll(int d, int t, int sa) { R_(0, t, d, sa, 0x00); }
static inline void e_srl(int d, int t, int sa) { R_(0, t, d, sa, 0x02); }
static inline void e_sra(int d, int t, int sa) { R_(0, t, d, sa, 0x03); }
static inline void e_sllv(int d, int t, int s) { R_(s, t, d, 0, 0x04); }
static inline void e_srlv(int d, int t, int s) { R_(s, t, d, 0, 0x06); }
static inline void e_srav(int d, int t, int s) { R_(s, t, d, 0, 0x07); }
static inline void e_jr(int s) { R_(s, 0, 0, 0, 0x08); }
static inline void e_jalr(int s) { R_(s, 0, rRA, 0, 0x09); }
static inline void e_movz(int d, int s, int t) { R_(s, t, d, 0, 0x0a); }
static inline void e_movn(int d, int s, int t) { R_(s, t, d, 0, 0x0b); }
static inline void e_mult(int s, int t) { R_(s, t, 0, 0, 0x18); }
static inline void e_multu(int s, int t) { R_(s, t, 0, 0, 0x19); }
static inline void e_mflo(int d) { R_(0, 0, d, 0, 0x12); }
static inline void e_mfhi(int d) { R_(0, 0, d, 0, 0x10); }
static inline void e_nop() { E(0); }
static inline void e_move(int d, int s) { e_addu(d, s, rZERO); }
static inline void e_addiu(int t, int s, int imm) { I_(0x09, s, t, imm); }
static inline void e_slti(int t, int s, int imm) { I_(0x0a, s, t, imm); }
static inline void e_sltiu(int t, int s, int imm) { I_(0x0b, s, t, imm); }
static inline void e_andi(int t, int s, int imm) { I_(0x0c, s, t, imm); }
static inline void e_ori(int t, int s, int imm) { I_(0x0d, s, t, imm); }
static inline void e_xori(int t, int s, int imm) { I_(0x0e, s, t, imm); }
static inline void e_lui(int t, int imm) { I_(0x0f, 0, t, imm); }
static inline void e_lb(int t, int off, int b) { I_(0x20, b, t, off); }
static inline void e_lh(int t, int off, int b) { I_(0x21, b, t, off); }
static inline void e_lw(int t, int off, int b) { I_(0x23, b, t, off); }
static inline void e_lbu(int t, int off, int b) { I_(0x24, b, t, off); }
static inline void e_lhu(int t, int off, int b) { I_(0x25, b, t, off); }
static inline void e_sb(int t, int off, int b) { I_(0x28, b, t, off); }
static inline void e_sh(int t, int off, int b) { I_(0x29, b, t, off); }
static inline void e_sw(int t, int off, int b) { I_(0x2b, b, t, off); }
// MIPS32r2 (Allegrex has these too)
static inline void e_ext(int t, int s, int pos, int size) { R_(s, t, size - 1, pos, 0x00) ; drc_ptr[-1] |= 0x1fu << 26; }
static inline void e_ins(int t, int s, int pos, int size) { R_(s, t, pos + size - 1, pos, 0x04); drc_ptr[-1] |= 0x1fu << 26; }
static inline void e_seb(int d, int t) { R_(0, t, d, 0x10, 0x20); drc_ptr[-1] |= 0x1fu << 26; }
static inline void e_seh(int d, int t) { R_(0, t, d, 0x18, 0x20); drc_ptr[-1] |= 0x1fu << 26; }

static inline void e_li(int t, UINT32 v)
{
	if ((INT32)v >= -32768 && (INT32)v < 32768)
		e_addiu(t, rZERO, (int)v);
	else if (v < 0x10000)
		e_ori(t, rZERO, (int)v);
	else {
		e_lui(t, (int)(v >> 16));
		if (v & 0xffff)
			e_ori(t, t, (int)(v & 0xffff));
	}
}

// Branches: remember the position, fill in the target later (same block, +-128KB)
static inline UINT32 *e_beq(int s, int t) { UINT32 *p = drc_ptr; I_(0x04, s, t, 0); return p; }
static inline UINT32 *e_bne(int s, int t) { UINT32 *p = drc_ptr; I_(0x05, s, t, 0); return p; }
static inline UINT32 *e_blez(int s) { UINT32 *p = drc_ptr; I_(0x06, s, 0, 0); return p; }
static inline UINT32 *e_bgtz(int s) { UINT32 *p = drc_ptr; I_(0x07, s, 0, 0); return p; }
static inline UINT32 *e_bltz(int s) { UINT32 *p = drc_ptr; I_(0x01, s, 0, 0); return p; }
static inline UINT32 *e_bgez(int s) { UINT32 *p = drc_ptr; I_(0x01, s, 1, 0); return p; }
static UINT32 *drc_nomerge;
static inline void e_patch(UINT32 *br, UINT32 *target)
{
	if (target == drc_ptr)
		drc_nomerge = drc_ptr; // (this spot is a branch target: do not merge with the previous insn)
	if (!br || drc_overflow || br >= drc_end)
		return;
	const INT32 d = (INT32)(target - (br + 1));
	*br = (*br & 0xffff0000u) | ((UINT32)d & 0xffff);
}

// C function calls (args already in a0..a3). jalr via t9: code cache and executable may not share a 256MB region
static inline void e_call(const void *fn)
{
	e_li(rT9, (UINT32)(uintptr_t)fn);
	e_jalr(rT9);
	e_nop();
}

// ---------------------------------------------------------------------------
// Code cache and block table

#ifdef REI_DRC_CACHE_KB
#define DRC_CACHE_BYTES (REI_DRC_CACHE_KB << 10)
#else
#define DRC_CACHE_BYTES (2 << 20)
#endif
#define DRC_HASH 8192            // block table (open addressing, power of two)
#define DRC_MAX_INSNS 48         // max insns per block
#define DRC_FRAME 104            // block stack frame: 0..15 arg slots, 16..51 t saves, 52 v0, 56..87 s0..s7, 88 ra, 92 shared routine ra, 96 fp

static int drc_cache_bytes = DRC_CACHE_BYTES;

struct drc_block {
	UINT32 key;          // PS << 16 | ip (0xffffffff = empty)
	UINT32 pc;           // linear address of first insn
	UINT32 (*fn)(nec_state_t *); // NULL = not compiled (RAM etc.): use the interpreter
	UINT8 *host0, *host1; // insn page pointers of first/last page (bank check)
	UINT16 pg0, pg1;
	UINT16 ninsn;
	UINT8 cnt;           // prefetch count at entry (part of the key: in-block prefetch is computed at compile time)
	UINT8 rbw;           // (V35) register bank (part of the key: register slots differ per bank). 0 for NEC
};
static int drc_rbw; // register bank of the block being compiled/looked up (V35 RBW, NEC 0)
static struct drc_block drc_tab0[DRC_HASH];
// (ReiREM) When growing the second code cache region, grow the table from the heap too (drc_grow_seg1): big-code games (dsoccr94, ~7000 blocks)
// filled the table to 3/4 with code space left and flushed everything, delaying the following frames by the compile budget
static struct drc_block *drc_tab = drc_tab0;
static UINT32 drc_hmask = DRC_HASH - 1;
static int drc_sreg_cont = 0; // (continue block after MOV sreg: halves dispatcher entries but overlapping blocks grow code ~9%, filling the code cache on dsoccr94 hardware. Enable with harness REI_DRC_SREGCONT)
static unsigned long long drc_chk_calls, drc_chk_pages; // (diag: printed by drc_interp_note)
static UINT32 *drc_last_stub;      // linkable exit the block left through, if any (used by H_exit_link)
#define DRC_JC 1024
static UINT32 drc_jc[DRC_JC][2];   // jump cache: runtime targets (PS << 16 | ip, prefetch 0) -> block body (read by block tail code)
static UINT8 *drc_page_host[2048]; // insn page pointer at the time blocks were compiled in that page (0 = no blocks)
static UINT8 drc_page_vol[2048];   // pages whose bank has changed: never link into their blocks (checked on every entry)
// List of pages holding blocks: only these are checked on map change. Scanning the full 2048-entry table (two pointers, 16KB = the whole PSP
// data cache) evicted the cache every time in scenes that switch banks often (nbbatman scene changes: hundreds per frame)
static UINT16 drc_code_pg[2048];
static UINT8 drc_code_pg_in[2048];
static int drc_code_pg_n;
static inline void drc_note_page(int pg, UINT8 *host)
{
	if (!drc_code_pg_in[pg]) {
		drc_code_pg_in[pg] = 1;
		drc_code_pg[drc_code_pg_n++] = (UINT16)pg;
	}
	drc_page_host[pg] = host;
}
static UINT32 *drc_xt_ra, *drc_xt_info; // icount exit table (nec_drc_ops.inc) of the current region
static int drc_xt_n, drc_xt_cap;
// Code cache regions (PSP: fast-path table region + second region from the heap). A block stays in one region; exit tables per region (address order)
struct drc_segment { UINT32 *code_start, *code_end, *xt_ra, *xt_info; int xt_n, xt_cap; };
static struct drc_segment drc_segs[2];
static int drc_nsegs, drc_cur;
static unsigned drc_gen; // bumped on every flush (whether an earlier block's exit address points to old code)
static void drc_load_seg(int i)
{
	drc_cur = i;
	drc_end = drc_segs[i].code_end;
	drc_xt_ra = drc_segs[i].xt_ra;
	drc_xt_info = drc_segs[i].xt_info;
	drc_xt_n = drc_segs[i].xt_n;
	drc_xt_cap = drc_segs[i].xt_cap;
}
static void drc_set_seg(int i, UINT32 *buf, int bytes) // last 1/8 is the exit table (8 bytes per exit)
{
	drc_segs[i].code_start = buf;
	drc_segs[i].xt_cap = bytes / 64;
	drc_segs[i].xt_ra = (UINT32 *)((char *)buf + bytes - drc_segs[i].xt_cap * 8);
	drc_segs[i].xt_info = drc_segs[i].xt_ra + drc_segs[i].xt_cap;
	drc_segs[i].code_end = drc_segs[i].xt_ra;
	drc_segs[i].xt_n = 0;
}
static int drc_next_seg(void) // switch to the second region (if present and still in the first)
{
	if (drc_cur != 0 || drc_nsegs < 2)
		return 0;
	drc_segs[0].xt_n = drc_xt_n;
	drc_load_seg(1);
	drc_ptr = drc_segs[1].code_start;
	return 1;
}
// (ReiREM) The first time the code cache fills, enlarge the second region once (not reserved up front for all games; most never overflow).
// A full flush follows, so code in the old region is unused. PSP tries REI_DRC_GROW_KB (default 4096) then halves; harness uses REI_DRC_GROW_KB
static int drc_grow_seg1(void)
{
	static int done;
	if (done) return 0;
#ifdef REI_DRC_V25
	if (rei_drc35_on_me) return 0; // (ME cannot allocate: just flush)
#endif
	done = 1;
	int kb = 0;
#if defined(__psp__)
#ifdef REI_DRC_GROW_KB
	kb = REI_DRC_GROW_KB;
#endif
#else
	if (const char *g = getenv("REI_DRC_GROW_KB")) kb = atoi(g);
#endif
	const int old_bytes = drc_nsegs >= 2 ? (int)((char *)drc_segs[1].xt_info + drc_segs[1].xt_cap * 4 - (char *)drc_segs[1].code_start) : 0;
	for (; kb >= 1024 && (kb << 10) > old_bytes; kb >>= 1) {
		const int bytes = kb << 10;
#if defined(__psp__)
		UINT32 *b = (UINT32 *)memalign(64, bytes);
#else
		void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		UINT32 *b = (p == MAP_FAILED) ? NULL : (UINT32 *)p;
#endif
		if (!b) continue;
		if (((uintptr_t)b >> 28) != ((uintptr_t)drc_buf >> 28) || ((uintptr_t)b >> 28) != ((uintptr_t)((char *)b + bytes - 1) >> 28)) {
#if defined(__psp__)
			free(b);
#else
			munmap(b, bytes);
#endif
			continue;
		}
#if defined(__psp__)
		if (drc_nsegs >= 2) free(drc_segs[1].code_start); // (PSP second region is memalign)
#endif
		drc_set_seg(1, b, bytes);
		drc_nsegs = 2;
#ifndef REI_DRC_V25
		if (drc_tab == drc_tab0) { // (full flush follows, so old table entries are dropped. Sized to code space: 2x for 4MB, 4x for 8MB)
			const int mul = kb >= 8192 ? 4 : 2;
			struct drc_block *t = (struct drc_block *)malloc(sizeof(struct drc_block) * DRC_HASH * mul);
			if (t) {
				drc_tab = t;
				drc_hmask = DRC_HASH * mul - 1;
			}
		}
#endif
		bprintf(PRINT_IMPORTANT, _T("drc: code cache second area grown to %d KB\n"), kb);
		return 1;
	}
	return 0;
}
static UINT32 *drc_code0; // where blocks go (after the shared routines at the front of the code cache, nec_drc_ops.inc)
static void drc_gen_helpers(void);
static void drc_pf_tab_init(void);
static void drc_cache_sync(UINT32 *from, UINT32 *to);
static int drc_tab_used;

#if !defined(__psp__)
extern "C" { unsigned rei_drc_flush_why[6]; unsigned long long rei_drc_variants, rei_drc_probes, rei_drc_finds; } // (diag) 0 code 1 table 2 overflow 3 page 4 idle address
#define DRC_WHY(i) (rei_drc_flush_why[i]++)
#elif !defined(REI_DRC_V25)
extern "C" { unsigned rei_drc_flush_why[6]; } // (PSP dev log: flush reasons in later stages)
#define DRC_WHY(i) (rei_drc_flush_why[i]++)
#else
#define DRC_WHY(i) ((void)0)
#endif
static void drc_flush_all(void)
{
	for (UINT32 i = 0; i <= drc_hmask; i++)
		drc_tab[i].key = 0xffffffffu;
	memset(drc_page_host, 0, sizeof(drc_page_host));
	memset(drc_code_pg_in, 0, sizeof(drc_code_pg_in));
	drc_code_pg_n = 0;
	for (int i = 0; i < DRC_JC; i++)
		drc_jc[i][0] = 0xffffffffu;
	drc_segs[0].xt_n = drc_segs[1].xt_n = 0;
	if (drc_nsegs)
		drc_load_seg(0);
	drc_tab_used = 0;
	drc_ptr = drc_code0 ? drc_code0 : drc_buf;
	drc_gen++;
	rei_drc_stats[4]++;
}

static int drc_init(void)
{
	if (drc_buf)
		return 1;
#if defined(__psp__) && defined(REI_DRC_FASTDATA)
	// Fixed region of the per-game fast-path table (psp/me.ld .reifast_data 576KB): empty in dynarec builds since hooks are unused
	if (rei_drc_fastdata_owner && rei_drc_fastdata_owner != DRC_INSTANCE_ID) { // another instance owns the fixed region: use the heap
		drc_cache_bytes = 0x80000;
		drc_buf = (UINT32 *)memalign(64, drc_cache_bytes);
	} else {
		rei_drc_fastdata_owner = DRC_INSTANCE_ID;
		extern char __reifast_start[];
		drc_buf = (UINT32 *)(void *)__reifast_start;
		drc_cache_bytes = 0x90000;
	}
#elif defined(__psp__)
	drc_buf = (UINT32 *)memalign(64, DRC_CACHE_BYTES);
#else
	if (const char *bg = getenv("REI_DRC_BUDGET")) // (experiment) compile budget per frame
		drc_budget_frame = drc_budget = atoi(bg);
	if (const char *kb = getenv("REI_DRC_KB")) // (diag) code cache size KB
		drc_cache_bytes = atoi(kb) << 10;
	void *p = mmap(NULL, drc_cache_bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	drc_buf = (p == MAP_FAILED) ? NULL : (UINT32 *)p;
#endif
#ifdef __psp__
	if (!rei_drc_cache_flush) { // frontend gave no cache function
		free(drc_buf);
		drc_buf = NULL;
	}
#endif
	if (!drc_buf) {
		rei_drc_enabled = 0;
		bprintf(PRINT_ERROR, _T("drc: no code cache, using the interpreter\n"));
		return 0;
	}
	drc_set_seg(0, drc_buf, drc_cache_bytes);
	drc_nsegs = 1;
	{ // second region (PSP: if heap has room; harness: test with REI_DRC_KB2)
		int bytes2 = 0;
		UINT32 *b2 = NULL;
#if defined(__psp__) && defined(REI_DRC_EXTRA_KB)
		bytes2 = REI_DRC_EXTRA_KB << 10;
		b2 = (UINT32 *)memalign(64, bytes2);
#elif !defined(__psp__)
		if (const char *kb2 = getenv("REI_DRC_KB2")) {
			bytes2 = atoi(kb2) << 10;
			void *p2 = mmap(NULL, bytes2, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			b2 = (p2 == MAP_FAILED) ? NULL : (UINT32 *)p2;
		}
#endif
		if (b2 && ((uintptr_t)b2 >> 28) == ((uintptr_t)drc_buf >> 28) && ((uintptr_t)b2 >> 28) == ((uintptr_t)((char *)b2 + bytes2 - 1) >> 28)) {
			drc_set_seg(1, b2, bytes2);
			drc_nsegs = 2;
		}
	}
	drc_load_seg(0);
#if !defined(__psp__)
	if (getenv("REI_DRC_SREGCONT")) drc_sreg_cont = 1;
	if (const char *off = getenv("REI_DRC_OFF")) { // (bisect) opcodes not to emit: "83,8b"
		for (const char *q = off; *q;) {
			char *e2;
			const long v = strtol(q, &e2, 16);
			if (e2 == q) break;
			if (v >= 0 && v < 256) drc_native_off[v] = 1;
			q = (*e2 == ',') ? e2 + 1 : e2;
		}
	}
#endif
	if (((uintptr_t)drc_buf >> 28) != ((uintptr_t)(drc_end - 1) >> 28)) { // jal cannot reach
		rei_drc_enabled = 0;
		bprintf(PRINT_ERROR, _T("drc: code cache crosses a 256MB boundary, using the interpreter\n"));
		return 0;
	}
	drc_pf_tab_init();
	drc_gen_helpers();
	drc_cache_sync(drc_buf, drc_code0);
	drc_flush_all();
	rei_drc_stats[4] = 0;
	return 1;
}

static inline UINT32 drc_hash(UINT32 key, int cnt) { return (key ^ (key >> 13) ^ (key >> 7) ^ ((UINT32)cnt * 0x9e5) ^ ((UINT32)drc_rbw * 0x3c1)) & drc_hmask; }

#ifndef REI_DRC_V25
REI_LINE_FN(18)
#endif
static struct drc_block *drc_find(UINT32 key, int cnt, const UINT8 *host0)
{
#if !defined(__psp__)
	rei_drc_finds++;
#endif
	for (UINT32 h = drc_hash(key, cnt), n = 0; n <= drc_hmask; h = (h + 1) & drc_hmask, n++) {
#if !defined(__psp__)
		rei_drc_probes++;
#endif
		if (drc_tab[h].key == key && drc_tab[h].cnt == cnt && drc_tab[h].host0 == host0 && drc_tab[h].rbw == drc_rbw)
			return &drc_tab[h];
		if (drc_tab[h].key == 0xffffffffu)
			return NULL;
	}
	return NULL;
}

static struct drc_block *drc_insert(UINT32 key, int cnt)
{
	if (drc_tab_used >= (int)(drc_hmask + 1) * 3 / 4) {
		DRC_WHY(1);
		drc_flush_all();
	}
	UINT32 h = drc_hash(key, cnt);
	while (drc_tab[h].key != 0xffffffffu)
		h = (h + 1) & drc_hmask;
	drc_tab_used++;
	struct drc_block *b = &drc_tab[h];
	memset(b, 0, sizeof(*b));
	b->key = key;
	b->cnt = (UINT8)cnt;
	b->rbw = (UINT8)drc_rbw;
	return b;
}

// ---------------------------------------------------------------------------
// Instruction length (incl. prefixes, bytes this core FETCHes). Same as tools/recomp/v33recomp.py insn_len_at

static inline UINT8 drc_byte(UINT32 a) { return rei_nec_op(a & 0xfffff); }
#ifdef REI_DRC_V25
// (V35) prefix and opcode bytes go through the decrypt table (interpreter fetchop); the rest (ModRM, disp, imm, byte after 0F) as is
static const UINT8 *drc_dec_tab;
static inline UINT8 drc_opbyte(UINT32 a) { const UINT8 b = drc_byte(a); return drc_dec_tab ? drc_dec_tab[b] : b; }
#else
#define drc_opbyte drc_byte
#endif

static int drc_modrm_len(UINT8 m) // incl. ModRM
{
	const int mod = m >> 6, rm = m & 7;
	if (mod == 3) return 1;
	if (mod == 0) return rm == 6 ? 3 : 1;
	return mod == 1 ? 2 : 3;
}

static int drc_is_modrm_op(UINT8 op)
{
	if (op < 0x40) return (op & 7) < 4;
	if (op == 0x62 || op == 0x69 || op == 0x6b) return 1;
	if (op >= 0x80 && op <= 0x8f) return 1;
	if (op == 0xc0 || op == 0xc1 || op == 0xc4 || op == 0xc5 || op == 0xc6 || op == 0xc7) return 1;
	if (op >= 0xd0 && op <= 0xd3) return 1;
	if (op >= 0xd8 && op <= 0xdf) return 1;
	return op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff;
}

// Insn length at pc, *opp = opcode after prefixes, *npre = prefix count. 0 if unknown
static int drc_insn_len(UINT32 pc, UINT8 *opp, int *npre)
{
	UINT32 i = pc;
	int n = 0;
	for (;;) {
		const UINT8 b = drc_opbyte(i);
		if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0xf2 || b == 0xf3 || b == 0x64 || b == 0x65) {
			i++;
			if (++n > 4) return 0;
		} else
			break;
	}
	const UINT8 op = drc_opbyte(i);
	*opp = op;
	*npre = n;
	int len = n + 1;
	if (op == 0x0f) {
		const UINT8 op2 = drc_byte(i + 1);
		len++;
		if ((op2 >= 0x10 && op2 <= 0x17) || op2 == 0x28 || op2 == 0x2a) len += drc_modrm_len(drc_byte(i + 2));
		else if (op2 >= 0x18 && op2 <= 0x1f) len += drc_modrm_len(drc_byte(i + 2)) + 1;
		else if (op2 == 0x31 || op2 == 0x33 || op2 == 0xe0 || op2 == 0xf0 || op2 == 0xff) len += 1;
		return len;
	}
	if (drc_is_modrm_op(op)) {
		const UINT8 m = drc_byte(i + 1);
		len += drc_modrm_len(m);
		const int reg = (m >> 3) & 7;
		if (op == 0x80 || op == 0x82 || op == 0x83 || op == 0xc0 || op == 0xc1 || op == 0xc6 || op == 0x6b) len += 1;
		else if (op == 0x81 || op == 0xc7 || op == 0x69) len += 2;
		else if (op == 0xf6 && reg <= 1) len += 1;
		else if (op == 0xf7 && reg <= 1) len += 2;
		return len;
	}
	switch (op) {
	case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c:
	case 0x6a: case 0xa8: case 0xcd: case 0xd4: case 0xd5: case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xeb:
		return len + 1;
	case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d:
	case 0x68: case 0xa9: case 0xc2: case 0xca: case 0xe8: case 0xe9: case 0xa0: case 0xa1: case 0xa2: case 0xa3:
		return len + 2;
	case 0x9a: case 0xea:
		return len + 4;
	case 0xc8:
		return len + 3;
	}
	if ((op >= 0x70 && op <= 0x7f) || (op >= 0xb0 && op <= 0xb7) || (op >= 0xe0 && op <= 0xe3)) return len + 1;
	if (op >= 0xb8 && op <= 0xbf) return len + 2;
	return len;
}

// May the block continue past this insn (sequential flow, interpreter loop has nothing to do at the boundary).
// Jcc/LOOP only when emitted (taken = block exit, not taken = continue)
static int drc_straight(UINT32 pc, UINT8 op, int npre)
{
	for (int i = 0; i < npre; i++) { // REP family: block end (emitted REP MOVS/STOS continue)
		const UINT8 p = drc_opbyte(pc + i);
		if (p == 0x64 || p == 0x65) return 0;
		if ((p == 0xf2 || p == 0xf3) && !((op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab) && !drc_native_off[op]))
			return 0;
	}
	if ((op >= 0x70 && op <= 0x7f) || (op >= 0xe0 && op <= 0xe3))
		return !npre && !drc_native_off[op];
	switch (op) {
	case 0x8e: // MOV sreg (no_interrupt): when emitted, continue the block; no interrupts are taken in-block anyway, and before the next insn
		// no_interrupt goes to 0 (interpreter loop decrement). An icount exit in between leaves with no_interrupt = 1 so the dispatcher sees the shadow
		return drc_sreg_cont && !npre && !drc_native_off[op];
	case 0x17: case 0xf0:                             // no_interrupt
	case 0x9a: case 0x9b: case 0x9d: case 0xc2: case 0xc3: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
	case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xf4: case 0xfa: case 0xfb:
	case 0x62:                                        // chkind (may raise an interrupt)
		return 0;
#ifdef REI_DRC_V25
	case 0x63: case 0xf1:                             // (V35) BRKN/BRKS: go to an interrupt and change mode (MF)
		return 0;
#endif
	case 0xf6: case 0xf7: // div only (divide-by-zero interrupt)
		return ((drc_byte(pc + npre + 1) >> 3) & 7) < 6;
	case 0x0f: { // test1/clr1/set1/not1 only
		const UINT8 op2 = drc_byte(pc + npre + 1);
		return op2 >= 0x10 && op2 <= 0x1f;
	}
	case 0xff: {
		const int r = (drc_byte(pc + npre + 1) >> 3) & 7;
		return r < 2 || r == 6;
	}
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Runtime helpers (called by generated code)

static inline int drc_must_exit(nec_state_t *s)
{
#ifdef REI_DRC_V25
	return s->icount <= 0 || s->stop_run || s->no_interrupt || s->halted ||
	       ((s->pending_irq & s->unmasked_irq) && ((s->pending_irq & NMI_IRQ) || s->IF)) || drc_map_dirty ||
	       rei_tim[s - sChips].dirty || drc_v25_idb != s->IDB || s->RBW != drc_rbw;
#else
	return s->icount <= 0 || s->stop_run || s->no_interrupt || s->halted ||
	       (s->pending_irq && ((s->pending_irq & NMI_IRQ) || s->IF)) || drc_map_dirty;
#endif
}

// One interpreter step (same as the insn execution part of the nec_execute loop body). Nonzero = leave the block
#if !defined(__psp__)
extern "C" { unsigned long long rei_drc_gstep_ops[256]; } // (diag) in-block interpreter steps per opcode
#endif
static int drc_gstep(nec_state_t *nec_state)
{
#if !defined(__psp__)
	{ // (skip prefixes to the opcode; opcodes after prefixes counted as is, without a +0x100 slot)
		UINT32 a = PC(nec_state) & 0xfffff;
		UINT8 o = rei_nec_op(a);
		for (int k = 0; k < 4 && (o == 0x26 || o == 0x2e || o == 0x36 || o == 0x3e || o == 0xf2 || o == 0xf3); k++)
			o = rei_nec_op(++a & 0xfffff);
		rei_drc_gstep_ops[o]++;
	}
#endif
	const int prev_ICount = nec_state->icount;
	rei_ins_ic = prev_ICount;
#if defined(REI_PROFILE) && !defined(REI_DRC_V25)
	rei_prof_ins++;
#endif
	nec_instruction[fetchop(nec_state)](nec_state);
	if (drc_pfsh == 0) rei_prefetch1(nec_state, prev_ICount);
	else do_prefetch(nec_state, prev_ICount, drc_pfsh);
	return drc_must_exit(nec_state);
}

#include "nec_drc_ops.inc"


// Idle-time compile: collect keys of blocks that ran interpreted after the frame budget ran out, and build them in the spare time before
// the frontend waits for vblank (drc_idle_compile). Splits scene-change bursts of 2000+ insns (~20ms) per frame. Results are identical
// (block or interpreter give the same result, same as the budget). Only builds when there is room without flushing the region
#define DRC_Q 128
static struct { nec_state_t *st; UINT32 key; UINT8 cnt, rbw; } drc_q[DRC_Q];
static int drc_qn;
static UINT32 drc_q_seen[256]; // (avoid re-queuing the same key: key ^ prefetch count + 1, 0 when empty)
static void drc_queue(nec_state_t *st, UINT32 key, int cnt)
{
	if (!rei_drc_idle_on || drc_qn >= DRC_Q)
		return;
	const UINT32 tag = (key ^ ((UINT32)cnt << 28) ^ ((UINT32)drc_rbw << 24)) + 1;
	UINT32 *seen = &drc_q_seen[(tag ^ (tag >> 8) ^ (tag >> 16)) & 255];
	if (*seen == tag)
		return;
	*seen = tag;
	drc_q[drc_qn].st = st;
	drc_q[drc_qn].key = key;
	drc_q[drc_qn].cnt = (UINT8)cnt;
	drc_q[drc_qn].rbw = (UINT8)drc_rbw;
	drc_qn++;
}
static int drc_idle_compile(unsigned budget_us)
{
	if (!drc_qn || !rei_drc_now_us)
		return 0;
	int n = 0;
	const int rbw0 = drc_rbw;
	const unsigned t0 = rei_drc_now_us();
	while (drc_qn > 0 && !drc_map_dirty) {
		if (rei_drc_now_us() - t0 >= budget_us)
			break;
		if (drc_end - drc_ptr < 8192 || drc_tab_used >= (int)(drc_hmask + 1) * 3 / 4 - 16 || drc_xt_cap - drc_xt_n < 2 * DRC_MAX_INSNS) {
			drc_qn = 0;
			break;
		}
		drc_qn--;
		const UINT32 key = drc_q[drc_qn].key;
		const int cnt = drc_q[drc_qn].cnt;
		drc_rbw = drc_q[drc_qn].rbw;
		const UINT32 pc = ((key >> 16 << 4) + (key & 0xffff)) & 0xfffff;
		if (pc == rei_drc_idle_pc || drc_find(key, cnt, rei_nec_pg_op[pc >> 9]))
			continue;
		const unsigned gen = drc_gen;
		drc_compile(drc_q[drc_qn].st, key, cnt);
		n++;
		if (drc_gen != gen) {
			drc_qn = 0;
			break;
		}
	}
	drc_rbw = rbw0;
	if (!drc_qn)
		memset(drc_q_seen, 0, sizeof(drc_q_seen));
	rei_drc_idle_n += n;
	return n;
}

// Is the block still intact in the current map (bank switching)
static inline int drc_valid(const struct drc_block *b)
{
	return rei_nec_pg_op[b->pg0] == b->host0 && rei_nec_pg_op[b->pg1] == b->host1;
}

// After a map change: if the insn page pointer of a page with blocks changed, mark the page volatile; the first time,
// flush everything (exits may already be linked into that page)
static void drc_check_pages(void)
{
	int flush = 0;
	drc_chk_calls++;
	drc_chk_pages += drc_code_pg_n;
	for (int i = 0; i < drc_code_pg_n; i++) {
		const int pg = drc_code_pg[i];
		if (drc_page_host[pg] && drc_page_host[pg] != rei_nec_pg_op[pg]) {
			if (!drc_page_vol[pg]) {
				drc_page_vol[pg] = 1;
				rei_drc_stats[9]++;
				flush = 1;
			}
			drc_page_host[pg] = rei_nec_pg_op[pg];
		}
	}
	if (flush) {
		DRC_WHY(3);
		drc_flush_all();
	}
}

// Link exit `from` (drc_emit_link) to block b, only when the dispatcher loop has nothing to do in between (same target and prefetch count,
// not an idle-loop address, not a short backward jump (auto idle-detect visit), not a volatile page)
static void drc_try_link(UINT32 *from, struct drc_block *b, UINT32 ps)
{
	if (from[0] != 0)
		return; // (already linked)
	const UINT32 ip = from[4] & 0xffff, k = from[1] & 0xffff;
	const UINT32 pc = ((from[5] & 0xffff) << 16) | (from[6] & 0xffff);
	if (b->key != ((ps << 16) | ip) || b->cnt != k)
		return;
	if (b->pc == rei_drc_idle_pc)
		return;
#ifdef REI_DRC_V25
	if (b->pc <= pc && pc - b->pc <= V25_AUTO_SPAN && rei_v25_auto_enabled && rei_v25_auto_with_drc) {
#else
	if (b->pc < pc && pc - b->pc <= REI_AUTO_IDLE_SPAN && rei_auto_idle_enabled) {
#endif
		// Short backward jump: only when auto idle-detect has decided this loop head is not a wait loop (skipping the visit gives the same
		// result, since idle skipping never changes results; wait loops go to the dispatcher so they keep being skipped)
#ifdef REI_DRC_V25
		if (!v25_auto_is_bad(b->pc, pc))
			return;
#else
		const int k2 = (b->pc ^ (b->pc >> 6)) & (REI_AUTO_BAD - 1);
		if (rei_auto[0].bad_head[k2] != b->pc || rei_auto[0].bad_n[k2] < 8)
			return;
#endif
	}
	if (drc_page_vol[b->pg0] || drc_page_vol[b->pg1])
		return;
	const UINT32 *body = (const UINT32 *)(void *)b->fn + 4; // (after the prologue)
	from[2] = (0x02u << 26) | (((UINT32)(uintptr_t)body >> 2) & 0x3ffffff); // j body
	from[0] = (0x01u << 26) | ((UINT32)rS1 << 21) | 3u;                          // bltz s1, [4] (s1 = icount - 1)
	drc_cache_sync(from, from + 4);
	rei_drc_stats[8]++;
}

// ---------------------------------------------------------------------------
// Dispatcher: same loop as nec_execute (no per-game fast-path hooks or static recompilation)

#ifndef REI_DRC_V25
REI_LINE_FN(17) static int nec_execute_drc(int cycles)
{
	nec_state_t *nec_state = sChipsPtr;
	int prev_ICount;

	nec_state->icount = cycles;
	nec_state->cycles_remaining = cycles;
	nec_state->stop_run = 0;

	rei_idle_t *idle = &rei_idle[nec_state - sChips];
	rei_idle_begin(idle, cycles);
	rei_nec_slice++;
	const int auto_cpu = (int)(nec_state - sChips);
	rei_auto[auto_cpu].mark = rei_auto[auto_cpu].lp_mark = 0;
	rei_rec = 0;
	UINT32 auto_prev = 0;

	if (nec_state->halted) {
		nec_state->icount = 0;
		return cycles;
	}

	{ // chip (block cycles/prefetch differ per chip): flush all on change
		const int v30 = nec_state->chip_type == V30_TYPE;
		if (v30 != drc_v30) {
			drc_v30 = v30;
			drc_pfsh = v30 ? 1 : 0;
			drc_flush_all();
		}
	}
	const UINT32 idle_pc = idle->pc ? idle->pc : 0xffffffffu;
	if (idle_pc != rei_drc_idle_pc) { // idle-loop address changed: re-cut blocks
		rei_drc_idle_pc = idle_pc;
		DRC_WHY(4);
		drc_flush_all();
	}
	UINT32 *link_from = NULL; // linkable exit the previous block left through
	unsigned link_gen = 0;    // flush generation at that time
	while ((nec_state->icount > 0) && (!nec_state->stop_run)) {
		if (nec_state->pending_irq && nec_state->no_interrupt == 0) {
			if (nec_state->pending_irq & NMI_IRQ)
				external_int(nec_state);
			else if (nec_state->IF)
				external_int(nec_state);
		}
		const int shadow = nec_state->no_interrupt;
		if (nec_state->no_interrupt)
			nec_state->no_interrupt--;
		const UINT32 pc_now = PC(nec_state);
		if (pc_now == idle_pc)
			rei_idle_visit(idle, &nec_state->icount, nec_state->icount);
		if (pc_now <= auto_prev && auto_prev - pc_now <= REI_AUTO_IDLE_SPAN && rei_auto_idle_enabled)
			rei_auto_visit(nec_state, auto_cpu, pc_now, auto_prev);
		auto_prev = pc_now;

		// (shadow: right after MOV SS/POP SS etc.; no interrupt taken at this boundary. If none is pending at the next boundary either,
		//  result equals the interpreter, so run the block. Most dispatcher-interpreted insns on dsoccr94 PSP were this, 2026-10-17)
		if ((!shadow || (shadow == 1 && !(nec_state->pending_irq && ((nec_state->pending_irq & NMI_IRQ) || nec_state->IF)))) &&
		    !rei_rec && !nec_state->prefetch_reset) {
			if (drc_map_dirty) {
				drc_map_dirty = 0;
				drc_check_pages();
				link_from = NULL;
			}
#if !defined(__psp__)
			drc_prof_tick();
#endif
			const UINT32 key = ((UINT32)Sreg(PS) << 16) | nec_state->ip;
			const int cnt = nec_state->prefetch_count;
			struct drc_block *b = drc_find(key, cnt, rei_nec_pg_op[pc_now >> 9]);
			if (b && b->fn && !drc_valid(b)) { // second page's bank changed: tombstone this entry and recompile
				rei_drc_stats[5]++;
				b->key = 0xfffffffeu;
				b = NULL;
				link_from = NULL;
			}
			if (!b && drc_budget <= 0)
				drc_queue(nec_state, key, cnt);
			if (!b && drc_budget > 0) {
#ifdef REI_PROFILE
				const uint64_t tc0 = rei_prof_clock();
#else
				const unsigned tc0 = rei_drc_clock32 ? rei_drc_clock32() : 0;
#endif
				b = drc_compile(nec_state, key, cnt);
				drc_budget -= (b && b->fn) ? b->ninsn : 1;
#ifdef REI_PROFILE
				rei_drc_stats[10] += rei_prof_clock() - tc0;
#else
				if (rei_drc_clock32)
					rei_drc_stats[10] += rei_drc_clock32() - tc0;
#endif
			}
			if (b && b->fn) {
				if (link_from && link_gen == drc_gen) // (flushed in between: stale exit address, do not link)
					drc_try_link(link_from, b, Sreg(PS));
				if (cnt == 0 && b->pc != rei_drc_idle_pc && !drc_page_vol[b->pg0] && !drc_page_vol[b->pg1]) {
					const UINT32 h = (key ^ (key >> 10)) & (DRC_JC - 1);
					drc_jc[h][0] = key;
					drc_jc[h][1] = (UINT32)(uintptr_t)((const UINT32 *)(void *)b->fn + 4);
				}
				rei_drc_stats[0]++;
				rei_drc_stats[1] += b->ninsn;
#if !defined(REI_DRC_V25)
				if (__builtin_expect(rei_drc_interp_stat != 0, 0)) drc_entry_note(b, link_from != NULL, pc_now);
#endif
#ifndef REI_DRC_TEST
				drc_last_stub = NULL;
#if defined(REI_PROFILE) && !defined(REI_PROF_FRAME_ONLY)
				{ // (profile) time spent in blocks (1/16 sampled, REI_PROF_FAST slot; dynarec builds have no fast-path hooks)
					static unsigned pn;
					const uint64_t tb = ((++pn * 0x9e3779b1u) >> 28) == 0 ? rei_prof_clock() : 0;
					auto_prev = b->fn(nec_state);
					if (tb) REI_PROF_ACC[REI_PROF_FAST] += (rei_prof_clock() - tb) << 4;
				}
#else
#ifndef REI_DRC_V25
				if (__builtin_expect(rei_drc_blk_clock != NULL, 0)) {
					const unsigned tb = rei_drc_blk_clock();
					auto_prev = b->fn(nec_state);
					rei_drc_blk_acc += rei_drc_blk_clock() - tb;
				} else
#endif
				auto_prev = b->fn(nec_state);
#endif
				link_from = drc_last_stub;
				link_gen = drc_gen;
				continue;
#endif
			}
		}
		link_from = NULL;

		prev_ICount = nec_state->icount;
		rei_ins_ic = prev_ICount;
		rei_drc_stats[2]++;
#if !defined(REI_DRC_V25)
		if (__builtin_expect(rei_drc_interp_stat != 0, 0)) drc_interp_note(nec_state, shadow, pc_now);
#endif
#ifdef REI_PROFILE
		rei_prof_ins++;
#endif
		nec_instruction[fetchop(nec_state)](nec_state);
		if (drc_pfsh == 0) rei_prefetch1(nec_state, prev_ICount);
		else do_prefetch(nec_state, prev_ICount, drc_pfsh);
	}

	rei_idle_end(idle, nec_state->icount);
#ifdef REI_PROFILE
	REI_PROF_ACC[REI_PROF_INS] += rei_prof_ins;
	REI_PROF_ACC[REI_PROF_HOOK] += 1000;
	rei_prof_ins = 0;
#endif
	cycles = cycles - nec_state->icount;
	nec_state->cycles_total += cycles;
	nec_state->cycles_remaining = nec_state->icount = 0;
	return cycles;
}

#endif // !REI_DRC_V25 (V35 dispatcher is v25.cpp v25_execute_drc)

#endif // REI_DRC && __mips__
