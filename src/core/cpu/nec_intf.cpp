// Nec V20/V30/V33/V25/V35 interface
// Written by OopsWare
// http://oopsware.googlepages.com
// Heavily modified by iq_132 (Nov, 2011), dink (2023)

#include "burnint.h"
#include "nec_intf.h"
#include "rei_line.h" // ReiREM PSP port: per-line code block

#define MAX_VEZ		4

//----------------------------------------------------------------------------------
// nec.cpp
void necInit(INT32 cpu, INT32 type);
void necCpuOpen(INT32 cpu);
void necCpuClose();
INT32 nec_reset();
INT32 nec_execute(INT32 cycles);
void nec_set_irq_line_and_vector(INT32 irqline, INT32 vector, INT32 state);
UINT32 nec_total_cycles();
void nec_new_frame();
INT32 necGetPC(INT32 n);
void necScan(INT32 cpu, INT32 nAction);
void necRunEnd();
void necIdle(INT32 cycles);

// v25.cpp
INT32 v25_reset();
void v25_open(INT32 cpu);
void v25_close();
void v25_set_irq_line_and_vector(INT32 irqline, INT32 vector, INT32 state);
INT32 v25_execute(INT32 cycles);
void v25Init(INT32 cpu, INT32 type, INT32 clock);
void v25_set_decode(UINT8 *table);
UINT32 v25_total_cycles();
void v25_new_frame();
INT32 v25GetPC(INT32 n);
void v25Scan(INT32 cpu, INT32 nAction);
void v25RunEnd();
void v25Idle(INT32 cycles);

//----------------------------------------------------------------------------------

struct VezContext {
	void (*cpu_open)(INT32);
	void (*cpu_close)();
	INT32 (*cpu_reset)();
	INT32 (*cpu_execute)(INT32);
	void (*cpu_set_irq_line)(INT32, INT32, INT32);
	void (*decode)(UINT8*);
	UINT32 (*total_cycles)();
	INT32 (*get_pc)(INT32);
	void (*scan)(INT32, INT32);
	void (*runend)();
	void (*idle)(INT32);

	UINT8 * ppMemRead[2048];
	UINT8 * ppMemWrite[2048];
	UINT8 * ppMemFetch[2048];
	UINT8 * ppMemFetchData[2048];

	INT32 reset;
	INT32 halt;

	// Handlers
 #ifdef FASTCALL
	UINT8 (__fastcall *ReadHandler)(UINT32 a);
	void (__fastcall *WriteHandler)(UINT32 a, UINT8 d);
	UINT8 (__fastcall *ReadPort)(UINT32 a);
	void (__fastcall *WritePort)(UINT32 a, UINT8 d);
 #else
	UINT8 (__cdecl *ReadHandler)(UINT32 a);
	void (__cdecl *WriteHandler)(UINT32 a, UINT8 d);
	UINT8 (__cdecl *ReadPort)(UINT32 a);
	void (__cdecl *WritePort)(UINT32 a, UINT8 d);
 #endif
};

static struct VezContext *VezCPUContext[MAX_VEZ] = { NULL, NULL, NULL, NULL };
#include <stdint.h>
// ReiREM Vita port: the V33 (main thread) and V35 (sound thread) run concurrently in pipelined
// mode, so the "currently open CPU" state of the Vez API is kept per thread. GCC __thread is
// emulated (emutls) on VitaSDK and fails in threads made by sceKernelCreateThread, so threads are
// told apart by the ARM user thread register (TPIDRURO), one instruction to read.
struct vezpstack {
	INT32 nHostCPU;
	INT32 nPushedCPU;
};
#define MAX_PSTACK 10

struct rei_vez_thread {
	uintptr_t key;                 // thread identity, 0 = free slot
	struct VezContext *cur;        // VezCurrentCPU
	INT32 opened;                  // nOpenedCPU
	INT32 pstacknum;
	struct vezpstack pstack[MAX_PSTACK];
} __attribute__((aligned(64))); // one cache line set per thread: the PSP's Media Engine does not share caches
#define REI_VEZ_THREADS 4
static struct rei_vez_thread rei_vez_threads[REI_VEZ_THREADS];

#if defined(__PSP__)
extern "C" int sceKernelGetThreadId(void);
#endif

