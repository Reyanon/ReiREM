/****************************************************************************

    NEC V25/V35 emulator

    ---------------------------------------------

    TODO:

    Using V20/V30 cycle counts for now. V25/V35 cycle counts
    vary based on whether internal RAM access is enabled (RAMEN).
    Likewise, the programmable clock divider (PCK) currently only
    affects the timers, not instruction execution.

    BTCLR and STOP instructions not implemented.

    IBRK flag (trap I/O instructions) not implemented.

    Interrupt macro service function not implemented.

    Port implementation is incomplete: mode control registers are ignored.

    Timer implementation is incomplete: polling is not implemented
    (reading any of the registers just returns the last value written)

    Serial interface and DMA functions not implemented.
    Note that these functions differ considerably between
    the V25/35 and the V25+/35+.

    Make internal RAM into a real RAM region, and use an
    internal address map (remapped when IDB is written to)
    instead of memory access wrapper functions.
    That way the internal RAM would be visible to the debugger,
    among other benefits.

****************************************************************************/
// ReiREM Vita port: the V35 uses its own memory/port accessors (see nec_intf.cpp)
#define cpu_readport      v25_cpu_readport
#define cpu_writeport     v25_cpu_writeport
#include "burnint.h"
#include <stddef.h>
#include <limits.h>
#include "driver.h"
#include "nec_intf.h"
#include "bitswap.h"

#if defined(__psp__)
// (ReiREM PSP) V35 may run on the Media Engine (M107 sound side, mode 3): no syscalls there, so no logging
// (bprintf -> frontend log -> printf). The dynarec below uses its own function pointers (rei_drc35_*)
#include <stdarg.h>
static inline int v25_on_me(void)
{
	uintptr_t sp;
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	return (sp >> 31) != 0; // (ME stack is in kernel space; same test as nec_intf.cpp)
}
static INT32 v25_bprintf(INT32 nStatus, const TCHAR *fmt, ...)
{
	if (v25_on_me())
		return 0;
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	return bprintf(nStatus, _T("%s"), b);
}
#define bprintf v25_bprintf
#endif

// ReiREM PSP port: V35 memory access inlined into the core, as nec.cpp does for the V33 (same page
// tables and handlers as v25_cpu_readmem20 & co. in nec_intf.cpp). A call per fetched byte was a large
// part of the V35 time on the PSP.
extern UINT8 **rei_v25_pg_rd, **rei_v25_pg_wr, **rei_v25_pg_op, **rei_v25_pg_arg;
// ReiREM port: side effects seen by the automatic idle loop detection (v25_auto_visit): memory writes, handler
// reads, SFR and port accesses. An iteration without any and with the same state at the loop head repeats exactly
static UINT32 rei_fx35;
// Mapped memory writes are logged instead (ring, position = writes so far): an iteration may write memory if it writes the
// same bytes with the same values as the iteration before (stack use of CALL/RET/PUSH/POP in a polling loop) - then the
// memory, like the registers, is the same at the next loop head
#define V25_WL 64
static UINT32 v25_wl_a[V25_WL], v25_wl_pos;
static UINT8 v25_wl_v[V25_WL];
// While one iteration of a loop is checked for counters (v25_rec), mapped reads and all writes are logged in order (as
// nec.cpp rei_rec): a loop may write memory as counters - read-modify-write of an address not read elsewhere in the loop
#define V25_REC_MAX 16
static int v25_rec, v25_rec_n;
static UINT32 v25_rec_a[V25_REC_MAX];
static UINT8 v25_rec_w[V25_REC_MAX], v25_rec_v[V25_REC_MAX]; // 0 read, 1 write, 2 handler write
static __attribute__((noinline)) void v25_rec_log(UINT32 a, int w, UINT8 v)
{
	if (v25_rec_n < V25_REC_MAX) {
		v25_rec_a[v25_rec_n] = a;
		v25_rec_w[v25_rec_n] = (UINT8)w;
		v25_rec_v[v25_rec_n] = v;
	} else {
		v25_rec = 0;
	}
	v25_rec_n++;
}
extern UINT8 (__fastcall **rei_v25_rh)(UINT32);
extern void (__fastcall **rei_v25_wh)(UINT32, UINT8);
static inline __attribute__((always_inline)) UINT8 rei_v25_rd(UINT32 a)
{
	a &= 0xFFFFF;
	UINT8 *p = rei_v25_pg_rd[a >> 9];
	if (!p) { rei_fx35++; return (*rei_v25_rh)(a); }
	if (__builtin_expect(v25_rec, 0)) v25_rec_log(a, 0, p[a]);
	return p[a];
}
static inline __attribute__((always_inline)) UINT8 rei_v25_op(UINT32 a)
{
	a &= 0xFFFFF;
	UINT8 *p = rei_v25_pg_op[a >> 9];
	if (!p) { rei_fx35++; return (*rei_v25_rh)(a); }
	return p[a];
}
static inline __attribute__((always_inline)) UINT8 rei_v25_arg(UINT32 a)
{
	a &= 0xFFFFF;
	UINT8 *p = rei_v25_pg_arg[a >> 9];
	if (!p) { rei_fx35++; return (*rei_v25_rh)(a); }
	return p[a];
}
static inline __attribute__((always_inline)) void rei_v25_wr(UINT32 a, UINT8 d)
{
	a &= 0xFFFFF;
	UINT8 *p = rei_v25_pg_wr[a >> 9];
	if (__builtin_expect(v25_rec, 0)) v25_rec_log(a, p ? 1 : 2, d);
	if (p) {
		p[a] = d;
		v25_wl_a[v25_wl_pos & (V25_WL - 1)] = a;
		v25_wl_v[v25_wl_pos & (V25_WL - 1)] = d;
		v25_wl_pos++;
	} else {
		rei_fx35++;
		(*rei_v25_wh)(a, d);
	}
}
#define cpu_readmem20     rei_v25_rd
#define cpu_readmem20_op  rei_v25_op
#define cpu_readmem20_arg rei_v25_arg
#define cpu_writemem20    rei_v25_wr

unsigned char cpu_readport(unsigned int);
void cpu_writeport(unsigned int,unsigned int);

static inline void write_port_word(unsigned int a, unsigned short d)
{
	rei_fx35++;
	cpu_writeport((a),(unsigned char)(d));
	cpu_writeport((a)+1,(d)>>8);
}

#define cpu_readop cpu_readmem20_op
#define cpu_readop_arg cpu_readmem20_arg

#define INPUT_LINE_NMI	0x20 // nmi

#define NEC_INLINE

typedef UINT8 BOOLEAN;
typedef UINT8 BYTE;
typedef UINT16 WORD;
typedef UINT32 DWORD;

#include "v25priv.h"
#include "rei_prof.h" // ReiREM Vita port: PC histogram hook (no-op unless REI_PCHIST)
#include "rei_idle.h" // ReiREM Vita port: idle loop skip

// ReiREM port: lazy internal timers. Instead of subtracting each instruction's cycles from every
// enabled timer, the cycles are accumulated (pend) and applied once they reach the nearest expiry (next) -
// i.e. after the same instruction the per-instruction loop would have fired at - or before anything reads
// or changes the timers (rei_tim_sync: TMC0/TMC1 writes, add_timer, idle skip, state scan). Until then no
// timer can reach zero, so the values and the firing points are the same.
static struct { INT32 pend, next, dirty; } rei_tim[4];
static v25_state_t sChips[4]; // 4 cpus should be plenty!
static UINT32 v25_auto_epoch = 1; // a slot counts only with the current value: new frame, reset, state scan, run start (no keep)

static int drc35_in, drc35_ic0; // (V35 dynarec) inside a compiled block, icount at its start or at the last sync
static int rei_ins_ic35;
static void rei_tim_sync(v25_state_t *nec_state)
{
	int n = nec_state - sChips;
	if (drc35_in) { // (cycles of the block's instructions before this one)
		rei_tim[n].pend += drc35_ic0 - rei_ins_ic35;
		drc35_ic0 = rei_ins_ic35;
	}
	if (rei_tim[n].pend) {
		for (int i = 0; i < 4; i++)
			if (nec_state->timer_enabled[i])
				nec_state->timer_cycles_until_trigger[i] -= rei_tim[n].pend;
		rei_tim[n].pend = 0;
	}
	rei_tim[n].dirty = 1; // recompute 'next' after the current instruction
}

static void add_timer(v25_state_t *nec_state, int timer, double tmp, int param, int flags)
{
	rei_tim_sync(nec_state);
	// (ReiREM: M72 V35 gets clock 0 via VezInit(cpu, type), so the original formula is 0 * inf = NaN. Use x86 (int)NaN = INT_MIN
	//  on all platforms; the PSP FPU hung converting NaN to int (kengo/ltswords/cosmccop stuck loading), ARM gives 0)
	int icycles = INT_MIN;
	if (nec_state->clock != 0) {
		float cycles = (nec_state->clock / 2) * (tmp * (1.000000000 / nec_state->clock));
		icycles = (int)cycles;
	}

	nec_state->timer_param[timer]			= param;
	nec_state->timer_enabled[timer]			= 1;
	nec_state->timer_flags[timer]			= flags; // 1 = periodic, 0 = oneshot
	nec_state->timer_cycles_period[timer]		= icycles;
	nec_state->timer_cycles_until_trigger[timer]	= icycles;
}

#include "v25sfr.c"

static v25_state_t *sChipsPtr;

static void v25_timer_callback(int param)
{
	v25_state_t *nec_state = sChipsPtr;
	nec_state->pending_irq |= param;
}

NEC_INLINE void prefetch(v25_state_t *nec_state)
{
	nec_state->prefetch_count--;
}

// ReiREM port: sh = log2(prefetch_cycles) for 1/2/4, else -1 - computed once per run (rei_pf_sh)
static inline int rei_pf_sh(int c) { return (c == 1) ? 0 : (c == 2) ? 1 : (c == 4) ? 2 : -1; }
static void do_prefetch(v25_state_t *nec_state, int previous_ICount, int sh)
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
		// ReiREM PSP port: closed form of the loops below for 1/2/4 cycles per byte (V35 = 2), the same
		// code as nec.cpp (V33), identical results without per-byte iterations.
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

// ReiREM port: do_prefetch with 2 cycles per byte (the V35) as a constant — the shifts and compares fold (same code otherwise)
static inline __attribute__((always_inline)) void do_prefetch2(v25_state_t *nec_state, int previous_ICount)
{
	int diff = previous_ICount - (int) nec_state->icount;
	int k = -nec_state->prefetch_count;
	if (k > 0) {
		int m = (diff > 0) ? ((diff - 1) >> 1) : 0;
		if (m > k) m = k;
		diff -= m << 1;
		nec_state->icount -= (k - m) << 1;
		nec_state->prefetch_count = 0;
	}
	if (nec_state->prefetch_reset) {
		nec_state->prefetch_count = 0;
		nec_state->prefetch_reset = 0;
		return;
	}
	if (diff >= 2) {
		int n = diff >> 1;
		int room = nec_state->prefetch_size - nec_state->prefetch_count;
		if (room > 0) nec_state->prefetch_count += (n < room) ? n : room;
	}
}

