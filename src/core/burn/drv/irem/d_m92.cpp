// FB Neo Irem M92 system driver
// Based on MAME driver by Bryan McPhail
// Original port from MAME by OopsWare
// Overhauled and tuned up by dink in August 2014, R-Type LEO title intro-effect fixed Dec.2016

#include "tiles_generic.h"
#include "burn_ym2151.h"
#include "nec_intf.h"
#include "msm6295.h" // ppan
#include "irem_cpu.h"
#include "iremga20.h"
#include "stddef.h"
#include "pic8259.h"
#include "rei_prof.h" // ReiREM Vita port: section profiler (no-op unless REI_PROFILE)
#include "rei_line.h" // ReiREM PSP port: per-line code block
extern "C" int rei_interleave_mul; // ReiREM Vita port: CPU slices per scanline (FBNeo default 8)

// ReiREM Vita port: pipelined main/sound mode (docs/DECISIONS.md ADR-003, ADR-006)
// Main->sound (latch) and sound->main (status + IRQ3) are exchanged as events instead of
// running the other CPU immediately. Every event carries the absolute scanline number of the
// side that produced it, and both sides consume events by the same rules:
//   - sound line L applies latch writes produced by main lines <= L, at their cycle position.
//   - main line L applies status writes produced by sound lines <= L-2, at (time + 2 lines).
// So the result is the same whether the two sides alternate on one thread (harness, single
// thread build) or run on two threads where the sound thread trails by 0..2 lines.
extern "C" int rei_pipelined;
extern "C" int rei_threaded;              // 1: sound side runs on its own thread
#include "rei_me.h"                        // PSP: sound side on the Media Engine (uncached shared data)
extern "C" volatile int rei_stop;          // set to release waiting threads on exit (REI_SH: uncached on PSP)
extern "C" void (*rei_wait_hook)(void);    // called while spinning (may be NULL)
extern "C" int rei_render_threaded;        // 1: recorded frames are drawn by a render thread
extern "C" int rei_index_out;              // 1: stop at the index buffer, the frontend converts (PSP GE)
extern "C" const UINT32 *rei_out_pal;      // index_out: palette of the last drawn frame (0x801 entries)
extern "C" int rei_out_blank;              // index_out: last drawn frame is blanked
#include "rei_core.h"
extern "C" int rei_ge_layers;              // PSP port: tile layers drawn by the frontend (GE), G2
extern "C" int rei_ge_ref;                 // ge_layers: also draw the CPU reference frame into pTransDraw
extern "C" rei_layer_fn rei_layer_cb;      // ge_layers: called per raster segment after the VRAM replay
extern "C" void *rei_layer_cb_user;
extern "C" UINT16 *rei_spr_buf;            // ge_layers: sprite-only buffer (see rei_core_sprite_frame)
extern "C" int rei_ge_sprites;             // PSP port (G3): sprites drawn by the frontend from a tile list
extern "C" rei_spr_tile *rei_spr_tiles;
extern "C" int rei_spr_ntiles, rei_spr_cap, rei_spr_overflow;
extern "C" UINT64 rei_main_wait_sound_us, rei_main_wait_render_us; // diagnostics (REI_PROFILE)
#define REI_RING 1024                      // power of two
extern "C" int rei_s2m_delay;           // sound->main delay in lines (ADR-003/006; 32, PSP 128)
#define REI_S2M_DELAY_LINES rei_s2m_delay
#ifdef REI_YM_SPLIT // PSP: the main CPU's share of the YM2151 (burn_ym2151.cpp), fed from the main side's loops
extern "C" INT32 rei_ym_split_on;
extern "C" int rei_ym_main_poll(void);
#define REI_YM_MAIN_POLL() do { if (rei_ym_split_on) rei_ym_main_poll(); } while (0)
#else
#define REI_YM_MAIN_POLL() ((void)0)
#endif
#define REI_LINES 256
struct rei_m2s_ev { INT64 line; INT32 cyc33; UINT8 data; };
struct rei_s2m_ev { INT64 src_line; INT64 apply; UINT8 data; };
// Shared by the main and sound sides. On the PSP (REI_ME_SPLIT) the sound side runs on the Media Engine,
// so these live in their own cache-line aligned block and every access goes through the uncached alias
// (rei_me.h); the names below are macros for that. Elsewhere they are plain variables.
static struct rei_m2s_ev rei_m2s_q_[REI_RING] REI_ME_SHARED;
static struct rei_s2m_ev rei_s2m_q_[REI_RING] REI_ME_SHARED;
static UINT32 rei_m2s_w_ REI_ME_SHARED, rei_m2s_r_ REI_ME_SHARED; // ring indices (SPSC)
static UINT32 rei_s2m_w_ REI_ME_SHARED, rei_s2m_r_ REI_ME_SHARED;
static INT64 rei_main_done_ REI_ME_SHARED, rei_sound_done_ REI_ME_SHARED; // lines completed (published)
static INT64 rei_sound_waiting_ REI_ME_SHARED = -1;      // line the sound thread is idle-waiting for, else -1
#define rei_m2s_q (REI_UNCACHED(&rei_m2s_q_[0]))
#define rei_s2m_q (REI_UNCACHED(&rei_s2m_q_[0]))
#define rei_m2s_w REI_SH(rei_m2s_w_)
#define rei_m2s_r REI_SH(rei_m2s_r_)
#define rei_s2m_w REI_SH(rei_s2m_w_)
#define rei_s2m_r REI_SH(rei_s2m_r_)
#define rei_main_done REI_SH(rei_main_done_)
#define rei_sound_done REI_SH(rei_sound_done_)
#define rei_sound_waiting REI_SH(rei_sound_waiting_)
static INT64 rei_main_line;                              // next absolute line, main side
// ReiREM PSP port: main line spans (PipelinedMainSpan). While a span runs the V33 over several scanlines in one
// slice, the handlers that depend on the scanline use the line of the instruction (from its start cycle) and
// end the span at writes that act at the end of the current line (sprite DMA, raster IRQ position)
static INT32 rei_span_active, rei_span_stop, rei_span_stop_line;
extern int rei_v33_span_active;                          // nec.cpp: same flag for the V33 fast paths
static INT64 rei_span_line0;                             // absolute line of frame line 0
static INT32 rei_span35_active REI_ME_DATA;              // sound side span (PipelinedSoundSpan), written by the ME
static INT64 rei_span35_line0 REI_ME_DATA;
unsigned int v25_ins_start_cycles();
unsigned int nec_ins_start_cycles();
static INT64 rei_sound_line REI_ME_DATA;                 // next absolute line, sound side
static INT32 rei_total33 = 150000, rei_total35 = 119318;
static INT32 ReiLineOfCycle(INT32 c);
static INT32 ReiLineOfCycle35(INT32 c);
static void ReiSpanStop() // end the running span after this instruction; its line ends normally
{
	rei_span_stop = 1;
	rei_span_stop_line = ReiLineOfCycle((INT32)nec_ins_start_cycles());
	VezRunEnd();
}
#define REI_VLOG (1 << 18)                                // VRAM write log entries (power of two)
static UINT32 *rei_vlog;                                  // (offset << 8) | byte
static UINT32 rei_vlog_w, rei_vlog_r;                     // SPSC: main writes, render applies
static UINT32 rei_vlog_lim;                               // main side: rei_vlog_w may grow up to here without checking
static void ReiVramLogRoom();
static void ReiCaptureSegment(INT32 start, INT32 finish);  // render capture (see below)
static void ReiCaptureFrame();
#ifdef REI_ME_SPLIT
// PSP: the cross-CPU variables are uncached (in-order loads/stores); sync orders the stores. The other users
// (VRAM log, recorded frames) are main-CPU only there. No 64-bit atomics (the ME cannot share them).
#define REI_LOAD(x)     (x)
#define REI_STORE(x, v) do { REI_ME_SYNC(); (x) = (v); REI_ME_SYNC(); } while (0)
#else
#define REI_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define REI_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#endif
#ifdef REI_TRACE_EVENTS
#include <stdio.h>
extern "C" UINT32 nCurrentFrame;
#define REI_TRACE(...) do { if (nCurrentFrame < 4000) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define REI_TRACE(...) ((void)0)
#endif

static UINT8 *Mem = NULL;
static UINT8 *MemEnd = NULL;
static UINT8 *RamStart;
static UINT8 *RamEnd;
static UINT8 *DrvV33ROM;
static UINT8 *DrvV30ROM;
static UINT8 *DrvGfxROM0;
static UINT8 *DrvGfxROM1;
static UINT8 *DrvSndROM;
static UINT8 *DrvVidRAM;
static UINT8 *DrvV33RAM;
static UINT8 *DrvV30RAM;
static UINT8 *DrvSprRAM;
static UINT8 *DrvSprBuf;
static UINT8 *DrvPalRAM;
static UINT8 *DrvEEPROM;

static UINT8 *pf_control[4];

static UINT8 *sound_status;
static UINT8 *sound_latch;
#ifdef REI_ME_SPLIT
static UINT8 rei_me_sound_latch[4] REI_ME_DATA; // PSP: written by the sound side (ME), kept off main-side lines
#endif

static UINT8 *RamPrioBitmap;

static UINT32 *DrvPalette;
static UINT8 bRecalcPalette = 0;

static UINT32 PalBank;
static INT32 no_palbank = 0;

static INT32 sprite_extent = 0;
static UINT8 m92_sprite_buffer_busy;
static INT32 m92_sprite_list;
static INT32 m92_sprite_buffer_timer;
static INT32 m92_raster_irq_position = 0;

static UINT8 DrvButton[8];
static UINT8 DrvJoy1[8];
static UINT8 DrvJoy2[8];
static UINT8 DrvJoy3[8];
static UINT8 DrvJoy4[8];
static UINT8 DrvInput[9];
static UINT8 DrvReset = 0;

static INT32 m92_main_bank;

static INT32 graphics_mask[2] = { 0, 0 };

static INT32 nInterleave = 256; // 256 scanlines
static INT32 nCyclesDone[2] = { 0, 0 };
static INT32 nCyclesTotal[2] = { 0, 0 };

static INT32 m92_kludge = 0;
static INT32 m92_banks = 0;
static UINT8 *m92_bank_ptr;   // base used directly with window addresses (NULL = game without banking: window stays as mapped in the table)
static INT32 m92_bank_mapped; // window is still mapped in the table (DrvInit's memory map)
static INT32 nPrevScreenPos = 0;
static INT32 nScreenOffsets[2] = { 0, 0 }; // x,y (ppan)

static UINT16 m92_video_reg = 0;
static INT32 m92_ok_to_blank = 0;

static INT32 msm6295_bank;

static INT32 leaguemna = 0;

typedef struct _m92_layer m92_layer;
struct _m92_layer
{
	INT32 enable;
	INT32 wide;
	INT32 enable_rowscroll;

	UINT16 scrollx;
	UINT16 scrolly;

	UINT16 *scroll;
	UINT16 *vram;
};

static struct _m92_layer *m92_layers[3];

enum { VECTOR_INIT, YM2151_ASSERT, YM2151_CLEAR, V30_ASSERT, V30_CLEAR };

static struct BurnInputInfo p2CommonInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(p2Common)

static struct BurnInputInfo p3CommonInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"P3 Coin",			BIT_DIGITAL,	DrvJoy3 + 5,	"p3 coin"	},
	{"P3 Start",		BIT_DIGITAL,	DrvJoy3 + 4,	"p3 start"	},
	{"P3 Up",			BIT_DIGITAL,	DrvJoy3 + 3,	"p3 up"		},
	{"P3 Down",			BIT_DIGITAL,	DrvJoy3 + 2,	"p3 down"	},
	{"P3 Left",			BIT_DIGITAL,	DrvJoy3 + 1,	"p3 left"	},
	{"P3 Right",		BIT_DIGITAL,	DrvJoy3 + 0,	"p3 right"	},
	{"P3 Button 1",		BIT_DIGITAL,	DrvJoy3 + 7,	"p3 fire 1"	},
	{"P3 Button 2",		BIT_DIGITAL,	DrvJoy3 + 6,	"p3 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(p3Common)

static struct BurnInputInfo p4CommonInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	}, //  0
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	}, //  8
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"P3 Coin",			BIT_DIGITAL,	DrvJoy3 + 5,	"p3 coin"	}, // 10
	{"P3 Start",		BIT_DIGITAL,	DrvJoy3 + 4,	"p3 start"	},
	{"P3 Up",			BIT_DIGITAL,	DrvJoy3 + 3,	"p3 up"		},
	{"P3 Down",			BIT_DIGITAL,	DrvJoy3 + 2,	"p3 down"	},
	{"P3 Left",			BIT_DIGITAL,	DrvJoy3 + 1,	"p3 left"	},
	{"P3 Right",		BIT_DIGITAL,	DrvJoy3 + 0,	"p3 right"	},
	{"P3 Button 1",		BIT_DIGITAL,	DrvJoy3 + 7,	"p3 fire 1"	},
	{"P3 Button 2",		BIT_DIGITAL,	DrvJoy3 + 6,	"p3 fire 2"	},

	{"P4 Coin",			BIT_DIGITAL,	DrvJoy4 + 5,	"p4 coin"	},
	{"P4 Start",		BIT_DIGITAL,	DrvJoy4 + 4,	"p4 start"	}, // 19
	{"P4 Up",			BIT_DIGITAL,	DrvJoy4 + 3,	"p4 up"		},
	{"P4 Down",			BIT_DIGITAL,	DrvJoy4 + 2,	"p4 down"	},
	{"P4 Left",			BIT_DIGITAL,	DrvJoy4 + 1,	"p4 left"	},
	{"P4 Right",		BIT_DIGITAL,	DrvJoy4 + 0,	"p4 right"	},
	{"P4 Button 1",		BIT_DIGITAL,	DrvJoy4 + 7,	"p4 fire 1"	},
	{"P4 Button 2",		BIT_DIGITAL,	DrvJoy4 + 6,	"p4 fire 2"	}, // 1f

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		}, // 20
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		}, // 22
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(p4Common)

static struct BurnInputInfo nbbatmanInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"P3 Coin",			BIT_DIGITAL,	DrvJoy3 + 5,	"p3 coin"	},
	{"P3 Start",		BIT_DIGITAL,	DrvJoy3 + 4,	"p3 start"	},
	{"P3 Up",			BIT_DIGITAL,	DrvJoy3 + 3,	"p3 up"		},
	{"P3 Down",			BIT_DIGITAL,	DrvJoy3 + 2,	"p3 down"	},
	{"P3 Left",			BIT_DIGITAL,	DrvJoy3 + 1,	"p3 left"	},
	{"P3 Right",		BIT_DIGITAL,	DrvJoy3 + 0,	"p3 right"	},
	{"P3 Button 1",		BIT_DIGITAL,	DrvJoy3 + 7,	"p3 fire 1"	},
	{"P3 Button 2",		BIT_DIGITAL,	DrvJoy3 + 6,	"p3 fire 2"	},

	{"P4 Coin",			BIT_DIGITAL,	DrvJoy4 + 5,	"p4 coin"	},
	{"P4 Start",		BIT_DIGITAL,	DrvJoy4 + 4,	"p4 start"	},
	{"P4 Up",			BIT_DIGITAL,	DrvJoy4 + 3,	"p4 up"		},
	{"P4 Down",			BIT_DIGITAL,	DrvJoy4 + 2,	"p4 down"	},
	{"P4 Left",			BIT_DIGITAL,	DrvJoy4 + 1,	"p4 left"	},
	{"P4 Right",		BIT_DIGITAL,	DrvJoy4 + 0,	"p4 right"	},
	{"P4 Button 1",		BIT_DIGITAL,	DrvJoy4 + 7,	"p4 fire 1"	},
	{"P4 Button 2",		BIT_DIGITAL,	DrvJoy4 + 6,	"p4 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
	{"Dip D",			BIT_DIPSWITCH,	DrvInput + 8,	"dip"		},
};

STDINPUTINFO(nbbatman)

static struct BurnInputInfo PsoldierInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},
	{"P1 Button 3",		BIT_DIGITAL,	DrvJoy1 + 5,	"p1 fire 3"	},
	{"P1 Button 4",		BIT_DIGITAL,	DrvJoy4 + 1,	"p1 fire 4"	},
	{"P1 Button 5",		BIT_DIGITAL,	DrvJoy4 + 2,	"p1 fire 5"	},
	{"P1 Button 6",		BIT_DIGITAL,	DrvJoy4 + 3,	"p1 fire 6"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},
	{"P2 Button 3",		BIT_DIGITAL,	DrvJoy2 + 5,	"p2 fire 3"	},
	{"P2 Button 4",		BIT_DIGITAL,	DrvJoy4 + 5,	"p2 fire 4"	},
	{"P2 Button 5",		BIT_DIGITAL,	DrvJoy4 + 6,	"p2 fire 5"	},
	{"P2 Button 6",		BIT_DIGITAL,	DrvJoy4 + 7,	"p2 fire 6"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(Psoldier)

static struct BurnDIPInfo BmasterDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL		},
	{0x13, 0xff, 0xff, 0xff, NULL		},
	{0x14, 0xff, 0xff, 0xff, NULL		},

	{0   , 0xfe, 0   ,    4, "Lives"		},
	{0x12, 0x01, 0x03, 0x00, "1"		},
	{0x12, 0x01, 0x03, 0x03, "2"		},
	{0x12, 0x01, 0x03, 0x02, "3"		},
	{0x12, 0x01, 0x03, 0x01, "4"		},

	{0   , 0xfe, 0   ,    4, "Difficulty"		},
	{0x12, 0x01, 0x0c, 0x00, "Very Easy"		},
	{0x12, 0x01, 0x0c, 0x08, "Easy"		},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"		},
	{0x12, 0x01, 0x0c, 0x04, "Hard"		},

	{0   , 0xfe, 0   ,    2, "Bonus Life"		},
	{0x12, 0x01, 0x10, 0x10, "300k only"		},
	{0x12, 0x01, 0x10, 0x00, "None"		},

	{0   , 0xfe, 0   ,    2, "Allow Continue"		},
	{0x12, 0x01, 0x20, 0x00, "No"		},
	{0x12, 0x01, 0x20, 0x20, "Yes"		},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"		},
	{0x12, 0x01, 0x40, 0x40, "Off"		},
	{0x12, 0x01, 0x40, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Service Mode"		},
	{0x12, 0x01, 0x80, 0x80, "Off"		},
	{0x12, 0x01, 0x80, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Flip Screen"		},
	{0x13, 0x01, 0x01, 0x01, "Off"		},
	{0x13, 0x01, 0x01, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Coin Slots"		},
	{0x13, 0x01, 0x04, 0x04, "Common"		},
	{0x13, 0x01, 0x04, 0x00, "Separate"		},

	{0   , 0xfe, 0   ,    2, "Coin Mode"		},
	{0x13, 0x01, 0x08, 0x08, "1"		},
	{0x13, 0x01, 0x08, 0x00, "2"		},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"		},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"		},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"		},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"		},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"		},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"		},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"		},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"		},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"		},

	{0   , 0xfe, 0   ,    4, "Coin B"		},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"		},
#endif
};

STDDIPINFO(Bmaster)

static struct BurnDIPInfo GunforceDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL		},
	{0x13, 0xff, 0xff, 0xfd, NULL		},
	{0x14, 0xff, 0xff, 0xf0, NULL		},

	{0   , 0xfe, 0   ,    4, "Lives"		},
	{0x12, 0x01, 0x03, 0x02, "2"		},
	{0x12, 0x01, 0x03, 0x03, "3"		},
	{0x12, 0x01, 0x03, 0x01, "4"		},
	{0x12, 0x01, 0x03, 0x00, "5"		},

	{0   , 0xfe, 0   ,    4, "Difficulty"		},
	{0x12, 0x01, 0x0c, 0x00, "Very Easy"		},
	{0x12, 0x01, 0x0c, 0x08, "Easy"		},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"		},
	{0x12, 0x01, 0x0c, 0x04, "Hard"		},

	{0   , 0xfe, 0   ,    2, "Bonus Life"		},
	{0x12, 0x01, 0x10, 0x00, "15000 35000 75000 120000"		},
	{0x12, 0x01, 0x10, 0x10, "20000 40000 90000 150000"		},

	{0   , 0xfe, 0   ,    2, "Zero score each stage"		},
	{0x12, 0x01, 0x20, 0x00, "No"		},
	{0x12, 0x01, 0x20, 0x20, "Yes (sometimes it's 100)"		},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"		},
	{0x12, 0x01, 0x40, 0x40, "Off"		},
	{0x12, 0x01, 0x40, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Service Mode"		},
	{0x12, 0x01, 0x80, 0x80, "Off"		},
	{0x12, 0x01, 0x80, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Flip Screen"		},
	{0x13, 0x01, 0x01, 0x01, "Off"		},
	{0x13, 0x01, 0x01, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Coin Slots"		},
	{0x13, 0x01, 0x04, 0x04, "Common"		},
	{0x13, 0x01, 0x04, 0x00, "Separate"		},

	{0   , 0xfe, 0   ,    2, "Coin Mode"		},
	{0x13, 0x01, 0x08, 0x08, "1"		},
	{0x13, 0x01, 0x08, 0x00, "2"		},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"		},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"		},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"		},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"		},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"		},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"		},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"		},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"		},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"		},

	{0   , 0xfe, 0   ,    4, "Coin B"		},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"		},
#endif
};

STDDIPINFO(Gunforce)


static struct BurnDIPInfo MysticriDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL		},
	{0x13, 0xff, 0xff, 0xfd, NULL		},

	{0   , 0xfe, 0   ,    4, "Lives"		},
	{0x12, 0x01, 0x03, 0x02, "2"		},
	{0x12, 0x01, 0x03, 0x03, "3"		},
	{0x12, 0x01, 0x03, 0x01, "4"		},
	{0x12, 0x01, 0x03, 0x00, "5"		},

	{0   , 0xfe, 0   ,    4, "Difficulty"		},
	{0x12, 0x01, 0x0c, 0x08, "Easy"		},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"		},
	{0x12, 0x01, 0x0c, 0x04, "Hard"		},
	{0x12, 0x01, 0x0c, 0x00, "Hardest"		},

	{0   , 0xfe, 0   ,    2, "Bonus Life"		},
	{0x12, 0x01, 0x10, 0x00, "15000 35000 60000"		},
	{0x12, 0x01, 0x10, 0x10, "20000 50000 90000"		},

	{0   , 0xfe, 0   ,    2, "Allow Continue"		},
	{0x12, 0x01, 0x20, 0x00, "No"		},
	{0x12, 0x01, 0x20, 0x20, "Yes"		},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"		},
	{0x12, 0x01, 0x40, 0x40, "Off"		},
	{0x12, 0x01, 0x40, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Service Mode"		},
	{0x12, 0x01, 0x80, 0x80, "Off"		},
	{0x12, 0x01, 0x80, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Flip Screen"		},
	{0x13, 0x01, 0x01, 0x01, "Off"		},
	{0x13, 0x01, 0x01, 0x00, "On"		},

	{0   , 0xfe, 0   ,    2, "Coin Slots"		},
	{0x13, 0x01, 0x04, 0x04, "Common"		},
	{0x13, 0x01, 0x04, 0x00, "Separate"		},

	{0   , 0xfe, 0   ,    2, "Coin Mode"		},
	{0x13, 0x01, 0x08, 0x08, "1"		},
	{0x13, 0x01, 0x08, 0x00, "2"		},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"		},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"		},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"		},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"		},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"		},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"		},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"		},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"		},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"		},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"		},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"		},

	{0   , 0xfe, 0   ,    4, "Coin B"		},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"		},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"		},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"		},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"		},
#endif
};

STDDIPINFO(Mysticri)

static struct BurnDIPInfo Gunforc2DIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL					},
	{0x13, 0xff, 0xff, 0xfd, NULL					},
	{0x14, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x12, 0x01, 0x03, 0x02, "3"					},
	{0x12, 0x01, 0x03, 0x03, "2"					},
	{0x12, 0x01, 0x03, 0x01, "4"					},
	{0x12, 0x01, 0x03, 0x00, "1"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x12, 0x01, 0x0c, 0x08, "Easy"					},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x12, 0x01, 0x0c, 0x04, "Hard"					},
	{0x12, 0x01, 0x0c, 0x00, "Hardest"				},

#if 0
	// has no effect in-game
	{0   , 0xfe, 0   ,    2, "Allow Continue"			},
	{0x12, 0x01, 0x20, 0x00, "No"					},
	{0x12, 0x01, 0x20, 0x20, "Yes"					},
#endif

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x12, 0x01, 0x40, 0x40, "Off"					},
	{0x12, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x12, 0x01, 0x80, 0x80, "Off"					},
	{0x12, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x13, 0x01, 0x01, 0x01, "Off"					},
	{0x13, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x13, 0x01, 0x04, 0x04, "Common"				},
	{0x13, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x13, 0x01, 0x08, 0x08, "1"					},
	{0x13, 0x01, 0x08, 0x00, "2"					},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"				},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"				},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"			},

	{0   , 0xfe, 0   ,    4, "Coin B"				},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"			},
#endif
};

STDDIPINFO(Gunforc2)

static struct BurnDIPInfo RtypeleoDIPList[]=
{
	{0x12, 0xff, 0xff, 0xaf, NULL					},
	{0x13, 0xff, 0xff, 0xfd, NULL					},
	{0x14, 0xff, 0xff, 0xf0, NULL					},

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x12, 0x01, 0x03, 0x02, "2"					},
	{0x12, 0x01, 0x03, 0x03, "3"					},
	{0x12, 0x01, 0x03, 0x01, "4"					},
	{0x12, 0x01, 0x03, 0x00, "5"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x12, 0x01, 0x0c, 0x00, "Very Easy"				},
	{0x12, 0x01, 0x0c, 0x08, "Easy"					},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x12, 0x01, 0x0c, 0x04, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Allow Continue"			},
	{0x12, 0x01, 0x20, 0x00, "No"					},
	{0x12, 0x01, 0x20, 0x20, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x12, 0x01, 0x40, 0x40, "Off"					},
	{0x12, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x12, 0x01, 0x80, 0x80, "Off"					},
	{0x12, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x13, 0x01, 0x01, 0x01, "Off"					},
	{0x13, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x13, 0x01, 0x04, 0x04, "Common"				},
	{0x13, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x13, 0x01, 0x08, 0x08, "1"					},
	{0x13, 0x01, 0x08, 0x00, "2"					},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"				},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"				},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"			},

	{0   , 0xfe, 0   ,    4, "Coin B"				},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"			},
#endif
};

STDDIPINFO(Rtypeleo)

static struct BurnDIPInfo InthuntDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL					},
	{0x13, 0xff, 0xff, 0xff, NULL					},
	{0x14, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x12, 0x01, 0x03, 0x02, "2"					},
	{0x12, 0x01, 0x03, 0x03, "3"					},
	{0x12, 0x01, 0x03, 0x01, "4"					},
	{0x12, 0x01, 0x03, 0x00, "5"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x12, 0x01, 0x0c, 0x00, "Very Easy"				},
	{0x12, 0x01, 0x0c, 0x08, "Easy"					},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x12, 0x01, 0x0c, 0x04, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Any Button to Start"			},
	{0x12, 0x01, 0x20, 0x20, "No"					},
	{0x12, 0x01, 0x20, 0x00, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x12, 0x01, 0x40, 0x40, "Off"					},
	{0x12, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x12, 0x01, 0x80, 0x80, "Off"					},
	{0x12, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x13, 0x01, 0x01, 0x01, "Off"					},
	{0x13, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x13, 0x01, 0x04, 0x04, "Common"				},
	{0x13, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x13, 0x01, 0x08, 0x08, "1"					},
	{0x13, 0x01, 0x08, 0x00, "2"					},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"				},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"				},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"			},

	{0   , 0xfe, 0   ,    4, "Coin B"				},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"			},
#endif
};

STDDIPINFO(Inthunt)

static struct BurnDIPInfo LethalthDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL					},
	{0x13, 0xff, 0xff, 0xfd, NULL					},
	{0x14, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x12, 0x01, 0x03, 0x02, "2"					},
	{0x12, 0x01, 0x03, 0x03, "3"					},
	{0x12, 0x01, 0x03, 0x01, "4"					},
	{0x12, 0x01, 0x03, 0x00, "5"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x12, 0x01, 0x0c, 0x00, "Very Easy"				},
	{0x12, 0x01, 0x0c, 0x08, "Easy"					},
	{0x12, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x12, 0x01, 0x0c, 0x04, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Continuous Play"			},
	{0x12, 0x01, 0x10, 0x00, "Off"					},
	{0x12, 0x01, 0x10, 0x10, "On"					},

	{0   , 0xfe, 0   ,    2, "Allow Continue"			},
	{0x12, 0x01, 0x20, 0x00, "No"					},
	{0x12, 0x01, 0x20, 0x20, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x12, 0x01, 0x40, 0x40, "Off"					},
	{0x12, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x12, 0x01, 0x80, 0x80, "Off"					},
	{0x12, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x13, 0x01, 0x01, 0x01, "Off"					},
	{0x13, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x13, 0x01, 0x04, 0x04, "Common"				},
	{0x13, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x13, 0x01, 0x08, 0x08, "1"					},
	{0x13, 0x01, 0x08, 0x00, "2"					},

#if 1
	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x13, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x13, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x13, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x13, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x13, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x13, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x13, 0x01, 0xf0, 0x00, "Free Play"				},
#else
	{0   , 0xfe, 0   ,    4, "Coin A"				},
	{0x13, 0x01, 0x30, 0x00, "5 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x10, "3 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x20, "2 Coins 1 Credits"			},
	{0x13, 0x01, 0x30, 0x30, "1 Coin  1 Credits"			},

	{0   , 0xfe, 0   ,    4, "Coin B"				},
	{0x13, 0x01, 0xc0, 0xc0, "1 Coin  2 Credits"			},
	{0x13, 0x01, 0xc0, 0x80, "1 Coin  3 Credits"			},
	{0x13, 0x01, 0xc0, 0x40, "1 Coin  5 Credits"			},
	{0x13, 0x01, 0xc0, 0x00, "1 Coin  6 Credits"			},
#endif

	{0   , 0xfe, 0   ,    4, "Bonus Life"				},
	{0x14, 0x01, 0x03, 0x02, "500K & 1M"				},
	{0x14, 0x01, 0x03, 0x03, "700K & 1.5M"				},
	{0x14, 0x01, 0x03, 0x00, "700K, 1.5M, 3M & 4.5M"		},
	{0x14, 0x01, 0x03, 0x01, "1M & 2M"				},
};	

STDDIPINFO(Lethalth)