static inline uintptr_t rei_thread_key()
{
#if defined(__vita__) || (defined(__arm__) && !defined(__linux__))
	uintptr_t v;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
	return v;
#elif defined(__PSP__)
	// PSP port: only the main thread drives the V33/V35 on the main CPU, and the Media Engine runs
	// on a kernel-segment stack (>= 0x80000000), so the stack pointer's top bit is enough. A
	// sceKernelGetThreadId() syscall here costs hundreds of cycles per Vez* call.
	uintptr_t sp;
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	return 1 + (sp >> 31);
#else
	static __thread char marker; // native TLS on the PC harness
	return (uintptr_t)&marker;
#endif
}

static struct rei_vez_thread *rei_vez_slot()
{
	uintptr_t k = rei_thread_key();
#if defined(__PSP__)
	// PSP: key 1 = main CPU, 2 = Media Engine. Fixed slots - the two CPUs must not race on the keys
	// (no shared atomics, separate caches); each slot is only ever touched by its own CPU.
	struct rei_vez_thread *t = &rei_vez_threads[k - 1];
	if (t->key != k) {
		t->key = k;
		t->cur = 0;
		t->opened = -1;
		t->pstacknum = 0;
	}
	return t;
#endif
	for (INT32 i = 0; i < REI_VEZ_THREADS; i++)
		if (__atomic_load_n(&rei_vez_threads[i].key, __ATOMIC_ACQUIRE) == k)
			return &rei_vez_threads[i];
	for (INT32 i = 0; i < REI_VEZ_THREADS; i++) {
		uintptr_t expected = 0;
		if (__atomic_compare_exchange_n(&rei_vez_threads[i].key, &expected, k, false,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			rei_vez_threads[i].cur = 0;
			rei_vez_threads[i].opened = -1;
			rei_vez_threads[i].pstacknum = 0;
			return &rei_vez_threads[i];
		}
	}
	return &rei_vez_threads[0]; // more threads than expected: share slot 0
}

#define VezCurrentCPU (rei_vez_slot()->cur)
#define nOpenedCPU    (rei_vez_slot()->opened)
#define pstack        (rei_vez_slot()->pstack)
#define pstacknum     (rei_vez_slot()->pstacknum)

// per-core contexts for the memory/port accessors (one V33 and one V35 in this driver)
static struct VezContext *rei_ctx_nec = 0, *rei_ctx_v25 = 0;
extern "C" int rei_nec_present(void) { return rei_ctx_nec != 0; } // (ReiREM) a V30/V33 runs in this game (nec.cpp)

#define VEZ_MEM_SHIFT	9
#define VEZ_MEM_MASK	((1 << VEZ_MEM_SHIFT) - 1)

static INT32 nCPUCount = 0;
INT32 nVezCount;

UINT8 __fastcall VezDummyReadHandler(UINT32) { return 0; }
void __fastcall VezDummyWriteHandler(UINT32, UINT8) { }
UINT8 __fastcall VezDummyReadPort(UINT32) { return 0; }
void __fastcall VezDummyWritePort(UINT32, UINT8) { }

// ReiREM Vita port: memory/port accessors bound to a fixed per-core context instead of the
// (now per-thread) VezCurrentCPU. cpu_* = V33 (nec core), v25_cpu_* = V35 (v25 core, see v25.cpp).
#define REI_VEZ_ACCESSORS(PFX, CTX)                                     \
UINT8 PFX##readport(UINT32 p)                                           \
{                                                                       \
	return CTX->ReadPort(p);                                            \
}                                                                       \
                                                                        \
void PFX##writeport(UINT32 p,UINT32 d)                                  \
{                                                                       \
	CTX->WritePort(p, d);                                               \
}                                                                       \
                                                                        \
UINT8 PFX##readmem20(UINT32 a)                                          \
{                                                                       \
	a &= 0xFFFFF;                                                       \
	UINT8 * p = CTX->ppMemRead[ a >> VEZ_MEM_SHIFT ];                   \
	if ( p )                                                            \
		return *(p + a);                                                \
	else                                                                \
		return CTX->ReadHandler(a);                                     \
}                                                                       \
                                                                        \
UINT8 PFX##readmem20_op(UINT32 a)                                       \
{                                                                       \
	a &= 0xFFFFF;                                                       \
	UINT8 * p = CTX->ppMemFetch[ a >> VEZ_MEM_SHIFT ];                  \
	if ( p )                                                            \
		return *(p + a);                                                \
	else                                                                \
		return CTX->ReadHandler(a);                                     \
}                                                                       \
                                                                        \
UINT8 PFX##readmem20_arg(UINT32 a)                                      \
{                                                                       \
	a &= 0xFFFFF;                                                       \
	UINT8 * p = CTX->ppMemFetchData[ a >> VEZ_MEM_SHIFT ];              \
	if ( p )                                                            \
		return *(p + a);                                                \
	else                                                                \
		return CTX->ReadHandler(a);                                     \
}                                                                       \
                                                                        \
void PFX##writemem20(UINT32 a, UINT8 d)                                 \
{                                                                       \
	a &= 0xFFFFF;                                                       \
	UINT8 * p = CTX->ppMemWrite[ a >> VEZ_MEM_SHIFT ];                  \
	if ( p )                                                            \
		*(p + a) = d;                                                   \
	else                                                                \
		CTX->WriteHandler(a, d);                                        \
}

REI_VEZ_ACCESSORS(cpu_, rei_ctx_nec)

// ReiREM Vita port: V33 page tables for the inlined accessors in nec.cpp
UINT8 **rei_nec_pg_rd, **rei_nec_pg_wr, **rei_nec_pg_op, **rei_nec_pg_arg;
#if defined(__psp__) || defined(REI_FETCH_CACHE)
// ReiREM PSP port: nec.cpp keeps the page of the last V33 instruction fetch; clear it when V33 fetch pages change
extern "C" void rei_nec_fetch_flush(void);
extern "C" void rei_v25_fetch_flush(void);
#define REI_FETCH_FLUSH(ctx) do { if ((ctx) == rei_ctx_nec) rei_nec_fetch_flush(); else if ((ctx) == rei_ctx_v25) rei_v25_fetch_flush(); } while (0)
#else
#define REI_FETCH_FLUSH(ctx) ((void)0)
#endif
UINT8 (__fastcall **rei_nec_rh)(UINT32);
void (__fastcall **rei_nec_wh)(UINT32, UINT8);
REI_VEZ_ACCESSORS(v25_cpu_, rei_ctx_v25)

// ReiREM PSP port: V35 page tables for the inlined accessors in v25.cpp (as for the V33)
UINT8 **rei_v25_pg_rd, **rei_v25_pg_wr, **rei_v25_pg_op, **rei_v25_pg_arg;
UINT8 (__fastcall **rei_v25_rh)(UINT32);
void (__fastcall **rei_v25_wh)(UINT32, UINT8);

void VezWriteByte(UINT32 a, UINT8 d)
{
	a &= 0xFFFFF;
	
	UINT8 * p = VezCurrentCPU->ppMemWrite[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		*(p + a) = d;
	else
		VezCurrentCPU->WriteHandler(a, d);
}

void VezWriteWord(UINT32 a, UINT16 d)
{
	a &= 0xFFFFF;
	
	UINT16 * p = (UINT16*)VezCurrentCPU->ppMemWrite[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		*(p + (a / 2)) = BURN_ENDIAN_SWAP_INT16(d);
	else {
		VezCurrentCPU->WriteHandler(a, d);
		VezCurrentCPU->WriteHandler(a+1, d >> 8);
	}
}

void VezWriteLong(UINT32 a, UINT32 d)
{
	a &= 0xFFFFF;
	
	UINT32 * p = (UINT32*)VezCurrentCPU->ppMemWrite[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		*(p + (a / 4)) = BURN_ENDIAN_SWAP_INT32(d);
	else {
		VezCurrentCPU->WriteHandler(a, d);
		VezCurrentCPU->WriteHandler(a+1, d >> 8);
		VezCurrentCPU->WriteHandler(a+2, d >> 16);
		VezCurrentCPU->WriteHandler(a+3, d >> 24);

	}
}

UINT8 VezReadByte(UINT32 a)
{
	a &= 0xFFFFF;
	
	UINT8 * p = VezCurrentCPU->ppMemRead[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		return *(p + a);
	else
		return VezCurrentCPU->ReadHandler(a);
}

UINT16 VezReadWord(UINT32 a)
{
	a &= 0xFFFFF;
	
	UINT16 * p = (UINT16*)VezCurrentCPU->ppMemRead[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		return BURN_ENDIAN_SWAP_INT16(*(p + (a / 2)));
	else
		return VezCurrentCPU->ReadHandler(a) + (VezCurrentCPU->ReadHandler(a+1) * 0x100);
}

UINT32 VezReadLong(UINT32 a)
{
	a &= 0xFFFFF;
	
	UINT32 * p = (UINT32*)VezCurrentCPU->ppMemRead[ a >> VEZ_MEM_SHIFT ];
	if ( p )
		return BURN_ENDIAN_SWAP_INT32(*(p + (a / 4)));
	else
		return VezCurrentCPU->ReadHandler(a) + (VezCurrentCPU->ReadHandler(a+1) << 8) +
			(VezCurrentCPU->ReadHandler(a+2) << 16) + (VezCurrentCPU->ReadHandler(a+3) << 24);
}

void VezCheatWrite(UINT32 a, UINT8 d)
{
	a &= 0xfffff;

	UINT8 * p;

	p = VezCurrentCPU->ppMemWrite[ a >> VEZ_MEM_SHIFT ];
	if ( p ) *(p + a) = d;

	p = VezCurrentCPU->ppMemRead[ a >> VEZ_MEM_SHIFT ];
	if ( p ) *(p + a) = d;

	p = VezCurrentCPU->ppMemFetch[ a >> VEZ_MEM_SHIFT ];
	if ( p ) *(p + a) = d;

	p = VezCurrentCPU->ppMemFetchData[ a >> VEZ_MEM_SHIFT ];
	if ( p ) *(p + a) = d;

	VezCurrentCPU->WriteHandler(a, d);
}

void VezSetReadHandler(UINT8 (__fastcall *pHandler)(UINT32))
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetReadHandler called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetReadHandler called when no CPU open\n"));
#endif

	VezCurrentCPU->ReadHandler = pHandler;
}

void VezSetWriteHandler(void (__fastcall *pHandler)(UINT32, UINT8))
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetWriteHandler called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetWriteHandler called when no CPU open\n"));
#endif

	VezCurrentCPU->WriteHandler = pHandler;
}

void VezSetReadPort(UINT8 (__fastcall *pHandler)(UINT32))
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetReadPort called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetReadPort called when no CPU open\n"));
#endif

	VezCurrentCPU->ReadPort = pHandler;
}

void VezSetWritePort(void (__fastcall *pHandler)(UINT32, UINT8))
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetWritePort called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetWritePort called when no CPU open\n"));
#endif

	VezCurrentCPU->WritePort = pHandler;
}

void VezSetDecode(UINT8 *table)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetDecode called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetDcode called when no CPU open\n"));
#endif

	if (VezCurrentCPU->decode) {
		VezCurrentCPU->decode(table);
	}
}

// ## VezCPUPush() / VezCPUPop() ## internal helpers for sending signals to other vez's
// (ReiREM Vita port: vezpstack / pstack / pstacknum moved to the per-thread slot above)

void VezCPUPush(INT32 nCPU)
{
	vezpstack *p = &pstack[pstacknum++];

	if (pstacknum + 1 >= MAX_PSTACK) {
		bprintf(0, _T("VezCPUPush(): out of stack!  Possible infinite recursion?  Crash pending..\n"));
	}

	p->nPushedCPU = nCPU;

	p->nHostCPU = VezGetActive();

	if (p->nHostCPU != p->nPushedCPU) {
		if (p->nHostCPU != -1) VezClose();
		VezOpen(p->nPushedCPU);
	}
}

void VezCPUPop()
{
	vezpstack *p = &pstack[--pstacknum];

	if (p->nHostCPU != p->nPushedCPU) {
		VezClose();
		if (p->nHostCPU != -1) VezOpen(p->nHostCPU);
	}
}

static void core_set_irq(INT32 cpu, INT32 line, INT32 state)
{
	VezCPUPush(cpu);

	VezCurrentCPU->cpu_set_irq_line(line & 0xffff, line >> 16, state);

	VezCPUPop();
}

cpu_core_config VezConfig =
{
	"nec v20-v35",
	VezCPUPush,
	VezCPUPop,
	cpu_readmem20,
	VezCheatWrite,
	VezGetActive,
	VezTotalCycles,
	VezNewFrame,
	VezIdle,
	core_set_irq,
	VezRun,
	VezRunEnd,
	VezReset,
	VezScan,
	VezExit,
	0x100000,
	0
};

INT32 VezInit(INT32 cpu, INT32 type, INT32 clock)
{
	DebugCPU_VezInitted = 1;
	
	if (cpu >= MAX_VEZ) {
		bprintf (0, _T("Only %d Vez available! Increase MAX_VEZ in vez.cpp.\n"), MAX_VEZ);
	}

	VezCPUContext[cpu] = (VezContext*)BurnMalloc(sizeof(VezContext));

	VezCurrentCPU = VezCPUContext[cpu];

	memset(VezCurrentCPU, 0, sizeof(struct VezContext));

	switch (type)
	{
		case i86_TYPE:
		case V20_TYPE:
		case V30_TYPE:
		case V33_TYPE:
		{
			necInit(cpu, type);

			rei_ctx_nec = VezCurrentCPU; // ReiREM Vita port
			rei_nec_pg_rd = VezCurrentCPU->ppMemRead;
			rei_nec_pg_wr = VezCurrentCPU->ppMemWrite;
			rei_nec_pg_op = VezCurrentCPU->ppMemFetch;
			rei_nec_pg_arg = VezCurrentCPU->ppMemFetchData;
			rei_nec_rh = &VezCurrentCPU->ReadHandler;
			rei_nec_wh = &VezCurrentCPU->WriteHandler;
			VezCurrentCPU->cpu_open = necCpuOpen;
			VezCurrentCPU->cpu_close = necCpuClose;
			VezCurrentCPU->cpu_reset = nec_reset;
			VezCurrentCPU->cpu_execute = nec_execute;
			VezCurrentCPU->cpu_set_irq_line = nec_set_irq_line_and_vector;
			VezCurrentCPU->decode = NULL; // ?
			VezCurrentCPU->total_cycles = nec_total_cycles;
			VezCurrentCPU->get_pc = necGetPC;
			VezCurrentCPU->scan = necScan;
			VezCurrentCPU->runend = necRunEnd;
			VezCurrentCPU->idle = necIdle;
		}
		break;

		case V25_TYPE:
		case V35_TYPE:
		{
			v25Init(cpu, type&0xff, clock);

			rei_ctx_v25 = VezCurrentCPU; // ReiREM Vita port
			rei_v25_pg_rd = VezCurrentCPU->ppMemRead;
			rei_v25_pg_wr = VezCurrentCPU->ppMemWrite;
			rei_v25_pg_op = VezCurrentCPU->ppMemFetch;
			rei_v25_pg_arg = VezCurrentCPU->ppMemFetchData;
			rei_v25_rh = &VezCurrentCPU->ReadHandler;
			rei_v25_wh = &VezCurrentCPU->WriteHandler;
			VezCurrentCPU->cpu_open = v25_open;
			VezCurrentCPU->cpu_close = v25_close;
			VezCurrentCPU->cpu_reset = v25_reset;
			VezCurrentCPU->cpu_execute = v25_execute;
			VezCurrentCPU->cpu_set_irq_line = v25_set_irq_line_and_vector;
			VezCurrentCPU->decode = v25_set_decode;
			VezCurrentCPU->total_cycles = v25_total_cycles;
			VezCurrentCPU->get_pc = v25GetPC;
			VezCurrentCPU->scan = v25Scan;
			VezCurrentCPU->runend = v25RunEnd;
			VezCurrentCPU->idle = v25Idle;

		}
		break;
	}
		
	VezCurrentCPU->ReadHandler = VezDummyReadHandler;
	VezCurrentCPU->WriteHandler = VezDummyWriteHandler;
	VezCurrentCPU->ReadPort = VezDummyReadPort;
	VezCurrentCPU->WritePort = VezDummyWritePort;

	INT32 nCount = nVezCount+1;

	nVezCount = nCPUCount = nCount;

	CpuCheatRegister(cpu, &VezConfig);

	return 0;
}

INT32 VezInit(INT32 cpu, INT32 type)
{
	return VezInit(cpu, type, 0);
}

#if defined(REI_DRC) && (defined(__mips__) || defined(REI_DRC_TEST))
extern "C" void rei_drc_exit(void);   // nec.cpp (V33/V30 dynarec)
extern "C" void rei_drc35_exit(void); // v25.cpp (V35 dynarec)
#endif
void VezExit()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezExit called without init\n"));
#endif

