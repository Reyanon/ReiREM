/****************************************************************************

    NEC V20/V30/V33 emulator

    ---------------------------------------------

    V20 = uPD70108 = 8-bit data bus @ 5MHz or 8MHz
    V20HL = uPD70108H = V20 with EMS support (24-bit address bus)

    V25 = uPD70320 = V20 with on-chip features:
            - 256 bytes on-chip RAM
            - 8 register banks
            - 4-bit input port
            - 20-bit I/O port
            - 2 channel serial interface
            - interrupt controller
            - 2 channel DMA controller
            - 2 channel 16-bit timer
            - new instructions: BTCLR, RETRBI, STOP, BRKCS, TSKSW,
                                MOVSPA, MOVSPB

    V25+ = uPD70325 = V25 @ 8MHz or 10MHz plus changes:
            - faster DMA
            - improved serial interface

    ---------------------------------------------

    V30 = uPD70116 = 16-bit data bus version of V20
    V30HL = uPD70116H = 16-bit data bus version of V20HL
    V30MX = V30HL with separate address and data busses

    V35 = uPD70330 = 16-bit data bus version of V25

    V35+ = uPD70335 = 16-bit data bus version of V25+

    ---------------------------------------------

    V40 = uPD70208 = 8-bit data bus @ 10MHz
    V40HL = uPD70208H = V40 with support up to 20Mhz

    ---------------------------------------------

    V50 = uPD70216 = 16-bit data bus version of V40
    V50HL = uPD70216H = 16-bit data bus version of V40HL

    ---------------------------------------------

    V41 = uPD70270

    V51 = uPD70280



    V33A = uPD70136A

    V53A = uPD70236A



    Instruction differences:
        V20, V30, V40, V50 have dedicated emulation instructions
            (BRKEM, RETEM, CALLN)

        V33A, V53A has dedicated address mode instructions
            (BRKXA, RETXA)



    (Re)Written June-September 2000 by Bryan McPhail (mish@tendril.co.uk) based
    on code by Oliver Bergmann (Raul_Bloodworth@hotmail.com) who based code
    on the i286 emulator by Fabrice Frances which had initial work based on
    David Hedley's pcemu(!).

    This new core features 99% accurate cycle counts for each processor,
    there are still some complex situations where cycle counts are wrong,
    typically where a few instructions have differing counts for odd/even
    source and odd/even destination memory operands.

    Flag settings are also correct for the NEC processors rather than the
    I86 versions.

    Changelist:

    22/02/2003:
        Removed cycle counts from memory accesses - they are certainly wrong,
        and there is already a memory access cycle penalty in the opcodes
        using them.

        Fixed save states.

        Fixed ADJBA/ADJBS/ADJ4A/ADJ4S flags/return values for all situations.
        (Fixes bugs in Geostorm and Thunderblaster)

        Fixed carry flag on NEG (I thought this had been fixed circa Mame 0.58,
        but it seems I never actually submitted the fix).

        Fixed many cycle counts in instructions and bug in cycle count
        macros (odd word cases were testing for odd instruction word address
        not data address).

    Todo!
        Double check cycle timing is 100%.

****************************************************************************/

#include "burnint.h"
//#include <stdlib.h>
//#include <stdio.h>
#include "driver.h"
//#include "state.h"
#include "nec_intf.h"
#include "bitswap.h"
#include <stddef.h>

#define INPUT_LINE_NMI	0x20 // nmi

#define NEC_INLINE
#define NEC_CPU_CPP

unsigned char cpu_readmem20_op(unsigned int);
unsigned char cpu_readmem20_arg(unsigned int);
unsigned char cpu_readport(unsigned int);
void cpu_writeport(unsigned int,unsigned int);

// ReiREM Vita port: V33 memory access inlined into the core. Same page tables and handlers as
// cpu_readmem20 & co. in nec_intf.cpp (pages may be remapped at any time; the arrays are shared).
extern UINT8 **rei_nec_pg_rd, **rei_nec_pg_wr, **rei_nec_pg_op, **rei_nec_pg_arg;
extern UINT8 (__fastcall **rei_nec_rh)(UINT32);
extern void (__fastcall **rei_nec_wh)(UINT32, UINT8);
#ifdef REI_V33STAT // diagnostics: per-frame V33 counters (instructions, cycles, handler calls)
extern "C" { unsigned long long rei_v33st[4]; } // 0 ins, 1 cycles, 2 handler reads, 3 handler writes
#define REI_ST(i, n) (rei_v33st[i] += (n))
#else
#define REI_ST(i, n) ((void)0)
#endif
#ifdef REI_PAGEHIST // diagnostics: V33 accesses per 512-byte page (0 data read, 1 code, 2 write)
extern "C" { unsigned rei_pagehist[3][2048]; }
#define REI_PH(k, a) (rei_pagehist[k][(a) >> 9]++)
#else
#define REI_PH(k, a) ((void)0)
#endif
// M92 port: side effects seen by the automatic idle loop detection (rei_auto_idle): memory writes, handler reads
// (I/O-like regions whose value may change with time) and port accesses. A loop iteration with none of them and the
// same registers as the previous one repeats exactly
static UINT32 rei_fx;
// Ports 0..31 whose value cannot change while the CPU runs (inputs, a vblank flag set between runs): the driver marks
// them (bit n = port n) so a loop polling one is still an idle loop. Reads of other ports count as side effects
UINT32 rei_nec_pure_ports;
// (dynarec) driver-maintained copy of pure ports 0..7 (8-byte aligned, always equal to what the handler returns; changed only between lines).
// If non-NULL, IN imm8 reads from this copy without calling the handler
UINT8 *rei_nec_port_shadow;
static inline UINT32 rei_port_fx(UINT32 a, UINT32 bits) { return (a < 31 && ((rei_nec_pure_ports >> a) & bits) == bits) ? 0 : 1; }
// While one loop iteration is checked (rei_rec), mapped memory reads and all writes are logged: a loop may write
// memory only as counters (read-modify-write of the same address, not read elsewhere in the loop) — the idle counters
// of Irem games ("inc word [x]" while waiting for an interrupt flag)
#define REI_REC_MAX 16
static int rei_rec, rei_rec_n;
static UINT32 rei_rec_a[REI_REC_MAX];
static UINT8 rei_rec_w[REI_REC_MAX], rei_rec_v[REI_REC_MAX]; // 0 read, 1 write, 2 handler write
static __attribute__((noinline)) void rei_rec_log(UINT32 a, int w, UINT8 v)
{
	if (rei_rec_n < REI_REC_MAX) {
		rei_rec_a[rei_rec_n] = a;
		rei_rec_w[rei_rec_n] = (UINT8)w;
		rei_rec_v[rei_rec_n] = v;
	} else {
		rei_rec = 0; // not a counting loop (or the loop was left): stop logging until the next loop head
	}
	rei_rec_n++;
}
static inline __attribute__((always_inline)) UINT8 rei_nec_rd(UINT32 a)
{
	a &= 0xFFFFF;
	REI_PH(0, a);
	UINT8 *p = rei_nec_pg_rd[a >> 9];
	if (!p) { REI_ST(2, 1); rei_fx++; return (*rei_nec_rh)(a); }
	if (__builtin_expect(rei_rec, 0)) rei_rec_log(a, 0, p[a]);
	return p[a];
}
static inline __attribute__((always_inline)) UINT8 rei_nec_op(UINT32 a)
{
	a &= 0xFFFFF;
	REI_PH(1, a);
	UINT8 *p = rei_nec_pg_op[a >> 9];
	if (!p) { rei_fx++; return (*rei_nec_rh)(a); } // (code through a handler: a side effect for the idle detection)
	return p[a];
}
static inline __attribute__((always_inline)) UINT8 rei_nec_arg(UINT32 a)
{
	a &= 0xFFFFF;
	REI_PH(1, a);
	UINT8 *p = rei_nec_pg_arg[a >> 9];
	if (!p) { rei_fx++; return (*rei_nec_rh)(a); }
	return p[a];
}
static inline __attribute__((always_inline)) void rei_nec_wr(UINT32 a, UINT8 d)
{
	a &= 0xFFFFF;
	REI_PH(2, a);
	rei_fx++;
	UINT8 *p = rei_nec_pg_wr[a >> 9];
	if (__builtin_expect(rei_rec, 0)) rei_rec_log(a, p ? 1 : 2, d);
	if (p) p[a] = d;
	else { REI_ST(3, 1); (*rei_nec_wh)(a, d); }
}
#define cpu_readmem20     rei_nec_rd
#define cpu_readmem20_op  rei_nec_op
#define cpu_readmem20_arg rei_nec_arg
#define cpu_writemem20    rei_nec_wr

static NEC_INLINE void write_mem_word(unsigned int a, unsigned short d)
{
	cpu_writemem20((a),(unsigned char)(d));
	cpu_writemem20((a)+1,(d)>>8);
}

static NEC_INLINE void write_port_word(unsigned int a, unsigned short d)
{
	rei_fx++;
	cpu_writeport((a),(unsigned char)(d));
	cpu_writeport((a)+1,(d)>>8);
}

typedef UINT8 BOOLEAN;
typedef UINT8 BYTE;
typedef UINT16 WORD;
typedef UINT32 DWORD;

#include "nec.h"
#include "necpriv.h"
#if defined(__psp__) || defined(REI_FETCH_CACHE)
// ReiREM PSP port: the V33 (chip_type 0, M92) takes its cycle counts as constants (same values as the packed tables
// shifted by chip_type 0); other chips (V30 on M72/M81/M82/M84/M90, V20) use the packed tables as upstream
#undef CLKS
#undef CLKW
#undef CLKM
#undef CLKR
#define REI_CLK(v20,v30,v33) (__builtin_expect(nec_state->chip_type == 0, 1) ? ((v33)&0x7f) : (((((v20)<<16)|((v30)<<8)|(v33)) >> nec_state->chip_type)&0x7f))
#define CLKS(v20,v30,v33) { nec_state->icount-=REI_CLK(v20,v30,v33); }
#define CLKW(v20o,v30o,v33o,v20e,v30e,v33e,addr) { nec_state->icount-=((addr)&1)?REI_CLK(v20o,v30o,v33o):REI_CLK(v20e,v30e,v33e); }
#define CLKM(v20,v30,v33,v20m,v30m,v33m) { nec_state->icount-=( ModRM >=0xc0 )?REI_CLK(v20,v30,v33):REI_CLK(v20m,v30m,v33m); }
#define CLKR(v20o,v30o,v33o,v20e,v30e,v33e,vall,addr) { if (ModRM >=0xc0) nec_state->icount-=vall; else nec_state->icount-=((addr)&1)?REI_CLK(v20o,v30o,v33o):REI_CLK(v20e,v30e,v33e); }
#endif

#include "rei_idle.h" // ReiREM Vita port: idle loop skip
#include "rei_line.h" // ReiREM PSP port: per-line code block
static nec_state_t sChips[4]; // 4 cpus should be plenty!

// ReiREM Vita port: per-chip idle loop skip state (see rei_idle.h)
static rei_idle_t rei_idle[4];
static UINT32 rei_nec_slice, rei_nec_irqs; // ReiREM PSP port: time slices run, interrupts taken (piece loop)
#include "rei_prof.h"
#ifdef REI_PROFILE
static UINT32 rei_prof_ins; // instructions run by the interpreter (profile builds)
#endif

// ReiREM Vita port: idle skip statistics
void nec_get_idle_stats(int cpu, unsigned long long *visits, unsigned long long *skipped, int *last_d)
{
	*visits = rei_idle[cpu].visits;
	*skipped = rei_idle[cpu].skipped;
	*last_d = rei_idle[cpu].last_d;
}

void nec_set_idle_pc(int cpu, unsigned pc)
{
	rei_idle[cpu].pc = pc;
	rei_idle[cpu].mark = 0;
	rei_idle[cpu].iter = 0;
	rei_idle[cpu].tail = -1;
}

// M92 port: automatic idle loop detection (every game, no per-game addresses). A short backward jump marks a loop
// head. At the next visit of the same head, if the iteration had no side effects (rei_fx: memory writes, handler
// reads, port accesses) and the CPU state is the same as at the previous visit, the iteration repeats exactly until
// an interrupt (which only comes between instructions from outside this slice, or restarts the measurement). With the
// same iteration cost d twice, whole iterations are skipped like rei_idle_visit (icount left >= 1, so the slice ends
// at the same instruction and cycle as without the skip: same results, PC harness hashes)
#define REI_AUTO_IDLE_SPAN 48 // longest loop body (bytes) taken as a wait loop
struct rei_auto_snap {
	necbasicregs regs;
	UINT16 sregs[4];
	INT32 SignVal;
	UINT32 AuxVal, OverVal, ZeroVal, CarryVal, ParityVal;
	UINT8 TF, IF, DF, MF;
	UINT32 pending_irq;
	UINT8 no_interrupt, halted;
	INT8 prefetch_count;
	UINT8 prefetch_reset;
	UINT32 prefix_base;
	UINT8 seg_prefix;
};
// counters written by one loop iteration: byte or word (two consecutive byte writes) units
#define REI_AUTO_UNITS 4
struct rei_auto_units {
	int n;
	UINT32 a[REI_AUTO_UNITS];
	UINT8 word[REI_AUTO_UNITS];
	UINT16 v[REI_AUTO_UNITS];
};

// the logged iteration -> counter units. 0 if the loop does anything else than counting (too many accesses, handler
// writes, a written address read outside its own read-modify-write)
static int rei_auto_units_of(struct rei_auto_units *u)
{
	u->n = 0;
	if (rei_rec_n > REI_REC_MAX)
		return 0;
	for (int i = 0; i < rei_rec_n; i++) {
		if (rei_rec_w[i] == 2)
			return 0;
		if (rei_rec_w[i] != 1)
			continue;
		const UINT32 a = rei_rec_a[i];
		// exactly one read of a, before this write, with only accesses to the same instruction's bytes in between
		int reads = 0, rpos = -1;
		for (int j = 0; j < rei_rec_n; j++)
			if (rei_rec_w[j] == 0 && rei_rec_a[j] == a) {
				reads++;
				rpos = j;
			}
		if (reads != 1 || rpos > i)
			return 0;
		for (int j = rpos + 1; j < i; j++)
			if (rei_rec_a[j] + 1 < a || rei_rec_a[j] > a + 1)
				return 0;
		for (int j = 0; j < i; j++) // written twice in one iteration: not a counter
			if (rei_rec_w[j] == 1 && rei_rec_a[j] == a)
				return 0;
		if (u->n > 0 && !u->word[u->n - 1] && u->a[u->n - 1] + 1 == a && rei_rec_w[i - 1] == 1 && rei_rec_a[i - 1] + 1 == a) {
			u->word[u->n - 1] = 1; // the high byte of the previous unit
			u->v[u->n - 1] |= (UINT16)(rei_rec_v[i] << 8);
			continue;
		}
		if (u->n >= REI_AUTO_UNITS)
			return 0;
		u->a[u->n] = a;
		u->word[u->n] = 0;
		u->v[u->n] = rei_rec_v[i];
		u->n++;
	}
	return 1;
}

// Flags that matter at the loop head. When the body (head .. the backward jump) has only instructions that don't read
// flags (a short list: mov, inc/dec, test, cmp, and/or/xor/add/sub, test1 ...), only the flags the closing jump reads
// must repeat (their raw values: the same value each iteration means their source doesn't change). Otherwise all.
// (In the Hunt / Gunforce: "inc word [x]; test1 word [y], b; jz" — test1 sets only ZF, SF/PF/AF keep the counter's)
enum { REI_F_S = 1, REI_F_A = 2, REI_F_O = 4, REI_F_Z = 8, REI_F_C = 16, REI_F_P = 32, REI_F_ALL = 63 };
static int rei_auto_modrm_len(UINT32 a) // bytes after the ModRM byte (16-bit addressing)
{
	const UINT8 m = rei_nec_op(a);
	const int mod = m >> 6, rm = m & 7;
	return mod == 3 ? 0 : mod == 1 ? 1 : mod == 2 ? 2 : (rm == 6 ? 2 : 0);
}
static int rei_auto_flag_mask(UINT32 head, UINT32 end)
{
	// (code read through a handler - e.g. M72 MCU shared RAM - is not analysed: the read itself may sync another CPU)
	if (!rei_nec_pg_op[(head & 0xfffff) >> 9] || !rei_nec_pg_op[((end + 2) & 0xfffff) >> 9])
		return REI_F_ALL;
	UINT32 a = head;
	for (int guard = 0; a < end && guard < 32; guard++) {
		const UINT8 op = rei_nec_op(a);
		int len = -1;
		switch (op) {
		case 0x26: case 0x2e: case 0x36: case 0x3e: len = 1; break; // segment prefixes
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2a: case 0x2b:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3a: case 0x3b:
		case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b:
		case 0xfe: case 0xff:
			if ((op == 0xfe || op == 0xff) && ((rei_nec_op(a + 1) >> 3) & 7) > 1)
				return REI_F_ALL; // only inc/dec
			len = 2 + rei_auto_modrm_len(a + 1);
			break;
		case 0x04: case 0x0c: case 0x24: case 0x2c: case 0x34: case 0x3c: case 0xa8: len = 2; break;
		case 0x05: case 0x0d: case 0x25: case 0x2d: case 0x35: case 0x3d: case 0xa9: len = 3; break;
		case 0xa0: case 0xa1: case 0xa2: case 0xa3: len = 3; break;
		case 0x90: len = 1; break;
		case 0x80: case 0x81: case 0x83: {
			const int r = (rei_nec_op(a + 1) >> 3) & 7;
			if (r == 2 || r == 3)
				return REI_F_ALL; // adc, sbb read CF
			len = 2 + rei_auto_modrm_len(a + 1) + (op == 0x81 ? 2 : 1);
			break;
		}
		case 0xf6: case 0xf7: {
			const int r = (rei_nec_op(a + 1) >> 3) & 7;
			if (r == 0)
				len = 2 + rei_auto_modrm_len(a + 1) + (op == 0xf7 ? 2 : 1);
			else if (r == 2 || r == 3)
				len = 2 + rei_auto_modrm_len(a + 1);
			else
				return REI_F_ALL;
			break;
		}
		case 0x0f: {
			const UINT8 op2 = rei_nec_op(a + 1);
			if (op2 < 0x10 || op2 > 0x1f)
				return REI_F_ALL;
			len = 3 + rei_auto_modrm_len(a + 2) + (op2 >= 0x18 ? 1 : 0); // test1/clr1/set1/not1
			break;
		}
		default:
			if (op >= 0x40 && op <= 0x4f) { len = 1; break; }
			if (op >= 0xb0 && op <= 0xb7) { len = 2; break; }
			if (op >= 0xb8 && op <= 0xbf) { len = 3; break; }
			return REI_F_ALL;
		}
		a += len;
	}
	if (a != end)
		return REI_F_ALL;
	switch (rei_nec_op(end)) { // the closing jump
	case 0x70: case 0x71: return REI_F_O;
	case 0x72: case 0x73: return REI_F_C;
	case 0x74: case 0x75: return REI_F_Z;
	case 0x76: case 0x77: return REI_F_C | REI_F_Z;
	case 0x78: case 0x79: return REI_F_S;
	case 0x7a: case 0x7b: return REI_F_P;
	case 0x7c: case 0x7d: return REI_F_S | REI_F_O;
	case 0x7e: case 0x7f: return REI_F_S | REI_F_O | REI_F_Z;
	case 0xeb: return 0;
	default: return REI_F_ALL;
	}
}

static int rei_auto_same(const struct rei_auto_snap *x, const struct rei_auto_snap *y, int fmask)
{
	if (memcmp(&x->regs, &y->regs, sizeof(x->regs)) || memcmp(x->sregs, y->sregs, sizeof(x->sregs)) || x->TF != y->TF ||
	    x->IF != y->IF || x->DF != y->DF || x->MF != y->MF || x->pending_irq != y->pending_irq ||
	    x->no_interrupt != y->no_interrupt || x->halted != y->halted || x->prefetch_count != y->prefetch_count ||
	    x->prefetch_reset != y->prefetch_reset || x->prefix_base != y->prefix_base || x->seg_prefix != y->seg_prefix)
		return 0;
	return (!(fmask & REI_F_S) || x->SignVal == y->SignVal) && (!(fmask & REI_F_A) || x->AuxVal == y->AuxVal) &&
	       (!(fmask & REI_F_O) || x->OverVal == y->OverVal) && (!(fmask & REI_F_Z) || x->ZeroVal == y->ZeroVal) &&
	       (!(fmask & REI_F_C) || x->CarryVal == y->CarryVal) && (!(fmask & REI_F_P) || x->ParityVal == y->ParityVal);
}

// how many iterations can be skipped without a counter reaching 0 or its sign boundary (so no instruction in the loop
// can see a carry, zero or sign change of a counter that didn't happen in the checked iterations: adc into a high word)
static int rei_auto_limit(const struct rei_auto_units *u, const UINT16 *step, int k)
{
	for (int i = 0; i < u->n; i++) {
		if (!step[i])
			continue;
		const int bits = u->word[i] ? 16 : 8;
		const int half = 1 << (bits - 1), full = 1 << bits;
		int v = u->v[i] & (full - 1), s = step[i] & (full - 1);
		if (s >= half) s -= full; // signed step
		// distance to the next boundary (0 or half) in the step's direction
		const int lo = v >= half ? half : 0, hi = v >= half ? full : half;
		const int room = s > 0 ? (hi - 1 - v) / s : (v - lo - (lo == 0)) / -s; // (down to 0: stop at 1)
		if (room < k)
			k = room;
	}
	return k;
}

#define REI_AUTO_BAD 64 // loop heads that keep failing the check are not checked again (game logic loops)
static struct {
	UINT32 head;  // loop head (linear PC), 0 = none
	int mark;     // icount at the last visit (0 = not measured in this slice)
	int phase;    // 1: first iteration being checked, 2: second (units / d of the first known)
	int iter;     // cost of the last checked iteration
	int fmask;    // flags that must repeat (rei_auto_flag_mask)
	UINT32 fx;    // rei_fx at the last visit
	struct rei_auto_snap snap;
	struct rei_auto_units prev; // counters after the last checked iteration
	int pw_n;      // writes of the last checked iteration when they are all mapped (-1: none known)
	UINT32 lp_head, lp_fx; // LOOP $ (delay loop on CW): last visit
	int lp_mark, lp_d;
	UINT16 lp_cw;
	struct rei_auto_snap lp_snap;
	UINT32 pw_a[REI_REC_MAX];
	UINT8 pw_v[REI_REC_MAX];
	UINT32 bad_head[REI_AUTO_BAD];
	UINT8 bad_n[REI_AUTO_BAD];
	unsigned long long skipped; // statistics
} rei_auto[4];
int rei_auto_idle_enabled = 1; // harness --no-idle-skip

static inline void rei_auto_take(struct rei_auto_snap *s, const nec_state_t *n)
{
	memset(s, 0, sizeof(*s));
	s->regs = n->regs;
	memcpy(s->sregs, n->sregs, sizeof(s->sregs));
	s->SignVal = n->SignVal;
	s->AuxVal = n->AuxVal;
	s->OverVal = n->OverVal;
	s->ZeroVal = n->ZeroVal;
	s->CarryVal = n->CarryVal;
	s->ParityVal = n->ParityVal;
	s->TF = n->TF;
	s->IF = n->IF;
	s->DF = n->DF;
	s->MF = n->MF;
	s->pending_irq = n->pending_irq;
	s->no_interrupt = n->no_interrupt;
	s->halted = n->halted;
	s->prefetch_count = n->prefetch_count;
	s->prefetch_reset = n->prefetch_reset;
	s->prefix_base = 0; // (ReiREM: meaningless between instructions; dynarec does not keep it)
	s->seg_prefix = n->seg_prefix;
}

static void rei_auto_start(nec_state_t *n, int cpu, UINT32 head, UINT32 end, const struct rei_auto_snap *now)
{
	if (rei_auto[cpu].head != head)
		rei_auto[cpu].fmask = rei_auto_flag_mask(head, end);
	rei_auto[cpu].head = head;
	rei_auto[cpu].phase = 1;
	rei_auto[cpu].snap = *now;
	rei_auto[cpu].mark = n->icount;
	rei_auto[cpu].fx = rei_fx;
	rei_auto[cpu].pw_n = -1;
	rei_rec = 1;
	rei_rec_n = 0;
}

static void rei_auto_fail(int cpu, UINT32 head)
{
	const int k = (head ^ (head >> 6)) & (REI_AUTO_BAD - 1);
	if (rei_auto[cpu].bad_head[k] != head) {
		rei_auto[cpu].bad_head[k] = head;
		rei_auto[cpu].bad_n[k] = 0;
	}
	if (rei_auto[cpu].bad_n[k] < 255)
		rei_auto[cpu].bad_n[k]++;
}

// at a loop head reached by a short backward jump
static __attribute__((noinline)) void rei_auto_visit(nec_state_t *n, int cpu, UINT32 head, UINT32 end)
{
	const int k = (head ^ (head >> 6)) & (REI_AUTO_BAD - 1);
#ifdef REI_AUTODBG
	if (head == REI_AUTODBG) { static int c; if (c++ < 12) fprintf(stderr, "autodbg visit end %05x bad %d cur %05x mark %d rec %d n %d\n", end,
		rei_auto[cpu].bad_head[k] == head ? rei_auto[cpu].bad_n[k] : -1, rei_auto[cpu].head, rei_auto[cpu].mark, rei_rec, rei_rec_n); }
#endif
	if (rei_auto[cpu].bad_head[k] == head && rei_auto[cpu].bad_n[k] >= 8)
		return;
	// loop code read through a handler (M72 MCU shared RAM: each fetch syncs the MCU, which may rewrite the code) is never
	// skipped - the dynarec runs such code compiled, without the fetches, so rei_fx alone would not see them
	if (!rei_nec_pg_op[(head & 0xfffff) >> 9] || !rei_nec_pg_op[((end + 2) & 0xfffff) >> 9])
		return;
	struct rei_auto_snap now;
	rei_auto_take(&now, n);
	// "LOOP $" (e2 fe): a delay loop counting CW down - every iteration the same but CW one less. After two iterations of
	// the same cost, skip whole iterations while CW stays above 0 (LOOP changes no flags)
	if (head == end && rei_nec_op(head) == 0xe2 && rei_nec_op((head + 1) & 0xfffff) == 0xfe) {
		const UINT16 cw = now.regs.w[CW];
		now.regs.w[CW] = 0;
		if (rei_auto[cpu].lp_head == head && rei_auto[cpu].lp_mark > 0 && rei_auto[cpu].lp_fx == rei_fx &&
		    cw == (UINT16)(rei_auto[cpu].lp_cw - 1) && rei_auto_same(&now, &rei_auto[cpu].lp_snap, REI_F_ALL)) {
			const int d = rei_auto[cpu].lp_mark - n->icount;
			if (d > 0 && d == rei_auto[cpu].lp_d) {
				int kk = (n->icount - 1) / d;
				const int left = cw ? cw - 1 : 65535; // (iterations that still jump back)
				if (kk > left) kk = left;
				if (kk > 0) {
					n->regs.w[CW] = (UINT16)(cw - kk);
					n->icount -= kk * d;
					rei_auto[cpu].skipped += (unsigned long long)(kk * d);
				}
			}
			rei_auto[cpu].lp_d = d;
		} else {
			rei_auto[cpu].lp_d = 0;
		}
		rei_auto[cpu].lp_head = head;
		rei_auto[cpu].lp_mark = n->icount;
		rei_auto[cpu].lp_cw = n->regs.w[CW];
		rei_auto[cpu].lp_snap = now;
		rei_auto[cpu].lp_fx = rei_fx;
		return;
	}
	if (head != rei_auto[cpu].head || rei_auto[cpu].mark <= 0 || (!rei_rec && rei_rec_n <= REI_REC_MAX)) {
		rei_auto_start(n, cpu, head, end, &now);
		return;
	}
	rei_rec = 0;
	struct rei_auto_units u;
	int writes = 0;
	for (int i = 0; i < rei_rec_n && i < REI_REC_MAX; i++)
		writes += rei_rec_w[i] != 0;
	// same state at the head, side effects = only the logged writes (no handler reads, ports), writes are counters
#ifdef REI_AUTODBG
	if (head == REI_AUTODBG) { static int c; if (c++ < 20) fprintf(stderr, "autodbg same %d fx %u writes %d units %d ss %04x\n",
		rei_auto_same(&now, &rei_auto[cpu].snap, rei_auto[cpu].fmask), rei_fx - rei_auto[cpu].fx, writes, rei_auto_units_of(&u), n->sregs[2]); }
#endif
	// or: the writes are the same bytes and values as in the iteration before (stack use of a CALL in a polling loop) -
	// then the memory, like the registers, is the same at the next head (no counters to move)
	int cw_n = -1;
	UINT32 cw_a[REI_REC_MAX];
	UINT8 cw_v[REI_REC_MAX];
	if (rei_rec_n <= REI_REC_MAX) {
		cw_n = 0;
		for (int i = 0; i < rei_rec_n; i++) {
			if (rei_rec_w[i] == 2) { cw_n = -1; break; }
			if (rei_rec_w[i] == 1) { cw_a[cw_n] = rei_rec_a[i]; cw_v[cw_n] = rei_rec_v[i]; cw_n++; }
		}
	}
	const int units_ok = rei_auto_units_of(&u);
	if (!units_ok) u.n = 0;
	const int idem = !units_ok && cw_n > 0 && cw_n == rei_auto[cpu].pw_n &&
		!memcmp(cw_a, rei_auto[cpu].pw_a, cw_n * sizeof(UINT32)) && !memcmp(cw_v, rei_auto[cpu].pw_v, cw_n);
	if (!rei_auto_same(&now, &rei_auto[cpu].snap, rei_auto[cpu].fmask) || rei_fx - rei_auto[cpu].fx != (UINT32)writes ||
	    (!units_ok && cw_n <= 0)) {
		rei_auto_fail(cpu, head);
		rei_auto_start(n, cpu, head, end, &now);
		return;
	}
	const int d = rei_auto[cpu].mark - n->icount;
#ifdef REI_AUTODBG
	if (head == REI_AUTODBG) { static int c; if (c++ < 20) fprintf(stderr, "autodbg units %d cw %d pw %d idem %d phase %d d %d iter %d\n", units_ok, cw_n, rei_auto[cpu].pw_n, idem, rei_auto[cpu].phase, d, rei_auto[cpu].iter); }
#endif
	if (idem && rei_auto[cpu].phase == 2 && d == rei_auto[cpu].iter && d > 0 && d <= 2048) { // (longer iterations: polling through a CALL)
		const int kk = (n->icount - 1) / d;
		if (kk > 0) {
			n->icount -= kk * d;
			rei_auto[cpu].skipped += (unsigned long long)(kk * d);
		}
		const int bk = (head ^ (head >> 6)) & (REI_AUTO_BAD - 1);
		if (rei_auto[cpu].bad_head[bk] == head)
			rei_auto[cpu].bad_n[bk] = 0;
	} else if (units_ok && rei_auto[cpu].phase == 2 && d == rei_auto[cpu].iter && d > 0 && d <= REI_IDLE_MAX_ITER &&
	    u.n == rei_auto[cpu].prev.n) {
		// the same counters, each moved by the same step as in the iteration before: skip whole iterations
		int same = 1;
		UINT16 step[REI_AUTO_UNITS];
		for (int i = 0; i < u.n; i++) {
			same &= u.a[i] == rei_auto[cpu].prev.a[i] && u.word[i] == rei_auto[cpu].prev.word[i];
			step[i] = (UINT16)(u.v[i] - rei_auto[cpu].prev.v[i]);
		}
		const int avail = n->icount - 1;
		const int kk = same ? rei_auto_limit(&u, step, avail / d) : 0;
		if (kk > 0) {
#ifdef REI_AUTOSKIPDBG
			if (head == end) { static int c; if (c++ < 40) fprintf(stderr, "selfskip %05x kk %d d %d ic %d irq %x IF %d\n", head, kk, d, n->icount, n->pending_irq, n->IF); }
#endif
			n->icount -= kk * d;
			rei_auto[cpu].skipped += (unsigned long long)(kk * d);
			for (int i = 0; i < u.n; i++) {
				const UINT16 v = (UINT16)(u.v[i] + step[i] * kk);
				u.v[i] = v;
				UINT8 *p = rei_nec_pg_wr[u.a[i] >> 9];
				if (p) p[u.a[i]] = (UINT8)v;
				if (u.word[i]) {
					p = rei_nec_pg_wr[(u.a[i] + 1) >> 9];
					if (p) p[u.a[i] + 1] = (UINT8)(v >> 8);
				}
			}
		}
		const int bk = (head ^ (head >> 6)) & (REI_AUTO_BAD - 1);
		if (rei_auto[cpu].bad_head[bk] == head)
			rei_auto[cpu].bad_n[bk] = 0;
	}
	// next iteration: check again against this one
	rei_auto[cpu].prev = u;
	rei_auto[cpu].pw_n = cw_n;
	if (cw_n > 0) {
		memcpy(rei_auto[cpu].pw_a, cw_a, cw_n * sizeof(UINT32));
		memcpy(rei_auto[cpu].pw_v, cw_v, cw_n);
	}
	rei_auto[cpu].iter = d;
	rei_auto[cpu].phase = 2;
	rei_auto[cpu].snap = now;
	rei_auto[cpu].mark = n->icount;
	rei_auto[cpu].fx = rei_fx;
	rei_rec = 1;
	rei_rec_n = 0;
}