static struct BurnDIPInfo UccopsDIPList[]=
{
	{0x1a, 0xff, 0xff, 0xae, NULL					},
	{0x1b, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x1a, 0x01, 0x03, 0x00, "1"					},
	{0x1a, 0x01, 0x03, 0x03, "2"					},
	{0x1a, 0x01, 0x03, 0x02, "3"					},
	{0x1a, 0x01, 0x03, 0x01, "4"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x1a, 0x01, 0x0c, 0x00, "Very Easy"				},
	{0x1a, 0x01, 0x0c, 0x08, "Easy"					},
	{0x1a, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x1a, 0x01, 0x0c, 0x04, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Any Button to Start"			},
	{0x1a, 0x01, 0x20, 0x00, "No"					},
	{0x1a, 0x01, 0x20, 0x20, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x1a, 0x01, 0x40, 0x40, "Off"					},
	{0x1a, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x1a, 0x01, 0x80, 0x80, "Off"					},
	{0x1a, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x1b, 0x01, 0x01, 0x01, "Off"					},
	{0x1b, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Cabinet"				},
	{0x1b, 0x01, 0x02, 0x02, "2 Players"				},
	{0x1b, 0x01, 0x02, 0x00, "4 Players"				},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x1b, 0x01, 0x04, 0x04, "Common"				},
	{0x1b, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x1b, 0x01, 0x08, 0x08, "1"					},
	{0x1b, 0x01, 0x08, 0x00, "2"					},

	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x1b, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x1b, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x1b, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x1b, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x1b, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x1b, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x1b, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x1b, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x1b, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x1b, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x1b, 0x01, 0xf0, 0x00, "Free Play"				},
};

STDDIPINFO(Uccops)

static struct BurnDIPInfo HookDIPList[]=
{
	DIP_OFFSET(0x22)

	{0x00, 0xff, 0xff, 0xbf, NULL                             },
	{0x01, 0xff, 0xff, 0xff, NULL                             },
	{0x02, 0xff, 0xff, 0xff, NULL                             },

	{0   , 0xfe, 0   ,    4, "Lives"                          },
	{0x00, 0x01, 0x03, 0x00, "1"                              },
	{0x00, 0x01, 0x03, 0x03, "2"                              },
	{0x00, 0x01, 0x03, 0x02, "3"                              },
	{0x00, 0x01, 0x03, 0x01, "4"                              },

	{0   , 0xfe, 0   ,    4, "Difficulty"                     },
	{0x00, 0x01, 0x0c, 0x00, "Very Easy"                      },
	{0x00, 0x01, 0x0c, 0x08, "Easy"                           },
	{0x00, 0x01, 0x0c, 0x0c, "Normal"                         },
	{0x00, 0x01, 0x0c, 0x04, "Hard"                           },

	{0   , 0xfe, 0   ,    2, "Any Button to Start"            },
	{0x00, 0x01, 0x20, 0x00, "No"                             },
	{0x00, 0x01, 0x20, 0x20, "Yes"                            },

	{0   , 0xfe, 0   ,    2, "Demo Sounds"                    },
	{0x00, 0x01, 0x40, 0x40, "Off"                            },
	{0x00, 0x01, 0x40, 0x00, "On"                             },

	{0   , 0xfe, 0   ,    2, "Service Mode"                   },
	{0x00, 0x01, 0x80, 0x80, "Off"                            },
	{0x00, 0x01, 0x80, 0x00, "On"                             },

	{0   , 0xfe, 0   ,    2, "Flip Screen"                    },
	{0x01, 0x01, 0x01, 0x01, "Off"                            },
	{0x01, 0x01, 0x01, 0x00, "On"                             },

	{0   , 0xfe, 0   ,    2, "Cabinet"                        },
	{0x01, 0x01, 0x02, 0x02, "2 Players"                      },
	{0x01, 0x01, 0x02, 0x00, "4 Players"                      },

	{0   , 0xfe, 0   ,    2, "Coin Slots"                     },
	{0x01, 0x01, 0x04, 0x04, "Common"                         },
	{0x01, 0x01, 0x04, 0x00, "Separate"                       },

	{0   , 0xfe, 0   ,    2, "Coin Mode"                      },
	{0x01, 0x01, 0x08, 0x08, "1"                              },
	{0x01, 0x01, 0x08, 0x00, "2"                              },