	for (INT32 i = 0; i < MAX_VEZ; i++) {
		if (VezCPUContext[i]) {
			BurnFree(VezCPUContext[i]);
			VezCPUContext[i] = NULL;
		}
	}

	nCPUCount = 0;
	nOpenedCPU = -1;
	nVezCount = 0;
	rei_ctx_nec = rei_ctx_v25 = 0; // (ReiREM)
#if defined(REI_DRC) && (defined(__mips__) || defined(REI_DRC_TEST))
	// (ReiREM) V33/V30 and V35 dynarec code caches back to nothing: the next game in the same process starts clean
	rei_drc_exit();
	rei_drc35_exit();
#endif
#if !defined(__PSP__) // (PSP: fixed slots per CPU, one game per process)
	// (ReiREM) free the per-thread slots: this game's threads have stopped, and the next game in the same process
	// (in-app game switching) runs on new threads (loader, sound) that each need a slot
	for (INT32 i = 0; i < REI_VEZ_THREADS; i++)
		__atomic_store_n(&rei_vez_threads[i].key, (uintptr_t)0, __ATOMIC_RELEASE);
#endif

	nOpenedCPU = -1;
	
	DebugCPU_VezInitted = 0;
}

REI_LINE_FN(41) void VezOpen(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezOpen called without init\n"));
	if (nCPU > nCPUCount) bprintf(PRINT_ERROR, _T("VezOpen called with invalid index %x\n"), nCPU);
	if (nOpenedCPU != -1) bprintf(PRINT_ERROR, _T("VezOpen called when CPU already open with index %x\n"), nCPU);
#endif

	if (nCPU >= MAX_VEZ || nCPU < 0) nCPU = 0;

	nOpenedCPU = nCPU;
	VezCurrentCPU = VezCPUContext[nCPU];
	VezCurrentCPU->cpu_open(nCPU);
}

