// FB Alpha Irem M107 system driver
// Based on MAME driver by Bryan McPhail
// Based on port of M92 driver from MAME by OopsWare

#include "tiles_generic.h"
#include "rei_defer.h" // (ReiREM: deferred drawing)
#include "rei_m107ge.h" // (ReiREM: PSP tile layer GE mode)
#include "rei_me.h"
#include "rei_prof.h"
#include "rei_line.h" // ReiREM PSP port: per-line code block
#include "burn_ym2151.h"
#include "nec_intf.h"
#include "irem_cpu.h"
#include "iremga20.h"
#include "stddef.h"

static UINT8 *Mem;
static UINT8 *MemEnd;
static UINT8 *RamStart;
static UINT8 *RamEnd;
static UINT8 *DrvV33ROM;
static UINT8 *DrvV30ROM;
static UINT8 *DrvGfxROM0;
static UINT8 *DrvGfxROM1;
static UINT8 *DrvSprTable;
static UINT8 *DrvSndROM;
static UINT8 *DrvVidRAM;
static UINT8 *DrvV33RAM;
static UINT8 *DrvV30RAM;
static UINT8 *DrvSprRAM;
static UINT8 *DrvSprBuf;
static UINT8 *DrvPalRAM;
static UINT8 *RamPrioBitmap;

static UINT32 *DrvPalette;
static UINT8 bRecalcPalette;

static UINT8 *pf_control[4];
static UINT8 *sound_status;
static UINT8 *sound_latch;
#ifdef REI_ME_SPLIT
// (ReiREM PSP) When the sound side runs on the ME, values it writes live in the ME region so they share no cache lines with main's (rei_me.h).
// The latch save slot stays inside "All Ram" (same format): exchanged with m107_latch_ram before save / after load (DrvScan)
static UINT8 m107_me_latch[4] REI_ME_DATA;
static UINT8 *m107_latch_ram;
#endif

static INT32 sprite_enable;
static INT32 raster_irq_position;
static INT32 sound_cpu_reset REI_ME_DATA;

static INT32 nBankswitchData;
static INT32 has_bankswitch = 0;

static UINT8 DrvButton[8];
static UINT8 DrvJoy1[8];
static UINT8 DrvJoy2[8];
static UINT8 DrvJoy3[8];
static UINT8 DrvJoy4[8];
static UINT8 DrvInput[8];
static UINT8 DrvReset;

static INT32 config_cpu_speed;
static INT32 spritesystem;
static INT32 vblank;
static INT32 irq_vectorbase;
static INT32 graphics_mask[2];
#ifndef REI_ORIGINAL_DRAW
// (ReiREM) Per row of each 8x8 tile: bit r = row r has a nonzero pixel, bit 8+r = row r is all nonzero. Built by main at init,
// read-only for drawing (incl. ME) — draw_layer_byline skips empty rows and writes full rows without tests (same result)
static UINT16 *m107_rowpens;
static UINT16 *m107_sprrows; // (16x16 sprite tiles: bit r = row r has a nonzero pixel)
static void m107_sprrows_build(const UINT8 *gfx, INT32 tiles)
{
	m107_sprrows = (UINT16 *)BurnMalloc(tiles * sizeof(UINT16));
	if (!m107_sprrows) return;
	for (INT32 t = 0; t < tiles; t++) {
		UINT16 f = 0;
		for (INT32 r = 0; r < 16; r++) {
			const UINT8 *p = gfx + t * 0x100 + r * 16;
			for (INT32 x = 0; x < 16; x++)
				if (p[x]) { f |= 1 << r; break; }
		}
		m107_sprrows[t] = f;
	}
}
static void m107_rowpens_build(const UINT8 *gfx, INT32 tiles)
{
	m107_rowpens = (UINT16 *)BurnMalloc(tiles * sizeof(UINT16));
	if (!m107_rowpens) return;
	for (INT32 t = 0; t < tiles; t++) {
		UINT16 f = 0;
		for (INT32 r = 0; r < 8; r++) {
			const UINT8 *p = gfx + t * 0x40 + r * 8;
			INT32 nz = 0;
			for (INT32 x = 0; x < 8; x++) nz += (p[x] != 0);
			if (nz) f |= 1 << r;
			if (nz == 8) f |= 0x100 << r;
		}
		m107_rowpens[t] = f;
	}
}

// (ReiREM) GE mode (rei_m107ge.h): ops chosen by the frontend (set by main before starting the ME, read by render — uncached),
// render-side frame state (render only), sprite coverage table (allocated in init, render only)
static const rei_m107_ge_ops *m107_ge_ops REI_ME_SHARED;
static INT32 m107_ge_check REI_ME_SHARED;
static REI_RDR const rei_m107_ge_ops *m107_cur;
static REI_RDR rei_m107_spr m107_spr;
static REI_RDR INT32 m107_nb, m107_ab;
static REI_RDR INT16 m107_cov_y0, m107_cov_y1 = 240; // (lines in the coverage table that had sprites last frame — all at first)
static UINT8 *m107_cov;
static UINT16 m107_palshadow[0x800]; // (previous palette RAM values: m107_pal_sync)
extern "C" void rei_m107_set_ge(const rei_m107_ge_ops *ops, int check)
{
	REI_SH(m107_ge_ops) = ops;
	REI_SH(m107_ge_check) = check;
}
extern "C" int rei_m107_get_tiles(rei_m107_tiles *out)
{
	if (!Mem || !m107_rowpens || !m107_cov) return -1;
	out->gfx = DrvGfxROM0;
	out->rowpens = m107_rowpens;
	out->tile_mask = graphics_mask[0];
	return 0;
}
#endif

// (ReiREM: M107 pipeline, ADR-019) Main (V33) and sound (V35/YM2151/GA20) run separately — same rules as M92
// ADR-003/006: main→sound (latch, sound reset) applies on the same line, sound→main (status) 32 lines later. Sound follows main,
// and main may run up to 32 lines ahead, so both cores run concurrently. Result is independent of thread timing (even single-threaded)
extern "C" int rei_pipelined, rei_threaded;
extern "C" volatile int rei_stop;
extern "C" void (*rei_wait_hook)(void);
void nec_new_frame();
void v25_new_frame();
#define M107_LINES 256
#define M107_DELAY 32  // default sound→main delay in lines (m107_delay: core setting s2m_delay_lines, 192 when PSP sound runs on the ME)
#define M107_RING  256
extern "C" int rei_s2m_delay;
static INT32 m107_delay = M107_DELAY;
struct m107_m2s { INT64 line; INT32 cyc33; UINT8 type, data; }; // type 0 latch, 1 sound reset (data = in reset)
static INT64 m107_sound_line_of_ins(); // (m107_run_sound_lines below: during a merged run, the line the instruction originally ran on)
struct m107_s2m { INT64 src_line; INT64 apply; UINT8 data; };   // apply = main absolute cycle (frame start + cycle within frame)
#ifdef REI_ME_SPLIT
// (PSP) The sound side can run on the ME: shared data lives in a separately aligned region, accessed uncached only (same as M92, rei_me.h).
// 64-bit atomics cannot be shared with the ME — uncached read/write + sync (only one side writes each value)
static struct m107_m2s m107_m2s_q_[M107_RING] REI_ME_SHARED;
static struct m107_s2m m107_s2m_q_[M107_RING] REI_ME_SHARED;
static UINT32 m107_m2s_w_ REI_ME_SHARED, m107_m2s_r_ REI_ME_SHARED, m107_s2m_w_ REI_ME_SHARED, m107_s2m_r_ REI_ME_SHARED;
static INT64 m107_main_done_ REI_ME_SHARED, m107_sound_done_ REI_ME_SHARED, m107_sound_waiting_ REI_ME_SHARED = -1;
static UINT32 m107_epoch_ REI_ME_SHARED;
// (Uncached only in threaded mode (sound on ME): the single-thread pipeline (default) reads/writes these many times per line, and uncached
//  cost about 1ms in dsoccr94 gameplay. Mode is fixed at game start, and main cache is flushed before starting the ME)
#define M107_SHV(x) (*(rei_threaded ? (volatile __typeof__(x) *)REI_UNCACHED(&(x)) : (volatile __typeof__(x) *)&(x)))
#define m107_m2s_q (rei_threaded ? REI_UNCACHED(&m107_m2s_q_[0]) : &m107_m2s_q_[0])
#define m107_s2m_q (rei_threaded ? REI_UNCACHED(&m107_s2m_q_[0]) : &m107_s2m_q_[0])
#define m107_m2s_w M107_SHV(m107_m2s_w_)
#define m107_m2s_r M107_SHV(m107_m2s_r_)
#define m107_s2m_w M107_SHV(m107_s2m_w_)
#define m107_s2m_r M107_SHV(m107_s2m_r_)
#define m107_main_done M107_SHV(m107_main_done_)
#define m107_sound_done M107_SHV(m107_sound_done_)
#define m107_sound_waiting M107_SHV(m107_sound_waiting_)
#define m107_epoch M107_SHV(m107_epoch_)
#define M107_LOAD(x)     (x)
#define M107_STORE(x, v) do { if (rei_threaded) REI_ME_SYNC(); (x) = (v); if (rei_threaded) REI_ME_SYNC(); } while (0)
#else
static struct m107_m2s m107_m2s_q[M107_RING];
static struct m107_s2m m107_s2m_q[M107_RING];
static UINT32 m107_m2s_w, m107_m2s_r, m107_s2m_w, m107_s2m_r;
static INT64 m107_main_done, m107_sound_done, m107_sound_waiting = -1; // lines published, line sound is waiting on (-1 none)
static UINT32 m107_epoch; // pipeline restart count (m107_pipe_clear) — sound side discards the frame it was waiting on
#define M107_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define M107_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#endif
static INT64 m107_main_line;                                      // absolute line each side is currently on
static INT64 m107_sound_line REI_ME_DATA;
static INT32 m107_total33, m107_total35;
// (ReiREM) Main line merging (same approach as M92 PipelinedMainSpan): lines with no end-of-line work (except raster IRQ lines, 8, 248) run
// as one V33 slice. During a span, line-bound handlers derive the line from the instruction start cycle (sound latch events), and writes acting
// at line end (raster IRQ position) end the span at that line. Harness hashes match per-line runs. Fewer VezRun/dispatcher entries cut I-cache
// misses (dsoccr94 PSP: out-of-block cycles were about a third of heavy frames, 2026-10-17)
static INT32 m107_span_active, m107_span_stop, m107_span_stop_line;
static INT64 m107_span_line0;                                     // absolute number of line 0 of the frame the span runs in
extern "C" { INT32 rei_m107_span_enabled = 1; }
unsigned int nec_ins_start_cycles();
static INT32 m107_line_of_cycle(INT32 c);
static void m107_span_stop_now() // end the span after this instruction (that line finishes normally)
{
	m107_span_stop = 1;
	m107_span_stop_line = m107_line_of_cycle((INT32)nec_ins_start_cycles());
	VezRunEnd();
}

static INT32 nPrevScreenPos = 0;

typedef struct _m107_layer m107_layer;
struct _m107_layer
{
	INT32 enable;
	INT32 enable_rowscroll;

	UINT16 scrollx;
	UINT16 scrolly;

	UINT16 *scroll;
	UINT16 *vram;
};

static struct _m107_layer *m107_layers[4];

enum { VECTOR_INIT, YM2151_ASSERT, YM2151_CLEAR, V30_ASSERT, V30_CLEAR };

static struct BurnInputInfo FirebarrInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},
	{"P1 Button 3",		BIT_DIGITAL,	DrvJoy1 + 5,	"p1 fire 3"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},
	{"P2 Button 3",		BIT_DIGITAL,	DrvJoy2 + 5,	"p2 fire 3"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(Firebarr)

static struct BurnInputInfo Dsoccr94InputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvButton + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvButton + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},
	{"P1 Button 3",		BIT_DIGITAL,	DrvJoy1 + 5,	"p1 fire 3"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvButton + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvButton + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},
	{"P2 Button 3",		BIT_DIGITAL,	DrvJoy2 + 5,	"p2 fire 3"	},

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
	{"P4 Button 1",		BIT_DIGITAL,	DrvJoy4 + 5,	"p4 fire 1"	},
	{"P4 Button 2",		BIT_DIGITAL,	DrvJoy4 + 6,	"p4 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvButton + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvInput + 5,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvInput + 6,	"dip"		},
	{"Dip C",			BIT_DIPSWITCH,	DrvInput + 7,	"dip"		},
};

STDINPUTINFO(Dsoccr94)