void nec_get_auto_idle_stats(int cpu, unsigned long long *skipped)
{
	*skipped = rei_auto[cpu].skipped;
}
extern "C" unsigned long long rei_auto_skipped(int cpu) { return rei_auto[cpu].skipped; } // (diag: frontend)
static nec_state_t *sChipsPtr;

NEC_INLINE void prefetch(nec_state_t *nec_state)
{
	nec_state->prefetch_count--;
}

// ReiREM port: sh = log2(prefetch_cycles) for 1/2/4, else -1 - computed once per run (rei_pf_sh)
static inline int rei_pf_sh(int c) { return (c == 1) ? 0 : (c == 2) ? 1 : (c == 4) ? 2 : -1; }
static void do_prefetch(nec_state_t *nec_state, int previous_ICount, int sh)
{
	int diff = previous_ICount - (int) nec_state->icount;

	/* The implementation is not accurate, but comes close.
     * It does not respect that the V30 will fetch two bytes
     * at once directly, but instead uses only 2 cycles instead
     * of 4. There are however only very few sources publicly
     * available and they are vague.
     */
	const int c = nec_state->prefetch_cycles;
	if (sh >= 0) {
		// ReiREM Vita port: closed form of the loops below for 1/2/4 cycles per byte
		// (V33 = 1), identical results without per-byte iterations.
		int k = -nec_state->prefetch_count;
		if (k > 0) {
			int m = (diff > 0) ? ((diff - 1) >> sh) : 0; // iterations that still find diff > c
			if (m > k) m = k;
			diff -= m << sh;
			nec_state->icount -= (k - m) << sh;
			nec_state->prefetch_count = 0;
		}
		if (nec_state->prefetch_reset) {
			nec_state->prefetch_count = 0;
			nec_state->prefetch_reset = 0;
			return;
		}
		if (diff >= c) {
			int n = diff >> sh;
			int room = nec_state->prefetch_size - nec_state->prefetch_count;
			if (room > 0) nec_state->prefetch_count += (n < room) ? n : room;
		}
		return;
	}

	while (nec_state->prefetch_count<0)
	{
		nec_state->prefetch_count++;
		if (diff>nec_state->prefetch_cycles)
			diff -= nec_state->prefetch_cycles;
		else
			nec_state->icount -= nec_state->prefetch_cycles;
	}

	if (nec_state->prefetch_reset)
	{
		nec_state->prefetch_count = 0;
		nec_state->prefetch_reset = 0;
		return;
	}

	while (diff>=nec_state->prefetch_cycles && nec_state->prefetch_count < nec_state->prefetch_size)
	{
		diff -= nec_state->prefetch_cycles;
		nec_state->prefetch_count++;
	}

}

#if defined(__psp__) || defined(REI_FETCH_CACHE)
// ReiREM PSP port: instruction bytes through a one-page cache (the 512-byte page of the last fetch). fetchop is inlined into
// the dispatch loop. A hit reads the page's own pointer directly; a miss (another page, or a page without a pointer)
// goes through the page tables as before and refills the cache. Same bytes and the same prefetch count, so results
// are unchanged. VezMapArea and the V33 code page cache clear it (rei_nec_fetch_flush). PSP perf PP19
extern "C" { UINT32 rei_fc_pg = 0xffffffffu; UINT8 *rei_fc_op, *rei_fc_arg; }
#ifdef REI_DRC
static int drc_map_dirty; // (nec_drc.inc) a V33 code page changed: inside a block, exit at the next instruction
// (nec_drc.inc) per-frame compile budget (V33 instructions). Once spent, new blocks that frame run interpreted, so
// recompiles after a cache flush do not pile into one frame (same results). PSP: about 13us per instruction
#ifdef __psp__
#define DRC_BUDGET_FRAME 2000 // (lower was slower due to interpreted loops: 19k interpreted instructions in one frame = 114ms, measured)
#else
#define DRC_BUDGET_FRAME (1 << 30)
#endif
static int drc_budget_frame = DRC_BUDGET_FRAME, drc_budget = DRC_BUDGET_FRAME;
extern "C" void rei_drc_set_budget(int n) { if (n > 0) drc_budget_frame = drc_budget = n; } // (test) per-frame compile budget
extern "C" void rei_nec_fetch_flush(void) { rei_fc_pg = 0xffffffffu; drc_map_dirty = 1; }
#else
extern "C" void rei_nec_fetch_flush(void) { rei_fc_pg = 0xffffffffu; }
#endif
static __attribute__((noinline)) UINT8 rei_fetch_miss(UINT32 a, int op)
{
	UINT8 *po = rei_nec_pg_op[a >> 9], *pa = rei_nec_pg_arg[a >> 9];
	if (po && pa) {
		rei_fc_pg = a >> 9;
		rei_fc_op = po;
		rei_fc_arg = pa;
		return op ? po[a] : pa[a];
	}
	return (*rei_nec_rh)(a); // as rei_nec_op / rei_nec_arg without a page pointer
}
// (inlined into every handler the code grew and play got 0.8ms slower: kept a call)
static __attribute__((noinline)) UINT8 fetch(nec_state_t *nec_state)
{
	prefetch(nec_state);
	const UINT32 a = (((Sreg(PS)<<4)+nec_state->ip++) ^ nec_state->fetch_xor) & 0xFFFFF;
	if ((a >> 9) == rei_fc_pg) return rei_fc_arg[a];
	return rei_fetch_miss(a, 0);
}
#define REI_FETCHOP_CACHED 1
// do_prefetch for 1 cycle per byte (sh 0, the V33), inlined into the dispatch loop: same closed form
static inline __attribute__((always_inline)) void rei_prefetch1(nec_state_t *nec_state, int previous_ICount)
{
	int diff = previous_ICount - (int) nec_state->icount;
	const int k = -nec_state->prefetch_count;
	if (k > 0) {
		int m = (diff > 0) ? diff - 1 : 0;
		if (m > k) m = k;
		diff -= m;
		nec_state->icount -= k - m;
		nec_state->prefetch_count = 0;
	}
	if (nec_state->prefetch_reset) {
		nec_state->prefetch_count = 0;
		nec_state->prefetch_reset = 0;
		return;
	}
	if (diff >= 1) {
		const int room = nec_state->prefetch_size - nec_state->prefetch_count;
		if (room > 0) nec_state->prefetch_count += (diff < room) ? diff : room;
	}
}
#else
NEC_INLINE UINT8 fetch(nec_state_t *nec_state)
{
	prefetch(nec_state);
	return cpu_readop_arg(((Sreg(PS)<<4)+nec_state->ip++) ^ nec_state->fetch_xor); // ^ fetch_xor?
}
#endif

#ifdef REI_FETCHOP_CACHED
// ReiREM PSP port: both bytes in one call when they are in the cached page (as two fetch() otherwise)
static __attribute__((noinline)) UINT16 fetchword(nec_state_t *nec_state)
{
	const UINT32 a = (((Sreg(PS)<<4)+nec_state->ip) ^ nec_state->fetch_xor) & 0xFFFFF;
	if ((a >> 9) == rei_fc_pg && (a & 0x1ff) != 0x1ff && (UINT16)(nec_state->ip + 1) != 0 && !nec_state->fetch_xor) {
		nec_state->prefetch_count -= 2;
		nec_state->ip += 2;
		return rei_fc_arg[a] | (rei_fc_arg[a + 1] << 8);
	}
	UINT16 r = FETCH();
	r |= (FETCH()<<8);
	return r;
}
#else
NEC_INLINE UINT16 fetchword(nec_state_t *nec_state)
{
	UINT16 r = FETCH();
	r |= (FETCH()<<8);
	return r;
}
#endif

// ReiREM port: REP STOSW into plain memory pages (no write handler) without the per-word handler calls.
// Same bytes, same final IY/CW, same cycles (CLKW depends only on IY's parity, constant with a step of 2).
// Not taken when DF is set, IY would wrap, or any page of the range has a handler (VRAM, I/O...).
static int rei_rep_stosw(nec_state_t *nec_state, UINT16 c)
{
	if (nec_state->DF) return 0;
	const UINT32 iy = Wreg(IY);
	if (iy + 2u * c > 0x10000u) return 0;
	const UINT32 a = ((UINT32)Sreg(DS1) << 4) + iy, end = a + 2u * c;
	if (end > 0x100000u) return 0;
	for (UINT32 pg = a >> 9; pg <= (end - 1) >> 9; pg++)
		if (!rei_nec_pg_wr[pg]) return 0;
	const UINT8 lo = (UINT8)Wreg(AW), hi = (UINT8)(Wreg(AW) >> 8);
	for (UINT32 x = a; x < end; x += 2) {
		rei_nec_pg_wr[x >> 9][x] = lo;
		rei_nec_pg_wr[(x + 1) >> 9][x + 1] = hi;
	}
	Wreg(IY) = (UINT16)(iy + 2u * c);
	const int ic0 = nec_state->icount;
	CLKW(8,8,5,8,4,3,Wreg(IY));
	nec_state->icount = ic0 - (ic0 - nec_state->icount) * (int)c;
	return 1;
}
#define REI_REP_STOSW(s, c) rei_rep_stosw(s, c)

#include "necinstr.h"
#include "necmacro.h"
#include "necea.h"
#include "necmodrm.h"
#include "rei_prof.h" // ReiREM Vita port: PC histogram hook (no-op unless REI_PCHIST)

static UINT8 parity_table[256];

#ifdef REI_FETCHOP_CACHED
static inline __attribute__((always_inline)) UINT8 fetchop(nec_state_t *nec_state)
{
	prefetch(nec_state);
	const UINT32 a = (((Sreg(PS)<<4)+nec_state->ip++) ^ nec_state->fetch_xor) & 0xFFFFF;
	if ((a >> 9) == rei_fc_pg) return rei_fc_op[a];
	return rei_fetch_miss(a, 1);
}
#else
static UINT8 fetchop(nec_state_t *nec_state)
{
	prefetch(nec_state);
	return cpu_readop(((Sreg(PS)<<4)+nec_state->ip++) ^ nec_state->fetch_xor);
}
#endif


/***************************************************************************/

void nec_set_vector_callback(UINT8 (*cb)())
{
	nec_state_t *nec_state = sChipsPtr;

	nec_state->getvector_cb = cb;
}

int nec_reset()
{
	nec_state_t *nec_state = sChipsPtr;

	memset( &nec_state->regs.w, 0, sizeof(nec_state->regs.w));

	nec_state->ip = 0;
	nec_state->TF = 0;
	nec_state->IF = 0;
	nec_state->DF = 0;
	nec_state->MF = 1;	// brkem should set to 0 when implemented
	nec_state->SignVal = 0;
	nec_state->AuxVal = 0;
	nec_state->OverVal = 0;
	nec_state->ZeroVal = 1;
	nec_state->CarryVal = 0;
	nec_state->ParityVal = 1;
	nec_state->pending_irq = 0;
	nec_state->nmi_state = 0;
	nec_state->irq_state = 0;
	nec_state->poll_state = 1;
	nec_state->halted = 0;
	nec_state->cycles_total = 0;
	nec_state->cycles_remaining = 0;
	nec_state->vector = 0xff; //default vector

	Sreg(PS) = 0xffff;
	Sreg(SS_) = 0;
	Sreg(DS0) = 0;
	Sreg(DS1) = 0;

	CHANGE_PC;

	return 0;
}

static void nec_interrupt(nec_state_t *nec_state, unsigned int_num, INTSOURCES source)
{
	UINT32 dest_seg, dest_off;

	rei_idle_restart(&rei_idle[nec_state - sChips]); // ReiREM Vita port: restart idle measurement
	rei_auto[nec_state - sChips].mark = rei_auto[nec_state - sChips].lp_mark = 0;            // M92 port: also the automatic one
	rei_rec = 0;
	rei_nec_irqs++;

	i_pushf(nec_state);
	nec_state->TF = nec_state->IF = 0;

	if (source == INT_IRQ) {	/* get vector */
		int_num = (nec_state->getvector_cb) ? nec_state->getvector_cb() : nec_state->vector;
	}

	dest_off = read_mem_word(int_num*4);
	dest_seg = read_mem_word(int_num*4+2);

	PUSH(Sreg(PS));
	PUSH(nec_state->ip);
	nec_state->ip = (WORD)dest_off;
	Sreg(PS) = (WORD)dest_seg;
	CHANGE_PC;
}

static void nec_trap(nec_state_t *nec_state)
{
	nec_instruction[fetchop(nec_state)](nec_state);
	nec_interrupt(nec_state, NEC_TRAP_VECTOR, BRK);
}

REI_LINE_FN(15) static void external_int(nec_state_t *nec_state)
{
	if (nec_state->pending_irq & NMI_IRQ)
	{
		nec_interrupt(nec_state, NEC_NMI_VECTOR, NMI_IRQ);
		nec_state->pending_irq &= ~NMI_IRQ;
	}
	else if (nec_state->pending_irq)
	{
		/* the actual vector is retrieved after pushing flags */
		/* and clearing the IF */
		nec_interrupt(nec_state, (UINT32)-1, INT_IRQ);
		nec_state->irq_state = CLEAR_LINE;
		nec_state->pending_irq &= ~INT_IRQ;
	}
}

/****************************************************************************/
/*                             OPCODES                                      */
/****************************************************************************/

#include "necinstr.c"

#if defined(REI_RECOMP) && !defined(REI_PCHIST) && !defined(REI_V33STAT)
// ReiREM Vita port: M6 V33 static recompilation (tools/recomp/v33recomp.py, ADR-008).
// Generated code is included here so it can use the handlers, macros and do_prefetch.
#include "v33_rc.inc"
#define REI_RC_ACTIVE 1
int rei_rc_enabled = 1;
#ifdef REI_RC_STATS
extern "C" { unsigned long long rei_rc_st[4]; unsigned rei_rc_miss[1 << 20]; }
#endif
static UINT8 rei_rc_map[0xa0000]; // region index + 1 for each compiled instruction start
static int rei_rc_ready;
static void rei_rc_init()
{
	for (int i = 0; i < REI_RC_NUM_ENTRIES; i++)
		rei_rc_map[rei_rc_entries[i] & 0xfffff] = (UINT8)((rei_rc_entries[i] >> 20) + 1);
	rei_rc_ready = 1;
#ifdef REI_RC_IMAGE_HASH
	// the generated code holds constants from one ROM: run it only on the same program image
	UINT64 h = 0xcbf29ce484222325ULL;
	for (UINT32 a = 0; a < 0xa0000; a++)
		h = (h ^ cpu_readmem20(a)) * 0x100000001b3ULL;
	if (h != REI_RC_IMAGE_HASH) {
		rei_rc_enabled = 0;
		bprintf(PRINT_IMPORTANT, _T("recomp: ROM image differs from the generated code, using the interpreter\n"));
	}
#endif
}
#else
int rei_rc_enabled = 0;
#endif

/*****************************************************************************/

void nec_set_irq_line(int state) // for use with vector callback
{
	nec_state_t *nec_state = sChipsPtr;

	nec_state->irq_state = state;
	if (state == CLEAR_LINE)
		nec_state->pending_irq &= ~INT_IRQ;
	else
	{
		nec_state->pending_irq |= INT_IRQ;
		nec_state->halted = 0;
	}
}

void nec_set_irq_line_and_vector(int irqline, int vector, int state)
{
	nec_state_t *nec_state = sChipsPtr;

	switch (irqline)
	{
		case 0:
			nec_state->irq_state = state;
			if (state == CLEAR_LINE)
				nec_state->pending_irq &= ~INT_IRQ;
			else
			{
				nec_state->vector = vector;
				nec_state->pending_irq |= INT_IRQ;
				nec_state->halted = 0;
			}
			break;
		case INPUT_LINE_NMI:
			if (nec_state->nmi_state == (unsigned int)state) return;
		    	nec_state->nmi_state = state;
			if (state != CLEAR_LINE)
			{
				nec_state->vector = vector;
				nec_state->pending_irq |= NMI_IRQ;
				nec_state->halted = 0;
			}
			break;
		case NEC_INPUT_LINE_POLL:
			nec_state->vector = vector;
			nec_state->poll_state = state;
			break;
	}
}

void nec_init(int cpu)
{
	nec_state_t *nec_state = &sChips[cpu];

	unsigned int i, j, c;

	static const WREGS wreg_name[8]={ AW, CW, DW, BW, SP, BP, IX, IY };
	static const BREGS breg_name[8]={ AL, CL, DL, BL, AH, CH, DH, BH };

	for (i = 0; i < 256; i++)
	{
		for (j = i, c = 0; j > 0; j >>= 1)
			if (j & 1) c++;
		parity_table[i] = !(c & 1);
	}

	for (i = 0; i < 256; i++)
	{
		Mod_RM.reg.b[i] = breg_name[(i & 0x38) >> 3];
		Mod_RM.reg.w[i] = wreg_name[(i & 0x38) >> 3];
	}

	for (i = 0xc0; i < 0x100; i++)
	{
		Mod_RM.RM.w[i] = wreg_name[i & 7];
		Mod_RM.RM.b[i] = breg_name[i & 7];
	}

	memset(nec_state, 0, sizeof(*nec_state));
}

void necCpuOpen(int cpu)
{
	sChipsPtr = &sChips[cpu];
}

void necCpuClose()
{
	sChipsPtr = NULL;
}

unsigned int nec_total_cycles()
{
	return sChipsPtr->cycles_total + (sChipsPtr->cycles_remaining - sChipsPtr->icount);
}

// ReiREM port: cycle position at the start of the instruction the interpreter is running (for handlers that
// need to know which scanline an instruction belongs to - d_m92.cpp line spans)
static int rei_ins_ic;
unsigned int nec_ins_start_cycles()
{
	return sChipsPtr->cycles_total + (sChipsPtr->cycles_remaining - rei_ins_ic);
}

void nec_new_frame()
{
#ifdef REI_DRC
	drc_budget = drc_budget_frame;
#endif
	for (int i = 0; i < 4; i++) {
		nec_state_t *nec_state = &sChips[i];
		nec_state->cycles_total = 0;
	}
}

void nec_set_vector(int vector)
{
	nec_state_t *nec_state = sChipsPtr;
	nec_state->vector = vector;
}

void necRunEnd()
{
	sChipsPtr->stop_run = 1;
}

void necIdle(int cycles)
{
	sChipsPtr->cycles_total += cycles;
}

// ReiREM PSP port: fast path for the game's sprite-slot scan loop (V33 0x2434..0x24d7, ~half of the
// V33 time in play). Each iteration over an empty slot (0xffff) is:
//   push cx / push bx / test word ss:[1af4],ffff / jne / mov bp,ss:[bx] / cmp bp,ffff / jne / jmp 24cf /
//   pop bx / add bx,2 / pop cx / dec cx / je / jmp 2434
// and costs 81 cycles with the prefetch queue in its steady state (count 0, no reset) at the loop head
// (measured with the interpreter). m whole empty iterations are applied at once: registers, the two stack
// words, flags of the last add/dec and the cycles are exactly what the interpreter leaves. Anything
// unusual (other code bytes, prefetch state, pending interrupt shadow, end of the time slice, the exit
// iteration, overlapping stack/table) falls back to the interpreter, so the result is identical.
// The code is checked by hashes (0x2434..0x244c, 0x24cf..0x24d9) so the game bytes are not kept in the source.
#define REI_SCAN_PC   0x2434
#define REI_SCAN_CYC  81
int rei_scan_enabled = 1;
static int rei_scan_state; // 0 = code not checked, 1 = matches, -1 = different code (off)
static inline UINT16 rei_rdw(UINT32 a) { return cpu_readmem20(a) | (cpu_readmem20(a + 1) << 8); }
// ReiREM PSP port: the fast paths below live in their own section, placed after the interpreter by
// psp/me.ld, so changing them does not move the interpreter's hot code (PSP 16KB instruction cache)
// Their tables go to .reifast_data, placed after .bss, so that growing them does not move other data either.
#ifdef __psp__
#define REI_FAST_FN __attribute__((noinline, section(".reifast_text")))
#define REI_FAST_DATA __attribute__((section(".reifast_data")))
#else
#define REI_FAST_FN __attribute__((noinline))
#define REI_FAST_DATA
#endif
static REI_FAST_FN UINT32 rei_code_hash(UINT32 a, UINT32 n)
{
	UINT32 h = 0x811c9dc5u;
	for (UINT32 i = 0; i < n; i++)
		h = (h ^ cpu_readmem20_op(a + i)) * 0x01000193u;
	return h;
}
static REI_FAST_FN void rei_scan_skip(nec_state_t *nec_state)
{
	if (rei_scan_state == 0)
		rei_scan_state = (rei_code_hash(0x2434, 25) == 0xf7a44854u && rei_code_hash(0x24cf, 11) == 0x4329f0e3u) ? 1 : -1;
	if (rei_scan_state < 0 || nec_state->prefetch_count != 0 || nec_state->prefetch_reset || nec_state->no_interrupt)
		return;
	const UINT32 ss = Sreg(SS_) << 4;
	const UINT32 sp = Wreg(SP), cx = Wreg(CW), bx = Wreg(BW);
	if (sp < 4 || rei_rdw(ss + 0x1af4) == 0)
		return; // (exits through 24fb) - interpreter
	INT32 maxm = (nec_state->icount - 1) / REI_SCAN_CYC; // icount stays > 0 through every skipped iteration
	if ((INT32)cx - 2 < maxm) maxm = (INT32)cx - 2;     // keep the exit iteration (cx -> 0) for the interpreter
	INT32 m = 0;
	while (m < maxm && bx + 2 * m + 1 <= 0xffff && rei_rdw(ss + bx + 2 * m) == 0xffff)
		m++;
	if (m <= 0)
		return;
	// the stack words the loop writes must not be among the slots or [1af4] it reads
	const UINT32 st0 = ss + sp - 4, st1 = ss + sp; // [st0, st1)
	const UINT32 tb0 = ss + bx, tb1 = ss + bx + 2 * m;
	if ((st0 < tb1 && tb0 < st1) || (st0 < ss + 0x1af6 && ss + 0x1af4 < st1))
		return;
	const UINT32 last_cx = cx - (m - 1), last_bx = bx + 2 * (m - 1);
	cpu_writemem20(ss + sp - 2, last_cx & 0xff);      // push cx
	cpu_writemem20(ss + sp - 1, last_cx >> 8);
	cpu_writemem20(ss + sp - 4, last_bx & 0xff);      // push bx
	cpu_writemem20(ss + sp - 3, last_bx >> 8);
	Wreg(BP) = 0xffff;                                 // mov bp,ss:[bx]
	{ UINT32 dst = last_bx, src = 2, res = dst + src;  // add bx,2
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(BW) = (WORD)res; }
	Wreg(CW) = last_cx;                                // dec cx
	{ DecWordReg(CW); }
	nec_state->icount -= m * REI_SCAN_CYC;
}

// The rest of an empty-slot iteration one instruction at a time: the whole-iteration skip above stops when the
// time slice has less than one iteration left (a scanline is ~570 cycles, the scan ~72 lines), so every line
// used to end and start with an interpreted iteration. Each of the 14 instructions is done natively with its
// branch condition checked on the real state; its cost and the prefetch state it leaves come from the
// interpreter (the first time an (instruction, prefetch state, sp/bx parity) is met the real handler runs it
// and is measured). Steps stop exactly where the interpreter would (icount <= 0), and the next slice resumes
// mid-iteration (rei_scan_run at the start of nec_execute).
static const UINT16 rei_scan_pcs[14] = { 0x2434, 0x2435, 0x2436, 0x243d, 0x2442, 0x2445, 0x2448, 0x244a,
	0x24cf, 0x24d0, 0x24d3, 0x24d4, 0x24d5, 0x24d7 };
static struct rei_step_cost { INT16 d; INT8 pf; UINT8 rst; } rei_scan_tab[14][16][2][4] REI_FAST_DATA;
static unsigned rei_scan_stats[3]; // native steps, learned, disagreements
static inline int rei_scan_index(UINT32 pc)
{
	if (pc - 0x2434 > 0x24d7 - 0x2434)
		return -1;
	for (int k = 0; k < 14; k++)
		if (rei_scan_pcs[k] == pc)
			return k;
	return -1;
}
// the native effect of step k with its learned cost
static REI_FAST_FN void rei_scan_apply(nec_state_t *nec_state, int k, const struct rei_step_cost *e)
{
	const UINT32 ss = Sreg(SS_) << 4;
	switch (k) {
	case 0: Wreg(SP) -= 2; write_mem_word(ss + Wreg(SP), Wreg(CW)); break;  // push cx
	case 1: Wreg(SP) -= 2; write_mem_word(ss + Wreg(SP), Wreg(BW)); break;  // push bx
	case 2: { UINT32 dst = rei_rdw(ss + 0x1af4);                             // test word ss:[1af4],ffff
		nec_state->CarryVal = nec_state->OverVal = 0; SetSZPF_Word(dst); break; } // (this core's TEST keeps AF)
	case 3: break;                                                           // jne 2442 (taken)
	case 4: Wreg(BP) = rei_rdw(ss + Wreg(BW)); break;                        // mov bp,ss:[bx]
	case 5: { UINT32 dst = Wreg(BP), src = 0xffff, res = dst - src;          // cmp bp,ffff
		SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); break; }
	case 6: break;                                                           // jne 244d (not taken)
	case 7: break;                                                           // jmp 24cf
	case 8: Wreg(SP) += 2; Wreg(BW) = rei_rdw(ss + ((Wreg(SP) - 2) & 0xffff)); break; // pop bx
	case 9: { UINT32 dst = Wreg(BW), src = 2, res = dst + src;               // add bx,2
		SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(BW) = (WORD)res; break; }
	case 10: Wreg(SP) += 2; Wreg(CW) = rei_rdw(ss + ((Wreg(SP) - 2) & 0xffff)); break; // pop cx
	case 11: { DecWordReg(CW); break; }                                      // dec cx
	case 12: break;                                                          // je 24da (not taken)
	case 13: break;                                                          // jmp 2434
	}
	nec_state->ip = (WORD)(nec_state->ip + (rei_scan_pcs[(k + 1) % 14] - rei_scan_pcs[k]));
	nec_state->icount -= e->d;
	nec_state->prefetch_count = e->pf;
	nec_state->prefetch_reset = e->rst;
}

// one step of the empty-slot path, k = index of the instruction at PC. 0 = leave it to the interpreter
static REI_FAST_FN int rei_scan_step(nec_state_t *nec_state, int k, int pf_sh)
{
	// branch conditions of the empty-slot path (anything else runs in the interpreter)
	if ((k == 3 && ZF) || (k == 6 && !ZF) || (k == 12 && ZF))
		return 0;
	if (nec_state->prefetch_count < -8 || nec_state->prefetch_count > 7)
		return 0;
	struct rei_step_cost *e = &rei_scan_tab[k][(nec_state->prefetch_count + 8) & 15][nec_state->prefetch_reset & 1]
					   [(Wreg(SP) & 1) | (Wreg(BW) & 1) << 1];
	if (e->d == 0) {
		// learn: the real handler runs this instruction, exactly like the loop in nec_execute
		const int prev = nec_state->icount;
		nec_instruction[fetchop(nec_state)](nec_state);
		do_prefetch(nec_state, prev, pf_sh);
		const UINT32 next = rei_scan_pcs[(k + 1) % 14];
		if (PC(nec_state) != next) { // not the path it should have taken
			rei_scan_stats[2]++;
			rei_scan_state = -1;
			return 0;
		}
		e->d = (INT16)(prev - nec_state->icount);
		e->pf = nec_state->prefetch_count;
		e->rst = nec_state->prefetch_reset;
		rei_scan_stats[1]++;
		return 1;
	}
#ifdef REI_SCAN_VERIFY
	{ // the native step on a copy against the real handler on the real state
		nec_state_t copy = *nec_state;
		rei_scan_apply(&copy, k, e);
		const int prev = nec_state->icount;
		nec_instruction[fetchop(nec_state)](nec_state);
		do_prefetch(nec_state, prev, pf_sh);
		copy.prefix_base = nec_state->prefix_base;
		if (memcmp(&copy, nec_state, sizeof(copy))) {
			static int shown;
			if (shown++ < 10) {
				fprintf(stderr, "scan verify k%d: ip %04x/%04x ic %d/%d pf %d/%d rst %d/%d A%x/%x O%x/%x Z%x/%x C%x/%x S%x/%x P%x/%x",
					k, copy.ip, nec_state->ip, copy.icount, nec_state->icount, copy.prefetch_count, nec_state->prefetch_count,
					copy.prefetch_reset, nec_state->prefetch_reset, copy.AuxVal, nec_state->AuxVal, copy.OverVal, nec_state->OverVal,
					copy.ZeroVal, nec_state->ZeroVal, copy.CarryVal, nec_state->CarryVal, (unsigned)copy.SignVal,
					(unsigned)nec_state->SignVal, copy.ParityVal, nec_state->ParityVal);
				for (int r = 0; r < 8; r++)
					if (copy.regs.w[r] != nec_state->regs.w[r]) fprintf(stderr, " r%d %04x/%04x", r, copy.regs.w[r], nec_state->regs.w[r]);
				fputc(10, stderr);
			}
		}
		rei_scan_stats[0]++;
		return 1;
	}
#endif
	rei_scan_apply(nec_state, k, e);
	rei_scan_stats[0]++;
	return 1;
}
// at an instruction of the empty-slot path: whole iterations, then single steps up to the end of the slice
static REI_FAST_FN void rei_scan_run(nec_state_t *nec_state, int pf_sh)
{
	if (rei_scan_state == 0)
		rei_scan_state = (rei_code_hash(0x2434, 25) == 0xf7a44854u && rei_code_hash(0x24cf, 11) == 0x4329f0e3u) ? 1 : -1;
	if (rei_scan_state < 0 || nec_state->no_interrupt || nec_state->seg_prefix)
		return;
	for (;;) {
		const UINT32 pc = PC(nec_state);
		if (pc == REI_SCAN_PC)
			rei_scan_skip(nec_state);
		const int k = rei_scan_index(PC(nec_state));
		if (k < 0 || nec_state->icount <= 0 || rei_scan_state < 0)
			return;
		if (k == 0) { // only empty slots with the list not full (the path of the steps)
			const UINT32 ss = Sreg(SS_) << 4;
			if (rei_rdw(ss + 0x1af4) == 0 || rei_rdw(ss + Wreg(BW)) != 0xffff || Wreg(CW) == 1)
				return;
		}
		if (!rei_scan_step(nec_state, k, pf_sh) || nec_state->icount <= 0)
			return;
	}
}

void nec_get_scan_stats(unsigned *native, unsigned *learned, unsigned *bad)
{
	*native = rei_scan_stats[0];
	*learned = rei_scan_stats[1];
	*bad = rei_scan_stats[2];
}

// ReiREM PSP port: fast path for the object flag loop at V33 0x1f78..0x1f9c (~10% of the V33 time in play).
// One iteration is
//   push cx / test byte [bp+6],1 / je / (call) / test byte [bp+6],2 / je / (call) / pop cx / add bp,8 /
//   mov ax,es / add ax,2 / mov es,ax / loop 1f78
// and almost every entry has both bits clear (no call). Such an iteration costs 50 cycles with the prefetch
// queue full (count 6, no reset) at the loop head (measured with the interpreter: the only steady state seen).
// m clear iterations whose loop is taken are applied at once like the slot scan above; any other case runs
// in the interpreter. The code is checked by a hash so the game bytes are not kept in the source.
#define REI_FLAGS_PC   0x1f78
#define REI_FLAGS_LEN  38
#define REI_FLAGS_HASH 0xbcab3fc9u
#define REI_FLAGS_CYC  50
static int rei_flags_state; // 0 = code not checked, 1 = matches, -1 = different code (off)
static REI_FAST_FN void rei_flags_skip(nec_state_t *nec_state)
{
	if (rei_flags_state == 0) {
		UINT32 h = 0x811c9dc5u;
		for (UINT32 i = 0; i < REI_FLAGS_LEN; i++)
			h = (h ^ cpu_readmem20_op(REI_FLAGS_PC + i)) * 0x01000193u;
		rei_flags_state = h == REI_FLAGS_HASH ? 1 : -1;
	}
	if (rei_flags_state < 0 || nec_state->prefetch_count != 6 || nec_state->prefetch_reset || nec_state->no_interrupt)
		return;
	const UINT32 ss = Sreg(SS_) << 4;
	const UINT32 sp = Wreg(SP), cx = Wreg(CW), bp = Wreg(BP), es = Sreg(DS1);
	if (sp < 2 || cx < 2)
		return;
	INT32 maxm = (nec_state->icount - 1) / REI_FLAGS_CYC; // icount stays > 0 through every skipped iteration
	if ((INT32)cx - 1 < maxm) maxm = (INT32)cx - 1;       // the loop is taken after each skipped iteration
	const UINT32 st0 = ss + sp - 2;                       // the word push cx writes
	INT32 m = 0;
	while (m < maxm) {
		const UINT32 off = bp + 8 * m + 6;
		if (off > 0xffff)
			break;
		const UINT32 a = ss + off;
		if (a == st0 || a == st0 + 1 || (cpu_readmem20(a) & 3))
			break;
		m++;
	}
	if (m <= 0)
		return;
	const UINT32 last_cx = cx - (m - 1), last_es = (es + 2 * (m - 1)) & 0xffff;
	cpu_writemem20(st0, last_cx & 0xff);              // push cx
	cpu_writemem20(st0 + 1, last_cx >> 8);
	Wreg(BP) = (WORD)(bp + 8 * m);                    // add bp,8 (its flags are overwritten below)
	{ UINT32 dst = last_es, src = 2, res = dst + src; // mov ax,es / add ax,2 / mov es,ax
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(AW) = (WORD)res; Sreg(DS1) = (WORD)res; }
	Wreg(CW) = (WORD)(cx - m);                        // loop
	nec_state->icount -= m * REI_FLAGS_CYC;
}