REI_LINE_FN(42) void VezClose()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezClose called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezClose called when no CPU open\n"));
#endif

	nOpenedCPU = -1;
	VezCurrentCPU->cpu_close();
	VezCurrentCPU = 0;
}

void VezNewFrame()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezNewFrame called without init\n"));
#endif

	// should be separated?
	v25_new_frame();
	nec_new_frame();
}

void VezRunEnd()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezRunEnd called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezRunEnd called when no CPU open\n"));
#endif

	VezCurrentCPU->runend();
}

void VezRunEnd(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezRunEnd called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezRunEnd called when no CPU open\n"));
#endif

	VezCPUPush(nCPU);

	VezRunEnd();

	VezCPUPop();
}

INT32 VezIdle(INT32 cycles)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezIdle called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezIdle called when no CPU open\n"));
#endif

	VezCurrentCPU->idle(cycles);

	return cycles;
}

INT32 VezIdle(INT32 nCPU, INT32 nCycles)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezIdle called without init\n"));
#endif

	VezCPUPush(nCPU);

	INT32 nRet = VezIdle(nCycles);

	VezCPUPop();

	return nRet;
}

REI_LINE_FN(43) INT32 VezTotalCycles()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezTotalCycles called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezTotalCycles called when no CPU open\n"));
#endif

	return VezCurrentCPU->total_cycles();
}