// ReiREM port: PS<<4 of the instruction being run, set before each dispatch (v25_set_csb). Instructions that change PS or the
// register bank fetch nothing after it, so operand fetches use it instead of reading PS from the register bank each byte
static UINT32 v25_csb;
static inline __attribute__((always_inline)) void v25_set_csb(v25_state_t *nec_state) { v25_csb = Sreg(PS) << 4; }

NEC_INLINE UINT8 fetch(v25_state_t *nec_state)
{
	prefetch(nec_state);
	return cpu_readop_arg((v25_csb+nec_state->ip++) ^ nec_state->fetch_xor);
}

NEC_INLINE UINT16 fetchword(v25_state_t *nec_state)
{
	UINT16 r = FETCH();
	r |= (FETCH()<<8);
	return r;
}

#define nec_state_t v25_state_t

#include "v25instr.h"
#include "necmacro.h"
#include "necea.h"
#include "necmodrm.h"

static UINT8 parity_table[256];

// (ReiREM port: inlined into the run loop and prefix handlers — a call per instruction on the Vita)
static inline __attribute__((always_inline)) UINT8 fetchop(v25_state_t *nec_state)
{
	UINT8 ret;

	prefetch(nec_state);
	ret = cpu_readop(((Sreg(PS)<<4)+nec_state->ip++) ^ nec_state->fetch_xor);

	const UINT8 *dec = nec_state->decode;
	if (dec && nec_state->MF == 0)
		ret = dec[ret];
	return ret;
}


void v25_open(int cpu)
{
	sChipsPtr = &sChips[cpu];
}

void v25_close()
{
	sChipsPtr = NULL;
}


/***************************************************************************/

int v25_reset()
{
	v25_state_t *nec_state = sChipsPtr;
	int tmp;

	nec_state->ip = 0;
	nec_state->IBRK = 1;
	nec_state->F0 = 0;
	nec_state->F1 = 0;
	nec_state->TF = 0;
	nec_state->IF = 0;
	nec_state->DF = 0;
	nec_state->SignVal = 0;
	nec_state->AuxVal = 0;
	nec_state->OverVal = 0;
	nec_state->ZeroVal = 1;
	nec_state->CarryVal = 0;
	nec_state->ParityVal = 1;
	nec_state->pending_irq = 0;
	nec_state->unmasked_irq = INT_IRQ | NMI_IRQ;
	nec_state->bankswitch_irq = 0;
	nec_state->priority_inttu = 7;
	nec_state->priority_intd = 7;
	nec_state->priority_intp = 7;
	nec_state->priority_ints0 = 7;
	nec_state->priority_ints1 = 7;
	nec_state->IRQS = nec_state->ISPR = 0;
	nec_state->nmi_state = 0;
	nec_state->irq_state = 0;
	nec_state->poll_state = 1;
	nec_state->intp_state[0] = 0;
	nec_state->intp_state[1] = 0;
	nec_state->intp_state[2] = 0;
	nec_state->halted = 0;
	nec_state->vector = 0xff; // default
	nec_state->cycles_total = 0;
	v25_auto_epoch++; // (ReiREM: automatic idle slots)

	nec_state->TM0 = nec_state->MD0 = nec_state->TM1 = nec_state->MD1 = 0;
	nec_state->TMC0 = nec_state->TMC1 = 0;

	nec_state->RAMEN = 1;
	nec_state->TB = 20;
	nec_state->PCK = 8;
	nec_state->IDB = 0xFFE00;

	memset (nec_state->timer_enabled, 0, sizeof(char) * 4);

	tmp = nec_state->PCK << nec_state->TB;
	add_timer(nec_state, 3, tmp, INTTB, 1);

	SetRB(7);
	Sreg(PS) = 0xffff;
	Sreg(SS_) = 0;
	Sreg(DS0) = 0;
	Sreg(DS1) = 0;

	CHANGE_PC;

	return 0;
}

static rei_idle_t rei_idle[4]; // ReiREM Vita port: per-chip idle loop skip state

// ReiREM Vita port: idle skip statistics
void v25_get_idle_stats(int cpu, unsigned long long *visits, unsigned long long *skipped, int *last_d)
{
	*visits = rei_idle[cpu].visits;
	*skipped = rei_idle[cpu].skipped;
	*last_d = rei_idle[cpu].last_d;
}

void v25_set_idle_pc(int cpu, unsigned pc)
{
	rei_idle[cpu].pc = pc;
	rei_idle[cpu].mark = 0;
	rei_idle[cpu].iter = 0;
	rei_idle[cpu].tail = -1;
}

static void nec_interrupt(v25_state_t *nec_state, unsigned int_num, V25_INTSOURCES source)
{
	UINT32 dest_seg, dest_off;

	rei_idle_restart(&rei_idle[nec_state - sChips]); // ReiREM Vita port: restart idle measurement

	i_pushf(nec_state);
	nec_state->TF = nec_state->IF = 0;
	nec_state->MF = nec_state->mode_state;

	switch(source)
	{
		case BRKN:	/* force native mode */
			nec_state->MF = 1;
			break;
		case BRKS:	/* force secure mode */
			if (nec_state->decode)
				nec_state->MF = 0;
//			else
//				logerror("%06x: BRKS executed with no decryption table\n",PC(nec_state));
			break;
		case INT_IRQ:	/* get vector */
			int_num = nec_state->vector; //(*nec_state->irq_callback)(0);
			break;
		default:
			break;
	}

	dest_off = read_mem_word(int_num*4);
 	dest_seg = read_mem_word(int_num*4+2);

	PUSH(Sreg(PS));
	PUSH(nec_state->ip);
	nec_state->ip = (WORD)dest_off;
	Sreg(PS) = (WORD)dest_seg;
	CHANGE_PC;
}

static void nec_bankswitch(v25_state_t *nec_state, unsigned bank_num)
{
	int tmp = CompressFlags();

	nec_state->TF = nec_state->IF = 0;
	nec_state->MF = nec_state->mode_state;

	SetRB(bank_num);

	Wreg(PSW_SAVE) = tmp;
	Wreg(PC_SAVE) = nec_state->ip;
	nec_state->ip = Wreg(VECTOR_PC);
	CHANGE_PC;
}

static void nec_trap(v25_state_t *nec_state)
{
	v25_set_csb(nec_state);
	nec_instruction[fetchop(nec_state)](nec_state);
	nec_interrupt(nec_state, NEC_TRAP_VECTOR, BRK);
}

#define INTERRUPT(source, vector, priority) \
	if(pending & (source)) {				\
		nec_state->IRQS = vector;				\
		nec_state->ISPR |= (1 << (priority));	\
		nec_state->pending_irq &= ~(source);	\
		if(nec_state->bankswitch_irq & (source))	\
			nec_bankswitch(nec_state, priority);	\
		else									\
			nec_interrupt(nec_state, vector, source);	\
		break;	/* break out of loop */	\
	}

/* interrupt sources subject to priority control */
#define SOURCES	(INTTU0 | INTTU1 | INTTU2 | INTD0 | INTD1 | INTP0 | INTP1 | INTP2 \
				| INTSER0 | INTSR0 | INTST0 | INTSER1 | INTSR1 | INTST1 | INTTB)