// ReiREM PSP port: fast path for the sprite piece loop of the sprite list builder (V33 0x2445..0x24ce,
// ~25% of the V33 time in play). Three pieces of straight code, each from one hook address to the next:
//  LOOP / LAST (0x2468 -> 0x2468 / 0x24c9): one piece at ds:si becomes 4 sprite words at es:di, clipped
//   against 0x180 / 0x200:
//   mov ax,[si] / mov dx,[bp+6] / bh,bl <- ah / and bl,6 / ror ah,1 / sbb ah,ah / rol bh,2 / and bh,3 /
//   mov ch,dh / xor dh,bh / test ch,2 / je A / (xor bh,bh / neg ax / sub ax,cs:[2410+bx]) /
//   A: add ax,[bp+4] / cmp ax,180 / jae B / or ah,bl / mov bx,[si+4] / test ch,1 / je C / (neg bx / sub bx,10) /
//   C: add bx,[bp+2] / cmp bx,200 / jae B / stosw ax,[si+2],dx,bx / dec word ss:[1af4] / je (exit) /
//   B: add si,6 / dec cl / jne 2468          (LAST: cl was 1, falls through to 0x24c9)
//  NEXT (0x24c9 -> 0x2468): the next object of the slot:
//   mov bp,[bp] / jmp 2445 / cmp bp,ffff / jne 244d / mov si,[bp+8] / mov ax,8000 / mov dx,2 / test si,1 /
//   je 2462 / (mov ax,9000 / mov dx,1) / mov ds,ax / mov cl,[si] / add si,dx
// The branches taken and the parity of the word addresses (access timing) form a signature. The cost of a
// (piece, signature, prefetch state) and the prefetch state it leaves are taken from the interpreter: the first
// time one is seen the interpreter runs it and it is measured between the two hook visits in the same time
// slice with no interrupt. Known ones are done natively, with the memory accesses in the same order, and only
// while icount stays > 0 (like the loops above), so registers, flags, memory, cycles and interrupt timing are
// the interpreter's. A measurement that disagrees with a stored one turns the fast path off. Anything else
// (end of the slot or of the sprite list, stores outside plain RAM, ...) runs in the interpreter.
#define REI_PIECE_PC   0x2468
#define REI_NEXT_PC    0x24c9
#define REI_PIECE_CODE 0x2445
#define REI_PIECE_LEN  138 // 0x2445..0x24ce
#define REI_PIECE_HASH 0xba028edbu
enum { REI_SEG_LOOP, REI_SEG_LAST, REI_SEG_NEXT };
static int rei_piece_state; // 0 = code not checked, 1 = on, -1 = off
static unsigned rei_piece_stats[3]; // native pieces, learned costs, disagreements
static struct rei_piece_cost { INT16 d; INT8 pf; UINT8 rst; } rei_piece_tab[1 << 13] REI_FAST_DATA;
static struct { int on; UINT32 key, end_pc; int icount; UINT32 slice, irq; } rei_piece_pend;

struct rei_piece_out { UINT16 ax, bx, cx, dx, si, di, bp, ds, w[4]; int store, seg; };

static inline int rei_plain_wr(UINT32 a) // a word store that goes straight to RAM (no handler)
{
	return rei_nec_pg_wr[(a & 0xfffff) >> 9] && rei_nec_pg_wr[((a + 1) & 0xfffff) >> 9];
}

// one piece without side effects: the signature, or -1 if it needs the interpreter
static REI_FAST_FN int rei_piece_sim(nec_state_t *nec_state, struct rei_piece_out *o)
{
	const UINT32 ds = Sreg(DS0) << 4, ss = Sreg(SS_) << 4, es = Sreg(DS1) << 4, cs = Sreg(PS) << 4;
	const UINT32 si = Wreg(IX), bp = Wreg(BP), di = Wreg(IY), cx = Wreg(CW);
	if (si > 0xfff9 || bp > 0xfff8 || di > 0xfff8)
		return -1;
	UINT32 ax = rei_rdw(ds + si);
	UINT32 dx = rei_rdw(ss + bp + 6);
	const UINT32 ah = ax >> 8;
	const UINT32 bl = ah & 6;
	UINT32 bh = ((ah << 2) | (ah >> 6)) & 3;
	ax = (ax & 0xff) | ((ah & 1) ? 0xff00 : 0);       // ror ah,1 / sbb ah,ah
	const UINT32 ch = dx >> 8;
	dx ^= bh << 8;                                    // xor dh,bh
	int sig = 0;
	if (ch & 2) {
		sig |= 1;
		bh = 0;
		ax = (0u - ax) & 0xffff;                      // neg ax
		ax = (ax - rei_rdw(cs + ((0x2410 + bl) & 0xffff))) & 0xffff;
	}
	ax = (ax + rei_rdw(ss + bp + 4)) & 0xffff;
	UINT32 bx = (bh << 8) | bl;
	o->store = 0;
	if (ax >= 0x180) {
		sig |= 2;
	} else {
		ax |= bl << 8;                                // or ah,bl
		bx = rei_rdw(ds + si + 4);
		if (ch & 1) {
			sig |= 4;
			bx = ((0u - bx) - 0x10) & 0xffff;
		}
		bx = (bx + rei_rdw(ss + bp + 2)) & 0xffff;
		if (bx >= 0x200) {
			sig |= 8;
		} else {
			// the stores must not touch what is read after them ([si+2], [1af4]) and must go to plain RAM
			const UINT32 w0 = es + di, w1 = es + di + 8;
			const UINT32 r0 = ds + si + 2, r1 = ss + 0x1af4;
			if ((r0 + 2 > w0 && r0 < w1) || (r1 + 2 > w0 && r1 < w1) || rei_rdw(r1) == 1 || nec_state->DF ||
			    !rei_plain_wr(w0) || !rei_plain_wr(w0 + 6) || !rei_plain_wr(r1))
				return -1;
			o->store = 1;
			o->w[0] = ax;
			o->w[1] = rei_rdw(r0);
			o->w[2] = dx;
			o->w[3] = bx;
			ax = bx;
		}
	}
	sig |= (si & 1) << 4 | (bp & 1) << 5 | (di & 1) << 6;
	o->seg = (cx & 0xff) == 1 ? REI_SEG_LAST : REI_SEG_LOOP;
	o->ax = ax;
	o->bx = bx;
	o->cx = (ch << 8) | ((cx - 1) & 0xff);
	o->dx = dx;
	o->si = si + 6;
	o->di = o->store ? di + 8 : di;
	return sig;
}

// the step to the next object of the slot (0x24c9 -> 0x2468)
static REI_FAST_FN int rei_next_sim(nec_state_t *nec_state, struct rei_piece_out *o)
{
	const UINT32 ss = Sreg(SS_) << 4;
	const UINT32 bp = Wreg(BP);
	if (bp > 0xfffe)
		return -1;
	const UINT32 nbp = rei_rdw(ss + bp);              // mov bp,[bp]
	if (nbp == 0xffff || nbp > 0xfff6)                 // end of the slot -> 0x24cf (interpreter)
		return -1;
	const UINT32 nsi = rei_rdw(ss + nbp + 8);         // mov si,[bp+8]
	const UINT32 ax = (nsi & 1) ? 0x9000 : 0x8000, dx = (nsi & 1) ? 1 : 2;
	o->seg = REI_SEG_NEXT;
	o->bp = nbp;
	o->si = nsi;
	o->ax = ax;
	o->dx = dx;
	o->ds = ax;
	o->cx = (Wreg(CW) & 0xff00) | cpu_readmem20((ax << 4) + nsi); // mov ds,ax / mov cl,[si]
	return (int)((bp & 1) | (nbp & 1) << 1 | (nsi & 1) << 2);
}

static inline UINT32 rei_piece_key(nec_state_t *nec_state, int seg, int sig)
{
	return (UINT32)sig | (UINT32)(nec_state->prefetch_count & 7) << 7 | (UINT32)(nec_state->prefetch_reset & 1) << 10 |
	       (UINT32)seg << 11;
}

// at 0x2468 or 0x24c9
static REI_FAST_FN void rei_piece_skip(nec_state_t *nec_state, UINT32 pc)
{
	if (rei_piece_state == 0) {
		UINT32 h = 0x811c9dc5u;
		for (UINT32 i = 0; i < REI_PIECE_LEN; i++)
			h = (h ^ cpu_readmem20_op(REI_PIECE_CODE + i)) * 0x01000193u;
		rei_piece_state = h == REI_PIECE_HASH ? 1 : -1;
	}
	if (rei_piece_state < 0)
		return;
	// the piece measured since the last visit
	if (rei_piece_pend.on && rei_piece_pend.end_pc == pc && rei_piece_pend.slice == rei_nec_slice &&
	    rei_piece_pend.irq == rei_nec_irqs) {
		struct rei_piece_cost *e = &rei_piece_tab[rei_piece_pend.key];
		const int d = rei_piece_pend.icount - nec_state->icount;
		if (e->d == 0) {
			e->d = (INT16)d;
			e->pf = nec_state->prefetch_count;
			e->rst = nec_state->prefetch_reset;
			rei_piece_stats[1]++;
		} else if (e->d != d || e->pf != nec_state->prefetch_count || e->rst != nec_state->prefetch_reset) {
			rei_piece_stats[2]++;
			rei_piece_state = -1;
			return;
		}
	}
	rei_piece_pend.on = 0;
	if (nec_state->no_interrupt || nec_state->seg_prefix)
		return;
	for (;;) {
		if (nec_state->prefetch_count < 0 || nec_state->prefetch_count > 7)
			return;
		struct rei_piece_out o;
		const int sig = pc == REI_PIECE_PC ? rei_piece_sim(nec_state, &o) : rei_next_sim(nec_state, &o);
		if (sig < 0)
			return;
		const UINT32 key = rei_piece_key(nec_state, o.seg, sig);
		const UINT32 end_pc = o.seg == REI_SEG_LAST ? REI_NEXT_PC : REI_PIECE_PC;
		const struct rei_piece_cost *e = &rei_piece_tab[key];
		if (e->d == 0 || nec_state->icount - e->d < 1) {
			// the interpreter runs this one; measure it unless it is cut by the end of the slice
			rei_piece_pend.on = 1;
			rei_piece_pend.key = key;
			rei_piece_pend.end_pc = end_pc;
			rei_piece_pend.icount = nec_state->icount;
			rei_piece_pend.slice = rei_nec_slice;
			rei_piece_pend.irq = rei_nec_irqs;
			return;
		}
		if (o.seg == REI_SEG_NEXT) {
			Wreg(BP) = o.bp;
			Wreg(AW) = o.ax;
			Wreg(DW) = o.dx;
			Sreg(DS0) = o.ds;
			Wreg(CW) = o.cx;
			{ UINT32 dst = o.si, src = o.dx, res = dst + src; // add si,dx
			  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(IX) = (WORD)res; }
		} else {
			if (o.store) {
				const UINT32 es = Sreg(DS1) << 4, di = Wreg(IY);
				for (int k = 0; k < 4; k++)
					write_mem_word(es + di + 2 * k, o.w[k]);
				const UINT32 a = (Sreg(SS_) << 4) + 0x1af4;    // dec word ss:[1af4] (not to zero: checked)
				write_mem_word(a, (UINT16)(rei_rdw(a) - 1));
			}
			Wreg(AW) = o.ax;
			Wreg(BW) = o.bx;
			Wreg(DW) = o.dx;
			Wreg(IY) = o.di;
			{ UINT32 dst = Wreg(IX), src = 6, res = dst + src; // add si,6
			  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(IX) = (WORD)res; }
			{ UINT32 tmp = Wreg(CW) & 0xff, tmp1 = tmp - 1;    // dec cl
			  nec_state->OverVal = (tmp == 0x80); SetAF(tmp1, tmp, 1); SetSZPF_Byte(tmp1); }
			Wreg(CW) = o.cx;
		}
		nec_state->ip = (WORD)(nec_state->ip + (end_pc - pc));
		pc = end_pc;
		nec_state->icount -= e->d;
		nec_state->prefetch_count = e->pf;
		nec_state->prefetch_reset = e->rst;
		rei_piece_stats[0]++;
	}
}

void nec_get_piece_stats(unsigned *native, unsigned *learned, unsigned *bad)
{
	*native = rei_piece_stats[0];
	*learned = rei_piece_stats[1];
	*bad = rei_piece_stats[2];
}

// ReiREM PSP port: fast path for a whole call of the game's player-object check at V33 0x3ef04 (far
// called, returns with retf; CF = result). It only reads the object at ss:bp (one of the four players
// 0x4480..0x46c0, stride 0xc0):
//   cmp bp,4480 / jb no / cmp bp,46c0 / ja no / cmp bp,{4480,4540,4600,46c0} / je ok (else int3) /
//   ok: cmp [bp],2953 / jne A / cmp [bp+2],0 / A: je no / cmp [bp+48],d / je no / cmp [bp+48],c / je no /
//   cmp [bp+48],4 / je no / test [bp+c],2000 / jne no / test [bp+4c],8 / jne no / stc / retf / no: clc / retf
// The branches taken form the path; with the parity of bp and sp (word access timing) and the prefetch state
// it keys the cost and the prefetch state after the retf. Those come from the interpreter: an unknown key is
// run by the real handlers here (up to the retf) and measured. Known ones are done natively: the flags of the
// last compare or test, CF from clc/stc, the far return; only while icount stays > 0. The int3 path and
// anything else run in the interpreter.
#define REI_PCHECK_PC   0x3ef04
#define REI_PCHECK_HASH 0xdb3b5a3bu // 0x3ef04..0x3ef5a
static int rei_pcheck_state;
static unsigned rei_pcheck_stats[3]; // native calls, learned, disagreements
static struct rei_pcheck_cost { UINT32 key; INT16 d; INT8 pf; UINT8 rst; } rei_pcheck_tab[256] REI_FAST_DATA;
struct rei_flags_w { UINT32 C, O, A, Z, P; INT32 S; };
static inline void rei_fw_sub(struct rei_flags_w *f, UINT32 dst, UINT32 src) // SUBW (cmp)
{
	UINT32 res = dst - src;
	f->C = res & 0x10000;
	f->O = ((dst ^ src) & (dst ^ res)) & 0x8000;
	f->A = (res ^ (src ^ dst)) & 0x10;
	f->S = f->Z = f->P = (INT16)res;
}
static inline void rei_fw_test(struct rei_flags_w *f, UINT32 v) // TEST word (AF kept)
{
	f->C = f->O = 0;
	f->S = f->Z = f->P = (INT16)v;
}
// the path of one call (branch outcomes, 1-prefixed) and its flags/result; -1 = interpreter
static REI_FAST_FN int rei_pcheck_sim(nec_state_t *nec_state, struct rei_flags_w *f, int *carry)
{
	const UINT32 ss = Sreg(SS_) << 4, bp = Wreg(BP);
	f->A = nec_state->AuxVal;
	UINT32 path = 1;
#define REI_BR(c) (path = path * 2 + ((c) ? 1 : 0), (c))
	rei_fw_sub(f, bp, 0x4480);
	if (REI_BR(bp < 0x4480)) goto no;                 // jb
	rei_fw_sub(f, bp, 0x46c0);
	if (REI_BR(bp > 0x46c0)) goto no;                 // ja
	{
		static const UINT16 pl[4] = { 0x4480, 0x4540, 0x4600, 0x46c0 };
		int k = 0;
		for (; k < 4; k++) {
			rei_fw_sub(f, bp, pl[k]);
			if (REI_BR(bp == pl[k])) break;           // je
		}
		if (k == 4)
			return -1;                                // int3
	}
	{
		const UINT32 v0 = rei_rdw(ss + bp);
		rei_fw_sub(f, v0, 0x2953);
		if (!REI_BR(v0 != 0x2953)) {                  // jne A
			const UINT32 v2 = rei_rdw(ss + ((bp + 2) & 0xffff));
			rei_fw_sub(f, v2, 0);
		}
		if (REI_BR(f->Z == 0)) goto no;               // A: je no
		const UINT32 v48 = rei_rdw(ss + ((bp + 0x48) & 0xffff));
		rei_fw_sub(f, v48, 0xd);
		if (REI_BR(v48 == 0xd)) goto no;
		rei_fw_sub(f, v48, 0xc);
		if (REI_BR(v48 == 0xc)) goto no;
		rei_fw_sub(f, v48, 4);
		if (REI_BR(v48 == 4)) goto no;
		rei_fw_test(f, rei_rdw(ss + ((bp + 0xc) & 0xffff)) & 0x2000);
		if (REI_BR(f->Z != 0)) goto no;               // jne no
		rei_fw_test(f, rei_rdw(ss + ((bp + 0x4c) & 0xffff)) & 8);
		if (REI_BR(f->Z != 0)) goto no;
		*carry = 1;                                   // stc
		return (int)path;
	}
no:
	*carry = 0;                                       // clc
	return (int)path;
#undef REI_BR
}

static REI_FAST_FN void rei_pcheck_run(nec_state_t *nec_state, int pf_sh)
{
	if (rei_pcheck_state == 0)
		rei_pcheck_state = rei_code_hash(REI_PCHECK_PC, 87) == REI_PCHECK_HASH ? 1 : -1;
	if (rei_pcheck_state < 0 || nec_state->no_interrupt || nec_state->seg_prefix || nec_state->prefetch_count < -8 ||
	    nec_state->prefetch_count > 7)
		return;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (sp > 0xfffc)
		return;
	struct rei_flags_w f;
	int carry = 0;
	const int path = rei_pcheck_sim(nec_state, &f, &carry);
	if (path < 0)
		return;
	const UINT32 key = (UINT32)path << 8 | (Wreg(BP) & 1) << 7 | (sp & 1) << 6 | (UINT32)((nec_state->prefetch_count + 8) & 15) << 1 |
			   (nec_state->prefetch_reset & 1);
	struct rei_pcheck_cost *e = &rei_pcheck_tab[(key * 0x9e3779b1u) >> 24];
	if (e->key != key || e->d == 0) {
		if (e->d != 0 && e->key != key)
			return; // slot taken by another path: interpreter
		// learn: run the call with the real handlers up to the retf
		const UINT32 ret_ip = rei_rdw(ss + sp), ret_cs = rei_rdw(ss + ((sp + 2) & 0xffff));
		const int ic0 = nec_state->icount;
		for (int n = 0; n < 40; n++) {
			if (nec_state->icount <= 0)
				return; // slice ended inside: not measured (the interpreter carries on from here)
			const int prev = nec_state->icount;
			nec_instruction[fetchop(nec_state)](nec_state);
			do_prefetch(nec_state, prev, pf_sh);
			if (nec_state->ip == ret_ip && Sreg(PS) == ret_cs && Wreg(SP) == ((sp + 4) & 0xffff)) {
				if (CF != (carry != 0) || nec_state->OverVal != f.O || nec_state->AuxVal != f.A || nec_state->SignVal != f.S ||
				    nec_state->ZeroVal != f.Z || nec_state->ParityVal != f.P) { // not what the native code computes
					rei_pcheck_stats[2]++;
					rei_pcheck_state = -1;
					return;
				}
				e->key = key;
				e->d = (INT16)(ic0 - nec_state->icount);
				e->pf = nec_state->prefetch_count;
				e->rst = nec_state->prefetch_reset;
				rei_pcheck_stats[1]++;
				return;
			}
		}
		rei_pcheck_stats[2]++; // did not return as expected
		rei_pcheck_state = -1;
		return;
	}
	if (nec_state->icount - e->d < 1)
		return;
	nec_state->CarryVal = carry ? 1 : 0;
	nec_state->OverVal = f.O;
	nec_state->AuxVal = f.A;
	nec_state->SignVal = f.S;
	nec_state->ZeroVal = f.Z;
	nec_state->ParityVal = f.P;
	nec_state->ip = rei_rdw(ss + sp);                                // retf
	Sreg(PS) = rei_rdw(ss + ((sp + 2) & 0xffff));
	Wreg(SP) = (WORD)(sp + 4);
	nec_state->icount -= e->d;
	nec_state->prefetch_count = e->pf;
	nec_state->prefetch_reset = e->rst;
	rei_pcheck_stats[0]++;
}

// ReiREM PSP port: fast path for the game's task loop at V33 0x2974..0x29e2 (26 tasks a frame in play).
// Each task (a segment, ds) is run as
//  PRO (0x2974 -> the task code): xor ax,ax / xchg ax,[4a] / mov [4c],ax / test [c],1 / je / (inc ss:[7b6]) /
//   [6b] and [6a]: mov al,[x] / sub al,1 / adc al,0 / mov [x],al (decrement, not below 0) / mov byte [8f],0 /
//   cmp sp,4000 / je (else int3) / mov ss:[1afa],sp / push [0] / push [2] / call far [0]
//  EPI (0x29bb, where the task returns -> 0x2974): pop dx / pop ax / sub ax,[0] / sub dx,[2] / or ax,dx / je /
//   (or [c],8 | and [c],fff7) / mov word [56],0 / mov ds,[6] / jmp 2974
// Same scheme as the player check: cost and prefetch state per (piece, branch, prefetch state) learned by running
// the real handlers in the hook, where the native result (registers, flags) is checked against them.
#define REI_TASK_PC   0x2974
#define REI_TASKE_PC  0x29bb
#define REI_TASK_HASH 0x07202a63u // 0x2974..0x29e3
// the native piece on st (memory writes only if wr). Branch bit, or -1 for the interpreter
static REI_FAST_FN int rei_task_apply(nec_state_t *nec_state, int epi, int wr)
{
	const UINT32 ds = Sreg(DS0) << 4, ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (!epi) {
		if (sp != 0x4000)
			return -1;                                 // int3 path
		// the stores (ss:[7b6], ss:[1afa], 4 stack words, ds:[4a..8f]) must not touch ds:[0..7] read after them
		const UINT32 lo = ss + 0x7b6, hi = ss + sp;
		if (ds + 8 > lo && ds < hi + 2)
			return -1;
		const UINT32 t = rei_rdw(ds + 0x4a);
		const int br = (rei_rdw(ds + 0xc) & 1) ? 1 : 0;
		UINT32 b6b = cpu_readmem20(ds + 0x6b), b6a = cpu_readmem20(ds + 0x6a);
		b6b = b6b ? b6b - 1 : 0;
		b6a = b6a ? b6a - 1 : 0;
		const UINT32 tip = rei_rdw(ds), tcs = rei_rdw(ds + 2);
		if (wr) {
			write_mem_word(ds + 0x4a, 0);
			write_mem_word(ds + 0x4c, (UINT16)t);
			if (br)
				write_mem_word(ss + 0x7b6, (UINT16)(rei_rdw(ss + 0x7b6) + 1));
			cpu_writemem20(ds + 0x6b, b6b);
			cpu_writemem20(ds + 0x6a, b6a);
			cpu_writemem20(ds + 0x8f, 0);
			write_mem_word(ss + 0x1afa, (UINT16)sp);
			write_mem_word(ss + sp - 2, (UINT16)tip);     // push [0]
			write_mem_word(ss + sp - 4, (UINT16)tcs);     // push [2]
			write_mem_word(ss + sp - 6, Sreg(PS));        // call far [0]
			write_mem_word(ss + sp - 8, (UINT16)(nec_state->ip + (0x29bb - REI_TASK_PC)));
		}
		Wreg(AW) = (WORD)((t & 0xff00) | b6a);
		{ UINT32 dst = sp, src = 0x4000, res = dst - src;   // cmp sp,4000
		  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
		Wreg(SP) = (WORD)(sp - 8);
		Sreg(PS) = (WORD)tcs;
		nec_state->ip = (WORD)tip;
		return br;
	}
	if (sp > 0xfffb)
		return -1;
	const UINT32 dx0 = rei_rdw(ss + sp), ax0 = rei_rdw(ss + sp + 2);
	const UINT32 ax = ((ax0 - rei_rdw(ds)) | (dx0 - rei_rdw(ds + 2))) & 0xffff, dx = (dx0 - rei_rdw(ds + 2)) & 0xffff;
	const int br = ax == 0;                              // je
	const UINT32 c = rei_rdw(ds + 0xc), res = br ? (c & 0xfff7) : (c | 8);
	const UINT32 nds = rei_rdw(ds + 6);
	if (wr) {
		write_mem_word(ds + 0xc, (UINT16)res);
		write_mem_word(ds + 0x56, 0);
	}
	Wreg(AW) = (WORD)ax;
	Wreg(DW) = (WORD)dx;
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0; // or / and [c]
	SetSZPF_Word(res);
	Wreg(SP) = (WORD)(sp + 4);
	Sreg(DS0) = (WORD)nds;
	nec_state->ip = (WORD)(nec_state->ip + (REI_TASK_PC - REI_TASKE_PC));
	return br;
}

// Generic learned piece of straight code (used by the task loop and small functions below): apply(st, sub, wr)
// does the piece natively on st (memory writes only if wr) and returns a branch/variant id 0..3, or -1 for
// the interpreter. The cost and prefetch state after it per (sub, id, prefetch state) are learned by running the
// real handlers here, where registers, segments, flags and the interrupt shadow must match the native result.
// Returns 1 if the piece was done natively.
typedef int (*rei_fp_apply_fn)(nec_state_t *nec_state, int sub, int wr);
struct rei_fp {
	int state;              // 0 unchecked, 1 on, -1 off
	unsigned stats[3];      // native, learned, disagreements
	struct rei_step_cost tab[128][8][2]; // [(sub << 6) + id][prefetch count 0..7][reset]
};
// (the tables are zero-initialized (.bss): initialized ones went to .data and moved the interpreter's hot data,
// which cost ~1ms a frame on the PSP)
// Optional mark: with rei_fp_mark_tab set (same shape as tab) the cycles from the start of the piece to the start of
// the instruction at ip rei_fp_mark_ip are learned too and handed to apply in rei_fp_mark_cur (for a handler that
// needs that instruction's cycle position, nec_ins_start_cycles).
static INT16 *rei_fp_mark_tab;
static UINT32 rei_fp_mark_ip;
static int rei_fp_mark_cur;
// PC check (REI_FP_VERIFY=1): every learned piece always runs in the interpreter and is compared with the native result
// and the stored cost, not only the first time (a value that happened to match then would hide a wrong native result).
#if !defined(__psp__) && !defined(__vita__)
#include <stdlib.h>
static int rei_fp_verify_on() { static int v = -1; if (v < 0) v = getenv("REI_FP_VERIFY") != NULL; return v; }
#define REI_FP_VERIFY rei_fp_verify_on()
#define REI_FP_REPORT(what, k) fprintf(stderr, "nbb fp verify: %s differs (code %05x, key %x)\n", what, code, (unsigned)(k))
#else
#define REI_FP_VERIFY 0
#define REI_FP_REPORT(what, k) ((void)0)
#endif
// (the learning part of rei_fp_piece, out of its hot path: after learning a piece only the lookup runs)
static REI_FAST_FN __attribute__((noinline)) int rei_fp_learn(struct rei_fp *fp, nec_state_t *nec_state, const nec_state_t &copy,
							       struct rei_step_cost *e, int pf_sh, int sub, int br, UINT32 code)
{
	(void)code;
	// learn with the real handlers up to the end of the piece; the native result must match
	const int ic0 = nec_state->icount;
	int mark = -1;
	for (int n = 0; n < 48; n++) {
		if (nec_state->icount <= 0)
			return 0;
		// as the loop in nec_execute: an interrupt to take ends the measurement, the shadow counts down
		if (nec_state->pending_irq && !nec_state->no_interrupt && ((nec_state->pending_irq & NMI_IRQ) || nec_state->IF))
			return 0;
		if (nec_state->no_interrupt)
			nec_state->no_interrupt--;
		const int prev = nec_state->icount;
		if (rei_fp_mark_tab && nec_state->ip == rei_fp_mark_ip && mark < 0)
			mark = ic0 - prev;
		rei_ins_ic = prev; // (as nec_execute: handlers may ask for the instruction's cycle)
		nec_instruction[fetchop(nec_state)](nec_state);
		do_prefetch(nec_state, prev, pf_sh);
		if (nec_state->ip == copy.ip && Sreg(PS) == copy.sregs[PS] && Wreg(SP) == copy.regs.w[SP])
			break;
	}
	if (nec_state->ip != copy.ip || memcmp(nec_state->regs.w, copy.regs.w, sizeof(copy.regs)) ||
	    memcmp(nec_state->sregs, copy.sregs, sizeof(copy.sregs)) || nec_state->CarryVal != copy.CarryVal ||
	    nec_state->OverVal != copy.OverVal || nec_state->AuxVal != copy.AuxVal || nec_state->SignVal != copy.SignVal ||
	    nec_state->ZeroVal != copy.ZeroVal || nec_state->ParityVal != copy.ParityVal || nec_state->no_interrupt) {
		REI_FP_REPORT("state", (sub << 6) + br);
#if !defined(__psp__) && !defined(__vita__)
		fprintf(stderr, " ip %x/%x ax %x/%x bx %x/%x cx %x/%x dx %x/%x sp %x/%x ps %x/%x C %x/%x O %x/%x A %x/%x S %x/%x Z %x/%x P %x/%x ni %d\n",
			nec_state->ip, copy.ip, Wreg(AW), copy.regs.w[AW], Wreg(BW), copy.regs.w[BW], Wreg(CW), copy.regs.w[CW], Wreg(DW), copy.regs.w[DW],
			Wreg(SP), copy.regs.w[SP], Sreg(PS), copy.sregs[PS], nec_state->CarryVal, copy.CarryVal, nec_state->OverVal, copy.OverVal,
			nec_state->AuxVal, copy.AuxVal, nec_state->SignVal, copy.SignVal, nec_state->ZeroVal, copy.ZeroVal, nec_state->ParityVal, copy.ParityVal, nec_state->no_interrupt);
#endif
		fp->stats[2]++;
		fp->state = -1;
		return 0;
	}
	if (e->d != 0) { // (verify: already learned)
		if (e->d != ic0 - nec_state->icount || e->pf != nec_state->prefetch_count || e->rst != nec_state->prefetch_reset ||
		    (rei_fp_mark_tab && rei_fp_mark_tab[e - &fp->tab[0][0][0]] != mark)) {
			REI_FP_REPORT("cost", (sub << 6) + br);
			fp->stats[2]++;
			fp->state = -1;
		}
		return 0;
	}
	if (rei_fp_mark_tab) {
		if (mark < 0) {
			fp->stats[2]++;
			fp->state = -1;
			return 0;
		}
		rei_fp_mark_tab[e - &fp->tab[0][0][0]] = (INT16)mark;
	}
	e->d = (INT16)(ic0 - nec_state->icount);
	e->pf = nec_state->prefetch_count;
	e->rst = nec_state->prefetch_reset;
	fp->stats[1]++;
	return 0;
}
static REI_LINE_FAST_FN(11) int rei_fp_piece(struct rei_fp *fp, nec_state_t *nec_state, int pf_sh, rei_fp_apply_fn apply, int sub,
				    UINT32 code, UINT32 len, UINT32 hash)
{
	if (fp->state == 0)
		fp->state = rei_code_hash(code, len) == hash ? 1 : -1;
	if (fp->state < 0 || nec_state->no_interrupt || nec_state->seg_prefix || nec_state->prefetch_count < 0 ||
	    nec_state->prefetch_count > 7)
		return 0;
	nec_state_t copy = *nec_state;
	const int br = apply(&copy, sub, 0);
	if (br < 0)
		return 0;
	struct rei_step_cost *e = &fp->tab[((sub << 6) + br) & 127][nec_state->prefetch_count & 7][nec_state->prefetch_reset & 1];
	if (e->d == 0 || REI_FP_VERIFY)
		return rei_fp_learn(fp, nec_state, copy, e, pf_sh, sub, br, code);
	if (nec_state->icount - e->d < 1)
		return 0;
	if (rei_fp_mark_tab)
		rei_fp_mark_cur = rei_fp_mark_tab[e - &fp->tab[0][0][0]];
	apply(nec_state, sub, 1);
	nec_state->icount -= e->d;
	nec_state->prefetch_count = e->pf;
	nec_state->prefetch_reset = e->rst;
	fp->stats[0]++;
	return 1;
}

static struct rei_fp rei_task_fp REI_FAST_DATA;
static REI_FAST_FN void rei_task_run(nec_state_t *nec_state, int pf_sh)
{
	for (int guard = 0; guard < 4; guard++) {
		const UINT32 pc = PC(nec_state);
		const int epi = pc == REI_TASKE_PC;
		if (!epi && pc != REI_TASK_PC)
			return;
		if (!rei_fp_piece(&rei_task_fp, nec_state, pf_sh, rei_task_apply, epi, REI_TASK_PC, 112, REI_TASK_HASH) || !epi)
			return; // not done natively, or now in the task code
	}
}

// 0x151a (far called, 14 a frame in play): object position -> sprite cell. ss:bp = object, ax, cx in
//   push cx / mov bx,[bp+4] / add bx,ax / sar bx,1 / and bx,fc / not cx / add cx,[bp+6] / sar cx,3 / and cl,3f /
//   add bh,cl / mov es,[bp+0] / pop cx / retf
#define REI_CELL_PC 0x151a
static REI_FAST_FN int rei_cell_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, bp = Wreg(BP), sp = Wreg(SP), cx = Wreg(CW);
	if (bp > 0xfff8 || sp < 2 || sp > 0xfffb)
		return -1;
	UINT32 bx = (rei_rdw(ss + bp + 4) + Wreg(AW)) & 0xffff;
	bx = ((UINT32)((INT16)bx >> 1)) & 0xfc;
	const UINT32 c = (UINT32)((INT16)((~cx + rei_rdw(ss + bp + 6)) & 0xffff) >> 3) & 0x3f;
	{ UINT32 dst = 0, src = c, res = dst + src;       // add bh,cl (bh is 0 after and bx,fc)
	  SetCFB(res); SetOFB_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Byte(res); }
	const UINT32 es = rei_rdw(ss + bp);
	if (wr)
		write_mem_word(ss + sp - 2, (UINT16)cx);      // push cx / pop cx
	Wreg(BW) = (WORD)(c << 8 | bx);
	Sreg(DS1) = (WORD)es;
	nec_state->ip = rei_rdw(ss + sp);                  // retf
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 4);
	return (int)((bp & 1) | (sp & 1) << 1);
}
static struct rei_fp rei_cell_fp REI_FAST_DATA;