INT32 VezTotalCycles(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezTotalCycles called without init\n"));
#endif

	VezCPUPush(nCPU);

	INT32 nRet = VezTotalCycles();

	VezCPUPop();

	return nRet;
}

INT32 VezGetActive()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetActive called without init\n"));
#endif

	return nOpenedCPU;
}

INT32 VezMemCallback(INT32 nStart,INT32 nEnd,INT32 nMode)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezMemCallback called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezMemCallback called when no CPU open\n"));
#endif

	nStart >>= VEZ_MEM_SHIFT;
	nEnd += VEZ_MEM_MASK;
	nEnd >>= VEZ_MEM_SHIFT;

	for (INT32 i = nStart; i < nEnd; i++) {
		switch (nMode) {
			case 0:
				VezCurrentCPU->ppMemRead[i] = NULL;
				break;
			case 1:
				VezCurrentCPU->ppMemWrite[i] = NULL;
				break;
			case 2:
				VezCurrentCPU->ppMemFetch[i] = NULL;
				VezCurrentCPU->ppMemFetchData[i] = NULL;
				break;
		}
	}
	REI_FETCH_FLUSH(VezCurrentCPU);
	return 0;
}

// ReiREM PSP port: copy read-only V33 pages (code/ROM: fetched, read, not writable) into buf (512 bytes each, in
// the order given) and point the page tables there. On the PSP buf is the 16KB scratchpad, so the hottest code pages
// stop missing the data cache. Pages that are writable or not plainly mapped are skipped. Returns pages mapped.
extern "C" int rei_vez_v33_page_cache(unsigned char *buf, int size, const unsigned int *pages, int n)
{
	struct VezContext *c = rei_ctx_nec;
	if (!c)
		return 0;
	int used = 0;
	for (int i = 0; i < n && (used + 1) << VEZ_MEM_SHIFT <= size; i++) {
		const unsigned int pa = pages[i] & ~(unsigned int)VEZ_MEM_MASK, k = pa >> VEZ_MEM_SHIFT;
		if (k >= (1u << (20 - VEZ_MEM_SHIFT)))
			continue;
		UINT8 *f = c->ppMemFetch[k];
		if (!f || c->ppMemWrite[k] || c->ppMemFetchData[k] != f || (c->ppMemRead[k] && c->ppMemRead[k] != f))
			continue;
		UINT8 *slot = buf + (used << VEZ_MEM_SHIFT);
		memcpy(slot, f + pa, 1 << VEZ_MEM_SHIFT);
		UINT8 *p = slot - pa;
		c->ppMemFetch[k] = c->ppMemFetchData[k] = p;
		if (c->ppMemRead[k])
			c->ppMemRead[k] = p;
		used++;
	}
	REI_FETCH_FLUSH(c);
	return used;
}