static void external_int(v25_state_t *nec_state)
{
	int pending = nec_state->pending_irq & nec_state->unmasked_irq;

	if (pending & NMI_IRQ)
	{
		nec_interrupt(nec_state, NEC_NMI_VECTOR, NMI_IRQ);
		nec_state->pending_irq &= ~NMI_IRQ;
	}
	else if (pending & SOURCES)
	{
		for(int i = 0; i < 8; i++)
		{
			if (nec_state->ISPR & (1 << i)) break;

			if (nec_state->priority_inttu == i)
			{
				INTERRUPT(INTTU0, NEC_INTTU0_VECTOR, i)
				INTERRUPT(INTTU1, NEC_INTTU1_VECTOR, i)
				INTERRUPT(INTTU2, NEC_INTTU2_VECTOR, i)
			}

			if (nec_state->priority_intd == i)
			{
				INTERRUPT(INTD0, NEC_INTD0_VECTOR, i)
				INTERRUPT(INTD1, NEC_INTD1_VECTOR, i)
			}

			if (nec_state->priority_intp == i)
			{
				INTERRUPT(INTP0, NEC_INTP0_VECTOR, i)
				INTERRUPT(INTP1, NEC_INTP1_VECTOR, i)
				INTERRUPT(INTP2, NEC_INTP2_VECTOR, i)
			}

			if (nec_state->priority_ints0 == i)
			{
				INTERRUPT(INTSER0, NEC_INTSER0_VECTOR, i)
				INTERRUPT(INTSR0, NEC_INTSR0_VECTOR, i)
				INTERRUPT(INTST0, NEC_INTST0_VECTOR, i)
			}

			if (nec_state->priority_ints1 == i)
			{
				INTERRUPT(INTSER1, NEC_INTSER1_VECTOR, i)
				INTERRUPT(INTSR1, NEC_INTSR1_VECTOR, i)
				INTERRUPT(INTST1, NEC_INTST1_VECTOR, i)
			}

			if (i == 7)
				INTERRUPT(INTTB, NEC_INTTB_VECTOR, 7)
		}
	}
	else if (pending & INT_IRQ)
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

#define REI_REP_STOSW(s, c) 0 // ReiREM port: V33 only (nec.cpp)
#include "necinstr.c"
#include "v25instr.c"

/*****************************************************************************/

void v25_set_irq_line_and_vector(int irqline, int vector, int state)
{
	v25_state_t *nec_state = sChipsPtr;

	switch (irqline)
	{
		case 0:
			nec_state->vector = vector;
			nec_state->irq_state = state;
			if (state == CLEAR_LINE)
				nec_state->pending_irq &= ~INT_IRQ;
			else
			{
				nec_state->pending_irq |= INT_IRQ;
				nec_state->halted = 0;;
			}
			break;
		case INPUT_LINE_NMI:
			nec_state->vector = vector;
			if (nec_state->nmi_state == (unsigned int)state) return;
			nec_state->nmi_state = state;
			if (state != CLEAR_LINE)
			{
				nec_state->pending_irq |= NMI_IRQ;
				nec_state->halted = 0;
			}
			break;
		case NEC_INPUT_LINE_INTP0:
		case NEC_INPUT_LINE_INTP1:
		case NEC_INPUT_LINE_INTP2:
			nec_state->vector = vector;
			irqline -= NEC_INPUT_LINE_INTP0;
			if (nec_state->intp_state[irqline] == (unsigned int)state) return;
			nec_state->intp_state[irqline] = state;
			if (state != CLEAR_LINE)
				nec_state->pending_irq |= (INTP0 << irqline);
			break;
		case NEC_INPUT_LINE_POLL:
			nec_state->vector = vector;
			nec_state->poll_state = state;
			break;
	}
}

void v25_common_init(int)
{
	v25_state_t *nec_state = sChipsPtr;

	unsigned int i, j, c;

	static const V25_WREGS wreg_name[8]={ AW, CW, DW, BW, SP, BP, IX, IY };
	static const V25_BREGS breg_name[8]={ AL, CL, DL, BL, AH, CH, DH, BH };

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

	nec_state->decode = NULL;
}

void v25_set_decode(unsigned char *table)
{
	sChipsPtr->decode = table;
	sChipsPtr->mode_state = sChipsPtr->MF = (table != NULL) ? 0 : 1;
}

// ReiREM port: cycle position at the start of the instruction being interpreted (d_m92.cpp sound line spans)
unsigned int v25_ins_start_cycles()
{
	return sChipsPtr->cycles_total + (sChipsPtr->cycles_remaining - rei_ins_ic35);
}

unsigned int v25_total_cycles()
{
	return sChipsPtr->cycles_total + (sChipsPtr->cycles_remaining - sChipsPtr->icount);
}

// ReiREM PSP port: V35 dynarec state used before its instance (v25_execute_drc below)
static int drc_map_dirty = 1;
#ifdef __psp__
#define DRC_BUDGET_FRAME 2000
#else
#define DRC_BUDGET_FRAME (1 << 30)
#endif
static int drc_budget_frame = DRC_BUDGET_FRAME, drc_budget = DRC_BUDGET_FRAME;
extern "C" void rei_drc35_set_budget(int n) { if (n > 0) drc_budget_frame = drc_budget = n; } // (test) per-frame compile budget
extern "C" void rei_v25_fetch_flush(void) { drc_map_dirty = 1; } // (nec_intf: a V35 map changed)
static UINT32 drc_v25_idb = 0xffffffffu;

static int v25_frames, v25_noskip_clear; // (auto idle: see v25_noskip_n below)
extern "C" int rei_v25_counter_rule = 1; // (auto idle counter rule: record the next iteration to find counters)
void v25_new_frame()
{
	for (int i = 0; i < 4; i++) {
		nec_state_t *nec_state = &sChips[i];
		nec_state->cycles_total = 0;
	}
	v25_auto_epoch++; // (ReiREM: automatic idle slot times are frame-relative)
	drc_budget = drc_budget_frame;
	if ((++v25_frames & 127) == 0) v25_noskip_clear = 1; // (ReiREM: recheck paused loop heads every 128 frames)
}

// ReiREM port: automatic idle loops (as nec.cpp rei_auto_visit for the V33, z80 z80_auto_visit). At a loop head reached by
// a short backward jump: if the state (current register bank, flags, ip, interrupt and prefetch state) and the side-effect
// counter (rei_fx35) are the same as at the previous visit through the same jump, the CPU repeats with that period - nothing
// outside changes during a run (the internal timers only count cycles: never skipped across an expiry, the skipped cycles
// are charged to them) - so whole iterations are skipped. Not while an interrupt could be taken. Kept per run (between runs
// the driver may change memory or input lines). The jump is part of the key: an outer loop around a counting inner loop
// (M107 sound: "inc al / and al,3 / jnz" inside "cmp ss:[x],3 / jnz") repeats only at its own back edge.
#define V25_AUTO_SPAN 0x400
#define V25_AUTO_SLOTS 4
struct v25_auto_snap {
	UINT16 regs[16];
	INT32 SignVal;
	UINT32 AuxVal, OverVal, ZeroVal, CarryVal, ParityVal, pending_irq, unmasked_irq, prefix_base;
	UINT16 ip;
	UINT8 IBRK, F0, F1, TF, IF, DF, MF, RBW, ISPR, IRQS, no_interrupt, halted, prefetch_reset, seg_prefix;
	INT8 prefetch_count;
};
#define V25_AUTO_WMAX 16
// counters written by one iteration: byte or word (two consecutive byte writes) units (as nec.cpp)
#define V25_UNITS 4
struct v25_units { int n; UINT32 a[V25_UNITS]; UINT8 word[V25_UNITS]; UINT16 v[V25_UNITS]; };
static struct {
	UINT32 head, from, mark, fx, epoch, wpos;
	int wn; UINT32 wa[V25_AUTO_WMAX]; UINT8 wv[V25_AUTO_WMAX]; // writes of the last iteration (the same-writes rule)
	int fmask;            // flags that must repeat at the head for the counter rule (v25_flag_mask)
	int cphase, citer;    // counter rule: 2 = the last iteration had counters, its cost
	struct v25_units cprev; // counters after the last iteration
	struct v25_auto_snap s;
} v25_auto[4][V25_AUTO_SLOTS];
static int v25_rec_owner = -1; // slot (cpu * V25_AUTO_SLOTS + j) whose iteration v25_rec logs
// loop heads whose state kept changing (real work, not a wait): not checked again (as nec.cpp REI_AUTO_BAD) — the dynarec
// then runs them as compiled blocks (while a head is checked, its iteration runs in the interpreter so every access is logged)
#define V25_AUTO_BAD 64
static UINT32 v25_bad_head[V25_AUTO_BAD], v25_bad_from[V25_AUTO_BAD]; // (ReiREM: head and jump source, so inner/outer loops sharing a head are tracked separately)
static UINT8 v25_bad_n[V25_AUTO_BAD];
static UINT8 v25_noskip_n[V25_AUTO_BAD]; // (dynarec) visits without a skip (even with equal head state); at 64 the head is no longer recorded
static UINT8 v25_recfail_n[V25_AUTO_BAD]; // (ReiREM) recordings started without a skip; at 8 the head pauses recording (0 on skip, retried every 128 frames)
// (ReiREM) marks are per (head, source): dsoccr94 sound has a 4-iteration delay loop returning to the same head inside an outer wait loop
// (memory compare) (inc al; and al,3; jnz); the inner loop marked the head "not waiting", blocking the outer loop too, and the dynarec chained both so V35
// really ran all frame (the interpreter kept checking and skipped)
static inline int v25_bad_k(UINT32 head, UINT32 from) { const UINT32 x = head ^ (from * 0x9e37u); return (int)((x ^ (x >> 6)) & (V25_AUTO_BAD - 1)); }
static int v25_auto_is_bad(UINT32 head, UINT32 from) { const int k = v25_bad_k(head, from); return v25_bad_head[k] == head && v25_bad_from[k] == from && (v25_bad_n[k] >= 8 || v25_noskip_n[k] >= 64); }
static unsigned v25_drc_deferred; // (dynarec) blocks deferred over budget in this window (see drc35 auto idle decision below)
static int v25_drc_on; // the V35 dynarec runs (bad heads are tracked only then: the interpreter keeps checking every head)
static int v25_drc_iter_interp = 1; // (dynarec) the iteration since the last visit ran entirely in the interpreter
int rei_v25_auto_keep; // driver promise: nothing outside the V35 changes its memory between runs (M107 sound) - keep slots
static int v25_auto_rr[4];
int rei_v25_auto_enabled = 1; // harness --no-idle-skip
// (ReiREM PSP) disable idle loop auto skip only while the V35 dynarec runs (0): loop recording runs interpreted and cost more than
// the skip saved (in-game emu: dynablst 15.8->12.7, riskchal 14.3->12.8ms). Kept on when interpreting (kengo interpreted with it off:
// 14.0->41.4ms). Same results (harness --no-v25-auto hashes match for 10 games). Default 1 (harness, Vita)
int rei_v25_auto_with_drc = 1;
int rei_v25_auto_drc_policy = 0; // (PSP: 1) under dynarec, toggle auto idle per 120-frame window by amount skipped (see drc35 decision below)
int rei_drc35_short_off = 1; // (diag) 0 = keep dynarec even with short blocks (PSP dev loop host0:/rei_drc35keep)
unsigned long long rei_v25_auto_skipped[4];

static void v25_auto_take(struct v25_auto_snap *s, const v25_state_t *n)
{
	memset(s, 0, sizeof(*s));
	memcpy(s->regs, &n->ram.w[n->RBW], sizeof(s->regs));
	s->SignVal = n->SignVal;
	s->AuxVal = n->AuxVal;
	s->OverVal = n->OverVal;
	s->ZeroVal = n->ZeroVal;
	s->CarryVal = n->CarryVal;
	s->ParityVal = n->ParityVal;
	s->pending_irq = n->pending_irq;
	s->unmasked_irq = n->unmasked_irq;
	s->prefix_base = 0; // (ReiREM: meaningless between instructions; dynarec does not keep it, so iterations looked different from interpreted ones, kengo skips 0)
	s->ip = n->ip;
	s->IBRK = n->IBRK; s->F0 = n->F0; s->F1 = n->F1; s->TF = n->TF; s->IF = n->IF; s->DF = n->DF; s->MF = n->MF;
	s->RBW = n->RBW;
	s->ISPR = n->ISPR;
	s->IRQS = n->IRQS;
	s->no_interrupt = n->no_interrupt;
	s->halted = n->halted;
	s->prefetch_reset = n->prefetch_reset;
	s->seg_prefix = n->seg_prefix;
	s->prefetch_count = n->prefetch_count;
}

// --- counters (nec.cpp rei_auto_units_of / rei_auto_flag_mask / rei_auto_limit, for the V35 with its opcode table) ---
static int v25_units_of(struct v25_units *u)
{
	u->n = 0;
	if (v25_rec_n > V25_REC_MAX)
		return 0;
	for (int i = 0; i < v25_rec_n; i++) {
		if (v25_rec_w[i] == 2)
			return 0;
		if (v25_rec_w[i] != 1)
			continue;
		const UINT32 a = v25_rec_a[i];
		int reads = 0, rpos = -1; // exactly one read of a, before this write, only this instruction's bytes in between
		for (int j = 0; j < v25_rec_n; j++)
			if (v25_rec_w[j] == 0 && v25_rec_a[j] == a) {
				reads++;
				rpos = j;
			}
		if (reads != 1 || rpos > i)
			return 0;
		for (int j = rpos + 1; j < i; j++)
			if (v25_rec_a[j] + 1 < a || v25_rec_a[j] > a + 1)
				return 0;
		for (int j = 0; j < i; j++)
			if (v25_rec_w[j] == 1 && v25_rec_a[j] == a)
				return 0;
		if (u->n > 0 && !u->word[u->n - 1] && u->a[u->n - 1] + 1 == a && v25_rec_w[i - 1] == 1 && v25_rec_a[i - 1] + 1 == a) {
			u->word[u->n - 1] = 1;
			u->v[u->n - 1] |= (UINT16)(v25_rec_v[i] << 8);
			continue;
		}
		if (u->n >= V25_UNITS)
			return 0;
		u->a[u->n] = a;
		u->word[u->n] = 0;
		u->v[u->n] = v25_rec_v[i];
		u->n++;
	}
	return 1;
}

enum { V25_F_S = 1, V25_F_A = 2, V25_F_O = 4, V25_F_Z = 8, V25_F_C = 16, V25_F_P = 32, V25_F_ALL = 63 };
// code bytes without side effects (mapped pages only); opcode bytes through the decryption table like fetchop
static int v25_code_ok(UINT32 a) { return rei_v25_pg_op[(a & 0xfffff) >> 9] != NULL; }
static inline UINT8 v25_code_byte(UINT32 a) { a &= 0xfffff; return rei_v25_pg_op[a >> 9][a]; }
static inline UINT8 v25_code_op(const v25_state_t *n, UINT32 a)
{
	const UINT8 b = v25_code_byte(a);
	return (n->MF == 0 && n->decode) ? n->decode[b] : b;
}
static int v25_modrm_len(UINT32 a)
{
	const UINT8 m = v25_code_byte(a);
	const int mod = m >> 6, rm = m & 7;
	return mod == 3 ? 0 : mod == 1 ? 1 : mod == 2 ? 2 : (rm == 6 ? 2 : 0);
}
// the flags that matter at the loop head: if the body (head .. the closing jump) has only instructions that don't read
// flags, the ones the closing jump reads; otherwise all
static int v25_flag_mask(const v25_state_t *n, UINT32 head, UINT32 end)
{
	if (!v25_code_ok(head) || !v25_code_ok(end + 2) || end < head)
		return V25_F_ALL;
	UINT32 a = head;
	for (int guard = 0; a < end && guard < 32; guard++) {
		const UINT8 op = v25_code_op(n, a);
		int len = -1;
		switch (op) {
		case 0x26: case 0x2e: case 0x36: case 0x3e: len = 1; break;
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2a: case 0x2b:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3a: case 0x3b:
		case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b:
		case 0xfe: case 0xff:
			if ((op == 0xfe || op == 0xff) && ((v25_code_byte(a + 1) >> 3) & 7) > 1)
				return V25_F_ALL; // only inc/dec
			len = 2 + v25_modrm_len(a + 1);
			break;
		case 0x04: case 0x0c: case 0x24: case 0x2c: case 0x34: case 0x3c: case 0xa8: len = 2; break;
		case 0x05: case 0x0d: case 0x25: case 0x2d: case 0x35: case 0x3d: case 0xa9: len = 3; break;
		case 0xa0: case 0xa1: case 0xa2: case 0xa3: len = 3; break;
		case 0x90: len = 1; break;
		case 0x80: case 0x81: case 0x83: {
			const int r = (v25_code_byte(a + 1) >> 3) & 7;
			if (r == 2 || r == 3)
				return V25_F_ALL;
			len = 2 + v25_modrm_len(a + 1) + (op == 0x81 ? 2 : 1);
			break;
		}
		case 0xf6: case 0xf7: {
			const int r = (v25_code_byte(a + 1) >> 3) & 7;
			if (r == 0)
				len = 2 + v25_modrm_len(a + 1) + (op == 0xf7 ? 2 : 1);
			else if (r == 2 || r == 3)
				len = 2 + v25_modrm_len(a + 1);
			else
				return V25_F_ALL;
			break;
		}
		default:
			if (op >= 0x40 && op <= 0x4f) { len = 1; break; }
			if (op >= 0xb0 && op <= 0xb7) { len = 2; break; }
			if (op >= 0xb8 && op <= 0xbf) { len = 3; break; }
			return V25_F_ALL;
		}
		a += len;
	}
	if (a != end)
		return V25_F_ALL;
	switch (v25_code_op(n, end)) {
	case 0x70: case 0x71: return V25_F_O;
	case 0x72: case 0x73: return V25_F_C;
	case 0x74: case 0x75: return V25_F_Z;
	case 0x76: case 0x77: return V25_F_C | V25_F_Z;
	case 0x78: case 0x79: return V25_F_S;
	case 0x7a: case 0x7b: return V25_F_P;
	case 0x7c: case 0x7d: return V25_F_S | V25_F_O;
	case 0x7e: case 0x7f: return V25_F_S | V25_F_O | V25_F_Z;
	case 0xeb: return 0;
	default: return V25_F_ALL;
	}
}
// whole-state compare with only the flags in fmask
static int v25_same_masked(const struct v25_auto_snap *x, const struct v25_auto_snap *y, int fmask)
{
	struct v25_auto_snap a = *x, b = *y;
	if (!(fmask & V25_F_S)) a.SignVal = b.SignVal = 0;
	if (!(fmask & V25_F_A)) a.AuxVal = b.AuxVal = 0;
	if (!(fmask & V25_F_O)) a.OverVal = b.OverVal = 0;
	if (!(fmask & V25_F_Z)) a.ZeroVal = b.ZeroVal = 0;
	if (!(fmask & V25_F_C)) a.CarryVal = b.CarryVal = 0;
	if (!(fmask & V25_F_P)) a.ParityVal = b.ParityVal = 0;
	return !memcmp(&a, &b, sizeof(a));
}
// iterations that can be skipped without a counter reaching 0 or its sign boundary (nec.cpp rei_auto_limit)
static int v25_units_limit(const struct v25_units *u, const UINT16 *step, int k)
{
	for (int i = 0; i < u->n; i++) {
		if (!step[i])
			continue;
		const int bits = u->word[i] ? 16 : 8;
		const int half = 1 << (bits - 1), full = 1 << bits;
		int v = u->v[i] & (full - 1), st = step[i] & (full - 1);
		if (st >= half) st -= full;
		const int lo = v >= half ? half : 0, hi = v >= half ? full : half;
		const int room = st > 0 ? (hi - 1 - v) / st : (v - lo - (lo == 0)) / -st;
		if (room < k)
			k = room;
	}
	return k;
}

extern "C" { unsigned long long rei_v25_visits[2]; } // (diag) visits, skipped visits
static __attribute__((noinline)) void v25_auto_visit(v25_state_t *nec_state, int cpu, UINT32 head, UINT32 from)
{
	rei_v25_visits[0]++;
	const int bk = v25_bad_k(head, from);
	if (v25_noskip_clear) { v25_noskip_clear = 0; memset(v25_noskip_n, 0, sizeof(v25_noskip_n)); memset(v25_recfail_n, 0, sizeof(v25_recfail_n)); }
	if (v25_bad_head[bk] == head && v25_bad_from[bk] == from && ((v25_drc_on && v25_bad_n[bk] >= 8) || v25_noskip_n[bk] >= 64))
		return;
	int did_skip = 0; // (dynarec) did this visit skip
	{ // (ReiREM) visits without a skip, counted on any slot path, interpreter too (0 below on skip). After 64 in a row the head
	  // pauses visits (next-iteration recording): recorded iterations log every write and are slow; M92 sound (ME) V35 slowed and nbbatman main
	  // waited on sound (in-game 11.7->14.6ms, after 1d35beb counter rule). Rechecked every 128 frames. Same results
		if (v25_bad_head[bk] != head || v25_bad_from[bk] != from) { v25_bad_head[bk] = head; v25_bad_from[bk] = from; v25_bad_n[bk] = 0; v25_noskip_n[bk] = 0; v25_recfail_n[bk] = 0; }
		if (v25_noskip_n[bk] < 255) v25_noskip_n[bk]++;
	}
	const int full = v25_drc_iter_interp; // (dynarec: a compiled block ran — its writes are not logged, nothing to compare)
	struct v25_auto_snap now;
	v25_auto_take(&now, nec_state);
	const UINT32 t = (UINT32)(nec_state->cycles_total + nec_state->cycles_remaining - nec_state->icount); // (cycles in this frame)
	int j;
	for (j = 0; j < V25_AUTO_SLOTS; j++)
		if (v25_auto[cpu][j].epoch == v25_auto_epoch && v25_auto[cpu][j].head == head && v25_auto[cpu][j].from == from)
			break;
	// writes since the last visit through this jump (-1: too many to compare)
	int wn = -1;
	UINT32 wa[V25_AUTO_WMAX];
	UINT8 wv[V25_AUTO_WMAX];
	if (j < V25_AUTO_SLOTS && full) {
		const UINT32 n = v25_wl_pos - v25_auto[cpu][j].wpos;
		if (n <= V25_AUTO_WMAX) {
			wn = (int)n;
			for (int i = 0; i < wn; i++) {
				const UINT32 k = (v25_auto[cpu][j].wpos + i) & (V25_WL - 1);
				wa[i] = v25_wl_a[k];
				wv[i] = v25_wl_v[k];
			}
		}
	}
	int want_rec = 0; // (record the next iteration for the counter rule? see below)
	if (j < V25_AUTO_SLOTS) {
		const INT32 d = (INT32)(t - v25_auto[cpu][j].mark);
		const int same_w = wn >= 0 && wn == v25_auto[cpu][j].wn &&
			!memcmp(wa, v25_auto[cpu][j].wa, wn * sizeof(UINT32)) && !memcmp(wv, v25_auto[cpu][j].wv, wn);
		if (v25_auto[cpu][j].fx == rei_fx35 && same_w && d > 0 && !nec_state->no_interrupt &&
		    !(nec_state->pending_irq & nec_state->unmasked_irq) && !memcmp(&now, &v25_auto[cpu][j].s, sizeof(now))) {
			rei_tim_sync(nec_state);
			int limit = nec_state->icount;
			for (int i = 0; i < 4; i++)
				if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] - 1 < limit)
					limit = nec_state->timer_cycles_until_trigger[i] - 1;
			if (limit > d) {
				const int skip = ((limit - 1) / d) * d;
				nec_state->icount -= skip;
				for (int i = 0; i < 4; i++)
					if (nec_state->timer_enabled[i])
						nec_state->timer_cycles_until_trigger[i] -= skip;
				rei_v25_auto_skipped[cpu] += (unsigned long long)skip;
				rei_v25_visits[1]++;
				did_skip = 1;
			}
		} else if (full && v25_rec_owner == cpu * V25_AUTO_SLOTS + j && v25_rec_n <= V25_REC_MAX) {
			// counters: the iteration wrote only read-modify-write counters, the rest of the state repeats (the flags the
			// loop reads at its head) — skip whole iterations, moving each counter by its step (as nec.cpp)
			struct v25_units u;
			const int units_ok = v25_units_of(&u);
			if (units_ok && u.n > 0 && v25_auto[cpu][j].fx == rei_fx35 && d > 0 && d <= 2048 && !nec_state->no_interrupt &&
			    !(nec_state->pending_irq & nec_state->unmasked_irq) && v25_auto[cpu][j].cphase == 2 &&
			    d == v25_auto[cpu][j].citer && u.n == v25_auto[cpu][j].cprev.n &&
			    v25_same_masked(&now, &v25_auto[cpu][j].s, v25_auto[cpu][j].fmask)) {
				int same = 1;
				UINT16 step[V25_UNITS];
				for (int i = 0; i < u.n; i++) {
					same &= u.a[i] == v25_auto[cpu][j].cprev.a[i] && u.word[i] == v25_auto[cpu][j].cprev.word[i];
					step[i] = (UINT16)(u.v[i] - v25_auto[cpu][j].cprev.v[i]);
				}
				if (same) {
					rei_tim_sync(nec_state);
					int limit = nec_state->icount;
					for (int i = 0; i < 4; i++)
						if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] - 1 < limit)
							limit = nec_state->timer_cycles_until_trigger[i] - 1;
					const int kk = limit > d ? v25_units_limit(&u, step, (limit - 1) / d) : 0;
					if (kk > 0) {
						nec_state->icount -= kk * d;
						for (int i = 0; i < 4; i++)
							if (nec_state->timer_enabled[i])
								nec_state->timer_cycles_until_trigger[i] -= kk * d;
						rei_v25_auto_skipped[cpu] += (unsigned long long)(kk * d);
						did_skip = 1;
						for (int i = 0; i < u.n; i++) {
							const UINT16 v = (UINT16)(u.v[i] + step[i] * kk);
							u.v[i] = v;
							UINT8 *pg = rei_v25_pg_wr[u.a[i] >> 9];
							if (pg) pg[u.a[i]] = (UINT8)v;
							if (u.word[i]) {
								pg = rei_v25_pg_wr[(u.a[i] + 1) >> 9];
								if (pg) pg[u.a[i] + 1] = (UINT8)(v >> 8);
							}
						}
					}
				}
			}
			v25_auto[cpu][j].cphase = units_ok && u.n > 0 ? 2 : 0;
			v25_auto[cpu][j].citer = d;
			v25_auto[cpu][j].cprev = u;
		} else {
			v25_auto[cpu][j].cphase = 0;
		}
		// (ReiREM) record only when the counter rule can apply: no skip, head state equal (looking only at flags the loop reads),
		// effect count/period match, only writes differ. Recorded iterations run interpreted and are costly; recording heads that never skip
		// on every visit only added interpreter time under dynarec (dynablst: 0 skips, 70k recorded insns per 60 frames). Skips are still verified
		want_rec = !did_skip && full && v25_recfail_n[bk] < 8 && v25_auto[cpu][j].fx == rei_fx35 && d > 0 && d <= 2048 &&
			v25_same_masked(&now, &v25_auto[cpu][j].s, v25_auto[cpu][j].fmask);
		if (full && v25_drc_on) { // (dynarec: the state at the head moved and no counter rule: count towards "not a wait loop")
			if (v25_auto[cpu][j].cphase != 2 && !v25_same_masked(&now, &v25_auto[cpu][j].s, v25_auto[cpu][j].fmask)) {
				if (v25_bad_n[bk] < 255) v25_bad_n[bk]++;
			} else {
				v25_bad_n[bk] = 0;
			}
			// (ReiREM) stop recording after 64 consecutive non-skipping visits even with equal head state; recording runs interpreted only,
			// making dynarec slower than the interpreter (PSP kengo play: 74% of V35 insns in recorded iterations, 43ms vs interpreter 25ms). Same results
		}
		if (did_skip) v25_noskip_n[bk] = v25_recfail_n[bk] = 0;
		if (want_rec && v25_recfail_n[bk] < 255) v25_recfail_n[bk]++;
	} else {
		j = v25_auto_rr[cpu];
		v25_auto_rr[cpu] = (j + 1) % V25_AUTO_SLOTS;
		v25_auto[cpu][j].fmask = v25_flag_mask(nec_state, head, from);
		v25_auto[cpu][j].cphase = 0;
	}
	v25_auto[cpu][j].epoch = v25_auto_epoch;
	v25_auto[cpu][j].head = head;
	v25_auto[cpu][j].from = from;
	v25_auto[cpu][j].mark = (UINT32)(nec_state->cycles_total + nec_state->cycles_remaining - nec_state->icount);
	v25_auto[cpu][j].fx = rei_fx35;
	v25_auto[cpu][j].wpos = v25_wl_pos;
	v25_auto[cpu][j].wn = wn;
	if (wn > 0) {
		memcpy(v25_auto[cpu][j].wa, wa, wn * sizeof(UINT32));
		memcpy(v25_auto[cpu][j].wv, wv, wn);
	}
	v25_auto[cpu][j].s = now;
	if (rei_v25_counter_rule && want_rec) { // (ReiREM: off for PSP M92 sound (ME); recording cost delayed the sound side and main frames
	                            //  spiked, nbbatman windows over 20ms 4->10/10, 1d35beb. Same results)
		v25_rec_owner = cpu * V25_AUTO_SLOTS + j; // (this slot's next iteration is logged for the counter rule)
		v25_rec = 1;
		v25_rec_n = 0;
	} else { // (no recording: end any interpreted recording so the next visit does not use a stale record)
		v25_rec = 0;
		v25_rec_owner = -1;
	}
	v25_drc_iter_interp = 1;
}