// 0x23a3 (far called, 14 a frame in play): put an object into the sprite slot lists. si = object (ffff: none),
// bx = slot, ax cx dx = data. Takes a 10-byte node from ss:[1af2] (pool end 1af2), fills it, links it in front
// of slot bx, or, for the same slot as last time with dx bit 10, after the last node (ss:[1af6], ss:[1af8]).
//   cmp si,ffff / jne / retf | and bx,1ff / add bx,bx / add bx,ebe / mov bp,ss:[1af2] / cmp bp,1af2 / jae full /
//   add word ss:[1af2],a / mov [bp+2..8],ax cx dx si / cmp bx,ss:[1af8] / jne L / test dx,400 / je L /
//   mov si,ss:[1af6] / cmp si,ffff / je L / mov ax,ss:[si] / mov [bp],ax / mov ss:[si],bp / retf |
//   L: mov si,ss:[bx] / mov [bp],si / mov ss:[bx],bp / cmp word ss:[1af6],ffff / jne S / mov ss:[1af6],bp /
//   mov ss:[1af8],bx / S: stc / retf | full: clc / retf
#define REI_SLOT_PC 0x23a3
static REI_FAST_FN int rei_slot_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), si = Wreg(IX), ax = Wreg(AW), cx = Wreg(CW), dx = Wreg(DW);
	if (sp > 0xfffb)
		return -1;
	const UINT32 ret_ip = rei_rdw(ss + sp), ret_cs = rei_rdw(ss + sp + 2);
	int id;
	UINT32 acc = 0; // parity of the word accesses that vary: ss:[si] (id 2)
	if (si == 0xffff) {
		id = 0;
		{ UINT32 dst = si, src = 0xffff, res = dst - src;
		  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
	} else {
		const UINT32 bx = 0xebe + 2 * (Wreg(BW) & 0x1ff);
		const UINT32 bp = rei_rdw(ss + 0x1af2);
		Wreg(BW) = (WORD)bx;
		Wreg(BP) = (WORD)bp;
		{ UINT32 dst = bp, src = 0x1af2, res = dst - src;
		  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
		if (bp >= 0x1af2) {
			id = 1;
			nec_state->CarryVal = 0; // clc
		} else {
			if (bp > 0xfff5)
				return -1;
			if (wr) {
				write_mem_word(ss + 0x1af2, (UINT16)(bp + 0xa));
				write_mem_word(ss + bp + 2, (UINT16)ax);
				write_mem_word(ss + bp + 4, (UINT16)cx);
				write_mem_word(ss + bp + 6, (UINT16)dx);
				write_mem_word(ss + bp + 8, (UINT16)si);
			}
			int via = 0; // 0: bx != last slot, 1: dx bit 10 clear, 2: no last node, 3: after the last node
			if (bx != rei_rdw(ss + 0x1af8))
				via = 0;
			else if (!(dx & 0x400))
				via = 1;
			else {
				const UINT32 last = rei_rdw(ss + 0x1af6);
				Wreg(IX) = (WORD)last;
				via = last == 0xffff ? 2 : 3;
			}
			if (via == 3) {
				const UINT32 last = Wreg(IX);
				id = 2;
				acc = last & 1;
				const UINT32 nx = rei_rdw(ss + last);
				Wreg(AW) = (WORD)nx;
				if (wr) {
					write_mem_word(ss + bp, (UINT16)nx);
					write_mem_word(ss + last, (UINT16)bp);
				}
				{ UINT32 dst = last, src = 0xffff, res = dst - src; // cmp si,ffff
				  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
			} else {
				const UINT32 head = rei_rdw(ss + bx);
				Wreg(IX) = (WORD)head;
				if (wr) {
					write_mem_word(ss + bp, (UINT16)head);
					write_mem_word(ss + bx, (UINT16)bp);
				}
				const UINT32 l6 = rei_rdw(ss + 0x1af6);
				{ UINT32 dst = l6, src = 0xffff, res = dst - src; // cmp word ss:[1af6],ffff
				  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
				if (l6 == 0xffff && wr) {
					write_mem_word(ss + 0x1af6, (UINT16)bp);
					write_mem_word(ss + 0x1af8, (UINT16)bx);
				}
				id = 3 + via * 2 + (l6 == 0xffff ? 1 : 0); // 3..8
				nec_state->CarryVal = 1; // stc
			}
		}
		id = id << 2 | (int)(bp & 1) << 1 | (int)acc;
	}
	nec_state->ip = (WORD)ret_ip; // retf
	Sreg(PS) = (WORD)ret_cs;
	Wreg(SP) = (WORD)(sp + 4);
	return id << 1 | (int)(sp & 1);
}
static struct rei_fp rei_slot_fp REI_FAST_DATA;

// 0x3eb5d..0x3eba7: for each of the 4 players (bp, dh) and 4 entries (dl, ss:[si]) call the player check.
//  CALL (0x3eb5f -> 0x3ef04): mov ss:[si+7],dl / mov ss:[si+6],dh / mov ss:[si+4],bp / mov word ss:[si+2],0 /
//   call 3000:ef04
//  NEXT (0x3eb76 -> 0x3eb5f or 0x3eba8): jae Z / mov word ss:[si+2],ffff / mov cx,ss:[si] / jcxz N /
//   mov es,cx / test word es:[4c],80 / jne N / Z: mov word ss:[si],0 / N: add si,8 / inc dl / cmp dl,4 / jb 3eb5f /
//   add bp,c0 / inc dh / cmp dh,4 / jb 3eb5d (mov dl,0 -> 3eb5f)
#define REI_PLC_PC 0x3eb5f
#define REI_PLN_PC 0x3eb76
static REI_FAST_FN int rei_pl_apply(nec_state_t *nec_state, int sub, int wr)
{
	const UINT32 ss = Sreg(SS_) << 4, si = Wreg(IX), sp = Wreg(SP);
	if (si > 0xfff6)
		return -1;
	if (sub == 0) {
		if (sp < 4)
			return -1;
		if (wr) {
			cpu_writemem20(ss + si + 7, Wreg(DW) & 0xff);
			cpu_writemem20(ss + si + 6, Wreg(DW) >> 8);
			write_mem_word(ss + si + 4, Wreg(BP));
			write_mem_word(ss + si + 2, 0);
			write_mem_word(ss + sp - 2, Sreg(PS));                                  // call far
			write_mem_word(ss + sp - 4, (UINT16)(nec_state->ip + (REI_PLN_PC - REI_PLC_PC)));
		}
		Wreg(SP) = (WORD)(sp - 4);
		Sreg(PS) = 0x3000;
		nec_state->ip = 0xef04;
		return (int)((si & 1) | (sp & 1) << 1);
	}
	int id = 0;
	if (!CF) {                                              // jae Z
		id = 1;
	} else {
		if (wr)
			write_mem_word(ss + si + 2, 0xffff);
		const UINT32 cx = rei_rdw(ss + si);
		Wreg(CW) = (WORD)cx;
		if (cx == 0) {
			id = 2;                                         // jcxz N
		} else {
			Sreg(DS1) = (WORD)cx;                           // mov es,cx
			const UINT32 t = rei_rdw((cx << 4) + 0x4c);
			nec_state->CarryVal = nec_state->OverVal = 0;   // test (keeps AF; flags are overwritten below)
			SetSZPF_Word(t & 0x80);
			id = (t & 0x80) ? 3 : 4;                        // jne N
		}
	}
	if ((id == 1 || id == 4) && wr)
		write_mem_word(ss + si, 0);                         // Z
	Wreg(IX) = (WORD)(si + 8);
	UINT32 dl = (Wreg(DW) & 0xff) + 1, dh = Wreg(DW) >> 8;
	UINT32 cmpv = dl, end = 0;
	if (dl >= 4) {
		Wreg(BP) = (WORD)(Wreg(BP) + 0xc0);
		dh = (dh + 1) & 0xff;
		cmpv = dh;
		if (dh < 4)
			dl = 0;                                         // mov dl,0 at 3eb5d
		else
			end = 1;
		id += 8 * (end ? 2 : 1);
	}
	Wreg(DW) = (WORD)(dh << 8 | (dl & 0xff));
	{ UINT32 dst = cmpv, src = 4, res = dst - src;          // cmp dl,4 / cmp dh,4
	  nec_state->CarryVal = res & 0x100; SetOFB_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Byte(res); }
	nec_state->ip = (WORD)(nec_state->ip + ((end ? 0x3eba8 : REI_PLC_PC) - REI_PLN_PC));
	return id << 1 | (int)(si & 1);
}
static struct rei_fp rei_pl_fp REI_FAST_DATA;

// Palette fade (continue, game over, ranking: ~1/3 of the V33 instructions there). The code is checked by hashes
// (0x1f9f..0x1fee, 0x202e..0x2110).
// 0x1f9f: one palette entry (es = entry, ss:bp = its 8-byte record), the iterations without a call:
//   push cx / test byte [bp+6],4 / je F / test byte [bp+6],1 / je / test byte [bp+6],2 / je / jmp E |
//   F: test byte ss:[dbd],ff / je (call) / test byte [bp+6],1 / je / test byte [bp+6],2 / je |
//   E: pop cx / add bp,8 / mov ax,es / add ax,2 / mov es,ax / loop 1f9f
#define REI_PFADE_PC 0x1f9f
static REI_FAST_FN int rei_pfade_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), bp = Wreg(BP), cx = Wreg(CW), es = Sreg(DS1);
	if (sp < 2 || bp > 0xfff9)
		return -1;
	const UINT32 st0 = ss + sp - 2; // the word push cx writes (read back by pop cx)
	const UINT32 fa = ss + bp + 6;
	if (fa == st0 || fa == st0 + 1 || ss + 0xdbd == st0 || ss + 0xdbd == st0 + 1)
		return -1;
	const UINT32 f = cpu_readmem20(fa);
	int id;
	if (f & 4) {
		if (f & 3)
			return -1;
		id = 0;
	} else {
		if ((f & 3) || cpu_readmem20(ss + 0xdbd) == 0)
			return -1;
		id = 1;
	}
	if (wr)
		write_mem_word(st0, (UINT16)cx);
	Wreg(BP) = (WORD)(bp + 8);
	{ UINT32 dst = es, src = 2, res = dst + src; // mov ax,es / add ax,2 / mov es,ax (flags of add bp,8 overwritten)
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(AW) = (WORD)res; Sreg(DS1) = (WORD)res; }
	Wreg(CW) = (WORD)(cx - 1);
	if (Wreg(CW) == 0)
		nec_state->ip = (WORD)(nec_state->ip + (0x1fee - REI_PFADE_PC));
	return id << 1 | (int)(sp & 1);
}
static struct rei_fp rei_pfade_fp REI_FAST_DATA;

// 0x2033: one colour of the brightness loop of 0x202e: ds:si colour - ss:[db8..dba] per component, clipped to 0..1f
//   push cx / lodsw / call 20cb / stosw / pop cx / loop 2033
//   20cb: mov dx,ax / and ax,7c1f / and dx,3e0 / shr ah,2 / shl dx,3 / mov bx,db8 / sub al,ss:[bx] / jg / xor al,al /
//   inc bx / (same for ah, dh) / cmp al,20 / jb / mov al,1f / (same for ah, dh) / shl ah,2 / shr dx,3 / or ax,dx / ret
#define REI_BRT_PC 0x2033
static inline UINT32 rei_brt_comp(UINT32 v, UINT32 m, int *id, int bit)
{
	const UINT32 r = (v - m) & 0xff;
	const int gt = r != 0 && !(r & 0x80) == !((v ^ m) & (v ^ r) & 0x80); // jg after sub
	if (gt)
		*id |= 1 << bit;
	const UINT32 c = gt ? r : 0;
	if (c < 0x20) { // jb
		*id |= 8 << bit;
		return c;
	}
	return 0x1f;
}
static REI_FAST_FN int rei_brt_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), si = Wreg(IX), di = Wreg(IY), cx = Wreg(CW);
	if (nec_state->DF || sp < 4 || ((sp | si | di) & 1))
		return -1; // (odd word addresses: other access timing)
	const UINT32 src = (Sreg(DS0) << 4) + si, dst = (Sreg(DS1) << 4) + di, st = ss + sp - 4; // st: return ip, cx
	// a read or the pop that would see one of the piece's own writes: interpreter
	if ((src + 2 > st && src < st + 4) || (ss + 0xdb8 + 3 > st && ss + 0xdb8 < st + 4) || (dst + 2 > st + 2 && dst < st + 4))
		return -1;
	const UINT32 c = rei_rdw(src);
	int id = 0;
	const UINT32 al = rei_brt_comp(c & 0x1f, cpu_readmem20(ss + 0xdb8), &id, 0);
	const UINT32 ah = rei_brt_comp((c >> 10) & 0x1f, cpu_readmem20(ss + 0xdb9), &id, 1);
	const UINT32 dh = rei_brt_comp((c >> 5) & 0x1f, cpu_readmem20(ss + 0xdba), &id, 2);
	const UINT32 res = al | ah << 10 | dh << 5;
	if (wr) {
		write_mem_word(st + 2, (UINT16)cx);                             // push cx
		write_mem_word(st, (UINT16)(nec_state->ip + 5));                // call (return to 0x2038)
		write_mem_word(dst, (UINT16)res);                               // stosw
	}
	Wreg(AW) = (WORD)res;
	Wreg(BW) = 0xdba;
	Wreg(DW) = (WORD)(dh << 5);
	Wreg(IX) = (WORD)(si + 2);
	Wreg(IY) = (WORD)(di + 2);
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0; // or ax,dx
	SetSZPF_Word(res);
	Wreg(CW) = (WORD)(cx - 1);
	if (Wreg(CW) == 0)
		nec_state->ip = (WORD)(nec_state->ip + (0x203c - REI_BRT_PC));
	return id;
}
static struct rei_fp rei_brt_fp REI_FAST_DATA;

// 0x5549 (game over balls, ~45% of the V33 there): move the object at ds toward ds:[a0],[a2] and put its sprite.
//   mov ax,[a0] / sub ax,[26] / shl ax,5 / mov cx,[a2] / sub cx,[28] / shl cx,5 / call 0:3758 / mov si,969d /
//   call 0:3919 (-> 0x23a3, which has its own fast path)
//   3758: cwd / mov dl,ah / add [2a],al / adc [26],dx / mov ax,cx / cwd / mov dl,ah / add [2b],al / adc [28],dx / retf
//   3919: mov bl,[21] / cmp bl,4 / jb / (int3) / xor bh,bh / shl bx,1 / mov dx,[bx+10] / and dx,7f / mov bx,[c] /
//   mov [e],bx / and bx,780 / or dx,bx / mov bx,[18] / mov ax,[26] / mov cx,[28] / add cx,[30] / test word [c],4000 /
//   je (jmp 0:23a3) / retf
// Checked by hashes of the three pieces of code.
#define REI_BALL_PC 0x5549
static int rei_ball_code; // 0 unchecked, 1 matches, -1 different
static REI_FAST_FN int rei_ball_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), ds = Sreg(DS0) << 4, ps = Sreg(PS), ip = nec_state->ip;
	if (sp < 4 || (ss + sp - 4 < ds + 0xa4 && ss + sp > ds))
		return -1; // (the stack inside the object: the reads would see the pushes)
	const UINT32 bl = cpu_readmem20(ds + 0x21);
	if (bl >= 4)
		return -1;
	const UINT32 ax = ((rei_rdw(ds + 0xa0) - rei_rdw(ds + 0x26)) << 5) & 0xffff;
	const UINT32 cx = ((rei_rdw(ds + 0xa2) - rei_rdw(ds + 0x28)) << 5) & 0xffff;
	// 3758: x += ax / 256 with the fraction in [2a], y likewise ([2b], [28])
	const UINT32 fx = cpu_readmem20(ds + 0x2a) + (ax & 0xff), fy = cpu_readmem20(ds + 0x2b) + (cx & 0xff);
	const UINT32 dxa = ((ax & 0x8000) ? 0xff00 : 0) | ax >> 8, dxc = ((cx & 0x8000) ? 0xff00 : 0) | cx >> 8;
	const UINT32 x = (rei_rdw(ds + 0x26) + dxa + (fx >> 8)) & 0xffff, y = (rei_rdw(ds + 0x28) + dxc + (fy >> 8)) & 0xffff;
	const UINT32 c = rei_rdw(ds + 0xc);
	const UINT32 dx = (rei_rdw(ds + 0x10 + 2 * bl) & 0x7f) | (c & 0x780);
	const UINT32 a30 = rei_rdw(ds + 0x30);
	const UINT32 bx = rei_rdw(ds + 0x18), ncx = (y + a30) & 0xffff;
	const int ret = (c & 0x4000) != 0;
	if (wr) {
		write_mem_word(ss + sp - 2, (UINT16)ps);                       // call 0:3758
		write_mem_word(ss + sp - 4, (UINT16)(ip + (0x5563 - REI_BALL_PC)));
		cpu_writemem20(ds + 0x2a, fx & 0xff);
		write_mem_word(ds + 0x26, (UINT16)x);
		cpu_writemem20(ds + 0x2b, fy & 0xff);
		write_mem_word(ds + 0x28, (UINT16)y);
		write_mem_word(ss + sp - 2, (UINT16)ps);                       // call 0:3919
		write_mem_word(ss + sp - 4, (UINT16)(ip + (0x556b - REI_BALL_PC)));
		write_mem_word(ds + 0xe, (UINT16)c);
	}
	Wreg(AW) = (WORD)x;
	Wreg(BW) = (WORD)bx;
	Wreg(CW) = (WORD)ncx;
	Wreg(DW) = (WORD)dx;
	Wreg(IX) = 0x969d;
	// test word [c],4000 (f7 /0) clears C, O but keeps A from add cx,[30]
	nec_state->CarryVal = nec_state->OverVal = 0;
	SetAF(y + a30, a30, y);
	SetSZPF_Word(c & 0x4000);
	if (ret) { // retf back to the caller of 0x5549's piece (0x556b)
		nec_state->ip = (WORD)(ip + (0x556b - REI_BALL_PC));
		Wreg(SP) = (WORD)sp;
	} else {   // jmp 0:23a3
		nec_state->ip = 0x23a3;
		Sreg(PS) = 0;
		Wreg(SP) = (WORD)(sp - 4);
	}
	return ret << 1 | (int)(sp & 1);
}
static struct rei_fp rei_ball_fp REI_FAST_DATA;

// 0x0dda (score ranking: raster interrupt every line, ~1/3 of the V33 instructions there): next line's scroll from
// a table and the next raster line.
//   push bx / push ax / mov bx,ss:[792] / mov ax,ss:[10] / shl ax,2 / add bx,ax / mov ss:[792],bx / mov ax,ss:[bx] /
//   add ax,ss:[796] / out 8c,ax / mov ax,ss:[bx+2] / out 88,ax / mov ax,ss:[e] / add ax,ss:[10] / mov ss:[e],ax /
//   out 9e,ax / pop ax / pop bx / iret
// The port writes go to the real handlers in the same order. The raster line write needs the cycle of its instruction
// inside a main line span (d_m92.cpp ReiSpanStop): learned as the piece's mark.
#define REI_RIRQ_PC 0x0dda
int rei_v33_span_active; // set by d_m92.cpp while a main line span runs
static REI_LINE_FAST_FN(12) int rei_rirq_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (sp < 4 || sp > 0xfff9)
		return -1;
	const UINT32 st = ss + sp - 4; // push ax (st), push bx (st + 2); the interrupt frame at ss + sp
	const UINT32 bx = (rei_rdw(ss + 0x792) + (rei_rdw(ss + 0x10) << 2)) & 0xffff;
	if (bx > 0xfffc)
		return -1;
	// a read that would see one of the piece's own writes (stack, [792], [e]): interpreter
	const UINT32 lo = ss + bx, hi = ss + bx + 4;
	if ((lo < st + 4 && hi > st) || (lo < ss + 0x794 && hi > ss + 0x792) || (lo < ss + 0x10 && hi > ss + 0xe) ||
	    (ss + 0x792 < st + 10 && ss + 0x798 > st) || (ss + 0xe < st + 10 && ss + 0x12 > st))
		return -1; // (st + 10: also the interrupt frame iret reads)
	const UINT32 f = rei_rdw(ss + sp + 4);
	if (f & 0x100)
		return -1; // (TF: popf traps)
	const UINT32 a1 = (rei_rdw(ss + bx) + rei_rdw(ss + 0x796)) & 0xffff, a2 = rei_rdw(ss + bx + 2);
	const UINT32 a3 = (rei_rdw(ss + 0xe) + rei_rdw(ss + 0x10)) & 0xffff;
	if (wr) {
		write_mem_word(st + 2, Wreg(BW));
		write_mem_word(st, Wreg(AW));
		write_mem_word(ss + 0x792, (UINT16)bx);
		write_port_word(0x8c, (UINT16)a1);
		write_port_word(0x88, (UINT16)a2);
		write_mem_word(ss + 0xe, (UINT16)a3);
		rei_ins_ic = nec_state->icount - rei_fp_mark_cur; // out 9e,ax
		write_port_word(0x9e, (UINT16)a3);
	}
	nec_state->ip = rei_rdw(ss + sp); // iret
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 6);
	ExpandFlags(f);
	return (int)((bx & 1) | (sp & 1) << 1);
}
static struct rei_fp rei_rirq_fp REI_FAST_DATA;
static INT16 rei_rirq_mark[128][8][2] REI_FAST_DATA; // cycles to out 9e (rei_fp_mark_tab)

// 0x0d0a (stage guide map: the same per-line raster interrupt for layer 0, ~10% of the V33 there):
//   push bx / push ax / mov bx,ss:[792] / mov ax,ss:[bx] / add ax,ss:[794] / out 84,ax / mov ax,ss:[bx+2] /
//   out 80,ax / mov ax,ss:[10] / shl ax,2 / add bx,ax / mov ss:[792],bx / mov ax,ss:[e] / add ax,ss:[10] /
//   mov ss:[e],ax / out 9e,ax / pop ax / pop bx / iret
#define REI_RIRQ0_PC 0x0d0a
static REI_LINE_FAST_FN(13) int rei_rirq0_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (sp < 4 || sp > 0xfff9)
		return -1;
	const UINT32 st = ss + sp - 4;
	const UINT32 bx0 = rei_rdw(ss + 0x792);
	if (bx0 > 0xfffc)
		return -1;
	const UINT32 lo = ss + bx0, hi = ss + bx0 + 4;
	if ((lo < st + 4 && hi > st) || (ss + 0x792 < st + 10 && ss + 0x798 > st) || (ss + 0xe < st + 10 && ss + 0x12 > st))
		return -1;
	const UINT32 f = rei_rdw(ss + sp + 4);
	if (f & 0x100)
		return -1;
	const UINT32 a1 = (rei_rdw(ss + bx0) + rei_rdw(ss + 0x794)) & 0xffff, a2 = rei_rdw(ss + bx0 + 2);
	const UINT32 bx = (bx0 + (rei_rdw(ss + 0x10) << 2)) & 0xffff;
	const UINT32 a3 = (rei_rdw(ss + 0xe) + rei_rdw(ss + 0x10)) & 0xffff;
	if (wr) {
		write_mem_word(st + 2, Wreg(BW));
		write_mem_word(st, Wreg(AW));
		write_port_word(0x84, (UINT16)a1);
		write_port_word(0x80, (UINT16)a2);
		write_mem_word(ss + 0x792, (UINT16)bx);
		write_mem_word(ss + 0xe, (UINT16)a3);
		rei_ins_ic = nec_state->icount - rei_fp_mark_cur; // out 9e,ax
		write_port_word(0x9e, (UINT16)a3);
	}
	nec_state->ip = rei_rdw(ss + sp); // iret
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 6);
	ExpandFlags(f);
	return (int)((bx0 & 1) | (sp & 1) << 1);
}
static struct rei_fp rei_rirq0_fp REI_FAST_DATA;
static INT16 rei_rirq0_mark[128][8][2] REI_FAST_DATA;

// 0x925d (stage guide map: per-line scroll table into VRAM, 240 iterations a frame): one iteration
//   mov ax,cx / sub ax,si / sar ax,1 / imul bx / mov al,ah / mov ah,dl / add ax,bp / add ax,ss:[794] / stosw /
//   and di,3fe / inc cx / cmp cx,f0 / jb 925d
#define REI_GSCR_PC 0x925d
static REI_FAST_FN int rei_gscr_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, cx = Wreg(CW), di = Wreg(IY);
	if (nec_state->DF)
		return -1;
	const INT32 p = (INT32)(INT16)(((INT16)(UINT16)(cx - Wreg(IX))) >> 1) * (INT32)(INT16)Wreg(BW);
	const UINT32 dxw = ((UINT32)p >> 16) & 0xffff;
	const UINT32 ax = (((((UINT32)p >> 8) & 0xff) | ((dxw & 0xff) << 8)) + Wreg(BP) + rei_rdw(ss + 0x794)) & 0xffff;
	if (wr)
		write_mem_word((Sreg(DS1) << 4) + di, (UINT16)ax);
	Wreg(AW) = (WORD)ax;
	Wreg(DW) = (WORD)dxw;
	Wreg(IY) = (WORD)((di + 2) & 0x3fe);
	const UINT32 ncx = (cx + 1) & 0xffff;
	Wreg(CW) = (WORD)ncx;
	{ UINT32 dst = ncx, src = 0xf0, res = dst - src; // cmp cx,f0
	  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
	if (ncx >= 0xf0)
		nec_state->ip = (WORD)(nec_state->ip + (0x927c - REI_GSCR_PC));
	return (int)(di & 1);
}
static struct rei_fp rei_gscr_fp REI_FAST_DATA;

// 0x0a8e (stage intro: per-line raster interrupt for layer 1 through a pointer table, ~1/4 of the V33 there):
//   push bx / push ax / mov bx,ss:[792] / mov ax,ss:[10] / shl ax,2 / add bx,ax / mov ss:[792],bx / mov bx,ss:[bx] /
//   mov al,ss:[bx+2] / out 9a,al / mov ax,ss:[bx+36] / add ax,ss:[796] / out 8c,ax / mov ax,ss:[bx+38] / out 88,ax /
//   mov ax,ss:[e] / add ax,ss:[10] / mov ss:[e],ax / out 9e,ax / pop ax / pop bx / iret
// 0x0b6d (stage 6, ~8% there): the same for layer 0 (ports 98, 84, 80 and ss:[794])
#define REI_RIRQ1_PC 0x0a8e
#define REI_RIRQ2_PC 0x0b6d
static const struct rei_rirqp { UINT8 info, x, y; UINT16 add; } rei_rirqp[2] = { { 0x9a, 0x8c, 0x88, 0x796 },
										     { 0x98, 0x84, 0x80, 0x794 } };
static REI_LINE_FAST_FN(14) int rei_rirq1_apply(nec_state_t *nec_state, int sub, int wr)
{
	const struct rei_rirqp *c = &rei_rirqp[sub & 1];
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (sp < 4 || sp > 0xfff9)
		return -1;
	const UINT32 st = ss + sp - 4;
	const UINT32 p = (rei_rdw(ss + 0x792) + (rei_rdw(ss + 0x10) << 2)) & 0xffff;
	if (p > 0xfffd)
		return -1;
	if ((ss + p < st + 4 && ss + p + 2 > st) || (ss + p < ss + 0x794 && ss + p + 2 > ss + 0x792) ||
	    (ss + 0x792 < st + 10 && ss + 0x798 > st) || (ss + 0xe < st + 10 && ss + 0x12 > st))
		return -1;
	const UINT32 bx = rei_rdw(ss + p);
	if (bx > 0xffc5)
		return -1;
	const UINT32 lo = ss + bx + 2, hi = ss + bx + 0x3a; // [bx+2], [bx+36], [bx+38]
	if ((lo < st + 4 && hi > st) || (lo < ss + 0x794 && hi > ss + 0x792) || (lo < ss + 0x10 && hi > ss + 0xe))
		return -1;
	const UINT32 f = rei_rdw(ss + sp + 4);
	if (f & 0x100)
		return -1;
	const UINT32 b9a = cpu_readmem20(ss + bx + 2);
	const UINT32 a1 = (rei_rdw(ss + bx + 0x36) + rei_rdw(ss + c->add)) & 0xffff, a2 = rei_rdw(ss + bx + 0x38);
	const UINT32 a3 = (rei_rdw(ss + 0xe) + rei_rdw(ss + 0x10)) & 0xffff;
	if (wr) {
		write_mem_word(st + 2, Wreg(BW));
		write_mem_word(st, Wreg(AW));
		write_mem_word(ss + 0x792, (UINT16)p);
		write_port_byte(c->info, b9a);
		write_port_word(c->x, (UINT16)a1);
		write_port_word(c->y, (UINT16)a2);
		write_mem_word(ss + 0xe, (UINT16)a3);
		rei_ins_ic = nec_state->icount - rei_fp_mark_cur; // out 9e,ax
		write_port_word(0x9e, (UINT16)a3);
	}
	nec_state->ip = rei_rdw(ss + sp); // iret
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 6);
	ExpandFlags(f);
	return (int)((p & 1) | (sp & 1) << 1 | (bx & 1) << 2);
}
static struct rei_fp rei_rirq1_fp REI_FAST_DATA, rei_rirq2_fp REI_FAST_DATA;
static INT16 rei_rirq1_mark[128][8][2] REI_FAST_DATA, rei_rirq2_mark[128][8][2] REI_FAST_DATA;

// 0x0e51 (far called, 240 a frame from the per-line table loops of the guide map and stage intro): store ax, cx for
// line bx into one of two tables.
//   cmp bx,f0 / jb / retf | push bx / shl bx,2 / test word ss:[7aa],1 / jne B / add bx,12 / jmp S | B: add bx,3d2 |
//   S: mov ss:[bx],ax / mov ss:[bx+2],cx / pop bx / retf
#define REI_LTAB_PC 0x0e51
static REI_FAST_FN int rei_ltab_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), bx = Wreg(BW);
	if (sp < 2 || sp > 0xfffb)
		return -1;
	int id;
	if (bx >= 0xf0) {
		const UINT32 src = 0xf0, res = bx - src;
		SetCFW(res); SetOFW_Sub(res, src, bx); SetAF(res, src, bx); SetSZPF_Word(res);
		id = 0;
	} else {
		const UINT32 alt = rei_rdw(ss + 0x7aa) & 1, off = alt ? 0x3d2 : 0x12;
		const UINT32 dst = (bx << 2) & 0xffff, res = dst + off, a = ss + (res & 0xffff);
		if ((res & 0xffff) > 0xfffc || (a < ss + sp + 4 && a + 4 > ss + sp - 2) || (ss + 0x7aa < ss + sp && ss + 0x7ac > ss + sp - 2))
			return -1;
		if (wr) {
			write_mem_word(ss + sp - 2, (UINT16)bx);
			write_mem_word(a, Wreg(AW));
			write_mem_word(a + 2, Wreg(CW));
		}
		SetCFW(res); SetOFW_Add(res, off, dst); SetAF(res, off, dst); SetSZPF_Word(res);
		id = 1 + (int)alt;
	}
	nec_state->ip = rei_rdw(ss + sp); // retf
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 4);
	return id << 1 | (int)(sp & 1);
}
static struct rei_fp rei_ltab_fp REI_FAST_DATA;