INT32 VezMapArea(INT32 nStart, INT32 nEnd, INT32 nMode, UINT8 *Mem)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezMapArea called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezMapArea called when no CPU open\n"));
#endif

	INT32 s = nStart >> VEZ_MEM_SHIFT;
	INT32 e = (nEnd + VEZ_MEM_MASK) >> VEZ_MEM_SHIFT;
	// (dynarec word fast path reads/writes even addresses as halfwords: odd-address buffers aren't mapped and go to handlers; no current driver has them)
	if (((uintptr_t)Mem - (uintptr_t)nStart) & 1) {
		fprintf(stderr, "VezMapArea: odd buffer %x-%x mode %d (unmapped)\n", nStart, nEnd, nMode);
		return 1;
	}

	for (INT32 i = s; i < e; i++) {
		switch (nMode) {
			case 0:
				VezCurrentCPU->ppMemRead[i] = Mem - nStart;
				break;
			case 1:
				VezCurrentCPU->ppMemWrite[i] = Mem - nStart;
				break;
			case 2:
				VezCurrentCPU->ppMemFetch[i] = Mem - nStart;
				VezCurrentCPU->ppMemFetchData[i] = Mem - nStart;
				break;
		}
	}
	REI_FETCH_FLUSH(VezCurrentCPU);

	return 0;
}