#if !defined(__psp__)
static int v35_run_no, v35_pcrun = -2, v35_chip; // (v35_chip: REI_V35_CHIP; M107 sound is 1) // (diag) REI_V35_PCRUN=K: PC at each dispatcher step of the K-th run
#endif
// ---------------------------------------------------------------------------------------------------------------------
// ReiREM PSP port: the V33/V30 dynarec (nec_drc.inc, ADR-020/024) once more for the V35 (main CPU of M90 and the V35 M72
// boards: it runs as a V30 — same cycle tables and prefetch). Differences handled here (REI_DRC_V25):
// - registers and segment registers live in the internal RAM bank (RBW): blocks are compiled for one bank (block key)
// - internal RAM / SFR (IDB page, and 0xffffe/f): the compiled code's page tables leave that page empty -> slow path ->
//   v25_read_* / v25_write_* (the interpreter's memory functions)
// - opcode bytes go through the decryption table (M90, gunforce tables), operands do not (as fetchop / fetch)
// - internal timers: a block never runs past the next timer expiry (the interpreter's per-instruction timer check fires
//   right after the instruction that reaches it; the block stops at that instruction), and leaves when a handler changed
//   the timers, IDB or the bank
// - interrupts: the V35 controller (pending & unmasked), the dispatcher dispatches like v25_execute
#if defined(REI_DRC) && (defined(__mips__) || defined(REI_DRC_TEST))
#define REI_DRC_V25 1
#define rei_drc_enabled   rei_drc35_enabled
#define rei_drc_stats     rei_drc35_stats
#define rei_drc_gstep_ops rei_drc35_gstep_ops
#define rei_drc_flush_why rei_drc35_flush_why
#define rei_drc_variants  rei_drc35_variants
#define rei_drc_probes    rei_drc35_probes
#define rei_drc_finds     rei_drc35_finds
#define rei_ins_ic        rei_ins_ic35
#define rei_prefetch1(s, p) do_prefetch((s), (p), 0) // (V33 only, not used: the V35 runs with drc_pfsh 1)
// (ReiREM PSP) the V35 instance has its own cache flush and clock: on the ME (mode 3) the frontend installs an ME cache flush and clears
// the clock (main versions are syscalls). The code area is allocated by main before starting the ME (rei_drc35_preinit), never grown on the ME
#define rei_drc_cache_flush rei_drc35_cache_flush
#define rei_drc_clock32     rei_drc35_clock32
#define rei_drc_clock_fine  rei_drc35_clock_fine
#define rei_drc_now_us      rei_drc35_now_us
extern "C" {
void (*rei_drc35_cache_flush)(void *p, int len);
unsigned (*rei_drc35_clock32)(void);
unsigned (*rei_drc35_clock_fine)(void);
unsigned (*rei_drc35_now_us)(void);
int rei_drc35_on_me; // V35 dynarec runs on the ME (dynarec allowed in threaded mode, no area growth)
}
static UINT8 *drc35_rd_tab[2048], *drc35_wr_tab[2048];
static UINT8 **drc35_pg_rd = drc35_rd_tab, **drc35_pg_wr = drc35_wr_tab;
#define rei_nec_pg_rd drc35_pg_rd
#define rei_nec_pg_wr drc35_pg_wr
#define rei_nec_pg_op rei_v25_pg_op
static inline UINT8 rei_nec_op(UINT32 a) { a &= 0xfffff; const UINT8 *p = rei_v25_pg_op[a >> 9]; return p ? p[a] : 0; } // (code bytes, no side effect)
#include "nec_drc.inc"