// Stage intro / guide map screen wipes (~2/3 of the V33 instructions there): loops over the 240 lines that pick a
// value per line and call 0x0e51 (the line table store above) - one iteration per piece, the call done by
// rei_ltab_apply (lcall pushes PS and the return ip first; with wr 0 they are not written, so ip/PS are set here).
//   0x9eb7: mov ax,7ba / mov cx,bx / and cx,7 / cmp cl,[39] / jl + / mov ax,86e / + lcall 0:e51 / inc bx / cmp bx,f0 / jb
//   0x9f2b: mov ax,7f6 / cmp bx,[26] / jle + / cmp bx,[28] / jge + / mov ax,86e / + lcall 0:e51 / inc bx / cmp bx,f0 / jb
//   0x9f9d: mov ax,86e / mov cx,6f / sub cx,[38] / cmp bx,cx / jl + / mov cx,6f / add cx,[38] / cmp bx,cx / jge + /
//           mov ax,7f6 / + lcall 0:e51 / inc bx / cmp bx,f0 / jb
#define REI_WIPEA_PC 0x9eb7
#define REI_WIPEB_PC 0x9f2b
#define REI_WIPEC_PC 0x9f9d
static REI_FAST_FN int rei_wipe_tail(nec_state_t *nec_state, UINT32 ip0, UINT32 ret, UINT32 end, int wr)
{
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), ps = Sreg(PS);
	if (sp < 6 || sp > 0xfffb)
		return -1;
	if (0x7aa < sp && 0x7ac > sp - 4)
		return -1; // 0x0e51 would read [7aa] from the return address just pushed (not written with wr 0)
	if (wr) { // lcall 0:e51
		write_mem_word(ss + sp - 2, (UINT16)ps);
		write_mem_word(ss + sp - 4, (UINT16)(ip0 + ret));
	}
	Wreg(SP) = (WORD)(sp - 4);
	const int id = rei_ltab_apply(nec_state, 0, wr);
	if (id < 0)
		return -1;
	nec_state->ip = (WORD)(ip0 + ret); // (retf: with wr 0 the return address is not in memory)
	Sreg(PS) = (WORD)ps;
	Wreg(SP) = (WORD)sp;
	const UINT32 bx = (Wreg(BW) + 1) & 0xffff; // inc bx / cmp bx,f0 / jb
	Wreg(BW) = (WORD)bx;
	{
		UINT32 dst = bx, src = 0xf0;
		SUBW;
		(void)dst;
	}
	const int exit = bx >= 0xf0;
	nec_state->ip = (WORD)(exit ? ip0 + end : ip0);
	return id << 1 | exit; // (id: 0x0e51 path and sp & 1)
}
static REI_FAST_FN int rei_wipe_apply(nec_state_t *nec_state, int sub, int wr)
{
	const UINT32 ip0 = nec_state->ip, ds = Sreg(DS0) << 4, bx = Wreg(BW);
	if (bx >= 0xf0)
		return -1;
	int p;
	UINT32 ret, end;
	if (sub == 0) { // 0x9eb7
		const UINT32 cl = bx & 7;
		Wreg(CW) = (WORD)cl;
		p = (INT8)cl < (INT8)cpu_readmem20(ds + 0x39);
		Wreg(AW) = p ? 0x7ba : 0x86e;
		ret = 0x9ece - REI_WIPEA_PC;
		end = 0x9ed5 - REI_WIPEA_PC;
	} else if (sub == 1) { // 0x9f2b
		Wreg(AW) = 0x7f6;
		if ((INT16)bx <= (INT16)rei_rdw(ds + 0x26))
			p = 0;
		else if ((INT16)bx >= (INT16)rei_rdw(ds + 0x28))
			p = 1;
		else {
			p = 2;
			Wreg(AW) = 0x86e;
		}
		ret = 0x9f42 - REI_WIPEB_PC;
		end = 0x9f49 - REI_WIPEB_PC;
	} else { // 0x9f9d
		const UINT32 m = rei_rdw(ds + 0x38), c1 = (0x6f - m) & 0xffff;
		Wreg(AW) = 0x86e;
		if ((INT16)bx < (INT16)c1) {
			p = 0;
			Wreg(CW) = (WORD)c1;
		} else {
			const UINT32 c2 = (0x6f + m) & 0xffff;
			Wreg(CW) = (WORD)c2;
			if ((INT16)bx >= (INT16)c2)
				p = 1;
			else {
				p = 2;
				Wreg(AW) = 0x7f6;
			}
		}
		ret = 0x9fbe - REI_WIPEC_PC;
		end = 0x9fc5 - REI_WIPEC_PC;
	}
	const int t = rei_wipe_tail(nec_state, ip0, ret, end, wr);
	return t < 0 ? -1 : t << 2 | p; // (t < 16: sub << 6 keeps the loops apart)
}
// (tables shared to stay in the fixed .reifast_data block: 0x9eb7/0x9f2b keys (sub 0/1) << 6 apart, 0x9f9d (sub 2 ->
// keys 0..47) with 0x108a below (sub 1 -> keys 64..79))
static struct rei_fp rei_wipe_fp REI_FAST_DATA, rei_wipec_fp REI_FAST_DATA;

// 0x905b (stage guide map, ~45% of the V33 instructions there): the map's per-line table over the 240 lines, one
// iteration per piece, the call of 0x0e51 by rei_ltab_apply as for the wipes above.
//   cmp bx,di / jb B / cmp bx,si / jae B / push cx / mov cx,bp / mov ax,ss:[7be] / lcall 0:e51 / pop cx / add ch,cl /
//   adc bp,dx / jmp N / B: push cx / mov cx,ss:[7c0] / add cx,100 / mov ax,ss:[7be] / lcall 0:e51 / pop cx /
//   N: inc bx / cmp bx,f0 / jb
#define REI_GMAP_PC 0x905b
static REI_FAST_FN int rei_gmap_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ip0 = nec_state->ip, ss = Sreg(SS_) << 4, sp = Wreg(SP), ps = Sreg(PS), bx = Wreg(BW), cx0 = Wreg(CW);
	if (bx >= 0xf0 || sp < 8 || sp > 0xfffb)
		return -1;
	// reads after push cx / lcall (ss:[7be], ss:[7c0], 0x0e51's [7aa] and table store) must not meet those pushes
	const UINT32 lo = ss + sp - 6, hi = ss + sp;
	const UINT32 t = ss + (((bx << 2) + ((rei_rdw(ss + 0x7aa) & 1) ? 0x3d2 : 0x12)) & 0xffff);
	if ((ss + 0x7be < hi && ss + 0x7c0 > lo) || (ss + 0x7c0 < hi && ss + 0x7c2 > lo) || (ss + 0x7aa < hi && ss + 0x7ac > lo) ||
	    (t < hi && t + 4 > lo))
		return -1;
	const int lt = bx < Wreg(IY), a = !lt && !(bx >= Wreg(IX)); // (B by jb or by jae: different cycles)
	Wreg(AW) = rei_rdw(ss + 0x7be);
	Wreg(CW) = a ? Wreg(BP) : (WORD)(rei_rdw(ss + 0x7c0) + 0x100);
	const UINT32 ret = ip0 + (a ? 0x906f - REI_GMAP_PC : 0x908a - REI_GMAP_PC);
	if (wr) {
		write_mem_word(ss + sp - 2, (UINT16)cx0); // push cx
		write_mem_word(ss + sp - 4, (UINT16)ps);  // lcall 0:e51
		write_mem_word(ss + sp - 6, (UINT16)ret);
	}
	Wreg(SP) = (WORD)(sp - 6);
	const int id = rei_ltab_apply(nec_state, 0, wr);
	if (id < 0)
		return -1;
	Wreg(SP) = (WORD)sp;
	Sreg(PS) = (WORD)ps;
	Wreg(CW) = (WORD)cx0; // pop cx
	if (a) { // add ch,cl / adc bp,dx
		const UINT32 r = (cx0 >> 8) + (cx0 & 0xff);
		Wreg(CW) = (WORD)(((r & 0xff) << 8) | (cx0 & 0xff));
		Wreg(BP) = (WORD)(Wreg(BP) + Wreg(DW) + (r >> 8));
	}
	const UINT32 nbx = (bx + 1) & 0xffff; // inc bx / cmp bx,f0 / jb
	Wreg(BW) = (WORD)nbx;
	{
		UINT32 dst = nbx, src = 0xf0;
		SUBW;
		(void)dst;
	}
	const int exit = nbx >= 0xf0;
	nec_state->ip = (WORD)(exit ? ip0 + (0x9092 - REI_GMAP_PC) : ip0);
	return (a ? 2 : lt) | id << 2 | exit << 5;
}
static struct rei_fp rei_gmap_fp REI_FAST_DATA;

// 0x108a (stage intro: fills a row of VRAM cells, ~30% of the V33 instructions there):
//   cmp ax,ffff / je + / mov es:[bx],ax / + cmp dx,ffff / je + / mov es:[bx+2],dx / + add bl,4 / dec cl / jne
// Only for VRAM (its writes are just logged); other targets may act on the instruction's cycle.
#define REI_VFILL_PC 0x108a
static REI_FAST_FN int rei_vfill_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 es = Sreg(DS1) << 4, ax = Wreg(AW), dx = Wreg(DW);
	UINT32 bx = Wreg(BW);
	const UINT32 a0 = es + bx, a1 = es + ((bx + 2) & 0xffff);
	if ((a0 & 0xf0000) != 0xd0000 || ((a0 + 1) & 0xf0000) != 0xd0000 || (a1 & 0xf0000) != 0xd0000 ||
	    ((a1 + 1) & 0xf0000) != 0xd0000)
		return -1;
	if (wr) {
		if (ax != 0xffff)
			write_mem_word(a0, (UINT16)ax);
		if (dx != 0xffff)
			write_mem_word(a1, (UINT16)dx);
	}
	const int par = (int)(bx & 1);
	{ // add bl,4
		UINT32 dst = bx & 0xff, src = 4;
		ADDB;
		bx = (bx & 0xff00) | dst;
	}
	Wreg(BW) = (WORD)bx;
	const UINT32 tmp = Wreg(CW) & 0xff, tmp1 = tmp - 1; // dec cl
	nec_state->OverVal = (tmp == 0x80);
	SetAF(tmp1, tmp, 1);
	SetSZPF_Byte(tmp1);
	Wreg(CW) = (WORD)((Wreg(CW) & 0xff00) | (tmp1 & 0xff));
	const int exit = (tmp1 & 0xff) == 0;
	if (exit)
		nec_state->ip = (WORD)(nec_state->ip + 0x18);
	return (ax == 0xffff) | (dx == 0xffff) << 1 | exit << 2 | par << 3;
}
#define rei_vfill_fp rei_wipec_fp

// Learned piece like rei_fp_piece, for pieces with many branch paths: apply returns a signature (any value >= 0) and the
// costs are kept in an open-addressed table keyed by (signature, prefetch state). A full probe run is not learned.
#define REI_FPH_SIZE 4096
struct rei_fph {
	int state;
	unsigned stats[3];
	struct { UINT32 key; INT16 d; INT8 pf; UINT8 rst; } tab[REI_FPH_SIZE];
};
static REI_FAST_FN int rei_fph_piece(struct rei_fph *fp, nec_state_t *nec_state, int pf_sh, rei_fp_apply_fn apply, int sub,
				     UINT32 code, UINT32 len, UINT32 hash)
{
	if (fp->state == 0)
		fp->state = rei_code_hash(code, len) == hash ? 1 : -1;
	if (fp->state < 0 || nec_state->no_interrupt || nec_state->seg_prefix || nec_state->prefetch_count < 0 ||
	    nec_state->prefetch_count > 7)
		return 0;
	nec_state_t copy = *nec_state;
	const int sig = apply(&copy, sub, 0);
	if (sig < 0)
		return 0;
	const UINT32 sk = (UINT32)sig << 1 | (UINT32)(sub & 1);
	const UINT32 key = (sk << 4 | (UINT32)(nec_state->prefetch_count & 7) << 1 | (UINT32)(nec_state->prefetch_reset & 1)) + 1;
	UINT32 slot = (key * 0x9e3779b1u) >> 20, i;
	for (i = 0; i < 8; i++, slot = (slot + 1) & (REI_FPH_SIZE - 1))
		if (fp->tab[slot].key == key || fp->tab[slot].key == 0)
			break;
	if (i == 8)
		return 0;
	if (fp->tab[slot].key == 0 || REI_FP_VERIFY) {
		// learn with the real handlers up to the end of the piece; the native result must match
		const int ic0 = nec_state->icount;
		for (int n = 0; n < 80; n++) {
			if (nec_state->icount <= 0)
				return 0;
			if (nec_state->pending_irq && !nec_state->no_interrupt && ((nec_state->pending_irq & NMI_IRQ) || nec_state->IF))
				return 0;
			if (nec_state->no_interrupt)
				nec_state->no_interrupt--;
			const int prev = nec_state->icount;
			rei_ins_ic = prev;
			nec_instruction[fetchop(nec_state)](nec_state);
			do_prefetch(nec_state, prev, pf_sh);
			if (nec_state->ip == copy.ip && Sreg(PS) == copy.sregs[PS] && Wreg(SP) == copy.regs.w[SP])
				break;
		}
		if (nec_state->ip != copy.ip || memcmp(nec_state->regs.w, copy.regs.w, sizeof(copy.regs)) ||
		    memcmp(nec_state->sregs, copy.sregs, sizeof(copy.sregs)) || nec_state->CarryVal != copy.CarryVal ||
		    nec_state->OverVal != copy.OverVal || nec_state->AuxVal != copy.AuxVal || nec_state->SignVal != copy.SignVal ||
		    nec_state->ZeroVal != copy.ZeroVal || nec_state->ParityVal != copy.ParityVal || nec_state->no_interrupt) {
			REI_FP_REPORT("state", key);
			fp->stats[2]++;
			fp->state = -1;
			return 0;
		}
		if (fp->tab[slot].key != 0) { // (verify: already learned)
			if (fp->tab[slot].d != ic0 - nec_state->icount || fp->tab[slot].pf != nec_state->prefetch_count ||
			    fp->tab[slot].rst != nec_state->prefetch_reset) {
				REI_FP_REPORT("cost", key);
				fp->stats[2]++;
				fp->state = -1;
			}
			return 0;
		}
		fp->tab[slot].key = key;
		fp->tab[slot].d = (INT16)(ic0 - nec_state->icount);
		fp->tab[slot].pf = nec_state->prefetch_count;
		fp->tab[slot].rst = nec_state->prefetch_reset;
		fp->stats[1]++;
		return 0;
	}
	const int d = fp->tab[slot].d;
	if (nec_state->icount - d < 1)
		return 0;
	apply(nec_state, sub, 1);
	nec_state->icount -= d;
	nec_state->prefetch_count = fp->tab[slot].pf;
	nec_state->prefetch_reset = fp->tab[slot].rst;
	fp->stats[0]++;
	return 1;
}

// 0x3b6f / 0x3c5e: the sprite pieces of an object's animation script command 1 (script at es:di, object at ds), every
// play scene. One piece (0x3b6f) up to the call of 0x23a3 (slot list, own fast path) or to the next piece (0x3c5e):
//   test byte [8f],ff / je A / cmp byte [8f],1 / jne N / mov dx,21 / test word ss:[7aa],2 / je C / mov dx,22 / jmp C |
//   N: mov dx,23 / test word ss:[7aa],1 / jne C / mov dx,24 / jmp C |
//   A: mov bl,es:[di+1] / rol bl,2 / and bx,3 / test bx,bx / jne B / mov bl,[21] / and bx,3 | B: shl bx,1 /
//   mov dx,[bx+10] / and dx,7f |
//   C: mov bx,[c] / mov [e],bx / and bx,780 / mov al,es:[di+1] / mov bp,ax / and al,3 / xor bh,al / or dx,bx /
//   mov bx,[18] / test bp,8 / je P / mov ax,[26] / mov cx,[28] / jmp Y |
//   P: mov ax,es:[di+4] / test word [c],100 / je / neg ax / add ax,[26] / mov cx,es:[di+6] / test word [c],200 / je /
//   neg cx / add cx,[28] |
//   Y: add cx,[30] / test word [c],4000 / jne S / test word [c],10 / je D / test bp,4 / jne S |
//   D: test word ss:[7b0],1 / je E / test word ss:[7a0],8000 / je E / test byte [6b],ff / je E / test word ss:[7aa],1 /
//   je S | E: test word [4c],1000 / jne S / mov si,es:[di+2] / call 0:23a3 | S: (0x3c5e)
// 0x3c5e (sub 1): mov al,es:[di+1] / add di,4 / test al,8 / jne / add di,4 / cmp byte es:[di],1 / jne (end: interpreter) /
//   jmp 3b6f, then the piece above.
#define REI_SPC_PC   0x3b6f
#define REI_SPCP_PC  0x3b5c // (4 instructions before it: sub 2)
#define REI_SPCN_PC  0x3c5e
// end of the piece list (0x3c6c with es:[di] != 1), up to the retf:
//   jne E | E: mov cl,[1e] / test cl,cl / je S / test byte [6a],ff / jne C / dec cl / mov [1e],cl / jne C /
//   cmp byte es:[di],c / je S / mov [1a],di | C: clc / retf | S: stc / retf
static REI_FAST_FN int rei_spc_end(nec_state_t *nec_state, int wr, UINT32 ax, UINT32 di, UINT32 sig)
{
	const UINT32 ds = Sreg(DS0) << 4, es = Sreg(DS1) << 4, ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (sp > 0xfffb || (es + di >= ds + 0x1e && es + di < ds + 0x1f))
		return -1;
	sig = sig << 1 | 0;
	UINT32 cl = cpu_readmem20(ds + 0x1e);
	int carry, wcl = 0, wdi = 0;
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0; // test cl,cl
	SetSZPF_Byte(cl);
	sig = sig << 1 | (cl == 0);
	if (cl == 0) {
		carry = 1;
	} else {
		const UINT32 f6a = cpu_readmem20(ds + 0x6a);
		SetSZPF_Byte(f6a);                                           // test byte [6a],ff (A stays 0)
		sig = sig << 1 | (f6a != 0);
		if (f6a) {
			carry = 0;
		} else {
			const UINT32 t = (cl - 1) & 0xff;                            // dec cl
			nec_state->OverVal = (cl == 0x80); SetAF(t, cl, 1); SetSZPF_Byte(t);
			cl = t;
			wcl = 1;
			sig = sig << 1 | (t == 0);
			if (t) {
				carry = 0;
			} else {
				const UINT32 v = cpu_readmem20(es + di), res = v - 0xc; // cmp byte es:[di],c
				SetCFB(res); SetOFB_Sub(res, 0xc, v); SetAF(res, 0xc, v); SetSZPF_Byte(res);
				sig = sig << 1 | (v == 0xc);
				carry = v == 0xc;
				wdi = v != 0xc;
			}
		}
	}
	if (wr) {
		if (wcl)
			cpu_writemem20(ds + 0x1e, cl);
		if (wdi)
			write_mem_word(ds + 0x1a, (UINT16)di);
	}
	nec_state->CarryVal = carry;                                      // clc / stc
	Wreg(AW) = (WORD)ax;
	Wreg(CW) = (WORD)((Wreg(CW) & 0xff00) | cl);
	Wreg(IY) = (WORD)di;
	nec_state->ip = rei_rdw(ss + sp);                                 // retf
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 4);
	return (int)((sig << 1 | (sp & 1)) << 1 | 1); // (bit 0 tells it from a piece)
}
static int rei_spcp_code;
static REI_FAST_FN int rei_spc_apply(nec_state_t *nec_state, int sub, int wr)
{
	const UINT32 ds = Sreg(DS0) << 4, es = Sreg(DS1) << 4, ss = Sreg(SS_) << 4, sp = Wreg(SP);
	UINT32 di = Wreg(IY), ax = Wreg(AW);
	UINT32 sig = 1;
	if (sub == 2) { // 0x3b5c, just before: mov [22],di / test [c],404 / jne + / mov ss:[1af6],ffff / + (0x3b6f)
		const UINT32 c = rei_rdw(ds + 0xc) & 0x404, w2 = ss + 0x1af6;
		// the piece's reads (object ds+0..8f, script es:di.., ss:7a0..7b1) must not see these writes
		if ((ds + 0x22 < es + di + 8 && ds + 0x24 > es + di) ||
		    (!c && ((w2 < ds + 0x90 && w2 + 2 > ds) || (w2 < es + di + 8 && w2 + 2 > es + di) || (w2 < ss + 0x7b2 && w2 + 2 > ss + 0x7a0))))
			return -1;
		if (wr) {
			write_mem_word(ds + 0x22, (UINT16)di);
			if (!c)
				write_mem_word(w2, 0xffff);
		}
		nec_state->CarryVal = nec_state->OverVal = 0; // (test: A kept; the piece's own tests come next)
		SetSZPF_Word(c);
		nec_state->ip = (WORD)(nec_state->ip + (REI_SPC_PC - REI_SPCP_PC));
		sub = 0;
		sig = c ? 2 : 3; // (keys apart from the plain piece)
	}
	if (sub) { // next piece
		const UINT32 b = cpu_readmem20(es + ((di + 1) & 0xffff));
		ax = (ax & 0xff00) | b;
		di = (di + ((b & 8) ? 4 : 8)) & 0xffff;
		sig = sig << 1 | ((b >> 3) & 1);
		if (cpu_readmem20(es + di) != 1)
			return rei_spc_end(nec_state, wr, ax, di, sig);
		sig = sig << 1 | 1;
	}
	if (di > 0xfff9 || sp < 4)
		return -1;
	// reads that could see the [e] write or the call's pushes: interpreter
	const UINT32 e = ds + 0xe;
	if ((es + di < e + 2 && es + di + 8 > e) || (ss + 0x7a0 < e + 2 && ss + 0x7b2 > e) ||
	    (ss + sp - 4 < ds + 0x90 && ss + sp > ds))
		return -1;
	const UINT32 b1 = cpu_readmem20(es + di + 1);
	const UINT32 f8f = cpu_readmem20(ds + 0x8f);
	UINT32 dx;
	if (f8f == 0) {
		sig = sig << 2 | 0;
		UINT32 bx = b1 >> 6;
		sig = sig << 1 | (bx == 0);
		if (bx == 0)
			bx = cpu_readmem20(ds + 0x21) & 3;
		dx = rei_rdw(ds + 0x10 + 2 * bx) & 0x7f;
	} else if (f8f == 1) {
		const int t = (rei_rdw(ss + 0x7aa) & 2) != 0;
		sig = (sig << 2 | 1) << 1 | (UINT32)t;
		dx = t ? 0x22 : 0x21;
	} else {
		const int t = (rei_rdw(ss + 0x7aa) & 1) != 0;
		sig = (sig << 2 | 2) << 1 | (UINT32)t;
		dx = t ? 0x23 : 0x24;
	}
	const UINT32 c = rei_rdw(ds + 0xc);
	const UINT32 bp = (ax & 0xff00) | b1;
	dx |= ((((c >> 8) & 7) ^ (b1 & 3)) << 8) | (c & 0x80);
	const UINT32 bx = rei_rdw(ds + 0x18);
	UINT32 nax, ncx;
	if (bp & 8) {
		sig = sig << 1 | 1;
		nax = rei_rdw(ds + 0x26);
		ncx = rei_rdw(ds + 0x28);
	} else {
		sig = (sig << 1) << 2 | ((c >> 8) & 3); // 0 + flip bits
		const UINT32 x = rei_rdw(es + di + 4), y = rei_rdw(es + di + 6);
		nax = ((c & 0x100) ? -x : x) + rei_rdw(ds + 0x26);
		ncx = ((c & 0x200) ? -y : y) + rei_rdw(ds + 0x28);
	}
	nax &= 0xffff;
	ncx &= 0xffff;
	const UINT32 a30 = rei_rdw(ds + 0x30), cx0 = ncx;
	ncx = (cx0 + a30) & 0xffff;
	// draw or skip: S, Z, P are those of the last test
	int draw = 0;
	UINT32 fres = 0;
	if (c & 0x4000) {
		sig = sig << 1 | 1; fres = c & 0x4000;
	} else if ((sig = sig << 1, sig = sig << 1 | ((c >> 4) & 1), (c & 0x10) && (bp & 4))) {
		sig = sig << 1 | 1; fres = bp & 4;
	} else {
		if (c & 0x10)
			sig = sig << 1;
		int skip = 0;
		const UINT32 g7b0 = rei_rdw(ss + 0x7b0) & 1;
		sig = sig << 1 | g7b0;
		if (g7b0) {
			const UINT32 g7a0 = (rei_rdw(ss + 0x7a0) & 0x8000) != 0;
			sig = sig << 1 | g7a0;
			if (g7a0) {
				const UINT32 g6b = cpu_readmem20(ds + 0x6b) != 0;
				sig = sig << 1 | g6b;
				if (g6b) {
					const UINT32 g7aa = rei_rdw(ss + 0x7aa) & 1;
					sig = sig << 1 | g7aa;
					if (!g7aa) { skip = 1; fres = 0; }
				}
			}
		}
		if (!skip) {
			const UINT32 g4c = rei_rdw(ds + 0x4c) & 0x1000;
			sig = sig << 1 | (g4c != 0);
			fres = g4c;
			draw = g4c == 0;
		}
	}
	if (sig >> 22)
		return -1; // (signature too long for the key)
	const UINT32 nsi = draw ? rei_rdw(es + di + 2) : Wreg(IX);
	if (wr) {
		write_mem_word(e, (UINT16)c);
		if (draw) {
			write_mem_word(ss + sp - 2, (UINT16)Sreg(PS));
			write_mem_word(ss + sp - 4, (UINT16)(nec_state->ip + (REI_SPCN_PC - (sub ? REI_SPCN_PC : REI_SPC_PC))));
		}
	}
	Wreg(AW) = (WORD)nax;
	Wreg(BW) = (WORD)bx;
	Wreg(CW) = (WORD)ncx;
	Wreg(DW) = (WORD)dx;
	Wreg(BP) = (WORD)bp;
	Wreg(IX) = (WORD)nsi;
	Wreg(IY) = (WORD)di;
	// the tests (f6/f7 /0, test bp,imm) clear C, O but keep A from add cx,[30]
	nec_state->CarryVal = nec_state->OverVal = 0;
	SetAF(cx0 + a30, a30, cx0);
	SetSZPF_Word(fres);
	if (draw) {
		nec_state->ip = 0x23a3;
		Sreg(PS) = 0;
		Wreg(SP) = (WORD)(sp - 4);
	} else if (!sub) {
		nec_state->ip = (WORD)(nec_state->ip + (REI_SPCN_PC - REI_SPC_PC));
	} else {
		return -1; // (sub 1 that skips would end at 0x3c5e again with a different di: leave it to the interpreter)
	}
	return (int)((sig << 2 | (di & 1) << 1 | (sp & 1)) << 1);
}
static struct rei_fph rei_spc_fp REI_FAST_DATA;

// 0x39c3: the animation script interpreter's dispatch (every object, every frame), up to the command handler
//   test word ss:[7b0],1 / je / (call 6000:5816) | les di,[1a] / cmp di,[22] / je D / mov al,[1f] / inc al / je D /
//   mov [1f],al | D: mov bl,es:[di] / and bl,f / cmp bl,d / jb / (int3) / xor bh,bh / add bx,bx / jmp cs:[39fa+bx]
#define REI_SDIS_PC 0x39c3
static REI_FAST_FN int rei_sdis_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4;
	if (Sreg(PS) != 0 || (rei_rdw(ss + 0x7b0) & 1))
		return -1; // (the table is cs-relative; the far call)
	const UINT32 di = rei_rdw(ds + 0x1a), es = rei_rdw(ds + 0x1c);
	if (((es << 4) + di) == ds + 0x1f)
		return -1; // (es:[di] read after the [1f] write)
	UINT32 ax = Wreg(AW), sig = 1;
	int w1f = 0;
	const int eq = di == rei_rdw(ds + 0x22);
	sig = sig << 1 | (UINT32)eq;
	if (!eq) {
		const UINT32 al = (cpu_readmem20(ds + 0x1f) + 1) & 0xff;
		ax = (ax & 0xff00) | al;
		sig = sig << 1 | (al == 0);
		w1f = al != 0;
	}
	const UINT32 bl = cpu_readmem20((es << 4) + di) & 0xf;
	if (bl >= 0xd)
		return -1;
	if (wr && w1f)
		cpu_writemem20(ds + 0x1f, ax & 0xff);
	const UINT32 bx = bl * 2;
	{ UINT32 dst = bl, src = bl, res = dst + src; // add bx,bx
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
	Wreg(AW) = (WORD)ax;
	Wreg(BW) = (WORD)bx;
	Wreg(IY) = (WORD)di;
	Sreg(DS1) = (WORD)es;
	nec_state->ip = rei_rdw(0x39fa + bx);
	return (int)(sig << 4 | bl);
}
static struct rei_fph rei_sdis_fp REI_FAST_DATA;

// 0x657b6 (collision: the object list loop that checks each object against an attack box with 0x655bc, ~5% of the V33
// instructions in busy play): the iterations where 0x655bc rejects the object at once.
//   mov bp,[bp+6] / shl bp,4 / cmp word [bp+a],0 / jbe (end) / call 655bc / jae N / (or word [c],1000) / N: jmp 657b6
//   655bc: push es / test word [bp+4c],800 / jne K / test byte es:[si+1],ff / jne D | K: test byte [bp+6b],ff / je D /
//   jmp R | D: mov ax,ds / shl ax,4 / cmp bp,ax / jne F / jmp R | F: mov di,[bp+60] / cmp di,ffff / jne (full check) /
//   jmp R | R: clc / pop es / ret
#define REI_COLL_PC 0x657b6
#define REI_COLL2_PC 0x657a1 // the same for the list at 0x4840: mov bp,[bp+6] / shl bp,4 / cmp word [bp+a],0 / jbe / call 655bc / jmp 657a1
static int rei_coll_code; // 0 unchecked, 1 matches, -1 different
static int rei_coll2_code;
static REI_FAST_FN int rei_box_apply(nec_state_t *nec_state, int sub, int wr);
// the full check of 0x655bc for a pair the loop did not skip (di = [bp+60] != ffff), when the boxes end up apart:
//   mov ax,[50] / cmp ax,ffff / jne + / + mov dx,[bp+52] / cmp dx,ffff / je T / cmp dx,ax / js T / cmp ss:[3205],dx /
//   js T / jmp 656aa (skip) / T: add si,a / push es / lcall 6000:54d4 / lcall 6000:543b / (box test, apart: rei_box_apply)
//   / jb (no) / lret / pop es / jb (no) / sub si,a / jmp 656aa / clc / pop es / ret
// One piece for the whole loop iteration (one native call instead of ~35 interpreted instructions and the box call:
// a native call costs about as much as 15 interpreted instructions on the PSP). Boxes that overlap: interpreter
static REI_FAST_FN int rei_coll_full(nec_state_t *nec_state, int wr, UINT32 ss, UINT32 sp, UINT32 o, UINT32 bp, UINT32 di,
				    UINT32 sig)
{
	static int code; // the rest of 0x655bc, 0x654d4, 0x6561a and the box test: checked once
	if (code == 0)
		code = rei_code_hash(0x655bc, 0x5e) == 0x20ae81c6u && rei_code_hash(0x654d4, 8) == 0x316202e8u &&
		       rei_code_hash(0x6561a, 9) == 0xf73a4481u && rei_code_hash(0x6543b, 0x99) == 0x4c2d9c79u ? 1 : -1;
	const UINT32 ds = Sreg(DS0) << 4, es0 = Sreg(DS1), si0 = Wreg(IX), ps = Sreg(PS), ip0 = nec_state->ip;
	if (code < 0 || sp < 14 || sp > 0xfffd)
		return -1;
	const UINT32 lo = ss + sp - 14, hi = ss + sp; // what this piece pushes
#define OVL(a, n) ((a) < hi && (a) + (n) > lo)
	if (OVL(ds + 0x50, 2) || OVL(o + 0x52, 2) || OVL(ss + 0x3205, 2))
		return -1;
	const UINT32 ax = rei_rdw(ds + 0x50);
	if (ax == 0xffff)
		return -1; // (lcall 6000:53c0)
	const UINT32 dx = rei_rdw(o + 0x52);
	int t;
	if (dx == 0xffff) {
		UINT32 dst = dx, src = 0xffff;
		SUBW;
		(void)dst;
		t = 0;
	} else {
		UINT32 dst = dx, src = ax;
		SUBW;
		if (dst & 0x8000)
			t = 1;
		else {
			dst = rei_rdw(ss + 0x3205);
			src = dx;
			SUBW;
			if (dst & 0x8000)
				t = 2;
			else { // jmp 656aa: clc / pop es / ret
				if (wr) {
					write_mem_word(ss + sp - 2, (UINT16)(ip0 + (0x657c5 - REI_COLL_PC)));
					write_mem_word(ss + sp - 4, (UINT16)es0);
				}
				nec_state->CarryVal = 0;
				Wreg(BP) = (WORD)bp;
				Wreg(AW) = (WORD)ax;
				Wreg(IY) = (WORD)di;
				Wreg(DW) = (WORD)dx;
				return (int)(sig << 3 | 3) << 10;
			}
		}
	}
	// the box test's reads must not see the pushes
	const UINT32 si1 = (si0 + 0xa) & 0xffff, ea = (UINT32)rei_rdw(ds + 0x1c) << 4, eb = (UINT32)rei_rdw(o + 0x1c) << 4;
	if (OVL(ds + 0x1c, 2) || OVL(ds + 0xc, 2) || OVL(ds + 0x26, 2) || OVL(o + 0x1c, 2) || OVL(o + 0xc, 2) || OVL(o + 0x26, 2) ||
	    OVL(ea + si1, 2) || OVL(ea + ((si1 + 2) & 0xffff), 2) || OVL(eb + di, 2) || OVL(eb + ((di + 2) & 0xffff), 2))
		return -1;
#undef OVL
	Wreg(BP) = (WORD)bp;
	Wreg(AW) = (WORD)ax;
	Wreg(IY) = (WORD)di;
	Wreg(DW) = (WORD)dx;
	Wreg(IX) = (WORD)si1;
	Wreg(SP) = (WORD)(sp - 14);
	const int b = rei_box_apply(nec_state, 0, wr);
	if (b < 0 || ((b >> 2) & 7) >= 2)
		return -1; // (overlapping boxes: longer than a piece can learn)
	if (wr) {
		write_mem_word(ss + sp - 2, (UINT16)(ip0 + (0x657c5 - REI_COLL_PC))); // call 655bc
		write_mem_word(ss + sp - 4, (UINT16)es0);                            // push es
		write_mem_word(ss + sp - 6, (UINT16)es0);                            // push es
		write_mem_word(ss + sp - 8, (UINT16)ps);                             // lcall 6000:54d4
		write_mem_word(ss + sp - 10, 0x561a);
		write_mem_word(ss + sp - 12, 0x6000);                                // lcall 6000:543b
		write_mem_word(ss + sp - 14, 0x54d9);
	}
	{ // (after the box test's lret) pop es / sub si,a / clc / pop es / ret
		UINT32 dst = si1, src = 0xa;
		SUBW;
		(void)dst;
	}
	nec_state->CarryVal = 0;
	Wreg(IX) = (WORD)si0;
	Sreg(DS1) = (WORD)es0;
	Sreg(PS) = (WORD)ps;
	nec_state->ip = (WORD)ip0;
	Wreg(SP) = (WORD)sp;
	return (int)((sig << 3 | (UINT32)t) << 10 | 1u << 9 | (UINT32)(b & 0x1ff));
}
static REI_FAST_FN int rei_coll_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), obp = Wreg(BP);
	if (sp < 4 || obp > 0xfff9)
		return -1;
	const UINT32 bp = (rei_rdw(ss + obp + 6) << 4) & 0xffff;
	if (bp > 0xff94)
		return -1;
	const UINT32 a = rei_rdw(ss + bp + 0xa);
	if (a == 0)
		return -1; // (end of the list)
	// reads after the pushes ([bp+4c], [bp+6b], [bp+60], es:[si+1]) must not see them
	const UINT32 st = ss + sp - 4, o = ss + bp, e1a = (Sreg(DS1) << 4) + ((Wreg(IX) + 1) & 0xffff);
	if ((o + 0x4c < st + 4 && o + 0x62 > st) || (e1a >= st && e1a < st + 4))
		return -1;
	UINT32 sig = 1, ax = Wreg(AW), di = Wreg(IY), fres;
	int byte = 0, k;
	const int t4c = (rei_rdw(o + 0x4c) & 0x800) != 0;
	sig = sig << 1 | (UINT32)t4c;
	if (t4c) {
		k = 1;
	} else {
		const int e1 = cpu_readmem20(e1a) != 0;
		sig = sig << 1 | (UINT32)e1;
		k = !e1;
	}
	int rej = 0;
	if (k) { // test byte [bp+6b],ff
		const UINT32 b6b = cpu_readmem20(o + 0x6b);
		sig = sig << 1 | (b6b != 0);
		if (b6b) {
			rej = 1;
			byte = 1;
			fres = b6b;
		}
	}
	if (!rej) {
		ax = (Sreg(DS0) << 4) & 0xffff;
		const int eq = bp == ax;
		sig = sig << 1 | (UINT32)eq;
		if (!eq) {
			di = rei_rdw(o + 0x60);
			if (di != 0xffff)
				return rei_coll_full(nec_state, wr, ss, sp, o, bp, di, sig); // (the full check)
		}
		fres = 0; // cmp bp,ax / cmp di,ffff: equal
	}
	if (wr) {
		write_mem_word(ss + sp - 2, (UINT16)(nec_state->ip + (0x657c5 - REI_COLL_PC))); // call 655bc (0x657b0 - REI_COLL2_PC the same)
		write_mem_word(st, Sreg(DS1));                                                    // push es
	}
	Wreg(BP) = (WORD)bp;
	Wreg(AW) = (WORD)ax;
	Wreg(IY) = (WORD)di;
	// flags: the last test or compare (C, O clear; A is 0 both ways: test keeps the A of cmp word [bp+a],0, which is 0,
	// and an equal compare gives 0), then clc
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0;
	if (byte)
		SetSZPF_Byte(fres);
	else
		SetSZPF_Word(fres);
	return (int)(sig << 1 | (sp & 1));
}
static struct rei_fph rei_coll_fp REI_FAST_DATA;