INT32 VezMapArea(INT32 nStart, INT32 nEnd, INT32 nMode, UINT8 *Mem1, UINT8 *Mem2)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezMapArea called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezMapArea called when no CPU open\n"));
#endif

	INT32 s = nStart >> VEZ_MEM_SHIFT;
	INT32 e = (nEnd + VEZ_MEM_MASK) >> VEZ_MEM_SHIFT;
	
	if (nMode != 2) return 1;
	
	for (INT32 i = s; i < e; i++) {
		VezCurrentCPU->ppMemFetch[i] = Mem1 - nStart;
		VezCurrentCPU->ppMemFetchData[i] = Mem2 - nStart;
	}
	REI_FETCH_FLUSH(VezCurrentCPU);

	return 0;
}

INT32 VezMapMemory(UINT8 *Mem, INT32 nStart, INT32 nEnd, INT32 nMode)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezMapMemory called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezMapMemory called when no CPU open\n"));
#endif

	for (INT32 i = 0; i < 3; i++) {
		if (nMode & (1 << i)) {
			VezMapArea(nStart, nEnd, i, Mem);
		}
	}

	return 0;
}

void VezReset()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezReset called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezReset called when no CPU open\n"));
#endif

	VezCurrentCPU->cpu_reset();
}

void VezReset(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezReset called without init\n"));
#endif

	VezCPUPush(nCPU);

	VezReset();

	VezCPUPop();
}

void VezSetRESETLine(INT32 nStatus)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetRESETLine called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetRESETLine called when no CPU open\n"));
#endif

	if (VezCurrentCPU->reset && nStatus == 0) {
		VezCurrentCPU->cpu_reset();
	}

	VezCurrentCPU->reset = nStatus;
}

void VezSetRESETLine(INT32 nCPU, INT32 nStatus)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetRESETLine called without init\n"));
#endif

	VezCPUPush(nCPU);

	VezSetRESETLine(nStatus);

	VezCPUPop();
}

void VezSetHALT(INT32 nStatus)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetHALT called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetHALT called when no CPU open\n"));
#endif

	VezCurrentCPU->halt = nStatus;
	if (nStatus) VezRunEnd(); // end timeslice
}