#ifdef REI_DRC_ACTIVE
// (frontend) compile deferred blocks in spare time before waiting for vblank (nec_drc.inc drc_idle_compile). Returns count
extern "C" int rei_drc35_idle_compile(unsigned budget_us) { return drc_idle_compile(budget_us); }
// (frontend, on main before starting the ME) prebuild the V35 dynarec code area and shared routines; the ME cannot allocate or make syscalls. 1 = success
extern "C" int rei_drc35_preinit(void)
{
#if defined(__psp__) && defined(REI_DRC_FASTDATA)
	if (!rei_drc_fastdata_owner)
		rei_drc_fastdata_owner = 1; // (leave the fixed area to V33, which normally runs first and takes it. V35 uses heap)
#endif
	return drc_init();
}
#endif

// the compiled code's page tables: the V35 tables without the internal RAM / SFR page
static void drc35_build_tables(v25_state_t *n)
{
	memcpy(drc35_rd_tab, rei_v25_pg_rd, sizeof(drc35_rd_tab));
	memcpy(drc35_wr_tab, rei_v25_pg_wr, sizeof(drc35_wr_tab));
	drc35_rd_tab[0x7ff] = drc35_wr_tab[0x7ff] = NULL; // (0xffffe / 0xfffff)
	const UINT32 pg = (n->IDB >> 9) & 0x7ff;
	drc35_rd_tab[pg] = drc35_wr_tab[pg] = NULL;
	drc_v25_idb = n->IDB;
}

// the interpreter's timer step after cycles 'tapply' (pend + this run's), when an expiry or a change is due
static void drc35_tim_update(v25_state_t *nec_state, int tn, int tapply)
{
	rei_tim[tn].pend = 0;
	rei_tim[tn].dirty = 0;
	for (int i = 0; i < 4; i++) {
		if (nec_state->timer_enabled[i]) {
			nec_state->timer_cycles_until_trigger[i] -= tapply;
			if (nec_state->timer_cycles_until_trigger[i] <= 0) {
				if (nec_state->timer_flags[i]) {	// periodic
					nec_state->timer_cycles_until_trigger[i] = nec_state->timer_cycles_period[i];
				} else {				// oneshot
					nec_state->timer_enabled[i] = 0;
				}
				v25_timer_callback(nec_state->timer_param[i]);
			}
		}
	}
	int nx = 0x7fffffff;
	for (int i = 0; i < 4; i++)
		if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] < nx)
			nx = nec_state->timer_cycles_until_trigger[i];
	rei_tim[tn].next = nx;
}

// (PSP) one code cache region for both dynarec instances: the V30/V33 one has it when that CPU is in the game (M92/M107
// main CPU — the sound V35 waits most of the time anyway); the V35 one only on boards whose main CPU is the V35 (M90, M72 V35)
// (2026-10-13) on M107 the sound V35 also runs on main (single-thread pipeline), 1/3 of main insns in dsoccr94 gameplay. Then V35 gets
// a separate heap code area (nec_drc.inc drc_init). M92 (threaded) with V35 on the ME stays interpreted
extern "C" int rei_nec_present(void);
extern int rei_threaded;
static inline int drc35_allowed(void)
{
#if defined(__psp__) && defined(REI_DRC_FASTDATA)
	return !rei_nec_present() || !rei_threaded || rei_drc35_on_me; // (mode 3: sound V35 runs dynarec on the ME)
#else
	return 1;
#endif
}