// 0x2011 (palette fade toward a target, ~5% of the V33 instructions in stage 6): one colour of the loop of 0x1ffb
//   push cx / push ax / lodsw / call 2077 / pop ax / pop cx / sbb bl,bl / and ah,bl / loop 2011
//   2077: mov bx,ax / mov cx,es:[di] / and bx,7c1f / and cx,7c1f / mov dx,401 / sub bl,cl / je / jae / neg dl /
//   add cl,dl / sub bh,ch / je / jae / neg dh / add ch,dh / and cx,7c1f / mov bx,es:[di] / and ax,3e0 / and bx,3e0 /
//   mov dx,20 / sub ax,bx / je / jae / neg dx / add bx,dx / and bx,3e0 / mov ax,bx / or ax,cx / cmp es:[di],ax / je E /
//   stosw / clc / ret | E: add di,2 / stc / ret
// (one step of each of red, blue, green toward the target colour at ds:si; CF = unchanged)
#define REI_PFT_PC 0x2011
static int rei_pft_code;
static inline UINT32 rei_pft_step(UINT32 t, UINT32 c, UINT32 step, UINT32 *sig)
{
	if (t == c) { *sig = *sig * 3; return c; }
	if (t > c) { *sig = *sig * 3 + 1; return c + step; }
	*sig = *sig * 3 + 2;
	return c - step;
}
static REI_FAST_FN int rei_pft_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), si = Wreg(IX), di = Wreg(IY), ax0 = Wreg(AW), cx0 = Wreg(CW);
	if (nec_state->DF || sp < 6 || si > 0xfffd || di > 0xfffd)
		return -1;
	const UINT32 src = (Sreg(DS0) << 4) + si, dst = (Sreg(DS1) << 4) + di, st = ss + sp - 6;
	if ((src + 2 > st && src < st + 6) || (dst + 2 > st && dst < st + 6))
		return -1;
	const UINT32 t = rei_rdw(src), c = rei_rdw(dst);
	UINT32 sig = 0;
	const UINT32 r = rei_pft_step(t & 0x1f, c & 0x1f, 1, &sig);
	const UINT32 b = rei_pft_step((t >> 8) & 0x7c, (c >> 8) & 0x7c, 4, &sig);
	const UINT32 g = rei_pft_step(t & 0x3e0, c & 0x3e0, 0x20, &sig);
	const UINT32 dx = (t & 0x3e0) < (c & 0x3e0) ? 0xffe0 : 0x20;
	const UINT32 col = ((b << 8 | r) & 0x7c1f) | (g & 0x3e0);
	const int same = col == c;
	if (wr) {
		write_mem_word(st + 4, (UINT16)cx0);                                         // push cx
		write_mem_word(st + 2, (UINT16)ax0);                                         // push ax
		write_mem_word(st, (UINT16)(nec_state->ip + (0x2017 - REI_PFT_PC)));          // call 2077
		if (!same)
			write_mem_word(dst, (UINT16)col);                                        // stosw
	}
	const UINT32 bl = same ? 0xff : 0, ah = ((ax0 >> 8) & bl) & 0xff;
	Wreg(AW) = (WORD)((ax0 & 0xff) | ah << 8);
	Wreg(BW) = (WORD)(((g & 0x3e0) & 0xff00) | bl);
	Wreg(DW) = (WORD)dx;
	Wreg(IX) = (WORD)(si + 2);
	Wreg(IY) = (WORD)(di + 2);
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0; // and ah,bl
	SetSZPF_Byte(ah);
	Wreg(CW) = (WORD)(cx0 - 1);
	if (Wreg(CW) == 0)
		nec_state->ip = (WORD)(nec_state->ip + (0x201f - REI_PFT_PC));
	return (int)(((sig * 2 + (UINT32)same) * 2 + (si & 1)) * 4 + (di & 1) * 2 + (sp & 1));
}
static struct rei_fph rei_pft_fp REI_FAST_DATA;

// 0x202ee (for each of the 4 players, ~6% of the V33 instructions in busy play): the loop around 0x3eedb, whose middle
// is the player check 0x3ef04 (own fast path).
//  A (0x202ee -> 0x3ef04): push cx / push bp / call 3000:eedb / cmp bp,4480 / jb / cmp bp,46c0 / ja / call 3000:ef04
//  B (0x3eeec -> 0x202ee, check not hit): jae R / ... | R: clc / retf / pop bp / pop cx / jae N / ... | N: add bp,c0 /
//    loop 202ee
#define REI_PLA_PC 0x202ee
#define REI_PLB_PC 0x3eeec
static int rei_plab_code;
static REI_FAST_FN int rei_pla_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), bp = Wreg(BP);
	if (sp < 12 || bp < 0x4480 || bp > 0x46c0)
		return -1;
	if (wr) {
		write_mem_word(ss + sp - 2, Wreg(CW));
		write_mem_word(ss + sp - 4, (UINT16)bp);
		write_mem_word(ss + sp - 6, Sreg(PS));
		write_mem_word(ss + sp - 8, (UINT16)(nec_state->ip + (0x202f5 - REI_PLA_PC)));
		write_mem_word(ss + sp - 10, 0x3000);
		write_mem_word(ss + sp - 12, 0xeeec);
	}
	{ UINT32 dst = bp, src = 0x46c0, res = dst - src; // cmp bp,46c0
	  SetCFW(res); SetOFW_Sub(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
	Sreg(PS) = 0x3000;
	nec_state->ip = 0xef04;
	Wreg(SP) = (WORD)(sp - 12);
	return (int)(sp & 1);
}
static REI_FAST_FN int rei_plb_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	(void)wr;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP);
	if (nec_state->CarryVal || sp > 0xfff7)
		return -1; // (hit: the long path)
	const UINT32 ip = rei_rdw(ss + sp), ps = rei_rdw(ss + sp + 2); // retf
	if (((ps << 4) + ip) != 0x202f5)
		return -1; // (0x3eedb called from somewhere else)
	const UINT32 bp = rei_rdw(ss + sp + 4), cx = rei_rdw(ss + sp + 6);
	const UINT32 nbp = (bp + 0xc0) & 0xffff;
	{ UINT32 dst = bp, src = 0xc0, res = dst + src; // add bp,c0
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); }
	Wreg(BP) = (WORD)nbp;
	Wreg(CW) = (WORD)(cx - 1);
	Wreg(SP) = (WORD)(sp + 8);
	Sreg(PS) = (WORD)ps;
	nec_state->ip = (WORD)(ip + (Wreg(CW) ? (REI_PLA_PC - 0x202f5) : (0x2031b - 0x202f5)));
	return (int)((Wreg(CW) == 0) << 1 | (sp & 1));
}
static struct rei_fp rei_pla_fp REI_FAST_DATA, rei_plb_fp REI_FAST_DATA;

// 0x3e0d2 (stage 3 wall boxes, ~18% of the V33 instructions there): one box of the loop of 0x3e095 (ss:bp = box: owner,
// x, w, y, h, top; ds = object: [26] x, [28] y, [30] z, [4c] blocked bits, [4e] floor; ss:[3141], [3143] = new x, y)
//   mov ax,ds / cmp [bp],ax / jne / jmp N | mov ax,[26] / mov bx,[28] / T(ax,bx) -> X / mov ax,[bp+a] / cmp ax,[30] / jg N /
//   cmp ax,[4e] / jb N / mov [4e],ax / N |
//   X: mov dx,ss:[3141] / cmp dx,ax / je Y / mov ax,dx / mov bx,[28] / T(ax,bx) -> Y / mov ax,[bp+a] / cmp ax,[30] / jg G /
//   cmp ax,[4e] / jb Y / mov [4e],ax / Y | G: or word [4c],1 / mov ax,[26] |
//   Y: mov dx,ss:[3143] / cmp dx,bx / je N / mov bx,dx / T(ax,bx) -> N / mov ax,[bp+a] / cmp ax,[30] / jg H / cmp ax,[4e] /
//   jb N / mov [4e],ax / N | H: or word [4c],2 |
//   N: add bp,c / dec cx / je (end 0x3e193) / jmp 3e0d2
//   T(a,b): mov dx,a / sub dx,[bp+2] / cmp dx,[bp+4] / ja out / mov dx,b / sub dx,[bp+6] / cmp dx,[bp+8] / ja out
// The code's own register flow is kept (after a new-x hit Y tests with ax = the box top).
#define REI_WALL_PC 0x3e0d2
static REI_FAST_FN int rei_wall_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, bp = Wreg(BP), cx = Wreg(CW);
	if (bp > 0xfff3)
		return -1;
	const UINT32 o = ss + bp;
	// what is read after a write of [4c]/[4e] must not overlap them (the box, ss:[3141..3144])
	if ((o < ds + 0x50 && o + 0xc > ds + 0x4c) || (ss + 0x3141 < ds + 0x50 && ss + 0x3145 > ds + 0x4c))
		return -1;
	const UINT32 owner = rei_rdw(o), bxl = rei_rdw(o + 2), bw = rei_rdw(o + 4), byt = rei_rdw(o + 6), bh = rei_rdw(o + 8);
	const UINT32 top = rei_rdw(o + 0xa), z = rei_rdw(ds + 0x30);
	UINT32 ax, bx = Wreg(BW), dx = Wreg(DW), w4e = rei_rdw(ds + 0x4e), w4c = rei_rdw(ds + 0x4c);
	UINT32 wlog[6]; // ordered writes: (1 = [4e], 2 = [4c]) << 16 | value
	int nw = 0;
	UINT32 sig = 1;
#define REI_WALL_T(a, b, in) do { dx = ((a) - bxl) & 0xffff; in = 0; const int xok = dx <= bw; sig = sig << 1 | (UINT32)xok; \
	if (xok) { dx = ((b) - byt) & 0xffff; in = dx <= bh; sig = sig << 1 | (UINT32)in; } } while (0)
#define REI_WALL_TOP(hitbit) do { ax = top; const int above = (INT16)ax > (INT16)z; sig = sig << 1 | (UINT32)above; \
	if (above) { if (hitbit) { w4c |= (hitbit); wlog[nw++] = 2u << 16 | w4c; if ((hitbit) == 1) ax = rei_rdw(ds + 0x26); } } \
	else { const int lower = ax < w4e; sig = sig << 1 | (UINT32)lower; if (!lower) { w4e = ax; wlog[nw++] = 1u << 16 | w4e; } } } while (0)
	ax = Sreg(DS0);
	if (owner == ax) {
		sig = sig << 1 | 1;
	} else {
		sig <<= 1;
		ax = rei_rdw(ds + 0x26);
		bx = rei_rdw(ds + 0x28);
		int in;
		REI_WALL_T(ax, bx, in);
		if (in) {
			REI_WALL_TOP(0); // (above: nothing)
		} else {
			dx = rei_rdw(ss + 0x3141);
			const int same = dx == ax;
			sig = sig << 1 | (UINT32)same;
			if (!same) {
				ax = dx;
				bx = rei_rdw(ds + 0x28);
				REI_WALL_T(ax, bx, in);
				if (in)
					REI_WALL_TOP(1);
			}
			dx = rei_rdw(ss + 0x3143);
			const int samey = dx == bx;
			sig = sig << 1 | (UINT32)samey;
			if (!samey) {
				bx = dx;
				REI_WALL_T(ax, bx, in);
				if (in)
					REI_WALL_TOP(2);
			}
		}
	}
#undef REI_WALL_T
#undef REI_WALL_TOP
	if (sig >> 26)
		return -1;
	if (wr)
		for (int i = 0; i < nw; i++)
			write_mem_word(ds + ((wlog[i] >> 16) == 1 ? 0x4e : 0x4c), (UINT16)wlog[i]);
	Wreg(AW) = (WORD)ax;
	Wreg(BW) = (WORD)bx;
	Wreg(DW) = (WORD)dx;
	{ UINT32 dst = bp, src = 0xc, res = dst + src; // add bp,c (C stays through dec cx)
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(BP) = (WORD)res; }
	{ const UINT32 t1 = (cx - 1) & 0xffff;            // dec cx
	  nec_state->OverVal = (cx == 0x8000); SetAF(t1, cx, 1); SetSZPF_Word(t1); Wreg(CW) = (WORD)t1; }
	if (Wreg(CW) == 0)
		nec_state->ip = (WORD)(nec_state->ip + (0x3e193 - REI_WALL_PC));
	return (int)(sig << 1 | (bp & 1));
}
static struct rei_fph rei_wall_fp REI_FAST_DATA;

// 0x2180b (an object's hit box es:si against the 4 players ss:bp 0x4480.., ~6% of the V33 instructions in stage 3): one
// player of the loop
//   push dx / cmp bp,4480 / je / cmp bp,4540 / je / cmp bp,4600 / je / cmp bp,46c0 / je / (int3) |
//   cmp word [bp],2953 / jne F / cmp word [bp+2],0 | F: je N / mov ax,[bp+26] / sub ax,[26] / mov cx,[bp+28] / sub cx,[28] /
//   mov di,[bp+30] / sub di,[30] / then for (ax,0) (cx,4) (di,8): mov bx,es:[si+k] / mov dx,es:[si+k+2] / sub dx,bx /
//   sub r,bx / cmp r,dx / jae N | test word [bp+4c],40 / jne N / cmp byte [7e],3 / je B / or word [bp+4a],10 /
//   mov [bp+78],ds / jmp N | B: or word [bp+4a],20 / mov [bp+7a],ds |
//   N: add bp,c0 / pop dx / dec dx / je (end 0x218a3) / jmp 2180b
// 0x6543b (collision box test, called through lcall from 0x654d4; ~20% of the V33 instructions with many enemies in
// stage 1): horizontal extents of the two boxes (each flipped by bit 8 of its object's [c]) moved by the objects' x
// ([26]); apart: clc / lret. Overlapping: the middle x to [bp+5c], then the vertical extents ([28]) and the flags
// of cmp dx,ax for the caller's jb.
//   mov es,[1c] / mov ax,es:[si] / mov dx,es:[si+2] / test [c],100 / je + / neg ax / neg dx / xchg ax,dx / + cmp ax,dx /
//   jl + / int3 / + mov es,[bp+1c] / mov bx,es:[di] / mov cx,es:[di+2] / test [bp+c],100 / je + / neg bx / neg cx /
//   xchg bx,cx / + cmp bx,cx / jl + / int3 / + add ax,[26] / add dx,[26] / add bx,[bp+26] / add cx,[bp+26] /
//   cmp bx,dx / jg A / cmp ax,cx / jle B / A: clc / lret
//   B: cmp ax,bx / jae + / mov ax,bx / + cmp dx,cx / jb + / mov dx,cx / + add dx,ax / sar dx,1 / mov [bp+5c],dx /
//   mov es,[1c] / mov ax,es:[si+4] / mov dx,es:[si+6] / cmp ax,dx / jl + / int3 / + mov es,[bp+1c] /
//   mov bx,es:[di+4] / mov cx,es:[di+6] / cmp bx,cx / jl + / int3 / + sub ax,dx / neg ax / sub cx,bx / add ax,cx /
//   add dx,[28] / add bx,[bp+28] / sub dx,bx / cmp dx,ax / lret
#define REI_BOX_PC 0x6543b
static REI_FAST_FN int rei_box_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ds = Sreg(DS0) << 4, ss = Sreg(SS_) << 4, si = Wreg(IX), di = Wreg(IY), bp = Wreg(BP), sp = Wreg(SP);
	if (sp > 0xfffb)
		return -1;
#define BPW(o) (ss + ((bp + (o)) & 0xffff))
	const UINT32 ea = (UINT32)rei_rdw(ds + 0x1c) << 4, eb = (UINT32)rei_rdw(BPW(0x1c)) << 4;
	UINT32 ax = rei_rdw(ea + si), dx = rei_rdw(ea + ((si + 2) & 0xffff));
	const int fa = (rei_rdw(ds + 0xc) & 0x100) != 0, fb = (rei_rdw(BPW(0xc)) & 0x100) != 0;
	if (fa) {
		const UINT32 t = (0x10000 - ax) & 0xffff;
		ax = (0x10000 - dx) & 0xffff;
		dx = t;
	}
	if (!((INT16)ax < (INT16)dx))
		return -1; // (int3)
	UINT32 bx = rei_rdw(eb + di), cx = rei_rdw(eb + ((di + 2) & 0xffff));
	if (fb) {
		const UINT32 t = (0x10000 - bx) & 0xffff;
		bx = (0x10000 - cx) & 0xffff;
		cx = t;
	}
	if (!((INT16)bx < (INT16)cx))
		return -1;
	const UINT32 x0 = rei_rdw(ds + 0x26), x1 = rei_rdw(BPW(0x26));
	ax = (ax + x0) & 0xffff;
	dx = (dx + x0) & 0xffff;
	bx = (bx + x1) & 0xffff;
	cx = (cx + x1) & 0xffff;
	int path;
	if ((INT16)bx > (INT16)dx || !((INT16)ax <= (INT16)cx)) { // apart
		path = (INT16)bx > (INT16)dx ? 0 : 1;
		UINT32 dst = path ? ax : bx, src = path ? cx : dx;
		SUBW;
		(void)dst;
		nec_state->CarryVal = 0; // clc
	} else {
		path = 2;
		if (ax < bx) {
			ax = bx;
			path += 1;
		}
		if (!(dx < cx)) {
			dx = cx;
			path += 2;
		}
		const UINT32 mid = (UINT32)(UINT16)((INT16)(UINT16)(dx + ax) >> 1);
		const UINT32 w = BPW(0x5c);
		// later reads that would see the write: interpreter
		const UINT32 rd[6] = { ds + 0x1c, BPW(0x1c), ea + ((si + 4) & 0xffff), ea + ((si + 6) & 0xffff), eb + ((di + 4) & 0xffff),
				       eb + ((di + 6) & 0xffff) };
		for (int k = 0; k < 6; k++)
			if (rd[k] < w + 2 && rd[k] + 2 > w)
				return -1;
		if ((ds + 0x28 < w + 2 && ds + 0x2a > w) || (BPW(0x28) < w + 2 && BPW(0x28) + 2 > w) ||
		    (ss + sp < w + 2 && ss + sp + 4 > w))
			return -1;
		const UINT32 ea2 = (UINT32)rei_rdw(ds + 0x1c) << 4;
		if (ea2 != ea)
			return -1;
		ax = rei_rdw(ea + ((si + 4) & 0xffff));
		dx = rei_rdw(ea + ((si + 6) & 0xffff));
		if (!((INT16)ax < (INT16)dx))
			return -1;
		bx = rei_rdw(eb + ((di + 4) & 0xffff));
		cx = rei_rdw(eb + ((di + 6) & 0xffff));
		if (!((INT16)bx < (INT16)cx))
			return -1;
		if (wr)
			write_mem_word(w, (UINT16)mid);
		ax = (0x10000 - ((ax - dx) & 0xffff)) & 0xffff;
		cx = (cx - bx) & 0xffff;
		ax = (ax + cx) & 0xffff;
		dx = (dx + rei_rdw(ds + 0x28)) & 0xffff;
		bx = (bx + rei_rdw(BPW(0x28))) & 0xffff;
		dx = (dx - bx) & 0xffff;
		{
			UINT32 dst = dx, src = ax;
			SUBW;
			(void)dst;
		}
	}
#undef BPW
	Sreg(DS1) = (WORD)(eb >> 4);
	Wreg(AW) = (WORD)ax;
	Wreg(BW) = (WORD)bx;
	Wreg(CW) = (WORD)cx;
	Wreg(DW) = (WORD)dx;
	nec_state->ip = rei_rdw(ss + sp); // lret
	Sreg(PS) = rei_rdw(ss + ((sp + 2) & 0xffff));
	Wreg(SP) = (WORD)(sp + 4);
	return fa | fb << 1 | path << 2 | (int)(si & 1) << 5 | (int)(di & 1) << 6 | (int)(bp & 1) << 7 | (int)(sp & 1) << 8;
}
static struct rei_fph rei_box_fp REI_FAST_DATA;

#define REI_PHIT_PC 0x2180b
static REI_FAST_FN int rei_phit_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, es = Sreg(DS1) << 4, sp = Wreg(SP), bp = Wreg(BP);
	const UINT32 si = Wreg(IX), dx0 = Wreg(DW);
	if (sp < 2 || sp > 0xfffe || si > 0xfff3)
		return -1;
	UINT32 idx;
	if (bp == 0x4480) idx = 0; else if (bp == 0x4540) idx = 1; else if (bp == 0x4600) idx = 2; else if (bp == 0x46c0) idx = 3;
	else return -1;
	const UINT32 o = ss + bp, st = ss + sp - 2, box = es + si;
	if ((o < st + 2 && o + 0x7c > st) || (box < st + 2 && box + 0xc > st))
		return -1; // (reads that would see push dx)
	UINT32 sig = 1 << 2 | idx;
	UINT32 ax = Wreg(AW), bx = Wreg(BW), cx = Wreg(CW), di = Wreg(IY), dx = dx0;
	int hit = -1; // -1 none, else 0x10 / 0x20
	const int skip = rei_rdw(o) == 0x2953 && rei_rdw(o + 2) == 0;
	sig = sig << 2 | (rei_rdw(o) == 0x2953 ? 2u : 0u) | (UINT32)skip;
	if (!skip) {
		ax = (rei_rdw(o + 0x26) - rei_rdw(ds + 0x26)) & 0xffff;
		cx = (rei_rdw(o + 0x28) - rei_rdw(ds + 0x28)) & 0xffff;
		di = (rei_rdw(o + 0x30) - rei_rdw(ds + 0x30)) & 0xffff;
		UINT32 *r[3] = { &ax, &cx, &di };
		int in = 1;
		for (int k = 0; k < 3 && in; k++) {
			bx = rei_rdw(box + 4 * k);
			dx = (rei_rdw(box + 4 * k + 2) - bx) & 0xffff;
			*r[k] = (*r[k] - bx) & 0xffff;
			in = *r[k] < dx;
			sig = sig << 1 | (UINT32)in;
		}
		if (in) {
			const int nt = (rei_rdw(o + 0x4c) & 0x40) == 0;
			sig = sig << 1 | (UINT32)nt;
			if (nt) {
				const int b = cpu_readmem20(ds + 0x7e) == 3;
				sig = sig << 1 | (UINT32)b;
				hit = b ? 0x20 : 0x10;
			}
		}
	}
	if (wr) {
		write_mem_word(st, (UINT16)dx0); // push dx
		if (hit >= 0) {
			write_mem_word(o + 0x4a, (UINT16)(rei_rdw(o + 0x4a) | hit));
			write_mem_word(o + (hit == 0x20 ? 0x7a : 0x78), Sreg(DS0));
		}
	}
	Wreg(AW) = (WORD)ax;
	Wreg(BW) = (WORD)bx;
	Wreg(CW) = (WORD)cx;
	Wreg(IY) = (WORD)di;
	{ UINT32 dst = bp, src = 0xc0, res = dst + src; // add bp,c0 (C stays through dec dx)
	  SetCFW(res); SetOFW_Add(res, src, dst); SetAF(res, src, dst); SetSZPF_Word(res); Wreg(BP) = (WORD)res; }
	{ const UINT32 t1 = (dx0 - 1) & 0xffff;          // pop dx / dec dx
	  nec_state->OverVal = (dx0 == 0x8000); SetAF(t1, dx0, 1); SetSZPF_Word(t1); Wreg(DW) = (WORD)t1; }
	if (Wreg(DW) == 0)
		nec_state->ip = (WORD)(nec_state->ip + (0x218a3 - REI_PHIT_PC));
	return (int)((sig << 1 | (sp & 1)) << 1 | (si & 1));
}
static struct rei_fph rei_phit_fp REI_FAST_DATA;

// 0x3dc05 / 0x3dc71: an object's move (stage 6 busy scenes ~8% of the V33 instructions), around its map check 0:fb10
//  M1 (0x3dc05 -> 0xfb10): mov bp,7f6 / mov ax,[bp+1c] / sub [26],ax / mov ax,[bp+1e] / sub [28],ax / mov ax,[64] /
//   add ax,[2c] / mov cx,[66] / add cx,[2e] / test word [4c],1 / je / xor ax,ax / test word [4c],2 / je / xor cx,cx /
//   test byte [6a],ff / je B / mov al,ah / cbw / mov si,ax / mov al,ch / cbw / mov di,ax / call 0:fb10 (-> 3dc4f) |
//   B: cwd / mov dl,ah / add [2a],al / adc dx,0 / mov si,dx / mov ax,cx / cwd / mov dl,ah / add [2b],al / adc dx,0 /
//   mov di,dx / call 0:fb10 (-> 3dc71)
//  M2 (0x3dc71 -> retf): mov [26],ax / mov [28],cx / for [64], [66]: mov ax,[v] / test ax,ax / je / sar ax,3 / or ax,1 /
//   sub [v],ax | mov ax,[28] / mov [18],ax / test word [4c],1 / je E / test byte [56],ff / je E / mov dx,[58] /
//   cmp dx,ffff / je E / mov es,dx / mov dx,[64] / neg dx / mov es:[64],dx | E: pop [2e] / pop [2c] / retf
#define REI_MOV1_PC 0x3dc05
#define REI_MOV2_PC 0x3dc71
static REI_FAST_FN int rei_mov1_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP), ps = Sreg(PS), ip = nec_state->ip;
	if (sp < 4 || (ss + sp - 4 < ds + 0x70 && ss + sp > ds) || (ss + 0x812 < ds + 0x70 && ss + 0x816 > ds))
		return -1; // (stack or ss:[812..815] inside the object fields)
	const UINT32 d26 = (rei_rdw(ds + 0x26) - rei_rdw(ss + 0x812)) & 0xffff;
	const UINT32 d28 = (rei_rdw(ds + 0x28) - rei_rdw(ss + 0x814)) & 0xffff;
	const UINT32 v66 = rei_rdw(ds + 0x66), v2e = rei_rdw(ds + 0x2e), f4c = rei_rdw(ds + 0x4c);
	UINT32 ax = (rei_rdw(ds + 0x64) + rei_rdw(ds + 0x2c)) & 0xffff, cx = (v66 + v2e) & 0xffff;
	UINT32 af = (cx ^ v2e ^ v66) & 0x10; // add cx,[2e] (the tests after it keep A, an xor clears it)
	UINT32 sig = 1;
	sig = sig << 1 | (f4c & 1);
	if (f4c & 1) { ax = 0; af = 0; }
	sig = sig << 1 | ((f4c >> 1) & 1);
	if (f4c & 2) { cx = 0; af = 0; }
	const UINT32 f6a = cpu_readmem20(ds + 0x6a);
	sig = sig << 1 | (f6a != 0);
	UINT32 si, di, dx = Wreg(DW), ret, w2a = 0, w2b = 0;
	if (f6a) { // A
		si = (UINT32)(INT32)(INT8)(ax >> 8) & 0xffff;
		ax = si;                                        // (mov al,ah / cbw leaves ax = si)
		di = (UINT32)(INT32)(INT8)(cx >> 8) & 0xffff;
		ax = di;
		nec_state->CarryVal = nec_state->OverVal = 0;   // test byte [6a],ff
		nec_state->AuxVal = af;
		SetSZPF_Byte(f6a);
		ret = 0x3dc4f;
	} else {   // B
		UINT32 d = ((ax & 0x8000) ? 0xff00 : 0) | (ax >> 8);
		UINT32 s = cpu_readmem20(ds + 0x2a) + (ax & 0xff);
		w2a = s & 0xff;
		UINT32 res = d + (s >> 8);                      // adc dx,0
		si = res & 0xffff;
		ax = cx;
		d = ((ax & 0x8000) ? 0xff00 : 0) | (ax >> 8);
		s = cpu_readmem20(ds + 0x2b) + (ax & 0xff);
		w2b = s & 0xff;
		const UINT32 c = s >> 8;
		res = d + c;
		SetCFW(res); SetOFW_Add(res, c, d); SetAF(res, c, d); SetSZPF_Word(res);
		dx = di = res & 0xffff;
		ret = 0x3dc71;
	}
	if (wr) {
		write_mem_word(ds + 0x26, d26);
		write_mem_word(ds + 0x28, d28);
		if (!f6a) {
			cpu_writemem20(ds + 0x2a, w2a);
			cpu_writemem20(ds + 0x2b, w2b);
		}
		write_mem_word(ss + sp - 2, ps);                    // call 0:fb10
		write_mem_word(ss + sp - 4, (ip + (ret - REI_MOV1_PC)) & 0xffff);
	}
	Wreg(AW) = (WORD)ax;
	Wreg(CW) = (WORD)cx;
	Wreg(DW) = (WORD)dx;
	Wreg(BP) = 0x7f6;
	Wreg(IX) = (WORD)si;
	Wreg(IY) = (WORD)di;
	nec_state->ip = 0xfb10;
	Sreg(PS) = 0;
	Wreg(SP) = (WORD)(sp - 4);
	return (int)(sig << 1 | (sp & 1));
}
static REI_FAST_FN int rei_mov2_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP);
	if (sp > 0xfff7 || (ss + sp < ds + 0x70 && ss + sp + 8 > ds))
		return -1;
	const UINT32 ax0 = Wreg(AW), cx0 = Wreg(CW);
	UINT32 sig = 1, dx = Wreg(DW), es = Sreg(DS1), af = 0, w64 = 0, w66 = 0, wes = 0;
	UINT32 v[2] = { rei_rdw(ds + 0x64), rei_rdw(ds + 0x66) }, nv[2];
	for (int k = 0; k < 2; k++) {
		const UINT32 a = v[k];
		sig = sig << 1 | (a != 0);
		nv[k] = a;
		af = 0;                                         // test ax,ax
		if (a) {
			const UINT32 t = ((UINT32)((INT32)(INT16)a >> 3) | 1) & 0xffff; // sar ax,3 / or ax,1
			const UINT32 res = a - t;                    // sub [v],ax
			af = (res ^ t ^ a) & 0x10;
			nv[k] = res & 0xffff;
			if (k) w66 = 1; else w64 = 1;
		}
	}
	// flags after the two blocks: the last test or sub; the code below only reads S/Z/P from the tests that follow
	const UINT32 f4c = rei_rdw(ds + 0x4c);
	int end;
	sig = sig << 1 | (f4c & 1);
	nec_state->CarryVal = nec_state->OverVal = 0;       // test word [4c],1
	nec_state->AuxVal = af;
	SetSZPF_Word(f4c & 1);
	end = !(f4c & 1);
	if (!end) {
		const UINT32 b56 = cpu_readmem20(ds + 0x56);
		sig = sig << 1 | (b56 != 0);
		SetSZPF_Byte(b56);                              // test byte [56],ff (keeps A)
		end = !b56;
		if (!end) {
			dx = rei_rdw(ds + 0x58);
			const UINT32 res = dx - 0xffff;             // cmp dx,ffff
			SetCFW(res); SetOFW_Sub(res, 0xffff, dx); SetAF(res, 0xffff, dx); SetSZPF_Word(res);
			sig = sig << 1 | (dx == 0xffff);
			end = dx == 0xffff;
			if (!end) {
				es = dx;
				const UINT32 n = nv[0];
				nec_state->CarryVal = n != 0;           // neg dx (O, A stay)
				dx = (~n + 1) & 0xffff;
				SetSZPF_Word(dx);
				wes = 1;
			}
		}
	}
	if (wes && (((es << 4) + 0x64 < ds + 0x70 && (es << 4) + 0x66 > ds) || ((es << 4) + 0x64 < ss + sp + 8 && (es << 4) + 0x66 > ss + sp)))
		return -1; // (es:[64] inside this object or the stack read by the pops)
	if (wr) {
		write_mem_word(ds + 0x26, ax0);
		write_mem_word(ds + 0x28, cx0);
		if (w64) write_mem_word(ds + 0x64, nv[0]);
		if (w66) write_mem_word(ds + 0x66, nv[1]);
		write_mem_word(ds + 0x18, cx0);
		if (wes) write_mem_word((es << 4) + 0x64, dx);
		write_mem_word(ds + 0x2e, rei_rdw(ss + sp));         // pop [2e]
		write_mem_word(ds + 0x2c, rei_rdw(ss + sp + 2));     // pop [2c]
	}
	Wreg(AW) = (WORD)cx0;                               // mov ax,[28] (= cx just stored)
	Wreg(DW) = (WORD)dx;
	Sreg(DS1) = (WORD)es;
	nec_state->ip = rei_rdw(ss + sp + 4);               // retf
	Sreg(PS) = rei_rdw(ss + sp + 6);
	Wreg(SP) = (WORD)(sp + 8);
	return (int)(sig << 1 | (sp & 1));
}
static struct rei_fph rei_mov_fp REI_FAST_DATA;