void VezSetHALT(INT32 nCPU, INT32 nStatus)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetHALT called without init\n"));
#endif

	VezCPUPush(nCPU);

	VezSetHALT(nStatus);

	VezCPUPop();
}

INT32 VezGetHALT()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetHALT called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezGetHALT called when no CPU open\n"));
#endif

	return VezCurrentCPU->halt;
}

INT32 VezGetHALT(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetHALT called without init\n"));
#endif

	VezCPUPush(nCPU);

	INT32 nRet = VezGetHALT();

	VezCPUPop();

	return nRet;
}

INT32 VezGetRESETLine()
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetRESETLine called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezGetRESETLine called when no CPU open\n"));
#endif

	return VezCurrentCPU->reset;
}

INT32 VezGetRESETLine(INT32 nCPU)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetRESETLine called without init\n"));
#endif

	VezCPUPush(nCPU);

	INT32 nRet = VezGetRESETLine();

	VezCPUPop();

	return nRet;
}

#include "rei_prof.h"

unsigned long long rei_vez_runs[4]; // (diag) VezRun call count per CPU (printed by harness REI_V25SKIP)
REI_LINE_FN(40) INT32 VezRun(INT32 nCycles)
{
	rei_vez_runs[nOpenedCPU & 3]++;
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezRun called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezRun called when no CPU open\n"));
#endif

	if (nCycles <= 0) return 0;
	if (VezCurrentCPU->reset || VezCurrentCPU->halt) {
		VezCurrentCPU->idle(nCycles); // doesn't return cycles...
		return nCycles;
	} else {
		REI_PROF_CPU_BEGIN(nec);
		const INT32 r = VezCurrentCPU->cpu_execute(nCycles);
		// (profile: the second Vez CPU (M107/M92 sound V35) goes in the "z80" slot, to separate it from V33 on boards without a Z80)
		REI_PROF_CPU_END(nec, nOpenedCPU == 1 ? REI_PROF_Z80 : REI_PROF_NEC);
		return r;
	}
}

INT32 VezRun(INT32 nCPU, INT32 nCycles)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezRun called without init\n"));
#endif

	VezCPUPush(nCPU);

	INT32 nRet = VezRun(nCycles);

	VezCPUPop();

	return nRet;
}

UINT32 VezGetPC(INT32 n)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezGetPc called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezGetPc called when no CPU open\n"));
#endif

	if (n == -1) {
		return VezCurrentCPU->get_pc(-1);
	} else {
		if (n >= MAX_VEZ) return 0;
		struct VezContext *CPU = VezCPUContext[n];
		return CPU->get_pc(n);
	}

	return 0;
}

INT32 VezScan(INT32 nAction)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezScan called without init\n"));
#endif

	if ((nAction & ACB_DRIVER_DATA) == 0)
		return 0;

	for (INT32 i = 0; i < nCPUCount; i++) {
		struct VezContext *CPU = VezCPUContext[i];
		if (CPU->scan) {
			CPU->scan(i, nAction);

			SCAN_VAR(CPU->reset);
			SCAN_VAR(CPU->halt);
		}
	}
	
	return 0;
}

void VezSetIRQLineAndVector(const INT32 line, const INT32 vector, const INT32 status)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetIRQLineAndVector called without init\n"));
	if (nOpenedCPU == -1) bprintf(PRINT_ERROR, _T("VezSetIRQLineAndVector called when no CPU open\n"));
#endif

	if (status == CPU_IRQSTATUS_AUTO)
	{
		VezCurrentCPU->cpu_set_irq_line(line, vector, CPU_IRQSTATUS_ACK);
		VezCurrentCPU->cpu_execute(100);
		VezCurrentCPU->cpu_set_irq_line(line, vector, CPU_IRQSTATUS_NONE);
		VezCurrentCPU->cpu_execute(100);
	}
	else
	{
		VezCurrentCPU->cpu_set_irq_line(line, vector, status);
	}
}

void VezSetIRQLineAndVector(INT32 nCPU, const INT32 line, const INT32 vector, const INT32 status)
{
#if defined FBNEO_DEBUG
	if (!DebugCPU_VezInitted) bprintf(PRINT_ERROR, _T("VezSetIRQLineAndVector called without init\n"));
#endif

	VezCPUPush(nCPU);

	VezSetIRQLineAndVector(line, vector, status);

	VezCPUPop();
}