	{0   , 0xfe, 0   ,   16, "Coinage"                        },
	{0x01, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"              },
	{0x01, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"              },
	{0x01, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"              },
	{0x01, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"              },
	{0x01, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"              },
	{0x01, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue" },
	{0x01, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"              },
	{0x01, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"              },
	{0x01, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"              },
	{0x01, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"              },
	{0x01, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"              },
	{0x01, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"              },
	{0x01, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"              },
	{0x01, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"              },
	{0x01, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"              },
	{0x01, 0x01, 0xf0, 0x00, "Free Play"                      },
};

STDDIPINFO(Hook)

static struct BurnDIPInfo NbbatmanDIPList[]=
{
	{0x22, 0xff, 0xff, 0x9f, NULL					},
	{0x23, 0xff, 0xff, 0xff, NULL					},
	{0x24, 0xff, 0xff, 0xff, NULL					},
	{0x25, 0xff, 0xff, 0x00, NULL                   },

	{0   , 0xfe, 0   ,    4, "Lives"				},
	{0x22, 0x01, 0x03, 0x00, "1"					},
	{0x22, 0x01, 0x03, 0x03, "2"					},
	{0x22, 0x01, 0x03, 0x02, "3"					},
	{0x22, 0x01, 0x03, 0x01, "4"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x22, 0x01, 0x0c, 0x08, "Easy"					},
	{0x22, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x22, 0x01, 0x0c, 0x04, "Hard"					},
	{0x22, 0x01, 0x0c, 0x00, "Hardest"				},

	{0   , 0xfe, 0   ,    2, "Any Button to Start"			},
	{0x22, 0x01, 0x20, 0x20, "No"					},
	{0x22, 0x01, 0x20, 0x00, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x22, 0x01, 0x40, 0x40, "Off"					},
	{0x22, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x22, 0x01, 0x80, 0x80, "Off"					},
	{0x22, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x23, 0x01, 0x01, 0x01, "Off"					},
	{0x23, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Cabinet"				},
	{0x23, 0x01, 0x02, 0x02, "2 Players"				},
	{0x23, 0x01, 0x02, 0x00, "4 Players"				},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x23, 0x01, 0x04, 0x04, "Common"				},
	{0x23, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x23, 0x01, 0x08, 0x08, "1"					},
	{0x23, 0x01, 0x08, 0x00, "2"					},

	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x23, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x23, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x23, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x23, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x23, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x23, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x23, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x23, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x23, 0x01, 0xf0, 0x00, "Free Play"				},

	{0   , 0xfe, 0   ,    2, "Early SoundCPU emulation mode. (HACK)" },
	{0x25, 0x01, 0x01, 0x00, "Off"					},
	{0x25, 0x01, 0x01, 0x01, "On (Reload game after change!)" },
};

STDDIPINFO(Nbbatman)

static struct BurnDIPInfo Majtitl2DIPList[]=
{
	{0x22, 0xff, 0xff, 0x9f, NULL					},
	{0x23, 0xff, 0xff, 0xfd, NULL					},
	{0x24, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    2, "Given Holes/Stroke Play"		},
	{0x22, 0x01, 0x01, 0x00, "1"					},
	{0x22, 0x01, 0x01, 0x01, "2"					},

	{0   , 0xfe, 0   ,    2, "Given Holes/Match or Skins"		},
	{0x22, 0x01, 0x02, 0x00, "1"					},
	{0x22, 0x01, 0x02, 0x02, "2"					},

	{0   , 0xfe, 0   ,    2, "Difficulty"				},
	{0x22, 0x01, 0x04, 0x04, "Normal"				},
	{0x22, 0x01, 0x04, 0x00, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Joystick Configuration"		},
	{0x22, 0x01, 0x08, 0x08, "Upright"				},
	{0x22, 0x01, 0x08, 0x00, "Cocktail"				},

	{0   , 0xfe, 0   ,    2, "Number of Joysticks"			},
	{0x22, 0x01, 0x10, 0x10, "2 Joysticks"				},
	{0x22, 0x01, 0x10, 0x00, "4 Joysticks"				},

	{0   , 0xfe, 0   ,    2, "Any Button to Start"			},
	{0x22, 0x01, 0x20, 0x20, "No"					},
	{0x22, 0x01, 0x20, 0x00, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x22, 0x01, 0x40, 0x40, "Off"					},
	{0x22, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x22, 0x01, 0x80, 0x80, "Off"					},
	{0x22, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x23, 0x01, 0x01, 0x01, "Off"					},
	{0x23, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Cabinet"				},
	{0x23, 0x01, 0x02, 0x00, "Upright"				},
	{0x23, 0x01, 0x02, 0x02, "Cocktail"				},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x23, 0x01, 0x04, 0x04, "Common"				},
	{0x23, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x23, 0x01, 0x08, 0x08, "1"					},
	{0x23, 0x01, 0x08, 0x00, "2"					},

	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x23, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x23, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x23, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x23, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x23, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x23, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x23, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x23, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x23, 0x01, 0xf0, 0x00, "Free Play"				},

	{0   , 0xfe, 0   ,    2, "Ticket Dispenser"			},
	{0x24, 0x01, 0x01, 0x01, "Off"					},
	{0x24, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    4, "Points Per Ticket"			},
	{0x24, 0x01, 0x06, 0x06, "1 Point - 1 Ticket"			},
	{0x24, 0x01, 0x06, 0x04, "2 Points - 1 Ticket"			},
	{0x24, 0x01, 0x06, 0x02, "5 Points - 1 Ticket"			},
	{0x24, 0x01, 0x06, 0x00, "10 Points - 1 Ticket"			},

	{0   , 0xfe, 0   ,    2, "Deltronics Model"			},
	{0x24, 0x01, 0x80, 0x80, "DL 1275"				},
	{0x24, 0x01, 0x80, 0x00, "DL 4SS"				},
};

STDDIPINFO(Majtitl2)

static struct BurnDIPInfo Dsoccr94jDIPList[]=
{
	{0x22, 0xff, 0xff, 0xbf, NULL					},
	{0x23, 0xff, 0xff, 0xff, NULL					},
	{0x24, 0xff, 0xff, 0xff, NULL					},

	{0   , 0xfe, 0   ,    4, "Time"					},
	{0x22, 0x01, 0x03, 0x00, "1:30"					},
	{0x22, 0x01, 0x03, 0x03, "2:00"					},
	{0x22, 0x01, 0x03, 0x02, "2:30"					},
	{0x22, 0x01, 0x03, 0x01, "3:00"					},

	{0   , 0xfe, 0   ,    4, "Difficulty"				},
	{0x22, 0x01, 0x0c, 0x00, "Very Easy"				},
	{0x22, 0x01, 0x0c, 0x08, "Easy"					},
	{0x22, 0x01, 0x0c, 0x0c, "Normal"				},
	{0x22, 0x01, 0x0c, 0x04, "Hard"					},

	{0   , 0xfe, 0   ,    2, "Game Mode"				},
	{0x22, 0x01, 0x10, 0x10, "Match Mode"				},
	{0x22, 0x01, 0x10, 0x00, "Power Mode"				},

	{0   , 0xfe, 0   ,    2, "Starting Button"			},
	{0x22, 0x01, 0x20, 0x00, "Button 1"				},
	{0x22, 0x01, 0x20, 0x20, "Start Button"				},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x22, 0x01, 0x40, 0x40, "Off"					},
	{0x22, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x22, 0x01, 0x80, 0x80, "Off"					},
	{0x22, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x23, 0x01, 0x01, 0x01, "Off"					},
	{0x23, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Cabinet"				},
	{0x23, 0x01, 0x02, 0x02, "2 Players"				},
	{0x23, 0x01, 0x02, 0x00, "4 Players"				},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x23, 0x01, 0x04, 0x04, "Common"				},
	{0x23, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x23, 0x01, 0x08, 0x08, "1"					},
	{0x23, 0x01, 0x08, 0x00, "2"					},

	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x23, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x23, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x23, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x23, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x23, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x23, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x23, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x23, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x23, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x23, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x23, 0x01, 0xf0, 0x00, "Free Play"				},

	{0   , 0xfe, 0   ,    4, "Player Power"				},
	{0x24, 0x01, 0x03, 0x00, "500"					},
	{0x24, 0x01, 0x03, 0x03, "1000"					},
	{0x24, 0x01, 0x03, 0x01, "1500"					},
	{0x24, 0x01, 0x03, 0x02, "2000"					},
};

STDDIPINFO(Dsoccr94j)

static struct BurnDIPInfo PsoldierDIPList[]=
{
	{0x1a, 0xff, 0xff, 0x9f, NULL					},
	{0x1b, 0xff, 0xff, 0xfd, NULL					},

	{0   , 0xfe, 0   ,    2, "Any Button to Start"			},
	{0x1a, 0x01, 0x20, 0x20, "No"					},
	{0x1a, 0x01, 0x20, 0x00, "Yes"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"				},
	{0x1a, 0x01, 0x40, 0x40, "Off"					},
	{0x1a, 0x01, 0x40, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"				},
	{0x1a, 0x01, 0x80, 0x80, "Off"					},
	{0x1a, 0x01, 0x80, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Flip Screen"				},
	{0x1b, 0x01, 0x01, 0x01, "Off"					},
	{0x1b, 0x01, 0x01, 0x00, "On"					},

	{0   , 0xfe, 0   ,    2, "Coin Slots"				},
	{0x1b, 0x01, 0x04, 0x04, "Common"				},
	{0x1b, 0x01, 0x04, 0x00, "Separate"				},

	{0   , 0xfe, 0   ,    2, "Coin Mode"				},
	{0x1b, 0x01, 0x08, 0x08, "1"					},
	{0x1b, 0x01, 0x08, 0x00, "2"					},

	{0   , 0xfe, 0   ,   16, "Coinage"				},
	{0x1b, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x1b, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x1b, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x1b, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x1b, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x1b, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x1b, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x1b, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x1b, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x1b, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x1b, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x1b, 0x01, 0xf0, 0x00, "Free Play"				},
};

STDDIPINFO(Psoldier)

static struct BurnDIPInfo Hook4pDIPList[] =
{
	{0x01, 0xff, 0xff, 0xfd, NULL},
};

STDDIPINFOEXT(Hook4p, Hook, Hook4p)

inline static UINT32 CalcCol(INT32 offs)
{
	INT32 nColour = DrvPalRAM[offs + 0] | (DrvPalRAM[offs + 1] << 8);
	INT32 r, g, b;

	r = (nColour & 0x001F) << 3;
	r |= r >> 5;
	g = (nColour & 0x03E0) >> 2;
	g |= g >> 5;
	b = (nColour & 0x7C00) >> 7;
	b |= b >> 5;

	return BurnHighCol(r, g, b, 0);
}

static void m92YM2151IRQHandler(INT32 nStatus)
{
	if (VezGetActive() == -1) return;
	VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff/*default*/, nStatus ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
	// ReiREM Vita port: this handler is also called from inside a V35 run (YM register writes).
	// The nested VezRun ends the outer run early while it is still counted as complete. With
	// FBNeo's 58-cycle slices the loss is small; in pipelined mode the V35 runs a whole frame per
	// call, so the nested run is skipped there and the IRQ is taken by the ongoing/next run.
	if (!rei_pipelined) VezRun(100);
}

static UINT8 __fastcall m92ReadByte(UINT32 address)
{
	if (address - 0xa0000 < 0x20000 && m92_bank_ptr) // bank window (m92MainBank)
		return m92_bank_ptr[address];

	if ((address & 0xff800) == 0xf8800 )
		return DrvPalRAM[ address - 0xf8800 + PalBank ];

	if ((address & 0xfc000) == 0xf0000 ) {
		if (address & 1) {
			return 0xff;
		} else {
			return DrvEEPROM[(address & 0x3fff) / 2];
		}
	}

	return 0;
}

static void __fastcall m92WriteByte(UINT32 address, UINT8 data)
{
	if ((address & 0xf0000) == 0xd0000) { // ReiREM Vita port: VRAM, write mapping removed in pipelined mode
		DrvVidRAM[address & 0xffff] = data;
		// no barrier per write: the render thread only reads up to vlog_pos published with the frame record,
		// and the free-space bound is refreshed from rei_vlog_r only when it is reached
		UINT32 w = rei_vlog_w;
		if ((INT32)(w - rei_vlog_lim) >= 0) ReiVramLogRoom();
		rei_vlog[w & (REI_VLOG - 1)] = ((address & 0xffff) << 8) | data;
		rei_vlog_w = w + 1;
		return;
	}

	if ((address & 0xff800) == 0xf8800 ) {
		DrvPalRAM[ address - 0xf8800 + PalBank ] = data;
		if (address & 1) {
			INT32 offs = (address - 0xf8800 + PalBank) >> 1;
			DrvPalette[offs] = CalcCol( offs << 1 );
		}
		return;
	}

	if ((address & 0xfc001) == 0xf0000 ) {
		DrvEEPROM[(address & 0x3fff) / 2] = data;
		return;
	}

	switch (address)
	{
		case 0xf9000:
			sprite_extent = (sprite_extent & 0xff00) | (data << 0);
			return;

		case 0xf9001:
			sprite_extent = (sprite_extent & 0x00ff) | (data << 8);
			return;

		case 0xf9004:
			m92_sprite_list = (data==8) ? (((0x100 - sprite_extent)&0xff)*4) : 0x400;
			return;

		case 0xf9008:
			m92_sprite_buffer_busy = 0;
			m92_sprite_buffer_timer = 1;
			pic8259_set_irq_line(1, 0);
			if (rei_span_active) ReiSpanStop(); // acts at the end of this line
			return;

		case 0xf9800:
			if (!no_palbank) {
				PalBank = (data & 0x02) ? 0x0800 : 0x0000;
			}
			m92_video_reg = (m92_video_reg & 0xff00) | (data << 0);
			return;

		case 0xf9801:
			m92_video_reg = (m92_video_reg & 0x00ff) | (data << 8);
			return;

//		default:
//			bprintf(PRINT_NORMAL, _T("Attempt to write byte value %x to location %x\n"), data, address);
	}
}

static UINT8 __fastcall m92ReadPort(UINT32 port)
{
	switch (port)
	{
		case 0x00: return ~DrvInput[0];	// player 1
		case 0x01: return ~DrvInput[1];	// player 2
		case 0x02: return (~DrvInput[4] & 0x7F) | m92_sprite_buffer_busy;
		case 0x03: return  DrvInput[7];	// dip 3
		case 0x04: return  DrvInput[5];	// dip 1
		case 0x05: return  DrvInput[6];	// dip 2
		case 0x06: return ~DrvInput[2];	// player 3
		case 0x07: return ~DrvInput[3];	// player 4

		case 0x08: REI_TRACE("f%u read-status %02x @%d\n", nCurrentFrame, sound_status[0], VezTotalCycles()); pic8259_set_irq_line(3, 0); return sound_status[0];
		case 0x09: pic8259_set_irq_line(3, 0); return sound_status[1];

		case 0x18: return (m92_kludge == 3) ? MSM6295Read(0) : 0; // ppan

		case 0x40:
		case 0x42:
			return pic8259_read((port & 3) / 2);
		case 0x41:
		case 0x43:
			return 0;

		default:
			bprintf(PRINT_NORMAL, _T("Attempt to read byte value of port %x\n"), port);
	}
	return 0;
}

static void set_pf_info(INT32 layer, INT32 data)
{
	struct _m92_layer *ptr = m92_layers[layer];

	if (data & 0x10) {
		ptr->enable = 0;
	} else {
		ptr->enable = 1;
		ptr->wide = (data & 0x04) ? 128 : 64;
	}

	ptr->enable_rowscroll = data & 0x40;

	ptr->vram = (UINT16*)(DrvVidRAM + ((data & 0x03) * 0x4000));
}

static void set_pf_scroll(INT32 layer)
{
	struct _m92_layer *ptr = m92_layers[layer];

	ptr->scrollx = (pf_control[layer][4] << 0) | (pf_control[layer][5] << 8);
	ptr->scrolly = (pf_control[layer][0] << 0) | (pf_control[layer][1] << 8);
}

// ReiREM port: bank window 0xa0000-0xbffff is not in the page table; m92ReadByte reads it (opcode fetch too). A bank switch
// is a single pointer update. Remapping the table (two VezMapArea = 768 table slots) took 64µs each on PSP; in scenes that switch
// banks per tile (nbbatman scene transitions: 300-470 per frame) that was 20-30ms/frame. Values read are identical (psp/docs/PERF.md PP22)

static void m92MainBank(INT32 data)
{
	m92_main_bank = data;
	m92_bank_ptr = DrvV33ROM + 0x100000 + (data&0x7)*0x10000 - 0xa0000;
	if (m92_bank_mapped) {
		m92_bank_mapped = 0;
		VezMemCallback(0xa0000, 0xbffff, 0);
		VezMemCallback(0xa0000, 0xbffff, 2);
	}
}

REI_LINE_FN(25) static void __fastcall m92WritePort(UINT32 port, UINT8 data)
{
	switch (port)
	{
		case 0x00:
	//	case 0x01:
			REI_TRACE("f%u latch %02x @%d\n", nCurrentFrame, data, VezTotalCycles());
			if (rei_pipelined) { // ReiREM Vita port: queue for the sound side
				UINT32 w = rei_m2s_w;
				if (w - REI_LOAD(rei_m2s_r) < REI_RING) {
					struct rei_m2s_ev *ev = &rei_m2s_q[w & (REI_RING - 1)];
					ev->line = rei_span_active ? rei_span_line0 + ReiLineOfCycle((INT32)nec_ins_start_cycles()) : rei_main_line;
					ev->cyc33 = VezTotalCycles();
					ev->data = data;
#ifdef REI_SPAN_DEBUG
					if (ev->line / REI_LINES > 1005 && ev->line / REI_LINES < 1016) fprintf(stderr, "m2s f%u line %lld cyc %d data %02x\n", nCurrentFrame, (long long)ev->line, ev->cyc33, data);
#endif
					REI_STORE(rei_m2s_w, w + 1);
				}
				return;
			}
			sound_latch[0] = data;
			VezClose();
			VezOpen(1);
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_ACK);
			VezRun(10);
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
			VezRun(10);
			VezClose();
			VezOpen(0);
			return;

		case 0x02:
		case 0x03:
			//coin counter
			return;

		case 0x10: // ppan
			if (m92_kludge == 3) {
				if (msm6295_bank != (data + 1)) {
					msm6295_bank = (data & 3) + 1;
					memcpy(DrvSndROM + 0x20000, DrvSndROM + 0x100000 + msm6295_bank * 0x20000, 0x20000);
				}
			}
		return;

		case 0x18: // ppan
			if (m92_kludge == 3) MSM6295Write(0, data);
		return;

		case 0x20:
	//	case 0x21:
			if (m92_banks) { // majtitl2, nbbatman, dsoccr94j, gunforc2
				m92MainBank(data);
			}
			return;

		case 0x40:
		case 0x42:
			pic8259_write((port & 3) / 2, data);
			return;
		case 0x41:
		case 0x43:
			return; // hi bytes of pic8529 (ignored)

		case 0x80: pf_control[0][0] = data; set_pf_scroll(0); return;
		case 0x81: pf_control[0][1] = data; set_pf_scroll(0); return;
		case 0x82: pf_control[0][2] = data; return;
		case 0x83: pf_control[0][3] = data; return;
		case 0x84: pf_control[0][4] = data; set_pf_scroll(0); return;
		case 0x85: pf_control[0][5] = data; set_pf_scroll(0); return;
		case 0x86: pf_control[0][6] = data; return;
		case 0x87: pf_control[0][7] = data; return;
		case 0x88: pf_control[1][0] = data; set_pf_scroll(1); return;
		case 0x89: pf_control[1][1] = data; set_pf_scroll(1); return;
		case 0x8a: pf_control[1][2] = data; return;
		case 0x8b: pf_control[1][3] = data; return;
		case 0x8c: pf_control[1][4] = data; set_pf_scroll(1); return;
		case 0x8d: pf_control[1][5] = data; set_pf_scroll(1); return;
		case 0x8e: pf_control[1][6] = data; return;
		case 0x8f: pf_control[1][7] = data; return;
		case 0x90: pf_control[2][0] = data; set_pf_scroll(2); return;
		case 0x91: pf_control[2][1] = data; set_pf_scroll(2); return;
		case 0x92: pf_control[2][2] = data; return;
		case 0x93: pf_control[2][3] = data; return;
		case 0x94: pf_control[2][4] = data; set_pf_scroll(2); return;
		case 0x95: pf_control[2][5] = data; set_pf_scroll(2); return;
		case 0x96: pf_control[2][6] = data; return;
		case 0x97: pf_control[2][7] = data; return;
		case 0x98: pf_control[3][0] = data; set_pf_info(0, data); return;
		case 0x99: pf_control[3][1] = data; return;
		case 0x9a: pf_control[3][2] = data; set_pf_info(1, data); return;
		case 0x9b: pf_control[3][3] = data; return;
		case 0x9c: pf_control[3][4] = data; set_pf_info(2, data); return;
		case 0x9d: pf_control[3][5] = data; return;
		case 0x9e: pf_control[3][6] = data;
			return;
		case 0x9f: pf_control[3][7] = data;
			m92_raster_irq_position = ((pf_control[3][7]<<8) | pf_control[3][6]) - 128;
			pic8259_set_irq_line(2, 0);
			if (rei_span_active) ReiSpanStop(); // the span was planned with the old raster line
			return;

		case 0xc0:
		case 0xc1:// sound reset
			return;

//		default:
//			bprintf(PRINT_NORMAL, _T("Attempt to write byte value %x to port %x\n"), data, port);
	}
}

static UINT8 __fastcall m92SndReadByte(UINT32 address)
{
	if ((address & 0xfffc0) == 0xa8000) {
		return iremga20_read( 0, (address & 0x0003f) / 2 );
	}

	switch (address)
	{
		case 0xa8042:
			return BurnYM2151Read();

		case 0xa8044:
			return sound_latch[0];

		case 0xa8045:
			return 0xff; // soundlatch high bits, always 0xff

//		default:
//			bprintf(PRINT_NORMAL, _T("V30 Attempt to read byte value of location %x\n"), address);
	}
	return 0;
}

static void __fastcall m92SndWriteByte(UINT32 address, UINT8 data)
{
	if ((address & 0xfffc0) == 0xa8000) {
		iremga20_write( 0, (address & 0x0003f) / 2, data );
		return;
	}

	if ((address & 0xfff00) == 0x9ff00) { // NOP
		return;
	}

	switch (address)
	{
		case 0xa8040:
			REI_TRACE("f%u yms %02x\n", nCurrentFrame, data);
			BurnYM2151SelectRegister(data);
			return;

		case 0xa8042:
			REI_TRACE("f%u ymw %02x\n", nCurrentFrame, data);
			BurnYM2151WriteRegister(data);
			return;

		case 0xa8044:
			//VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
			return;

		case 0xa8046:
			REI_TRACE("f%u status %02x @%d(v35)\n", nCurrentFrame, data, VezTotalCycles());
			if (rei_pipelined) { // ReiREM Vita port: applied to the main side 2 lines later
				UINT32 w = rei_s2m_w;
				if (w - REI_LOAD(rei_s2m_r) < REI_RING) {
					struct rei_s2m_ev *ev = &rei_s2m_q[w & (REI_RING - 1)];
					// (in a sound span: the line of this instruction, from its start cycle)
					const INT64 sline = rei_span35_active ? rei_span35_line0 + ReiLineOfCycle35((INT32)v25_ins_start_cycles())
									      : rei_sound_line;
					INT64 frame_base = (sline / REI_LINES) * rei_total33;
					ev->src_line = sline;
#ifdef REI_SPAN_DEBUG
					if (sline / REI_LINES > 1005 && sline / REI_LINES < 1016) fprintf(stderr, "s2m f%u src %lld v35 %d data %02x\n", nCurrentFrame, (long long)sline, VezTotalCycles(), data);
#endif
					ev->apply = frame_base + (INT64)VezTotalCycles() * rei_total33 / rei_total35
						+ REI_S2M_DELAY_LINES * rei_total33 / REI_LINES;
					ev->data = data;
					REI_STORE(rei_s2m_w, w + 1);
				}
				return;
			}
			sound_status[0] = data;
			VezClose();
			VezOpen(0);
			pic8259_set_irq_line(3, 1);
			VezClose();
			VezOpen(1);
			return;

	//	default:
	//		bprintf(PRINT_NORMAL, _T("V30 Attempt to write byte value %x to location %x\n"), data, address);
	}
}

static INT32 DrvDoReset()
{
	memset(RamStart, 0, RamEnd - RamStart);

	VezOpen(0);
	pic8259_reset();
	if (m92_banks) m92MainBank(0);
	VezReset();
	VezClose();

	VezOpen(1);
	VezReset();
	VezClose();

	BurnYM2151Reset();
	iremga20_reset(0);
	if (m92_kludge == 3) { // ppan
		MSM6295Reset(0);
		msm6295_bank = -1;
		m92WritePort(0x10, 0); // set bank
	}

	if (m92_kludge == 1) sound_status[0] = 0x80;

	m92_sprite_buffer_busy = 0x80;
	m92_sprite_buffer_timer = 0;
	PalBank	= 0;
	m92_video_reg = 0;

	{
		struct _m92_layer *ptr;
		for (INT32 i = 0; i < 3; i++) {
			ptr = m92_layers[i];
			ptr->scroll = (UINT16*)(DrvVidRAM + (0xf400 + 0x400 * i));
		}
	}

	HiscoreReset();

	return 0;
}

static void loadDecodeGfx01(UINT8 *tmp, INT32 rid, INT32 shift, INT32 size)
{
	UINT8 * pgfx = DrvGfxROM0;

	BurnLoadRom(tmp, rid, 1);

	for (INT32 i=0; i<(size/8); i++) {
		for( INT32 y=0;y<8;y++, tmp++, pgfx+=8) {
			for ( INT32 j=0;j<8;j++) {
				pgfx[j] |= ((tmp[0]>>(j^7))&1)<<shift;
			}
		}
	}
}

static void loadDecodeGfx02(UINT8 *tmp, INT32 rid, INT32 shift, INT32 size)
{
	UINT8 * pgfx = DrvGfxROM1;

	BurnLoadRom(tmp, rid, 1);

	for (INT32 i=0; i<(size/32); i++, tmp+=16) {
		for( INT32 y=0;y<16;y++, tmp++, pgfx+=16) {
			for ( INT32 j=0; j<16;j++) {
				pgfx[j] |= ((tmp[(j&8)<<1]>>(~j&7))&1)<<shift;
			}
		}
	}
}

static void loadDecodeGfx03(UINT8 *tmp, INT32 rid, INT32 shift, INT32 size)
{
	UINT8 * pgfx = DrvGfxROM1;

	BurnLoadRom(tmp + 1, rid + 0, 2);
	BurnLoadRom(tmp + 0, rid + 1, 2);

	for (INT32 i = 0; i < size*8; i++) {
		pgfx[i] |= ((tmp[i/8] >> (7-(i&7)))&1)<<shift;
	}
}

static INT32 MemIndex(INT32 gfxlen1, INT32 gfxlen2)
{
	UINT8 *Next; Next = Mem;
	DrvV33ROM 	= Next; Next += 0x180000;
	DrvV30ROM	= Next; Next += 0x020000;
	DrvGfxROM0	= Next; Next += gfxlen1 * 2;
	DrvGfxROM1	= Next; Next += gfxlen2 * 2;
	MSM6295ROM	= Next; // ppan
	DrvSndROM	= Next; Next += 0x180000;

	DrvEEPROM	= Next; Next += 0x002000;

	RamPrioBitmap	= Next; Next += 320 * 240;

	RamStart	= Next;

	DrvSprRAM	= Next; Next += 0x000800;
	DrvSprBuf	= Next; Next += 0x000800;
	DrvVidRAM	= Next; Next += 0x010000;
	DrvV33RAM	= Next; Next += 0x010000;
	DrvV30RAM	= Next; Next += 0x004000;
	DrvPalRAM	= Next; Next += 0x001000;

	sound_status	= Next; Next += 0x000004; // 2
	sound_latch	= Next; Next += 0x000004; // 1
#ifdef REI_ME_SPLIT
	sound_latch = rei_me_sound_latch;
#endif

	pf_control[0]	= Next; Next += 0x000008;
	pf_control[1]	= Next; Next += 0x000008;
	pf_control[2]	= Next; Next += 0x000008;
	pf_control[3]	= Next; Next += 0x000008;

	RamEnd		= Next;

	// scanned separately from ram due to pointers in structs
	m92_layers[0]	= (struct _m92_layer*)Next; Next += sizeof(struct _m92_layer);
	m92_layers[1]	= (struct _m92_layer*)Next; Next += sizeof(struct _m92_layer);
	m92_layers[2]	= (struct _m92_layer*)Next; Next += sizeof(struct _m92_layer);

	DrvPalette	= (UINT32 *) Next; Next += 0x0801 * sizeof(UINT32);

	MemEnd		= Next;
	return 0;
}

static INT32 RomLoad(INT32 v33off, INT32 gfxlen0, INT32 gfxlen1, INT32 gfxtype1, INT32 eep)
{
	if (BurnLoadRom(DrvV33ROM + 0x000001, 0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM + 0x000000, 1, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM + 0x000001 + v33off, 2, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM + 0x000000 + v33off, 3, 2)) return 1;

	if (BurnLoadRom(DrvV30ROM + 0x000001, 4, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM + 0x000000, 5, 2)) return 1;

	// ReiREM port: size the decode buffer from the ROM list (largest ROM x2, enough for the
	// gap-2 interleaved loads) instead of a fixed 8MB - the PSP has only ~21MB of heap under PSPLink
	INT32 tmplen = 0;
	for (INT32 i = 0; i < 256; i++) {
		struct BurnRomInfo ri;
		ri.nLen = 0;
		if (BurnDrvGetRomInfo(&ri, i)) break;
		if ((INT32)ri.nLen * 2 > tmplen) tmplen = ri.nLen * 2;
	}
	UINT8 *tmp = (UINT8 *)BurnMalloc(tmplen);
	if (tmp == NULL) {
		return 1;
	}

	loadDecodeGfx01(tmp,  6, 0, gfxlen0);
	loadDecodeGfx01(tmp,  7, 1, gfxlen0);
	loadDecodeGfx01(tmp,  8, 2, gfxlen0);
	loadDecodeGfx01(tmp,  9, 3, gfxlen0);

	if (gfxtype1) {
		loadDecodeGfx03(tmp, 10, 0, gfxlen1);
		loadDecodeGfx03(tmp, 12, 1, gfxlen1);
		loadDecodeGfx03(tmp, 14, 2, gfxlen1);
		loadDecodeGfx03(tmp, 16, 3, gfxlen1);

		if (BurnLoadRom(DrvSndROM + 0x000000, 18, 1)) return 1;
	} else {
		loadDecodeGfx02(tmp, 10, 0, gfxlen1);
		loadDecodeGfx02(tmp, 11, 1, gfxlen1);
		loadDecodeGfx02(tmp, 12, 2, gfxlen1);
		loadDecodeGfx02(tmp, 13, 3, gfxlen1);

		if (BurnLoadRom(DrvSndROM + 0x000000, 14, 1)) return 1;
	}

	if (eep) {
		if (BurnLoadRom(DrvEEPROM + 0x000000, eep, 1)) return 1;
	}

	BurnFree(tmp);

	return 0;
}

static INT32 DrvInit(INT32 (*pRomLoadCallback)(), const UINT8 *sound_decrypt_table, INT32 type, INT32 gfxlen1, INT32 gfxlen2)
{
	Mem = NULL;
	MemIndex(gfxlen1, gfxlen2);
	INT32 nLen = MemEnd - (UINT8 *)0;
	if ((Mem = (UINT8 *)BurnMalloc(nLen)) == NULL) return 1;
	memset(Mem, 0, nLen);
	MemIndex(gfxlen1, gfxlen2);

	if (pRomLoadCallback) {
		if (pRomLoadCallback()) return 1; 
	}

	{
		VezInit(0, V33_TYPE);
		VezInit(1, V35_TYPE, 14318180 /*before divider*/);

		VezOpen(0);

		// interrupt controller
		pic8259_init(nec_set_irq_line);
		nec_set_vector_callback(pic8259_inta_cb);

		m92_bank_ptr = NULL;
		m92_bank_mapped = 0;
		if (type == 0) { // lethalh
			VezMapArea(0x00000, 0x7ffff, 0, DrvV33ROM + 0x00000);
			VezMapArea(0x00000, 0x7ffff, 2, DrvV33ROM + 0x00000);
			VezMapArea(0x80000, 0x8ffff, 0, DrvVidRAM);
			VezMapArea(0x80000, 0x8ffff, 1, DrvVidRAM);
			VezMapArea(0x80000, 0x8ffff, 2, DrvVidRAM);
		} else {	// all others
			VezMapArea(0x00000, 0x9ffff, 0, DrvV33ROM + 0x00000);
			VezMapArea(0x00000, 0x9ffff, 2, DrvV33ROM + 0x00000);
			VezMapArea(0xa0000, 0xbffff, 0, DrvV33ROM + 0xa0000);
			VezMapArea(0xa0000, 0xbffff, 2, DrvV33ROM + 0xa0000);
			m92_bank_mapped = 1;
			VezMapArea(0xc0000, 0xcffff, 0, DrvV33ROM + 0x00000);	// in the hunt protection...
			VezMapArea(0xc0000, 0xcffff, 2, DrvV33ROM + 0x00000);
			VezMapArea(0xd0000, 0xdffff, 0, DrvVidRAM);
			VezMapArea(0xd0000, 0xdffff, 1, DrvVidRAM);
			VezMapArea(0xd0000, 0xdffff, 2, DrvVidRAM);
		}
		VezMapArea(0xe0000, 0xeffff, 0, DrvV33RAM);
		VezMapArea(0xe0000, 0xeffff, 1, DrvV33RAM);
		VezMapArea(0xe0000, 0xeffff, 2, DrvV33RAM);
		VezMapArea(0xf8000, 0xf87ff, 0, DrvSprRAM);
		VezMapArea(0xf8000, 0xf87ff, 1, DrvSprRAM);
		VezMapArea(0xff800, 0xfffff, 0, DrvV33ROM + 0x7f800);  // vectors?
		VezMapArea(0xff800, 0xfffff, 2, DrvV33ROM + 0x7f800);
		VezSetReadHandler(m92ReadByte);
		VezSetWriteHandler(m92WriteByte);
		VezSetReadPort(m92ReadPort);
		VezSetWritePort(m92WritePort);
		if (rei_pipelined) // ReiREM port: VRAM writes go through m92WriteByte to version the render snapshots (all games)
			VezMemCallback(0xd0000, 0xdffff, 1);
		VezClose();

		VezOpen(1);
		if (sound_decrypt_table != NULL) {
			VezSetDecode((UINT8*)sound_decrypt_table);
		}
		VezMapArea(0x00000, 0x1ffff, 0, DrvV30ROM + 0x00000);
		VezMapArea(0x00000, 0x1ffff, 2, DrvV30ROM + 0x00000);
		VezMapArea(0xa0000, 0xa3fff, 0, DrvV30RAM);
		VezMapArea(0xa0000, 0xa3fff, 1, DrvV30RAM);
		VezMapArea(0xa0000, 0xa3fff, 2, DrvV30RAM);
		VezMapArea(0xff800, 0xfffff, 0, DrvV30ROM + 0x1f800);
		VezMapArea(0xff800, 0xfffff, 2, DrvV30ROM + 0x1f800);
		VezSetReadHandler(m92SndReadByte);
		VezSetWriteHandler(m92SndWriteByte);
		VezClose();
	}

	graphics_mask[0] = ((gfxlen1 * 2) - 1) / (8 * 8);
	graphics_mask[1] = ((gfxlen2 * 2) - 1) / (16 * 16);

	BurnYM2151InitBuffered(3579545, 1, NULL, 0);
	YM2151SetIrqHandler(0, &m92YM2151IRQHandler);
	BurnYM2151SetAllRoutes(0.40, BURN_SND_ROUTE_BOTH);
	BurnTimerAttachVez(7159090);

	iremga20_init(0, DrvSndROM, 0x100000, 3579545);
	itemga20_set_route(0, 1.20, BURN_SND_ROUTE_BOTH);

	MSM6295Init(0, 1000000 / 132, 0); // ppan
	MSM6295SetRoute(0, 1.00, BURN_SND_ROUTE_BOTH);

	GenericTilesInit();

	DrvDoReset();

	return 0;
}

static INT32 DrvExit()
{
	GenericTilesExit();

	BurnYM2151Exit();
	iremga20_exit();
	MSM6295Exit(0); // ppan

	VezExit();
	pic8259_exit();

	BurnFree(Mem);
	
	nPrevScreenPos = 0;
	m92_banks = 0;
	m92_kludge = 0;
	m92_raster_irq_position = 0;
	nScreenOffsets[0] = nScreenOffsets[1] = 0;
	m92_ok_to_blank = 0;
	no_palbank = 0;
	leaguemna = 0;

	return 0;
}

static void RenderTilePrio(UINT16 *dest, UINT8 *gfx, INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 width, INT32 height, UINT8 *pri, INT32 prio)
{
	if (sx <= (0-width) || sx >= nScreenWidth || sy <= (0-height) || sy >= nScreenHeight) return;

	INT32 flip = 0;
	if (flipy) flip |= (height - 1) * width;
	if (flipx) flip |= width - 1;

	gfx += code * width * height;

	prio |= 1 << 31; // always on!

	for (INT32 y = 0; y < height; y++, sy++) {
		if (sy < 0 || sy >= nScreenHeight) continue;

		INT32 row = sy * nScreenWidth;

		for (INT32 x = 0; x < width; x++, sx++) {
			if (sx < 0 || sx >= nScreenWidth) continue;

			INT32 pxl = gfx[((y * width) + x) ^ flip];

			if (pxl == 0) continue;

			if ((prio & (1 << (pri[row + sx] & 0x1f))) == 0) {
				dest[row + sx] = pxl | color;
			}
			pri[row + sx] |= 0x1f;
		}

		sx -= width;
	}
}

static void draw_sprites()
{
	UINT16 *ram = (UINT16*)DrvSprBuf;

	for (INT32 k=0; k<8; k++)
	{
		INT32 start_offs = (m92_kludge == 3) ? 4 : 0; // ppan: first word is sprite count
		INT32 end_offs = m92_sprite_list;
		if (m92_kludge == 3) {
			end_offs = (BURN_ENDIAN_SWAP_INT16(ram[0]) & 0xff) * 4 + 4; // +4 because offs <= amount in MAME
		}

		for (INT32 offs = start_offs; offs < end_offs; )
		{
			INT32 y;
			INT32 x;

			if (m92_kludge == 3) { // ppan - independent sprite offsets
				y = ((384 - 16 - (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0x1ff)) & 0x1ff) - 8;
				x = (BURN_ENDIAN_SWAP_INT16(ram[offs+3]) & 0x1ff) - 96;
				y -= 135; // relative to background: up 15 pixels (matches MAME ppan)
				x += 18;  // +5 relative offset + 13 global shift right
				y &= 0x1ff;
			} else {
				y = (((384 - 16 - (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0x1ff)) - nScreenOffsets[1]) & 0x1ff) - 8;
				x = ((BURN_ENDIAN_SWAP_INT16(ram[offs+3]) & 0x1ff) - nScreenOffsets[0]) - 96;
			}

			INT32 pri_s  = (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0xe000) >> 13;
			INT32 pri_b  = (~BURN_ENDIAN_SWAP_INT16(ram[offs+2]) >> 6) & 2;
			INT32 code   =  BURN_ENDIAN_SWAP_INT16(ram[offs+1]);
			INT32 color  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x007f;

			INT32 flipx  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x0100;
			INT32 flipy  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x0200;
			INT32 y_multi= 1 << ((BURN_ENDIAN_SWAP_INT16(ram[offs+0]) >>  9) & 3);
			INT32 x_multi= 1 << ((BURN_ENDIAN_SWAP_INT16(ram[offs+0]) >> 11) & 3);

			offs += 4 * x_multi;
			if (pri_s != k) continue;
	
			if (flipx) x+=16 * (x_multi - 1);

			for (INT32 j = 0; j < x_multi; j++)
			{
				INT32 s_ptr = j * 8;
				if (!flipy) s_ptr += y_multi-1;

				x &= 0x1ff;

				for (INT32 i=0; i<y_multi; i++)
				{
					RenderTilePrio(pTransDraw, DrvGfxROM1, (code + s_ptr) & graphics_mask[1], color << 4, x    , y-i*16, flipx, flipy, 16, 16, RamPrioBitmap, pri_b);
					if (x > 0x1f0) RenderTilePrio(pTransDraw, DrvGfxROM1, (code + s_ptr) & graphics_mask[1], color << 4, x-512, y-i*16, flipx, flipy, 16, 16, RamPrioBitmap, pri_b);

					if (flipy) s_ptr++; else s_ptr--;
				}

				if (flipx) x-=16; else x+=16;
			}
		}
	}
}

static void draw_layer_byline(INT32 start, INT32 finish, INT32 layer, INT32 forcelayer)
{
	struct _m92_layer *ptr = m92_layers[layer];

	if (ptr->enable == 0) return;

	INT32 wide = ptr->wide;
	INT32 scrolly = (ptr->scrolly + 136 - nScreenOffsets[1]) & 0x1ff;
	INT32 scrollx = ((((ptr->enable_rowscroll) ? 0 : ptr->scrollx) - nScreenOffsets[0]) - (2 * layer - ((wide & 0x80)<<1))) + 80;

	if (m92_kludge == 3) scrollx -= 13; // ppan - shift background right a bit more

	const UINT16 transmask[3][3][2] = { // layer, group, value
		{ { 0xffff, 0x0001 }, { 0x00ff, 0xff01 }, { 0x0001, 0xffff } },
		{ { 0xffff, 0x0001 }, { 0x00ff, 0xff01 }, { 0x0001, 0xffff } },
		{ { 0xffff, 0x0000 }, { 0x00ff, 0xff00 }, { 0x0001, 0xfffe } }
	};

	INT32 priority = forcelayer^1;

	for (INT32 sy = start; sy < finish; sy++)
	{
		UINT16 *dest = pTransDraw + (sy * nScreenWidth);
		UINT8  *pri  = RamPrioBitmap + (sy * nScreenWidth);

		INT32 scrollx_1 = scrollx;
		if (ptr->enable_rowscroll) scrollx_1 += BURN_ENDIAN_SWAP_INT16(ptr->scroll[(sy+scrolly)&0x1ff]);
		INT32 scrolly_1 = (scrolly + sy) & 0x1ff;
		INT32 romoff_1 = (scrolly_1 & 0x07) << 3;

		for (INT32 sx = 0; sx < nScreenWidth + 8; sx+=8)
		{
			INT32 offs  = ((scrolly_1 / 8) * wide) + (((scrollx_1 + sx) / 8) & (wide - 1));
			INT32 attr  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 1]);
			INT32 code  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 0]) | ((attr & 0x8000) << 1);
			INT32 color =(attr & 0x007f) << 4;
			INT32 flipy = attr & 0x0400;
			INT32 flipx = attr & 0x0200;

			INT32 group = 0;
			if (attr & 0x0180) group = (attr & 0x0100) ? 2 : 1;

			{
				INT32 x_xor = 0;
				INT32 romoff = romoff_1;
				if (flipy) romoff ^= 0x38;
				if (flipx) x_xor = 7;

				UINT8 *rom = DrvGfxROM0 + ((code & graphics_mask[0]) * 0x40) + romoff;
				UINT32 mask = transmask[layer][group][forcelayer];

				INT32 xx = sx - (scrollx_1&0x7);

				for (INT32 x = 0; x < 8; x++, xx++) {
					if (xx < 0 || xx >= nScreenWidth) continue;

					INT32 pxl = (rom[x ^ x_xor]&0x0f);
					if (mask & (1 << pxl)) continue;

					dest[xx] = pxl | color;
					pri[xx] |= priority;
				}
			}
		}
	}
}

static void DrawLayers(INT32 start, INT32 finish)
{
	REI_TRACE("f%u layers %d-%d\n", nCurrentFrame, start, finish);
	REI_PROF_BEGIN(layers);
	memset(RamPrioBitmap + (start * nScreenWidth), 0, nScreenWidth * (finish - start)); // clear priority

	if (~nBurnLayer & 1) memset(pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));

	if (~pf_control[3][4] & 0x10) {
		if (nBurnLayer & 1) draw_layer_byline(start, finish, 2, 1);
		if (nBurnLayer & 1) draw_layer_byline(start, finish, 2, 0);
	} else {
		memset(pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));
	}

	if (nBurnLayer & 2) draw_layer_byline(start, finish, 1, 1);
	if (nBurnLayer & 2) draw_layer_byline(start, finish, 1, 0);
	if (nBurnLayer & 4) draw_layer_byline(start, finish, 0, 1);
	if (nBurnLayer & 8) draw_layer_byline(start, finish, 0, 0);
	REI_PROF_END(layers, REI_PROF_LAYERS);
}

static INT32 DrvDraw()
{
	if (bRecalcPalette) {
		for (INT32 i=0; i<0x800;i++)
			DrvPalette[i] = CalcCol(i<<1);
		bRecalcPalette = 0;
	}

//	DrawLayers(0, nScreenHeight);

	REI_PROF_BEGIN(sprites);
	if (nSpriteEnable & 1) draw_sprites();
	REI_PROF_END(sprites, REI_PROF_SPRITES);

	if (m92_ok_to_blank && m92_video_reg & 0x80) BurnTransferClear(0x800); // most-likely probably screen disable (fixes bad fades in nbbatman, rtypeleo)

	BurnTransferCopy(DrvPalette);

	return 0;
}

static INT32 DrvReDraw()
{
	if (bRecalcPalette) {
		for (INT32 i=0; i<0x800;i++)
			DrvPalette[i] = CalcCol(i<<1);
		bRecalcPalette = 0;
	}

	DrawLayers(0, nScreenHeight);

	if (nSpriteEnable & 1) draw_sprites();

	BurnTransferCopy(DrvPalette);

	return 0;
}

static inline void DrvClearOpposites(UINT8* nJoystickInputs)
{
	if ((*nJoystickInputs & 0x03) == 0x03) {
		*nJoystickInputs &= ~0x03;
	}
	if ((*nJoystickInputs & 0x0c) == 0x0c) {
		*nJoystickInputs &= ~0x0c;
	}
}

static void compile_inputs()
{
	memset(DrvInput, 0, 5);

	for (INT32 i = 0; i < 8; i++) {
		DrvInput[0] |= (DrvJoy1[i] & 1) << i;
		DrvInput[1] |= (DrvJoy2[i] & 1) << i;
		DrvInput[2] |= (DrvJoy3[i] & 1) << i;
		DrvInput[3] |= (DrvJoy4[i] & 1) << i;
		DrvInput[4] |= (DrvButton[i] & 1) << i;
	}

	// Clear Opposites
	DrvClearOpposites(&DrvInput[0]);
	DrvClearOpposites(&DrvInput[1]);
}

// ---------------------------------------------------------------------------
// ReiREM Vita port: render capture (docs/DECISIONS.md ADR-007)
// In pipelined mode the main side does not draw. At each point where FBNeo would call
// DrawLayers it records the layer registers and (only if VRAM changed since the last record)
// a VRAM snapshot; at vblank it records the sprite buffer and palette. The render side draws
// from these records exactly as DrawLayers/DrvDraw would have at those moments.

#define REI_MAX_SEGS 256
#define REI_GFX_BUFS 2

struct rei_seg_layer {
	INT32 enable, wide, enable_rowscroll;
	UINT16 scrollx, scrolly;
	INT32 vram_off, scroll_off; // byte offsets into VRAM
};
struct rei_seg {
	INT16 start, finish;
	UINT8 pf3_4, layer_mask;
	UINT32 vlog_pos;            // VRAM writes before this point are applied first
	struct rei_seg_layer l[3];
};
struct rei_gfx {
	INT32 nsegs;
	struct rei_seg segs[REI_MAX_SEGS];
	INT32 has_draw, sprite_list, sprite_enable, blank;
	UINT16 sprbuf[0x400];
	UINT32 pal[0x801];
};
static struct rei_gfx rei_gfx[REI_GFX_BUFS];
static INT64 rei_gfx_produced, rei_gfx_consumed;
static UINT8 *rei_rvram;                    // render side copy of VRAM, kept current by replaying the log

static void ReiVramLogFull()
{
	// render thread: wait until it has applied some entries. single thread: cannot happen within a
	// frame (a frame writes far less than REI_VLOG bytes), so this is only a guard.
	while (rei_vlog_w - REI_LOAD(rei_vlog_r) >= REI_VLOG && rei_render_threaded && !REI_SH(rei_stop))
		if (rei_wait_hook) rei_wait_hook();
}

static void ReiVramLogRoom() // main side: bound reached, look at the render side's progress
{
	ReiVramLogFull();
	rei_vlog_lim = REI_LOAD(rei_vlog_r) + REI_VLOG;
}

static void ReiVramApply(UINT32 upto) // render side
{
	UINT32 r = rei_vlog_r;
	while (r != upto) {
		UINT32 e = rei_vlog[r & (REI_VLOG - 1)];
		rei_rvram[e >> 8] = (UINT8)e;
		r++;
	}
	REI_STORE(rei_vlog_r, r);
}

static struct rei_gfx *ReiCaptureBuf()
{
	return &rei_gfx[rei_gfx_produced % REI_GFX_BUFS];
}

static void ReiCaptureBegin() // start of a main frame
{
	if (!rei_rvram) { // first frame: both sides start from the same VRAM contents
		rei_rvram = (UINT8 *)BurnMalloc(0x10000);
		rei_vlog = (UINT32 *)BurnMalloc(REI_VLOG * sizeof(UINT32));
		memcpy(rei_rvram, DrvVidRAM, 0x10000);
		rei_vlog_w = rei_vlog_r = 0;
		rei_vlog_lim = REI_VLOG;
	}
	struct rei_gfx *g = ReiCaptureBuf();
	g->nsegs = 0;
	g->has_draw = 0;
}

REI_LINE_FN(24) static void ReiCaptureSegment(INT32 start, INT32 finish)
{
	struct rei_gfx *g = ReiCaptureBuf();
	if (g->nsegs >= REI_MAX_SEGS) return;

	REI_TRACE("f%u cap %d-%d seg%d vlog%u\n", nCurrentFrame, start, finish, g->nsegs, rei_vlog_w);
	struct rei_seg *sg = &g->segs[g->nsegs++];
	sg->start = start;
	sg->finish = finish;
	sg->pf3_4 = pf_control[3][4];
	sg->layer_mask = nBurnLayer;
	sg->vlog_pos = rei_vlog_w;
	for (INT32 i = 0; i < 3; i++) {
		struct _m92_layer *ptr = m92_layers[i];
		struct rei_seg_layer *L = &sg->l[i];
		L->enable = ptr->enable;
		L->wide = ptr->wide;
		L->enable_rowscroll = ptr->enable_rowscroll;
		L->scrollx = ptr->scrollx;
		L->scrolly = ptr->scrolly;
		L->vram_off = ptr->vram ? (INT32)((UINT8 *)ptr->vram - DrvVidRAM) : 0;
		L->scroll_off = ptr->scroll ? (INT32)((UINT8 *)ptr->scroll - DrvVidRAM) : 0;
	}
}

static void ReiCaptureFrame() // vblank, what DrvDraw would use
{
	struct rei_gfx *g = ReiCaptureBuf();
	if (bRecalcPalette) {
		for (INT32 i = 0; i < 0x800; i++)
			DrvPalette[i] = CalcCol(i << 1);
		bRecalcPalette = 0;
	}
	memcpy(g->pal, DrvPalette, sizeof(g->pal));
	memcpy(g->sprbuf, DrvSprBuf, 0x800);
	g->sprite_list = m92_sprite_list;
	g->sprite_enable = nSpriteEnable;
	g->blank = (m92_ok_to_blank && (m92_video_reg & 0x80)) ? 1 : 0;
	g->has_draw = 1;
}

// Pixel-value presence per 8-pixel tile row: bit p set if some pixel of the row has value p.
// Built once from the decoded tile ROM (docs/RULES.md 5-2 transparency flags); lets the render skip
// rows that a pass leaves untouched and write fully drawn rows without per-pixel tests.
static UINT16 *rei_row_presence;

static void ReiBuildRowPresence()
{
	INT32 rows = (graphics_mask[0] + 1) * 8;
	rei_row_presence = (UINT16 *)malloc(rows * sizeof(UINT16));
	for (INT32 r = 0; r < rows; r++) {
		const UINT8 *px = DrvGfxROM0 + r * 8;
		UINT16 m = 0;
		for (INT32 x = 0; x < 8; x++) m |= 1 << (px[x] & 0x0f);
		rei_row_presence[r] = m;
	}
}

// draw_layer_byline(forcelayer=1) followed by draw_layer_byline(forcelayer=0) for one layer,
// merged into one pass. Per pixel the result is identical: the forcelayer=0 pass (mask m0,
// priority bit 1) overwrites the forcelayer=1 pass (mask m1, no priority bit). Either pass can be
// disabled to match DrawLayers' nBurnLayer handling.
static void ReiDrawLayerMerged(INT32 start, INT32 finish, INT32 layer, INT32 do1, INT32 do0, const struct rei_seg_layer *ptr, UINT8 *vbase)
{
	if (ptr->enable == 0 || (!do1 && !do0)) return;

	const UINT16 *vram = (const UINT16 *)(vbase + ptr->vram_off);
	const UINT16 *rowscroll = (const UINT16 *)(vbase + ptr->scroll_off);
	INT32 wide = ptr->wide;
	INT32 scrolly = (ptr->scrolly + 136 - nScreenOffsets[1]) & 0x1ff;
	INT32 scrollx = ((((ptr->enable_rowscroll) ? 0 : ptr->scrollx) - nScreenOffsets[0]) - (2 * layer - ((wide & 0x80)<<1))) + 80;

	if (m92_kludge == 3) scrollx -= 13;

	static const UINT16 transmask[3][3][2] = {
		{ { 0xffff, 0x0001 }, { 0x00ff, 0xff01 }, { 0x0001, 0xffff } },
		{ { 0xffff, 0x0001 }, { 0x00ff, 0xff01 }, { 0x0001, 0xffff } },
		{ { 0xffff, 0x0000 }, { 0x00ff, 0xff00 }, { 0x0001, 0xfffe } }
	};

	const INT32 w = nScreenWidth;

	for (INT32 sy = start; sy < finish; sy++)
	{
		UINT16 *dest = pTransDraw + (sy * w);
		UINT8  *pri  = RamPrioBitmap + (sy * w);

		INT32 scrollx_1 = scrollx;
		if (ptr->enable_rowscroll) scrollx_1 += BURN_ENDIAN_SWAP_INT16(rowscroll[(sy+scrolly)&0x1ff]);
		INT32 scrolly_1 = (scrolly + sy) & 0x1ff;
		INT32 row_1 = scrolly_1 & 0x07;

		for (INT32 sx = 0; sx < w + 8; sx+=8)
		{
			INT32 offs  = ((scrolly_1 / 8) * wide) + (((scrollx_1 + sx) / 8) & (wide - 1));
			INT32 attr  = BURN_ENDIAN_SWAP_INT16(vram[(offs * 2) + 1]);
			INT32 code  = BURN_ENDIAN_SWAP_INT16(vram[(offs * 2) + 0]) | ((attr & 0x8000) << 1);
			INT32 color =(attr & 0x007f) << 4;

			INT32 group = 0;
			if (attr & 0x0180) group = (attr & 0x0100) ? 2 : 1;

			// a pass that is disabled behaves like a mask that hides every pixel
			UINT32 m1 = do1 ? transmask[layer][group][1] : 0xffff;
			UINT32 m0 = do0 ? transmask[layer][group][0] : 0xffff;

			INT32 row = row_1;
			if (attr & 0x0400) row ^= 7;                 // flipy
			INT32 tile = code & graphics_mask[0];
			UINT32 pres = rei_row_presence[tile * 8 + row];
			if ((pres & ~(m0 & m1) & 0xffff) == 0) continue; // nothing of this row is drawn

			const UINT8 *rom = DrvGfxROM0 + tile * 0x40 + (row << 3);
			INT32 x_xor = (attr & 0x0200) ? 7 : 0;       // flipx
			INT32 xx = sx - (scrollx_1&0x7);

			if ((pres & m0) == 0 && xx >= 0 && xx + 8 <= w) {
				// every pixel of the row is drawn by the forcelayer=0 pass
				for (INT32 x = 0; x < 8; x++) {
					dest[xx + x] = (rom[x ^ x_xor] & 0x0f) | color;
					pri[xx + x] |= 1;
				}
				continue;
			}

			for (INT32 x = 0; x < 8; x++, xx++) {
				if (xx < 0 || xx >= w) continue;
				INT32 pxl = (rom[x ^ x_xor]&0x0f);
				UINT32 bit = 1u << pxl;
				if (!(m0 & bit)) {
					dest[xx] = pxl | color;
					pri[xx] |= 1;
				} else if (!(m1 & bit)) {
					dest[xx] = pxl | color;
				}
			}
		}
	}
}

// same as DrawLayers
static void ReiDrawLayers(const struct rei_seg *sg, UINT8 *vbase)
{
	REI_PROF_BEGIN(layers);
	if (!rei_row_presence) ReiBuildRowPresence();
	INT32 start = sg->start, finish = sg->finish;
	memset(RamPrioBitmap + (start * nScreenWidth), 0, nScreenWidth * (finish - start));

	if (~sg->layer_mask & 1) memset(pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));

	if (~sg->pf3_4 & 0x10) {
		if (sg->layer_mask & 1) ReiDrawLayerMerged(start, finish, 2, 1, 1, &sg->l[2], vbase);
	} else {
		memset(pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));
	}

	if (sg->layer_mask & 2) ReiDrawLayerMerged(start, finish, 1, 1, 1, &sg->l[1], vbase);
	ReiDrawLayerMerged(start, finish, 0, sg->layer_mask & 4, sg->layer_mask & 8, &sg->l[0], vbase);
	REI_PROF_END(layers, REI_PROF_LAYERS);
}

// same as draw_sprites, reading the recorded sprite buffer
typedef void (*rei_tile_render_fn)(UINT16 *, UINT8 *, INT32, INT32, INT32, INT32, INT32, INT32, INT32, INT32, UINT8 *, INT32);

// PSP port (G2): sprite-only render. Same occupancy rule as RenderTilePrio with an empty priority map
// (an earlier sprite always wins), and the "hidden by high-priority tiles" bit is stored instead of applied:
// value = 0x800 | palette index | (pri_b ? 0x8000 : 0). The GE applies it against the layer stencil.
static void RenderTileS(UINT16 *dest, UINT8 *gfx, INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 width, INT32 height, UINT8 *pri, INT32 prio)
{
	if (sx <= (0-width) || sx >= nScreenWidth || sy <= (0-height) || sy >= nScreenHeight) return;

	INT32 flip = 0;
	if (flipy) flip |= (height - 1) * width;
	if (flipx) flip |= width - 1;

	gfx += code * width * height;
	const UINT16 mark = 0x800 | ((prio & 2) ? 0x8000 : 0);

	for (INT32 y = 0; y < height; y++, sy++) {
		if (sy < 0 || sy >= nScreenHeight) continue;
		INT32 row = sy * nScreenWidth;
		for (INT32 x = 0; x < width; x++, sx++) {
			if (sx < 0 || sx >= nScreenWidth) continue;
			INT32 pxl = gfx[((y * width) + x) ^ flip];
			if (pxl == 0 || dest[row + sx]) continue; // a written pixel is never 0 (0x800 mark): occupied
			dest[row + sx] = pxl | color | mark;
		}
		sx -= width;
	}
}

// PSP port: resumable form. Draws from pass *pk, list offset *poffs (-1 = start of the pass) and returns
// after about `budget` tiles with the position saved; returns 1 when the whole list is drawn. The drawing
// order is the same as one uninterrupted call.
static INT32 ReiDrawSpritesStep(const UINT16 *ram, INT32 sprite_list, UINT16 *dest, UINT8 *prio, rei_tile_render_fn render,
	INT32 *pk, INT32 *poffs, INT32 budget)
{
	for (INT32 k = *pk; k<8; k++)
	{
		INT32 start_offs = (m92_kludge == 3) ? 4 : 0;
		INT32 end_offs = sprite_list;
		if (m92_kludge == 3) {
			end_offs = (BURN_ENDIAN_SWAP_INT16(ram[0]) & 0xff) * 4 + 4;
		}

		INT32 offs0 = (k == *pk && *poffs >= 0) ? *poffs : start_offs;
		for (INT32 offs = offs0; offs < end_offs; )
		{
			if (budget <= 0) {
				*pk = k;
				*poffs = offs;
				return 0;
			}
			INT32 y;
			INT32 x;

			if (m92_kludge == 3) {
				y = ((384 - 16 - (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0x1ff)) & 0x1ff) - 8;
				x = (BURN_ENDIAN_SWAP_INT16(ram[offs+3]) & 0x1ff) - 96;
				y -= 135;
				x += 18;
				y &= 0x1ff;
			} else {
				y = (((384 - 16 - (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0x1ff)) - nScreenOffsets[1]) & 0x1ff) - 8;
				x = ((BURN_ENDIAN_SWAP_INT16(ram[offs+3]) & 0x1ff) - nScreenOffsets[0]) - 96;
			}

			INT32 pri_s  = (BURN_ENDIAN_SWAP_INT16(ram[offs+0]) & 0xe000) >> 13;
			INT32 pri_b  = (~BURN_ENDIAN_SWAP_INT16(ram[offs+2]) >> 6) & 2;
			INT32 code   =  BURN_ENDIAN_SWAP_INT16(ram[offs+1]);
			INT32 color  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x007f;

			INT32 flipx  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x0100;
			INT32 flipy  =  BURN_ENDIAN_SWAP_INT16(ram[offs+2]) & 0x0200;
			INT32 y_multi= 1 << ((BURN_ENDIAN_SWAP_INT16(ram[offs+0]) >>  9) & 3);
			INT32 x_multi= 1 << ((BURN_ENDIAN_SWAP_INT16(ram[offs+0]) >> 11) & 3);

			offs += 4 * x_multi;
			if (pri_s != k) continue;
			budget -= x_multi * y_multi;

			if (flipx) x+=16 * (x_multi - 1);

			for (INT32 j = 0; j < x_multi; j++)
			{
				INT32 s_ptr = j * 8;
				if (!flipy) s_ptr += y_multi-1;

				x &= 0x1ff;

				for (INT32 i=0; i<y_multi; i++)
				{
					render(dest, DrvGfxROM1, (code + s_ptr) & graphics_mask[1], color << 4, x    , y-i*16, flipx, flipy, 16, 16, prio, pri_b);
					if (x > 0x1f0) render(dest, DrvGfxROM1, (code + s_ptr) & graphics_mask[1], color << 4, x-512, y-i*16, flipx, flipy, 16, 16, prio, pri_b);

					if (flipy) s_ptr++; else s_ptr--;
				}

				if (flipx) x-=16; else x+=16;
			}
		}
	}
	*pk = 8;
	return 1;
}

// PSP port (G3): records the tile instead of drawing it - same culling as RenderTileS, same order
static void RenderTileRecord(UINT16 *dest, UINT8 *gfx, INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 width, INT32 height, UINT8 *pri, INT32 prio)
{
	if (sx <= (0-width) || sx >= nScreenWidth || sy <= (0-height) || sy >= nScreenHeight) return;
	if (rei_spr_ntiles >= rei_spr_cap) { rei_spr_overflow = 1; return; }
	rei_spr_tile *t = &rei_spr_tiles[rei_spr_ntiles++];
	t->x = (int16_t)sx;
	t->y = (int16_t)sy;
	t->code = (uint32_t)code;
	t->color = (uint8_t)(color >> 4);
	t->flags = (uint8_t)((flipx ? 1 : 0) | (flipy ? 2 : 0) | ((prio & 2) ? 4 : 0));
	t->pad = 0;
}

extern "C" const UINT8 *rei_m92_sprite_gfx()
{
	return DrvGfxROM1;
}

static void ReiDrawSprites(const UINT16 *ram, INT32 sprite_list, UINT16 *dest, UINT8 *prio, rei_tile_render_fn render)
{
	INT32 k = 0, offs = -1;
	ReiDrawSpritesStep(ram, sprite_list, dest, prio, render, &k, &offs, 0x7fffffff);
}

// draws one recorded frame into fb (nBurnBpp format). Render thread in M3-c.
// PSP port (G2): the frontend draws the tile layers. Each segment's VRAM is replayed and handed to the
// layer callback; sprites go to rei_spr_buf. With rei_ge_ref the CPU also draws the normal frame
// (layers + sprites into pTransDraw) for hashing and pixel comparison.
static void ReiRenderGfxGE(struct rei_gfx *g)
{
	UINT32 prev_pos = 0;
	for (INT32 i = 0; i < g->nsegs; i++) {
		const struct rei_seg *sg = &g->segs[i];
		ReiVramApply(sg->vlog_pos);
		if (rei_layer_cb) {
			rei_layer_seg s;
			s.start = sg->start;
			s.finish = sg->finish;
			s.pf2_off = (sg->pf3_4 & 0x10) ? 1 : 0;
			s.layer_mask = sg->layer_mask;
			s.vram_changed = (i == 0 || sg->vlog_pos != prev_pos) ? 1 : 0;
			for (INT32 l = 0; l < 3; l++) {
				const struct rei_seg_layer *L = &sg->l[l];
				s.l[l].enable = L->enable;
				s.l[l].wide = L->wide;
				s.l[l].rowscroll = L->enable_rowscroll;
				s.l[l].scrollx = L->scrollx;
				s.l[l].scrolly = L->scrolly;
				s.l[l].vram = (const uint16_t *)(rei_rvram + L->vram_off);
				s.l[l].scroll = (const uint16_t *)(rei_rvram + L->scroll_off);
			}
			rei_layer_cb(&s, rei_layer_cb_user);
		}
		prev_pos = sg->vlog_pos;
		if (rei_ge_ref) ReiDrawLayers(sg, rei_rvram);
	}

	if (!g->has_draw) return;

	if (!rei_spr_buf)
		rei_spr_buf = (UINT16 *)BurnMalloc(nScreenWidth * nScreenHeight * sizeof(UINT16));
	REI_PROF_BEGIN(sprites);
	memset(rei_spr_buf, 0, nScreenWidth * nScreenHeight * sizeof(UINT16));
	if (g->sprite_enable & 1) ReiDrawSprites(g->sprbuf, g->sprite_list, rei_spr_buf, NULL, RenderTileS);
	if (rei_ge_ref && (g->sprite_enable & 1)) ReiDrawSprites(g->sprbuf, g->sprite_list, pTransDraw, RamPrioBitmap, RenderTilePrio);
	REI_PROF_END(sprites, REI_PROF_SPRITES);
	if (rei_ge_ref && g->blank) BurnTransferClear(0x800);

	rei_out_pal = g->pal;
	rei_out_blank = g->blank;
}

extern "C" void rei_m92_tile_info(rei_tile_info *out)
{
	out->tiles = DrvGfxROM0;
	out->tile_mask = graphics_mask[0];
	out->ofs_x = nScreenOffsets[0];
	out->ofs_y = nScreenOffsets[1];
}

static void ReiRenderGfx(INT32 buf, UINT8 *fb, INT32 pitch)
{
	struct rei_gfx *g = &rei_gfx[buf];
	if (rei_ge_layers) { ReiRenderGfxGE(g); return; }
	for (INT32 i = 0; i < g->nsegs; i++) {
		ReiVramApply(g->segs[i].vlog_pos);
		ReiDrawLayers(&g->segs[i], rei_rvram);
	}

	if (!g->has_draw || (!fb && !rei_index_out)) return;

	REI_PROF_BEGIN(sprites);
	if (g->sprite_enable & 1) ReiDrawSprites(g->sprbuf, g->sprite_list, pTransDraw, RamPrioBitmap, RenderTilePrio);
	REI_PROF_END(sprites, REI_PROF_SPRITES);

	if (g->blank) BurnTransferClear(0x800);

	if (rei_index_out) { // PSP port: pTransDraw + palette go to the GE (psp/docs/M92_VIDEO.md G1)
		rei_out_pal = g->pal;
		rei_out_blank = g->blank;
		return;
	}

	REI_PROF_BEGIN(xfer);
	const UINT32 *pal = g->pal;
	const UINT16 *src = pTransDraw;
	if (nBurnBpp == 2) {
		for (INT32 y = 0; y < nScreenHeight; y++, src += nScreenWidth, fb += pitch)
			for (INT32 x = 0; x < nScreenWidth; x++)
				((UINT16 *)fb)[x] = (UINT16)pal[src[x]];
	} else {
		for (INT32 y = 0; y < nScreenHeight; y++, src += nScreenWidth, fb += pitch)
			for (INT32 x = 0; x < nScreenWidth; x++)
				((UINT32 *)fb)[x] = pal[src[x]];
	}
	REI_PROF_END(xfer, REI_PROF_XFER);
}

REI_LINE_FN(23) static void scanline_interrupts(INT32 scanline)
{
	if (m92_sprite_buffer_timer) {
		memcpy(DrvSprBuf, DrvSprRAM, 0x800);
		m92_sprite_buffer_busy = 0x80;
		pic8259_set_irq_line(1, 1);

		m92_sprite_buffer_timer = 0;
	}

	if (scanline == m92_raster_irq_position) {
		if (scanline>=8 && scanline < 248 && nPrevScreenPos != (scanline-8)+1) {
			if (nPrevScreenPos >= 0 && nPrevScreenPos <= 239) {
				if (rei_pipelined) ReiCaptureSegment(nPrevScreenPos, (scanline-8)+1); // ReiREM Vita port
				else DrawLayers(nPrevScreenPos, (scanline-8)+1);
			}
			nPrevScreenPos = (scanline-8)+1;
		}

		pic8259_set_irq_line(2, 1);
	}

	if (scanline == 248) // vblank
	{
		if (nPrevScreenPos != 240) {
			if (rei_pipelined) ReiCaptureSegment(nPrevScreenPos, 240); // ReiREM Vita port
			else DrawLayers(nPrevScreenPos, 240);
		}
		nPrevScreenPos = 0;

		if (rei_pipelined) {
			ReiCaptureFrame(); // ReiREM Vita port: sprites/palette drawn by the render side
		} else if (pBurnDraw) {
			DrvDraw();
		}

		pic8259_set_irq_line(0, 1);
	} else {
		pic8259_set_irq_line(0, 0);
	}
}

static INT32 PipelinedFrame();

static INT32 DrvFrame()
{
	if (rei_pipelined) return PipelinedFrame(); // ReiREM Vita port

	if (DrvReset) {
		DrvDoReset();
	}

	nPrevScreenPos = 0;

	VezNewFrame();

	compile_inputs();

	INT32 multiplier=rei_interleave_mul; // ReiREM Vita port: was 8
	nInterleave = 256*multiplier;

	// overclocking...
	nCyclesTotal[0] = (INT32)((INT64)(9000000 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesTotal[1] = (INT32)((INT64)(7159090 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesDone[0] = nCyclesDone[1] = 0;

	for (INT32 i = 0; i < nInterleave; i++)
	{
		VezOpen(0);
		INT32 segment = nCyclesTotal[0] / nInterleave;

		REI_PROF_SAMPLE_BEGIN(v33, i);
		nCyclesDone[0] += VezRun(segment);
		REI_PROF_SAMPLE_END(v33, REI_PROF_V33);
		if ((i%multiplier)==(multiplier-1))
			scanline_interrupts(i/multiplier); // update at hblank?
		VezClose();

		VezOpen(1);
		REI_PROF_SAMPLE_BEGIN(v35, i);
		CPU_RUN_TIMER(1);
		REI_PROF_SAMPLE_END(v35, REI_PROF_V35);
		VezClose();
	}

	VezOpen(1);

	REI_PROF_BEGIN(sound);
	REI_TRACE("f%u v35pc %05x\n", nCurrentFrame, VezGetPC(1));
	if (pBurnSoundOut) {
#ifdef REI_SOUND_SOLO // diagnostics: 1 = YM2151 only, 2 = GA20 only
		if (REI_SOUND_SOLO != 2) BurnYM2151Render(pBurnSoundOut, nBurnSoundLen);
		if (REI_SOUND_SOLO != 1) iremga20_update(0, pBurnSoundOut, nBurnSoundLen);
#else
		BurnYM2151Render(pBurnSoundOut, nBurnSoundLen);
		iremga20_update(0, pBurnSoundOut, nBurnSoundLen);
#endif
	}
	REI_PROF_END(sound, REI_PROF_SOUND);

	VezClose();

	return 0;
}

// ReiREM Vita port: one scanline of the main side (V33 + video). Main thread.
void nec_new_frame();
void v25_new_frame();

// frame line whose slice runs an instruction starting at frame cycle c: the first l with c < target33(l)
static inline INT32 ReiTarget33(INT32 l) { return (INT32)((INT64)(l + 1) * rei_total33 / REI_LINES); }
static inline INT32 ReiTarget35(INT32 l) { return (INT32)((INT64)(l + 1) * rei_total35 / REI_LINES); }
static INT32 ReiLineOfCycle35(INT32 c)
{
	INT32 l = (INT32)((INT64)c * REI_LINES / rei_total35);
	if (l < 0) l = 0;
	if (l > REI_LINES - 1) l = REI_LINES - 1;
	while (l > 0 && c < ReiTarget35(l - 1)) l--;
	while (l < REI_LINES - 1 && c >= ReiTarget35(l)) l++;
	return l;
}

static INT32 ReiLineOfCycle(INT32 c)
{
	INT32 l = (INT32)((INT64)c * REI_LINES / rei_total33);
	if (l < 0) l = 0;
	if (l > REI_LINES - 1) l = REI_LINES - 1;
	while (l > 0 && c < ReiTarget33(l - 1)) l--;
	while (l < REI_LINES - 1 && c >= ReiTarget33(l)) l++;
	return l;
}

#if !defined(__psp__)
static unsigned long long rei_slst[8]; // (diag REI_M92_SLICESTAT) 0 lines, 1 line event runs, 2 batches, 3 batch event runs, 4 batch stalls
#define REI_SLST(i) (rei_slst[i]++)
#else
#define REI_SLST(i) ((void)0)
#endif
REI_LINE_FN(21) static void PipelinedMainLine(INT32 line)
{
	REI_SLST(0);
	const INT64 L = rei_main_line;
	REI_YM_MAIN_POLL();
	// status events from sound lines <= L - delay must exist: sound_done >= L - delay + 1
	if (rei_threaded && REI_LOAD(rei_sound_done) < L - REI_S2M_DELAY_LINES + 1) {
#ifdef REI_PROFILE
		UINT64 t0 = rei_prof_clock();
#endif
		while (REI_LOAD(rei_sound_done) < L - REI_S2M_DELAY_LINES + 1 && !REI_SH(rei_stop)) {
			REI_YM_MAIN_POLL();
			if (rei_wait_hook) rei_wait_hook();
		}
#ifdef REI_PROFILE
		rei_main_wait_sound_us += rei_prof_clock() - t0;
#endif
	}
	REI_PROF_BEGIN(mline);
	const INT64 frame_base = (L / REI_LINES) * rei_total33;
	const INT32 target33 = (INT32)((INT64)(line + 1) * rei_total33 / REI_LINES);

	VezOpen(0);
	for (;;) {
		UINT32 r = rei_s2m_r;
		if (r == REI_LOAD(rei_s2m_w)) break;
		struct rei_s2m_ev *ev = &rei_s2m_q[r & (REI_RING - 1)];
		if (ev->src_line > L - REI_S2M_DELAY_LINES || ev->apply > frame_base + target33) break;
		INT32 left = (INT32)(ev->apply - frame_base) - VezTotalCycles();
		if (left > 0) { REI_SLST(1); VezRun(left); }
		sound_status[0] = ev->data;
		pic8259_set_irq_line(3, 1);
		REI_STORE(rei_s2m_r, r + 1);
	}
	INT32 left = target33 - VezTotalCycles();
	REI_PROF_BEGIN(v33);
#ifdef REI_SPLIT_TEST // experiment: split a line into two slices (measure fixed per-slice cost)
	if (left > 1) {
		VezRun(left / 2);
		INT32 rest = target33 - VezTotalCycles();
		if (rest > 0) VezRun(rest);
	} else
#endif
	if (left > 0) VezRun(left);
	REI_PROF_END(v33, REI_PROF_V33);
	scanline_interrupts(line);
	VezClose();

	rei_main_line = L + 1;
	REI_STORE(rei_main_done, L + 1);
	REI_PROF_END(mline, REI_PROF_MLINE);
}

// ReiREM PSP port: main lines [line, end) in one V33 slice. Only for lines whose end needs nothing (not the raster
// IRQ line, not 248/249 - see scanline_interrupts): there the per-line slices only cut the V33 run, and the same
// instructions run either way. Status events from the sound side are applied at the same instruction boundary as
// in PipelinedMainLine: at the first boundary at or after max(apply, start of the line where the event becomes
// visible). A write that acts at the end of its line (sprite DMA, raster IRQ position) stops the slice
// (ReiSpanStop); that line is then finished like a normal line and the span ends there. The PC harness checks
// that the result is the same (frame and audio hashes). Returns the next line to run.
extern "C" { INT32 rei_span_enabled = 1, rei_span35_enabled = 1, rei_span_max_lines = 0; }
REI_LINE_FN(22) static INT32 PipelinedMainSpan(INT32 line, INT32 end)
{
	const INT64 L = rei_main_line;                      // absolute line of `line`
	const INT64 line0 = L - line;                        // absolute line of frame line 0
	const INT64 Llast = line0 + end - 1;
	// status events from sound lines <= Llast - delay must exist (as PipelinedMainLine for the span's last line)
	if (rei_threaded && REI_LOAD(rei_sound_done) < Llast - REI_S2M_DELAY_LINES + 1) {
#ifdef REI_PROFILE
		UINT64 t0 = rei_prof_clock();
#endif
		while (REI_LOAD(rei_sound_done) < Llast - REI_S2M_DELAY_LINES + 1 && !REI_SH(rei_stop)) {
			REI_YM_MAIN_POLL();
			if (rei_wait_hook) rei_wait_hook();
		}
#ifdef REI_PROFILE
		rei_main_wait_sound_us += rei_prof_clock() - t0;
#endif
	}
	REI_PROF_BEGIN(mline);
	const INT64 frame_base = (L / REI_LINES) * rei_total33;
	REI_SLST(2);

	VezOpen(0);
	rei_span_line0 = line0;
	rei_span_stop = 0;
	rei_span_active = 1;
	rei_v33_span_active = 1;
	INT32 last = end - 1;
	REI_PROF_BEGIN(v33);
	for (;;) {
		const INT32 tgt = ReiTarget33(last);
		UINT32 r = rei_s2m_r;
		if (r != REI_LOAD(rei_s2m_w)) {
			struct rei_s2m_ev *ev = &rei_s2m_q[r & (REI_RING - 1)];
			if (ev->src_line <= line0 + last - REI_S2M_DELAY_LINES) {
				INT64 eff = ev->apply;
				const INT64 vis = ev->src_line + REI_S2M_DELAY_LINES - line0; // frame line where it becomes visible
				if (vis > 0) {
					const INT64 start = frame_base + ReiTarget33((INT32)vis - 1);
					if (eff < start) eff = start;
				}
				if (eff <= frame_base + tgt) {
					INT32 left = (INT32)(eff - frame_base) - VezTotalCycles();
					if (left > 0) { REI_SLST(3); VezRun(left); }
					if (rei_span_stop) { // ended before the event: the span ends at that line
						rei_span_stop = 0;
						last = rei_span_stop_line;
						continue;
					}
					sound_status[0] = ev->data;
					pic8259_set_irq_line(3, 1);
					REI_STORE(rei_s2m_r, r + 1);
					continue;
				}
			}
		}
		INT32 left = tgt - VezTotalCycles();
		if (left > 0) VezRun(left);
		if (rei_span_stop) {
			REI_SLST(4);
			rei_span_stop = 0;
			last = rei_span_stop_line; // finish that line normally (it may have more events)
			continue;
		}
		break;
	}
	rei_span_active = 0;
	rei_v33_span_active = 0;
	REI_PROF_END(v33, REI_PROF_V33);
	// the ends of the lines before `last` (scanline_interrupts with nothing to do there: IRQ0 low again)
	for (INT32 l = line; l < last; l++)
		pic8259_set_irq_line(0, 0);
	scanline_interrupts(last);
	VezClose();

	rei_main_line = line0 + last + 1;
	REI_STORE(rei_main_done, rei_main_line);
	REI_PROF_END(mline, REI_PROF_MLINE);
	return last + 1;
}

// a line whose end does something (scanline_interrupts) runs on its own
static inline INT32 ReiLineAlone(INT32 l)
{
	return l == m92_raster_irq_position || l == 248 || l == 249 || m92_sprite_buffer_timer;
}

// main lines [first, end): spans where possible
REI_LINE_FN(20) static void PipelinedMainLines(INT32 first, INT32 end)
{
#if !defined(__psp__)
	{
		static int on = -1, last;
		if (on < 0) on = getenv("REI_M92_SLICESTAT") != NULL;
		if (on && (int)nCurrentFrame - last >= 600) {
			last = nCurrentFrame;
			extern unsigned long long rei_vez_runs[4];
			static unsigned long long v0;
			fprintf(stderr, "slicestat f%d: lines %llu line-ev %llu spans %llu span-ev %llu span-stop %llu vezrun0 %llu sprbuf %d raster %d\n", (int)nCurrentFrame,
				rei_slst[0], rei_slst[1], rei_slst[2], rei_slst[3], rei_slst[4], rei_vez_runs[0] - v0, m92_sprite_buffer_timer, m92_raster_irq_position);
			v0 = rei_vez_runs[0];
			memset(rei_slst, 0, sizeof(rei_slst));
		}
	}
#endif
	for (INT32 line = first; line < end && !REI_SH(rei_stop);) {
		INT32 e = line;
		if (rei_span_enabled
#ifdef REI_YM_SPLIT
		    && !rei_ym_split_on
#endif
		) {
			// no further than the sound side already allows (the span needs its events: sound_done >= its last line -
			// delay + 1). The sound side only runs up to the main side's published lines, so a longer span would wait
			// for itself; when the sound side is behind the span just gets shorter (down to line by line)
			INT32 lim = end;
			if (rei_threaded) {
				const INT64 line0 = rei_main_line - line;
				const INT64 last_ok = REI_LOAD(rei_sound_done) + REI_S2M_DELAY_LINES - 1 - line0;
				if (last_ok + 1 < lim)
					lim = (INT32)(last_ok + 1);
				// the sound side sees a span's lines only when it ends: a long span makes it wait for all of them
				// (and then the main side for it). rei_span_max_lines cuts spans shorter (same result, see above)
				if (rei_span_max_lines > 0 && line + rei_span_max_lines < lim)
					lim = line + rei_span_max_lines;
			}
			while (e < lim && !ReiLineAlone(e))
				e++;
		}
		if (e - line >= 2) {
			line = PipelinedMainSpan(line, e);
		} else {
			PipelinedMainLine(line);
			line++;
		}
	}
}

// ReiREM Vita port: one scanline of the sound side (V35 + timers). Sound thread.
static void PipelinedSoundLine(INT32 line)
{
	const INT64 L = rei_sound_line;
	if (rei_threaded) { // latch events from main lines <= L must exist
		if (REI_LOAD(rei_main_done) < L + 1) {
			REI_PROF_BEGIN(swait);
			REI_STORE(rei_sound_waiting, L); // save states: sound side is idle here (see rei_m92_pipe_synced)
			while (REI_LOAD(rei_main_done) < L + 1 && !REI_SH(rei_stop))
				if (rei_wait_hook) rei_wait_hook();
			REI_STORE(rei_sound_waiting, (INT64)-1);
			REI_PROF_END(swait, REI_PROF_SWAIT);
		}
	}
	REI_PROF_BEGIN(sline);
	REI_PROF_BEGIN(v35);
	if (line == 0) v25_new_frame();

	VezOpen(1);
	for (;;) {
		UINT32 r = rei_m2s_r;
		if (r == REI_LOAD(rei_m2s_w)) break;
		struct rei_m2s_ev *ev = &rei_m2s_q[r & (REI_RING - 1)];
		if (ev->line > L) break;
		INT32 t35 = (INT32)((INT64)ev->cyc33 * rei_total35 / rei_total33);
		if (t35 > rei_total35) t35 = rei_total35;
		BurnTimerUpdate(t35);
		sound_latch[0] = ev->data;
		VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_ACK);
		VezRun(10);
		VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
		REI_STORE(rei_m2s_r, r + 1);
	}
	if (line == REI_LINES - 1) {
		BurnTimerEndFrame(rei_total35);
	} else {
		BurnTimerUpdate((INT32)((INT64)(line + 1) * rei_total35 / REI_LINES));
	}
	VezClose();
	REI_PROF_END(v35, REI_PROF_V35);

	rei_sound_line = L + 1;
	REI_STORE(rei_sound_done, L + 1);
	REI_PROF_END(sline, REI_PROF_SLINE);
}

// ReiREM PSP port: sound lines [line, end) with fewer V35 slices (as PipelinedMainSpan for the main side). A sound
// line ends with BurnTimerUpdate(its end). That cut only matters where a YM2151 timer can fire (BurnTimer converts
// between cycles and timer ticks with rounding, so a timer can be found expired at a line cut) and before a line
// with latches from the main side (they are applied after the previous line's cut). Every other line's cut is left
// out; the V35 then simply runs on to the next cut, through the same instructions. Not for line 0 (new frame) and
// the last line (end of frame). A status write records the line of its instruction. Returns end.
#define REI_SPAN35_TIMER_MARGIN 1024 // cycles: lines whose end is this close to the next timer cut keep their cut
static INT32 PipelinedSoundSpan(INT32 line, INT32 end)
{
	const INT64 L = rei_sound_line;
	const INT64 line0 = L - line;
	const INT64 Llast = line0 + end - 1;
	if (rei_threaded && REI_LOAD(rei_main_done) < Llast + 1) { // latch events from main lines <= Llast must exist
		REI_PROF_BEGIN(swait);
		REI_STORE(rei_sound_waiting, L);
		while (REI_LOAD(rei_main_done) < Llast + 1 && !REI_SH(rei_stop))
			if (rei_wait_hook) rei_wait_hook();
		REI_STORE(rei_sound_waiting, (INT64)-1);
		REI_PROF_END(swait, REI_PROF_SWAIT);
	}
	REI_PROF_BEGIN(sline);
	REI_PROF_BEGIN(v35);
	VezOpen(1);
	rei_span35_line0 = line0;
	rei_span35_active = 1;
	INT32 owed = -1; // a line whose cut was left out (the V35 has not been run to its end yet)
	for (INT32 l = line; l < end; l++) {
		// latches of line l (as PipelinedSoundLine): after the previous line's cut
		for (;;) {
			UINT32 r = rei_m2s_r;
			if (r == REI_LOAD(rei_m2s_w)) break;
			struct rei_m2s_ev *ev = &rei_m2s_q[r & (REI_RING - 1)];
			if (ev->line > line0 + l) break;
			if (owed >= 0) {
				BurnTimerUpdate(ReiTarget35(owed));
				owed = -1;
			}
			INT32 t35 = (INT32)((INT64)ev->cyc33 * rei_total35 / rei_total33);
			if (t35 > rei_total35) t35 = rei_total35;
			BurnTimerUpdate(t35);
			sound_latch[0] = ev->data;
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_ACK);
			VezRun(10);
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
			REI_STORE(rei_m2s_r, r + 1);
		}
		const INT32 tgt = ReiTarget35(l);
		// cut here: last line of the span, a timer near, or latches in the next line
		INT32 cut = (l == end - 1) || BurnTimerNextCycles() <= tgt + REI_SPAN35_TIMER_MARGIN;
		if (!cut) {
			UINT32 r = rei_m2s_r;
			if (r != REI_LOAD(rei_m2s_w) && rei_m2s_q[r & (REI_RING - 1)].line <= line0 + l + 1)
				cut = 1;
		}
		if (cut) {
			if (owed >= 0) BurnTimerUpdate(ReiTarget35(owed)); // (the cuts before a timer stay in place too)
			owed = -1;
			BurnTimerUpdate(tgt);
		} else {
			owed = l;
			if (BurnTimerNextCycles() <= ReiTarget35(l + 1) + REI_SPAN35_TIMER_MARGIN) { // timer near the next line
				BurnTimerUpdate(tgt);
				owed = -1;
			}
		}
	}
	rei_span35_active = 0;
	VezClose();
	REI_PROF_END(v35, REI_PROF_V35);

	rei_sound_line = Llast + 1;
	REI_STORE(rei_sound_done, Llast + 1);
	REI_PROF_END(sline, REI_PROF_SLINE);
	return end;
}

// sound lines [first, end): spans where possible
static void PipelinedSoundLines(INT32 first, INT32 end)
{
	for (INT32 line = first; line < end && !REI_SH(rei_stop);) {
		INT32 e = line;
		if (rei_span_enabled && rei_span35_enabled && line > 0
#ifdef REI_YM_SPLIT
		    && !rei_ym_split_on
#endif
		) {
			INT32 lim = end < REI_LINES - 1 ? end : REI_LINES - 1; // the last line ends the frame: on its own
			if (rei_threaded) { // only as far as the main side has published (else it would wait for itself)
				const INT64 line0 = rei_sound_line - line;
				const INT64 last_ok = REI_LOAD(rei_main_done) - 1 - line0;
				if (last_ok + 1 < lim)
					lim = (INT32)(last_ok + 1);
			}
			e = lim;
		}
		if (e - line >= 2) {
			line = PipelinedSoundSpan(line, e);
		} else {
			PipelinedSoundLine(line);
			line++;
		}
	}
}

// ReiREM Vita port: reset from the frontend menu. With threads, the sound and render threads must be
// stopped first; everything both sides share (event rings, line counters, VRAM log, recorded frames)
// starts over as at power-on.
extern "C" void rei_m92_reset(void)
{
	DrvDoReset();
	if (!rei_pipelined)
		return;
	rei_m2s_w = rei_m2s_r = rei_s2m_w = rei_s2m_r = 0;
	rei_main_line = rei_sound_line = rei_main_done = rei_sound_done = 0;
	rei_vlog_w = rei_vlog_r = 0;
	if (rei_rvram)
		memcpy(rei_rvram, DrvVidRAM, 0x10000);
	rei_gfx_produced = rei_gfx_consumed = 0;
}

// ReiREM Vita port: save states. Taken at a frame boundary when the sound side has caught up with
// the main side (rei_sound_done == rei_main_done, true while the frontend menu is open and always in
// single-thread mode). The latch ring is empty then; status replies still waiting for the 32-line delay
// are kept, with line numbers and cycle positions rebased so the loaded state starts at line 0.
struct rei_pipe_state { INT32 count; struct rei_s2m_ev ev[REI_RING]; };

extern "C" INT32 rei_m92_pipe_state_size(void) { return (INT32)sizeof(struct rei_pipe_state); }

// 1 when the sound side has finished everything up to the main side (incl. the frame's audio mix) and
// touches no emulation state. Main thread, between frames.
extern "C" INT32 rei_m92_pipe_synced(void)
{
	if (!rei_pipelined || !rei_threaded)
		return 1;
	return REI_LOAD(rei_sound_done) == rei_main_done && REI_LOAD(rei_sound_waiting) == rei_main_done;
}

// diagnostics (PSP watchdog / exit log): lines published by each side, the line the sound side waits at (-1 = none)
extern "C" void rei_m92_pipe_lines(INT64 *main_done, INT64 *sound_done, INT64 *sound_waiting)
{
	*main_done = REI_LOAD(rei_main_done);
	*sound_done = REI_LOAD(rei_sound_done);
	*sound_waiting = REI_LOAD(rei_sound_waiting);
}

extern "C" INT32 rei_m92_pipe_state_save(void *out)
{
	if (!rei_pipelined)
		return 0;
	if (REI_LOAD(rei_sound_done) != rei_main_done || rei_m2s_r != rei_m2s_w)
		return -1; // not at a consistent point
	// (out follows the scan data in the state buffer and is not aligned: build the block in an aligned copy and copy the
	// bytes - an unaligned 32/64-bit store faults on the PSP's MIPS)
	static struct rei_pipe_state tmp;
	struct rei_pipe_state *ps = &tmp;
	const INT64 L0 = rei_main_line, cyc0 = (L0 / REI_LINES) * rei_total33;
	ps->count = 0;
	for (UINT32 r = rei_s2m_r; r != REI_LOAD(rei_s2m_w); r++) {
		struct rei_s2m_ev e = rei_s2m_q[r & (REI_RING - 1)];
		e.src_line -= L0;
		e.apply -= cyc0;
		ps->ev[ps->count++] = e;
	}
	memcpy(out, ps, sizeof(*ps));
	return 0;
}

extern "C" void rei_m92_pipe_state_load(const void *in) // threads must be stopped
{
	if (!rei_pipelined)
		return;
	static struct rei_pipe_state tmp; // (in is not aligned, see rei_m92_pipe_state_save)
	memcpy(&tmp, in, sizeof(tmp));
	const struct rei_pipe_state *ps = &tmp;
	rei_m2s_w = rei_m2s_r = rei_s2m_w = rei_s2m_r = 0;
	rei_main_line = rei_sound_line = rei_main_done = rei_sound_done = 0;
	for (INT32 i = 0; i < ps->count && i < REI_RING; i++)
		rei_s2m_q[rei_s2m_w++] = ps->ev[i];
	rei_vlog_w = rei_vlog_r = 0;
	if (rei_rvram)
		memcpy(rei_rvram, DrvVidRAM, 0x10000);
	rei_gfx_produced = rei_gfx_consumed = 0;
	bRecalcPalette = 1;
}

// ReiREM Vita port: start of a main frame (main thread)
static void PipelinedMainBegin()
{
	if (DrvReset && !rei_threaded) {
		DrvDoReset();
		rei_m2s_w = rei_m2s_r = rei_s2m_w = rei_s2m_r = 0;
		rei_main_line = rei_sound_line = rei_main_done = rei_sound_done = 0;
	}
	if (rei_render_threaded && REI_LOAD(rei_gfx_produced) - REI_LOAD(rei_gfx_consumed) >= REI_GFX_BUFS) {
#ifdef REI_PROFILE
		UINT64 t0 = rei_prof_clock();
#endif
		// the capture buffer must have been drawn already
		while (REI_LOAD(rei_gfx_produced) - REI_LOAD(rei_gfx_consumed) >= REI_GFX_BUFS && !REI_SH(rei_stop)) {
			REI_YM_MAIN_POLL();
			if (rei_wait_hook) rei_wait_hook();
		}
#ifdef REI_PROFILE
		rei_main_wait_render_us += rei_prof_clock() - t0;
#endif
	}
	ReiCaptureBegin();
	nPrevScreenPos = 0;
	nec_new_frame();
	compile_inputs();
	nCyclesTotal[0] = rei_total33;
	nCyclesTotal[1] = rei_total35;
}

// ReiREM Vita port: end of a sound frame - mix YM2151 + GA20 into pBurnSoundOut (sound thread)
static void PipelinedSoundEnd(INT16 *out, INT32 len)
{
	VezOpen(1);
	REI_TRACE("f%u v35pc %05x\n", nCurrentFrame, VezGetPC(1));
	REI_PROF_BEGIN(sound);
	if (out) {
#ifdef REI_SOUND_SOLO // diagnostics: 1 = YM2151 only, 2 = GA20 only
		if (REI_SOUND_SOLO != 2) BurnYM2151Render(out, len);
		if (REI_SOUND_SOLO != 1) iremga20_update(0, out, len);
#else
		REI_PROF_BEGIN(ymend);
		BurnYM2151Render(out, len);
		REI_PROF_END(ymend, REI_PROF_YMEND);
		REI_PROF_BEGIN(ga20);
		iremga20_update(0, out, len);
		REI_PROF_END(ga20, REI_PROF_GA20);
#endif
	}
	REI_PROF_END(sound, REI_PROF_SOUND);
	VezClose();
}

// ReiREM Vita port: end of a main frame - publish the recorded frame, and draw it here
// unless a render thread does that
static void PipelinedMainEnd()
{
	INT64 n = rei_gfx_produced;
	REI_STORE(rei_gfx_produced, n + 1);
	if (!rei_render_threaded) {
		ReiRenderGfx((INT32)(n % REI_GFX_BUFS), pBurnDraw, nBurnPitch);
		REI_STORE(rei_gfx_consumed, n + 1);
	}
}

// ReiREM Vita port: render thread - wait for the next recorded frame and draw it into fb.
// Returns 0 when stopped.
extern "C" int rei_m92_render_next(UINT8 *fb, INT32 pitch)
{
	INT64 c = rei_gfx_consumed;
	while (REI_LOAD(rei_gfx_produced) <= c) {
		if (REI_SH(rei_stop)) return 0;
		if (rei_wait_hook) rei_wait_hook();
	}
	ReiRenderGfx((INT32)(c % REI_GFX_BUFS), fb, pitch);
	REI_STORE(rei_gfx_consumed, c + 1);
	return 1;
}

// PSP port: the oldest recorded frame drawn a piece at a time (ge_layers + render_threaded). The frontend
// calls this while the main CPU waits for the sound side, so drawing overlaps the Media Engine. Pieces: one
// layer segment, the sprite buffer clear, about REI_RSTEP_TILES sprite tiles. Same result as ReiRenderGfxGE.
// Returns -1 when no recorded frame waits, 1 after a piece, 0 when this call finished the frame.
#define REI_RSTEP_TILES 24
static struct { INT32 active, buf, seg, stage, k, offs; UINT32 prev_pos; } rei_rs;

extern "C" INT32 rei_m92_render_step()
{
	if (!rei_rs.active) {
		INT64 c = rei_gfx_consumed;
		if (REI_LOAD(rei_gfx_produced) <= c) return -1;
		rei_rs.active = 1;
		rei_rs.buf = (INT32)(c % REI_GFX_BUFS);
		rei_rs.seg = 0;
		rei_rs.stage = 0;
		rei_rs.prev_pos = 0;
	}
	struct rei_gfx *g = &rei_gfx[rei_rs.buf];
	switch (rei_rs.stage) {
	case 0: // one segment's layers
		if (rei_rs.seg < g->nsegs) {
			const struct rei_seg *sg = &g->segs[rei_rs.seg];
			ReiVramApply(sg->vlog_pos);
			if (rei_layer_cb) {
				rei_layer_seg s;
				s.start = sg->start;
				s.finish = sg->finish;
				s.pf2_off = (sg->pf3_4 & 0x10) ? 1 : 0;
				s.layer_mask = sg->layer_mask;
				s.vram_changed = (rei_rs.seg == 0 || sg->vlog_pos != rei_rs.prev_pos) ? 1 : 0;
				for (INT32 l = 0; l < 3; l++) {
					const struct rei_seg_layer *L = &sg->l[l];
					s.l[l].enable = L->enable;
					s.l[l].wide = L->wide;
					s.l[l].rowscroll = L->enable_rowscroll;
					s.l[l].scrollx = L->scrollx;
					s.l[l].scrolly = L->scrolly;
					s.l[l].vram = (const uint16_t *)(rei_rvram + L->vram_off);
					s.l[l].scroll = (const uint16_t *)(rei_rvram + L->scroll_off);
				}
				rei_layer_cb(&s, rei_layer_cb_user);
			}
			rei_rs.prev_pos = sg->vlog_pos;
			if (rei_ge_ref) ReiDrawLayers(sg, rei_rvram);
			rei_rs.seg++;
			return 1;
		}
		if (!g->has_draw) break;
		rei_rs.stage = 1;
		// fall through
	case 1: // clear the sprite buffer
		if (rei_ge_sprites) { // G3: record the tiles (cheap, one piece)
			rei_spr_ntiles = 0;
			rei_spr_overflow = 0;
			rei_rs.k = 0;
			rei_rs.offs = -1;
			if (g->sprite_enable & 1)
				ReiDrawSpritesStep(g->sprbuf, g->sprite_list, NULL, NULL, RenderTileRecord, &rei_rs.k, &rei_rs.offs, 0x7fffffff);
			if (rei_ge_ref) { // verification: also the CPU sprite buffer, to compare with the drawn tile list
				if (!rei_spr_buf)
					rei_spr_buf = (UINT16 *)BurnMalloc(nScreenWidth * nScreenHeight * sizeof(UINT16));
				memset(rei_spr_buf, 0, nScreenWidth * nScreenHeight * sizeof(UINT16));
				if (g->sprite_enable & 1) ReiDrawSprites(g->sprbuf, g->sprite_list, rei_spr_buf, NULL, RenderTileS);
			}
			rei_rs.stage = 3;
			return 1;
		}
		if (!rei_spr_buf)
			rei_spr_buf = (UINT16 *)BurnMalloc(nScreenWidth * nScreenHeight * sizeof(UINT16));
		memset(rei_spr_buf, 0, nScreenWidth * nScreenHeight * sizeof(UINT16));
		rei_rs.stage = 2;
		rei_rs.k = 0;
		rei_rs.offs = -1;
		if (!(g->sprite_enable & 1)) rei_rs.stage = 3;
		return 1;
	case 2: { // sprites
		REI_PROF_BEGIN(sprites);
		INT32 done = ReiDrawSpritesStep(g->sprbuf, g->sprite_list, rei_spr_buf, NULL, RenderTileS, &rei_rs.k, &rei_rs.offs, REI_RSTEP_TILES);
		REI_PROF_END(sprites, REI_PROF_SPRITES);
		if (!done) return 1;
		rei_rs.stage = 3;
	}
		// fall through
	case 3: // CPU reference frame (verification only)
		if (rei_ge_ref && (g->sprite_enable & 1)) ReiDrawSprites(g->sprbuf, g->sprite_list, pTransDraw, RamPrioBitmap, RenderTilePrio);
		if (rei_ge_ref && g->blank) BurnTransferClear(0x800);
		break;
	}
	rei_out_pal = g->pal;
	rei_out_blank = g->blank;
	rei_rs.active = 0;
	REI_STORE(rei_gfx_consumed, rei_gfx_consumed + 1);
	return 0;
}

// ReiREM Vita port: whole main frame (threaded mode, main thread)
extern "C" void rei_m92_main_frame()
{
	PipelinedMainBegin();
	PipelinedMainLines(0, REI_LINES);
	PipelinedMainEnd();
}

// ReiREM Vita port: whole sound frame (threaded mode, sound thread)
extern "C" void rei_m92_sound_frame(INT16 *out, INT32 len)
{
	PipelinedSoundLines(0, REI_LINES);
	PipelinedSoundEnd(out, len);
}

// single thread: alternate one main line and one sound line (same result as threaded)
static INT32 PipelinedFrame()
{
	PipelinedMainBegin();
	// Run the main side REI_S2M_DELAY_LINES lines ahead, then the sound side over the same lines.
	// The event protocol makes the result independent of the interleaving (same as the threaded mode),
	// and batching avoids switching between the V33 and V35 interpreters every line - on the PSP
	// (16KB I-cache, ~100KB per interpreter) that switch alone was a large part of both CPUs' time.
	for (INT32 base = 0; base < REI_LINES; base += REI_S2M_DELAY_LINES) {
		const INT32 e = base + REI_S2M_DELAY_LINES < REI_LINES ? base + REI_S2M_DELAY_LINES : REI_LINES;
		PipelinedMainLines(base, e);
		PipelinedSoundLines(base, e);
	}
	PipelinedMainEnd();
	PipelinedSoundEnd(pBurnSoundOut, nBurnSoundLen);
	return 0;
}

static INT32 PpanFrame()
{
	if (DrvReset) {
		DrvDoReset();
	}

	compile_inputs();

	// overclocking...
	nCyclesTotal[0] = (INT32)((INT64)(9000000 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesDone[0] = 0;

	VezOpen(0);

	for (INT32 i = 0; i < nInterleave; i++)
	{
		nCyclesDone[0] += VezRun(nCyclesTotal[0] / nInterleave);

		scanline_interrupts(i);
	}

	if (pBurnSoundOut) {
		MSM6295Render(0, pBurnSoundOut, nBurnSoundLen);
	}
	
	VezClose();

	return 0;
}

static INT32 DrvScan(INT32 nAction, INT32 *pnMin)
{
	if (pnMin)
	{
		*pnMin =  0x029737;
	}

	struct BurnArea ba;

	if (nAction & ACB_MEMORY_RAM)
	{	
		memset(&ba, 0, sizeof(ba));
		ba.Data	  = RamStart;
		ba.nLen	  = RamEnd-RamStart;
		ba.szName = "All Ram";
		BurnAcb(&ba);
#ifdef REI_ME_SPLIT
		ScanVar(rei_me_sound_latch, sizeof(rei_me_sound_latch), "sound latch"); // PSP: outside "All Ram"
#endif

		ScanVar(m92_layers[0], STRUCT_SIZE_HELPER(_m92_layer, scrolly), "m92 pf0");
		ScanVar(m92_layers[1], STRUCT_SIZE_HELPER(_m92_layer, scrolly), "m92 pf1");
		ScanVar(m92_layers[2], STRUCT_SIZE_HELPER(_m92_layer, scrolly), "m92 pf2");

		if (nAction & ACB_WRITE) {
			struct _m92_layer *ptr;
			for (INT32 i = 0; i < 3; i++) {
				ptr = m92_layers[i];
				ptr->scroll = (UINT16*)(DrvVidRAM + (0xf400 + 0x400 * i));
			}

			set_pf_info(0, pf_control[3][0]);
			set_pf_info(1, pf_control[3][2]);
			set_pf_info(2, pf_control[3][4]);
		}
	}

	if (nAction & ACB_DRIVER_DATA)
	{
		VezScan(nAction);

		iremga20_scan(nAction, pnMin);
		BurnYM2151Scan(nAction, pnMin);

		pic8259_scan(nAction);

		SCAN_VAR(PalBank);

		SCAN_VAR(m92_raster_irq_position);
		SCAN_VAR(sprite_extent);
		SCAN_VAR(m92_sprite_list);
		SCAN_VAR(m92_sprite_buffer_busy);
		SCAN_VAR(m92_sprite_buffer_timer);
		SCAN_VAR(m92_main_bank);

		if (nAction & ACB_WRITE) {
			VezOpen(0);
			if (m92_banks) m92MainBank(m92_main_bank);
			VezClose();
		}

		if (m92_kludge == 3) { // ppan
			MSM6295Scan(nAction, pnMin);

			SCAN_VAR(msm6295_bank);

			INT32 temp = msm6295_bank;
			msm6295_bank = -1;
			m92WritePort(0x10, temp);
		}
	}

	return 0;
}


//-----------------------------------------------------------------------------------------------------------------------------


// Hook (World)

static struct BurnRomInfo hookRomDesc[] = {
	{ "hook_-h0-d.ic25",	0x040000, 0x40189ff6, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "hook_-l0-d.ic38",	0x040000, 0x14567690, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "hook_-h1-.ic24",		0x020000, 0x264ba1f0, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "hook_-l1-.ic37",		0x020000, 0xf9913731, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "hook_-sh0-.ic27",	0x010000, 0x86a4e56e, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "hook_-sl0-.ic28",	0x010000, 0x10fd9676, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hook_-c0-.ic4",		0x040000, 0xdec63dcf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hook_-c1-.ic3",		0x040000, 0xe4eb0b92, 3 | BRF_GRA },           //  7
	{ "hook_-c2-.ic2",		0x040000, 0xa52b320b, 3 | BRF_GRA },           //  8
	{ "hook_-c3-.ic1",		0x040000, 0x7ef67731, 3 | BRF_GRA },           //  9

	{ "hook_-000-.ic33",	0x100000, 0xccceac30, 4 | BRF_GRA },           // 10 Sprites
	{ "hook_-010-.ic34",	0x100000, 0x8ac8da67, 4 | BRF_GRA },           // 11
	{ "hook_-020-.ic35",	0x100000, 0x8847af9a, 4 | BRF_GRA },           // 12
	{ "hook_-030-.ic36",	0x100000, 0x239e877e, 4 | BRF_GRA },           // 13

	{ "hook_-da-.ic11",		0x080000, 0x88cd0212, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },           // 18
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(hook)
STD_ROM_FN(hook)

static INT32 hookRomLoad()
{
	return RomLoad(0x080000, 0x040000, 0x100000, 0, 0);
}

static INT32 hookInit()
{
	return DrvInit(hookRomLoad, hook_decryption_table, 1, 0x100000, 0x400000);
}

struct BurnDriver BurnDrvHook = {
	"hook", NULL, NULL, NULL, "1992",
	"Hook (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookRomInfo, hookRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, HookDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (US)

static struct BurnRomInfo hookuRomDesc[] = {
	{ "hook_-h0-c.ic25",	0x040000, 0x84cc239e, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "hook_-l0-c.ic38",	0x040000, 0x45e194fe, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "hook_-h1-.ic24",		0x020000, 0x264ba1f0, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "hook_-l1-.ic37",		0x020000, 0xf9913731, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "hook_-sh0-.ic27",	0x010000, 0x86a4e56e, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "hook_-sl0-.ic28",	0x010000, 0x10fd9676, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hook_-c0-.ic4",		0x040000, 0xdec63dcf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hook_-c1-.ic3",		0x040000, 0xe4eb0b92, 3 | BRF_GRA },           //  7
	{ "hook_-c2-.ic2",		0x040000, 0xa52b320b, 3 | BRF_GRA },           //  8
	{ "hook_-c3-.ic1",		0x040000, 0x7ef67731, 3 | BRF_GRA },           //  9

	{ "hook_-000-.ic33",	0x100000, 0xccceac30, 4 | BRF_GRA },           // 10 Sprites
	{ "hook_-010-.ic34",	0x100000, 0x8ac8da67, 4 | BRF_GRA },           // 11
	{ "hook_-020-.ic35",	0x100000, 0x8847af9a, 4 | BRF_GRA },           // 12
	{ "hook_-030-.ic36",	0x100000, 0x239e877e, 4 | BRF_GRA },           // 13

	{ "hook_-da-.ic11",		0x080000, 0x88cd0212, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },           // 18
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(hooku)
STD_ROM_FN(hooku)

struct BurnDriver BurnDrvHooku = {
	"hooku", "hook", NULL, NULL, "1992",
	"Hook (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookuRomInfo, hookuRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, HookDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Japan)

static struct BurnRomInfo hookjRomDesc[] = {
	{ "hook_-h0-g.ic25",	0x040000, 0x5964c886, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "hook_-l0-g.ic38",	0x040000, 0x7f7433f2, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "hook_-h1-.ic24",		0x020000, 0x264ba1f0, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "hook_-l1-.ic37",		0x020000, 0xf9913731, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "hook_-sh0-a.ic27",	0x010000, 0xbd3d1f61, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "hook_-sl0-a.ic28",	0x010000, 0x76371def, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hook_-c0-.ic4",		0x040000, 0xdec63dcf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hook_-c1-.ic3",		0x040000, 0xe4eb0b92, 3 | BRF_GRA },           //  7
	{ "hook_-c2-.ic2",		0x040000, 0xa52b320b, 3 | BRF_GRA },           //  8
	{ "hook_-c3-.ic1",		0x040000, 0x7ef67731, 3 | BRF_GRA },           //  9

	{ "hook_-000-.ic33",	0x100000, 0xccceac30, 4 | BRF_GRA },           // 10 Sprites
	{ "hook_-010-.ic34",	0x100000, 0x8ac8da67, 4 | BRF_GRA },           // 11
	{ "hook_-020-.ic35",	0x100000, 0x8847af9a, 4 | BRF_GRA },           // 12
	{ "hook_-030-.ic36",	0x100000, 0x239e877e, 4 | BRF_GRA },           // 13

	{ "hook_-da-.ic11",		0x080000, 0x88cd0212, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },           // 18
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(hookj)
STD_ROM_FN(hookj)

struct BurnDriver BurnDrvHookj = {
	"hookj", "hook", NULL, NULL, "1992",
	"Hook (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookjRomInfo, hookjRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, HookDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Peter Pan (bootleg of Hook)

static struct BurnRomInfo ppanRomDesc[] = {
	{ "1.u6",		0x80000, 0xb135dd6e, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "2.u5",		0x80000, 0x7785289c, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "7.u114",		0x40000, 0xdec63dcf, 2 | BRF_PRG | BRF_ESS }, //  2 Background Tiles
	{ "6.u115",		0x40000, 0xe4eb0b92, 2 | BRF_PRG | BRF_ESS }, //  3
	{ "5.u116",		0x40000, 0xa52b320b, 2 | BRF_PRG | BRF_ESS }, //  4
	{ "4.u117",		0x40000, 0x7ef67731, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "15.u106",	0x80000, 0xcdfc2f78, 3 | BRF_GRA },           //  6 Sprites
	{ "14.u110",	0x80000, 0x87e767f0, 3 | BRF_GRA },           //  7
	{ "13.u107",	0x80000, 0xe07f2abe, 3 | BRF_GRA },           //  8
	{ "12.u111",	0x80000, 0xf446150e, 3 | BRF_GRA },           //  9
	{ "11.u108",	0x80000, 0x5c114daa, 3 | BRF_GRA },           // 10
	{ "10.u112",	0x80000, 0xfa11fa40, 3 | BRF_GRA },           // 11
	{ "9.u109",		0x80000, 0x9d466b1a, 3 | BRF_GRA },           // 12
	{ "8.u113",		0x80000, 0xd08a5f6b, 3 | BRF_GRA },           // 13

	{ "3.u122",		0x80000, 0xd0d37028, 4 | BRF_GRA },           // 14 OKI M6295 Samples
};

STD_ROM_PICK(ppan)
STD_ROM_FN(ppan)

static INT32 ppanRomLoad()
{
	if (BurnLoadRom(DrvV33ROM + 0x000001, 0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM + 0x000000, 1, 2)) return 1;

	UINT8 *tmp = (UINT8 *)BurnMalloc(0x080000);
	if (tmp == NULL) {
		return 1;
	}

	loadDecodeGfx01(tmp,  2, 0, 0x40000);
	loadDecodeGfx01(tmp,  3, 1, 0x40000);
	loadDecodeGfx01(tmp,  4, 2, 0x40000);
	loadDecodeGfx01(tmp,  5, 3, 0x40000);

	DrvGfxROM1 += 0x400000;

	loadDecodeGfx02(tmp,  7, 0, 0x80000);
	loadDecodeGfx02(tmp,  9, 1, 0x80000);
	loadDecodeGfx02(tmp, 11, 2, 0x80000);
	loadDecodeGfx02(tmp, 13, 3, 0x80000);

	DrvGfxROM1 -= 0x400000;

	loadDecodeGfx02(tmp,  6, 0, 0x80000);
	loadDecodeGfx02(tmp,  8, 1, 0x80000);
	loadDecodeGfx02(tmp, 10, 2, 0x80000);
	loadDecodeGfx02(tmp, 12, 3, 0x80000);

	if (BurnLoadRom(DrvSndROM + 0x100000, 14, 1)) return 1;
	memcpy(DrvSndROM, DrvSndROM + 0x100000, 0x40000);

	BurnFree(tmp);

	DrvSprBuf = DrvSprRAM; // no sprite buffer!!

	return 0;
}

static INT32 PpanInit()
{
	m92_kludge = 3;
	nScreenOffsets[0] = 0;
	nScreenOffsets[1] = 120;
	return DrvInit(ppanRomLoad, NULL, 1, 0x100000, 0x400000);
}

struct BurnDriver BurnDrvPpan = {
	"ppan", "hook", NULL, NULL, "1992",
	"Peter Pan (bootleg of Hook)\0", NULL, "bootleg", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_BOOTLEG | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, ppanRomInfo, ppanRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, HookDIPInfo,
	PpanInit, DrvExit, PpanFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// In The Hunt (World)
// M92-E-B  05C04238B1 ROM board

static struct BurnRomInfo inthuntRomDesc[] = {
	{ "ith_-h0-d.ic28",		0x040000, 0x52f8e7a6, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ith_-l0-d.ic39",		0x040000, 0x5db79eb7, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ith_-h1-b.ic38",		0x020000, 0xfc2899df, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ith_-l1-b.ic27",		0x020000, 0x955a605a, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ith_-sh0-.ic30",		0x010000, 0x209c8b7f, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ith_-sl0-.ic31",		0x010000, 0x18472d65, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ith_-c0-.ic26",		0x080000, 0x4c1818cf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ith_-c1-.ic25",		0x080000, 0x91145bae, 3 | BRF_GRA },           //  7
	{ "ith_-c2-.ic24",		0x080000, 0xfc03fe3b, 3 | BRF_GRA },           //  8
	{ "ith_-c3-.ic23",		0x080000, 0xee156a0a, 3 | BRF_GRA },           //  9

	{ "ith_-000-.ic34",		0x100000, 0xa019766e, 4 | BRF_GRA },           // 10 Sprites
	{ "ith_-010-.ic35",		0x100000, 0x3fca3073, 4 | BRF_GRA },           // 11
	{ "ith_-020-.ic36",		0x100000, 0x20d1b28b, 4 | BRF_GRA },           // 12
	{ "ith_-030-.ic37",		0x100000, 0x90b6fd4b, 4 | BRF_GRA },           // 13

	{ "ith_-da-.ic9",		0x080000, 0x318ee71a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3k-.ic20",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 19
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(inthunt)
STD_ROM_FN(inthunt)

static INT32 inthuntRomLoad()
{
	return RomLoad(0x080000, 0x080000, 0x100000, 0, 0);
}

static INT32 inthuntInit()
{
	return DrvInit(inthuntRomLoad, inthunt_decryption_table, 1, 0x200000, 0x400000);
}

struct BurnDriver BurnDrvInthunt = {
	"inthunt", NULL, NULL, NULL, "1993",
	"In The Hunt (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, inthuntRomInfo, inthuntRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, InthuntDIPInfo,
	inthuntInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// In The Hunt (US)
// M92-E-B  05C04238B1 ROM board

static struct BurnRomInfo inthuntuRomDesc[] = {
	{ "ith_-h0-c.ic28",		0x040000, 0x563dcec0, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ith_-l0-c.ic39",		0x040000, 0x1638c705, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ith_-h1-a.ic38",		0x020000, 0x0253065f, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ith_-l1-a.ic27",		0x020000, 0xa57d688d, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ith_-sh0-.ic30",		0x010000, 0x209c8b7f, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ith_-sl0-.ic31",		0x010000, 0x18472d65, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ith_-c0-.ic26",		0x080000, 0x4c1818cf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ith_-c1-.ic25",		0x080000, 0x91145bae, 3 | BRF_GRA },           //  7
	{ "ith_-c2-.ic24",		0x080000, 0xfc03fe3b, 3 | BRF_GRA },           //  8
	{ "ith_-c3-.ic23",		0x080000, 0xee156a0a, 3 | BRF_GRA },           //  9

	{ "ith_-000-.ic34",		0x100000, 0xa019766e, 4 | BRF_GRA },           // 10 Sprites
	{ "ith_-010-.ic35",		0x100000, 0x3fca3073, 4 | BRF_GRA },           // 11
	{ "ith_-020-.ic36",		0x100000, 0x20d1b28b, 4 | BRF_GRA },           // 12
	{ "ith_-030-.ic37",		0x100000, 0x90b6fd4b, 4 | BRF_GRA },           // 13

	{ "ith_-da-.ic9",		0x080000, 0x318ee71a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3k-.ic20",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 19
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(inthuntu)
STD_ROM_FN(inthuntu)

struct BurnDriver BurnDrvInthuntu = {
	"inthuntu", "inthunt", NULL, NULL, "1993",
	"In The Hunt (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, inthuntuRomInfo, inthuntuRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, InthuntDIPInfo,
	inthuntInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Kaitei Daisensou (Japan)

static struct BurnRomInfo kaiteidsRomDesc[] = {
	{ "ith_-h0-.ic28",		0x040000, 0xdc1dec36, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ith_-l0-.ic39",		0x040000, 0x8835d704, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ith_-h1-.ic38",		0x020000, 0x5a7b212d, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ith_-l1-.ic27",		0x020000, 0x4c084494, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ith_-sh0.ic30",		0x010000, 0x209c8b7f, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ith_-sl0.ic31",		0x010000, 0x18472d65, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ith_-c0-.ic26",		0x080000, 0x4c1818cf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ith_-c1-.ic25",		0x080000, 0x91145bae, 3 | BRF_GRA },           //  7
	{ "ith_-c2-.ic24",		0x080000, 0xfc03fe3b, 3 | BRF_GRA },           //  8
	{ "ith_-c3-.ic23",		0x080000, 0xee156a0a, 3 | BRF_GRA },           //  9

	{ "ith_-000-.ic34",		0x100000, 0xa019766e, 4 | BRF_GRA },           // 10 Sprites
	{ "ith_-010-.ic35",		0x100000, 0x3fca3073, 4 | BRF_GRA },           // 11
	{ "ith_-020-.ic36",		0x100000, 0x20d1b28b, 4 | BRF_GRA },           // 12
	{ "ith_-030-.ic37",		0x100000, 0x90b6fd4b, 4 | BRF_GRA },           // 13

	{ "ith_-da-.ic9",		0x080000, 0x318ee71a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3k-.ic20",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 19
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(kaiteids)
STD_ROM_FN(kaiteids)

struct BurnDriver BurnDrvKaiteids = {
	"kaiteids", "inthunt", NULL, NULL, "1993",
	"Kaitei Daisensou (Japan)\0", NULL, "Irem", "Irem M92",
	L"Kaitei Daisensou (Japan)\0\u6d77\u5e95\u5927\u6226\u4e89\0", NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, kaiteidsRomInfo, kaiteidsRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, InthuntDIPInfo,
	inthuntInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// In The Hunt (Korea)
// M92-E-B  05C04238B1 ROM board

static struct BurnRomInfo inthuntkRomDesc[] = {
	{ "ic28",				0x040000, 0x90f82ea3, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ic39",				0x040000, 0x4bf419c0, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ic38",				0x020000, 0xfc2899df, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ic27",				0x020000, 0x955a605a, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ic30",				0x010000, 0x209c8b7f, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ic31",				0x010000, 0x18472d65, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ith_-c0-.ic26",		0x080000, 0x4c1818cf, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ith_-c1-.ic25",		0x080000, 0x91145bae, 3 | BRF_GRA },           //  7
	{ "ith_-c2-.ic24",		0x080000, 0xfc03fe3b, 3 | BRF_GRA },           //  8
	{ "ith_-c3-.ic23",		0x080000, 0xee156a0a, 3 | BRF_GRA },           //  9

	{ "ith_-000-.ic34",		0x100000, 0xa019766e, 4 | BRF_GRA },           // 10 Sprites
	{ "ith_-010-.ic35",		0x100000, 0x3fca3073, 4 | BRF_GRA },           // 11
	{ "ith_-020-.ic36",		0x100000, 0x20d1b28b, 4 | BRF_GRA },           // 12
	{ "ith_-030-.ic37",		0x100000, 0x90b6fd4b, 4 | BRF_GRA },           // 13

	{ "ith_-da-.ic9",		0x080000, 0x318ee71a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3k-.ic20",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 19
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(inthuntk)
STD_ROM_FN(inthuntk)

struct BurnDriver BurnDrvInthuntk = {
	"inthuntk", "inthunt", NULL, NULL, "1993",
	"In The Hunt (Korea?)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, inthuntkRomInfo, inthuntkRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, InthuntDIPInfo,
	inthuntInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// R-Type Leo (World)

static struct BurnRomInfo rtypeleoRomDesc[] = {
	{ "rtl_-h0-c.ic28",		0x040000, 0x5fef7fa1, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "rtl_-l0-c.ic25",		0x040000, 0x8156456b, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "rtl_-h1-.ic27",		0x020000, 0x352ff444, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "rtl_-l1-.ic26",		0x020000, 0xfd34ea46, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "rtl_-sh0-a.ic14",	0x010000, 0xe518b4e3, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "rtl_-sl0-a.ic17",	0x010000, 0x896f0d36, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "rtl_-c0-.ic9",		0x080000, 0xfb588d7c, 3 | BRF_GRA },           //  6 Background Tiles
	{ "rtl_-c1-.ic10",		0x080000, 0xe5541bff, 3 | BRF_GRA },           //  7
	{ "rtl_-c2-.ic11",		0x080000, 0xfaa9ae27, 3 | BRF_GRA },           //  8
	{ "rtl_-c3-.ic12",		0x080000, 0x3a2343f6, 3 | BRF_GRA },           //  9

	{ "rtl_-000-.ic38",		0x100000, 0x82a06870, 4 | BRF_GRA },           // 10 Sprites
	{ "rtl_-010-.ic39",		0x100000, 0x417e7a56, 4 | BRF_GRA },           // 11
	{ "rtl_-020-.ic40",		0x100000, 0xf9a3f3a1, 4 | BRF_GRA },           // 12
	{ "rtl_-030-.ic41",		0x100000, 0x03528d95, 4 | BRF_GRA },           // 13

	{ "rtl_-da-.ic8",		0x080000, 0xdbebd1ff, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_c-2l-.ic7",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_c-7h-c.ic43",	0x000117, 0xebea28fe, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(rtypeleo)
STD_ROM_FN(rtypeleo)

static INT32 rtypeleoRomLoad()
{
	return RomLoad(0x080000, 0x080000, 0x100000, 0, 0);
}

static INT32 rtypeleoInit()
{
	m92_kludge = 4+1; // fix for sporatic tilemap corruption in the first attract-mode stage (stage 2)
	// same as nbbatman, but without scroll offset.
	m92_ok_to_blank = 1;

	no_palbank = 1;

	return DrvInit(rtypeleoRomLoad, rtypeleo_decryption_table, 1, 0x200000, 0x400000);
}

struct BurnDriver BurnDrvRtypeleo = {
	"rtypeleo", NULL, NULL, NULL, "1992",
	"R-Type Leo (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, rtypeleoRomInfo, rtypeleoRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, RtypeleoDIPInfo,
	rtypeleoInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// R-Type Leo (Japan)

static struct BurnRomInfo rtypeleojRomDesc[] = {
	{ "rtl_-h0-d.ic28",		0x040000, 0x3dbac89f, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "rtl_-l0-d.ic25",		0x040000, 0xf85a2537, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "rtl_-h1-.ic27",		0x020000, 0x352ff444, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "rtl_-l1-.ic26",		0x020000, 0xfd34ea46, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "rtl_-sh0-a.ic14",	0x010000, 0xe518b4e3, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "rtl_-sl0-a.ic17",	0x010000, 0x896f0d36, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "rtl_-c0-.ic9",		0x080000, 0xfb588d7c, 3 | BRF_GRA },           //  6 Background Tiles
	{ "rtl_-c1-.ic10",		0x080000, 0xe5541bff, 3 | BRF_GRA },           //  7
	{ "rtl_-c2-.ic11",		0x080000, 0xfaa9ae27, 3 | BRF_GRA },           //  8
	{ "rtl_-c3-.ic12",		0x080000, 0x3a2343f6, 3 | BRF_GRA },           //  9

	{ "rtl_-000-.ic38",		0x100000, 0x82a06870, 4 | BRF_GRA },           // 10 Sprites
	{ "rtl_-010-.ic39",		0x100000, 0x417e7a56, 4 | BRF_GRA },           // 11
	{ "rtl_-020-.ic40",		0x100000, 0xf9a3f3a1, 4 | BRF_GRA },           // 12
	{ "rtl_-030-.ic41",		0x100000, 0x03528d95, 4 | BRF_GRA },           // 13

	{ "rtl_-da-.ic8",		0x080000, 0xdbebd1ff, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_c-2l-.ic7",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_c-7h-c.ic43",	0x000117, 0xebea28fe, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(rtypeleoj)
STD_ROM_FN(rtypeleoj)

struct BurnDriver BurnDrvRtypeleoj = {
	"rtypeleoj", "rtypeleo", NULL, NULL, "1992",
	"R-Type Leo (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, rtypeleojRomInfo, rtypeleojRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, RtypeleoDIPInfo,
	rtypeleoInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Blade Master (World)

static struct BurnRomInfo bmasterRomDesc[] = {
	{ "bm_d-h0-b.ic28",		0x040000, 0x49b257c7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "bm_d-l0-b.ic25",		0x040000, 0xa873523e, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "bm_d-h1-b.ic27",		0x010000, 0x082b7158, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "bm_d-l1-b.ic26",		0x010000, 0x6ff0c04e, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "bm_d-sh0.ic14",		0x010000, 0x9f7c075b, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "bm_d-sl0.ic17",		0x010000, 0x1fa87c89, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "bm_c0.ic9",			0x040000, 0x2cc966b8, 3 | BRF_GRA },           //  6 Background Tiles
	{ "bm_c1.ic10",			0x040000, 0x46df773e, 3 | BRF_GRA },           //  7
	{ "bm_c2.ic11",			0x040000, 0x05b867bd, 3 | BRF_GRA },           //  8
	{ "bm_c3.ic12",			0x040000, 0x0a2227a4, 3 | BRF_GRA },           //  9

	{ "bm_000.ic38",		0x080000, 0x339fc9f3, 4 | BRF_GRA },           // 10 Sprites
	{ "bm_010.ic39",		0x080000, 0x6a14377d, 4 | BRF_GRA },           // 11
	{ "bm_020.ic40",		0x080000, 0x31532198, 4 | BRF_GRA },           // 12
	{ "bm_030.ic41",		0x080000, 0xd1a041d3, 4 | BRF_GRA },           // 13

	{ "bm_da.ic8",			0x080000, 0x62ce5798, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-a.ic43",	0x000117, 0x29481115, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(bmaster)
STD_ROM_FN(bmaster)

static INT32 bmasterRomLoad()
{
	return RomLoad(0x080000, 0x040000, 0x080000, 0, 0);
}

static INT32 bmasterInit()
{
	return DrvInit(bmasterRomLoad, bomberman_decryption_table, 1, 0x100000, 0x200000);
}

struct BurnDriver BurnDrvBmaster = {
	"bmaster", NULL, NULL, NULL, "1991",
	"Blade Master (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, bmasterRomInfo, bmasterRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, BmasterDIPInfo,
	bmasterInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Cross Blades! (Japan)

static struct BurnRomInfo crossbldRomDesc[] = {
	{ "bm_d-h0.ic25",		0x040000, 0xa28a5821, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "bm_d-l0.ic38",		0x040000, 0xa504f1a0, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "bm_d-h1.ic24",		0x010000, 0x18da6c47, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "bm_d-l1.ic37",		0x010000, 0xa65c1b42, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "bm_d-sh0.ic27",		0x010000, 0x9f7c075b, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "bm_d-sl0.ic28",		0x010000, 0x1fa87c89, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "bm_c0.ic9",			0x040000, 0x2cc966b8, 3 | BRF_GRA },           //  6 Background Tiles
	{ "bm_c1.ic10",			0x040000, 0x46df773e, 3 | BRF_GRA },           //  7
	{ "bm_c2.ic11",			0x040000, 0x05b867bd, 3 | BRF_GRA },           //  8
	{ "bm_c3.ic12",			0x040000, 0x0a2227a4, 3 | BRF_GRA },           //  9

	{ "bm_000.ic33",		0x080000, 0x339fc9f3, 4 | BRF_GRA },           // 10 Sprites
	{ "bm_010.ic34",		0x080000, 0x6a14377d, 4 | BRF_GRA },           // 11
	{ "bm_020.ic35",		0x080000, 0x31532198, 4 | BRF_GRA },           // 12
	{ "bm_030.ic36",		0x080000, 0xd1a041d3, 4 | BRF_GRA },           // 13

	{ "bm_da.ic11",			0x080000, 0x62ce5798, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },           // 18
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(crossbld)
STD_ROM_FN(crossbld)

struct BurnDriver BurnDrvCrossbld = {
	"crossbld", "bmaster", NULL, NULL, "1991",
	"Cross Blades! (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, crossbldRomInfo, crossbldRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, BmasterDIPInfo,
	bmasterInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Mystic Riders (World)

static struct BurnRomInfo mysticriRomDesc[] = {
	{ "mr_-h0-b.ic28",		0x040000, 0xd529f887, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mr_-l0-b.ic25",		0x040000, 0xa457ab44, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mr_-h1-b.ic27",		0x010000, 0xe17649b9, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mr_-l1-b.ic26",		0x010000, 0xa87c62b4, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mr_-sh0-.ic14",		0x010000, 0x50d335e4, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mr_-sl0-.ic17",		0x010000, 0x0fa32721, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "mr_-c0-.ic9",		0x040000, 0x872a8fad, 3 | BRF_GRA },           //  6 Background Tiles
	{ "mr_-c1-.ic10",		0x040000, 0xd2ffb27a, 3 | BRF_GRA },           //  7
	{ "mr_-c2-.ic11",		0x040000, 0x62bff287, 3 | BRF_GRA },           //  8
	{ "mr_-c3-.ic12",		0x040000, 0xd0da62ab, 3 | BRF_GRA },           //  9

	{ "mr_-000-.ic38",		0x080000, 0xa0f9ce16, 4 | BRF_GRA },           // 10 Sprites
	{ "mr_-010-.ic39",		0x080000, 0x4e70a9e9, 4 | BRF_GRA },           // 11
	{ "mr_-020-.ic40",		0x080000, 0xb9c468fc, 4 | BRF_GRA },           // 12
	{ "mr_-030-.ic41",		0x080000, 0xcc32433a, 4 | BRF_GRA },           // 13

	{ "mr_-da-.ic8",		0x040000, 0x1a11fc59, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-a.ic43",	0x000117, 0x29481115, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(mysticri)
STD_ROM_FN(mysticri)

static INT32 mysticriInit()
{
	return DrvInit(bmasterRomLoad, mysticri_decryption_table, 1, 0x100000, 0x200000);
}

struct BurnDriver BurnDrvMysticri = {
	"mysticri", NULL, NULL, NULL, "1992",
	"Mystic Riders (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, mysticriRomInfo, mysticriRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, MysticriDIPInfo,
	mysticriInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Mahou Keibitai Gun Hohki (Japan)

static struct BurnRomInfo gunhohkiRomDesc[] = {
	{ "mr_-h0-.ic28",		0x040000, 0x83352270, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mr_-l0-.ic25",		0x040000, 0x9db308ae, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mr_-h1-.ic27",		0x010000, 0xc9532b60, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mr_-l1-.ic26",		0x010000, 0x6349b520, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mr_-sh0-.ic14",		0x010000, 0x50d335e4, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mr_-sl0-.ic17",		0x010000, 0x0fa32721, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "mr_-c0-.ic9",		0x040000, 0x872a8fad, 3 | BRF_GRA },           //  6 Background Tiles
	{ "mr_-c1-.ic10",		0x040000, 0xd2ffb27a, 3 | BRF_GRA },           //  7
	{ "mr_-c2-.ic11",		0x040000, 0x62bff287, 3 | BRF_GRA },           //  8
	{ "mr_-c3-.ic12",		0x040000, 0xd0da62ab, 3 | BRF_GRA },           //  9

	{ "mr_-000-.ic38",		0x080000, 0xa0f9ce16, 4 | BRF_GRA },           // 10 Sprites
	{ "mr_-010-.ic39",		0x080000, 0x4e70a9e9, 4 | BRF_GRA },           // 11
	{ "mr_-020-.ic40",		0x080000, 0xb9c468fc, 4 | BRF_GRA },           // 12
	{ "mr_-030-.ic41",		0x080000, 0xcc32433a, 4 | BRF_GRA },           // 13

	{ "mr_-da-.ic8",		0x040000, 0x1a11fc59, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-a.ic43",	0x000117, 0x29481115, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(gunhohki)
STD_ROM_FN(gunhohki)

struct BurnDriver BurnDrvGunhohki = {
	"gunhohki", "mysticri", NULL, NULL, "1992",
	"Mahou Keibitai Gun Hohki (Japan)\0", NULL, "Irem", "Irem M92",
	L"Mahou Keibitai Gun Hohki (Japan)\0\u9b54\u6cd5\u8b66\u5099\u968a \u30ac\u30f3\u30db\u30fc\u30ad\0", NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, gunhohkiRomInfo, gunhohkiRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, MysticriDIPInfo,
	mysticriInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Mystic Riders (bootleg?)

static struct BurnRomInfo mysticribRomDesc[] = {
	{ "h0",				0x040000, 0xe38c1f56, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "l0",				0x040000, 0x77846e48, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "h1",				0x010000, 0x4dcb085b, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "l1",				0x010000, 0x88df4f70, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "sh0",			0x010000, 0xfc7221ee, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "sl0",			0x010000, 0x65c809e6, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "mr-c0.bin",		0x040000, 0x872a8fad, 3 | BRF_GRA },           //  6 Background Tiles
	{ "mr-c1.bin",		0x040000, 0xd2ffb27a, 3 | BRF_GRA },           //  7
	{ "mr-c2.bin",		0x040000, 0x62bff287, 3 | BRF_GRA },           //  8
	{ "mr-c3.bin",		0x040000, 0xd0da62ab, 3 | BRF_GRA },           //  9

	{ "mr-000.bin",		0x080000, 0xa0f9ce16, 4 | BRF_GRA },           // 10 Sprites
	{ "mr-010.bin",		0x080000, 0x4e70a9e9, 4 | BRF_GRA },           // 11
	{ "mr-020.bin",		0x080000, 0xb9c468fc, 4 | BRF_GRA },           // 12
	{ "mr-030.bin",		0x080000, 0xcc32433a, 4 | BRF_GRA },           // 13

	{ "mr-da.bin",		0x040000, 0x1a11fc59, 5 | BRF_SND },           // 14 Irem GA20 Samples
};

STD_ROM_PICK(mysticrib)
STD_ROM_FN(mysticrib)

struct BurnDriver BurnDrvMysticrib = {
	"mysticrib", "mysticri", NULL, NULL, "1992",
	"Mystic Riders (bootleg?)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_BOOTLEG | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_HORSHOOT, 0,
	NULL, mysticribRomInfo, mysticribRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, MysticriDIPInfo,
	mysticriInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Gunforce - Battle Fire Engulfed Terror Island (World)

static struct BurnRomInfo gunforceRomDesc[] = {
	{ "gf_b-h0-c.ic28",		0x020000, 0xc09bb634, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "gf_b-l0-c.ic25",		0x020000, 0x1bef6f7d, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "gf_b-h1-c.ic27",		0x020000, 0xc84188b7, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "gf_b-l1-c.ic26",		0x020000, 0xb189f72a, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "gf_b-sh0-.ic14",		0x010000, 0x3f8f16e0, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "gf_b-sl0-.ic17",		0x010000, 0xdb0b13a3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "gf_c0.ic9",			0x040000, 0xb3b74979, 3 | BRF_GRA },           //  6 Background Tiles
	{ "gf_c1.ic10",			0x040000, 0xf5c8590a, 3 | BRF_GRA },           //  7
	{ "gf_c2.ic11",			0x040000, 0x30f9fb64, 3 | BRF_GRA },           //  8
	{ "gf_c3.ic12",			0x040000, 0x87b3e621, 3 | BRF_GRA },           //  9

	{ "gf_000.ic38",		0x040000, 0x209e8e8d, 4 | BRF_GRA },           // 10 Sprites
	{ "gf_010.ic39",		0x040000, 0x6e6e7808, 4 | BRF_GRA },           // 11
	{ "gf_020.ic40",		0x040000, 0x6f5c3cb0, 4 | BRF_GRA },           // 12
	{ "gf_030.ic41",		0x040000, 0x18978a9f, 4 | BRF_GRA },           // 13

	{ "gf-da.ic8",			0x020000, 0x933ba935, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-.ic43",		0x000117, 0x5de0795b, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(gunforce)
STD_ROM_FN(gunforce)

static INT32 gunforceRomLoad()
{
	return RomLoad(0x040000, 0x040000, 0x040000, 0, 0);
}

static INT32 gunforceInit()
{
	return DrvInit(gunforceRomLoad, gunforce_decryption_table, 1, 0x100000, 0x100000);
}

struct BurnDriver BurnDrvGunforce = {
	"gunforce", NULL, NULL, NULL, "1991",
	"Gunforce - Battle Fire Engulfed Terror Island (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_RUNGUN, 0,
	NULL, gunforceRomInfo, gunforceRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, GunforceDIPInfo,
	gunforceInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Gunforce - Battle Fire Engulfed Terror Island (Japan)

static struct BurnRomInfo gunforcejRomDesc[] = {
	{ "gf_b-h0-e.ic28",		0x020000, 0x43c36e0f, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "gf_b-l0-e.ic25",		0x020000, 0x24a558d8, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "gf_b-h1-e.ic27",		0x020000, 0xd9744f5d, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "gf_b-l1-e.ic26",		0x020000, 0xa0f7b61b, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "gf_b-sh0-.ic14",		0x010000, 0x3f8f16e0, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "gf_b-sl0-.ic17",		0x010000, 0xdb0b13a3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "gf_c0.ic9",			0x040000, 0xb3b74979, 3 | BRF_GRA },           //  6 Background Tiles
	{ "gf_c1.ic10",			0x040000, 0xf5c8590a, 3 | BRF_GRA },           //  7
	{ "gf_c2.ic11",			0x040000, 0x30f9fb64, 3 | BRF_GRA },           //  8
	{ "gf_c3.ic12",			0x040000, 0x87b3e621, 3 | BRF_GRA },           //  9

	{ "gf_000.ic38",		0x040000, 0x209e8e8d, 4 | BRF_GRA },           // 10 Sprites
	{ "gf_010.ic39",		0x040000, 0x6e6e7808, 4 | BRF_GRA },           // 11
	{ "gf_020.ic40",		0x040000, 0x6f5c3cb0, 4 | BRF_GRA },           // 12
	{ "gf_030.ic41",		0x040000, 0x18978a9f, 4 | BRF_GRA },           // 13

	{ "gf-da.ic8",			0x020000, 0x933ba935, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-.ic43",		0x000117, 0x5de0795b, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(gunforcej)
STD_ROM_FN(gunforcej)

struct BurnDriver BurnDrvGunforcej = {
	"gunforcej", "gunforce", NULL, NULL, "1991",
	"Gunforce - Battle Fire Engulfed Terror Island (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_RUNGUN, 0,
	NULL, gunforcejRomInfo, gunforcejRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, GunforceDIPInfo,
	gunforceInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Gunforce - Battle Fire Engulfed Terror Island (US)

static struct BurnRomInfo gunforceuRomDesc[] = {
	{ "gf_b-h0-d.ic28",		0x020000, 0xa6db7b5c, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "gf_b-l0-d.ic25",		0x020000, 0x82cf55f6, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "gf_b-h1-d.ic27",		0x020000, 0x08a3736c, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "gf_b-l1-d.ic26",		0x020000, 0x435f524f, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "gf_b-sh0-.ic14",		0x010000, 0x3f8f16e0, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "gf_b-sl0-.ic17",		0x010000, 0xdb0b13a3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "gf_c0.ic9",			0x040000, 0xb3b74979, 3 | BRF_GRA },           //  6 Background Tiles
	{ "gf_c1.ic10",			0x040000, 0xf5c8590a, 3 | BRF_GRA },           //  7
	{ "gf_c2.ic11",			0x040000, 0x30f9fb64, 3 | BRF_GRA },           //  8
	{ "gf_c3.ic12",			0x040000, 0x87b3e621, 3 | BRF_GRA },           //  9

	{ "gf_000.ic38",		0x040000, 0x209e8e8d, 4 | BRF_GRA },           // 10 Sprites
	{ "gf_010.ic39",		0x040000, 0x6e6e7808, 4 | BRF_GRA },           // 11
	{ "gf_020.ic40",		0x040000, 0x6f5c3cb0, 4 | BRF_GRA },           // 12
	{ "gf_030.ic41",		0x040000, 0x18978a9f, 4 | BRF_GRA },           // 13

	{ "gf-da.ic8",			0x020000, 0x933ba935, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-.ic43",		0x000117, 0x5de0795b, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(gunforceu)
STD_ROM_FN(gunforceu)

struct BurnDriver BurnDrvGunforceu = {
	"gunforceu", "gunforce", NULL, NULL, "1991",
	"Gunforce - Battle Fire Engulfed Terror Island (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_RUNGUN, 0,
	NULL, gunforceuRomInfo, gunforceuRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, GunforceDIPInfo,
	gunforceInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Undercover Cops (World)

static struct BurnRomInfo uccopsRomDesc[] = {
	{ "ucc_e-h0.ic28",		0x040000, 0x240aa5f7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ucc_e-l0.ic39",		0x040000, 0xdf9a4826, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ucc_h1.ic27",		0x020000, 0x8d29bcd6, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ucc_l1.ic38",		0x020000, 0xa8a402d8, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ucc_e-sh0-.ic30",	0x010000, 0xdf90b198, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ucc_e-sl0-.ic31",	0x010000, 0x96c11aac, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "uc_w38m.ic26",		0x080000, 0x130a40e5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "uc_w39m.ic25",		0x080000, 0xe42ca144, 3 | BRF_GRA },           //  7
	{ "uc_w40m.ic24",		0x080000, 0xc2961648, 3 | BRF_GRA },           //  8
	{ "uc_w41m.ic23",		0x080000, 0xf5334b80, 3 | BRF_GRA },           //  9

	{ "uc_k16m.ic37",		0x100000, 0x4a225f09, 4 | BRF_GRA },           // 10 Sprites
	{ "uc_k17m.ic36",		0x100000, 0xe4ed9a54, 4 | BRF_GRA },           // 11
	{ "uc_k18m.ic35",		0x100000, 0xa626eb12, 4 | BRF_GRA },           // 12
	{ "uc_k19m.ic34",		0x100000, 0x5df46549, 4 | BRF_GRA },           // 13

	{ "uc_w42.ic9",			0x080000, 0xd17d3fd6, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(uccops)
STD_ROM_FN(uccops)

static INT32 uccopsRomLoad()
{
	return RomLoad(0x080000, 0x080000, 0x100000, 0, 0);
}

static INT32 uccopsInit()
{
	return DrvInit(uccopsRomLoad, dynablaster_decryption_table, 1, 0x200000, 0x400000);
}

struct BurnDriver BurnDrvUccops = {
	"uccops", NULL, NULL, NULL, "1992",
	"Undercover Cops (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 3, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, uccopsRomInfo, uccopsRomName, NULL, NULL, NULL, NULL, p3CommonInputInfo, UccopsDIPInfo,
	uccopsInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Undercover Cops (US)

static struct BurnRomInfo uccopsuRomDesc[] = {
	{ "ucc_e-h0.ic28",		0x040000, 0x240aa5f7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ucc_e-l0.ic39",		0x040000, 0xdf9a4826, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ucc_h1-g.ic27",		0x020000, 0x6b8ca2de, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ucc_l1-g.ic38",		0x020000, 0x2bdec7dd, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "ucc_e-sh0-.ic30",	0x010000, 0xdf90b198, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "ucc_e-sl0-.ic31",	0x010000, 0x96c11aac, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "uc_w38m.ic26",		0x080000, 0x130a40e5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "uc_w39m.ic25",		0x080000, 0xe42ca144, 3 | BRF_GRA },           //  7
	{ "uc_w40m.ic24",		0x080000, 0xc2961648, 3 | BRF_GRA },           //  8
	{ "uc_w41m.ic23",		0x080000, 0xf5334b80, 3 | BRF_GRA },           //  9

	{ "uc_k16m.ic37",		0x100000, 0x4a225f09, 4 | BRF_GRA },           // 10 Sprites
	{ "uc_k17m.ic36",		0x100000, 0xe4ed9a54, 4 | BRF_GRA },           // 11
	{ "uc_k18m.ic35",		0x100000, 0xa626eb12, 4 | BRF_GRA },           // 12
	{ "uc_k19m.ic34",		0x100000, 0x5df46549, 4 | BRF_GRA },           // 13

	{ "uc_w42.ic9",			0x080000, 0xd17d3fd6, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(uccopsu)
STD_ROM_FN(uccopsu)

struct BurnDriver BurnDrvUccopsu = {
	"uccopsu", "uccops", NULL, NULL, "1992",
	"Undercover Cops (US)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 3, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, uccopsuRomInfo, uccopsuRomName, NULL, NULL, NULL, NULL, p3CommonInputInfo, UccopsDIPInfo,
	uccopsInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Undercover Cops (Japan)

static struct BurnRomInfo uccopsjRomDesc[] = {
	{ "uc_h0_a.ic28",		0x040000, 0x9e17cada, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "uc_l0_a.ic39",		0x040000, 0x4a4e3208, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "uc_h1_a.ic27",		0x020000, 0x83f78dea, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "uc_l1_a.ic38",		0x020000, 0x19628280, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "uc_sh0-.ic30",		0x010000, 0xf0ca1b03, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "uc_sl0-.ic31",		0x010000, 0xd1661723, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "uc_c0.ic26",			0x080000, 0x6a419a36, 3 | BRF_GRA },           //  6 Background Tiles
	{ "uc_c1.ic25",			0x080000, 0xd703ecc7, 3 | BRF_GRA },           //  7
	{ "uc_c2.ic24",			0x080000, 0x96397ac6, 3 | BRF_GRA },           //  8
	{ "uc_c3.ic23",			0x080000, 0x5d07d10d, 3 | BRF_GRA },           //  9

	{ "uc_030.ic37",		0x100000, 0x97f7775e, 4 | BRF_GRA },           // 10 Sprites
	{ "uc_020.ic36",		0x100000, 0x5e0b1d65, 4 | BRF_GRA },           // 11
	{ "uc_010.ic35",		0x100000, 0xbdc224b3, 4 | BRF_GRA },           // 12
	{ "uc_000.ic34",		0x100000, 0x7526daec, 4 | BRF_GRA },           // 13

	{ "uc_da.bin",			0x080000, 0x0b2855e9, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(uccopsj)
STD_ROM_FN(uccopsj)

struct BurnDriver BurnDrvUccopsj = {
	"uccopsj", "uccops", NULL, NULL, "1992",
	"Undercover Cops (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 3, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, uccopsjRomInfo, uccopsjRomName, NULL, NULL, NULL, NULL, p3CommonInputInfo, UccopsDIPInfo,
	uccopsInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Undercover Cops - Alpha Renewal Version (World)

static struct BurnRomInfo uccopsarRomDesc[] = {
	{ "uca_-h0-.ic28",		0x040000, 0x9e17cada, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "uca_-l0-.ic39",		0x040000, 0x4a4e3208, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "uca_-h1-b.ic27",		0x020000, 0x79d79742, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "uca_-l1-b.ic38",		0x020000, 0x37211581, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "uc_sh0-.ic30",		0x010000, 0xf0ca1b03, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "uc_sl0-.ic31",		0x010000, 0xd1661723, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "uc_c0.ic26",			0x080000, 0x6a419a36, 3 | BRF_GRA },           //  6 Background Tiles
	{ "uc_c1.ic25",			0x080000, 0xd703ecc7, 3 | BRF_GRA },           //  7
	{ "uc_c2.ic24",			0x080000, 0x96397ac6, 3 | BRF_GRA },           //  8
	{ "uc_c3.ic23",			0x080000, 0x5d07d10d, 3 | BRF_GRA },           //  9

	{ "uc_030.ic37",		0x100000, 0x97f7775e, 4 | BRF_GRA },           // 10 Sprites
	{ "uc_020.ic36",		0x100000, 0x5e0b1d65, 4 | BRF_GRA },           // 11
	{ "uc_010.ic35",		0x100000, 0xbdc224b3, 4 | BRF_GRA },           // 12
	{ "uc_000.ic34",		0x100000, 0x7526daec, 4 | BRF_GRA },           // 13

	{ "uc_da.bin",			0x080000, 0x0b2855e9, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(uccopsar)
STD_ROM_FN(uccopsar)

struct BurnDriver BurnDrvUccopsar = {
	"uccopsar", "uccops", NULL, NULL, "1992",
	"Undercover Cops - Alpha Renewal Version (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 3, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, uccopsarRomInfo, uccopsarRomName, NULL, NULL, NULL, NULL, p3CommonInputInfo, UccopsDIPInfo,
	uccopsInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Undercover Cops - Alpha Renewal Version (US)

static struct BurnRomInfo uccopsaruRomDesc[] = {
	{ "uc_aru_h0.ic28",		0x040000, 0xe9522dc7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "uc_aru_l0.ic39",		0x040000, 0x619b6aee, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "uc_aru_h1.ic27",		0x020000, 0x2130d53b, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "uc_aru_l1.ic38",		0x020000, 0xd768b177, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "uc_sh0-.ic30",		0x010000, 0xf0ca1b03, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "uc_sl0-.ic31",		0x010000, 0xd1661723, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "uc_c0.ic26",			0x080000, 0x6a419a36, 3 | BRF_GRA },           //  6 Background Tiles
	{ "uc_c1.ic25",			0x080000, 0xd703ecc7, 3 | BRF_GRA },           //  7
	{ "uc_c2.ic24",			0x080000, 0x96397ac6, 3 | BRF_GRA },           //  8
	{ "uc_c3.ic23",			0x080000, 0x5d07d10d, 3 | BRF_GRA },           //  9

	{ "uc_030.ic37",		0x100000, 0x97f7775e, 4 | BRF_GRA },           // 10 Sprites
	{ "uc_020.ic36",		0x100000, 0x5e0b1d65, 4 | BRF_GRA },           // 11
	{ "uc_010.ic35",		0x100000, 0xbdc224b3, 4 | BRF_GRA },           // 12
	{ "uc_000.ic34",		0x100000, 0x7526daec, 4 | BRF_GRA },           // 13

	{ "uc_da.bin",			0x080000, 0x0b2855e9, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_e-3p-.ic21",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 18
	{ "m92_e-4k-.ic29",		0x000117, 0x506a0e20, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(uccopsaru)
STD_ROM_FN(uccopsaru)

struct BurnDriver BurnDrvUccopsaru = {
	"uccopsaru", "uccops", NULL, NULL, "1992",
	"Undercover Cops - Alpha Renewal Version (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 3, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, uccopsaruRomInfo, uccopsaruRomName, NULL, NULL, NULL, NULL, p3CommonInputInfo, UccopsDIPInfo,
	uccopsInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Gun Force II (US)

static struct BurnRomInfo gunforc2RomDesc[] = {
	{ "a2_-h0-a.ic37",		0x040000, 0x49965e22, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a2_-l0-a.ic49",		0x040000, 0x8c88b278, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a2_-h1-.ic36",		0x040000, 0x34280b88, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a2_-l1-.ic48",		0x040000, 0xc8c13f51, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a2_-sh0-.ic24",		0x010000, 0x2e2d103d, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a2_-sl0-.ic31",		0x010000, 0x2287e0b3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "a2_-c0-.ic1",		0x080000, 0x68b8f574, 3 | BRF_GRA },           //  6 Background Tiles
	{ "a2_-c1-.ic2",		0x080000, 0x0b9efe67, 3 | BRF_GRA },           //  7
	{ "a2_-c2-.ic16",		0x080000, 0x7a9e9978, 3 | BRF_GRA },           //  8
	{ "a2_-c3-.ic17",		0x080000, 0x1395ee6d, 3 | BRF_GRA },           //  9

	{ "a2_-000-.ic44",		0x100000, 0x38e03147, 4 | BRF_GRA },           // 10 Sprites
	{ "a2_-010-.ic45",		0x100000, 0x1d5b05f8, 4 | BRF_GRA },           // 11
	{ "a2_-020-.ic46",		0x100000, 0xf2f461cc, 4 | BRF_GRA },           // 12
	{ "a2_-030-.ic47",		0x100000, 0x97609d9d, 4 | BRF_GRA },           // 13

	{ "a2_-da-.ic10",		0x100000, 0x3c8cdb6a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-3f-.ic14",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_b-4f-.ic21",		0x000117, 0x5e87fd01, 0 | BRF_OPT },           // 19
	{ "m92_b-7j-a.ic41",	0x000117, 0x09f57872, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(gunforc2)
STD_ROM_FN(gunforc2)

static INT32 gunforc2RomLoad()
{
	return RomLoad(0x100000, 0x080000, 0x100000, leaguemna, 0);
}

static INT32 gunforc2Init()
{
	INT32 nRet;

	m92_banks = 1;

	nRet = DrvInit(gunforc2RomLoad, lethalth_decryption_table, 1, 0x200000, 0x400000);

	if (nRet == 0) {
		memcpy(DrvV33ROM + 0x80000, DrvV33ROM + 0x100000, 0x20000);
	}

	return nRet;
}

struct BurnDriver BurnDrvGunforc2 = {
	"gunforc2", NULL, NULL, NULL, "1994",
	"Gun Force II (US)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_RUNGUN, 0,
	NULL, gunforc2RomInfo, gunforc2RomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, Gunforc2DIPInfo,
	gunforc2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Geo Storm (Japan, 014 custom sound CPU)

static struct BurnRomInfo geostormRomDesc[] = {
	{ "a2_-h0-.ic37",		0x040000, 0x9be58d09, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a2_-l0-.ic49",		0x040000, 0x59abb75d, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a2_-h1-.ic36",		0x040000, 0x34280b88, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a2_-l1-.ic48",		0x040000, 0xc8c13f51, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a2_-sh0-.ic24",		0x010000, 0x2e2d103d, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a2_-sl0-.ic31",		0x010000, 0x2287e0b3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "a2_-c0-.ic1",		0x080000, 0x68b8f574, 3 | BRF_GRA },           //  6 Background Tiles
	{ "a2_-c1-.ic2",		0x080000, 0x0b9efe67, 3 | BRF_GRA },           //  7
	{ "a2_-c2-.ic16",		0x080000, 0x7a9e9978, 3 | BRF_GRA },           //  8
	{ "a2_-c3-.ic17",		0x080000, 0x1395ee6d, 3 | BRF_GRA },           //  9

	{ "a2_-000-.ic44",		0x100000, 0x38e03147, 4 | BRF_GRA },           // 10 Sprites
	{ "a2_-010-.ic45",		0x100000, 0x1d5b05f8, 4 | BRF_GRA },           // 11
	{ "a2_-020-.ic46",		0x100000, 0xf2f461cc, 4 | BRF_GRA },           // 12
	{ "a2_-030-.ic47",		0x100000, 0x97609d9d, 4 | BRF_GRA },           // 13

	{ "a2_-da-.ic10",		0x100000, 0x3c8cdb6a, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_b-3f-.ic14",		0x000117, 0x52ecf083, 0 | BRF_OPT },           // 18
	{ "m92_b-4f-.ic21",		0x000117, 0x5e87fd01, 0 | BRF_OPT },           // 19
	{ "m92_b-7j-a.ic41",	0x000117, 0x09f57872, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(geostorm)
STD_ROM_FN(geostorm)

struct BurnDriver BurnDrvGeostorm = {
	"geostorm", "gunforc2", NULL, NULL, "1994",
	"Geo Storm (Japan, 014 custom sound CPU)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_RUNGUN, 0,
	NULL, geostormRomInfo, geostormRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, Gunforc2DIPInfo,
	gunforc2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Bat Man (World)

static struct BurnRomInfo nbbatmanRomDesc[] = {
	{ "a1_-h0-c.ic34",		0x040000, 0x5c4a1e3f, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a1_-l0-c.ic31",		0x040000, 0x3d6d70ae, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a1_-h1-.ic33",		0x040000, 0x3ce2aab5, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a1_-l1-.ic32",		0x040000, 0x116d9bcc, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a1_-sh0-.ic14",		0x010000, 0xb7fae3e6, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a1_-sl0-.ic17",		0x010000, 0xb26d54fc, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "lh534k0c.ic9",		0x080000, 0x314a0c6d, 3 | BRF_GRA },           //  6 Background Tiles
	{ "lh534k0e.ic10",		0x080000, 0xdc31675b, 3 | BRF_GRA },           //  7
	{ "lh534k0f.ic11",		0x080000, 0xe15d8bfb, 3 | BRF_GRA },           //  8
	{ "lh534k0g.ic12",		0x080000, 0x888d71a3, 3 | BRF_GRA },           //  9

	{ "lh538393.ic42",		0x100000, 0x26cdd224, 4 | BRF_GRA },           // 10 Sprites
	{ "lh538394.ic43",		0x100000, 0x4bbe94fa, 4 | BRF_GRA },           // 11
	{ "lh538395.ic44",		0x100000, 0x2a533b5e, 4 | BRF_GRA },           // 12
	{ "lh538396.ic45",		0x100000, 0x863a66fa, 4 | BRF_GRA },           // 13

	{ "lh534k0k.ic8",		0x080000, 0x735e6380, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(nbbatman)
STD_ROM_FN(nbbatman)

// ReiREM Vita port: pure wait loops found with the PC histogram (docs/PERF.md P2)
//   V33 0x005a4: test byte ss:[0000],0xff / jnz 0x5a4  (waits for the IRQ handler)
//   V35 0x008e2: test byte ss:[0c34],0x80 / jz  0x8e2  (waits for the IRQ handler)
extern "C" int rei_idle_skip_enabled;
void nec_set_idle_pc(int cpu, unsigned pc);
void v25_set_idle_pc(int cpu, unsigned pc);

static INT32 nbbatmanInit()
{
	INT32 nRet;
	const UINT8 *decrtab = leagueman_decryption_table;

	m92_banks = 1;
	m92_kludge = 4; // tilemap offset(for the "roz"-like map at beginning of stage) + cycle hacks
	m92_ok_to_blank = 1;

	if (DrvInput[8] & 1) // dip option, use old soundCPU emulation "style", for weirdos! :)
		decrtab = leagueman_OLD_decryption_table;

	nRet = DrvInit(gunforc2RomLoad,  decrtab, 1, 0x200000, 0x400000);

	if (nRet == 0) {
		memcpy(DrvV33ROM + 0x80000, DrvV33ROM + 0x100000, 0x20000);
	}

	// (VRAM write log callback is set in DrvInit for all games)

	if (nRet == 0 && rei_idle_skip_enabled) {
		nec_set_idle_pc(0, 0x005a4);
		v25_set_idle_pc(1, 0x008e2);
	}

	return nRet;
}

struct BurnDriver BurnDrvNbbatman = {
	"nbbatman", NULL, NULL, NULL, "1993",
	"Ninja Baseball Bat Man (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbatmanRomInfo, nbbatmanRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Bat Man (US)

static struct BurnRomInfo nbbatmanuRomDesc[] = {
	{ "a1_-h0-a.ic34",		0x040000, 0x24a9b794, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a1_-l0-a.ic31",		0x040000, 0x846d7716, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a1_-h1-.ic33",		0x040000, 0x3ce2aab5, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a1_-l1-.ic32",		0x040000, 0x116d9bcc, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a1_-sh0-.ic14",		0x010000, 0xb7fae3e6, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a1_-sl0-.ic17",		0x010000, 0xb26d54fc, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "lh534k0c.ic9",		0x080000, 0x314a0c6d, 3 | BRF_GRA },           //  6 Background Tiles
	{ "lh534k0e.ic10",		0x080000, 0xdc31675b, 3 | BRF_GRA },           //  7
	{ "lh534k0f.ic11",		0x080000, 0xe15d8bfb, 3 | BRF_GRA },           //  8
	{ "lh534k0g.ic12",		0x080000, 0x888d71a3, 3 | BRF_GRA },           //  9

	{ "lh538393.ic42",		0x100000, 0x26cdd224, 4 | BRF_GRA },           // 10 Sprites
	{ "lh538394.ic43",		0x100000, 0x4bbe94fa, 4 | BRF_GRA },           // 11
	{ "lh538395.ic44",		0x100000, 0x2a533b5e, 4 | BRF_GRA },           // 12
	{ "lh538396.ic45",		0x100000, 0x863a66fa, 4 | BRF_GRA },           // 13

	{ "lh534k0k.ic8",		0x080000, 0x735e6380, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(nbbatmanu)
STD_ROM_FN(nbbatmanu)

struct BurnDriver BurnDrvNbbatmanu = {
	"nbbatmanu", "nbbatman", NULL, NULL, "1993",
	"Ninja Baseball Bat Man (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbatmanuRomInfo, nbbatmanuRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Yakyuu Kakutou League-Man (Japan, set 1)

static struct BurnRomInfo leaguemnRomDesc[] = {
	{ "a1_-h0-.ic34",		0x040000, 0x47c54204, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a1_-l0-.ic31",		0x040000, 0x1d062c82, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a1_-h1-.ic33",		0x040000, 0x3ce2aab5, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a1_-l1-.ic32",		0x040000, 0x116d9bcc, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a1_-sh0-a.ic14",		0x010000, 0xc4aef83a, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a1_-sl0-a.ic17",		0x010000, 0xe9ecbed2, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "lh534k0c.ic9",		0x080000, 0x314a0c6d, 3 | BRF_GRA },           //  6 Background Tiles
	{ "lh534k0e.ic10",		0x080000, 0xdc31675b, 3 | BRF_GRA },           //  7
	{ "lh534k0f.ic11",		0x080000, 0xe15d8bfb, 3 | BRF_GRA },           //  8
	{ "lh534k0g.ic12",		0x080000, 0x888d71a3, 3 | BRF_GRA },           //  9

	{ "lh538393.ic42",		0x100000, 0x26cdd224, 4 | BRF_GRA },           // 10 Sprites
	{ "lh538394.ic43",		0x100000, 0x4bbe94fa, 4 | BRF_GRA },           // 11
	{ "lh538395.ic44",		0x100000, 0x2a533b5e, 4 | BRF_GRA },           // 12
	{ "lh538396.ic45",		0x100000, 0x863a66fa, 4 | BRF_GRA },           // 13

	{ "lh534k0k.ic8",		0x080000, 0x735e6380, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 18
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(leaguemn)
STD_ROM_FN(leaguemn)

struct BurnDriver BurnDrvLeaguemn = {
	"leaguemn", "nbbatman", NULL, NULL, "1993",
	"Yakyuu Kakutou League-Man (Japan, set 1)\0", NULL, "Irem", "Irem M92",
	L"Yakyuu Kakutou League-Man (Japan, set 1)\0\u91ce\u7403\u683c\u95d8 \u30ea\u30fc\u30b0\u30de\u30f3\0", NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, leaguemnRomInfo, leaguemnRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Yakyuu Kakutou League-Man (Japan, set 2)

static struct BurnRomInfo leaguemnaRomDesc[] = {
	{ "a1_-h0-.ic9",		0x040000, 0x47c54204, 1 | BRF_PRG | BRF_ESS },    //  0 V33 Code
	{ "a1_-l0-.ic11",		0x040000, 0x1d062c82, 1 | BRF_PRG | BRF_ESS },    //  1
	{ "a1_-h1-.ic8",		0x040000, 0x3ce2aab5, 1 | BRF_PRG | BRF_ESS },    //  2
	{ "a1_-l1-.ic10",		0x040000, 0x116d9bcc, 1 | BRF_PRG | BRF_ESS },    //  3

	{ "a1_-sh0-a.ic4",		0x010000, 0xc4aef83a, 2 | BRF_PRG | BRF_ESS },    //  4 V30 Code
	{ "a1_-sl0-a.ic2",		0x010000, 0xe9ecbed2, 2 | BRF_PRG | BRF_ESS },    //  5

	{ "a1_-c0-.ic59",		0x080000, 0x314a0c6d, 3 | BRF_GRA },              //  6 Background Tiles
	{ "a1_-c1-.ic58",		0x080000, 0xdc31675b, 3 | BRF_GRA },              //  7
	{ "a1_-c2-.ic47",		0x080000, 0xe15d8bfb, 3 | BRF_GRA },              //  8
	{ "a1_-c3-.ic48",		0x080000, 0x888d71a3, 3 | BRF_GRA },              //  9

	{ "a1_-000-w.ic54",		0x080000, 0x437d3c0d, 4 | BRF_GRA },              // 10 Sprites
	{ "a1_-001-w.ic43",		0x080000, 0xc2ecd508, 4 | BRF_GRA },              // 11
	{ "a1_-010-w.ic31",		0x080000, 0xd4184ccc, 4 | BRF_GRA },              // 12
	{ "a1_-011-w.ic19",		0x080000, 0x7752fc95, 4 | BRF_GRA },              // 13
	{ "a1_-020-w.ic56",		0x080000, 0x3008822e, 4 | BRF_GRA },              // 14
	{ "a1_-021-w.ic45",		0x080000, 0x04272660, 4 | BRF_GRA },              // 15
	{ "a1_-030-w.ic33",		0x080000, 0x0d5e744c, 4 | BRF_GRA },              // 16
	{ "a1_-031-w.ic21",		0x080000, 0x4aa9994e, 4 | BRF_GRA },              // 17

	{ "a1_-da-.ic53",		0x080000, 0x735e6380, 5 | BRF_SND },              // 18 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },              // 19 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },              // 20
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },              // 21
	{ "m92_z-2k-.ic15",		0x000117, 0x0646be21, 0 | BRF_OPT },              // 22
	{ "m92_z-2l-.ic16",		0x000117, 0xa09df0ee, 0 | BRF_OPT },              // 23
	{ "m92_z-2f-.ic64",		0x000117, 0x00000000, 0 | BRF_OPT | BRF_NODUMP }, // 24
};

STD_ROM_PICK(leaguemna)
STD_ROM_FN(leaguemna)

static INT32 leaguemnaInit()
{
	leaguemna = 1;

	return nbbatmanInit();
}

struct BurnDriver BurnDrvleaguemna = {
	"leaguemna", "nbbatman", NULL, NULL, "1993",
	"Yakyuu Kakutou League-Man (Japan, set 2)\0", NULL, "Irem", "Irem M92",
	L"Yakyuu Kakutou League-Man (Japan, set 2)\0\u91ce\u7403\u683c\u95d8 \u30ea\u30fc\u30b0\u30de\u30f3\0", NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, leaguemnaRomInfo, leaguemnaRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	leaguemnaInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Lethal Thunder (World)

static struct BurnRomInfo lethalthRomDesc[] = {
	{ "lt_d-h0-b.ic25",		0x020000, 0x20c68935, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "lt_d-l0-b.ic38",		0x020000, 0xe1432fb3, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "lt_d-h1-.ic24",		0x020000, 0xd7dd3d48, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "lt_d-l1-.ic37",		0x020000, 0xb94b3bd8, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "lt_d-sh0-.ic27",		0x010000, 0xaf5b224f, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "lt_d-sl0-.ic28",		0x010000, 0xcb3faac3, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "lt_c0.ic9",			0x040000, 0xada0fd50, 3 | BRF_GRA },           //  6 Background Tiles
	{ "lt_c1.ic10",			0x040000, 0xd2596883, 3 | BRF_GRA },           //  7
	{ "lt_c2.ic11",			0x040000, 0x2de637ef, 3 | BRF_GRA },           //  8
	{ "lt_c3.ic12",			0x040000, 0x9f6585cd, 3 | BRF_GRA },           //  9

	{ "lt_000.ic33",		0x040000, 0xbaf8863e, 4 | BRF_GRA },           // 10 Sprites
	{ "lt_010.ic34",		0x040000, 0x40fd50af, 4 | BRF_GRA },           // 11
	{ "lt_020.ic35",		0x040000, 0xc8e970df, 4 | BRF_GRA },           // 12
	{ "lt_030.ic36",		0x040000, 0xf5436708, 4 | BRF_GRA },           // 13

	{ "lt_da.ic11",			0x040000, 0x357762a2, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },           // 18
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },           // 19
};

STD_ROM_PICK(lethalth)
STD_ROM_FN(lethalth)

static INT32 lethalthRomLoad()
{
	m92_kludge = 1;
	return RomLoad(0x040000, 0x040000, 0x040000, 0, 0);
}

static INT32 lethalthInit()
{
	return DrvInit(lethalthRomLoad, lethalth_decryption_table, 0, 0x100000, 0x100000);
}

struct BurnDriver BurnDrvLethalth = {
	"lethalth", NULL, NULL, NULL, "1991",
	"Lethal Thunder (World)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_ORIENTATION_VERTICAL | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_VERSHOOT, 0,
	NULL, lethalthRomInfo, lethalthRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, LethalthDIPInfo,
	lethalthInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	240, 320, 3, 4
};


// Thunder Blaster (Japan)

static struct BurnRomInfo thndblstRomDesc[] = {
	{ "lt_c-h0-.ic28",		0x020000, 0xdc218a18, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "lt_c-l0-.ic25",		0x020000, 0xae9a3f81, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "lt_c-h1-.ic27",		0x020000, 0xd7dd3d48, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "lt_c-l1-.ic26",		0x020000, 0xb94b3bd8, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "lt_c-sh0-a.ic14",	0x008000, 0xcb76fd08, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "lt_c-sl0-a.ic17",	0x008000, 0xabea2071, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "lt_c0.ic9",			0x040000, 0xada0fd50, 3 | BRF_GRA },           //  6 Background Tiles
	{ "lt_c1.ic10",			0x040000, 0xd2596883, 3 | BRF_GRA },           //  7
	{ "lt_c2.ic11",			0x040000, 0x2de637ef, 3 | BRF_GRA },           //  8
	{ "lt_c3.ic12",			0x040000, 0x9f6585cd, 3 | BRF_GRA },           //  9

	{ "lt_000.ic38",		0x040000, 0xbaf8863e, 4 | BRF_GRA },           // 10 Sprites
	{ "lt_010.ic39",		0x040000, 0x40fd50af, 4 | BRF_GRA },           // 11
	{ "lt_020.ic40",		0x040000, 0xc8e970df, 4 | BRF_GRA },           // 12
	{ "lt_030.ic41",		0x040000, 0xf5436708, 4 | BRF_GRA },           // 13

	{ "lt_da.ic8",			0x040000, 0x357762a2, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 15 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 16
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 17
	{ "m92_c-2l-.ic7",		0x000117, 0x67a4cc04, 0 | BRF_OPT },           // 16
	{ "m92_c-7h-c.ic43",	0x000117, 0xebea28fe, 0 | BRF_OPT },           // 17
};

STD_ROM_PICK(thndblst)
STD_ROM_FN(thndblst)

struct BurnDriver BurnDrvThndblst = {
	"thndblst", "lethalth", NULL, NULL, "1991",
	"Thunder Blaster (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_ORIENTATION_VERTICAL | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_VERSHOOT, 0,
	NULL, thndblstRomInfo, thndblstRomName, NULL, NULL, NULL, NULL, p2CommonInputInfo, LethalthDIPInfo,
	lethalthInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	240, 320, 3, 4
};


// Dream Soccer '94 (Japan, M92 hardware)

static struct BurnRomInfo dsoccr94jRomDesc[] = {
	{ "a3_-h0-e.ic37",		0x040000, 0x8de1dbcd, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a3_-l0-e.ic49",		0x040000, 0xd3df8bfd, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a3_-h1-c.ic36",		0x040000, 0x6109041b, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a3_-l1-c.ic48",		0x040000, 0x97a01f6b, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a3_-sh0-.ic24",		0x010000, 0x23fe6ffc, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "a3_-sl0-.ic31",		0x010000, 0x768132e5, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "a3_-c0-.ic1",		0x100000, 0x83ea8a47, 3 | BRF_GRA },           //  6 Background Tiles
	{ "a3_-c1-.ic2",		0x100000, 0x64063e6d, 3 | BRF_GRA },           //  7
	{ "a3_-c2-.ic16",		0x100000, 0xcc1f621a, 3 | BRF_GRA },           //  8
	{ "a3_-c3-.ic17",		0x100000, 0x515829e1, 3 | BRF_GRA },           //  9

	{ "a3_-000-w.ic44",		0x080000, 0xb094e5ad, 4 | BRF_GRA },           // 10 Sprites
	{ "a3_-001-w.ic32",		0x080000, 0x91f34018, 4 | BRF_GRA },           // 11
	{ "a3_-010-w.ic45",		0x080000, 0xedddeef4, 4 | BRF_GRA },           // 12
	{ "a3_-011-w.ic33",		0x080000, 0x274a9526, 4 | BRF_GRA },           // 13
	{ "a3_-020-w.ic46",		0x080000, 0x32064393, 4 | BRF_GRA },           // 14
	{ "a3_-021-w.ic34",		0x080000, 0x57bae3d9, 4 | BRF_GRA },           // 15
	{ "a3_-030-w.ic47",		0x080000, 0xbe838e2f, 4 | BRF_GRA },           // 16
	{ "a3_-031-w.ic35",		0x080000, 0xbf899f0d, 4 | BRF_GRA },           // 17

	{ "ds_da0.ic10",		0x100000, 0x67fc52fd, 5 | BRF_SND },           // 18 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 19 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 20
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 21
};

STD_ROM_PICK(dsoccr94j)
STD_ROM_FN(dsoccr94j)

static INT32 dsoccr94jRomLoad()
{
	return RomLoad(0x100000, 0x100000, 0x100000, 1, 0);
}

static INT32 dsoccr94jInit()
{
	m92_banks = 1;

	return DrvInit(dsoccr94jRomLoad, dsoccr94_decryption_table, 1, 0x400000, 0x400000);
}

struct BurnDriver BurnDrvDsoccr94j = {
	"dsoccr94j", "dsoccr94", NULL, NULL, "1994",
	"Dream Soccer '94 (Japan, M92 hardware)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SPORTSFOOTBALL, 0,
	NULL, dsoccr94jRomInfo, dsoccr94jRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Dsoccr94jDIPInfo,
	dsoccr94jInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Superior Soldiers (US)

static struct BurnRomInfo ssoldierRomDesc[] = {
	{ "f3_-h0-h.ic37",		0x040000, 0xb63fb9da, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "f3_-l0-h.ic49",		0x040000, 0x419361a2, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "f3_-h1-a.ic36",		0x020000, 0xe3d9f619, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "f3_-l1-a.ic48",		0x020000, 0x8cb5c396, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "f3_-sh0-.ic24",		0x010000, 0x90b55e5e, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "f3_-sl0-.ic31",		0x010000, 0x77c16d57, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "f3_w50.c0.ic1",		0x040000, 0x47e788ee, 3 | BRF_GRA },           //  6 Background Tiles
	{ "f3_w51.c1.ic2",		0x040000, 0x8e535e3f, 3 | BRF_GRA },           //  7
	{ "f3_w52.c2.ic16",		0x040000, 0xa6eb2e56, 3 | BRF_GRA },           //  8
	{ "f3_w53.c3.ic17",		0x040000, 0x2f992807, 3 | BRF_GRA },           //  9

	{ "f3_w37.000.ic44",	0x100000, 0xfd4cda03, 4 | BRF_GRA },           // 10 Sprites
	{ "f3_w38.001.ic32",	0x100000, 0x755bab10, 4 | BRF_GRA },           // 11
	{ "f3_w39.010.ic45",	0x100000, 0xb21ced92, 4 | BRF_GRA },           // 12
	{ "f3_w40.011.ic33",	0x100000, 0x2e906889, 4 | BRF_GRA },           // 13
	{ "f3_w41.020.ic46",	0x100000, 0x02455d10, 4 | BRF_GRA },           // 14
	{ "f3_w42.021.ic34",	0x100000, 0x124589b9, 4 | BRF_GRA },           // 15
	{ "f3_w43.030.ic47",	0x100000, 0xdae7327a, 4 | BRF_GRA },           // 16
	{ "f3_w44.031.ic35",	0x100000, 0xd0fc84ac, 4 | BRF_GRA },           // 17

	{ "f3_w95.da.ic10",		0x080000, 0xf7ca432b, 5 | BRF_SND },           // 18 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 19 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 20
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 21
};

STD_ROM_PICK(ssoldier)
STD_ROM_FN(ssoldier)

static INT32 ssoldierRomLoad()
{
	return RomLoad(0x080000, 0x040000, 0x200000, 1, 0);
}

static INT32 ssoldierInit()
{
	m92_kludge = 1;

	return DrvInit(ssoldierRomLoad, psoldier_decryption_table, 1, 0x100000, 0x800000);
}

struct BurnDriver BurnDrvSsoldier = {
	"ssoldier", NULL, NULL, NULL, "1993",
	"Superior Soldiers (US)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_VSFIGHT, 0,
	NULL, ssoldierRomInfo, ssoldierRomName, NULL, NULL, NULL, NULL, PsoldierInputInfo, PsoldierDIPInfo,
	ssoldierInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Perfect Soldiers (Japan)

static struct BurnRomInfo psoldierRomDesc[] = {
	{ "f3_-h0-d.ic37",		0x040000, 0x38f131fd, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "f3_-l0-d.ic49",		0x040000, 0x1662969c, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "f3_-h1-.ic36",		0x040000, 0xc8d1947c, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "f3_-l1-.ic48",		0x040000, 0x7b9492fc, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "f3_-sh0-.ic24",		0x010000, 0x90b55e5e, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "f3_-sl0-.ic31",		0x010000, 0x77c16d57, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "f3_w50.c0.ic1",		0x040000, 0x47e788ee, 3 | BRF_GRA },           //  6 Background Tiles
	{ "f3_w51.c1.ic2",		0x040000, 0x8e535e3f, 3 | BRF_GRA },           //  7
	{ "f3_w52.c2.ic16",		0x040000, 0xa6eb2e56, 3 | BRF_GRA },           //  8
	{ "f3_w53.c3.ic17",		0x040000, 0x2f992807, 3 | BRF_GRA },           //  9

	{ "f3_w37.000.ic44",	0x100000, 0xfd4cda03, 4 | BRF_GRA },           // 10 Sprites
	{ "f3_w38.001.ic32",	0x100000, 0x755bab10, 4 | BRF_GRA },           // 11
	{ "f3_w39.010.ic45",	0x100000, 0xb21ced92, 4 | BRF_GRA },           // 12
	{ "f3_w40.011.ic33",	0x100000, 0x2e906889, 4 | BRF_GRA },           // 13
	{ "f3_w41.020.ic46",	0x100000, 0x02455d10, 4 | BRF_GRA },           // 14
	{ "f3_w42.021.ic34",	0x100000, 0x124589b9, 4 | BRF_GRA },           // 15
	{ "f3_w43.030.ic47",	0x100000, 0xdae7327a, 4 | BRF_GRA },           // 16
	{ "f3_w44.031.ic35",	0x100000, 0xd0fc84ac, 4 | BRF_GRA },           // 17

	{ "f3_w95.da.ic10",		0x080000, 0xf7ca432b, 5 | BRF_SND },           // 18 Irem GA20 Samples

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 19 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 20
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 21
};

STD_ROM_PICK(psoldier)
STD_ROM_FN(psoldier)

struct BurnDriver BurnDrvPsoldier = {
	"psoldier", "ssoldier", NULL, NULL, "1993",
	"Perfect Soldiers (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_VSFIGHT, 0,
	NULL, psoldierRomInfo, psoldierRomName, NULL, NULL, NULL, NULL, PsoldierInputInfo, PsoldierDIPInfo,
	ssoldierInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Major Title 2 (World, set 1)

static struct BurnRomInfo majtitl2RomDesc[] = {
	{ "mt2_h0-b.ic34",		0x040000, 0xb163b12e, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mt2_l0-b.ic31",		0x040000, 0x6f3b5d9d, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mt2_h1-.ic33",		0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mt2_l1-.ic32",		0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-.ic14",		0x010000, 0x1ecbea43, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-.ic17",		0x010000, 0x8fd5b531, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep.ic30",		0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(majtitl2)
STD_ROM_FN(majtitl2)

static INT32 majtitl2RomLoad()
{
	m92_banks = 1;
	return RomLoad(0x100000, 0x040000, 0x100000, 0, 15);
}

static INT32 majtitl2Init()
{
	m92_kludge = 2;
	return DrvInit(majtitl2RomLoad, majtitl2_decryption_table, 1, 0x100000, 0x400000);
}

struct BurnDriver BurnDrvMajtitl2 = {
	"majtitl2", NULL, NULL, NULL, "1992",
	"Major Title 2 (World, set 1)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, majtitl2RomInfo, majtitl2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Major Title 2 (World, set 2)

static struct BurnRomInfo majtitl2bRomDesc[] = {
	{ "mt2_h0-e.ic34",		0x040000, 0xf6c3a28c, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mt2_l0-e.ic31",		0x040000, 0x0a061384, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mt2_h1-.ic33",		0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mt2_l1-.ic32",		0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-.ic14",		0x010000, 0x1ecbea43, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-.ic17",		0x010000, 0x8fd5b531, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep.ic30",		0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(majtitl2b)
STD_ROM_FN(majtitl2b)

struct BurnDriver BurnDrvMajtitl2b = {
	"majtitl2b", "majtitl2", NULL, NULL, "1992",
	"Major Title 2 (World, set 2)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, majtitl2bRomInfo, majtitl2bRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Major Title 2 (World, set 1, alt sound CPU)
// this set matches the 'majtitl2' except for the soundcpu roms, which are for a different CPU

static struct BurnRomInfo majtitl2aRomDesc[] = {
	{ "mt2_h0-.ic34",		0x040000, 0xb163b12e, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mt2_l0-.ic31",		0x040000, 0x6f3b5d9d, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mt2_h1-.ic33",		0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mt2_l1-.ic32",		0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-a.ic14",		0x010000, 0x50f076e5, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-a.ic17",		0x010000, 0xf4ecd7b5, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep.ic30",		0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(majtitl2a)
STD_ROM_FN(majtitl2a)

static INT32 majtitl2aInit()
{
	m92_kludge = 2;
	return DrvInit(majtitl2RomLoad, mysticri_decryption_table, 1, 0x100000, 0x400000);
}

struct BurnDriver BurnDrvMajtitl2a = {
	"majtitl2a", "majtitl2", NULL, NULL, "1992",
	"Major Title 2 (World, set 1, alt sound CPU)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, majtitl2aRomInfo, majtitl2aRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2aInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Major Title 2 (Japan)

static struct BurnRomInfo majtitl2jRomDesc[] = {
	{ "mt2_h0.ic34",		0x040000, 0x8a8d71ad, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mt2_l1.ic31",		0x040000, 0xdd4fff51, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mt2_h1-.ic33",		0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mt2_l1-.ic32",		0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-.ic14",		0x010000, 0x1ecbea43, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-.ic17",		0x010000, 0x8fd5b531, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep.ic30",		0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(majtitl2j)
STD_ROM_FN(majtitl2j)

struct BurnDriver BurnDrvMajtitl2j = {
	"majtitl2j", "majtitl2", NULL, NULL, "1992",
	"Major Title 2 (Japan)\0", NULL, "Irem", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, majtitl2jRomInfo, majtitl2jRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// The Irem Skins Game (US set 1)

static struct BurnRomInfo skingameRomDesc[] = {
	{ "is-h0-d.ic34",		0x040000, 0x80940abb, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "is-l0-d.ic31",		0x040000, 0xb84beed6, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "is-h1.ic33",			0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "is-l1.ic32",			0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-.ic14",		0x010000, 0x1ecbea43, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-.ic17",		0x010000, 0x8fd5b531, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep",				0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(skingame)
STD_ROM_FN(skingame)

struct BurnDriver BurnDrvSkingame = {
	"skingame", "majtitl2", NULL, NULL, "1992",
	"The Irem Skins Game (US set 1)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, skingameRomInfo, skingameRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// The Irem Skins Game (US set 2)

static struct BurnRomInfo skingame2RomDesc[] = {
	{ "mt2_h0-a.ic34",		0x040000, 0x7c6dbbc7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "mt2_l0-a.ic31",		0x040000, 0x9de5f689, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "mt2_h1-.ic33",		0x040000, 0x9ba8e1f2, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "mt2_l1-.ic32",		0x040000, 0xe4e00626, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "mt2_sh0-.ic14",		0x010000, 0x1ecbea43, 2 | BRF_PRG | BRF_ESS }, //  4 V30 Code
	{ "mt2_sl0-.ic17",		0x010000, 0x8fd5b531, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "hr0.ic9",			0x040000, 0x7e61e4b5, 3 | BRF_GRA },           //  6 Background Tiles
	{ "hr1.ic10",			0x040000, 0x0a667564, 3 | BRF_GRA },           //  7
	{ "hr2.ic11",			0x040000, 0x5eb44312, 3 | BRF_GRA },           //  8
	{ "hr3.ic12",			0x040000, 0xf2866294, 3 | BRF_GRA },           //  9

	{ "k30.ic42",			0x100000, 0x8c9a2678, 4 | BRF_GRA },           // 10 Sprites
	{ "k31.ic43",			0x100000, 0x5455df78, 4 | BRF_GRA },           // 11
	{ "k32.ic44",			0x100000, 0x3a258c41, 4 | BRF_GRA },           // 12
	{ "k33.ic45",			0x100000, 0xc1e91a14, 4 | BRF_GRA },           // 13

	{ "k0d.ic8",			0x080000, 0x713b9e9f, 5 | BRF_SND },           // 14 Irem GA20 Samples

	{ "mt2eep.ic30",		0x000800, 0x208af971, 6 | BRF_PRG | BRF_ESS }, // 15 EEPROM data

	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },           // 16 PLDs
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },           // 17
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },           // 18
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },           // 19
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },           // 20
};

STD_ROM_PICK(skingame2)
STD_ROM_FN(skingame2)

struct BurnDriver BurnDrvSkingame2 = {
	"skingame2", "majtitl2", NULL, NULL, "1992",
	"The Irem Skins Game (US set 2)\0", NULL, "Irem America", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M92, GBF_SPORTSMISC, 0,
	NULL, skingame2RomInfo, skingame2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Majtitl2DIPInfo,
	majtitl2Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// -----------------------------------------------------------------------------
// Ninja Baseball Batman Series
// -----------------------------------------------------------------------------

#define NBBATMAN_COMPONENTS													\
	{ "a1_-h1-.ic33",		0x040000, 0x3ce2aab5, 1 | BRF_PRG | BRF_ESS },	\
	{ "a1_-l1-.ic32",		0x040000, 0x116d9bcc, 1 | BRF_PRG | BRF_ESS },	\
	{ "a1_-sh0-.ic14",		0x010000, 0xb7fae3e6, 2 | BRF_PRG | BRF_ESS },	\
	{ "a1_-sl0-.ic17",		0x010000, 0xb26d54fc, 2 | BRF_PRG | BRF_ESS },	\
	{ "lh534k0c.ic9",		0x080000, 0x314a0c6d, 3 | BRF_GRA },			\
	{ "lh534k0e.ic10",		0x080000, 0xdc31675b, 3 | BRF_GRA },			\
	{ "lh534k0f.ic11",		0x080000, 0xe15d8bfb, 3 | BRF_GRA },			\
	{ "lh534k0g.ic12",		0x080000, 0x888d71a3, 3 | BRF_GRA },			\
	{ "lh538393.ic42",		0x100000, 0x26cdd224, 4 | BRF_GRA },			\
	{ "lh538394.ic43",		0x100000, 0x4bbe94fa, 4 | BRF_GRA },			\
	{ "lh538395.ic44",		0x100000, 0x2a533b5e, 4 | BRF_GRA },			\
	{ "lh538396.ic45",		0x100000, 0x863a66fa, 4 | BRF_GRA },			\
	{ "lh534k0k.ic8",		0x080000, 0x735e6380, 5 | BRF_SND },			\
	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },			\
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },			\
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },			\
	{ "mt2_b-2l-.ic7",		0x000117, 0x3bab14ee, 0 | BRF_OPT },			\
	{ "m92_b-7h-d.ic47",	0x000117, 0x59d86225, 0 | BRF_OPT },

// Ninja Baseball Batman (One Key Edition, Hack)
// Modified by 哆啦A梦
// 20200906

static struct BurnRomInfo nbbatmanoRomDesc[] = {
	{ "a1_-h0o-c.ic34",		0x040000, 0xad7e9c69, 1 | BRF_PRG | BRF_ESS },
	{ "a1_-l0o-c.ic31",		0x040000, 0x17da7b70, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbatmano)
STD_ROM_FN(nbbatmano)

struct BurnDriver BurnDrvNbbatmano = {
	"nbbatmano", "nbbatman", NULL, NULL, "2020",
	"Ninja Baseball Batman (One Key Edition, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbatmanoRomInfo, nbbatmanoRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Batman (X2, Hack)
// GOTVG 20180505

static struct BurnRomInfo nbbatmx2RomDesc[] = {
	{ "nbx2_-h0-c.ic34",	0x040000, 0x81e706e6, 1 | BRF_PRG | BRF_ESS },
	{ "nbx2_-l0-c.ic31",	0x040000, 0x353c90e9, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbatmx2)
STD_ROM_FN(nbbatmx2)

struct BurnDriver BurnDrvNbbatmx2 = {
	"nbbatmx2", "nbbatman", NULL, NULL, "2018",
	"Ninja Baseball Batman (X2, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbatmx2RomInfo, nbbatmx2RomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Batman (Transform, Hack)
// GOTVG 20161203

static struct BurnRomInfo nbbatmbhRomDesc[] = {
	{ "nbbh_-h0-a.ic34",	0x040000, 0xb49f2163, 1 | BRF_PRG | BRF_ESS },
	{ "nbbh_-l0-a.ic31",	0x040000, 0x0183e06b, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbatmbh)
STD_ROM_FN(nbbatmbh)

struct BurnDriver BurnDrvNbbatmbh = {
	"nbbatmbh", "nbbatman", NULL, NULL, "2016",
	"Ninja Baseball Batman (Transform, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbatmbhRomInfo, nbbatmbhRomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Batman (1v2, Hack)
// GOTVG 20200418

static struct BurnRomInfo nbbat1v2RomDesc[] = {
	{ "nb1v2_-h0-c.ic34",	0x040000, 0x331c5d1d, 1 | BRF_PRG | BRF_ESS },
	{ "nb1v2_-l0-c.ic31",	0x040000, 0x7ac5c39e, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbat1v2)
STD_ROM_FN(nbbat1v2)

struct BurnDriver BurnDrvNbbat1v2 = {
	"nbbat1v2", "nbbatman", NULL, NULL, "2020",
	"Ninja Baseball Batman (1v2, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbat1v2RomInfo, nbbat1v2RomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Batman (1v4, Hack)
// GOTVG 20200418

static struct BurnRomInfo nbbat1v4RomDesc[] = {
	{ "nb1v4_-h0-c.ic34",	0x040000, 0x7c17d59a, 1 | BRF_PRG | BRF_ESS },
	{ "nb1v4_-l0-c.ic31",	0x040000, 0xe54a6e81, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbat1v4)
STD_ROM_FN(nbbat1v4)

struct BurnDriver BurnDrvNbbat1v4 = {
	"nbbat1v4", "nbbatman", NULL, NULL, "2020",
	"Ninja Baseball Batman (1v4, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbat1v4RomInfo, nbbat1v4RomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Ninja Baseball Batman (1v8, Hack)
// GOTVG 20200108

static struct BurnRomInfo nbbat1v8RomDesc[] = {
	{ "nb1v8_-h0-c.ic34",	0x040000, 0x6b0d4c4a, 1 | BRF_PRG | BRF_ESS },
	{ "nb1v8_-l0-c.ic31",	0x040000, 0xae7bafa9, 1 | BRF_PRG | BRF_ESS },

	NBBATMAN_COMPONENTS
};

STD_ROM_PICK(nbbat1v8)
STD_ROM_FN(nbbat1v8)

struct BurnDriver BurnDrvNbbat1v8 = {
	"nbbat1v8", "nbbatman", NULL, NULL, "2020",
	"Ninja Baseball Batman (1v8, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, nbbat1v8RomInfo, nbbat1v8RomName, NULL, NULL, NULL, NULL, nbbatmanInputInfo, NbbatmanDIPInfo,
	nbbatmanInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};

#undef NBBATMAN_COMPONENTS


// -----------------------------------------------------------------------------
// Hook Series
// -----------------------------------------------------------------------------

#define HOOK_V33PRG2														\
	{ "hook_-h1-.ic24",		0x020000, 0x264ba1f0, 1 | BRF_PRG | BRF_ESS },	\
	{ "hook_-l1-.ic37",		0x020000, 0xf9913731, 1 | BRF_PRG | BRF_ESS },
#define HOOKJ_V30PRG														\
	{ "hook_-sh0-a.ic27",	0x010000, 0xbd3d1f61, 2 | BRF_PRG | BRF_ESS },	\
	{ "hook_-sl0-a.ic28",	0x010000, 0x76371def, 2 | BRF_PRG | BRF_ESS },
#define HOOK_MISC															\
	{ "hook_-c0-.ic4",		0x040000, 0xdec63dcf, 3 | BRF_GRA },			\
	{ "hook_-c1-.ic3",		0x040000, 0xe4eb0b92, 3 | BRF_GRA },			\
	{ "hook_-c2-.ic2",		0x040000, 0xa52b320b, 3 | BRF_GRA },			\
	{ "hook_-c3-.ic1",		0x040000, 0x7ef67731, 3 | BRF_GRA },			\
	{ "hook_-000-.ic33",	0x100000, 0xccceac30, 4 | BRF_GRA },			\
	{ "hook_-010-.ic34",	0x100000, 0x8ac8da67, 4 | BRF_GRA },			\
	{ "hook_-020-.ic35",	0x100000, 0x8847af9a, 4 | BRF_GRA },			\
	{ "hook_-030-.ic36",	0x100000, 0x239e877e, 4 | BRF_GRA },			\
	{ "hook_-da-.ic11",		0x080000, 0x88cd0212, 5 | BRF_SND },			\
	{ "m92_a-3m-.ic11",		0x000117, 0xfc718efe, 0 | BRF_OPT },			\
	{ "m92_a-7j-.ic41",		0x000117, 0x5730b25a, 0 | BRF_OPT },			\
	{ "m92_a-9j-.ic51",		0x000117, 0x92d477cf, 0 | BRF_OPT },			\
	{ "m92_d-3j-c.ic26",	0x000117, 0x9ec35216, 0 | BRF_OPT },			\
	{ "m92_d-3p-.ic29",		0x000117, 0x3f336904, 0 | BRF_OPT },
#define HOOK_COMPONENTS														\
	HOOK_V33PRG2															\
	{ "hook_-sh0-.ic27",	0x010000, 0x86a4e56e, 2 | BRF_PRG | BRF_ESS },	\
	{ "hook_-sl0-.ic28",	0x010000, 0x10fd9676, 2 | BRF_PRG | BRF_ESS },	\
	HOOK_MISC
#define HOOKJ_COMPONENTS													\
	HOOK_V33PRG2															\
	HOOKJ_V30PRG															\
	HOOK_MISC

// Hook (Xin Er, Hack)
// GOTVG 20180914

static struct BurnRomInfo hookxrRomDesc[] = {
	{ "hkxr_-h0-c.ic25",	0x040000, 0xa603d006, 1 | BRF_PRG | BRF_ESS },
	{ "hkxr_-l0-c.ic38",	0x040000, 0x8a1e8688, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookxr)
STD_ROM_FN(hookxr)

struct BurnDriver BurnDrvHookxr = {
	"hookxr", "hook", NULL, NULL, "2018",
	"Hook (Xin Er, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookxrRomInfo, hookxrRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Warriors, Hack)
// GOTVG 20160818

static struct BurnRomInfo hookdwRomDesc[] = {
	{ "hkdw_-h0-c.ic25",	0x040000, 0x9124994c, 1 | BRF_PRG | BRF_ESS },
	{ "hkdw_-l0-c.ic38",	0x040000, 0xcaae5586, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookdw)
STD_ROM_FN(hookdw)

struct BurnDriver BurnDrvHookdw = {
	"hookdw", "hook", NULL, NULL, "2016",
	"Hook (Warriors, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookdwRomInfo, hookdwRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (1v4, Hack)
// GOTVG 20160625

static struct BurnRomInfo hook1v4RomDesc[] = {
	{ "hk1v4_-h0-g.ic25",	0x040000, 0x13fa1eca, 1 | BRF_PRG | BRF_ESS },
	{ "hk1v4_-l0-g.ic38",	0x040000, 0xf3efc301, 1 | BRF_PRG | BRF_ESS },

	HOOKJ_COMPONENTS
};

STD_ROM_PICK(hook1v4)
STD_ROM_FN(hook1v4)

struct BurnDriver BurnDrvHook1v4 = {
	"hook1v4", "hook", NULL, NULL, "2016",
	"Hook (1v4, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hook1v4RomInfo, hook1v4RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (X2, Hack)
// GOTVG 20180505

static struct BurnRomInfo hookx2RomDesc[] = {
	{ "hkx2_-h0-d.ic25",	0x040000, 0x637f7d17, 1 | BRF_PRG | BRF_ESS },
	{ "hkx2_-l0-d.ic38",	0x040000, 0x180e4bbb, 1 | BRF_PRG | BRF_ESS },
	{ "hox2_-h1-.ic24",		0x040000, 0x9573d8b3, 1 | BRF_PRG | BRF_ESS },
	{ "hox2_-l1-.ic37",		0x040000, 0xeb5cd51c, 1 | BRF_PRG | BRF_ESS },

	{ "hook_-sh0-.ic27",	0x010000, 0x86a4e56e, 2 | BRF_PRG | BRF_ESS },
	{ "hook_-sl0-.ic28",	0x010000, 0x10fd9676, 2 | BRF_PRG | BRF_ESS },

	HOOK_MISC
};

STD_ROM_PICK(hookx2)
STD_ROM_FN(hookx2)

struct BurnDriver BurnDrvHookx2 = {
	"hookx2", "hook", NULL, NULL, "2018",
	"Hook (X2, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookx2RomInfo, hookx2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Pirate Wars, Hack)
// GOTVG 20240226

static struct BurnRomInfo hookhdRomDesc[] = {
	{ "hkhd_-h0-d.ic25",	0x040000, 0x9132b3a8, 1 | BRF_PRG | BRF_ESS },
	{ "hkhd_-l0-d.ic38",	0x040000, 0x2ef8ec74, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookhd)
STD_ROM_FN(hookhd)

struct BurnDriver BurnDrvHookhd = {
	"hookhd", "hook", NULL, NULL, "2024",
	"Hook (Pirate Wars, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookhdRomInfo, hookhdRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Pirate Wars II, Hack)
// GOTVG 20240227

static struct BurnRomInfo hookhd2RomDesc[] = {
	{ "hkhd2_-h0-d.ic25",	0x040000, 0xf7d949a5, 1 | BRF_PRG | BRF_ESS },
	{ "hkhd2_-l0-d.ic38",	0x040000, 0xf3fede71, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookhd2)
STD_ROM_FN(hookhd2)

struct BurnDriver BurnDrvHookhd2 = {
	"hookhd2", "hook", NULL, NULL, "2024",
	"Hook (Pirate Wars II, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookhd2RomInfo, hookhd2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (POK KAI, Hack)
// GOTVG 20240110

static struct BurnRomInfo hookpjRomDesc[] = {
	{ "hkpj_-h0-d.ic25",	0x040000, 0x9e0b8314, 1 | BRF_PRG | BRF_ESS },
	{ "hkpj_-l0-d.ic38",	0x040000, 0xbbb93f53, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookpj)
STD_ROM_FN(hookpj)

struct BurnDriver BurnDrvHookpj = {
	"hookpj", "hook", NULL, NULL, "2024",
	"Hook (POK KAI, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookpjRomInfo, hookpjRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Plus, Hack)
// GOTVG 20240327

static struct BurnRomInfo hookplsRomDesc[] = {
	{ "hkpls_-h0-d.ic25",	0x040000, 0x3403ba10, 1 | BRF_PRG | BRF_ESS },
	{ "hkpls_-l0-d.ic38",	0x040000, 0x5d6cf1af, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookpls)
STD_ROM_FN(hookpls)

struct BurnDriver BurnDrvHookpls = {
	"hookpls", "hook", NULL, NULL, "2024",
	"Hook (Plus, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookplsRomInfo, hookplsRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Gai Gai, Hack)
// GOTVG 20240226

static struct BurnRomInfo hookboRomDesc[] = {
	{ "hkbo_-h0-d.ic25",	0x040000, 0xac8e5749, 1 | BRF_PRG | BRF_ESS },
	{ "hkbo_-l0-d.ic38",	0x040000, 0x1c29754f, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookbo)
STD_ROM_FN(hookbo)

struct BurnDriver BurnDrvHookbo = {
	"hookbo", "hook", NULL, NULL, "2024",
	"Hook (Gai Gai, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookboRomInfo, hookboRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Summit War, Hack)
// GOTVG 20230313

static struct BurnRomInfo hookdszzRomDesc[] = {
	{ "hkdszz_-h0-d.ic25",	0x040000, 0x2fd40639, 1 | BRF_PRG | BRF_ESS },
	{ "hkdszz_-l0-d.ic38",	0x040000, 0x517fff64, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookdszz)
STD_ROM_FN(hookdszz)

struct BurnDriver BurnDrvHookdszz = {
	"hookdszz", "hook", NULL, NULL, "2023",
	"Hook (Summit War, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookdszzRomInfo, hookdszzRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (6th Emperor, Hack)
// GOTVG 20221204

static struct BurnRomInfo hook6hRomDesc[] = {
	{ "hk6h_-h0-d.ic25",	0x040000, 0x2bcab5ac, 1 | BRF_PRG | BRF_ESS },
	{ "hk6h_-l0-d.ic38",	0x040000, 0x5ea41729, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hook6h)
STD_ROM_FN(hook6h)

struct BurnDriver BurnDrvHook6h = {
	"hook6h", "hook", NULL, NULL, "2022",
	"Hook (6th Emperor, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hook6hRomInfo, hook6hRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (The Expendables, Hack)
// GOTVG 20220721

static struct BurnRomInfo hookgsRomDesc[] = {
	{ "hkgs_-h0-d.ic25",	0x040000, 0x07f870fd, 1 | BRF_PRG | BRF_ESS },
	{ "hkgs_-l0-d.ic38",	0x040000, 0x94b65fc4, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookgs)
STD_ROM_FN(hookgs)

struct BurnDriver BurnDrvHookgs = {
	"hookgs", "hook", NULL, NULL, "2022",
	"Hook (The Expendables, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookgsRomInfo, hookgsRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (The Expendables II, Hack)
// GOTVG 20230705

static struct BurnRomInfo hookgs2RomDesc[] = {
	{ "hkgs2_-h0-d.ic25",	0x040000, 0x054bd1e6, 1 | BRF_PRG | BRF_ESS },
	{ "hkgs2_-l0-d.ic38",	0x040000, 0x2a77b0a6, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookgs2)
STD_ROM_FN(hookgs2)

struct BurnDriver BurnDrvHookgs2 = {
	"hookgs2", "hook", NULL, NULL, "2023",
	"Hook (The Expendables II, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookgs2RomInfo, hookgs2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Hook's Reversal, Hack)
// GOTVG 20220721

static struct BurnRomInfo hooknxRomDesc[] = {
	{ "hknx_-h0-d.ic25",	0x040000, 0xf2cf3627, 1 | BRF_PRG | BRF_ESS },
	{ "hknx_-l0-d.ic38",	0x040000, 0x3aa4f997, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hooknx)
STD_ROM_FN(hooknx)

struct BurnDriver BurnDrvHooknx = {
	"hooknx", "hook", NULL, NULL, "2022",
	"Hook (Hook's Reversal, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hooknxRomInfo, hooknxRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Hook's Reversal II, Hack)
// GOTVG 20220807

static struct BurnRomInfo hooknx2RomDesc[] = {
	{ "hknx2_-h0-d.ic25",	0x040000, 0x885124b3, 1 | BRF_PRG | BRF_ESS },
	{ "hknx2_-l0-d.ic38",	0x040000, 0x28bb5acf, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hooknx2)
STD_ROM_FN(hooknx2)

struct BurnDriver BurnDrvHooknx2 = {
	"hooknx2", "hook", NULL, NULL, "2022",
	"Hook (Hook's Reversal II, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hooknx2RomInfo, hooknx2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (XX, Hack)
// GOTVG 20250114

static struct BurnRomInfo hookxxRomDesc[] = {
	{ "hkxx_-h0-d.ic25",	0x040000, 0x77cde5c5, 1 | BRF_PRG | BRF_ESS },
	{ "hkxx_-l0-d.ic38",	0x040000, 0x106d883a, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookxx)
STD_ROM_FN(hookxx)

struct BurnDriver BurnDrvHookxx = {
	"hookxx", "hook", NULL, NULL, "2025",
	"Hook (XX, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookxxRomInfo, hookxxRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (SW, Hack)
// GOTVG 20220111

static struct BurnRomInfo hookswRomDesc[] = {
	{ "hksw_-h0-d.ic25",	0x040000, 0x50acf650, 1 | BRF_PRG | BRF_ESS },
	{ "hksw_-l0-d.ic38",	0x040000, 0x56446fb8, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hooksw)
STD_ROM_FN(hooksw)

struct BurnDriver BurnDrvHooksw = {
	"hooksw", "hook", NULL, NULL, "2022",
	"Hook (SW, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookswRomInfo, hookswRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (SW2, Hack)
// GOTVG 20250118

static struct BurnRomInfo hooksw2RomDesc[] = {
	{ "hksw2_-h0-d.ic25",	0x040000, 0xd911aeec, 1 | BRF_PRG | BRF_ESS },
	{ "hksw2_-l0-d.ic38",	0x040000, 0x6f74bf82, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hooksw2)
STD_ROM_FN(hooksw2)

struct BurnDriver BurnDrvHooksw2 = {
	"hooksw2", "hook", NULL, NULL, "2025",
	"Hook (SW2, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hooksw2RomInfo, hooksw2RomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Elite, Hack)
// GOTVG 20240621

static struct BurnRomInfo hookjyRomDesc[] = {
	{ "hkjy_-h0-d.ic25",	0x040000, 0x6bcc9679, 1 | BRF_PRG | BRF_ESS },
	{ "hkjy_-l0-d.ic38",	0x040000, 0xece996a6, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookjy)
STD_ROM_FN(hookjy)

struct BurnDriver BurnDrvHookjy = {
	"hookjy", "hook", NULL, NULL, "2024",
	"Hook (Elite, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookjyRomInfo, hookjyRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Elite Competition, Hack)
// GOTVG 20250208

static struct BurnRomInfo hookjydsRomDesc[] = {
	{ "hkjyds_-h0-d.ic25",	0x040000, 0xba845593, 1 | BRF_PRG | BRF_ESS },
	{ "hkjyds_-l0-d.ic38",	0x040000, 0x69891198, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookjyds)
STD_ROM_FN(hookjyds)

struct BurnDriver BurnDrvHookjyds = {
	"hookjyds", "hook", NULL, NULL, "2025",
	"Hook (Elite Competition, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookjydsRomInfo, hookjydsRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Hook (Item, Hack)
// GOTVG 20250109

static struct BurnRomInfo hookdjRomDesc[] = {
	{ "hkdj_-h0-d.ic25",	0x040000, 0xb9fd79f4, 1 | BRF_PRG | BRF_ESS },
	{ "hkdj_-l0-d.ic38",	0x040000, 0x5576e39e, 1 | BRF_PRG | BRF_ESS },

	HOOK_COMPONENTS
};

STD_ROM_PICK(hookdj)
STD_ROM_FN(hookdj)

struct BurnDriver BurnDrvHookdj = {
	"hookdj", "hook", NULL, NULL, "2025",
	"Hook (Item, Hack)\0", NULL, "hack", "Irem M92",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HACK | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M92, GBF_SCRFIGHT, 0,
	NULL, hookdjRomInfo, hookdjRomName, NULL, NULL, NULL, NULL, p4CommonInputInfo, Hook4pDIPInfo,
	hookInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};

#undef HOOK_V33PRG2
#undef HOOKJ_V30PRG
#undef HOOK_MISC
#undef HOOK_COMPONENTS
#undef HOOKJ_COMPONENTS