// 0x3d695 (an object task of stage 1: thrown/bouncing objects, ~quarter of the V33 instructions at 2160..2400 there):
// up to the far call of 0x39c3 (own fast path) at 0x3d71b.
//   test word [a0],ffff / je P / mov ax,[a2] / xor ax,ss:[7aa] / shr ax,1 / sbb ax,ax / and ax,1000 / or [4c],ax |
//   P: mov ax,[2c] / mov cx,[2e] / call 0:3758 / call 3000:db95 / call 3000:de08 / jae Q / (landing: interpreter) |
//   Q: mov ax,[26] / mov cx,[2c] / add cx,[64] / cmp ax,70 / ja R / test cx,cx / js N | R: cmp ax,1b0 / jb E /
//   test cx,cx / js E | N: neg word [2c] / neg word [64] | E: mov ax,[28] / mov [18],ax
//   3758: cwd / mov dl,ah / add [2a],al / adc [26],dx / mov ax,cx / cwd / mov dl,ah / add [2b],al / adc [28],dx / retf
//   db95: mov bp,7f6 / mov ax,[bp+1c] / sub [26],ax / mov ax,[bp+1e] / sub [28],ax / retf
//   de08: test byte [6a],ff / jne C / mov ax,[32] / cwd / mov dl,ah / add [34],al / adc [30],dx / mov ax,[68] /
//   add ax,[32] / jns S / mov dx,[4e] / cmp dx,[30] / jl S / (mov [30],dx / stc: landing) | S: mov [32],ax | C: clc / retf
#define REI_OBJ_PC 0x3d695
static int rei_obj_code; // the called routines (0x3758, 0x3db95, 0x3de08)
static REI_FAST_FN int rei_obj_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP), ps = Sreg(PS), ip = nec_state->ip;
	if (sp < 4 || (ss + sp - 4 < ds + 0xa4 && ss + sp > ds + 0x18) || (ss + 0x7aa < ds + 0xa4 && ss + 0x816 > ds + 0x18) ||
	    (sp - 4 < 0x816 && sp > 0x812))
		return -1; // (the pushed return addresses or ss:[7aa], ss:[812..815] inside the object fields)
	UINT32 sig = 1, cf, of, af = nec_state->AuxVal, szp, byte = 0;
	// test word [a0],ffff (keeps A)
	const UINT32 a0 = rei_rdw(ds + 0xa0);
	cf = of = 0;
	szp = a0;
	sig = sig << 1 | (a0 != 0);
	UINT32 w4c = 0, ax, cx, dx;
	if (a0) {
		ax = rei_rdw(ds + 0xa2) ^ rei_rdw(ss + 0x7aa);   // xor (C, O, A clear)
		const UINT32 src = ax;
		cf = ax & 1;                                     // shr ax,1
		ax >>= 1;
		of = (src ^ ax) & 0x8000;
		const UINT32 s2 = ax + (cf != 0), res = ax - s2;  // sbb ax,ax
		ax = res & 0xffff;
		ax &= 0x1000;                                    // and (C, O, A clear)
		w4c = rei_rdw(ds + 0x4c) | ax;                   // or [4c],ax
		cf = of = af = 0;
		szp = w4c;
	}
	// 3758: [2a]:[26] += [2c] (8.8), [2b]:[28] += [2e]
	const UINT32 v2c = rei_rdw(ds + 0x2c), v2e = rei_rdw(ds + 0x2e);
	UINT32 b2a, b2b, w26, w28, res, s, d;
	{
		ax = v2c;
		dx = ((ax & 0x8000) ? 0xff00 : 0) | (ax >> 8);
		d = cpu_readmem20(ds + 0x2a);
		res = d + (ax & 0xff);
		b2a = res & 0xff;
		const UINT32 c = (res & 0x100) != 0;
		d = rei_rdw(ds + 0x26);
		s = dx + c;
		res = d + s;
		w26 = res & 0xffff;
		ax = v2e;
		dx = ((ax & 0x8000) ? 0xff00 : 0) | (ax >> 8);
		d = cpu_readmem20(ds + 0x2b);
		res = d + (ax & 0xff);
		b2b = res & 0xff;
		const UINT32 c2 = (res & 0x100) != 0;
		d = rei_rdw(ds + 0x28);
		s = dx + c2;
		res = d + s;
		w28 = res & 0xffff;
	}
	// db95: [26] -= ss:[812], [28] -= ss:[814]
	ax = rei_rdw(ss + 0x812);
	w26 = (w26 - ax) & 0xffff;
	ax = rei_rdw(ss + 0x814);
	d = w28;
	res = d - ax;
	w28 = res & 0xffff;
	cf = res & 0x10000; of = (d ^ ax) & (d ^ res) & 0x8000; af = (res ^ ax ^ d) & 0x10; szp = res; byte = 0; // (only A survives)
	// de08
	const UINT32 b6a = cpu_readmem20(ds + 0x6a);
	sig = sig << 1 | (b6a != 0);
	cf = of = 0;                                         // test byte [6a],ff (keeps A)
	szp = b6a;
	byte = 1;
	UINT32 w34 = 0, w30 = 0, w32 = 0, wf = 0;
	if (!b6a) {
		const UINT32 v32 = rei_rdw(ds + 0x32);
		ax = v32;
		dx = ((ax & 0x8000) ? 0xff00 : 0) | (ax >> 8);
		d = cpu_readmem20(ds + 0x34);
		res = d + (ax & 0xff);
		w34 = res & 0xff;
		const UINT32 c = (res & 0x100) != 0;
		d = rei_rdw(ds + 0x30);
		s = dx + c;
		res = d + s;
		w30 = res & 0xffff;
		d = rei_rdw(ds + 0x68);                          // mov ax,[68] / add ax,[32]
		res = d + v32;
		cf = res & 0x10000; of = (res ^ v32) & (res ^ d) & 0x8000; af = (res ^ v32 ^ d) & 0x10; szp = res; byte = 0;
		ax = res & 0xffff;
		const int neg = (ax & 0x8000) != 0;
		sig = sig << 1 | (UINT32)neg;
		if (neg) {
			dx = rei_rdw(ds + 0x4e);                     // cmp dx,[30]
			res = dx - w30;
			cf = res & 0x10000; of = (w30 ^ dx) & (dx ^ res) & 0x8000; af = (res ^ w30 ^ dx) & 0x10; szp = res;
			if (!(((res & 0x8000) != 0) != (of != 0)))
				return -1;                               // (not jl: landing, the interpreter runs it)
		}
		w32 = ax;
		wf = 1;
	}
	cf = 0;                                              // clc
	// Q
	ax = w26;
	{
		const UINT32 v64 = rei_rdw(ds + 0x64);
		res = v2c + v64;                                 // add cx,[64]
		cx = res & 0xffff;
		res = ax - 0x70;                                 // cmp ax,70
		cf = res & 0x10000; of = (0x70 ^ ax) & (ax ^ res) & 0x8000; af = (res ^ 0x70 ^ ax) & 0x10; szp = res; byte = 0;
		const int ja = ax > 0x70;
		sig = sig << 1 | (UINT32)ja;
		int bounce = 0, test = 0;
		if (!ja) {
			test = 1;
			sig = sig << 1 | (cx >> 15);
			if (cx & 0x8000)
				bounce = 1;
		}
		if (!bounce && (ja || !(cx & 0x8000))) {
			res = ax - 0x1b0;                            // cmp ax,1b0
			cf = res & 0x10000; of = (0x1b0 ^ ax) & (ax ^ res) & 0x8000; af = (res ^ 0x1b0 ^ ax) & 0x10; szp = res;
			test = 0;
			const int jb = ax < 0x1b0;
			sig = sig << 1 | (UINT32)jb;
			if (!jb) {
				test = 1;
				sig = sig << 1 | (cx >> 15);
				bounce = !(cx & 0x8000);
			}
		}
		if (test) { cf = of = af = 0; szp = cx; }        // test cx,cx
		UINT32 n2c = v2c, n64 = v64;
		if (bounce) {
			n2c = (~v2c + 1) & 0xffff;                   // neg word [2c] / neg word [64] (C and S, Z, P only)
			n64 = (~v64 + 1) & 0xffff;
			cf = v64 != 0;
			szp = n64;
		}
		if (wr) {
			if (a0) write_mem_word(ds + 0x4c, w4c);
			cpu_writemem20(ds + 0x2a, b2a);
			cpu_writemem20(ds + 0x2b, b2b);
			write_mem_word(ds + 0x26, w26);
			write_mem_word(ds + 0x28, w28);
			if (wf) {
				cpu_writemem20(ds + 0x34, w34);
				write_mem_word(ds + 0x30, w30);
				write_mem_word(ds + 0x32, w32);
			}
			if (bounce) {
				write_mem_word(ds + 0x2c, n2c);
				write_mem_word(ds + 0x64, n64);
			}
			write_mem_word(ds + 0x18, w28);
			write_mem_word(ss + sp - 2, ps);              // (the last of the three far calls: 3000:de08)
			write_mem_word(ss + sp - 4, (ip + (0x3d6c6 - REI_OBJ_PC)) & 0xffff);
		}
	}
	Wreg(AW) = (WORD)w28;
	Wreg(CW) = (WORD)cx;
	Wreg(DW) = (WORD)dx;
	Wreg(BP) = 0x7f6;
	nec_state->CarryVal = cf;
	nec_state->OverVal = of;
	nec_state->AuxVal = af;
	if (byte)
		SetSZPF_Byte(szp);
	else
		SetSZPF_Word(szp);
	nec_state->ip = (WORD)(ip + (0x3d71b - REI_OBJ_PC));
	return (int)(sig << 1 | (sp & 1));
}
static struct rei_fph rei_obj_fp REI_FAST_DATA;

// 0x3df04 / 0x3df8e: an object's cell in the ss:[2631] occupancy grid (called at the start of the move, ~9 a frame),
// around the cell computation 0:151a (own fast path)
//  P1 (0x3df04 -> 0x151a or ret): test word [4c],8 / jne R / mov ax,[30] / sub ax,[4e] / cmp ax,18 / jae R /
//   test word [c],1 / jne G | R: ret | G: mov ax,[26] / sub ax,60 / cmp ax,140 / jae R / mov ax,[28] / sub ax,88 /
//   cmp ax,f0 / jae R / mov ax,[2e] / cwd / mov dl,ah / add al,[2b] / adc dx,[28] / mov cx,dx / mov ax,[2c] / cwd /
//   mov dl,ah / add al,[2a] / adc dx,[26] / mov ax,dx / call 3df86 | 3df86: mov bp,7f6 / call 0:151a
//  P2 (0x3df8e -> ret): and bx,3fff / shr bh,1 / shr bx,3 / shl bx,1 / add bx,2631 / mov ax,ds / shr ax,2 / mov ah,al /
//   mov al,ss:[7aa] / ret (-> 3df5a) / cmp word [62],ffff / jne W / call 3dfa9 / jae W / (...) |
//   3dfa9: mov cx,ss:[bx] / sub cl,al / neg cl / cmp cl,2 / jae C / cmp ah,ch / je C / stc / ret | C: clc / ret |
//   W: mov ss:[bx],ax / ret
#define REI_GRID1_PC 0x3df04
#define REI_GRID2_PC 0x3df8e
static REI_FAST_FN int rei_grid1_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP), ps = Sreg(PS), ip = nec_state->ip;
	if (sp < 6 || sp > 0xfffd || (ss + sp - 6 < ds + 0x64 && ss + sp > ds))
		return -1;
	UINT32 sig = 1, ax = Wreg(AW), cx = Wreg(CW), dx = Wreg(DW);
	const UINT32 f4c = rei_rdw(ds + 0x4c);
	int full = 0;
	sig = sig << 1 | ((f4c >> 3) & 1);
	if (f4c & 8) {
		nec_state->CarryVal = nec_state->OverVal = 0; SetSZPF_Word(f4c & 8); // test (A stays)
	} else {
		const UINT32 a = rei_rdw(ds + 0x30), b = rei_rdw(ds + 0x4e), d = (a - b) & 0xffff, res = d - 0x18;
		ax = d;
		SetCFW(res); SetOFW_Sub(res, 0x18, d); SetAF(res, 0x18, d); SetSZPF_Word(res);
		sig = sig << 1 | (d >= 0x18);
		if (d < 0x18) {
			const UINT32 c = rei_rdw(ds + 0xc);
			sig = sig << 1 | (c & 1);
			if (!(c & 1)) {
				nec_state->CarryVal = nec_state->OverVal = 0; SetSZPF_Word(0); // test word [c],1 (A stays)
			} else {
				const UINT32 x = (rei_rdw(ds + 0x26) - 0x60) & 0xffff, rx = x - 0x140;
				ax = x;
				SetCFW(rx); SetOFW_Sub(rx, 0x140, x); SetAF(rx, 0x140, x); SetSZPF_Word(rx);
				sig = sig << 1 | (x >= 0x140);
				if (x < 0x140) {
					const UINT32 y = (rei_rdw(ds + 0x28) - 0x88) & 0xffff, ry = y - 0xf0;
					ax = y;
					SetCFW(ry); SetOFW_Sub(ry, 0xf0, y); SetAF(ry, 0xf0, y); SetSZPF_Word(ry);
					sig = sig << 1 | (y >= 0xf0);
					if (y < 0xf0) {
						full = 1;
						UINT32 a2 = rei_rdw(ds + 0x2e);
						UINT32 d2 = ((a2 & 0x8000) ? 0xff00 : 0) | (a2 >> 8);
						UINT32 c2 = ((a2 & 0xff) + cpu_readmem20(ds + 0x2b)) >> 8;
						cx = (d2 + rei_rdw(ds + 0x28) + c2) & 0xffff;
						a2 = rei_rdw(ds + 0x2c);
						d2 = ((a2 & 0x8000) ? 0xff00 : 0) | (a2 >> 8);
						c2 = ((a2 & 0xff) + cpu_readmem20(ds + 0x2a)) >> 8;
						const UINT32 src = rei_rdw(ds + 0x26) + c2, res = d2 + src; // adc dx,[26]
						SetCFW(res); SetOFW_Add(res, src, d2); SetAF(res, src, d2); SetSZPF_Word(res);
						ax = dx = res & 0xffff;
					}
				}
			}
		}
	}
	Wreg(AW) = (WORD)ax;
	Wreg(CW) = (WORD)cx;
	Wreg(DW) = (WORD)dx;
	if (full) {
		if (wr) {
			write_mem_word(ss + sp - 2, (UINT16)(ip + (0x3df5a - REI_GRID1_PC))); // call 3df86
			write_mem_word(ss + sp - 4, (UINT16)ps);                             // call 0:151a
			write_mem_word(ss + sp - 6, (UINT16)(ip + (0x3df8e - REI_GRID1_PC)));
		}
		Wreg(BP) = 0x7f6;
		nec_state->ip = 0x151a;
		Sreg(PS) = 0;
		Wreg(SP) = (WORD)(sp - 6);
	} else {
		nec_state->ip = rei_rdw(ss + sp);                                         // ret
		Wreg(SP) = (WORD)(sp + 2);
	}
	return (int)(sig << 1 | (sp & 1));
}
static REI_FAST_FN int rei_grid2_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP), ps = Sreg(PS);
	if (sp > 0xfffb || ((ps << 4) + rei_rdw(ss + sp)) != 0x3df5a)
		return -1; // (3df86 called from the other place)
	UINT32 b = Wreg(BW) & 0x3fff;
	b = ((b >> 9) << 8) | (b & 0xff);                    // shr bh,1
	b = ((b >> 3) << 1) + 0x2631;
	b &= 0xffff;
	const UINT32 dsv = Sreg(DS0) >> 2;
	const UINT32 ax = ((dsv & 0xff) << 8) | cpu_readmem20(ss + 0x7aa);
	UINT32 cx = Wreg(CW), sig = 1;
	const UINT32 f62 = rei_rdw(ds + 0x62);
	sig = sig << 1 | (f62 == 0xffff);
	if (f62 != 0xffff) {
		const UINT32 res = f62 - 0xffff;                 // cmp word [62],ffff
		SetCFW(res); SetOFW_Sub(res, 0xffff, f62); SetAF(res, 0xffff, f62); SetSZPF_Word(res);
	} else {
		if (b > 0xfffd || (ss + b < ss + sp + 4 && ss + b + 2 > ss + sp))
			return -1; // (the cell word overlaps the stack the calls use)
		cx = rei_rdw(ss + b);
		const UINT32 cl = (0 - (((cx & 0xff) - (ax & 0xff)) & 0xff)) & 0xff, r1 = cl - 2;
		SetCFB(r1); SetOFB_Sub(r1, 2, cl); SetAF(r1, 2, cl); SetSZPF_Byte(r1);
		cx = (cx & 0xff00) | cl;
		sig = sig << 1 | (cl >= 2);
		if (cl < 2) {
			const UINT32 ah = ax >> 8, ch = cx >> 8, r2 = ah - ch;
			SetCFB(r2); SetOFB_Sub(r2, ch, ah); SetAF(r2, ch, ah); SetSZPF_Byte(r2);
			sig = sig << 1 | (ah == ch);
			if (ah != ch)
				return -1; // (stc: the long path)
		}
		nec_state->CarryVal = 0;                         // clc
	}
	if (b > 0xfffd)
		return -1;
	if (wr) {
		if (f62 == 0xffff)
			write_mem_word(ss + sp, (UINT16)(rei_rdw(ss + sp) + (0x3df64 - 0x3df5a))); // call 3dfa9 (return 3df64)
		write_mem_word(ss + b, (UINT16)ax);              // mov ss:[bx],ax
	}
	Wreg(AW) = (WORD)ax;
	Wreg(BW) = (WORD)b;
	Wreg(CW) = (WORD)cx;
	nec_state->ip = rei_rdw(ss + sp + 2);                // ret (to the caller of 3df04)
	Wreg(SP) = (WORD)(sp + 4);
	return (int)(sig << 2 | (sp & 1) << 1 | (b & 1));
}
static struct rei_fph rei_grid_fp REI_FAST_DATA;

// 0x657d0 (far called, ~10 a frame in busy play): an object's attack check, when the object has no attack box ([62] = ffff)
//   push ax / call 6000:56f5 / test word ss:[3209],ffff / pop ax / je E / ... | E: mov ax,ss:[3207] / retf
//   56f5: xor ax,ax / mov ss:[3207],ax / mov ss:[3209],ax / mov es,[1c] / mov si,[62] / cmp si,ffff / jne (the check) /
//   xor ax,ax / retf
#define REI_ATK_PC 0x657d0
static int rei_atk_code;
static REI_FAST_FN int rei_atk_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, ds = Sreg(DS0) << 4, sp = Wreg(SP), ps = Sreg(PS), ip = nec_state->ip;
	if (sp < 6 || sp > 0xfffb || (ss + 0x3207 < ss + sp + 4 && ss + 0x320b > ss + sp - 6) ||
	    (ds + 0x1c < ss + sp && ds + 0x64 > ss + sp - 6))
		return -1; // (the words written overlap the stack, or the object fields read after the pushes)
	if (rei_rdw(ds + 0x62) != 0xffff)
		return -1; // (the check itself)
	if (wr) {
		write_mem_word(ss + sp - 2, Wreg(AW));                              // push ax
		write_mem_word(ss + sp - 4, (UINT16)ps);                            // call 6000:56f5
		write_mem_word(ss + sp - 6, (UINT16)(ip + (0x657d6 - REI_ATK_PC)));
		write_mem_word(ss + 0x3207, 0);
		write_mem_word(ss + 0x3209, 0);
	}
	Sreg(DS1) = rei_rdw(ds + 0x1c);
	Wreg(IX) = 0xffff;
	Wreg(AW) = 0;
	nec_state->CarryVal = nec_state->OverVal = nec_state->AuxVal = 0; // test word ss:[3209],ffff (= 0; A from xor)
	SetSZPF_Word(0);
	nec_state->ip = rei_rdw(ss + sp);                                   // retf
	Sreg(PS) = rei_rdw(ss + sp + 2);
	Wreg(SP) = (WORD)(sp + 4);
	return (int)(sp & 1);
}
static struct rei_fp rei_atk_fp REI_FAST_DATA;

// 0x3c967 (score ranking: one character, ~45% of the V33 there): character code -> sprite, put it if on screen.
//   call c995 / call c96e / ret
//   c995: shl si,1 / add si,e783 / mov dx,3000 / mov es,dx / mov si,es:[si] / ret
//   c96e: push ax / add ax,8 / mov dx,ax / add dx,[26] / cmp dx,48 / jb X / cmp dx,1b8 / ja X / mov cx,fff8 /
//   and word [c],bfff / xor di,di / call 0:395e | X: pop ax / ret
//   395e: mov bl,[21] / cmp bl,4 / jb / (int3) / xor bh,bh / shl bx,1 / mov dx,[bx+10] / and dx,7f / mov bx,[c] /
//   mov [e],bx / and bx,780 / or dx,bx / mov bx,[18] / add ax,[26] / add cx,[28] / add cx,[30] / test word [c],4000 /
//   je (jmp 0:23a3, always: bit 14 was just cleared) / retf
// On screen the piece ends at 0x23a3 (its own fast path), off screen back in the caller. Checked by hashes.
#define REI_RCHR_PC 0x3c967
static int rei_rchr_code; // 0 unchecked, 1 matches, -1 different
static REI_FAST_FN int rei_rchr_apply(nec_state_t *nec_state, int sub, int wr)
{
	(void)sub;
	const UINT32 ss = Sreg(SS_) << 4, sp = Wreg(SP), ds = Sreg(DS0) << 4, ps = Sreg(PS), ip = nec_state->ip;
	const UINT32 ax0 = Wreg(AW);
	if (sp < 8 || sp > 0xfffd || (ss + sp - 8 < ds + 0x32 && ss + sp + 2 > ds))
		return -1; // (the stack inside the object)
	const UINT32 ta = 0x30000 + (((Wreg(IX) << 1) + 0xe783) & 0xffff); // es:[si] after shl / add
	if (ta + 2 > ss + sp - 8 && ta < ss + sp + 2)
		return -1;
	const UINT32 si = rei_rdw(ta);
	const UINT32 ax = (ax0 + 8) & 0xffff, dx = (ax + rei_rdw(ds + 0x26)) & 0xffff;
	int id;
	if (dx < 0x48 || dx > 0x1b8) { // off screen: pop ax / ret / ret
		const UINT32 src = dx < 0x48 ? 0x48 : 0x1b8, res = dx - src;
		id = dx < 0x48 ? 1 : 2;
		if (wr) {
			write_mem_word(ss + sp - 2, (UINT16)(ip + 3));
			write_mem_word(ss + sp - 2, (UINT16)(ip + 6));
			write_mem_word(ss + sp - 4, (UINT16)ax0);
		}
		SetCFW(res); SetOFW_Sub(res, src, dx); SetAF(res, src, dx); SetSZPF_Word(res);
		Wreg(AW) = (WORD)ax0;
		Wreg(DW) = (WORD)dx;
		nec_state->ip = rei_rdw(ss + sp);
		Wreg(SP) = (WORD)(sp + 2);
	} else {
		const UINT32 bl = cpu_readmem20(ds + 0x21);
		if (bl >= 4)
			return -1;
		id = 0;
		const UINT32 c = rei_rdw(ds + 0xc) & 0xbfff;
		const UINT32 ndx = (rei_rdw(ds + 0x10 + 2 * bl) & 0x7f) | (c & 0x780);
		const UINT32 nax = (ax + rei_rdw(ds + 0x26)) & 0xffff;
		const UINT32 c28 = (0xfff8 + rei_rdw(ds + 0x28)) & 0xffff, a30 = rei_rdw(ds + 0x30);
		const UINT32 ncx = (c28 + a30) & 0xffff;
		const UINT32 bx = rei_rdw(ds + 0x18);
		if (wr) {
			write_mem_word(ss + sp - 2, (UINT16)(ip + 3));                // call c995
			write_mem_word(ss + sp - 2, (UINT16)(ip + 6));                // call c96e
			write_mem_word(ss + sp - 4, (UINT16)ax0);                     // push ax
			write_mem_word(ds + 0xc, (UINT16)c);                          // and word [c],bfff
			write_mem_word(ss + sp - 6, (UINT16)ps);                      // call 0:395e
			write_mem_word(ss + sp - 8, (UINT16)(ip + (0x3c993 - REI_RCHR_PC)));
			write_mem_word(ds + 0xe, (UINT16)c);
		}
		nec_state->CarryVal = nec_state->OverVal = 0; // test word [c],4000 (0; f7 /0 keeps A from add cx,[30])
		SetAF(c28 + a30, a30, c28);
		SetSZPF_Word(0);
		Wreg(AW) = (WORD)nax;
		Wreg(BW) = (WORD)bx;
		Wreg(CW) = (WORD)ncx;
		Wreg(DW) = (WORD)ndx;
		Wreg(IY) = 0;
		nec_state->ip = 0x23a3;
		Sreg(PS) = 0;
		Wreg(SP) = (WORD)(sp - 8);
	}
	Wreg(IX) = (WORD)si;
	Sreg(DS1) = 0x3000;
	return id << 2 | (int)(sp & 1) << 1 | (int)(ta & 1);
}
static struct rei_fp rei_rchr_fp REI_FAST_DATA;

void nec_get_task_stats(unsigned *native, unsigned *learned, unsigned *bad)
{
	*native = rei_task_fp.stats[0] + rei_cell_fp.stats[0] + rei_slot_fp.stats[0] + rei_pl_fp.stats[0] +
	       rei_pfade_fp.stats[0] + rei_brt_fp.stats[0] + rei_ball_fp.stats[0] +
	       rei_rirq_fp.stats[0] + rei_rchr_fp.stats[0] +
	       rei_rirq0_fp.stats[0] + rei_gscr_fp.stats[0] +
	       rei_rirq1_fp.stats[0] + rei_ltab_fp.stats[0] + rei_rirq2_fp.stats[0] + rei_spc_fp.stats[0] + rei_coll_fp.stats[0] + rei_pft_fp.stats[0] +
	       rei_pla_fp.stats[0] + rei_plb_fp.stats[0] + rei_wall_fp.stats[0] + rei_phit_fp.stats[0] + rei_sdis_fp.stats[0] + rei_mov_fp.stats[0] + rei_obj_fp.stats[0] + rei_grid_fp.stats[0] + rei_atk_fp.stats[0] +
	       rei_wipe_fp.stats[0] + rei_wipec_fp.stats[0] + rei_box_fp.stats[0] + rei_gmap_fp.stats[0];
	*learned = rei_task_fp.stats[1] + rei_cell_fp.stats[1] + rei_slot_fp.stats[1] + rei_pl_fp.stats[1] +
	       rei_pfade_fp.stats[1] + rei_brt_fp.stats[1] + rei_ball_fp.stats[1] +
	       rei_rirq_fp.stats[1] + rei_rchr_fp.stats[1] +
	       rei_rirq0_fp.stats[1] + rei_gscr_fp.stats[1] +
	       rei_rirq1_fp.stats[1] + rei_ltab_fp.stats[1] + rei_rirq2_fp.stats[1] + rei_spc_fp.stats[1] + rei_coll_fp.stats[1] + rei_pft_fp.stats[1] +
	       rei_pla_fp.stats[1] + rei_plb_fp.stats[1] + rei_wall_fp.stats[1] + rei_phit_fp.stats[1] + rei_sdis_fp.stats[1] + rei_mov_fp.stats[1] + rei_obj_fp.stats[1] + rei_grid_fp.stats[1] + rei_atk_fp.stats[1] +
	       rei_wipe_fp.stats[1] + rei_wipec_fp.stats[1] + rei_box_fp.stats[1] + rei_gmap_fp.stats[1];
	*bad = rei_task_fp.stats[2] + rei_cell_fp.stats[2] + rei_slot_fp.stats[2] + rei_pl_fp.stats[2] +
	       rei_pfade_fp.stats[2] + rei_brt_fp.stats[2] + rei_ball_fp.stats[2] +
	       rei_rirq_fp.stats[2] + rei_rchr_fp.stats[2] +
	       rei_rirq0_fp.stats[2] + rei_gscr_fp.stats[2] +
	       rei_rirq1_fp.stats[2] + rei_ltab_fp.stats[2] + rei_rirq2_fp.stats[2] + rei_spc_fp.stats[2] + rei_coll_fp.stats[2] + rei_pft_fp.stats[2] +
	       rei_pla_fp.stats[2] + rei_plb_fp.stats[2] + rei_wall_fp.stats[2] + rei_phit_fp.stats[2] + rei_sdis_fp.stats[2] + rei_mov_fp.stats[2] + rei_obj_fp.stats[2] + rei_grid_fp.stats[2] + rei_atk_fp.stats[2] +
	       rei_wipe_fp.stats[2] + rei_wipec_fp.stats[2] + rei_box_fp.stats[2] + rei_gmap_fp.stats[2];
}

void nec_get_pcheck_stats(unsigned *native, unsigned *learned, unsigned *bad)
{
	*native = rei_pcheck_stats[0];
	*learned = rei_pcheck_stats[1];
	*bad = rei_pcheck_stats[2];
}