#if !defined(__psp__)
static void v35_trace(v25_state_t *n, int done);
#define V35_PCLOG() do { if (v35_pcrun == v35_run_no && nec_state == &sChips[v35_chip]) fprintf(stderr, "pc %05x ic=%d f=%04x op %02x/%02x %02x %02x %02x %02x\n", pc_now, nec_state->icount, (unsigned)CompressFlags(), rei_nec_op(pc_now), (nec_state->MF == 0 && nec_state->decode) ? nec_state->decode[rei_nec_op(pc_now)] : rei_nec_op(pc_now), rei_nec_op(pc_now + 1), rei_nec_op(pc_now + 2), rei_nec_op(pc_now + 3), rei_nec_op(pc_now + 4)); } while (0)
#else
#define V35_PCLOG() ((void)0)
#define v35_trace(n, d) ((void)0)
#endif
static UINT32 drc35_jc_rbw = 0xffffffffu;
#if defined(__psp__)
#define drc35_dbg_noblk 0
#else
static const int drc35_dbg_noblk = getenv("REI_DRC35_NOBLK") != NULL; // (diag) build blocks but do not run them
#endif // the bank the jump cache entries were made for

extern "C" { unsigned long long rei_drc35_dbg[6]; } // (diag) 0 table rebuild, 1 interp due to shadow/record/prefetch reset, 2 interp due to timer dirty, 3 no block (fn NULL)/budget

extern "C" { unsigned long long rei_drc35_cycles; }
#if defined(__psp__)
#include "rei_me.h"
// (diag) progress of the V35 dynarec on the ME (uncached): 0 stage (1 entered 2 pre-compile 3 post-compile 4 pre-block 5 post-block), 1 compiles, 2 block runs, 3 last key
extern "C" { volatile UINT32 rei_drc35_me_diag[4] REI_ME_SHARED; }
#define V35_MED(i, v) do { if (rei_drc35_on_me) REI_SH(rei_drc35_me_diag[i]) = (UINT32)(v); } while (0)
#else
#define V35_MED(i, v) do { } while (0)
#endif
static int v25_execute_drc(int cycles)
{
	V35_MED(0, 1);
	v25_state_t *nec_state = sChipsPtr;
	int prev_ICount;

	int pending = nec_state->pending_irq & nec_state->unmasked_irq;
	if (nec_state->halted && pending)
	{
		for(int i = 0; i < 8; i++)
		{
			if (nec_state->ISPR & (1 << i)) break;
			if (nec_state->priority_inttu == i && (pending & (INTTU0|INTTU1|INTTU2)))
				nec_state->halted = 0;
			if (nec_state->priority_intd == i && (pending & (INTD0|INTD1)))
				nec_state->halted = 0;
			if (nec_state->priority_intp == i && (pending & (INTP0|INTP1|INTP2)))
				nec_state->halted = 0;
			if (nec_state->priority_ints0 == i && (pending & (INTSER0|INTSR0|INTST0)))
				nec_state->halted = 0;
			if (nec_state->priority_ints1 == i && (pending & (INTSER1|INTSR1|INTST1)))
				nec_state->halted = 0;
			if (i == 7 && (pending & INTTB))
				nec_state->halted = 0;
		}
	}
	if (nec_state->halted)
	{
		nec_state->icount = 0;
		return 0;
	}

	nec_state->icount = cycles;
	nec_state->cycles_remaining = cycles;
	nec_state->stop_run = 0;

	rei_idle_t *idle = &rei_idle[nec_state - sChips];
	rei_idle_begin(idle, cycles);
	const int pf_sh = rei_pf_sh(nec_state->prefetch_cycles);
	const UINT32 idle_pc = idle->pc ? idle->pc : 0xffffffffu;
	const int auto_cpu = nec_state - sChips;
	const int tn = auto_cpu;
	if (!rei_v25_auto_keep) v25_auto_epoch++;
	UINT32 auto_prev = 0;

	{ // (ReiREM) if blocks are too short (avg < 5.0 insns/block in a busy 120-frame window), interpret this game; block entry/exit and timer budget cost
	  // outweighed the gain (PSP kengo play: 3.5 insns/block, 29.3ms vs interpreter 25.2ms; M90 has 12-24). Same results.
	  // (2026-10-14 PSP in-game: kengo/ltswords 4.5-4.8 insns 17.5ms vs interpreter 14.4ms, dynablst 5.2-5.6 insns 15.9 vs 26.4ms,
	  //  cosmccop 9.6-11.8 insns 14.9 vs 18.6ms. With a 600-frame window kengo only switched ~600 frames after game start (1820):
	  //  V35 is mostly idle in title/demo so blocks are few -> 120-frame window, and only after 2 consecutive very busy windows (60k+ blocks,
	  //  kengo/ltswords 120k in-game). bombrman had one 22k-block, 3.6-insn window right after loading, switched, and slowed from 11 to 15ms)
		static UINT32 f0;
		static int started, short_n;
		static unsigned long long r0, i0;
		if (!started || (UINT32)(nCurrentFrame - f0) >= 120) {
			const unsigned long long runs = rei_drc_stats[0] - r0, insns = rei_drc_stats[1] - i0;
			if (!v25_drc_deferred) // (skip windows with over-budget deferred blocks: blocks break at not-yet-built blocks and look short)
				short_n = (started && runs > 60000 && insns * 10 < runs * 50) ? short_n + 1 : 0; // (avg < 5.0 insns)
			if (rei_drc35_short_off && short_n >= 2) {
				rei_drc_enabled = 0;
				v25_drc_on = 0; // (auto idle bad-head marking only under dynarec; the interpreter keeps checking all heads)
				bprintf(PRINT_IMPORTANT, _T("drc35: blocks too short (%u insns / %u runs), using the interpreter\n"), (unsigned)insns, (unsigned)runs);
			}
			if (rei_v25_auto_drc_policy && started) {
				// (ReiREM PSP) auto idle under dynarec: since blocks also log writes (drc_emit_wlog) and can skip without recording,
				// games with many idle loops got much faster (kengo in-game emu 16.4 -> 13.2, ltswords 17.3 -> 13.7) but games with nothing
				// to skip only paid visit cost (dynablst 12.9 -> 15.3). Decide by cycles skipped per window: off after 2 consecutive windows
				// under 2k cycles/frame; while off, retry every 4 windows (~8 s) (no idle loops right after start, so it turned off in the first window). Same results (skipping never changes results)
				static unsigned long long s0;
				static int off_n, low_n;
				unsigned long long sk = 0;
				for (int i = 0; i < 4; i++) sk += rei_v25_auto_skipped[i];
				const unsigned long long per_frame = (sk - s0) / 120;
				if (rei_v25_auto_with_drc) {
					// (skip windows with over-budget deferred blocks: idle loop blocks did not exist yet so nothing skipped; with V35 budget 500
					//  a kengo scene-change window read 0, turned it off, and it stayed off ~8 s: in-game 11.9 -> 17.2ms)
					if (!v25_drc_deferred)
						low_n = per_frame < 2000 ? low_n + 1 : 0;
					if (low_n >= 2) {
						low_n = 0;
						rei_v25_auto_with_drc = 0;
						off_n = 0;
						bprintf(PRINT_IMPORTANT, _T("drc35: idle skip off (%u cycles/frame skipped)\n"), (unsigned)per_frame);
					}
				} else if (++off_n >= 4) {
					rei_v25_auto_with_drc = 1;
					low_n = 1; // (a retry lasts one window)
				}
				s0 = sk;
			}
			// (diag, ME) window decision: 0-3 dynarec on, 4-7 short windows, 8-15 windows judged, 16- insns per block x10
			V35_MED(2, (UINT32)rei_drc_enabled | ((UINT32)(short_n & 15) << 4) | (((REI_SH(rei_drc35_me_diag[2]) >> 8) + 1) & 0xff) << 8 |
				(UINT32)(runs ? (insns * 10 / runs > 0xffff ? 0xffff : insns * 10 / runs) : 0) << 16);
			f0 = nCurrentFrame; r0 = rei_drc_stats[0]; i0 = rei_drc_stats[1];
			v25_drc_deferred = 0;
			started = 1;
		}
	}
	if (!drc_v30) { // (V35 = V30 cycles and prefetch)
		drc_v30 = 1;
		drc_pfsh = 1;
		drc_flush_all();
	}
	if (idle_pc != rei_drc_idle_pc) {
		rei_drc_idle_pc = idle_pc;
		DRC_WHY(4); drc_flush_all();
	}
	UINT32 *link_from = NULL;
	unsigned link_gen = 0;
	v25_drc_on = 1;
	while (nec_state->icount > 0 && !nec_state->stop_run) {
		if (nec_state->no_interrupt==0 && (nec_state->pending_irq & nec_state->unmasked_irq))
		{
			if (nec_state->pending_irq & NMI_IRQ)
				external_int(nec_state);
			else if (nec_state->IF)
				external_int(nec_state);
		}
		const int shadow = nec_state->no_interrupt;
		if (nec_state->no_interrupt)
			nec_state->no_interrupt--;

		const UINT32 pc_now = PC(nec_state);
		V35_PCLOG();
		if (pc_now == idle_pc) {
			rei_tim_sync(nec_state);
			int limit = nec_state->icount;
			for (int i = 0; i < 4; i++)
				if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] - 1 < limit)
					limit = nec_state->timer_cycles_until_trigger[i] - 1;
			int skipped = rei_idle_visit(idle, &nec_state->icount, limit);
			if (skipped)
				for (int i = 0; i < 4; i++)
					if (nec_state->timer_enabled[i])
						nec_state->timer_cycles_until_trigger[i] -= skipped;
		}
		if (pc_now <= auto_prev && auto_prev - pc_now <= V25_AUTO_SPAN && rei_v25_auto_enabled && rei_v25_auto_with_drc)
			v25_auto_visit(nec_state, auto_cpu, pc_now, auto_prev);
		auto_prev = pc_now;

		if (!shadow && !v25_rec && !nec_state->prefetch_reset && !rei_tim[tn].dirty) {
			if (drc_map_dirty || drc_v25_idb != nec_state->IDB) {
				rei_drc35_dbg[0]++;
				drc_map_dirty = 0;
				drc35_build_tables(nec_state);
				drc_check_pages();
				link_from = NULL;
			}
			if (nec_state->RBW != drc35_jc_rbw) { // (the jump cache goes to blocks of one bank)
#if !defined(__psp__)
				{ static int on = -1; static unsigned long long n; if (on < 0) on = getenv("REI_JCSTAT") != NULL;
				  if (on && (++n % 10000) == 0) fprintf(stderr, "jcstat clears %llu frame %d\n", n, (int)nCurrentFrame); }
#endif
				for (int i = 0; i < DRC_JC; i++)
					drc_jc[i][0] = 0xffffffffu;
				drc35_jc_rbw = nec_state->RBW;
				link_from = NULL;
			}
			drc_rbw = nec_state->RBW;
			{ // opcode table (BRKN/BRKS/RETEM/IRET switch the mode MF: the blocks decoded with the other table)
				const UINT8 *dec = (nec_state->MF == 0 && nec_state->decode) ? nec_state->decode : NULL;
				if (dec != drc_dec_tab) {
					drc_dec_tab = dec;
					DRC_WHY(5); drc_flush_all();
					link_from = NULL;
				}
			}
			const UINT32 key = ((UINT32)Sreg(PS) << 16) | nec_state->ip;
			const int cnt = nec_state->prefetch_count;
			struct drc_block *b = drc_find(key, cnt, rei_nec_pg_op[pc_now >> 9]);
			if (b && b->fn && !drc_valid(b)) {
				rei_drc_stats[5]++;
				b->key = 0xfffffffeu;
				b = NULL;
				link_from = NULL;
			}
			if (!b && drc_budget <= 0) {
				drc_queue(nec_state, key, cnt);
				v25_drc_deferred++;
			}
			if (!b && drc_budget > 0) {
#ifdef REI_PROFILE
				const uint64_t pc0 = rei_prof_clock(); // (profile: V35 compile -> "ym" slot)
#else
				const unsigned pc0 = rei_drc_clock32 ? rei_drc_clock32() : 0; // (diag: compile us -> stat 10)
#endif
				V35_MED(0, 2); V35_MED(3, key);
				b = drc_compile(nec_state, key, cnt);
				V35_MED(0, 3); V35_MED(1, REI_SH(rei_drc35_me_diag[1]) + 1);
				drc_budget -= (b && b->fn) ? b->ninsn : 1;
#ifdef REI_PROFILE
				REI_PROF_ACC[REI_PROF_YM] += rei_prof_clock() - pc0;
#else
				if (rei_drc_clock32)
					rei_drc_stats[10] += rei_drc_clock32() - pc0;
#endif
			}
			if (b && b->fn && !drc35_dbg_noblk) {
				if (link_from && link_gen == drc_gen)
					drc_try_link(link_from, b, Sreg(PS));
				if (cnt == 0 && b->pc != rei_drc_idle_pc && !drc_page_vol[b->pg0] && !drc_page_vol[b->pg1]) {
					const UINT32 h = (key ^ (key >> 10)) & (DRC_JC - 1);
					drc_jc[h][0] = key;
					drc_jc[h][1] = (UINT32)(uintptr_t)((const UINT32 *)(void *)b->fn + 4);
				}
				rei_drc_stats[0]++;
				rei_drc_stats[1] += b->ninsn;
#ifndef REI_DRC_TEST
				// timers: never past the next expiry (the block stops after the instruction that reaches it). icount runs
				// relative to that budget: cycles_remaining moves with it so the cycle position (v25_total_cycles,
				// v25_ins_start_cycles) stays right inside the block; rei_tim_sync inside the block (timer register
				// access) adds the block's cycles so far to pend (drc35_ic0)
				// (64-bit, >= 1: M72 V35 has clock 0 so timers start at INT_MIN; 32-bit next - pend overflowed,
				//  the budget went wrong and cycles jumped 2^31 so the run never ended. cosmccop frame 7381, reproduced on PSP/MIPS harness. If expiry passed or
				//  is negative, handle timers after one insn, as the interpreter does per insn)
				int budget = nec_state->icount;
				if (rei_tim[tn].next != 0x7fffffff) {
					const INT64 room = (INT64)rei_tim[tn].next - rei_tim[tn].pend;
					if (room < budget)
						budget = room < 1 ? 1 : (int)room;
				}
				const int saved = nec_state->icount - budget;
				nec_state->icount = budget;
				nec_state->cycles_remaining -= saved;
				drc35_ic0 = budget;
				drc35_in = 1;
				drc_last_stub = NULL;
#ifdef REI_PROFILE
				const uint64_t pb0 = rei_prof_clock(); // (profile: time inside V35 blocks -> "sound" slot)
#endif
				auto_prev = b->fn(nec_state);
#ifdef REI_PROFILE
				REI_PROF_ACC[REI_PROF_SOUND] += rei_prof_clock() - pb0;
#endif
				drc35_in = 0;
				const int used = drc35_ic0 - nec_state->icount;
				nec_state->icount += saved;
				nec_state->cycles_remaining += saved;
				// (blocks also log mapped writes to the write ring and count handler accesses in rei_fx35 (drc_emit_wlog), so iterations run
				//  by blocks can be compared too: v25_drc_iter_interp is not lowered)
				const int tapply = rei_tim[tn].pend + used;
				if (!rei_tim[tn].dirty && tapply < rei_tim[tn].next)
					rei_tim[tn].pend = tapply;
				else
					drc35_tim_update(nec_state, tn, tapply);
				link_from = drc_last_stub;
				link_gen = drc_gen;
				continue;
#endif
			}
		}
		link_from = NULL;
		if (v25_rec) rei_drc35_dbg[1]++;
		else if (shadow) rei_drc35_dbg[4]++;
		else if (nec_state->prefetch_reset) rei_drc35_dbg[5]++;
		else if (rei_tim[tn].dirty) rei_drc35_dbg[2]++;
		else rei_drc35_dbg[3]++;

		prev_ICount = nec_state->icount;
		rei_ins_ic35 = prev_ICount;
		rei_drc_stats[2]++;
		v25_set_csb(nec_state);
		nec_instruction[fetchop(nec_state)](nec_state);
		do_prefetch(nec_state, prev_ICount, pf_sh);

		// timers (as v25_execute)
		int tapply = rei_tim[tn].pend + (prev_ICount - nec_state->icount);
		if (!rei_tim[tn].dirty && tapply < rei_tim[tn].next) {
			rei_tim[tn].pend = tapply;
			continue;
		}
		drc35_tim_update(nec_state, tn, tapply);
	}

	V35_MED(0, 10);
	rei_idle_end(idle, nec_state->icount);
	cycles = cycles - nec_state->icount;
	nec_state->cycles_total += cycles;
	nec_state->icount = nec_state->cycles_remaining = 0;
	rei_drc35_cycles += (unsigned long long)cycles; // (diag: V35 cycles run by dynarec)
	v35_trace(nec_state, cycles);
	return cycles;
}
#endif // REI_DRC (V35)
// ---------------------------------------------------------------------------------------------------------------------