static struct BurnDIPInfo FirebarrDIPList[]=
{
	{0x14, 0xff, 0xff, 0xfb, NULL							},
	{0x15, 0xff, 0xff, 0xbf, NULL							},
	{0x16, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Rapid Fire"					},
	{0x14, 0x01, 0x0c, 0x00, "Button 1 Normal, Button 3 Rapid Fire"			},
	{0x14, 0x01, 0x0c, 0x04, "Button 1 Rapid Fire, Button 3 No Function"	},
	{0x14, 0x01, 0x0c, 0x08, "Off"							},
	{0x14, 0x01, 0x0c, 0x0c, "Off"							},

	{0   , 0xfe, 0   ,    2, "Continuous Play"				},
	{0x14, 0x01, 0x10, 0x10, "Off"							},
	{0x14, 0x01, 0x10, 0x00, "On"							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x15, 0x01, 0x03, 0x00, "2"							},
	{0x15, 0x01, 0x03, 0x03, "3"							},
	{0x15, 0x01, 0x03, 0x02, "4"							},
	{0x15, 0x01, 0x03, 0x01, "5"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x15, 0x01, 0x40, 0x40, "Off"							},
	{0x15, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x15, 0x01, 0x80, 0x80, "Off"							},
	{0x15, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x16, 0x01, 0x01, 0x01, "Off"							},
	{0x16, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
	{0x16, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"				},
	{0x16, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"				},
	{0x16, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"				},
	{0x16, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"				},
	{0x16, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"				},
	{0x16, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x16, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"				},
	{0x16, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"				},
	{0x16, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"				},
	{0x16, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"				},
	{0x16, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"				},
	{0x16, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"				},
	{0x16, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"				},
	{0x16, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"				},
	{0x16, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"				},
	{0x16, 0x01, 0xf0, 0x00, "Free Play"						},
};

STDDIPINFO(Firebarr)

static struct BurnDIPInfo Dsoccr94DIPList[]=
{
	{0x24, 0xff, 0xff, 0xff, NULL							},
	{0x25, 0xff, 0xff, 0xbf, NULL							},
	{0x26, 0xff, 0xff, 0xf5, NULL							},

	{0   , 0xfe, 0   ,    4, "Time"							},
	{0x25, 0x01, 0x03, 0x00, "1:30"							},
	{0x25, 0x01, 0x03, 0x03, "2:00"							},
	{0x25, 0x01, 0x03, 0x02, "2:30"							},
	{0x25, 0x01, 0x03, 0x01, "3:00"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x25, 0x01, 0x0c, 0x00, "Very Easy"					},
	{0x25, 0x01, 0x0c, 0x08, "Easy"							},
	{0x25, 0x01, 0x0c, 0x0c, "Normal"						},
	{0x25, 0x01, 0x0c, 0x04, "Hard"							},

	{0   , 0xfe, 0   ,    2, "Game Mode"					},
	{0x25, 0x01, 0x10, 0x10, "Match Mode"					},
	{0x25, 0x01, 0x10, 0x00, "Power Mode"					},

	{0   , 0xfe, 0   ,    2, "Starting Button"				},
	{0x25, 0x01, 0x20, 0x00, "Button 1"						},
	{0x25, 0x01, 0x20, 0x20, "Start Button"					},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x25, 0x01, 0x40, 0x40, "Off"							},
	{0x25, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x25, 0x01, 0x80, 0x80, "Off"							},
	{0x25, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x26, 0x01, 0x01, 0x01, "Off"							},
	{0x26, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
	{0x26, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"				},
	{0x26, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"				},
	{0x26, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"				},
	{0x26, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"				},
	{0x26, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"				},
	{0x26, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x26, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"				},
	{0x26, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"				},
	{0x26, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"				},
	{0x26, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"				},
	{0x26, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"				},
	{0x26, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"				},
	{0x26, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"				},
	{0x26, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"				},
	{0x26, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"				},
	{0x26, 0x01, 0xf0, 0x00, "Free Play"						},
};

STDDIPINFO(Dsoccr94)

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

static void m107Bankswitch(INT32 data)
{
	INT32 bank = 0x80000 + ((data >> 1) & 0x03) * 0x20000;

	nBankswitchData = data;

	VezMapArea(0xa0000, 0xbffff, 0, DrvV33ROM + bank);
	VezMapArea(0xa0000, 0xbffff, 2, DrvV33ROM + bank);
}

static void m107YM2151IRQHandler(INT32 nStatus)
{
	if (VezGetActive() == -1) return;

	VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff/*default*/, nStatus ? CPU_IRQSTATUS_ACK : CPU_IRQSTATUS_NONE);
}

static UINT8 __fastcall m107ReadByte(UINT32 address)
{
	switch (address)
	{
		case 0xf0000:
		case 0xf0001:
		case 0xf0002:
		case 0xf0003:
		case 0xf0004:
		case 0xf0005:
			return 0x00; // wpksoc
	}

	return 0;
}

static void __fastcall m107WriteByte(UINT32 address, UINT8 data)
{
	if ((address & 0xff000) == 0xf9000) {
		DrvPalRAM[ address - 0xf9000] = data;
		if (address & 1) {
			INT32 offs = (address - 0xf9000) >> 1;
			DrvPalette[offs] = CalcCol( offs << 1 );
		}
		return;
	}
}

// (ReiREM) Copy of plain port 0..7 values: dynarec reads IN without a handler (rei_nec_port_shadow). Same values as m107ReadPort —
// rebuilt whenever input/DIP/vblank change (between lines, V33 not running). dsoccr94's vblank wait loop read it ~800 times per frame
static UINT8 m107_port_shadow[8] __attribute__((aligned(8)));
static void m107_port_shadow_update()
{
	m107_port_shadow[0] = DrvInput[0];
	m107_port_shadow[1] = DrvInput[1];
	m107_port_shadow[2] = (DrvInput[4] & 0x7f) | vblank;
	m107_port_shadow[3] = DrvInput[5];
	m107_port_shadow[4] = DrvInput[6];
	m107_port_shadow[5] = DrvInput[7];
	m107_port_shadow[6] = DrvInput[2];
	m107_port_shadow[7] = DrvInput[3];
}

static UINT8 __fastcall m107ReadPort(UINT32 port)
{
	switch (port)
	{
		case 0x00: return  DrvInput[0];
		case 0x01: return  DrvInput[1];

		case 0x02: return (DrvInput[4] & 0x7f) | vblank;
		case 0x03: return  DrvInput[5];

		case 0x04: return  DrvInput[6];
		case 0x05: return  DrvInput[7];

		case 0x06: return  DrvInput[2];
		case 0x07: return  DrvInput[3];

		case 0x08: VezSetIRQLineAndVector(0, (irq_vectorbase + 12)/4, CPU_IRQSTATUS_NONE); return sound_status[0]; 
		case 0x09: VezSetIRQLineAndVector(0, (irq_vectorbase + 12)/4, CPU_IRQSTATUS_NONE); return sound_status[1];

		case 0x18: return 0;

		case 0xc0:
			return 0x02; // wpksoc - required to boot?

		case 0xc1:
			return 0x02; //

		case 0xc2:
		case 0xc3:
			return 0x00; // wpksoc
	}

	return 0;
}

static void set_pf_info(INT32 layer)
{
	struct _m107_layer *ptr = m107_layers[layer];

	INT32 data = (pf_control[layer][5] << 8) + pf_control[layer][4];

	ptr->enable				= (~data >> 7) & 1;
	ptr->vram				= (UINT16*)(DrvVidRAM + (((data >> 8) & 0x0f) * 0x1000));
	ptr->enable_rowscroll	= data & 0x03;
}

static void set_pf_scroll(INT32 layer)
{
	struct _m107_layer *ptr = m107_layers[layer];

	ptr->scrollx = (pf_control[layer][2] << 0) | (pf_control[layer][3] << 8);
	ptr->scrolly = (pf_control[layer][0] << 0) | (pf_control[layer][1] << 8);
}

static void __fastcall m107WritePort(UINT32 port, UINT8 data)
{
	switch (port)
	{
		case 0x00:
			if (rei_pipelined) { // (pipeline: sound side applies on the same line)
				UINT32 w = m107_m2s_w;
				if (w - M107_LOAD(m107_m2s_r) < M107_RING) {
					struct m107_m2s *ev = &m107_m2s_q[w & (M107_RING - 1)];
					ev->line = m107_span_active ? m107_span_line0 + m107_line_of_cycle((INT32)nec_ins_start_cycles()) : m107_main_line;
					ev->cyc33 = VezTotalCycles(); ev->type = 0; ev->data = data;
					M107_STORE(m107_m2s_w, w + 1);
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
			// coin counter
			return;

		case 0x06:
			if (has_bankswitch) {
				m107Bankswitch(data);
			}
			return;

		case 0x80: pf_control[0][0] = data; set_pf_scroll(0); return;
		case 0x81: pf_control[0][1] = data; set_pf_scroll(0); return;
		case 0x82: pf_control[0][2] = data; set_pf_scroll(0); return;
		case 0x83: pf_control[0][3] = data; set_pf_scroll(0); return;
		case 0x84: pf_control[1][0] = data; set_pf_scroll(1); return;
		case 0x85: pf_control[1][1] = data; set_pf_scroll(1); return;
		case 0x86: pf_control[1][2] = data; set_pf_scroll(1); return;
		case 0x87: pf_control[1][3] = data; set_pf_scroll(1); return;
		case 0x88: pf_control[2][0] = data; set_pf_scroll(2); return;
		case 0x89: pf_control[2][1] = data; set_pf_scroll(2); return;
		case 0x8a: pf_control[2][2] = data; set_pf_scroll(2); return;
		case 0x8b: pf_control[2][3] = data; set_pf_scroll(2); return;
		case 0x8c: pf_control[3][0] = data; set_pf_scroll(3); return;
		case 0x8d: pf_control[3][1] = data; set_pf_scroll(3); return;
		case 0x8e: pf_control[3][2] = data; set_pf_scroll(3); return;
		case 0x8f: pf_control[3][3] = data; set_pf_scroll(3); return;
		case 0x90: pf_control[0][4] = data; return;
		case 0x91: pf_control[0][5] = data; set_pf_info(0); return;
		case 0x92: pf_control[1][4] = data; return;
		case 0x93: pf_control[1][5] = data; set_pf_info(1); return;
		case 0x94: pf_control[2][4] = data; return;
		case 0x95: pf_control[2][5] = data; set_pf_info(2); return;
		case 0x96: pf_control[3][4] = data; return;
		case 0x97: pf_control[3][5] = data; set_pf_info(3); return;
		case 0x98: pf_control[0][6] = data; return;
		case 0x99: pf_control[0][7] = data; return;
		case 0x9a: pf_control[1][6] = data; return;
		case 0x9b: pf_control[1][7] = data; return;
		case 0x9c: pf_control[2][6] = data; return;
		case 0x9d: pf_control[2][7] = data; return;
		case 0x9e: pf_control[3][6] = data;
			raster_irq_position = ((pf_control[3][7]<<8) | pf_control[3][6]) - 128;
			if (m107_span_active) m107_span_stop_now(); // (the span was planned with the old raster line)
			return;
		case 0x9f: pf_control[3][7] = data;
			raster_irq_position = ((pf_control[3][7]<<8) | pf_control[3][6]) - 128;
			if (m107_span_active) m107_span_stop_now();
			return;

		case 0xb0:
			memcpy (DrvSprBuf, DrvSprRAM, 0x1000);
			return;

		case 0xb1:
			sprite_enable = ~data & 0x10;
			return;

		case 0xc0:
		case 0xc1:// sound reset
			if (rei_pipelined) {
				UINT32 w = m107_m2s_w;
				if (w - M107_LOAD(m107_m2s_r) < M107_RING) {
					struct m107_m2s *ev = &m107_m2s_q[w & (M107_RING - 1)];
					ev->line = m107_span_active ? m107_span_line0 + m107_line_of_cycle((INT32)nec_ins_start_cycles()) : m107_main_line;
					ev->cyc33 = VezTotalCycles(); ev->type = 1; ev->data = data ? 0 : 1;
					M107_STORE(m107_m2s_w, w + 1);
				}
				return;
			}
			VezClose();
			VezOpen(1);
			sound_cpu_reset = (data) ? 0 : 1;
			if (sound_cpu_reset) {
				VezReset();
			}
			VezClose();
			VezOpen(0);
			return;
	}
}

static UINT8 __fastcall m107SndReadByte(UINT32 address)
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
	}
	return 0;
}

static void __fastcall m107SndWriteByte(UINT32 address, UINT8 data)
{
	if ((address & 0xfffc0) == 0xa8000) {
		iremga20_write( 0, (address & 0x0003f) / 2, data );
		return;
	}

	switch (address)
	{
		case 0xa8040:
			BurnYM2151SelectRegister(data);
			return;

		case 0xa8042:
			BurnYM2151WriteRegister(data);
			return;

		case 0xa8044:
	//		VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
			return;

		case 0xa8046:
			if (rei_pipelined) { // (pipeline: main side applies 32 lines later)
				UINT32 w = m107_s2m_w;
				if (w - M107_LOAD(m107_s2m_r) < M107_RING) {
					struct m107_s2m *ev = &m107_s2m_q[w & (M107_RING - 1)];
					const INT64 L = m107_sound_line_of_ins();
					ev->src_line = L;
					ev->apply = (L / M107_LINES) * m107_total33 + (INT64)VezTotalCycles() * m107_total33 / m107_total35
						+ (INT64)m107_delay * m107_total33 / M107_LINES;
					ev->data = data;
					M107_STORE(m107_s2m_w, w + 1);
				}
				return;
			}
			sound_status[0] = data;
			VezClose();
			VezOpen(0);
			VezSetIRQLineAndVector(0, (irq_vectorbase + 12)/4, CPU_IRQSTATUS_ACK);
			VezClose();
			VezOpen(1);
			return;
	}
}

static INT32 DrvDoReset()
{
	memset (RamStart, 0, RamEnd - RamStart);
#ifdef REI_ME_SPLIT
	memset(m107_me_latch, 0, sizeof(m107_me_latch));
#endif

	VezOpen(0);
	VezReset();
	if (has_bankswitch) m107Bankswitch(0);
	VezClose();

	VezOpen(1);
	VezReset();
	VezClose();

	BurnYM2151Reset();
	iremga20_reset(0);

	for (INT32 i = 0; i < 4; i++)
	{
		set_pf_info(i);
		set_pf_scroll(i);
	}

	sprite_enable = 0;
	raster_irq_position = -1;
	sound_cpu_reset = 0;

	HiscoreReset();

	return 0;
}

static INT32 MemIndex(INT32 gfxlen1, INT32 gfxlen2)
{
	UINT8 *Next; Next = Mem;
	DrvV33ROM 		= Next; Next += 0x100000;
	DrvV30ROM		= Next; Next += 0x020000;
	DrvGfxROM0		= Next; Next += gfxlen1 * 2;
	DrvGfxROM1		= Next; Next += gfxlen2 * 2;

	if (spritesystem == 1) {
		DrvSprTable	= Next; Next += 0x040000;
	}

	DrvSndROM		= Next; Next += 0x100000;

	RamPrioBitmap	= Next; Next += 320 * 240;

	RamStart		= Next;

	DrvSprRAM		= Next; Next += 0x001000;
	DrvSprBuf		= Next; Next += 0x001000;
	DrvVidRAM		= Next; Next += 0x010000;
	DrvV33RAM		= Next; Next += 0x010000;
	DrvV30RAM		= Next; Next += 0x004000;
	DrvPalRAM		= Next; Next += 0x001000;

	sound_status	= Next; Next += 0x000004; // 2
	sound_latch		= Next; Next += 0x000004; // 1
#ifdef REI_ME_SPLIT
	m107_latch_ram = sound_latch;
	sound_latch = m107_me_latch;
#endif

	pf_control[0]	= Next; Next += 0x000008;
	pf_control[1]	= Next; Next += 0x000008;
	pf_control[2]	= Next; Next += 0x000008;
	pf_control[3]	= Next; Next += 0x000008;

	RamEnd			= Next;

	m107_layers[0]	= (struct _m107_layer*)Next; Next += sizeof(struct _m107_layer);
	m107_layers[1]	= (struct _m107_layer*)Next; Next += sizeof(struct _m107_layer);
	m107_layers[2]	= (struct _m107_layer*)Next; Next += sizeof(struct _m107_layer);
	m107_layers[3]	= (struct _m107_layer*)Next; Next += sizeof(struct _m107_layer);

	DrvPalette		= (UINT32 *) Next; Next += 0x0800 * sizeof(UINT32);

	MemEnd			= Next;
	return 0;
}

static void DrvGfxExpand(UINT8 *rom, INT32 len, INT32 type)
{
	INT32 Plane0[4]  = { 8, 0, 24, 16 };
	INT32 XOffs0[8]  = { STEP8(0,1) };
	INT32 YOffs0[8]  = { STEP8(0,32) };
	INT32 Plane1[4]  = { RGN_FRAC(len, 3,4), RGN_FRAC(len, 2,4), RGN_FRAC(len,1,4), RGN_FRAC(len,0,4) };
	INT32 XOffs1[16] = { STEP8(0,1), STEP8(128,1) };
	INT32 YOffs1[16] = { STEP16(0,8) };
	INT32 XOffs2[16] = { STEP8(8,1), STEP8(0,1) };
	INT32 YOffs2[16] = { STEP16(0,16) };

	UINT8 *tmp = (UINT8*)BurnMalloc(len);
	if (tmp == NULL) {
		return;
	}

	memcpy (tmp, rom, len);

	if (type == 0) GfxDecode((len * 2) / 0x040, 4,  8,  8, Plane0, XOffs0, YOffs0, 0x100, tmp, rom);
	if (type == 1) GfxDecode((len * 2) / 0x100, 4, 16, 16, Plane1, XOffs1, YOffs1, 0x100, tmp, rom);
	if (type == 2) GfxDecode((len * 2) / 0x100, 4, 16, 16, Plane1, XOffs2, YOffs2, 0x100, tmp, rom);

	BurnFree(tmp);
}

static void m107_defer_register();

static INT32 DrvInit(INT32 (*pRomLoadCallback)(), const UINT8 *sound_decrypt_table, INT32 gfxdecode, INT32 vectorbase, INT32 gfxlen1, INT32 gfxlen2)
{
	Mem = NULL;
	MemIndex(gfxlen1, gfxlen2);
	INT32 nLen = MemEnd - (UINT8 *)0;
	if ((Mem = (UINT8 *)BurnMalloc(nLen)) == NULL) return 1;
	memset(Mem, 0, nLen);
	MemIndex(gfxlen1, gfxlen2);

	if (pRomLoadCallback)
	{
		if (pRomLoadCallback()) return 1; 

		DrvGfxExpand(DrvGfxROM0, gfxlen1, 0);
		DrvGfxExpand(DrvGfxROM1, gfxlen2, gfxdecode);
	}

	{
		VezInit(0, V33_TYPE);
		VezInit(1, V35_TYPE, 14318180 /*before divider*/);

		VezOpen(0);
		VezMapArea(0x00000, 0x9ffff, 0, DrvV33ROM);
		VezMapArea(0x00000, 0x9ffff, 2, DrvV33ROM);
		VezMapArea(0xa0000, 0xbffff, 0, DrvV33ROM + 0xa0000);
		VezMapArea(0xa0000, 0xbffff, 2, DrvV33ROM + 0xa0000);
		VezMapArea(0xd0000, 0xdffff, 0, DrvVidRAM);
		VezMapArea(0xd0000, 0xdffff, 1, DrvVidRAM);
		VezMapArea(0xd0000, 0xdffff, 2, DrvVidRAM);
		VezMapArea(0xe0000, 0xeffff, 0, DrvV33RAM);
		VezMapArea(0xe0000, 0xeffff, 1, DrvV33RAM);
		VezMapArea(0xe0000, 0xeffff, 2, DrvV33RAM);
		VezMapArea(0xf8000, 0xf8fff, 0, DrvSprRAM);
		VezMapArea(0xf8000, 0xf8fff, 1, DrvSprRAM);
		VezMapArea(0xf9000, 0xf9fff, 0, DrvPalRAM);
#ifndef REI_ORIGINAL_DRAW
		// (ReiREM) Palette RAM writes are also direct-mapped: instead of recomputing colors in a handler per write, only changed entries at
		// frame end (m107_pal_sync) — the palette is only used at draw frame end. The dynarec slow path was hit 800 times per frame (dsoccr94, airass)
		VezMapArea(0xf9000, 0xf9fff, 1, DrvPalRAM);
#endif
		VezMapArea(0xff800, 0xfffff, 0, DrvV33ROM + 0x7f800);
		VezMapArea(0xff800, 0xfffff, 2, DrvV33ROM + 0x7f800);
		VezSetReadHandler(m107ReadByte);
		VezSetWriteHandler(m107WriteByte);
		VezSetReadPort(m107ReadPort);
		VezSetWritePort(m107WritePort);
		VezClose();
		// (ReiREM: ports 0-7 = input/DIP/vblank — change only between frames/lines. Loops waiting on these ports also auto-idle, nec.cpp)
		extern UINT32 rei_nec_pure_ports;
		rei_nec_pure_ports = 0xff;
		extern UINT8 *rei_nec_port_shadow;
		rei_nec_port_shadow = m107_port_shadow;

		VezOpen(1);
		if (sound_decrypt_table != NULL) {
			VezSetDecode((UINT8*)sound_decrypt_table);
		}
		VezMapArea(0x00000, 0x1ffff, 0, DrvV30ROM);
		VezMapArea(0x00000, 0x1ffff, 2, DrvV30ROM);
		VezMapArea(0xa0000, 0xa3fff, 0, DrvV30RAM);
		VezMapArea(0xa0000, 0xa3fff, 1, DrvV30RAM);
		VezMapArea(0xa0000, 0xa3fff, 2, DrvV30RAM);
		VezMapArea(0xff800, 0xfffff, 0, DrvV30ROM + 0x1f800);
		VezMapArea(0xff800, 0xfffff, 2, DrvV30ROM + 0x1f800);
		VezSetReadHandler(m107SndReadByte);
		VezSetWriteHandler(m107SndWriteByte);
		VezClose();
		// (ReiREM: V35 memory is used only by the V35 — keep auto-idle records across runs, v25.cpp)
		extern int rei_v25_auto_keep;
		rei_v25_auto_keep = 1;
	}

	graphics_mask[0] = ((gfxlen1 * 2) - 1) / (8 * 8);
	graphics_mask[1] = ((gfxlen2 * 2) - 1) / (16 * 16);
#ifndef REI_ORIGINAL_DRAW
	m107_rowpens_build(DrvGfxROM0, graphics_mask[0] + 1);
	m107_cov = (UINT8 *)BurnMalloc(320 * 240);
	m107_cov_y0 = 0; // (clear the whole coverage table once at first)
	m107_cov_y1 = 240;
	memset(m107_palshadow, 0, sizeof(m107_palshadow)); // (zero like palette RAM: never-written entries stay as with the handler)
	m107_sprrows_build(DrvGfxROM1, graphics_mask[1] + 1);
#endif

	irq_vectorbase = vectorbase;
	m107_delay = (rei_s2m_delay >= 1 && rei_s2m_delay <= 240) ? rei_s2m_delay : M107_DELAY; // (core setting s2m_delay_lines, default 32)
	// (m107_main_begin rewrites the same value every frame. Set early here: when PSP sound runs on the ME, the ME reads memory, but
	//  a main-written value may sit in main cache — so set it at init, which is flushed before start)
	m107_total33 = (INT32)((INT64)(config_cpu_speed / 60) * nBurnCPUSpeedAdjust / 0x0100);
	m107_total35 = (INT32)((INT64)(7159090 / 60) * nBurnCPUSpeedAdjust / 0x0100);

	BurnYM2151InitBuffered(3579545, 1, NULL, 0);
	YM2151SetIrqHandler(0, &m107YM2151IRQHandler);
	BurnYM2151SetAllRoutes(0.25, BURN_SND_ROUTE_BOTH);
	BurnTimerAttachVez(7159090);

	iremga20_init(0, DrvSndROM, 0x100000, 3579545);
	itemga20_set_route(0, 1.00, BURN_SND_ROUTE_BOTH);

	GenericTilesInit();

	DrvDoReset();

	m107_defer_register();
	m107_m2s_w = m107_m2s_r = m107_s2m_w = m107_s2m_r = 0;
	m107_main_line = m107_sound_line = m107_main_done = m107_sound_done = 0;
	m107_sound_waiting = -1;

	return 0;
}

static INT32 DrvExit()
{
	extern int rei_v25_auto_keep;
	rei_v25_auto_keep = 0;
	extern UINT32 rei_nec_pure_ports;
	rei_nec_pure_ports = 0;
	extern UINT8 *rei_nec_port_shadow;
	rei_nec_port_shadow = NULL;
	GenericTilesExit();
#ifndef REI_ORIGINAL_DRAW
	BurnFree(m107_rowpens);
	BurnFree(m107_cov);
	BurnFree(m107_sprrows);
#endif

	BurnYM2151Exit();
	iremga20_exit();

	VezExit();

	BurnFree(Mem);

	nPrevScreenPos = 0;
	has_bankswitch = 0;

	return 0;
}

// (ReiREM: deferred drawing, ADR-018) The draw functions below read rd_ state: real memory when drawing directly
// (m107_rd_live), the state captured by main (m107_seg) on the render thread. Palette computation stays on main (palette RAM)
static REI_RDR UINT8 *rd_DrvVidRAM, *rd_DrvSprBuf, *rd_pf_control[4];
static REI_RDR struct _m107_layer *rd_m107_layers[4];
static REI_RDR INT32 rd_sprite_enable;
static REI_RDR UINT32 *rd_DrvPalette;
static void m107_rd_live()
{
	rd_DrvVidRAM = DrvVidRAM; rd_DrvSprBuf = DrvSprBuf;
	for (INT32 i = 0; i < 4; i++) { rd_pf_control[i] = pf_control[i]; rd_m107_layers[i] = m107_layers[i]; }
	rd_sprite_enable = sprite_enable; rd_DrvPalette = DrvPalette;
}
#define DrvVidRAM rd_DrvVidRAM
#define DrvSprBuf rd_DrvSprBuf
#define m107_layers rd_m107_layers
#define pf_control rd_pf_control
#define sprite_enable rd_sprite_enable
#define DrvPalette rd_DrvPalette

static void RenderTilePrio(UINT16 *dest, UINT8 *gfx, INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 width, INT32 height, UINT8 *pri, INT32 prio)
{
	if (sx <= (0-width) || sx >= nScreenWidth || sy <= (0-height) || sy >= nScreenHeight) return;

	INT32 flip = 0;
	if (flipy) flip |= (height - 1) * width;
	if (flipx) flip |= width - 1;

#ifndef REI_ORIGINAL_DRAW
	if (m107_sprrows && width == 16 && height == 16) { // (ReiREM) skip empty rows, row position once, interior drawn without bounds tests (same result)
		const UINT32 rows = m107_sprrows[code];
		const UINT8 *g = gfx + code * 256;
		const INT32 inx = (sx >= 0 && sx + 16 <= nScreenWidth);
		for (INT32 y = 0; y < 16; y++) {
			const INT32 yy = sy + y;
			const INT32 srow = flipy ? 15 - y : y;
			if (yy < 0 || yy >= nScreenHeight || ((rows >> srow) & 1) == 0) continue;
			const UINT8 *src = g + srow * 16;
			UINT16 *d = dest + yy * nScreenWidth + sx;
			UINT8 *pp = pri + yy * nScreenWidth + sx;
			if (inx) { // (interior: no bounds tests, flip once per row — same result)
				const INT32 step = flipx ? -1 : 1;
				const UINT8 *s = flipx ? src + 15 : src;
				for (INT32 x = 0; x < 16; x++, s += step) {
					const INT32 pxl = *s;
					if (pxl == 0) continue;
					const INT32 pv = pp[x];
					if ((prio & (1 << pv)) == 0 && pv < 0x80) {
						d[x] = pxl | color;
						pp[x] = pv | 0x80;
					}
				}
				continue;
			}
			for (INT32 x = 0; x < 16; x++) {
				if (!inx && (sx + x < 0 || sx + x >= nScreenWidth)) continue;
				const INT32 pxl = src[flipx ? 15 - x : x];
				if (pxl == 0) continue;
				const INT32 pv = pp[x];
				if ((prio & (1 << pv)) == 0 && pv < 0x80) {
					d[x] = pxl | color;
					pp[x] = pv | 0x80;
				}
			}
		}
		return;
	}
#endif
	gfx += code * width * height;

	for (INT32 y = 0; y < height; y++, sy++) {
		if (sy < 0 || sy >= nScreenHeight) continue;

		for (INT32 x = 0; x < width; x++, sx++) {
			if (sx < 0 || sx >= nScreenWidth) continue;

			INT32 pxl = gfx[((y * width) + x) ^ flip];

			if (pxl == 0) continue;

			if ((prio & (1 << pri[sy * nScreenWidth + sx])) == 0 && pri[sy * nScreenWidth + sx] < 0x80) {
				dest[sy * nScreenWidth + sx] = pxl | color;
				pri[sy * nScreenWidth + sx] |= 0x80;
			}
		}

		sx -= width;
	}
}

// One sprite tile (16x16): CPU uses RenderTilePrio, GE mode uses spr_ge (A/B buffers — rei_m107ge.h)
typedef void (*m107_spr_fn)(INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 prio);
static void spr_cpu(INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 prio)
{
	RenderTilePrio(pTransDraw, DrvGfxROM1, code, color, sx, sy, flipx, flipy, 16, 16, RamPrioBitmap, prio);
}

#ifndef REI_ORIGINAL_DRAW
// GE mode: per pixel, the first covering sprite goes to A (0x1000 flag if hidden); where A is hidden, the first
// non-hidden sprite after it goes to B. m107_cov: 0 none, 1 A hidden, 2 A not hidden, 3 B present too
static void spr_ge(INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 prio)
{
	if (sx <= -16 || sx >= nScreenWidth || sy <= -16 || sy >= nScreenHeight) return;
	const UINT32 rows = m107_sprrows ? m107_sprrows[code] : 0xffff;
	const UINT8 *g = DrvGfxROM1 + code * 256;
	const UINT16 hid = prio ? 0x1000 : 0;
	const UINT8 mark = prio ? 1 : 2;
	{ // (line range: GE composition touches only these lines — over-wide is fine even without pixels)
		const INT32 y0 = sy < 0 ? 0 : sy, y1 = sy + 16 > nScreenHeight ? nScreenHeight : sy + 16;
		const INT32 bk = (color >> 8) & 7;
		if (y0 < m107_spr.ay0[bk]) m107_spr.ay0[bk] = (INT16)y0;
		if (y1 > m107_spr.ay1[bk]) m107_spr.ay1[bk] = (INT16)y1;
		if (prio) {
			if (y0 < m107_spr.hy0) m107_spr.hy0 = (INT16)y0;
			if (y1 > m107_spr.hy1) m107_spr.hy1 = (INT16)y1;
		}
	}
	for (INT32 y = 0; y < 16; y++) {
		const INT32 yy = sy + y;
		const INT32 srow = flipy ? 15 - y : y;
		if (yy < 0 || yy >= nScreenHeight || ((rows >> srow) & 1) == 0) continue;
		const UINT8 *src = g + srow * 16;
		UINT16 *a = m107_spr.a + yy * nScreenWidth;
		UINT8 *cv = m107_cov + yy * nScreenWidth;
		if (sx >= 0 && sx + 16 <= nScreenWidth) { // (interior: no bounds tests, flip once per row — same result)
			const INT32 step = flipx ? -1 : 1;
			const UINT8 *s = flipx ? src + 15 : src;
			UINT16 *ap = a + sx;
			UINT8 *cp = cv + sx;
			for (INT32 x = 0; x < 16; x++, s += step) {
				const INT32 pxl = *s;
				if (pxl == 0) continue;
				const INT32 c = cp[x];
				if (c == 0) {
					ap[x] = 0x800 | pxl | color | hid;
					cp[x] = mark;
					m107_ab |= prio;
				} else if (c == 1 && prio == 0) {
					m107_spr.b[yy * nScreenWidth + sx + x] = 0x800 | pxl | color;
					cp[x] = 3;
					m107_nb++;
					if (yy < m107_spr.by0) m107_spr.by0 = (INT16)yy;
					if (yy >= m107_spr.by1) m107_spr.by1 = (INT16)(yy + 1);
					m107_spr.bbanks |= 1 << ((color >> 8) & 7);
				}
			}
			continue;
		}
		for (INT32 x = 0; x < 16; x++) {
			const INT32 xx = sx + x;
			if (xx < 0 || xx >= nScreenWidth) continue;
			const INT32 pxl = src[flipx ? 15 - x : x];
			if (pxl == 0) continue;
			const INT32 c = cv[xx];
			if (c == 0) {
				a[xx] = 0x800 | pxl | color | hid;
				cv[xx] = mark;
				m107_ab |= prio;
			} else if (c == 1 && prio == 0) {
				m107_spr.b[yy * nScreenWidth + xx] = 0x800 | pxl | color;
				cv[xx] = 3;
				m107_nb++;
				if (yy < m107_spr.by0) m107_spr.by0 = (INT16)yy;
				if (yy >= m107_spr.by1) m107_spr.by1 = (INT16)(yy + 1);
				m107_spr.bbanks |= 1 << ((color >> 8) & 7);
			}
		}
	}
}
#endif

static void draw_sprites(m107_spr_fn rt)
{
	if (sprite_enable == 0) return;

	UINT16 *ram = (UINT16*)DrvSprBuf;
	int offs;
	UINT8 *rom = DrvSprTable;

	for (offs = 0;offs < 0x800;offs += 4)
	{
		int x,y,sprite,colour,fx,fy,y_multi,i,s_ptr,pri_mask;

		pri_mask = (!(BURN_ENDIAN_SWAP_INT16(ram[offs+2])&0x80)) ? 2 : 0;

		y=BURN_ENDIAN_SWAP_INT16(ram[offs+0]) + 8;
		x=BURN_ENDIAN_SWAP_INT16(ram[offs+3]);
		x&=0x1ff;
		y&=0x1ff;

		if (x==0 || y==0) continue; /* offscreen */

		sprite=BURN_ENDIAN_SWAP_INT16(ram[offs+1])&0x7fff;

		x = x - 16;
		y = 384 - 16 - y;

		colour=BURN_ENDIAN_SWAP_INT16(ram[offs+2])&0x7f;
		fx=(BURN_ENDIAN_SWAP_INT16(ram[offs+2])>>8)&0x1;
		fy=(BURN_ENDIAN_SWAP_INT16(ram[offs+2])>>8)&0x2;
		y_multi=(BURN_ENDIAN_SWAP_INT16(ram[offs+0])>>11)&0x3;

		if (spritesystem == 0)
		{
			y_multi=1 << y_multi; /* 1, 2, 4 or 8 */

			s_ptr = 0;
			if (!fy) s_ptr+=y_multi-1;

			for (i=0; i<y_multi; i++)
			{
				rt((sprite + s_ptr) & graphics_mask[1], colour << 4, x - 80, y-i*16, fx, fy, pri_mask);
				rt((sprite + s_ptr) & graphics_mask[1], colour << 4, x - 80, y-i*16 - 0x200, fx, fy, pri_mask);

				if (fy) s_ptr++; else s_ptr--;
			}
		}
		else
		{
			int rom_offs = sprite*8;

			if (rom[rom_offs+1] || rom[rom_offs+3] || rom[rom_offs+5] || rom[rom_offs+7])
			{
				while (rom_offs < 0x40000)  /* safety check */
				{
					int xdisp = rom[rom_offs+6]+256*rom[rom_offs+7];
					int ydisp = rom[rom_offs+2]+256*rom[rom_offs+3];
					int ffx=fx^(rom[rom_offs+1]&1);
					int ffy=fy^(rom[rom_offs+1]&2);
					sprite=rom[rom_offs+4]+256*rom[rom_offs+5];
					y_multi=1<<((rom[rom_offs+3]>>1)&0x3);
					if (fx) xdisp = -xdisp-16;
					if (fy) ydisp = -ydisp - (16*y_multi-1);
					if (!ffy) sprite+=y_multi-1;
					for (i=0; i<y_multi; i++)
					{
						rt((sprite+(ffy?i:-i)) & graphics_mask[1], colour << 4, ((x+xdisp)&0x1ff)-80,(y-ydisp-16*i)&0x1ff, ffx, ffy, pri_mask);
						rt((sprite+(ffy?i:-i)) & graphics_mask[1], colour << 4, ((x+xdisp)&0x1ff)-80,((y-ydisp-16*i)&0x1ff)-0x200, ffx, ffy, pri_mask);
					}

					if (rom[rom_offs+1]&0x80) break;    /* end of block */

					rom_offs += 8;
				}
			}
		}
	}
}

static void draw_layer_byline(INT32 start, INT32 finish, INT32 layer, INT32 forcelayer)
{
	struct _m107_layer *ptr = m107_layers[layer];

	if (ptr->enable == 0) return;

	INT32 wide = 64;
	INT32 scrolly = (ptr->scrolly + 136) & 0x1ff;
	INT32 scrollx = ((ptr->scrollx) + 80) + (3 - 2 * layer);

	INT32 transparency = (forcelayer & 2) ? ~0 : 0;
	INT32 priority = forcelayer & 1;

	UINT16 *xscroll = (UINT16*)(DrvVidRAM + (0xe000 + 0x200 * layer));
	UINT16 *yscroll = (UINT16*)(DrvVidRAM + (0xe800 + 0x200 * layer));

#ifndef REI_ORIGINAL_DRAW
	// (ReiREM) Screen lines with the same tile row and x scroll share tile decoding (address, attr, code, color, flip, row pens) — done once
	// and reused by the following lines (once per 8 lines without row scroll). Drawing is the same as the original path below
	if (m107_rowpens && nScreenWidth + 8 <= 8 * 64) {
		struct col { const UINT8 *rom; UINT32 pens, color; INT16 xx; UINT8 x_xor, flipy; };
		static col cols[64] REI_RDR;
		static INT32 ncols REI_RDR;
		INT32 key_row = -1, key_x = -1;
		for (INT32 sy = start; sy < finish; sy++)
		{
			UINT16 *dest = pTransDraw + (sy * nScreenWidth);
			UINT8  *pri  = RamPrioBitmap + (sy * nScreenWidth);

			INT32 scrolly_1 = (scrolly + sy) & 0x1ff;
			if (ptr->enable_rowscroll & 2) scrolly_1 = (scrolly_1 + BURN_ENDIAN_SWAP_INT16(yscroll[sy+8])) & 0x1ff;

			INT32 scrollx_1 = scrollx;
			if (ptr->enable_rowscroll & 1) scrollx_1 = (scrollx_1 + BURN_ENDIAN_SWAP_INT16(xscroll[((scrolly_1 + 136 + 0xff80)-(scrolly)) & 0x1ff])) & 0x1ff;

			const INT32 romoff_1 = (scrolly_1 & 0x07) << 3;

			if ((scrolly_1 >> 3) != key_row || scrollx_1 != key_x) {
				key_row = scrolly_1 >> 3;
				key_x = scrollx_1;
				ncols = 0;
				for (INT32 sx = 0; sx < nScreenWidth + 8; sx+=8)
				{
					INT32 sxx = (scrollx_1 + sx) & 0x1ff;
					INT32 offs  = ((scrolly_1 / 8) * wide) + (((sxx) / 8) & (wide - 1));
					INT32 attr  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 1]);
					INT32 group = (attr >> 9) & 1;
					if (priority != group) continue;
					INT32 code  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 0]) | ((attr & 0x1000) << 4);
					if (code == 0 && transparency == 0) continue; // skip blank tiles
					col *c = &cols[ncols++];
					c->color = (attr & 0x007f) << 4;
					c->flipy = (attr & 0x0800) ? 0x38 : 0;
					c->x_xor = (attr & 0x0400) ? 7 : 0;
					c->rom = DrvGfxROM0 + ((code & graphics_mask[0]) * 0x40);
					c->pens = m107_rowpens[code & graphics_mask[0]];
					c->xx = (INT16)(sx - (scrollx_1&0x7));
				}
			}

			for (INT32 i = 0; i < ncols; i++)
			{
				const col *c = &cols[i];
				const INT32 romoff = romoff_1 ^ c->flipy;
				const UINT32 f = c->pens >> (romoff >> 3);
				if (transparency == 0 && (f & 1) == 0) continue;
				const UINT8 *rom = c->rom + romoff;
				const INT32 x_xor = c->x_xor;
				const UINT32 color = c->color;
				INT32 xx = c->xx;
				if (xx >= 0 && xx + 8 <= nScreenWidth) {
					UINT16 *d = dest + xx;
					UINT8 *pp = pri + xx;
					if (transparency != 0 || (f & 0x100)) {
						if (x_xor) {
							d[0] = rom[7] | color; d[1] = rom[6] | color; d[2] = rom[5] | color; d[3] = rom[4] | color;
							d[4] = rom[3] | color; d[5] = rom[2] | color; d[6] = rom[1] | color; d[7] = rom[0] | color;
						} else {
							d[0] = rom[0] | color; d[1] = rom[1] | color; d[2] = rom[2] | color; d[3] = rom[3] | color;
							d[4] = rom[4] | color; d[5] = rom[5] | color; d[6] = rom[6] | color; d[7] = rom[7] | color;
						}
						memset(pp, priority, 8);
					} else {
						for (INT32 x = 0; x < 8; x++) {
							const INT32 pxl = rom[x ^ x_xor];
							if (pxl == 0) continue;
							d[x] = pxl | color;
							pp[x] = priority;
						}
					}
					continue;
				}
				for (INT32 x = 0; x < 8; x++, xx++) {
					if (xx < 0 || xx >= nScreenWidth) continue;

					INT32 pxl = rom[x ^ x_xor];
					if (pxl == transparency) continue;

					dest[xx] = pxl | color;
					pri[xx] = priority;
				}
			}
		}
		return;
	}
#endif

	for (INT32 sy = start; sy < finish; sy++)
	{
		UINT16 *dest = pTransDraw + (sy * nScreenWidth);
		UINT8  *pri  = RamPrioBitmap + (sy * nScreenWidth);

		INT32 scrolly_1 = (scrolly + sy) & 0x1ff;
		if (ptr->enable_rowscroll & 2) scrolly_1 = (scrolly_1 + BURN_ENDIAN_SWAP_INT16(yscroll[sy+8])) & 0x1ff;

		INT32 scrollx_1 = scrollx; 
		if (ptr->enable_rowscroll & 1) scrollx_1 = (scrollx_1 + BURN_ENDIAN_SWAP_INT16(xscroll[((scrolly_1 + 136 + 0xff80)-(scrolly)) & 0x1ff])) & 0x1ff;

		INT32 romoff_1 = (scrolly_1 & 0x07) << 3;

		for (INT32 sx = 0; sx < nScreenWidth + 8; sx+=8)
		{
			INT32 sxx = (scrollx_1 + sx) & 0x1ff;

			INT32 offs  = ((scrolly_1 / 8) * wide) + (((sxx) / 8) & (wide - 1));

			INT32 attr  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 1]);

			INT32 group = (attr >> 9) & 1;

			if (priority == group)
			{
				INT32 code  = BURN_ENDIAN_SWAP_INT16(ptr->vram[(offs * 2) + 0]) | ((attr & 0x1000) << 4);

				if (code == 0 && transparency == 0) continue; // skip blank tiles

				INT32 color = (attr & 0x007f) << 4;

				INT32 x_xor = 0;
				INT32 romoff = romoff_1;
				if (attr & 0x0800) romoff ^= 0x38;	// flipy
				if (attr & 0x0400) x_xor = 7;		// flipx

				UINT8 *rom = DrvGfxROM0 + ((code & graphics_mask[0]) * 0x40) + romoff;

				INT32 xx = sx - (scrollx_1&0x7);

#ifndef REI_ORIGINAL_DRAW
				if (m107_rowpens) { // (ReiREM) skip empty rows, interior tiles without bounds tests (full rows/opaque pass without zero tests either)
					const UINT32 f = m107_rowpens[code & graphics_mask[0]] >> (romoff >> 3);
					if (transparency == 0 && (f & 1) == 0) continue;
					if (xx >= 0 && xx + 8 <= nScreenWidth) {
						UINT16 *d = dest + xx;
						UINT8 *pp = pri + xx;
						if (transparency != 0 || (f & 0x100)) {
							for (INT32 x = 0; x < 8; x++) { d[x] = rom[x ^ x_xor] | color; pp[x] = group; }
						} else {
							for (INT32 x = 0; x < 8; x++) {
								const INT32 pxl = rom[x ^ x_xor];
								if (pxl == 0) continue;
								d[x] = pxl | color;
								pp[x] = group;
							}
						}
						continue;
					}
				}
#endif
				for (INT32 x = 0; x < 8; x++, xx++) {
					if (xx < 0 || xx >= nScreenWidth) continue;

					INT32 pxl = rom[x ^ x_xor];
					if (pxl == transparency) continue;

					dest[xx] = pxl | color;
					pri[xx] = group;
				}
			}
		}
	}
}

static void DrawLayers(INT32 start, INT32 finish)
{
#ifndef REI_ORIGINAL_DRAW
	if (m107_cur) { // (ReiREM GE mode: hand layer state to the frontend instead of pixels — rei_m107ge.h)
		rei_m107_seg s;
		s.start = start;
		s.finish = finish;
		s.l3_on = (~pf_control[3][4] & 0x80) != 0;
		s.layer_mask = nBurnLayer;
		s.sprite_mask = nSpriteEnable;
		for (INT32 i = 0; i < 4; i++) {
			const struct _m107_layer *p = m107_layers[i];
			s.l[i].enable = p->enable;
			s.l[i].rowscroll = p->enable_rowscroll;
			s.l[i].scrollx = p->scrollx;
			s.l[i].scrolly = p->scrolly;
			s.l[i].vram = p->vram;
			s.l[i].xscroll = (const UINT16 *)(DrvVidRAM + 0xe000 + 0x200 * i);
			s.l[i].yscroll = (const UINT16 *)(DrvVidRAM + 0xe800 + 0x200 * i);
		}
		m107_cur->segment(&s);
		if (!REI_SH(m107_ge_check))
			return;
	}
#endif
	memset (RamPrioBitmap + (start * nScreenWidth), 0, nScreenWidth * (finish - start)); // clear priority

	if (~pf_control[3][4] & 0x80) {

		if (~nBurnLayer & 8) memset (pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));

		if (nBurnLayer & 8) draw_layer_byline(start, finish, 3, 0|2);
		if (nBurnLayer & 8) draw_layer_byline(start, finish, 3, 1|2);

		if (nBurnLayer & 4) draw_layer_byline(start, finish, 2, 0);
	} else {
		if (~nBurnLayer & 4) memset (pTransDraw + (start * nScreenWidth), 0, nScreenWidth * (finish - start) * sizeof(INT16));

		if (nBurnLayer & 4) draw_layer_byline(start, finish, 2, 0 | 2);
	}

	if (nBurnLayer & 2) draw_layer_byline(start, finish, 1, 0);
	if (nBurnLayer & 1) draw_layer_byline(start, finish, 0, 0);
	if (nSpriteEnable & 4) draw_layer_byline(start, finish, 2, 1);
	if (nSpriteEnable & 2) draw_layer_byline(start, finish, 1, 1);
	if (nSpriteEnable & 1) draw_layer_byline(start, finish, 0, 1);
}

// Render thread: frame end (sprites and transfer from the original DrvDraw)
static void m107_frame_end()
{
#ifndef REI_ORIGINAL_DRAW
	if (m107_cur) { // (GE mode: sprites go to A/B buffers, priority against layers is done by the GE)
		// (clear only lines where this A buffer/coverage table had sprites last time — the rest are already 0)
		if (m107_spr.clr_y1 > m107_spr.clr_y0)
			memset(m107_spr.a + m107_spr.clr_y0 * nScreenWidth, 0, (m107_spr.clr_y1 - m107_spr.clr_y0) * nScreenWidth * sizeof(UINT16));
		if (m107_cov_y1 > m107_cov_y0)
			memset(m107_cov + m107_cov_y0 * nScreenWidth, 0, (m107_cov_y1 - m107_cov_y0) * nScreenWidth);
		m107_nb = 0;
		m107_ab = 0;
		for (INT32 i = 0; i < 8; i++) { m107_spr.ay0[i] = (INT16)nScreenHeight; m107_spr.ay1[i] = 0; }
		m107_spr.hy0 = m107_spr.by0 = (INT16)nScreenHeight;
		m107_spr.hy1 = m107_spr.by1 = 0;
		m107_spr.bbanks = 0;
		if (nBurnLayer & 8) draw_sprites(spr_ge);
		if (REI_SH(m107_ge_check) && (nBurnLayer & 8)) draw_sprites(spr_cpu);
		m107_cov_y0 = (INT16)nScreenHeight;
		m107_cov_y1 = 0;
		for (INT32 i = 0; i < 8; i++) {
			if (m107_spr.ay0[i] < m107_cov_y0) m107_cov_y0 = m107_spr.ay0[i];
			if (m107_spr.ay1[i] > m107_cov_y1) m107_cov_y1 = m107_spr.ay1[i];
		}
		BurnTransferCopy(DrvPalette);
		m107_cur->frame_end(&m107_spr, m107_nb, m107_ab != 0);
		m107_cur = NULL;
		return;
	}
#endif
	if (nBurnLayer & 8) draw_sprites(spr_cpu);
	BurnTransferCopy(DrvPalette);
}

#ifndef REI_ORIGINAL_DRAW
static void m107_frame_begin()
{
	m107_cur = REI_SH(m107_ge_ops);
	if (m107_cur && m107_cov)
		m107_cur->frame_begin(&m107_spr);
	else
		m107_cur = NULL;
}
#endif

#undef DrvVidRAM
#undef DrvSprBuf
#undef m107_layers
#undef pf_control
#undef sprite_enable
#undef DrvPalette

struct m107_seg {
	UINT8 vidram[0x10000], sprbuf[0x1000], pf_control[4][8];
	struct _m107_layer layers[4];
	UINT32 palette[0x800];
	INT32 sprite_enable;
};
static void m107_capture(void *p)
{
	struct m107_seg *g = (struct m107_seg *)p;
	rei_big_copy(g->vidram, DrvVidRAM, sizeof(g->vidram)); // (struct start: 64-byte boundary — PSP uses DMA)
	// (sprite buffer, palette, sprite_enable are read only at frame end (m107_frame_end) — captured by m107_capture_end)
	for (INT32 i = 0; i < 4; i++) {
		memcpy(g->pf_control[i], pf_control[i], 8);
		g->layers[i] = *m107_layers[i];
		// (layer VRAM/scroll pointers are inside VRAM: same offset within the captured copy)
		g->layers[i].vram = (UINT16 *)(g->vidram + ((UINT8 *)m107_layers[i]->vram - DrvVidRAM));
		if (m107_layers[i]->scroll)
			g->layers[i].scroll = (UINT16 *)(g->vidram + ((UINT8 *)m107_layers[i]->scroll - DrvVidRAM));
	}
}
// Frame-end state: only what m107_frame_end (sprites, transfer) reads (64KB VRAM goes with draw segments only — ~1ms per PSP main frame)
static void m107_capture_end(void *p)
{
	struct m107_seg *g = (struct m107_seg *)p;
	memcpy(g->sprbuf, DrvSprBuf, sizeof(g->sprbuf));
	memcpy(g->palette, DrvPalette, sizeof(g->palette));
	g->sprite_enable = sprite_enable;
}
static void m107_apply(const void *p)
{
	struct m107_seg *g = (struct m107_seg *)p;
	rd_DrvVidRAM = g->vidram; rd_DrvSprBuf = g->sprbuf;
	for (INT32 i = 0; i < 4; i++) { rd_pf_control[i] = g->pf_control[i]; rd_m107_layers[i] = &g->layers[i]; }
	rd_sprite_enable = g->sprite_enable; rd_DrvPalette = g->palette;
}
static void m107_defer_register()
{
	static const rei_defer_ops ops = { sizeof(struct m107_seg), m107_capture, m107_apply, DrawLayers, m107_frame_begin, m107_frame_end, NULL, m107_capture_end };
	rei_defer_register(&ops);
}
// Main: draw a raster segment
static void m107_drawlayers(INT32 start, INT32 finish)
{
#ifndef REI_ORIGINAL_DRAW
	if (rei_defer_on) {
		rei_defer_segment(start, finish);
		return;
	}
#endif
	m107_rd_live();
	DrawLayers(start, finish);
}

#ifndef REI_ORIGINAL_DRAW
// (ReiREM) Convert only changed entries of direct-mapped palette RAM to colors (frame-end values match the per-write handler)
static void m107_pal_sync()
{
	const UINT16 *p = (const UINT16 *)DrvPalRAM;
	for (INT32 i = 0; i < 0x800; i++)
		if (p[i] != m107_palshadow[i]) {
			m107_palshadow[i] = p[i];
			DrvPalette[i] = CalcCol(i << 1);
		}
}
#endif

static INT32 DrvDraw()
{
#ifndef REI_ORIGINAL_DRAW
	m107_pal_sync();
#endif
	if (bRecalcPalette) {
		for (INT32 i=0; i<0x800;i++)
			DrvPalette[i] = CalcCol(i<<1);
		bRecalcPalette = 0;
	}
//	DrawLayers(0, nScreenHeight);

#ifndef REI_ORIGINAL_DRAW
	if (rei_defer_on) {
		rei_defer_frame();
		return 0;
	}
#endif
	m107_rd_live();
	m107_frame_end();

	return 0;
}

static INT32 DrvReDraw()
{
#ifndef REI_ORIGINAL_DRAW
	m107_pal_sync();
#endif
	if (bRecalcPalette) {
		for (INT32 i=0; i<0x800;i++)
			DrvPalette[i] = CalcCol(i<<1);
		bRecalcPalette = 0;
	}

	BurnTransferClear();

	m107_rd_live();
	DrawLayers(0, nScreenHeight);

	m107_frame_end();

	return 0;
}

REI_LINE_FN(28) static void scanline_interrupts(INT32 scanline)
{
	if (scanline == raster_irq_position) {

		if (scanline>=8 && scanline < 248 && nPrevScreenPos != (scanline-8)+1) {
			if (nPrevScreenPos >= 0 && nPrevScreenPos <= 239)
				m107_drawlayers(nPrevScreenPos, (scanline-8)+1);
			nPrevScreenPos = (scanline-8)+1;
		}

		VezSetIRQLineAndVector(0, (irq_vectorbase + 8)/4, CPU_IRQSTATUS_ACK);
		VezRun(10);
		VezSetIRQLineAndVector(0, (irq_vectorbase + 8)/4, CPU_IRQSTATUS_NONE);

	}
	else if (scanline == 248) // vblank
	{
		vblank = 0;
		m107_port_shadow_update();

		if (nPrevScreenPos != 240) {
			m107_drawlayers(nPrevScreenPos, 240);
		}
		nPrevScreenPos = 0;

		if (rei_defer_on || pBurnDraw) {
			DrvDraw();
	//		DrvReDraw();
		}

		VezSetIRQLineAndVector(0, (irq_vectorbase + 0)/4, CPU_IRQSTATUS_ACK);
		VezRun(10);
		VezSetIRQLineAndVector(0, (irq_vectorbase + 0)/4, CPU_IRQSTATUS_NONE);
	} else if (scanline == 8) {
		vblank = 0x80;
		m107_port_shadow_update();
	}
}

static void m107_compile_inputs()
{
	memset (DrvInput, 0xff, 5);

	for (INT32 i = 0; i < 8; i++) {
		DrvInput[0] ^= (DrvJoy1[i] & 1) << i;
		DrvInput[1] ^= (DrvJoy2[i] & 1) << i;
		DrvInput[2] ^= (DrvJoy3[i] & 1) << i;
		DrvInput[3] ^= (DrvJoy4[i] & 1) << i;
		DrvInput[4] ^= (DrvButton[i] & 1) << i;
	}
}

static INT32 m107PipelinedFrame();

static INT32 DrvFrame()
{
	if (rei_pipelined) return m107PipelinedFrame();

	if (DrvReset) {
		DrvDoReset();
	}

	VezNewFrame();

	m107_compile_inputs();

	INT32 nInterleave = 256 * 8; // * 8 for tight sync
	INT32 nCyclesTotal[2] = { 0, 0 };
	INT32 nCyclesDone[2] = { 0, 0 };

	nCyclesTotal[0] = (INT32)((INT64)(config_cpu_speed / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesTotal[1] = (INT32)((INT64)(7159090 / 60) * nBurnCPUSpeedAdjust / 0x0100);

	vblank = 0;
	m107_port_shadow_update();

	for (INT32 i = 0; i < nInterleave; i++)
	{
		VezOpen(0);
		CPU_RUN(0, Vez);
		if ((i&7)==7) scanline_interrupts(i/8); // update at hblank?
		VezClose();

		VezOpen(1);
		if (sound_cpu_reset) {
			VezIdle(nCyclesTotal[1] / nInterleave);
		} else {
			CPU_RUN_TIMER(1);
		}
		VezClose();
	}

	if (pBurnSoundOut) {
		BurnYM2151Render(pBurnSoundOut, nBurnSoundLen);
		iremga20_update(0, pBurnSoundOut, nBurnSoundLen);
	}

	return 0;
}

// ---------------------------------------------------------------------------
// (ReiREM) M107 pipeline (see m107_m2s/s2m above). Line = scanline of the original DrvFrame (256, frame cycles split into 256)

static inline INT32 m107_target33(INT32 l) { return (INT32)((INT64)(l + 1) * m107_total33 / M107_LINES); }
// Line an instruction starting at frame cycle c belongs to when run per line: first l with c < target33(l)
static INT32 m107_line_of_cycle(INT32 c)
{
	INT32 l = (INT32)((INT64)c * M107_LINES / m107_total33);
	if (l < 0) l = 0;
	if (l > M107_LINES - 1) l = M107_LINES - 1;
	while (l > 0 && c < m107_target33(l - 1)) l--;
	while (l < M107_LINES - 1 && c >= m107_target33(l)) l++;
	return l;
}
static inline INT32 m107_target35(INT32 l) { return (INT32)((INT64)(l + 1) * m107_total35 / M107_LINES); }

static void m107_wait(void)
{
	if (rei_wait_hook) rei_wait_hook();
}

// Main: frame start (input, reset, cycles)
static void m107_main_begin()
{
	if (DrvReset) { // (also resets the sound side: frontend resets normally go through rei_m107_reset with threads stopped)
		DrvDoReset();
	}
	m107_compile_inputs();
	m107_total33 = (INT32)((INT64)(config_cpu_speed / 60) * nBurnCPUSpeedAdjust / 0x0100);
	m107_total35 = (INT32)((INT64)(7159090 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nec_new_frame();
	vblank = 0;
	m107_port_shadow_update();
}

#if defined(__psp__)
// (PSP diag) Single-thread pipeline in-frame clock totals: 0 main lines, 1 sound lines, 2 audio finish. Clock supplied by the frontend
// (µs, or event count in hardware counter builds). Not measured if absent
extern "C" { unsigned rei_m107_us[4]; unsigned (*rei_m107_clock)(void); } // (3: VezRun within main lines)
#define M107_T(v) const unsigned v = rei_m107_clock ? rei_m107_clock() : 0
#define M107_ACC(i, t0) do { if (rei_m107_clock) rei_m107_us[i] += rei_m107_clock() - (t0); } while (0)
#else
#define M107_T(v) const unsigned v = 0; (void)v
#define M107_ACC(i, t0) do { } while (0)
#endif
// One main line: check sound finished the line 32 back, apply arrived status events at their cycle, run V33 to line end
#ifdef REI_YM_SPLIT // (PSP: sound side on ME with YM2151 synthesized by main — drain the ring every line, burn_ym2151.cpp)
extern "C" INT32 rei_ym_split_on;
extern "C" int rei_ym_main_poll(void);
#endif
REI_LINE_FN(26) static void m107_run_main_line(INT32 l)
{
#ifdef REI_YM_SPLIT
	if (rei_ym_split_on) rei_ym_main_poll();
#endif
	const INT64 L = m107_main_line;
	if (rei_threaded && M107_LOAD(m107_sound_done) < L - m107_delay + 1) { // (threaded-mode diag: time main waited for sound → rei_m107_us[1])
		M107_T(tw);
		while (M107_LOAD(m107_sound_done) < L - m107_delay + 1 && !REI_SH(rei_stop))
			m107_wait();
		M107_ACC(1, tw);
	}
	const INT64 frame_base = (L / M107_LINES) * m107_total33;
	const INT32 target = m107_target33(l);
	VezOpen(0);
	for (;;) {
		UINT32 r = m107_s2m_r;
		if (r == M107_LOAD(m107_s2m_w)) break;
		struct m107_s2m *ev = &m107_s2m_q[r & (M107_RING - 1)];
		if (ev->src_line > L - m107_delay || ev->apply > frame_base + target) break;
		INT32 left = (INT32)(ev->apply - frame_base) - VezTotalCycles();
		if (left > 0) { M107_T(tv); VezRun(left); M107_ACC(3, tv); }
		sound_status[0] = ev->data;
		VezSetIRQLineAndVector(0, (irq_vectorbase + 12)/4, CPU_IRQSTATUS_ACK);
		M107_STORE(m107_s2m_r, r + 1);
	}
	INT32 left = target - VezTotalCycles();
	if (left > 0) { M107_T(tv); VezRun(left); M107_ACC(3, tv); }
	scanline_interrupts(l);
	VezClose();
	m107_main_line = L + 1;
	M107_STORE(m107_main_done, L + 1);
}

// Main lines [l, e) as one V33 slice (single thread only, same result as m107_run_main_line). Returns the next line
static inline INT32 m107_line_alone(INT32 l) { return l == raster_irq_position || l == 248 || l == 8; }
REI_LINE_FN(27) static INT32 m107_run_main_span(INT32 l, INT32 e)
{
	const INT64 L = m107_main_line;            // absolute line of l
	const INT64 line0 = L - l;
	const INT64 frame_base = (L / M107_LINES) * m107_total33;
	VezOpen(0);
	m107_span_line0 = line0;
	m107_span_stop = 0;
	m107_span_active = 1;
	INT32 last = e - 1;
	for (;;) {
		const INT32 tgt = m107_target33(last);
		UINT32 r = m107_s2m_r;
		if (r != M107_LOAD(m107_s2m_w)) {
			struct m107_s2m *ev = &m107_s2m_q[r & (M107_RING - 1)];
			if (ev->src_line <= line0 + last - m107_delay) {
				// Run per line, this event would not apply before the start of the first line it is visible on (src_line + delay)
				INT64 eff = ev->apply;
				const INT64 vis = ev->src_line + m107_delay - line0;
				if (vis > 0) {
					const INT64 start = frame_base + m107_target33((INT32)vis - 1);
					if (eff < start) eff = start;
				}
				if (eff <= frame_base + tgt) {
					INT32 left = (INT32)(eff - frame_base) - VezTotalCycles();
					if (left > 0) VezRun(left);
					if (m107_span_stop) { // (ended before the event: end the span on that line)
						m107_span_stop = 0;
						last = m107_span_stop_line;
						continue;
					}
					sound_status[0] = ev->data;
					VezSetIRQLineAndVector(0, (irq_vectorbase + 12)/4, CPU_IRQSTATUS_ACK);
					M107_STORE(m107_s2m_r, r + 1);
					continue;
				}
			}
		}
		INT32 left = tgt - VezTotalCycles();
		if (left > 0) VezRun(left);
		if (m107_span_stop) {
			m107_span_stop = 0;
			last = m107_span_stop_line; // (finish that line normally — more events may follow)
			continue;
		}
		break;
	}
	m107_span_active = 0;
	scanline_interrupts(last);
	VezClose();
	m107_main_line = line0 + last + 1;
	M107_STORE(m107_main_done, m107_main_line);
	return last + 1;
}

// (Sound line merging, single thread) After applying line l's events, update end-of-line timers for l..l1 at once via BurnTimerUpdateSlices —
// same result as stopping every line when the lines between have no events (see timer.cpp). Status events the V35 raises meanwhile get
// src_line = the line the instruction originally ran on (first line whose end target > instruction start cycle S)
unsigned int v25_ins_start_cycles();
static INT32 m107_mg_on REI_ME_DATA, m107_mg_cur REI_ME_DATA, m107_mg_l0 REI_ME_DATA, m107_mg_l1 REI_ME_DATA;
static INT64 m107_mg_L0 REI_ME_DATA;
static INT64 m107_sound_line_of_ins()
{
	if (!m107_mg_on) return m107_sound_line;
	const INT32 S = (INT32)v25_ins_start_cycles();
	while (m107_mg_cur < m107_mg_l1 && S >= m107_target35(m107_mg_cur)) m107_mg_cur++;
	return m107_mg_L0 + (m107_mg_cur - m107_mg_l0);
}
// First event line after line Lx among unapplied latches/resets (huge value if none)
static INT64 m107_m2s_next_after(INT64 Lx)
{
	for (UINT32 r = m107_m2s_r; r != M107_LOAD(m107_m2s_w); r++) {
		const struct m107_m2s *ev = &m107_m2s_q[r & (M107_RING - 1)];
		if (ev->line > Lx) return ev->line;
	}
	return (INT64)1 << 62;
}

// One sound line: check main finished this line, apply latches/resets up to it, run V35 to line end (driven by YM2151 timers).
// If l1 > l, also lines l+1..l1 (those must have no events, single thread only)
// (PSP, sound on ME) hook called per sound line: the ME catches up on pending drawing within the sound delay (set by the frontend)
extern "C" { void (*rei_sound_line_hook)(void) = NULL; }
// (PSP mode 3) how many lines sound lags main (main's finished line - sound's next line): the ME decides whether to defer drawing
extern "C" INT32 rei_m107_sound_lag(void) { return (INT32)(M107_LOAD(m107_main_done) - m107_sound_line); }
static void m107_run_sound_line(INT32 l, INT32 l1 = -1)
{
	if (l1 < l) l1 = l;
	if (rei_threaded && rei_sound_line_hook) rei_sound_line_hook();
	const INT64 L = m107_sound_line;
	if (rei_threaded && M107_LOAD(m107_main_done) < L + 1) {
		const UINT32 ep = M107_LOAD(m107_epoch);
		M107_STORE(m107_sound_waiting, L);
		while (M107_LOAD(m107_main_done) < L + 1 && !REI_SH(rei_stop) && M107_LOAD(m107_epoch) == ep)
			m107_wait();
		M107_STORE(m107_sound_waiting, (INT64)-1);
		if (M107_LOAD(m107_epoch) != ep) // (main restarted the pipeline while waiting: do not run this line)
			return;
	}
	if (l == 0) v25_new_frame();
	REI_PROF_BEGIN(sline); // (profile: whole sound line — V35 run, timers, events, in the "v35" slot)
	VezOpen(1);
	for (;;) {
		UINT32 r = m107_m2s_r;
		if (r == M107_LOAD(m107_m2s_w)) break;
		struct m107_m2s *ev = &m107_m2s_q[r & (M107_RING - 1)];
		if (ev->line > L) break;
		INT32 t35 = (INT32)((INT64)ev->cyc33 * m107_total35 / m107_total33);
		if (t35 > m107_total35) t35 = m107_total35;
		if (sound_cpu_reset) {
			INT32 idle = t35 - VezTotalCycles();
			if (idle > 0) VezIdle(idle);
		} else {
			BurnTimerUpdate(t35);
		}
		if (ev->type == 0) {
			sound_latch[0] = ev->data;
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_ACK);
			VezRun(10);
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP1, 0xff/*default*/, CPU_IRQSTATUS_NONE);
			VezRun(10);
		} else {
			sound_cpu_reset = ev->data;
			if (sound_cpu_reset) VezReset();
		}
		M107_STORE(m107_m2s_r, r + 1);
	}
	if (sound_cpu_reset) {
		INT32 idle = m107_target35(l1) - VezTotalCycles(); // (lines between are VezIdle only — same in one go)
		if (idle > 0) VezIdle(idle);
	} else if (l1 == M107_LINES - 1) {
		BurnTimerEndFrame(m107_total35);
	} else if (l1 > l) {
		m107_mg_on = 1;
		m107_mg_cur = m107_mg_l0 = l;
		m107_mg_l1 = l1;
		m107_mg_L0 = L;
		BurnTimerUpdateSlices(l, l1, M107_LINES, m107_total35);
		m107_mg_on = 0;
	} else {
		BurnTimerUpdate(m107_target35(l));
	}
	VezClose();
	REI_PROF_END(sline, REI_PROF_V35);
	m107_sound_line = L + 1 + (l1 - l);
	M107_STORE(m107_sound_done, L + 1 + (l1 - l));
}

static void m107_sound_end(INT16 *out, INT32 len)
{
	if (out) {
		BurnYM2151Render(out, len);
		iremga20_update(0, out, len);
	}
}

// Single thread: alternate 32 main lines and the same 32 sound lines (same result as threaded mode)
REI_LINE_FN(29) static INT32 m107PipelinedFrame()
{
#if !defined(__psp__)
	{ // (harness check: REI_M107_NOSPAN runs per line — hashes must match)
		static int chk;
		if (!chk) { chk = 1; if (getenv("REI_M107_NOSPAN")) rei_m107_span_enabled = 0; }
	}
#endif
	m107_main_begin();
	for (INT32 base = 0; base < M107_LINES; base += m107_delay) {
		const INT32 bend = base + m107_delay < M107_LINES ? base + m107_delay : M107_LINES; // (clip at frame end if the delay does not divide 256)
		M107_T(t0);
		for (INT32 l = base; l < bend; ) {
			INT32 e = l;
#ifdef REI_YM_SPLIT
			if (!rei_ym_split_on)
#endif
			if (rei_m107_span_enabled)
				while (e < bend && !m107_line_alone(e)) e++;
			if (e - l >= 2) {
				l = m107_run_main_span(l, e);
			} else {
				m107_run_main_line(l);
				l++;
			}
		}
		M107_ACC(0, t0);
		M107_T(t1);
		for (INT32 l = base; l < bend; ) {
			INT32 l1 = l;
#ifndef REI_NO_M107_SNDMERGE
			// merge following event-free lines (within this batch, excluding the frame's last line)
			const INT64 nx = m107_m2s_next_after(m107_sound_line);
			while (l1 + 1 < bend && l1 + 1 < M107_LINES - 1 && m107_sound_line + (l1 + 1 - l) < nx) l1++;
#endif
			m107_run_sound_line(l, l1);
			l = l1 + 1;
		}
		M107_ACC(1, t1);
	}
	M107_T(t2);
	m107_sound_end(pBurnSoundOut, nBurnSoundLen);
	M107_ACC(2, t2);
	return 0;
}

// Threaded mode: main thread / sound thread (burn_shim rei_core_run_frame / rei_core_sound_frame)
extern "C" void rei_m107_main_frame()
{
	M107_T(t0);
	m107_main_begin();
	for (INT32 l = 0; l < M107_LINES; l++) m107_run_main_line(l);
	M107_ACC(0, t0); // (threaded-mode diag: all main lines incl. waiting for sound)
}

extern "C" { INT32 rei_m107_merge_max = 32; } // (threaded-mode max lines per merge, changed for diagnostics)
extern "C" void rei_m107_sound_frame(INT16 *out, INT32 len)
{
	const UINT32 ep = M107_LOAD(m107_epoch);
	for (INT32 l = 0; l < M107_LINES; ) {
		INT32 l1 = l;
#ifndef REI_NO_M107_SNDMERGE
		// (Threaded mode also merges lines like single thread: if main already finished them and no events lie between — main queued
		//  all their events while running them. Not splitting V35 per line lets auto-idle skip (PSP sound on ME: per-line splits overran the ME). Same result)
		{
			const INT64 done = M107_LOAD(m107_main_done);
			if (done > m107_sound_line + 1) {
				const INT64 nx = m107_m2s_next_after(m107_sound_line);
				while (l1 + 1 < M107_LINES - 1 && l1 + 1 - l < rei_m107_merge_max && m107_sound_line + (l1 + 1 - l) < done && m107_sound_line + (l1 + 1 - l) < nx) l1++;
			}
		}
#endif
		m107_run_sound_line(l, l1);
		l = l1 + 1;
		if (M107_LOAD(m107_epoch) != ep) { // (load/reset: drop this frame, the next call starts at line 0)
			if (out) memset(out, 0, (size_t)len * 2 * sizeof(INT16));
			return;
		}
	}
	m107_sound_end(out, len);
}

static void m107_pipe_clear()
{
	m107_m2s_w = m107_m2s_r = m107_s2m_w = m107_s2m_r = 0;
	m107_main_line = m107_sound_line = m107_main_done = m107_sound_done = 0;
	m107_sound_waiting = -1;
	m107_epoch = m107_epoch + 1;
}

// Reset (with threads stopped): game reset + pipeline from scratch
extern "C" void rei_m107_reset()
{
	DrvDoReset();
	m107_pipe_clear();
}

// Save-safe point: sound has caught up with main and waits for the next line
extern "C" INT32 rei_m107_pipe_synced()
{
	if (!rei_pipelined || !rei_threaded) return 1;
	return M107_LOAD(m107_sound_done) == m107_main_done && M107_LOAD(m107_sound_waiting) == m107_main_done;
}

// Pipeline state for saves: unapplied status events (positions relative to the frame boundary)
#define M107_SAVE_EV 64
struct m107_pipe_state { INT32 count; INT32 src[M107_SAVE_EV]; INT32 apply[M107_SAVE_EV]; UINT8 data[M107_SAVE_EV]; };
extern "C" INT32 rei_m107_pipe_state_size() { return (INT32)sizeof(struct m107_pipe_state); }
extern "C" INT32 rei_m107_pipe_state_save(void *out)
{
	static struct m107_pipe_state tmp; // (out may be unaligned)
	memset(&tmp, 0, sizeof(tmp));
	if (rei_pipelined) {
		if (M107_LOAD(m107_sound_done) != m107_main_done || m107_m2s_r != M107_LOAD(m107_m2s_w)) return -1;
		const INT64 L0 = m107_main_line, c0 = (L0 / M107_LINES) * m107_total33;
		for (UINT32 r = m107_s2m_r; r != M107_LOAD(m107_s2m_w) && tmp.count < M107_SAVE_EV; r++) {
			struct m107_s2m *ev = &m107_s2m_q[r & (M107_RING - 1)];
			tmp.src[tmp.count] = (INT32)(ev->src_line - L0);
			tmp.apply[tmp.count] = (INT32)(ev->apply - c0);
			tmp.data[tmp.count] = ev->data;
			tmp.count++;
		}
	}
	memcpy(out, &tmp, sizeof(tmp));
	return 0;
}

extern "C" void rei_m107_pipe_state_load(const void *in) // (with threads stopped)
{
	static struct m107_pipe_state tmp;
	memcpy(&tmp, in, sizeof(tmp));
	m107_pipe_clear();
	if (!rei_pipelined) return;
	m107_total33 = (INT32)((INT64)(config_cpu_speed / 60) * nBurnCPUSpeedAdjust / 0x0100);
	m107_total35 = (INT32)((INT64)(7159090 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	INT32 n = tmp.count < M107_SAVE_EV ? tmp.count : M107_SAVE_EV;
	for (INT32 i = 0; i < n; i++) {
		struct m107_s2m *ev = &m107_s2m_q[i];
		ev->src_line = tmp.src[i];
		ev->apply = tmp.apply[i];
		ev->data = tmp.data[i];
	}
	m107_s2m_w = (UINT32)n;
}

static INT32 DrvScan(INT32 nAction,INT32 *pnMin)
{
	if (pnMin)
	{
		*pnMin =  0x029671;
	}

	struct BurnArea ba;

	if (nAction & ACB_MEMORY_RAM)
	{
		memset(&ba, 0, sizeof(ba));
		ba.Data		= RamStart;
		ba.nLen		= RamEnd-RamStart;
		ba.szName	= "All Ram";
#ifdef REI_ME_SPLIT
		if (nAction & ACB_READ) memcpy(m107_latch_ram, m107_me_latch, sizeof(m107_me_latch)); // (see m107_me_latch above)
#endif
		BurnAcb(&ba);
#ifdef REI_ME_SPLIT
		if (nAction & ACB_WRITE) memcpy(m107_me_latch, m107_latch_ram, sizeof(m107_me_latch));
#endif
	}

	if (nAction & ACB_DRIVER_DATA)
	{
		VezScan(nAction);

		iremga20_scan(nAction, pnMin);
		BurnYM2151Scan(nAction, pnMin);

		SCAN_VAR(raster_irq_position);
		SCAN_VAR(sound_cpu_reset);
		SCAN_VAR(sprite_enable);
		SCAN_VAR(nBankswitchData);
	}

	if (nAction & ACB_WRITE) {
		for (INT32 i = 0; i < 4; i++) {
			set_pf_scroll(i);
			set_pf_info(i);
		}

		if (has_bankswitch)
		{
			VezOpen(0);
			m107Bankswitch(nBankswitchData);
			VezClose();
		}
	}

	return 0;
}



// Air Assault (World)

static struct BurnRomInfo airassRomDesc[] = {
	{ "f4-a-h0-etc.h0",	0x040000, 0x038f2cbd, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "f4-a-l0-etc.l0",	0x040000, 0xd3eb7842, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "f4-a-h1-ss.h1",	0x020000, 0x4cb1c9ae, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "f4-a-l1-ss.l1",	0x020000, 0x1ddd192d, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "f4-b-sh0-c.sh0",	0x010000, 0x31c05c0d, 2 | BRF_PRG | BRF_ESS }, //  4 V35 Code
	{ "f4-b-sl0-c.sl0",	0x010000, 0x60a0d33a, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "w45.c00",		0x100000, 0x2aab419e, 3 | BRF_GRA },           //  6 Background Tiles
	{ "w46.c10",		0x100000, 0xd6e5c910, 3 | BRF_GRA },           //  7

	{ "w47.000",		0x100000, 0x72e1a253, 4 | BRF_GRA },           //  8 Sprite Tiles
	{ "w48.010",		0x100000, 0x1746b7f6, 4 | BRF_GRA },           //  9
	{ "w49.020",		0x100000, 0x17b5caf2, 4 | BRF_GRA },           // 10
	{ "w50.030",		0x100000, 0x63e4bec3, 4 | BRF_GRA },           // 11

	{ "f4-b-drh-.drh",	0x020000, 0x12001372, 5 | BRF_GRA },           // 12 Sprite Tables
	{ "f4-b-drl-.drl",	0x020000, 0x08cb7533, 5 | BRF_GRA },           // 13

	{ "w96.da0",		0x080000, 0x7a493e2e, 6 | BRF_SND },           // 14 Irem GA20 Samples
};

STD_ROM_PICK(airass)
STD_ROM_FN(airass)

static INT32 airassRomLoad()
{
	if (BurnLoadRom(DrvV33ROM  + 0x000001,  0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x000000,  1, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080001,  2, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080000,  3, 2)) return 1;

	if (BurnLoadRom(DrvV30ROM  + 0x000001,  4, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM  + 0x000000,  5, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM0 + 0x000000,  6, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x000001,  7, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM1 + 0x000000,  8, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100000,  9, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200000, 10, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300000, 11, 1)) return 1;

	if (BurnLoadRom(DrvSprTable + 0x00001, 12, 2)) return 1;
	if (BurnLoadRom(DrvSprTable + 0x00000, 13, 2)) return 1;

	if (BurnLoadRom(DrvSndROM  + 0x000000, 14, 1)) return 1;

	return 0;
}

static INT32 airassInit()
{
	spritesystem = 1;
	config_cpu_speed = 14000000;
	return DrvInit(airassRomLoad, gunforce_decryption_table, 1, 0x20, 0x200000, 0x400000);
}

struct BurnDriver BurnDrvAirass = {
	"airass", NULL, NULL, NULL, "1993",
	"Air Assault (World)\0", NULL, "Irem", "Irem M107",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_ORIENTATION_VERTICAL | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_MISC, GBF_VERSHOOT, 0,
	NULL, airassRomInfo, airassRomName, NULL, NULL, NULL, NULL, FirebarrInputInfo, FirebarrDIPInfo,
	airassInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	240, 320, 3, 4
};


// Fire Barrel (Japan)

static struct BurnRomInfo firebarrRomDesc[] = {
	{ "f4-a-h0-c.h0",	0x40000, 0x2aa5676e, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "f4-a-l0-c.l0",	0x40000, 0x42f75d59, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "f4-a-h1-c.h1",	0x20000, 0xbb7f6968, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "f4-a-l1-c.l1",	0x20000, 0x9d57edd6, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "f4-b-sh0-b.sh0",	0x10000, 0x30a8e232, 2 | BRF_PRG | BRF_ESS }, //  4 V35 Code
	{ "f4-b-sl0-b.sl0",	0x10000, 0x204b5f1f, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "f4-c00.c00",		0x80000, 0x50cab384, 3 | BRF_GRA },           //  6 Background Tiles
	{ "f4-c10.c10",		0x80000, 0x330c6df2, 3 | BRF_GRA },           //  7
	{ "f4-c01.c01",		0x80000, 0x12a698c8, 3 | BRF_GRA },           //  8
	{ "f4-c11.c11",		0x80000, 0x3f9add18, 3 | BRF_GRA },           //  9

	{ "f4-000.000",		0x80000, 0x920deee9, 4 | BRF_GRA },           // 10 Sprite Tiles
	{ "f4-001.001",		0x80000, 0xe5725eaf, 4 | BRF_GRA },           // 11
	{ "f4-010.010",		0x80000, 0x3505d185, 4 | BRF_GRA },           // 12
	{ "f4-011.011",		0x80000, 0x1912682f, 4 | BRF_GRA },           // 13
	{ "f4-020.020",		0x80000, 0xec130b8e, 4 | BRF_GRA },           // 14
	{ "f4-021.021",		0x80000, 0x8dd384dc, 4 | BRF_GRA },           // 15
	{ "f4-030.030",		0x80000, 0x7e7b30cd, 4 | BRF_GRA },           // 16
	{ "f4-031.031",		0x80000, 0x83ac56c5, 4 | BRF_GRA },           // 17

	{ "f4-b-drh-.drh",	0x20000, 0x12001372, 5 | BRF_GRA },           // 18 Sprite Tables
	{ "f4-b-drl-.drl",	0x20000, 0x08cb7533, 5 | BRF_GRA },           // 19

	{ "f4-b-da0.da0",	0x80000, 0x7a493e2e, 6 | BRF_SND },           // 20 Irem GA20 Samples
};

STD_ROM_PICK(firebarr)
STD_ROM_FN(firebarr)

static INT32 firebarrRomLoad()
{
	if (BurnLoadRom(DrvV33ROM  + 0x000001,  0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x000000,  1, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080001,  2, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080000,  3, 2)) return 1;

	if (BurnLoadRom(DrvV30ROM  + 0x000001,  4, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM  + 0x000000,  5, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM0 + 0x000000,  6, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x000001,  7, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x100000,  8, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x100001,  9, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM1 + 0x000000, 10, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x000001, 11, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100000, 12, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100001, 13, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200000, 14, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200001, 15, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300000, 16, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300001, 17, 2)) return 1;

	if (BurnLoadRom(DrvSprTable + 0x00001, 18, 2)) return 1;
	if (BurnLoadRom(DrvSprTable + 0x00000, 19, 2)) return 1;

	if (BurnLoadRom(DrvSndROM  + 0x000000, 20, 1)) return 1;

	return 0;
}

static INT32 firebarrInit()
{
	spritesystem = 1;
	config_cpu_speed = 14000000;
	return DrvInit(firebarrRomLoad, rtypeleo_decryption_table, 2, 0x20, 0x200000, 0x400000);
}

struct BurnDriver BurnDrvFirebarr = {
	"firebarr", "airass", NULL, NULL, "1993",
	"Fire Barrel (Japan)\0", NULL, "Irem", "Irem M107",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_ORIENTATION_VERTICAL | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_MISC, GBF_VERSHOOT, 0,
	NULL, firebarrRomInfo, firebarrRomName, NULL, NULL, NULL, NULL, FirebarrInputInfo, FirebarrDIPInfo,
	firebarrInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	240, 320, 3, 4
};


// Dream Soccer '94 (World, M107 hardware)

static struct BurnRomInfo dsoccr94RomDesc[] = {
	{ "a3-4p_h0-c-0.ic59",	0x040000, 0xd01d3fd7, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "a3-4p_l0-c-0.ic61",	0x040000, 0x8af0afe2, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "a3_h1-c-0.ic60",		0x040000, 0x6109041b, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "a3_l1-c-0.ic62",		0x040000, 0x97a01f6b, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a3-sh0-c-0.ic31",	0x010000, 0x23fe6ffc, 2 | BRF_PRG | BRF_ESS }, //  4 V35 Code
	{ "a3-sl0-c-0.ic37",	0x010000, 0x768132e5, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ds_c00.ic29",		0x100000, 0x2d31d418, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ds_c10.ic28",		0x100000, 0x57f7bcd3, 3 | BRF_GRA },           //  7
	{ "ds_c01.ic21",		0x100000, 0x9d31a464, 3 | BRF_GRA },           //  8
	{ "ds_c11.ic20",		0x100000, 0xa372e79f, 3 | BRF_GRA },           //  9

	{ "ds_000.ic11",		0x100000, 0x366b3e29, 4 | BRF_GRA },           // 10 Sprite Tiles
	{ "ds_010.ic12",		0x100000, 0x28a4cc40, 4 | BRF_GRA },           // 11
	{ "ds_020.ic13",		0x100000, 0x5a310f7f, 4 | BRF_GRA },           // 12
	{ "ds_030.ic14",		0x100000, 0x328b1f45, 4 | BRF_GRA },           // 13

	{ "ds_da0.ic24",		0x100000, 0x67fc52fd, 5 | BRF_SND },           // 14 Irem GA20 Samples
};

STD_ROM_PICK(dsoccr94)
STD_ROM_FN(dsoccr94)

static INT32 dsoccr94RomLoad()
{
	if (BurnLoadRom(DrvV33ROM  + 0x000001,  0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x000000,  1, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080001,  2, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x080000,  3, 2)) return 1;

	if (BurnLoadRom(DrvV30ROM  + 0x000001,  4, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM  + 0x000000,  5, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM0 + 0x000000,  6, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x000001,  7, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x200000,  8, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x200001,  9, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM1 + 0x000000, 10, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100000, 11, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200000, 12, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300000, 13, 1)) return 1;

	if (BurnLoadRom(DrvSndROM  + 0x000000, 14, 1)) return 1;

	return 0;
}

static INT32 dsoccr94Init()
{
	has_bankswitch = 1;
	spritesystem = 0;
	config_cpu_speed = 10000000;
	return DrvInit(dsoccr94RomLoad, dsoccr94_decryption_table, 1, 0x80, 0x400000, 0x400000);
}

struct BurnDriver BurnDrvDsoccr94 = {
	"dsoccr94", NULL, NULL, NULL, "1994",
	"Dream Soccer '94 (World, M107 hardware)\0", NULL, "Irem (Data East Corporation license)", "Irem M107",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_MISC, GBF_SPORTSFOOTBALL, 0,
	NULL, dsoccr94RomInfo, dsoccr94RomName, NULL, NULL, NULL, NULL, Dsoccr94InputInfo, Dsoccr94DIPInfo,
	dsoccr94Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// Dream Soccer '94 (Korea, M107 hardware)
// default team selected is Korea, so likely a Korean set

static struct BurnRomInfo dsoccr94kRomDesc[] = {
	{ "ic59_h0.bin",		0x040000, 0x7b26d8a3, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "ic61_l0.bin",		0x040000, 0xb13f0ff4, 1 | BRF_PRG | BRF_ESS }, //  1
	{ "ic60_h1.bin",		0x040000, 0x6109041b, 1 | BRF_PRG | BRF_ESS }, //  2
	{ "ic62_l1.bin",		0x040000, 0x97a01f6b, 1 | BRF_PRG | BRF_ESS }, //  3

	{ "a3-sh0-c-0.ic31",	0x010000, 0x23fe6ffc, 2 | BRF_PRG | BRF_ESS }, //  4 V35 Code
	{ "a3-sl0-c-0.ic37",	0x010000, 0x768132e5, 2 | BRF_PRG | BRF_ESS }, //  5

	{ "ds_c00.ic29",		0x100000, 0x2d31d418, 3 | BRF_GRA },           //  6 Background Tiles
	{ "ds_c10.ic28",		0x100000, 0x57f7bcd3, 3 | BRF_GRA },           //  7
	{ "ds_c01.ic21",		0x100000, 0x9d31a464, 3 | BRF_GRA },           //  8
	{ "ds_c11.ic20",		0x100000, 0xa372e79f, 3 | BRF_GRA },           //  9

	{ "ds_000.ic11",		0x100000, 0x366b3e29, 4 | BRF_GRA },           // 10 Sprite Tiles
	{ "ds_010.ic12",		0x100000, 0x28a4cc40, 4 | BRF_GRA },           // 11
	{ "ds_020.ic13",		0x100000, 0x5a310f7f, 4 | BRF_GRA },           // 12
	{ "ds_030.ic14",		0x100000, 0x328b1f45, 4 | BRF_GRA },           // 13

	{ "ds_da0.ic24",		0x100000, 0x67fc52fd, 5 | BRF_SND },           // 14 Irem GA20 Samples
};

STD_ROM_PICK(dsoccr94k)
STD_ROM_FN(dsoccr94k)

struct BurnDriver BurnDrvDsoccr94k = {
	"dsoccr94k", "dsoccr94", NULL, NULL, "1994",
	"Dream Soccer '94 (Korea, M107 hardware)\0", NULL, "Irem (Data East Corporation license)", "Irem M107",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_MISC, GBF_SPORTSFOOTBALL, 0,
	NULL, dsoccr94kRomInfo, dsoccr94kRomName, NULL, NULL, NULL, NULL, Dsoccr94InputInfo, Dsoccr94DIPInfo,
	dsoccr94Init, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};


// World PK Soccer

static struct BurnRomInfo wpksocRomDesc[] = {
	{ "pk-h0-eur-d.h0",	0x40000, 0xb4917788, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "pk-l0-eur-d.l0",	0x40000, 0x03816bae, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "pk-sh0.sh0",		0x10000, 0x1145998c, 2 | BRF_PRG | BRF_ESS }, //  2 V35 Code
	{ "pk-sl0.sl0",		0x10000, 0x542ee1c7, 2 | BRF_PRG | BRF_ESS }, //  3

	{ "pk-c00-os.c00",	0x80000, 0x42ae3d73, 3 | BRF_GRA },           //  4 Background Tiles
	{ "pk-c10-os.c10",	0x80000, 0x86acf45c, 3 | BRF_GRA },           //  5
	{ "pk-c01-os.c01",	0x80000, 0xb0d33f87, 3 | BRF_GRA },           //  6
	{ "pk-c11-os.c11",	0x80000, 0x19de7d63, 3 | BRF_GRA },           //  7

	{ "pk-000.000",		0x80000, 0x165ce027, 4 | BRF_GRA },           //  8 Sprite Tiles
	{ "pk-001.001",		0x80000, 0xe2745147, 4 | BRF_GRA },           //  9
	{ "pk-010.010",		0x80000, 0x6c171b73, 4 | BRF_GRA },           // 10
	{ "pk-011.011",		0x80000, 0x471c0bf4, 4 | BRF_GRA },           // 11
	{ "pk-020.020",		0x80000, 0xc886dad1, 4 | BRF_GRA },           // 12
	{ "pk-021.021",		0x80000, 0x91e877ff, 4 | BRF_GRA },           // 13
	{ "pk-030.030",		0x80000, 0x3390621c, 4 | BRF_GRA },           // 14
	{ "pk-031.031",		0x80000, 0x4d322804, 4 | BRF_GRA },           // 15

	{ "pk-da0.da0",		0x80000, 0x26a34cf4, 6 | BRF_SND },           // 16 Irem GA20 Samples
};

STD_ROM_PICK(wpksoc)
STD_ROM_FN(wpksoc)

static INT32 wpksocRomLoad()
{
	if (BurnLoadRom(DrvV33ROM  + 0x000001,  0, 2)) return 1;
	if (BurnLoadRom(DrvV33ROM  + 0x000000,  1, 2)) return 1;

	if (BurnLoadRom(DrvV30ROM  + 0x000001,  2, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM  + 0x000000,  3, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM0 + 0x000000,  4, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x000001,  5, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x100000,  6, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM0 + 0x100001,  7, 2)) return 1;

	if (BurnLoadRom(DrvGfxROM1 + 0x000000,  8, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x000001,  9, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100000, 10, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100001, 11, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200000, 12, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x200001, 13, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300000, 14, 2)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x300001, 15, 2)) return 1;

	if (BurnLoadRom(DrvSndROM  + 0x000000, 16, 1)) return 1;

	return 0;
}

static INT32 wpksocInit()
{
	has_bankswitch = 0;
	spritesystem = 0;
	config_cpu_speed = 14000000;
	return DrvInit(wpksocRomLoad, leagueman_decryption_table, 1, 0x80, 0x200000, 0x400000);
}

struct BurnDriverD BurnDrvWpksoc = {
	"wpksoc", NULL, NULL, NULL, "1995",
	"World PK Soccer\0", NULL, "Jaleco", "Irem M107",
	NULL, NULL, NULL, NULL,
	0, 4, HARDWARE_IREM_MISC, GBF_SPORTSFOOTBALL, 0,
	NULL, wpksocRomInfo, wpksocRomName, NULL, NULL, NULL, NULL, Dsoccr94InputInfo, Dsoccr94DIPInfo,
	wpksocInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};

// Kick for the Goal

static struct BurnRomInfo kftgoalRomDesc[] = {
	{ "pk-h0-usa-d.h0",	0x40000, 0xaed4cde0, 1 | BRF_PRG | BRF_ESS }, //  0 V33 Code
	{ "pk-l0-usa-d.l0",	0x40000, 0x39fe30d2, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "pk-sh0.sh0",		0x10000, 0x1145998c, 2 | BRF_PRG | BRF_ESS }, //  2 V35 Code
	{ "pk-sl0.sl0",		0x10000, 0x542ee1c7, 2 | BRF_PRG | BRF_ESS }, //  3

	{ "pk-c00-os.c00",	0x80000, 0x42ae3d73, 3 | BRF_GRA },           //  4 Background Tiles
	{ "pk-c10-os.c10",	0x80000, 0x86acf45c, 3 | BRF_GRA },           //  5
	{ "pk-c01-os.c01",	0x80000, 0xb0d33f87, 3 | BRF_GRA },           //  6
	{ "pk-c11-os.c11",	0x80000, 0x19de7d63, 3 | BRF_GRA },           //  7

	{ "pk-000-usa.000",	0x80000, 0x72e905ab, 4 | BRF_GRA },           //  8 Sprite Tiles
	{ "pk-001-usa.001",	0x80000, 0xeec4f43c, 4 | BRF_GRA },           //  9
	{ "pk-010-usa.010",	0x80000, 0xb3339d73, 4 | BRF_GRA },           // 10
	{ "pk-011-usa.011",	0x80000, 0xbab2b7cf, 4 | BRF_GRA },           // 11
	{ "pk-020-usa.020",	0x80000, 0x740a0bef, 4 | BRF_GRA },           // 12
	{ "pk-021-usa.021",	0x80000, 0xf44208a6, 4 | BRF_GRA },           // 13
	{ "pk-030-usa.030",	0x80000, 0x8eceef50, 4 | BRF_GRA },           // 14
	{ "pk-031-usa.031",	0x80000, 0x8aa7dc04, 4 | BRF_GRA },           // 15

	{ "pk-da0.da0",		0x80000, 0x26a34cf4, 6 | BRF_SND },           // 16 Irem GA20 Samples

	{ "st-m28c64c.eeprom",	0x02000, 0x8e0c8b7c, 7 | BRF_PRG | BRF_ESS }, // 17 Eeprom data
};

STD_ROM_PICK(kftgoal)
STD_ROM_FN(kftgoal)

struct BurnDriverD BurnDrvKftgoal = {
	"kftgoal", "wpksoc", NULL, NULL, "1994",
	"Kick for the Goal\0", NULL, "Jaleco", "Irem M107",
	NULL, NULL, NULL, NULL,
	BDF_CLONE, 4, HARDWARE_IREM_MISC, GBF_SPORTSFOOTBALL, 0,
	NULL, kftgoalRomInfo, kftgoalRomName, NULL, NULL, NULL, NULL, Dsoccr94InputInfo, Dsoccr94DIPInfo,
	wpksocInit, DrvExit, DrvFrame, DrvReDraw, DrvScan, &bRecalcPalette, 0x800,
	320, 240, 4, 3
};