// ReiREM PSP port: the fast path hooks by V33 address. One table look-up per instruction in nec_execute
// (index (pc ^ pc >> 4) & 511, all hooks in different slots) instead of a compare per hook.
#define REI_NO_HOOK 0xffffffffu
#define REI_HOOK_SLOT(pc) ((((pc) >> 5) ^ (pc)) & 511) // (>> 5: no two hooks share a slot, >> 4 left two out)
static UINT32 rei_hook_pc[512];
static void rei_hooks_init(void) // a hook that would share a slot is left out (logged)
{
	static const UINT32 pcs[] = { REI_SCAN_PC, REI_FLAGS_PC, REI_PIECE_PC, REI_NEXT_PC, REI_TASK_PC, REI_TASKE_PC, REI_CELL_PC,
				      REI_PCHECK_PC, REI_SLOT_PC, REI_PLC_PC, REI_PLN_PC, REI_PFADE_PC, REI_BRT_PC, REI_BALL_PC, REI_RIRQ_PC, REI_RCHR_PC, REI_RIRQ0_PC, REI_GSCR_PC, REI_RIRQ1_PC,
				      REI_LTAB_PC, REI_RIRQ2_PC, REI_SPC_PC, REI_SPCN_PC, REI_COLL_PC, REI_COLL2_PC, REI_PFT_PC, REI_PLA_PC,
				      REI_PLB_PC, REI_WALL_PC, REI_PHIT_PC, REI_SDIS_PC, REI_MOV1_PC,
				      REI_MOV2_PC, REI_GRID1_PC, REI_GRID2_PC, REI_ATK_PC, REI_OBJ_PC,
				      REI_WIPEA_PC, REI_WIPEB_PC, REI_WIPEC_PC, REI_VFILL_PC, REI_BOX_PC,
				      REI_SPCP_PC, REI_GMAP_PC };
	for (int i = 0; i < 512; i++)
		rei_hook_pc[i] = REI_NO_HOOK;
	for (UINT32 i = 0; i < sizeof(pcs) / sizeof(pcs[0]); i++) {
		UINT32 *slot = &rei_hook_pc[REI_HOOK_SLOT(pcs[i])];
		if (*slot == REI_NO_HOOK)
			*slot = pcs[i];
		else
			bprintf(0, _T("nbb: hook %05x shares a slot, left out\n"), pcs[i]);
	}
}
static REI_LINE_FAST_FN(10) void rei_hook(nec_state_t *nec_state, UINT32 pc, int pf_sh)
{
	switch (pc) {
	case REI_SCAN_PC: rei_scan_run(nec_state, pf_sh); break;
	case REI_FLAGS_PC: rei_flags_skip(nec_state); break;
	case REI_PIECE_PC: case REI_NEXT_PC: rei_piece_skip(nec_state, pc); break;
	case REI_TASK_PC: case REI_TASKE_PC: rei_task_run(nec_state, pf_sh); break;
	case REI_CELL_PC: rei_fp_piece(&rei_cell_fp, nec_state, pf_sh, rei_cell_apply, 0, REI_CELL_PC, 30, 0x60ee55a3u); break;
	case REI_SLOT_PC: rei_fp_piece(&rei_slot_fp, nec_state, pf_sh, rei_slot_apply, 0, REI_SLOT_PC, 109, 0xf0da381cu); break;
	case REI_PLC_PC: rei_fp_piece(&rei_pl_fp, nec_state, pf_sh, rei_pl_apply, 0, 0x3eb5d, 75, 0x0f452232u); break;
	case REI_PLN_PC: rei_fp_piece(&rei_pl_fp, nec_state, pf_sh, rei_pl_apply, 1, 0x3eb5d, 75, 0x0f452232u); break;
	case REI_PCHECK_PC: rei_pcheck_run(nec_state, pf_sh); break;
	case REI_PFADE_PC: rei_fp_piece(&rei_pfade_fp, nec_state, pf_sh, rei_pfade_apply, Wreg(CW) == 1, REI_PFADE_PC, 0x50, 0xaa98b568u); break;
	case REI_BRT_PC: rei_fp_piece(&rei_brt_fp, nec_state, pf_sh, rei_brt_apply, Wreg(CW) == 1, 0x202e, 0xe3, 0x7fd12e24u); break;
	case REI_BALL_PC:
		if (rei_ball_code == 0)
			rei_ball_code = rei_code_hash(0x3758, 0x19) == 0x52ea04d3u && rei_code_hash(0x3919, 0x40) == 0x5b70ff75u ? 1 : -1;
		if (rei_ball_code > 0)
			rei_fp_piece(&rei_ball_fp, nec_state, pf_sh, rei_ball_apply, 0, REI_BALL_PC, 0x22, 0x502eddc4u);
		break;
	case REI_RIRQ_PC:
		rei_fp_mark_tab = &rei_rirq_mark[0][0][0];
		rei_fp_mark_ip = (WORD)(nec_state->ip + (0x0e0c - REI_RIRQ_PC));
		rei_fp_piece(&rei_rirq_fp, nec_state, pf_sh, rei_rirq_apply, 0, REI_RIRQ_PC, 0x37, 0xc4613671u);
		rei_fp_mark_tab = NULL;
		break;
	case REI_RIRQ0_PC:
		rei_fp_mark_tab = &rei_rirq0_mark[0][0][0];
		rei_fp_mark_ip = (WORD)(nec_state->ip + (0x0d3c - REI_RIRQ0_PC));
		rei_fp_piece(&rei_rirq0_fp, nec_state, pf_sh, rei_rirq0_apply, 0, REI_RIRQ0_PC, 0x37, 0x673d17cfu);
		rei_fp_mark_tab = NULL;
		break;
	case REI_RIRQ1_PC:
		rei_fp_mark_tab = &rei_rirq1_mark[0][0][0];
		rei_fp_mark_ip = (WORD)(nec_state->ip + (0x0aca - REI_RIRQ1_PC));
		rei_fp_piece(&rei_rirq1_fp, nec_state, pf_sh, rei_rirq1_apply, 0, REI_RIRQ1_PC, 0x41, 0x7c51caa6u);
		rei_fp_mark_tab = NULL;
		break;
	case REI_RIRQ2_PC:
		rei_fp_mark_tab = &rei_rirq2_mark[0][0][0];
		rei_fp_mark_ip = (WORD)(nec_state->ip + (0x0ba9 - REI_RIRQ2_PC));
		rei_fp_piece(&rei_rirq2_fp, nec_state, pf_sh, rei_rirq1_apply, 1, REI_RIRQ2_PC, 0x41, 0x76ca56aau);
		rei_fp_mark_tab = NULL;
		break;
	case REI_ATK_PC:
		if (rei_atk_code == 0)
			rei_atk_code = rei_code_hash(0x656f5, 0x1a) == 0xad1eb05du ? 1 : -1;
		if (rei_atk_code > 0)
			rei_fp_piece(&rei_atk_fp, nec_state, pf_sh, rei_atk_apply, 0, REI_ATK_PC, 0x20, 0xe39f7713u);
		break;
	case REI_GRID1_PC: rei_fph_piece(&rei_grid_fp, nec_state, pf_sh, rei_grid1_apply, 0, REI_GRID1_PC, 0xb9, 0x4b47821au); break;
	case REI_GRID2_PC: rei_fph_piece(&rei_grid_fp, nec_state, pf_sh, rei_grid2_apply, 1, REI_GRID1_PC, 0xb9, 0x4b47821au); break;
	case REI_MOV1_PC: rei_fph_piece(&rei_mov_fp, nec_state, pf_sh, rei_mov1_apply, 0, REI_MOV1_PC, 0x6c, 0x89f2e909u); break;
	case REI_MOV2_PC: rei_fph_piece(&rei_mov_fp, nec_state, pf_sh, rei_mov2_apply, 1, REI_MOV2_PC, 0x5d, 0x5c13565fu); break;
	case REI_OBJ_PC:
		if (rei_obj_code == 0)
			rei_obj_code = rei_code_hash(0x3758, 0x19) == 0x52ea04d3u && rei_code_hash(0x3db95, 0x12) == 0x5e32df0cu &&
				       rei_code_hash(0x3de08, 0x33) == 0xd77cc1e8u ? 1 : -1;
		if (rei_obj_code > 0)
			rei_fph_piece(&rei_obj_fp, nec_state, pf_sh, rei_obj_apply, 0, REI_OBJ_PC, 0x8c, 0xd9c89dceu);
		break;
	case REI_SDIS_PC: rei_fph_piece(&rei_sdis_fp, nec_state, pf_sh, rei_sdis_apply, 0, REI_SDIS_PC, 0x51, 0x8a2f7a73u); break;
	case REI_PHIT_PC:
		rei_fph_piece(&rei_phit_fp, nec_state, pf_sh, rei_phit_apply, Wreg(DW) == 1, REI_PHIT_PC, 0x98, 0xd2762592u);
		break;
	case REI_WALL_PC:
		rei_fph_piece(&rei_wall_fp, nec_state, pf_sh, rei_wall_apply, Wreg(CW) == 1, REI_WALL_PC, 0xc1, 0x7a02d9c3u);
		break;
	case REI_PLA_PC: case REI_PLB_PC:
		if (rei_plab_code == 0)
			rei_plab_code = rei_code_hash(0x202ee, 0x2f) == 0x16a6aba3u && rei_code_hash(0x3eedb, 0x29) == 0xb5f949eeu ? 1 : -1;
		if (rei_plab_code > 0) {
			if (pc == REI_PLA_PC)
				rei_fp_piece(&rei_pla_fp, nec_state, pf_sh, rei_pla_apply, 0, REI_PLA_PC, 0x2f, 0x16a6aba3u);
			else
				rei_fp_piece(&rei_plb_fp, nec_state, pf_sh, rei_plb_apply, 0, 0x3eedb, 0x29, 0xb5f949eeu);
		}
		break;
	case REI_PFT_PC:
		if (rei_pft_code == 0)
			rei_pft_code = rei_code_hash(0x2077, 0x54) == 0x40f5a84eu ? 1 : -1;
		if (rei_pft_code > 0)
			rei_fph_piece(&rei_pft_fp, nec_state, pf_sh, rei_pft_apply, Wreg(CW) == 1, REI_PFT_PC, 0xe, 0xccaadbfeu);
		break;
	case REI_COLL_PC:
		if (rei_coll_code == 0)
			rei_coll_code = rei_code_hash(0x655bc, 0x2f) == 0x98d0adbbu && rei_code_hash(0x656aa, 3) == 0x991191f9u ? 1 : -1;
		if (rei_coll_code > 0)
			rei_fph_piece(&rei_coll_fp, nec_state, pf_sh, rei_coll_apply, 0, REI_COLL_PC, 0x1a, 0x17f19c17u);
		break;
	case REI_COLL2_PC: // the same loop over the list at 0x4840 (no jae: call 655bc / jmp 657a1, same return offset)
		if (rei_coll_code == 0)
			rei_coll_code = rei_code_hash(0x655bc, 0x2f) == 0x98d0adbbu && rei_code_hash(0x656aa, 3) == 0x991191f9u ? 1 : -1;
		if (rei_coll2_code == 0)
			rei_coll2_code = rei_code_hash(REI_COLL2_PC, 0x12) == 0xfa5359e7u ? 1 : -1;
		if (rei_coll_code > 0 && rei_coll2_code > 0) // (sub 1: its own learned costs)
			rei_fph_piece(&rei_coll_fp, nec_state, pf_sh, rei_coll_apply, 1, REI_COLL_PC, 0x1a, 0x17f19c17u);
		break;
	case REI_SPC_PC: case REI_SPCN_PC:
		rei_fph_piece(&rei_spc_fp, nec_state, pf_sh, rei_spc_apply, pc == REI_SPCN_PC, REI_SPC_PC, 0x106, 0xa039142du);
		break;
	case REI_SPCP_PC:
		if (rei_spcp_code == 0)
			rei_spcp_code = rei_code_hash(REI_SPCP_PC, 0x13) == 0x57bae15eu ? 1 : -1;
		if (rei_spcp_code > 0)
			rei_fph_piece(&rei_spc_fp, nec_state, pf_sh, rei_spc_apply, 2, REI_SPC_PC, 0x106, 0xa039142du);
		break;
	case REI_LTAB_PC: rei_fp_piece(&rei_ltab_fp, nec_state, pf_sh, rei_ltab_apply, 0, REI_LTAB_PC, 0x28, 0x37e5e40bu); break;
	case REI_WIPEA_PC: case REI_WIPEB_PC: case REI_WIPEC_PC:
		if (rei_ltab_fp.state == 0) // (0x0e51 code check)
			rei_ltab_fp.state = rei_code_hash(REI_LTAB_PC, 0x28) == 0x37e5e40bu ? 1 : -1;
		if (rei_ltab_fp.state > 0) {
			if (pc == REI_WIPEA_PC)
				rei_fp_piece(&rei_wipe_fp, nec_state, pf_sh, rei_wipe_apply, 0, REI_WIPEA_PC, 0x1e, 0x601c65a9u);
			else if (pc == REI_WIPEB_PC)
				rei_fp_piece(&rei_wipe_fp, nec_state, pf_sh, rei_wipe_apply, 1, REI_WIPEB_PC, 0x1e, 0x551acc0du);
			else
				rei_fp_piece(&rei_wipec_fp, nec_state, pf_sh, rei_wipe_apply, 2, REI_WIPEC_PC, 0x28, 0xf86a11b4u);
		}
		break;
	case REI_BOX_PC: rei_fph_piece(&rei_box_fp, nec_state, pf_sh, rei_box_apply, 0, REI_BOX_PC, 0x99, 0x4c2d9c79u); break;
	case REI_GMAP_PC:
		if (rei_ltab_fp.state == 0) // (0x0e51 code check)
			rei_ltab_fp.state = rei_code_hash(REI_LTAB_PC, 0x28) == 0x37e5e40bu ? 1 : -1;
		if (rei_ltab_fp.state > 0)
			rei_fp_piece(&rei_gmap_fp, nec_state, pf_sh, rei_gmap_apply, 0, REI_GMAP_PC, 0x38, 0xba62d4d6u);
		break;
	case REI_VFILL_PC: rei_fp_piece(&rei_vfill_fp, nec_state, pf_sh, rei_vfill_apply, 1, REI_VFILL_PC, 0x18, 0x4bc82912u); break;
	case REI_GSCR_PC:
		rei_fp_piece(&rei_gscr_fp, nec_state, pf_sh, rei_gscr_apply, ((Wreg(CW) + 1) & 0xffff) >= 0xf0, REI_GSCR_PC, 0x1f,
			     0x64b7a10fu);
		break;
	case REI_RCHR_PC:
		if (rei_rchr_code == 0)
			rei_rchr_code = rei_code_hash(0x395e, 0x41) == 0x83e112a1u ? 1 : -1;
		if (rei_rchr_code > 0)
			rei_fp_piece(&rei_rchr_fp, nec_state, pf_sh, rei_rchr_apply, 0, REI_RCHR_PC, 0x4c, 0xbcd9fb7au);
		break;
	}
}

#include "nec_drc.inc" // V33 dynamic recompiler (ADR-020, only with REI_DRC on MIPS)

#ifdef REI_DRC_ACTIVE
// (frontend) compile deferred blocks in spare time before waiting for vblank (nec_drc.inc drc_idle_compile). Returns count
extern "C" int rei_drc_idle_compile(unsigned budget_us) { return drc_idle_compile(budget_us); }
#endif

REI_LINE_FN(16) int nec_execute(int cycles) // (dispatcher included in the line section so placement is not shifted by other code size)
{
	nec_state_t *nec_state = sChipsPtr;
	int prev_ICount;
#ifdef REI_DRC_ACTIVE
	if (rei_drc_enabled && nec_state == &sChips[0] &&
	    ((nec_state->chip_type == V33_TYPE && nec_state->prefetch_cycles == 1) ||
	     (nec_state->chip_type == V30_TYPE && nec_state->prefetch_cycles == 2)) &&
	    nec_state->prefetch_size == 6 && drc_init())
		return nec_execute_drc(cycles);
#endif

	nec_state->icount = cycles;
	nec_state->cycles_remaining = cycles;
	nec_state->stop_run = 0;

	rei_idle_t *idle = &rei_idle[nec_state - sChips]; // ReiREM Vita port
#ifdef REI_RC_ACTIVE
	if (!rei_rc_ready) rei_rc_init();
#endif
	rei_idle_begin(idle, cycles);
	rei_nec_slice++;
	const int auto_cpu = (int)(nec_state - sChips);
	rei_auto[auto_cpu].mark = rei_auto[auto_cpu].lp_mark = 0; // (pending interrupts may change between slices: measure again)
	rei_rec = 0;
	UINT32 auto_prev = 0;        // PC of the previous instruction (a backward jump lands below it)

	if (nec_state->halted)
	{
		nec_state->icount = 0;
		//debugger_instruction_hook(device, (Sreg(PS)<<4) + nec_state->ip);
		return cycles;
	}

	const int pf_sh = rei_pf_sh(nec_state->prefetch_cycles); // ReiREM port: loop invariants
	const UINT32 idle_pc = idle->pc ? idle->pc : 0xffffffffu;
#ifdef REI_DRC
	const int scan_ok = 0; // (dynarec build: fast-path table area is used as code cache, so hooks are always off)
#else
	const int scan_ok = rei_scan_enabled && nec_state == &sChips[0];
#endif
	if (scan_ok && rei_hook_pc[0] == 0) // (never a hook address: table not built yet)
		rei_hooks_init();
	// ReiREM port: a slice that starts inside an empty-slot iteration (see rei_scan_run). Same as the loop
	// below would do first: nothing to dispatch and no interrupt shadow, then the hook
	if (scan_ok && !nec_state->no_interrupt && !(nec_state->pending_irq && ((nec_state->pending_irq & NMI_IRQ) || nec_state->IF)) &&
	    rei_scan_index(PC(nec_state)) > 0)
		rei_scan_run(nec_state, pf_sh);
	while((nec_state->icount>0) && (!nec_state->stop_run)) {
		/* Dispatch IRQ */
		if (nec_state->pending_irq && nec_state->no_interrupt==0)
		{
			if (nec_state->pending_irq & NMI_IRQ)
				external_int(nec_state);
			else if (nec_state->IF)
				external_int(nec_state);
		}

		/* No interrupt allowed between last instruction and this one */
		if (nec_state->no_interrupt)
			nec_state->no_interrupt--;

		//debugger_instruction_hook(device, (Sreg(PS)<<4) + nec_state->ip);
		// ReiREM Vita port: skip whole iterations of a known pure wait loop
		{
			const UINT32 pc_now = PC(nec_state);
			if (pc_now == idle_pc)
				rei_idle_visit(idle, &nec_state->icount, nec_state->icount);
			// M92 port: automatic idle loops (any game)
#ifdef REI_AUTODBG
			if (pc_now == REI_AUTODBG) { static int c; if (c++ < 6) fprintf(stderr, "autodbg at head prev %05x en %d\n", auto_prev, rei_auto_idle_enabled); }
#endif
			if (pc_now <= auto_prev && auto_prev - pc_now <= REI_AUTO_IDLE_SPAN && rei_auto_idle_enabled)
				rei_auto_visit(nec_state, auto_cpu, pc_now, auto_prev);
			auto_prev = pc_now;
			if (scan_ok && rei_hook_pc[REI_HOOK_SLOT(pc_now)] == pc_now) {
				// it may stop anywhere, also with icount <= 0: back to the top of the loop then
				const int ic = nec_state->icount;
				rei_auto[auto_cpu].mark = rei_auto[auto_cpu].lp_mark = 0; // (fast paths write memory directly, not through rei_fx)
				rei_rec = 0;
				REI_PROF_BEGIN(fast);
				rei_hook(nec_state, pc_now, pf_sh);
				REI_PROF_END(fast, REI_PROF_FAST);
				// the loop skips (flags, piece) leave icount > 0 and the next instruction to run here, as before
				if (nec_state->icount != ic && pc_now != REI_FLAGS_PC && pc_now != REI_PIECE_PC && pc_now != REI_NEXT_PC)
					continue;
			}
		}

#ifdef REI_RC_ACTIVE
		if (rei_rc_enabled) {
			UINT32 rpc = PC(nec_state);
			if (rpc < 0xa0000 && rei_rc_map[rpc]) {
#ifdef REI_RC_STATS
				rei_rc_st[0]++; int rc_c0 = nec_state->icount;
#endif
				rei_rc_fns[rei_rc_map[rpc] - 1](nec_state, rpc);
#ifdef REI_RC_STATS
				rei_rc_st[1] += rc_c0 - nec_state->icount;
#endif
				continue;
			}
		}
#endif
#ifdef REI_RC_STATS
		rei_rc_st[2]++; if (PC(nec_state) != 0x5a4 && PC(nec_state) != 0x5a9 && PC(nec_state) != 0x5aa) { rei_rc_st[3]++; rei_rc_miss[PC(nec_state) & 0xfffff]++; }
#endif
		prev_ICount = nec_state->icount;
		rei_ins_ic = prev_ICount;
		REI_ST(0, 1);
#ifdef REI_PROFILE
		rei_prof_ins++;
#endif
		REI_PCHIST_PC(rei_pc);
#ifdef REI_DUMPPC
		{ static int done; if (!done && PC(nec_state) == REI_DUMPPC) { done = 1; fprintf(stderr, "dump %05x:", REI_DUMPPC - 16);
		  for (int i = 0; i < 64; i++) fprintf(stderr, " %02x", cpu_readop(REI_DUMPPC - 16 + i)); fprintf(stderr, "\n"); } }
#endif
		nec_instruction[fetchop(nec_state)](nec_state);
#ifdef REI_FETCHOP_CACHED
		if (pf_sh == 0) rei_prefetch1(nec_state, prev_ICount);
		else
#endif
		do_prefetch(nec_state, prev_ICount, pf_sh);
		REI_PCHIST_ADD(0, rei_pc, prev_ICount - nec_state->icount);
#if 0
		extern int counter;
		if (counter ==-1) {
			if ((prev_ICount - nec_state->icount) > 1000) {
				bprintf(0, _T("huge cyc. jump  @  %x\n"),(Sreg(PS)<<4)+nec_state->ip);
			}
		}
#endif
	}

	rei_idle_end(idle, nec_state->icount); // ReiREM port: cycles run after the last idle-loop visit
#ifdef REI_PROFILE
	REI_PROF_ACC[REI_PROF_INS] += rei_prof_ins;
	REI_PROF_ACC[REI_PROF_HOOK] += 1000; // executed slices (shown as count/frame)
	rei_prof_ins = 0;
#endif
	cycles = cycles - nec_state->icount;
	REI_ST(1, cycles);
	nec_state->cycles_total += cycles;
	nec_state->cycles_remaining = nec_state->icount = 0;

	return cycles;
}

void necInit(int cpu, int type)
{
	nec_state_t *nec_state = &sChips[cpu];

	nec_init(cpu);

	switch (type)
	{
		case i86_TYPE:
		{
			bprintf(0, _T("*** NEC v20 w/i86 NEG opcodes (f6 18, f7 18)\n"));
			nec_state->i86_neg = 1;
			// fallthrough to V20_TYPE
		}
		case V20_TYPE:
		{
			nec_state->fetch_xor = 0;
			nec_state->chip_type=V20_TYPE;
			nec_state->prefetch_size = 4;		/* 3 words */
			nec_state->prefetch_cycles = 4;		/* four cycles per byte */
		}
		break;

		case V30_TYPE:
		{
			nec_state->fetch_xor = 0;
			nec_state->chip_type=V30_TYPE;
			nec_state->prefetch_size = 6;		/* 3 words */
			nec_state->prefetch_cycles = 2;		/* two cycles per byte / four per word */
		}
		break;

		case V33_TYPE:
		{
			nec_state->fetch_xor = 0;
			nec_state->chip_type=V33_TYPE;
			nec_state->prefetch_size = 6;
			/* FIXME: Need information about prefetch size and cycles for V33.
		         * complete guess below, nbbatman will not work
		         * properly without. */
			nec_state->prefetch_cycles = 1;		/* two cycles per byte / four per word */
		}
		break;
	}
}

int necGetPC(int n)
{
	if (n == -1) { // current cpu
		return ((sChipsPtr->sregs[PS]<<4) + sChipsPtr->ip);
	} else {
		nec_state_t *nec_state = &sChips[n];
		return ((nec_state->sregs[PS]<<4) + nec_state->ip);
	}
}

void necScan(int cpu, int nAction)
{
	nec_state_t *nec_state = &sChips[cpu];

	if (nAction & ACB_DRIVER_DATA) {
		struct BurnArea ba;
		char szText[] = "NEC #0";
		szText[5] = '1' + cpu;

		memset(&ba, 0, sizeof(ba));

		ba.Data	  = (unsigned char*)nec_state;
		ba.nLen	  = STRUCT_SIZE_HELPER(nec_state_t, stop_run);
		ba.szName = szText;
		BurnAcb(&ba);
	}
}


#if 0

/**************************************************************************
 * Generic set_info
 **************************************************************************/

static CPU_SET_INFO( nec )
{
	nec_state_t *nec_state = get_safe_token(device);

	switch (state)
	{
		/* --- the following bits of info are set as 64-bit signed integers --- */
		case CPUINFO_INT_INPUT_STATE + 0:					set_irq_line(nec_state, 0, info->i);					break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:		set_irq_line(nec_state, INPUT_LINE_NMI, info->i);		break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_POLL:	set_irq_line(nec_state, NEC_INPUT_LINE_POLL, info->i);	break;

		case CPUINFO_INT_PC:
		case CPUINFO_INT_REGISTER + NEC_PC:
			if( info->i - (Sreg(PS)<<4) < 0x10000 )
			{
				nec_state->ip = info->i - (Sreg(PS)<<4);
			}
			else
			{
				Sreg(PS) = info->i >> 4;
				nec_state->ip = info->i & 0x0000f;
			}
			break;
		case CPUINFO_INT_REGISTER + NEC_IP:				nec_state->ip = info->i;							break;
		case CPUINFO_INT_SP:
			if( info->i - (Sreg(SS_)<<4) < 0x10000 )
			{
				Wreg(SP) = info->i - (Sreg(SS_)<<4);
			}
			else
			{
				Sreg(SS_) = info->i >> 4;
				Wreg(SP) = info->i & 0x0000f;
			}
			break;
		case CPUINFO_INT_REGISTER + NEC_SP:				Wreg(SP) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_FLAGS:			ExpandFlags(info->i);					break;
		case CPUINFO_INT_REGISTER + NEC_AW:				Wreg(AW) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_CW:				Wreg(CW) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_DW:				Wreg(DW) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_BW:				Wreg(BW) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_BP:				Wreg(BP) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_IX:				Wreg(IX) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_IY:				Wreg(IY) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_ES:				Sreg(DS1) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_CS:				Sreg(PS) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_SS:				Sreg(SS_) = info->i;					break;
		case CPUINFO_INT_REGISTER + NEC_DS:				Sreg(DS0) = info->i;					break;
	}
}



/**************************************************************************
 * Generic get_info
 **************************************************************************/

static CPU_GET_INFO( nec )
{
	nec_state_t *nec_state = (device != NULL && device->token() != NULL) ? get_safe_token(device) : NULL;
	int flags;

	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CONTEXT_SIZE:					info->i = sizeof(nec_state_t);					break;
		case CPUINFO_INT_INPUT_LINES:					info->i = 1;							break;
		case CPUINFO_INT_DEFAULT_IRQ_VECTOR:			info->i = 0xff;							break;
		case DEVINFO_INT_ENDIANNESS:					info->i = ENDIANNESS_LITTLE;					break;
		case CPUINFO_INT_CLOCK_MULTIPLIER:				info->i = 1;							break;
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 1;							break;
		case CPUINFO_INT_MIN_INSTRUCTION_BYTES:			info->i = 1;							break;
		case CPUINFO_INT_MAX_INSTRUCTION_BYTES:			info->i = 8;							break;
		case CPUINFO_INT_MIN_CYCLES:					info->i = 1;							break;
		case CPUINFO_INT_MAX_CYCLES:					info->i = 80;							break;

		case DEVINFO_INT_DATABUS_WIDTH + AS_PROGRAM:	info->i = 16;					break;
		case DEVINFO_INT_ADDRBUS_WIDTH + AS_PROGRAM:	info->i = 20;					break;
		case DEVINFO_INT_ADDRBUS_SHIFT + AS_PROGRAM:	info->i = 0;					break;
		case DEVINFO_INT_DATABUS_WIDTH + AS_DATA:	info->i = 0;					break;
		case DEVINFO_INT_ADDRBUS_WIDTH + AS_DATA:	info->i = 0;					break;
		case DEVINFO_INT_ADDRBUS_SHIFT + AS_DATA:	info->i = 0;					break;
		case DEVINFO_INT_DATABUS_WIDTH + AS_IO:		info->i = 16;					break;
		case DEVINFO_INT_ADDRBUS_WIDTH + AS_IO:		info->i = 16;					break;
		case DEVINFO_INT_ADDRBUS_SHIFT + AS_IO:		info->i = 0;					break;

		case CPUINFO_INT_INPUT_STATE + 0:					info->i = (nec_state->pending_irq & INT_IRQ) ? ASSERT_LINE : CLEAR_LINE; break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:		info->i = nec_state->nmi_state;				break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_POLL:	info->i = nec_state->poll_state;				break;

		case CPUINFO_INT_PREVIOUSPC:					/* not supported */						break;

		case CPUINFO_INT_PC:
		case CPUINFO_INT_REGISTER + NEC_PC:				info->i = ((Sreg(PS)<<4) + nec_state->ip);	break;
		case CPUINFO_INT_REGISTER + NEC_IP:				info->i = nec_state->ip;							break;
		case CPUINFO_INT_SP:							info->i = (Sreg(SS_)<<4) + Wreg(SP); break;
		case CPUINFO_INT_REGISTER + NEC_SP:				info->i = Wreg(SP);					break;
		case CPUINFO_INT_REGISTER + NEC_FLAGS:			info->i = CompressFlags();				break;
		case CPUINFO_INT_REGISTER + NEC_AW:				info->i = Wreg(AW);					break;
		case CPUINFO_INT_REGISTER + NEC_CW:				info->i = Wreg(CW);					break;
		case CPUINFO_INT_REGISTER + NEC_DW:				info->i = Wreg(DW);					break;
		case CPUINFO_INT_REGISTER + NEC_BW:				info->i = Wreg(BW);					break;
		case CPUINFO_INT_REGISTER + NEC_BP:				info->i = Wreg(BP);					break;
		case CPUINFO_INT_REGISTER + NEC_IX:				info->i = Wreg(IX);					break;
		case CPUINFO_INT_REGISTER + NEC_IY:				info->i = Wreg(IY);					break;
		case CPUINFO_INT_REGISTER + NEC_ES:				info->i = Sreg(DS1);					break;
		case CPUINFO_INT_REGISTER + NEC_CS:				info->i = Sreg(PS);					break;
		case CPUINFO_INT_REGISTER + NEC_SS:				info->i = Sreg(SS_);					break;
		case CPUINFO_INT_REGISTER + NEC_DS:				info->i = Sreg(DS0);					break;
		case CPUINFO_INT_REGISTER + NEC_PENDING:		info->i = nec_state->pending_irq;				break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(nec);			break;
		case CPUINFO_FCT_INIT:							/* set per-CPU */						break;
		case CPUINFO_FCT_RESET:							info->reset = CPU_RESET_NAME(nec);				break;
		case CPUINFO_FCT_EXIT:							info->exit = CPU_EXIT_NAME(nec);					break;
		case CPUINFO_FCT_EXECUTE:						info->execute = CPU_EXECUTE_NAME(necv);			break;
		case CPUINFO_FCT_BURN:							info->burn = NULL;						break;
		case CPUINFO_FCT_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(nec);			break;
		case CPUINFO_PTR_INSTRUCTION_COUNTER:			info->icount = &nec_state->icount;				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "NEC");					break;
		case DEVINFO_STR_FAMILY:					strcpy(info->s, "NEC V-Series");		break;
		case DEVINFO_STR_VERSION:					strcpy(info->s, "2.0");					break;
		case DEVINFO_STR_SOURCE_FILE:						strcpy(info->s, __FILE__);				break;
		case DEVINFO_STR_CREDITS:					strcpy(info->s, "Bryan McPhail (V25/V35 support added by Alex W. Jackson)"); break;

		case CPUINFO_STR_FLAGS:
            flags = CompressFlags();
            sprintf(info->s, "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
                flags & 0x8000 ? 'N':'E',
                flags & 0x4000 ? '?':'.',
                flags & 0x2000 ? '?':'.',
                flags & 0x1000 ? '?':'.',
                flags & 0x0800 ? 'O':'.',
                flags & 0x0400 ? 'D':'.',
                flags & 0x0200 ? 'I':'.',
                flags & 0x0100 ? 'T':'.',
                flags & 0x0080 ? 'S':'.',
                flags & 0x0040 ? 'Z':'.',
                flags & 0x0020 ? '?':'.',
                flags & 0x0010 ? 'A':'.',
                flags & 0x0008 ? '?':'.',
                flags & 0x0004 ? 'P':'.',
                flags & 0x0002 ? '.':'?',
                flags & 0x0001 ? 'C':'.');
            break;

        case CPUINFO_STR_REGISTER + NEC_PC:				sprintf(info->s, "PC:%05X", (Sreg(PS)<<4) + nec_state->ip); break;
        case CPUINFO_STR_REGISTER + NEC_IP:				sprintf(info->s, "IP:%04X", nec_state->ip); break;
        case CPUINFO_STR_REGISTER + NEC_SP:				sprintf(info->s, "SP:%04X", Wreg(SP)); break;
        case CPUINFO_STR_REGISTER + NEC_FLAGS:			sprintf(info->s, "F:%04X", CompressFlags()); break;
        case CPUINFO_STR_REGISTER + NEC_AW:				sprintf(info->s, "AW:%04X", Wreg(AW)); break;
        case CPUINFO_STR_REGISTER + NEC_CW:				sprintf(info->s, "CW:%04X", Wreg(CW)); break;
        case CPUINFO_STR_REGISTER + NEC_DW:				sprintf(info->s, "DW:%04X", Wreg(DW)); break;
        case CPUINFO_STR_REGISTER + NEC_BW:				sprintf(info->s, "BW:%04X", Wreg(BW)); break;
        case CPUINFO_STR_REGISTER + NEC_BP:				sprintf(info->s, "BP:%04X", Wreg(BP)); break;
        case CPUINFO_STR_REGISTER + NEC_IX:				sprintf(info->s, "IX:%04X", Wreg(IX)); break;
        case CPUINFO_STR_REGISTER + NEC_IY:				sprintf(info->s, "IY:%04X", Wreg(IY)); break;
        case CPUINFO_STR_REGISTER + NEC_ES:				sprintf(info->s, "DS1:%04X", Sreg(DS1)); break;
        case CPUINFO_STR_REGISTER + NEC_CS:				sprintf(info->s, "PS:%04X", Sreg(PS)); break;
        case CPUINFO_STR_REGISTER + NEC_SS:				sprintf(info->s, "SS:%04X", Sreg(SS_)); break;
        case CPUINFO_STR_REGISTER + NEC_DS:				sprintf(info->s, "DS0:%04X", Sreg(DS0)); break;
	}
}


/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( v20 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case DEVINFO_INT_DATABUS_WIDTH + AS_PROGRAM:	info->i = 8;					break;
		case DEVINFO_INT_DATABUS_WIDTH + AS_IO:		info->i = 8;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(v20);					break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "V20");					break;

		default:										CPU_GET_INFO_CALL(nec);				break;
	}
}


/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( v30 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(v30);					break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "V30");					break;

		default:										CPU_GET_INFO_CALL(nec);				break;
	}
}


/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( v33 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(v33);					break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "V33");					break;

		default:										CPU_GET_INFO_CALL(nec);				break;
	}
}

DEFINE_LEGACY_CPU_DEVICE(V20, v20);
DEFINE_LEGACY_CPU_DEVICE(V30, v30);
DEFINE_LEGACY_CPU_DEVICE(V33, v33);

#endif