#if !defined(__psp__)
// (diag) REI_V35_TRACE: at each V35 run end, summary of PC/cycles/internal RAM/write pages/palette pages (dynarec diff)
static void v35_trace(v25_state_t *n, int done)
{
	static int on = -1;
	if (on < 0) on = getenv("REI_V35_TRACE") != NULL;
	if (v35_pcrun == -2) { const char *e = getenv("REI_V35_PCRUN"); v35_pcrun = e ? atoi(e) : -1; const char *c = getenv("REI_V35_CHIP"); v35_chip = c ? atoi(c) : 0; }
	if (n == &sChips[v35_chip]) v35_run_no++;
	if (!on || n != &sChips[0]) return;
	UINT32 h = 2166136261u;
	for (unsigned i = 0; i < sizeof(n->ram); i++) h = (h ^ ((UINT8 *)&n->ram)[i]) * 16777619u;
	UINT32 hm = 2166136261u;
	for (int pg = 0; pg < 2048; pg++) {
		const UINT8 *p = rei_v25_pg_wr[pg] ? rei_v25_pg_wr[pg] : (pg == (0xe0000 >> 9) || pg == (0xe0200 >> 9)) ? rei_v25_pg_rd[pg] : NULL;
		if (!p) continue;
		for (int k = 0; k < 512; k++) hm = (hm ^ p[(pg << 9) + k]) * 16777619u;
	}
	fprintf(stderr, "v35 pc=%05x done=%d tot=%u ip=%04x ram=%08x mem=%08x pend=%x\n", (unsigned)((n->ram.w[n->RBW + PS] << 4) + n->ip) & 0xfffff, done, (unsigned)n->cycles_total, n->ip, h, hm, (unsigned)n->pending_irq);
}
#else
#define v35_trace(n, d) ((void)0)
#endif

#ifndef V35_PCLOG
#define V35_PCLOG() ((void)0) // (build without dynarec)
#endif
#ifdef REI_DRC_ACTIVE
static v25_state_t *drc35_chip; // (dynarec for one V35 only: the first chip to enter; M90/M72 chip 0, M107 sound chip 1)
// (nec_intf VezExit, in-app game switching) code cache, tables and the chosen chip back to nothing for the next game
extern "C" void rei_drc35_exit(void)
{
	drc_exit();
	drc_v25_idb = 0xffffffffu;
	drc35_jc_rbw = 0xffffffffu;
	drc35_chip = NULL;
	v25_drc_on = 0;
}
#endif
int v25_execute(int cycles)
{
	v25_state_t *nec_state = sChipsPtr;
	int prev_ICount;
#ifdef REI_DRC_ACTIVE
	if (!drc35_chip) drc35_chip = nec_state;
	if (rei_drc_enabled && nec_state == drc35_chip && nec_state->chip_type == V30_TYPE && nec_state->prefetch_cycles == 2 &&
	    nec_state->prefetch_size == 6 && !nec_state->fetch_xor && drc35_allowed() && drc_init())
		return v25_execute_drc(cycles);
	v25_drc_on = 0; // (running interpreted: let auto idle check all heads, when dynarec was turned off midway)
#endif

	int pending = nec_state->pending_irq & nec_state->unmasked_irq;

	if (nec_state->halted && pending)
	{
		for(int i = 0; i < 8; i++)
		{
			if (nec_state->ISPR & (1 << i)) break;

			if (nec_state->priority_inttu == i && (pending & (INTTU0|INTTU1|INTTU2)))
				nec_state->halted = 0;

			if (nec_state->priority_intd == i && (pending & (INTD0|INTD1)))
				nec_state->halted = 0;

			if (nec_state->priority_intp == i && (pending & (INTP0|INTP1|INTP2)))
				nec_state->halted = 0;

			if (nec_state->priority_ints0 == i && (pending & (INTSER0|INTSR0|INTST0)))
				nec_state->halted = 0;

			if (nec_state->priority_ints1 == i && (pending & (INTSER1|INTSR1|INTST1)))
				nec_state->halted = 0;

			if (i == 7 && (pending & INTTB))
				nec_state->halted = 0;
		}
	}

	if (nec_state->halted)
	{
		nec_state->icount = 0;
		return 0;
	}

	nec_state->icount = cycles;
	nec_state->cycles_remaining = cycles;
	nec_state->stop_run = 0;

	rei_idle_t *idle = &rei_idle[nec_state - sChips]; // ReiREM Vita port
	rei_idle_begin(idle, cycles);

	const int pf_sh = rei_pf_sh(nec_state->prefetch_cycles); // ReiREM port: loop invariants
	const UINT32 idle_pc = idle->pc ? idle->pc : 0xffffffffu;
	const int auto_cpu = nec_state - sChips; // ReiREM port: automatic idle loops (v25_auto_visit), per run
	UINT32 auto_prev = 0;
	if (!rei_v25_auto_keep) v25_auto_epoch++;
	while(nec_state->icount>0 && !nec_state->stop_run) {
		/* Dispatch IRQ */
		if (nec_state->no_interrupt==0 && (nec_state->pending_irq & nec_state->unmasked_irq))
		{
			if (nec_state->pending_irq & NMI_IRQ)
				external_int(nec_state);
			else if (nec_state->IF)
				external_int(nec_state);
		}

		/* No interrupt allowed between last instruction and this one */
		if (nec_state->no_interrupt)
			nec_state->no_interrupt--;

		// ReiREM Vita port: skip whole iterations of a known pure wait loop.
		// Never skip across an internal timer expiry; skipped cycles are charged to the timers.
		v25_set_csb(nec_state);
		const UINT32 pc_now = v25_csb + nec_state->ip; // (ReiREM: once per instruction; skipping does not move the PC)
		if (pc_now == idle_pc) {
			rei_tim_sync(nec_state);
			int limit = nec_state->icount;
			for (int i = 0; i < 4; i++)
				if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] - 1 < limit)
					limit = nec_state->timer_cycles_until_trigger[i] - 1;
			int skipped = rei_idle_visit(idle, &nec_state->icount, limit);
			if (skipped)
				for (int i = 0; i < 4; i++)
					if (nec_state->timer_enabled[i])
						nec_state->timer_cycles_until_trigger[i] -= skipped;
		}
		{
			V35_PCLOG();
			if (pc_now <= auto_prev && auto_prev - pc_now <= V25_AUTO_SPAN && rei_v25_auto_enabled)
				v25_auto_visit(nec_state, auto_cpu, pc_now, auto_prev);
			auto_prev = pc_now;
		}

		prev_ICount = nec_state->icount;
		rei_ins_ic35 = prev_ICount;
		REI_PCHIST_PC(rei_pc);
		UINT8 op; // (ReiREM: fetchop with the PC computed above)
		{
			prefetch(nec_state);
			nec_state->ip++;
			op = cpu_readop(pc_now ^ nec_state->fetch_xor);
			const UINT8 *dec = nec_state->decode;
			if (dec && nec_state->MF == 0)
				op = dec[op];
		}
#ifdef REI_DUMPPC35
		{ static int done; if (!done && PC(nec_state) == REI_DUMPPC35) { done = 1; fprintf(stderr, "dump35 %05x:", REI_DUMPPC35 - 16);
		  for (int i = 0; i < 64; i++) { UINT8 b = cpu_readop(REI_DUMPPC35 - 16 + i); fprintf(stderr, " %02x/%02x", b, (nec_state->MF == 0 && nec_state->decode) ? nec_state->decode[b] : b); }
		  fprintf(stderr, "\n"); } }
#endif
		nec_instruction[op](nec_state);
		if (pf_sh == 1)
			do_prefetch2(nec_state, prev_ICount);
		else
			do_prefetch(nec_state, prev_ICount, pf_sh);
		REI_PCHIST_ADD(1, rei_pc, prev_ICount - nec_state->icount);

		// timers! (ReiREM port: applied lazily, see rei_tim_sync)
		const int tn = nec_state - sChips;
		int tapply = rei_tim[tn].pend + (prev_ICount - nec_state->icount);
		if (!rei_tim[tn].dirty && tapply < rei_tim[tn].next) {
			rei_tim[tn].pend = tapply;
			continue;
		}
		rei_tim[tn].pend = 0;
		rei_tim[tn].dirty = 0;
		for (int i = 0; i < 4; i++) {
			if (nec_state->timer_enabled[i]) {
				nec_state->timer_cycles_until_trigger[i] -= tapply;

				if (nec_state->timer_cycles_until_trigger[i] <= 0) {
					if (nec_state->timer_flags[i]) {	// periodic
						nec_state->timer_cycles_until_trigger[i] = nec_state->timer_cycles_period[i];
					} else {				// oneshot
						nec_state->timer_enabled[i] = 0; // disabled
					}

					v25_timer_callback(nec_state->timer_param[i]);
				}
			}
		}
		{
			int nx = 0x7fffffff;
			for (int i = 0; i < 4; i++)
				if (nec_state->timer_enabled[i] && nec_state->timer_cycles_until_trigger[i] < nx)
					nx = nec_state->timer_cycles_until_trigger[i];
			rei_tim[tn].next = nx;
		}
	}

	rei_idle_end(idle, nec_state->icount); // ReiREM port: cycles run after the last idle-loop visit
	cycles = cycles - nec_state->icount;
	nec_state->cycles_total += cycles;
	nec_state->icount = nec_state->cycles_remaining = 0;
	v35_trace(nec_state, cycles);

	return cycles;
}

void v25Init(int cpu, int type, int clock)
{
	sChipsPtr = &sChips[cpu];
	v25_state_t *nec_state = sChipsPtr;

	v25_common_init(cpu);

	nec_state->clock = clock; // not divided!

	switch (type)
	{
		case V20_TYPE: // v25
		{
			nec_state->fetch_xor = 0;
			nec_state->chip_type=V20_TYPE;
			nec_state->prefetch_size = 4;		/* 3 words */
			nec_state->prefetch_cycles = 4;		/* four cycles per byte */
		}
		break;

		case V30_TYPE: // v35
		{
			nec_state->fetch_xor = 0;
			nec_state->chip_type=V30_TYPE;
			nec_state->prefetch_size = 6;		/* 3 words */
			nec_state->prefetch_cycles = 2;		/* two cycles per byte / four per word */
		}
		break;
	}
}

void v25RunEnd()
{
	sChipsPtr->stop_run = 1;
}

void v25Idle(int cycles)
{
	sChipsPtr->cycles_total += cycles;
}

int v25GetPC(int n)
{
	if (n == -1) { // current cpu
		return ((sChipsPtr->ram.w[sChipsPtr->RBW + (PS)]<<4) + sChipsPtr->ip);
	} else {
		v25_state_t *nec_state = &sChips[n];
		return ((nec_state->ram.w[nec_state->RBW + (PS)]<<4) + nec_state->ip);
	}
}

void v25Scan(int cpu, int nAction)
{
	v25_state_t *nec_state = &sChips[cpu];

	if (nAction & ACB_DRIVER_DATA) {
		rei_tim_sync(nec_state); // ReiREM port: lazy timers -> real values in the state (and after a load)
		v25_auto_epoch++;
		struct BurnArea ba;
		char szText[] = "V25 #0";
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

static CPU_SET_INFO( v25 )
{
	v25_state_t *nec_state = get_safe_token(device);

	switch (state)
	{
		/* --- the following bits of info are set as 64-bit signed integers --- */
		case CPUINFO_INT_INPUT_STATE + 0:						set_irq_line(nec_state, 0, info->i);				break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:			set_irq_line(nec_state, INPUT_LINE_NMI, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP0:	set_irq_line(nec_state, NEC_INPUT_LINE_INTP0, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP1:	set_irq_line(nec_state, NEC_INPUT_LINE_INTP1, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP2:	set_irq_line(nec_state, NEC_INPUT_LINE_INTP2, info->i);	break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_POLL:		set_irq_line(nec_state, NEC_INPUT_LINE_POLL, info->i);	break;

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

static CPU_GET_INFO( v25v35 )
{
	v25_state_t *nec_state = (device != NULL && device->token() != NULL) ? get_safe_token(device) : NULL;
	int flags;

	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CONTEXT_SIZE:					info->i = sizeof(v25_state_t);					break;
		case CPUINFO_INT_INPUT_LINES:					info->i = 1;							break;
		case CPUINFO_INT_DEFAULT_IRQ_VECTOR:			info->i = 0xff;							break;
		case DEVINFO_INT_ENDIANNESS:					info->i = ENDIANNESS_LITTLE;					break;
		case CPUINFO_INT_CLOCK_MULTIPLIER:				info->i = 1;							break;
		case CPUINFO_INT_CLOCK_DIVIDER:					info->i = 2;							break;
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
		case DEVINFO_INT_ADDRBUS_WIDTH + AS_IO:		info->i = 17;					break;
		case DEVINFO_INT_ADDRBUS_SHIFT + AS_IO:		info->i = 0;					break;

		case CPUINFO_INT_INPUT_STATE + 0:						info->i = (nec_state->pending_irq & INT_IRQ) ? ASSERT_LINE : CLEAR_LINE; break;
		case CPUINFO_INT_INPUT_STATE + INPUT_LINE_NMI:			info->i = nec_state->nmi_state;				break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP0:	info->i = nec_state->intp_state[0];			break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP1:	info->i = nec_state->intp_state[1];			break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_INTP2:	info->i = nec_state->intp_state[2];			break;
		case CPUINFO_INT_INPUT_STATE + NEC_INPUT_LINE_POLL:		info->i = nec_state->poll_state;			break;

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
		case CPUINFO_FCT_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(v25);			break;
		case CPUINFO_FCT_INIT:							/* set per-CPU */						break;
		case CPUINFO_FCT_RESET:							info->reset = CPU_RESET_NAME(v25);				break;
		case CPUINFO_FCT_EXIT:							info->exit = CPU_EXIT_NAME(v25);					break;
		case CPUINFO_FCT_EXECUTE:						info->execute = CPU_EXECUTE_NAME(v25);			break;
		case CPUINFO_FCT_BURN:							info->burn = NULL;						break;
		case CPUINFO_FCT_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(v25);			break;
		case CPUINFO_PTR_INSTRUCTION_COUNTER:			info->icount = &nec_state->icount;				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "NEC");					break;
		case DEVINFO_STR_FAMILY:					strcpy(info->s, "NEC V-Series");		break;
		case DEVINFO_STR_VERSION:					strcpy(info->s, "2.0");					break;
		case DEVINFO_STR_SOURCE_FILE:						strcpy(info->s, __FILE__);				break;
		case DEVINFO_STR_CREDITS:					strcpy(info->s, "Bryan McPhail (V25/V35 support added by Alex W. Jackson)"); break;

		case CPUINFO_STR_FLAGS:
            flags = CompressFlags();
            sprintf(info->s, "%c %d %c%c%c%c%c%c%c%c%c%c%c%c",
                flags & 0x8000 ? 'N':'S',
                (flags & 0x7000) >> 12,
                flags & 0x0800 ? 'O':'.',
                flags & 0x0400 ? 'D':'.',
                flags & 0x0200 ? 'I':'.',
                flags & 0x0100 ? 'T':'.',
                flags & 0x0080 ? 'S':'.',
                flags & 0x0040 ? 'Z':'.',
                flags & 0x0020 ? '1':'.',
                flags & 0x0010 ? 'A':'.',
                flags & 0x0008 ? '0':'.',
                flags & 0x0004 ? 'P':'.',
                flags & 0x0002 ? '.':'I',
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

CPU_GET_INFO( v25 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case DEVINFO_INT_DATABUS_WIDTH + AS_PROGRAM:	info->i = 8;					break;
		case DEVINFO_INT_DATABUS_WIDTH + AS_IO:		info->i = 8;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(v25);					break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "V25");					break;

		default:										CPU_GET_INFO_CALL(v25v35);				break;
	}
}


/**************************************************************************
 * CPU-specific set_info
 **************************************************************************/

CPU_GET_INFO( v35 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case DEVINFO_INT_DATABUS_WIDTH + AS_PROGRAM:	info->i = 16;					break;
		case DEVINFO_INT_DATABUS_WIDTH + AS_IO:		info->i = 16;					break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(v35);					break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "V35");					break;

		default:										CPU_GET_INFO_CALL(v25v35);				break;
	}
}

DEFINE_LEGACY_CPU_DEVICE(V25, v25);
DEFINE_LEGACY_CPU_DEVICE(V35, v35);

#endif

