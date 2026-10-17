// FB Alpha Irem M90 driver module
// Based on MAME driver by Bryan McPhail

#include "tiles_generic.h"
#include "rei_defer.h" // (ReiREM: deferred drawing)
#include "rei_prof.h"  // (ReiREM: profile build sections)
#include "z80_intf.h"
#include "burn_ym2151.h"
#include "nec_intf.h"
#include "irem_cpu.h"
#include "dac.h"

static UINT8 *AllMem;
static UINT8 *MemEnd;
static UINT8 *AllRam;
static UINT8 *RamEnd;
static UINT8 *DrvV30ROM;
static UINT8 *DrvZ80ROM;
static UINT8 *DrvGfxROM0;
static UINT8 *DrvGfxROM1;
static UINT8 *DrvSndROM;
static UINT8 *DrvVidRAM;
static UINT8 *DrvV30RAM;
static UINT8 *DrvZ80RAM;
static UINT8 *DrvSprRAM;
static UINT8 *DrvPalRAM;

static UINT8 *m90_video_control;

static UINT8 *RamPrioBitmap;

static UINT8 *soundlatch;
static INT32 m90b_push(UINT8 data); // (ReiREM: batched execution, before DrvFrame)

static UINT32 *DrvPalette;
static UINT8 DrvRecalc;

static UINT8 irqvector;
static INT32 sample_address;

static UINT8 DrvJoy1[8];
static UINT8 DrvJoy2[8];
static UINT8 DrvJoy3[8];
static UINT8 DrvJoy4[8];
static UINT8 DrvJoy5[8];
static UINT8 DrvDips[2];
static UINT8 DrvInputs[5];
static UINT8 DrvReset;

static INT32 vblank;
static INT32 code_mask[2];
static INT32 video_offsets[2] = { 0, 0 };

static INT32 is_bbmanw = 0;

enum { VECTOR_INIT, YM2151_ASSERT, YM2151_CLEAR, Z80_ASSERT, Z80_CLEAR };

static struct BurnInputInfo p4commonInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvJoy3 + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvJoy3 + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvJoy3 + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvJoy3 + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"P3 Coin",			BIT_DIGITAL,	DrvJoy4 + 5,	"p3 coin"	},
	{"P3 Start",		BIT_DIGITAL,	DrvJoy4 + 4,	"p3 start"	},
	{"P3 Up",			BIT_DIGITAL,	DrvJoy4 + 3,	"p3 up"		},
	{"P3 Down",			BIT_DIGITAL,	DrvJoy4 + 2,	"p3 down"	},
	{"P3 Left",			BIT_DIGITAL,	DrvJoy4 + 1,	"p3 left"	},
	{"P3 Right",		BIT_DIGITAL,	DrvJoy4 + 0,	"p3 right"	},
	{"P3 Button 1",		BIT_DIGITAL,	DrvJoy4 + 7,	"p3 fire 1"	},
	{"P3 Button 2",		BIT_DIGITAL,	DrvJoy4 + 6,	"p3 fire 2"	},

	{"P4 Coin",			BIT_DIGITAL,	DrvJoy5 + 5,	"p4 coin"	},
	{"P4 Start",		BIT_DIGITAL,	DrvJoy5 + 4,	"p4 start"	},
	{"P4 Up",			BIT_DIGITAL,	DrvJoy5 + 3,	"p4 up"		},
	{"P4 Down",			BIT_DIGITAL,	DrvJoy5 + 2,	"p4 down"	},
	{"P4 Left",			BIT_DIGITAL,	DrvJoy5 + 1,	"p4 left"	},
	{"P4 Right",		BIT_DIGITAL,	DrvJoy5 + 0,	"p4 right"	},
	{"P4 Button 1",		BIT_DIGITAL,	DrvJoy5 + 7,	"p4 fire 1"	},
	{"P4 Button 2",		BIT_DIGITAL,	DrvJoy5 + 6,	"p4 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvJoy3 + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvDips + 0,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvDips + 1,	"dip"		},
};

STDINPUTINFO(p4common)

static struct BurnInputInfo p2commonInputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvJoy3 + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvJoy3 + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvJoy3 + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvJoy3 + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvJoy3 + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvDips + 0,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvDips + 1,	"dip"		},
};

STDINPUTINFO(p2common)

static struct BurnInputInfo Quizf1InputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvJoy3 + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvJoy3 + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},
	{"P1 Button 3",		BIT_DIGITAL,	DrvJoy1 + 5,	"p1 fire 3"	},
	{"P1 Button 4",		BIT_DIGITAL,	DrvJoy1 + 4,	"p1 fire 4"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvJoy3 + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvJoy3 + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},
	{"P2 Button 3",		BIT_DIGITAL,	DrvJoy2 + 5,	"p2 fire 3"	},
	{"P2 Button 4",		BIT_DIGITAL,	DrvJoy2 + 4,	"p2 fire 4"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvJoy3 + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvDips + 0,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvDips + 1,	"dip"		},
};

STDINPUTINFO(Quizf1)

static struct BurnInputInfo Matchit2InputList[] = {
	{"P1 Coin",			BIT_DIGITAL,	DrvJoy3 + 2,	"p1 coin"	},
	{"P1 Start",		BIT_DIGITAL,	DrvJoy3 + 0,	"p1 start"	},
	{"P1 Up",			BIT_DIGITAL,	DrvJoy1 + 3,	"p1 up"		},
	{"P1 Down",			BIT_DIGITAL,	DrvJoy1 + 2,	"p1 down"	},
	{"P1 Left",			BIT_DIGITAL,	DrvJoy1 + 1,	"p1 left"	},
	{"P1 Right",		BIT_DIGITAL,	DrvJoy1 + 0,	"p1 right"	},
	{"P1 Button 1",		BIT_DIGITAL,	DrvJoy1 + 7,	"p1 fire 1"	},
	{"P1 Button 2",		BIT_DIGITAL,	DrvJoy1 + 6,	"p1 fire 2"	},
	{"P1 Button 3",		BIT_DIGITAL,	DrvJoy1 + 5,	"p1 fire 3"	},

	{"P2 Coin",			BIT_DIGITAL,	DrvJoy3 + 3,	"p2 coin"	},
	{"P2 Start",		BIT_DIGITAL,	DrvJoy3 + 1,	"p2 start"	},
	{"P2 Up",			BIT_DIGITAL,	DrvJoy2 + 3,	"p2 up"		},
	{"P2 Down",			BIT_DIGITAL,	DrvJoy2 + 2,	"p2 down"	},
	{"P2 Left",			BIT_DIGITAL,	DrvJoy2 + 1,	"p2 left"	},
	{"P2 Right",		BIT_DIGITAL,	DrvJoy2 + 0,	"p2 right"	},
	{"P2 Button 1",		BIT_DIGITAL,	DrvJoy2 + 7,	"p2 fire 1"	},
	{"P2 Button 2",		BIT_DIGITAL,	DrvJoy2 + 6,	"p2 fire 2"	},
	{"P2 Button 3",		BIT_DIGITAL,	DrvJoy2 + 5,	"p2 fire 3"	},

	{"Reset",			BIT_DIGITAL,	&DrvReset,		"reset"		},
	{"Service",			BIT_DIGITAL,	DrvJoy3 + 4,	"service"	},
	{"Dip A",			BIT_DIPSWITCH,	DrvDips + 0,	"dip"		},
	{"Dip B",			BIT_DIPSWITCH,	DrvDips + 1,	"dip"		},
};

STDINPUTINFO(Matchit2)

static struct BurnDIPInfo DynablstDIPList[]=
{
	{0x22, 0xff, 0xff, 0xbf, NULL							},
	{0x23, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x22, 0x01, 0x03, 0x02, "2"							},
	{0x22, 0x01, 0x03, 0x03, "3"							},
	{0x22, 0x01, 0x03, 0x01, "4"							},
	{0x22, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x22, 0x01, 0x0c, 0x08, "Easy"							},
	{0x22, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x22, 0x01, 0x0c, 0x04, "Hard"							},
	{0x22, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Game Title"					},
	{0x22, 0x01, 0x10, 0x10, "Dynablaster"					},
	{0x22, 0x01, 0x10, 0x00, "Bomber Man"					},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x22, 0x01, 0x20, 0x00, "No"							},
	{0x22, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x22, 0x01, 0x40, 0x40, "Off"							},
	{0x22, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x22, 0x01, 0x80, 0x80, "Off"							},
	{0x22, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x23, 0x01, 0x01, 0x01, "Off"							},
	{0x23, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    4, "Cabinet"						},
	{0x23, 0x01, 0x06, 0x04, "2 Player Upright"				},
	{0x23, 0x01, 0x06, 0x06, "4 Player Upright A"			},
	{0x23, 0x01, 0x06, 0x02, "4 Player Upright B"			},
	{0x23, 0x01, 0x06, 0x00, "Cocktail"						},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x23, 0x01, 0x08, 0x08, "1"							},
	{0x23, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x23, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Dynablst)

static struct BurnDIPInfo AtompunkDIPList[]=
{
	{0x22, 0xff, 0xff, 0xbf, NULL							},
	{0x23, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x22, 0x01, 0x03, 0x02, "2"							},
	{0x22, 0x01, 0x03, 0x03, "3"							},
	{0x22, 0x01, 0x03, 0x01, "4"							},
	{0x22, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x22, 0x01, 0x0c, 0x08, "Easy"							},
	{0x22, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x22, 0x01, 0x0c, 0x04, "Hard"							},
	{0x22, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Unknown"						},
	{0x22, 0x01, 0x10, 0x10, "Off"							},
	{0x22, 0x01, 0x10, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x22, 0x01, 0x20, 0x00, "No"							},
	{0x22, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x22, 0x01, 0x40, 0x40, "Off"							},
	{0x22, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x22, 0x01, 0x80, 0x80, "Off"							},
	{0x22, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x23, 0x01, 0x01, 0x01, "Off"							},
	{0x23, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    4, "Cabinet"						},
	{0x23, 0x01, 0x06, 0x04, "2 Player Upright"				},
	{0x23, 0x01, 0x06, 0x06, "4 Player Upright A"			},
	{0x23, 0x01, 0x06, 0x02, "4 Player Upright B"			},
	{0x23, 0x01, 0x06, 0x00, "Cocktail"						},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x23, 0x01, 0x08, 0x08, "1"							},
	{0x23, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x23, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Atompunk)

static struct BurnDIPInfo BombrmanDIPList[]=
{
	{0x12, 0xff, 0xff, 0xaf, NULL							},
	{0x13, 0xff, 0xff, 0xf9, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x12, 0x01, 0x03, 0x02, "2"							},
	{0x12, 0x01, 0x03, 0x03, "3"							},
	{0x12, 0x01, 0x03, 0x01, "4"							},
	{0x12, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x12, 0x01, 0x0c, 0x08, "Easy"							},
	{0x12, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x12, 0x01, 0x0c, 0x04, "Hard"							},
	{0x12, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x12, 0x01, 0x20, 0x00, "No"							},
	{0x12, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x12, 0x01, 0x40, 0x40, "Off"							},
	{0x12, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x12, 0x01, 0x80, 0x80, "Off"							},
	{0x12, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x13, 0x01, 0x01, 0x01, "Off"							},
	{0x13, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x13, 0x01, 0x08, 0x08, "1"							},
	{0x13, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x13, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Bombrman)

static struct BurnDIPInfo HasamuDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL							},
	{0x13, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x12, 0x01, 0x20, 0x00, "No"							},
	{0x12, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x12, 0x01, 0x40, 0x40, "Off"							},
	{0x12, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x12, 0x01, 0x80, 0x80, "Off"							},
	{0x12, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x13, 0x01, 0x01, 0x01, "Off"							},
	{0x13, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Unknown"						},
	{0x13, 0x01, 0x02, 0x02, "Off"							},
	{0x13, 0x01, 0x02, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x13, 0x01, 0x08, 0x08, "1"							},
	{0x13, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x13, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Hasamu)

static struct BurnDIPInfo RiskchalDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL							},
	{0x13, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x12, 0x01, 0x03, 0x02, "2"							},
	{0x12, 0x01, 0x03, 0x03, "3"							},
	{0x12, 0x01, 0x03, 0x01, "4"							},
	{0x12, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x12, 0x01, 0x0c, 0x08, "Easy"							},
	{0x12, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x12, 0x01, 0x0c, 0x04, "Hard"							},
	{0x12, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x12, 0x01, 0x40, 0x40, "Off"							},
	{0x12, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x12, 0x01, 0x80, 0x80, "Off"							},
	{0x12, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x13, 0x01, 0x01, 0x01, "Off"							},
	{0x13, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Slots"					},
	{0x13, 0x01, 0x04, 0x04, "Common"						},
	{0x13, 0x01, 0x04, 0x00, "Separate"						},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x13, 0x01, 0x08, 0x08, "1"							},
	{0x13, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x13, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Riskchal)

static struct BurnDIPInfo DicegameDIPList[]=
{
	{0x12, 0xff, 0xff, 0xbf, NULL							},
	{0x13, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x12, 0x01, 0x03, 0x02, "2"							},
	{0x12, 0x01, 0x03, 0x03, "3"							},
	{0x12, 0x01, 0x03, 0x01, "4"							},
	{0x12, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x12, 0x01, 0x0c, 0x08, "Easy"							},
	{0x12, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x12, 0x01, 0x0c, 0x04, "Hard"							},
	{0x12, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x12, 0x01, 0x40, 0x40, "Off"							},
	{0x12, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x12, 0x01, 0x80, 0x80, "Off"							},
	{0x12, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x13, 0x01, 0x01, 0x01, "Off"							},
	{0x13, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Slots"					},
	{0x13, 0x01, 0x04, 0x04, "Common"						},
	{0x13, 0x01, 0x04, 0x00, "Separate"						},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x13, 0x01, 0x08, 0x08, "1"							},
	{0x13, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x13, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Dicegame)

static struct BurnDIPInfo BbmanwDIPList[]=
{
	{0x22, 0xff, 0xff, 0xbf, NULL							},
	{0x23, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x22, 0x01, 0x03, 0x02, "2"							},
	{0x22, 0x01, 0x03, 0x03, "3"							},
	{0x22, 0x01, 0x03, 0x01, "4"							},
	{0x22, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x22, 0x01, 0x0c, 0x08, "Easy"							},
	{0x22, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x22, 0x01, 0x0c, 0x04, "Hard"							},
	{0x22, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Game Title"					},
	{0x22, 0x01, 0x10, 0x10, "Bomber Man World"				},
	{0x22, 0x01, 0x10, 0x00, "New Dyna Blaster Global Quest"	},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x22, 0x01, 0x20, 0x00, "No"							},
	{0x22, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x22, 0x01, 0x40, 0x40, "Off"							},
	{0x22, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x22, 0x01, 0x80, 0x80, "Off"							},
	{0x22, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x23, 0x01, 0x01, 0x01, "Off"							},
	{0x23, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    4, "Cabinet"						},
	{0x23, 0x01, 0x06, 0x04, "2 Player"						},
	{0x23, 0x01, 0x06, 0x06, "4 Player Seprate Coins"		},
	{0x23, 0x01, 0x06, 0x02, "4 Player Shared Coins"		},
	{0x23, 0x01, 0x06, 0x00, "4 Player 1&2 3&4 Share Coins"		},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x23, 0x01, 0x08, 0x08, "1"							},
	{0x23, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x23, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Bbmanw)

static struct BurnDIPInfo BbmanwjDIPList[]=
{
	{0x22, 0xff, 0xff, 0xbf, NULL							},
	{0x23, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x22, 0x01, 0x03, 0x02, "2"							},
	{0x22, 0x01, 0x03, 0x03, "3"							},
	{0x22, 0x01, 0x03, 0x01, "4"							},
	{0x22, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x22, 0x01, 0x0c, 0x08, "Easy"							},
	{0x22, 0x01, 0x0c, 0x0c, "Medium"						},
	{0x22, 0x01, 0x0c, 0x04, "Hard"							},
	{0x22, 0x01, 0x0c, 0x00, "Hardest"						},

	{0   , 0xfe, 0   ,    2, "Allow Continue"				},
	{0x22, 0x01, 0x20, 0x00, "No"							},
	{0x22, 0x01, 0x20, 0x20, "Yes"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x22, 0x01, 0x40, 0x40, "Off"							},
	{0x22, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x22, 0x01, 0x80, 0x80, "Off"							},
	{0x22, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x23, 0x01, 0x01, 0x01, "Off"							},
	{0x23, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    4, "Cabinet"						},
	{0x23, 0x01, 0x06, 0x04, "2 Player"						},
	{0x23, 0x01, 0x06, 0x06, "4 Player Seprate Coins"		},
	{0x23, 0x01, 0x06, 0x02, "4 Player Shared Coins"		},
	{0x23, 0x01, 0x06, 0x00, "4 Player 1&2 3&4 Share Coins"		},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x23, 0x01, 0x08, 0x08, "1"							},
	{0x23, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
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
	{0x23, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Bbmanwj)

static struct BurnDIPInfo Quizf1DIPList[]=
{
	{0x16, 0xff, 0xff, 0x9f, NULL							},
	{0x17, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    4, "Lives"						},
	{0x16, 0x01, 0x03, 0x02, "2"							},
	{0x16, 0x01, 0x03, 0x03, "3"							},
	{0x16, 0x01, 0x03, 0x01, "4"							},
	{0x16, 0x01, 0x03, 0x00, "5"							},

	{0   , 0xfe, 0   ,    2, "Input Device"					},
	{0x16, 0x01, 0x20, 0x20, "Joystick"						},
	{0x16, 0x01, 0x20, 0x00, "Buttons"						},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x16, 0x01, 0x40, 0x40, "Off"							},
	{0x16, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x16, 0x01, 0x80, 0x80, "Off"							},
	{0x16, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x17, 0x01, 0x01, 0x01, "Off"							},
	{0x17, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x17, 0x01, 0x08, 0x08, "1"							},
	{0x17, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
	{0x17, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x17, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x17, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x17, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x17, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x17, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x17, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x17, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x17, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x17, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x17, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x17, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x17, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x17, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x17, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x17, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Quizf1)

static struct BurnDIPInfo Matchit2DIPList[]=
{
	{0x14, 0xff, 0xff, 0xff, NULL							},
	{0x15, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    2, "Girls Mode"					},
	{0x14, 0x01, 0x01, 0x00, "Off"							},
	{0x14, 0x01, 0x01, 0x01, "On"							},

	{0   , 0xfe, 0   ,    2, "China Tiles"					},
	{0x14, 0x01, 0x02, 0x02, "Mahjong"						},
	{0x14, 0x01, 0x02, 0x00, "Alpha-Numeric"				},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x14, 0x01, 0x0c, 0x00, "Very Hard"					},
	{0x14, 0x01, 0x0c, 0x04, "Hard"							},
	{0x14, 0x01, 0x0c, 0x0c, "Normal"						},
	{0x14, 0x01, 0x0c, 0x08, "Easy"							},

	{0   , 0xfe, 0   ,    4, "Timer Speed"					},
	{0x14, 0x01, 0x30, 0x00, "Very Hard"					},
	{0x14, 0x01, 0x30, 0x10, "Hard"							},
	{0x14, 0x01, 0x30, 0x30, "Normal"						},
	{0x14, 0x01, 0x30, 0x20, "Easy"							},

	{0   , 0xfe, 0   ,    2, "Title Screen"					},
	{0x14, 0x01, 0x40, 0x40, "Match It II"					},
	{0x14, 0x01, 0x40, 0x00, "Shisensho II"					},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x14, 0x01, 0x80, 0x80, "Off"							},
	{0x14, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x15, 0x01, 0x01, 0x01, "Off"							},
	{0x15, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    3, "Language"						},
	{0x15, 0x01, 0x06, 0x06, "English"						},
	{0x15, 0x01, 0x06, 0x04, "German"						},
	{0x15, 0x01, 0x06, 0x02, "Korean"						},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x15, 0x01, 0x08, 0x08, "1"							},
	{0x15, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
	{0x15, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x15, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x15, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x15, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x15, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x15, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x15, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x15, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x15, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x15, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x15, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Matchit2)

static struct BurnDIPInfo Shisen2DIPList[]=
{
	{0x14, 0xff, 0xff, 0xbf, NULL							},
	{0x15, 0xff, 0xff, 0xff, NULL							},

	{0   , 0xfe, 0   ,    2, "Girls Mode"					},
	{0x14, 0x01, 0x01, 0x00, "Off"							},
	{0x14, 0x01, 0x01, 0x01, "On"							},

	{0   , 0xfe, 0   ,    4, "Difficulty"					},
	{0x14, 0x01, 0x0c, 0x00, "Very Hard"					},
	{0x14, 0x01, 0x0c, 0x04, "Hard"							},
	{0x14, 0x01, 0x0c, 0x0c, "Normal"						},
	{0x14, 0x01, 0x0c, 0x08, "Easy"							},

	{0   , 0xfe, 0   ,    4, "Timer Speed"					},
	{0x14, 0x01, 0x30, 0x00, "Very Hard"					},
	{0x14, 0x01, 0x30, 0x10, "Hard"							},
	{0x14, 0x01, 0x30, 0x30, "Normal"						},
	{0x14, 0x01, 0x30, 0x20, "Easy"							},

	{0   , 0xfe, 0   ,    2, "Demo Sounds"					},
	{0x14, 0x01, 0x40, 0x40, "Off"							},
	{0x14, 0x01, 0x40, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Service Mode"					},
	{0x14, 0x01, 0x80, 0x80, "Off"							},
	{0x14, 0x01, 0x80, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Flip Screen"					},
	{0x15, 0x01, 0x01, 0x01, "Off"							},
	{0x15, 0x01, 0x01, 0x00, "On"							},

	{0   , 0xfe, 0   ,    2, "Coin Mode"					},
	{0x15, 0x01, 0x08, 0x08, "1"							},
	{0x15, 0x01, 0x08, 0x00, "2"							},

	{0   , 0xfe, 0   ,    16, "Coinage"						},
	{0x15, 0x01, 0xf0, 0xa0, "6 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xb0, "5 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xc0, "4 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xd0, "3 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0xe0, "2 Coins 1 Credits"			},
	{0x15, 0x01, 0xf0, 0x10, "2 Coins to Start/1 to Continue"	},
	{0x15, 0x01, 0xf0, 0x30, "3 Coins 2 Credits"			},
	{0x15, 0x01, 0xf0, 0x20, "4 Coins 3 Credits"			},
	{0x15, 0x01, 0xf0, 0xf0, "1 Coin  1 Credits"			},
	{0x15, 0x01, 0xf0, 0x40, "2 Coins 3 Credits"			},
	{0x15, 0x01, 0xf0, 0x90, "1 Coin  2 Credits"			},
	{0x15, 0x01, 0xf0, 0x80, "1 Coin  3 Credits"			},
	{0x15, 0x01, 0xf0, 0x70, "1 Coin  4 Credits"			},
	{0x15, 0x01, 0xf0, 0x60, "1 Coin  5 Credits"			},
	{0x15, 0x01, 0xf0, 0x50, "1 Coin  6 Credits"			},
	{0x15, 0x01, 0xf0, 0x00, "Free Play"					},
};

STDDIPINFO(Shisen2)

static void setvector_callback(INT32 param)
{
	switch (param)
	{
		case VECTOR_INIT:   irqvector  = 0xff; break;
		case YM2151_ASSERT: irqvector &= 0xef; break;
		case YM2151_CLEAR:  irqvector |= 0x10; break;
		case Z80_ASSERT:    irqvector &= 0xdf; break;
		case Z80_CLEAR:     irqvector |= 0x20; break;
	}

	ZetSetVector(irqvector);
	ZetSetIRQLine(0, (irqvector == 0xff) ? CPU_IRQSTATUS_NONE : CPU_IRQSTATUS_ACK);
}

static inline void update_palette_entry(INT32 entry)
{
	UINT16 d = *((UINT16*)(DrvPalRAM + entry));

#ifndef LSB_FIRST
	d = BURN_ENDIAN_SWAP_INT16(d);
#endif

	UINT8 r = (d >>  0) & 0x1f;
	UINT8 g = (d >>  5) & 0x1f;
	UINT8 b = (d >> 10) & 0x1f;
	
	r = (r << 3) | (r >> 2);
	g = (g << 3) | (g >> 2);
	b = (b << 3) | (b >> 2);

	DrvPalette[entry / 2] = BurnHighCol(r, g, b, 0);
}

static void __fastcall m90_main_write(UINT32 address, UINT8 data)
{
	if ((address & 0xffc00) == 0xe0000) {
		DrvPalRAM[address & 0x3ff] = data;
		update_palette_entry(address & 0x3fe);
		return;
	}
}

static UINT8 __fastcall m90_main_read(UINT32 /*address*/)
{
	return 0;
}

static void __fastcall m90_main_write_port(UINT32 port, UINT8 data)
{
	if ((port & 0xf0) == 0x80) {
		m90_video_control[port & 0x0f] = data;
		return;
	}

	switch (port)
	{
		case 0x00:
	//	case 0x01:
			if (m90b_push(data)) return; // (ReiREM: in batched mode, before the Z80 slice)
			*soundlatch = data;
			setvector_callback(Z80_ASSERT);
		return;

		case 0x02:
	//	case 0x03:
			// coin counter
		return;

		case 0x04:
		//case 0x05:
		{
			INT32 bankaddress = 0x100000 + 0x10000 * (data & 0x0f);
			VezMapArea(0x80000, 0x8ffff, 0, DrvV30ROM + bankaddress);
			VezMapArea(0x80000, 0x8ffff, 2, DrvV30ROM + bankaddress);
		}
		return;

		case 0x90:
	//	case 0x91: // nop dynablsb
		return; 

		//default:
		//	bprintf (0, _T("Writing %2.2x to port %2.2x wp\n"), data, port);
		//	break;
	}
}

static UINT8 __fastcall m90_main_read_port(UINT32 port)
{
	switch (port)
	{
		case 0x00: return DrvInputs[0];
		case 0x01: return DrvInputs[1];
		case 0x02: return (DrvInputs[2] & 0x7f) | vblank; // system
	//	case 0x03: return 0xff; // ?
		case 0x04: return DrvDips[0];
		case 0x05: return DrvDips[1];
		case 0x06: return DrvInputs[3];
		case 0x07: return DrvInputs[4];

		//default:
		//	bprintf (0, _T("Reading port: %2.2x\n"), port);
		//	break;
	}

	return 0;
}

static void __fastcall m90_sound_write_port(UINT16 port, UINT8 data)
{

	if (is_bbmanw) {
		switch (port & 0xff) {
			case 0x00: port = 0x10; break;
			case 0x01: port = 0x11; break;
		}
	}
	switch (port & 0xff)
	{
		case 0x00:
		case 0x40: // bbmanw
			BurnYM2151SelectRegister(data);
		return;

		case 0x01:
		case 0x41: // bbmanw
			BurnYM2151WriteRegister(data);
		return;

		case 0x10: // bbmanw
			sample_address >>= 4;
			sample_address = (sample_address & 0xff00) | (data << 0);
			sample_address <<= 4;
		return;

		case 0x11: // bbmanw
			sample_address >>= 4;
			sample_address = (sample_address & 0x00ff) | (data << 8);
			sample_address <<= 4;
		return;

		case 0x80:
			sample_address >>= 5;
			sample_address = (sample_address & 0xff00) | (data << 0);
			sample_address <<= 5;
		return;

		case 0x81:
			sample_address >>= 5;
			sample_address = (sample_address & 0x00ff) | (data << 8);
			sample_address <<= 5;
		return;

		case 0x82:
			DACSignedWrite(0, data);
			sample_address = (sample_address + 1) & 0x3ffff;
		return;

		case 0x42: // bbmanw
		case 0x83:
			setvector_callback(Z80_CLEAR);
		return;
	}
}

static UINT8 __fastcall m90_sound_read_port(UINT16 port)
{
	switch (port & 0xff)
	{
		case 0x00:
		case 0x01:
		case 0x40: // bbmanw
		case 0x41:
			return BurnYM2151Read();

		case 0x42: // bbmanw
		case 0x80:
			ZetSetVector(0xff);
			ZetSetIRQLine(0, CPU_IRQSTATUS_NONE);
			return *soundlatch;

		case 0x84:
			return DrvSndROM[sample_address & 0x3ffff];
	}

	return 0;
}

static void m72YM2151IRQHandler(INT32 nStatus)
{
	setvector_callback(nStatus ? YM2151_ASSERT : YM2151_CLEAR);
}

static INT32 DrvDoReset()
{
	memset (AllRam, 0, RamEnd - AllRam);

	VezOpen(0);
	VezReset();
	VezClose();

	ZetOpen(0);
	ZetReset();
	setvector_callback(VECTOR_INIT);
	ZetClose();

	BurnYM2151Reset();
	DACReset();

	sample_address = 0;

	HiscoreReset();

	return 0;
}

static INT32 MemIndex()
{
	UINT8 *Next; Next = AllMem;

	DrvV30ROM 	= Next; Next += 0x200000;
	DrvZ80ROM	= Next; Next += 0x010000;
	DrvGfxROM0	= Next; Next += 0x400000;
	DrvGfxROM1	= Next; Next += 0x400000;
	DrvSndROM	= Next; Next += 0x040000;

	RamPrioBitmap	= Next; Next += nScreenWidth * nScreenHeight;

	AllRam	= Next;

	DrvSprRAM	= Next; Next += 0x000800;
	DrvVidRAM	= Next; Next += 0x010000;
	DrvV30RAM	= Next; Next += 0x004000;
	DrvPalRAM	= Next; Next += 0x000800;
	DrvZ80RAM	= Next; Next += 0x001000;

	soundlatch	= Next; Next += 0x000004; // 1

	m90_video_control	= Next; Next += 0x000010;

	RamEnd		= Next;

	DrvPalette	= (UINT32 *) Next; Next += 0x0200 * sizeof(UINT32);

	MemEnd		= Next;

	return 0;
}

static INT32 DrvLoadRoms(INT32 codesize, INT32 samples, INT32 bank)
{
	if (BurnLoadRom(DrvV30ROM + 1,  0, 2)) return 1;
	if (BurnLoadRom(DrvV30ROM + 0,  1, 2)) return 1;

	if (codesize == 0x40000) {
		memcpy (DrvV30ROM + 0x7fff0, DrvV30ROM + 0x3fff0, 0x10);
	}

	if (BurnLoadRom(DrvZ80ROM,  2, 1)) return 1;

	if (BurnLoadRom(DrvGfxROM1 + 0x000000,  3, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x080000,  4, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x100000,  5, 1)) return 1;
	if (BurnLoadRom(DrvGfxROM1 + 0x180000,  6, 1)) return 1;

	if (samples) {
		if (BurnLoadRom(DrvSndROM,  7, 1)) return 1;

		if (samples < 0x40000) {
			memcpy (DrvSndROM + 0x20000, DrvSndROM + 0x00000, 0x20000);
		}
	}

	if (bank) {
		if (BurnLoadRom(DrvV30ROM + 0x100001,  8, 2)) return 1;
		if (BurnLoadRom(DrvV30ROM + 0x100000,  9, 2)) return 1;
	}

	return 0;
}

static void map_main_cpu(UINT8 *decrypt_table, INT32 codesize, INT32 spriteram)
{
	VezInit(0, V35_TYPE, 32000000/2);

	VezOpen(0);
	if (decrypt_table != NULL) {
		VezSetDecode(decrypt_table);
	}

	VezMapArea(0x00000, (codesize-1), 0, DrvV30ROM + 0x000000);
	VezMapArea(0x00000, (codesize-1), 2, DrvV30ROM + 0x000000);
	VezMapArea(0x80000, 0x8ffff, 0, DrvV30ROM + 0x100000);
	VezMapArea(0x80000, 0x8ffff, 2, DrvV30ROM + 0x100000);
	VezMapArea(0xa0000, 0xa3fff, 0, DrvV30RAM);
	VezMapArea(0xa0000, 0xa3fff, 1, DrvV30RAM);
	VezMapArea(0xa0000, 0xa3fff, 2, DrvV30RAM);
	VezMapArea(0xd0000, 0xdffff, 0, DrvVidRAM);
	VezMapArea(0xd0000, 0xdffff, 1, DrvVidRAM);
	VezMapArea(0xd0000, 0xdffff, 2, DrvVidRAM);
	VezMapArea(0xe0000, 0xe07ff, 0, DrvPalRAM);
//	VezMapArea(0xe0000, 0xe07ff, 1, DrvPalRAM);
	VezMapArea(0xe0000, 0xe07ff, 2, DrvPalRAM);
	VezMapArea(0xff800, 0xfffff, 0, DrvV30ROM + 0x07f800);
	VezMapArea(0xff800, 0xfffff, 2, DrvV30ROM + 0x07f800);

	if (spriteram) {
		VezMapArea(0x00000 + spriteram, 0x00fff + spriteram, 0, DrvSprRAM); // 00e - fff // ??
		VezMapArea(0x00000 + spriteram, 0x00fff + spriteram, 1, DrvSprRAM);
		VezMapArea(0x00000 + spriteram, 0x00fff + spriteram, 2, DrvSprRAM);	
	}

	VezSetReadHandler(m90_main_read);
	VezSetWriteHandler(m90_main_write);
	VezSetReadPort(m90_main_read_port);
	VezSetWritePort(m90_main_write_port);
	VezClose();
}

static void DrvGfxDecode()
{
	INT32 Plane[4]  = { 0x180000 * 8, 0x100000 * 8, 0x080000 * 8, 0x000000 * 8 };
	INT32 XOffs[16] = { STEP8(0, 1), STEP8(128, 1) };
	INT32 YOffs[16] = { STEP16(0, 8) };

	UINT8 *tmp = (UINT8*)BurnMalloc(0x200000);
	if (tmp == NULL) {
		return;
	}

	memcpy (tmp, DrvGfxROM1, 0x200000);

	GfxDecode(0x10000, 4,  8,  8, Plane, XOffs, YOffs, 0x040, tmp, DrvGfxROM0);
	GfxDecode(0x04000, 4, 16, 16, Plane, XOffs, YOffs, 0x100, tmp, DrvGfxROM1);

	BurnFree (tmp);
}

static void m90_defer_register(); // (ReiREM: deferred drawing)

static INT32 DrvInit(INT32 codesize, INT32 gfxlen, INT32 samples, INT32 bank, INT32 spriteram, const UINT8 *decrypt_table)
{
	GenericTilesInit();

	AllMem = NULL;
	MemIndex();
	INT32 nLen = MemEnd - (UINT8 *)0;
	if ((AllMem = (UINT8 *)BurnMalloc(nLen)) == NULL) return 1;
	memset(AllMem, 0, nLen);
	MemIndex();

	if (DrvLoadRoms(codesize, samples, bank)) return 1; 

	DrvGfxDecode();

	map_main_cpu((UINT8*)decrypt_table, codesize, spriteram);

	ZetInit(0);
	ZetOpen(0);
	ZetMapMemory(DrvZ80ROM, 0x0000, 0xefff, MAP_ROM);
	ZetMapMemory(DrvZ80RAM, 0xf000, 0xffff, MAP_RAM);
	ZetSetOutHandler(m90_sound_write_port);
	ZetSetInHandler(m90_sound_read_port);
	ZetClose();

	BurnYM2151Init(3579545);
	YM2151SetIrqHandler(0, &m72YM2151IRQHandler);
	BurnYM2151SetAllRoutes(0.15, BURN_SND_ROUTE_BOTH);

	DACInit(0, 0, 1, ZetTotalCycles, 3579545);
	DACSetRoute(0, 0.20, BURN_SND_ROUTE_BOTH);

	code_mask[0] = ((gfxlen * 2) - 1) / (8 * 8);
	code_mask[1] = ((gfxlen * 2) - 1) / (16 * 16);

	DrvDoReset();

	m90_defer_register();

	return 0;
}

#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
static void m90_tile_pens_exit(); // (before draw_layer below)
#endif

static INT32 DrvExit()
{
	GenericTilesExit();
#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
	m90_tile_pens_exit();
#endif

	BurnYM2151Exit();
	DACExit();

	ZetExit();
	VezExit();

	BurnFree(AllMem);

	video_offsets[0] = video_offsets[1] = 0;

	is_bbmanw = 0;

	return 0;
}

// (ReiREM: deferred drawing, ADR-018) State read by the draw functions: real memory when drawing directly, captured state on the render thread
static REI_RDR UINT8 *rdVidRAM, *rdVideoControl;
static REI_RDR UINT32 *rdPalette;
static void rd_live() { rdVidRAM = DrvVidRAM; rdVideoControl = m90_video_control; rdPalette = DrvPalette; }
struct m90_seg { UINT8 vidram[0x10000], video_control[0x10]; UINT32 palette[0x200]; };
static void DrvDraw_now();
static void m90_capture(void *p)
{
	struct m90_seg *g = (struct m90_seg *)p;
	memcpy(g->vidram, DrvVidRAM, sizeof(g->vidram));
	memcpy(g->video_control, m90_video_control, sizeof(g->video_control));
	memcpy(g->palette, DrvPalette, sizeof(g->palette));
}
static void m90_apply(const void *p)
{
	struct m90_seg *g = (struct m90_seg *)p;
	rdVidRAM = g->vidram;
	rdVideoControl = g->video_control;
	rdPalette = g->palette;
}
static void m90_draw_unused(INT32, INT32) {}
static void m90_prepare(); // (next to m90_tile_pens below)
static void m90_defer_register()
{
	// Drawn whole once per frame, so draw at frame end without segments
	static const rei_defer_ops ops = { sizeof(struct m90_seg), m90_capture, m90_apply, m90_draw_unused, NULL, DrvDraw_now, m90_prepare };
	rei_defer_register(&ops);
}

static void RenderTilePrio(UINT16 *dest, UINT8 *gfx, INT32 code, INT32 color, INT32 sx, INT32 sy, INT32 flipx, INT32 flipy, INT32 width, INT32 height, UINT8 *pri, INT32 prio)
{
	if (sx <= (0-width) || sx >= nScreenWidth || sy <= (0-height) || sy >= nScreenHeight) return;

	INT32 flip = 0;
	if (flipy) flip |= (height - 1) * width;
	if (flipx) flip |= width - 1;

	gfx += code * width * height;

	for (INT32 y = 0; y < height; y++, sy++) {
		if (sy < 0 || sy >= nScreenHeight) continue;

		for (INT32 x = 0; x < width; x++, sx++) {
			if (sx < 0 || sx >= nScreenWidth) continue;

			INT32 pxl = gfx[((y * width) + x) ^ flip];

			if (pxl == 0) continue;

			if ((prio & (1 << pri[sy * nScreenWidth + sx])) == 0) {
				dest[sy * nScreenWidth + sx] = pxl | color;
				pri[sy * nScreenWidth + sx] = 7;
			}
		}

		sx -= width;
	}
}

static void draw_sprites()
{
	INT32 pri_ctrl = rdVideoControl[14] & 0x03;
	UINT16 *sram = (UINT16*)(rdVidRAM + 0xee00);

	for (INT32 offs = 0x1f2/2; offs >= 0; offs -= 3)
	{
		INT32 sy    = BURN_ENDIAN_SWAP_INT16(sram[offs + 0]);
		INT32 code  = BURN_ENDIAN_SWAP_INT16(sram[offs + 1]);
		INT32 sx    = BURN_ENDIAN_SWAP_INT16(sram[offs + 2]);

		INT32 flipy = sy & 0x8000;
		INT32 flipx = sx & 0x0200;
		INT32 multi = 1 << ((sy & 0x6000) >> 13);
		INT32 color = (sy >> 9) & 0x0f;

		sx = ((sx - video_offsets[0]) & 0x1ff) - 16;
		sy = (512 - ((sy + video_offsets[1]) & 0x1ff)) - (16 * multi);

		INT32 prio = 0x82;
		if (pri_ctrl & 0x01) {
			if ((color & 0x08) == 0x08) prio = 0x80;
		} else if (pri_ctrl & 0x02) {
			if ((color & 0x0c) == 0x0c) prio = 0x80;
		}

		for (INT32 i = 0; i < multi; i++) {
			RenderTilePrio(pTransDraw, DrvGfxROM1, (code + (flipy ? multi-1 - i : i)) & code_mask[1], (color << 4)|0x100, sx, sy+i*16, flipx, flipy, 16, 16, RamPrioBitmap, prio);
		}
	}
}

#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
// (ReiREM PSP) Set of pens used per tile (64 pixels, bit = pen, values over 16 all mixed); built once on first draw
static UINT16 *m90_tpens;
static const UINT8 *m90_tpens_gfx;
static const UINT16 *m90_tile_pens(const UINT8 *gfx, INT32 ntiles)
{
	if (m90_tpens_gfx != gfx) {
		if (rei_render_noalloc) return NULL; // (PSP ME render: draw without the table; normally m90_prepare builds it ahead)
		BurnFree(m90_tpens);
		m90_tpens_gfx = NULL;
		m90_tpens = (UINT16 *)BurnMalloc(ntiles * sizeof(UINT16));
		if (!m90_tpens) return NULL;
		for (INT32 t = 0; t < ntiles; t++) {
			UINT32 pens = 0;
			for (INT32 i = 0; i < 0x40; i++) pens |= (gfx[t * 0x40 + i] < 16) ? (1u << gfx[t * 0x40 + i]) : 0xffffu;
			m90_tpens[t] = (UINT16)pens;
		}
		m90_tpens_gfx = gfx;
	}
	return m90_tpens;
}
static void m90_tile_pens_exit()
{
	BurnFree(m90_tpens);
	m90_tpens_gfx = NULL;
}
#endif

// (ReiREM) One 8-pixel row of an opaque tile; flip applied once (same result)
static inline void m90_put8(UINT16 *d, const UINT8 *rom, INT32 x_xor, INT32 color)
{
	if (x_xor) {
		d[0] = rom[7] | color; d[1] = rom[6] | color; d[2] = rom[5] | color; d[3] = rom[4] | color;
		d[4] = rom[3] | color; d[5] = rom[2] | color; d[6] = rom[1] | color; d[7] = rom[0] | color;
	} else {
		d[0] = rom[0] | color; d[1] = rom[1] | color; d[2] = rom[2] | color; d[3] = rom[3] | color;
		d[4] = rom[4] | color; d[5] = rom[5] | color; d[6] = rom[6] | color; d[7] = rom[7] | color;
	}
}

static void draw_layer(INT32 layer)
{
	INT32 control = rdVideoControl[10 + (layer * 2)];

	if (control & 0x10) return; // disable layer

	INT32 enable_rowscroll = control & 0x20;
	INT32 enable_colscroll = control & 0x40;

	INT32 wide = (control & 0x04) ? 128 : 64;

	INT32 trans = (layer) ? 0xff : 0;
	
	INT32 pmask = (wide == 128) ? 2 : 3;
	UINT16 *vram = (UINT16*)(rdVidRAM + (control & pmask) * 0x4000);

	UINT16 scrollx = ((rdVideoControl[(layer*4)+2] << 0) | (rdVideoControl[(layer*4)+3] << 8));
	UINT16 scrolly = ((rdVideoControl[(layer*4)+0] << 0) | (rdVideoControl[(layer*4)+1] << 8));

	if (enable_rowscroll) scrollx = 0;

	scrollx += (layer) ? -2 : 2;
	scrollx += ((wide & 0x80) * 2);

	UINT16 *xscroll = (UINT16*)(rdVidRAM + 0xf000 + (layer * 0x400));
	UINT16 *yscroll = (UINT16*)(rdVidRAM + 0xf800 + (layer * 0x400));

#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
	// (ReiREM PSP: without row/column scroll, draw each tile row (8 lines) cell by cell, using the tile pen table to skip empty cells and skip tests on full cells.
	//  Cells in one pass never overlap, so order does not affect the result)
	if (!enable_rowscroll && !enable_colscroll) {
		const UINT16 *tpens = m90_tile_pens(DrvGfxROM0, code_mask[0] + 1);
		const INT32 scrollx_1 = scrollx + video_offsets[0];
		for (INT32 sy0 = 0; sy0 < nScreenHeight;) {
			const INT32 scrolly_0 = (video_offsets[1] + scrolly + sy0) & 0x1ff;
			INT32 sy1 = sy0 + 8 - (scrolly_0 & 7);
			if (sy1 > nScreenHeight) sy1 = nScreenHeight;
			const INT32 rowbase = (scrolly_0 / 8) * wide;
			for (INT32 sx = 0; sx < nScreenWidth + 8; sx += 8) {
				const INT32 scrollx_2 = (scrollx_1 + sx) & ((wide * 8) - 1);
				const INT32 offs = rowbase | (scrollx_2 / 8);
				const INT32 code  = BURN_ENDIAN_SWAP_INT16(vram[offs * 2 + 0]) & code_mask[0];
				INT32 color = BURN_ENDIAN_SWAP_INT16(vram[offs * 2 + 1]);
				const INT32 flipy = color & 0x80, flipx = color & 0x40, group = (color & 0x30) ? 0 : 1;
				color = (color & 0x0f) << 4;
				const UINT32 tp = tpens ? tpens[code] : 0xffffu;
				if (trans == 0 && tp == 1) continue;                 // pen 0 only (transparent)
				// (layer 1 has trans 0xff: always opaque for 4-bit graphics; per pixel if the table is 0xffff (may be values over 16))
				const INT32 tile_opaque = trans != 0 ? (tp != 0xffffu) : !(tp & 1);
				const INT32 x_xor = flipx ? 7 : 0;
				const INT32 xx0 = sx - (scrollx_2 & 0x7);
				const INT32 inside = xx0 >= 0 && xx0 + 8 <= nScreenWidth;
				const UINT8 pv = 1 << group;
				for (INT32 sy = sy0; sy < sy1; sy++) {
					INT32 romoff = ((video_offsets[1] + scrolly + sy) & 0x07) << 3;
					if (flipy) romoff ^= 0x38;
					const UINT8 *rom = DrvGfxROM0 + (code * 0x40) + romoff;
					UINT16 *dest = pTransDraw + (sy * nScreenWidth);
					UINT8 *pri = RamPrioBitmap + (sy * nScreenWidth);
					if (inside && tile_opaque) {
						m90_put8(dest + xx0, rom, x_xor, color);
						memset(pri + xx0, pv, 8);
						continue;
					}
					for (INT32 x = 0, xx = xx0; x < 8; x++, xx++) {
						if (xx < 0 || xx >= nScreenWidth) continue;
						const INT32 pxl = rom[x ^ x_xor];
						if (pxl == trans) continue;
						dest[xx] = pxl | color;
						pri[xx] = pv;
					}
				}
			}
			sy0 = sy1;
		}
		return;
	}
#endif

#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
	// (ReiREM PSP: tile pen table on the row/column scroll path too; skip empty cells, no tests on full cells inside the screen. Same result)
	const UINT16 *tpens_r = m90_tile_pens(DrvGfxROM0, code_mask[0] + 1);
#else
	const UINT16 *tpens_r = NULL;
#endif
	for (INT32 sy = 0; sy < nScreenHeight; sy++)
	{
		INT32 scrollx_1 = scrollx + video_offsets[0];
		INT32 scrolly_1 = video_offsets[1];
		UINT16 *dest = pTransDraw + (sy * nScreenWidth);
		UINT8 *pri = RamPrioBitmap + (sy * nScreenWidth);

		if (enable_rowscroll) {
			scrollx_1 = (scrollx_1 + BURN_ENDIAN_SWAP_INT16(xscroll[sy])) & ((wide * 8) - 1);
		}

		if (enable_colscroll) {
			scrolly_1 = (scrolly_1 + sy + BURN_ENDIAN_SWAP_INT16(yscroll[sy + video_offsets[1]]) + 0x200) & 0x1ff;
		} else {
			scrolly_1 = (scrolly_1 + scrolly + sy) & 0x1ff;
		}

		INT32 romoff_1 = (scrolly_1 & 0x07) << 3;
		
		for (INT32 sx = 0; sx < nScreenWidth + 8; sx+=8)
		{
			INT32 scrollx_2 = (scrollx_1 + sx) & ((wide * 8) - 1);

			INT32 offs = ((scrolly_1 / 8) * wide) | (scrollx_2 / 8);

			INT32 code  = BURN_ENDIAN_SWAP_INT16(vram[offs * 2 + 0]) & code_mask[0];
			INT32 color = BURN_ENDIAN_SWAP_INT16(vram[offs * 2 + 1]);

			INT32 flipy = color & 0x80;
			INT32 flipx = color & 0x40;
			INT32 group =(color & 0x30) ? 0 : 1;
			color &= 0x0f;

			{
				color <<= 4;
				INT32 x_xor = 0;
				INT32 romoff = romoff_1;
				if (flipy) romoff ^= 0x38;
				if (flipx) x_xor = 7;

				UINT8 *rom = DrvGfxROM0 + (code * 0x40) + romoff;

				INT32 xx = sx - (scrollx_2&0x7);

				if (tpens_r) {
					const UINT32 tp = tpens_r[code];
					if (trans == 0 && tp == 1) continue; // (pen 0 only: transparent)
					const INT32 tile_opaque = trans != 0 ? (tp != 0xffffu) : !(tp & 1);
					if (tile_opaque && xx >= 0 && xx + 8 <= nScreenWidth) {
						m90_put8(dest + xx, rom, x_xor, color);
						memset(pri + xx, 1 << group, 8);
						continue;
					}
				}
				for (INT32 x = 0; x < 8; x++, xx++) {
					if (xx < 0 || xx >= nScreenWidth) continue;

					INT32 pxl = rom[x ^ x_xor];
					if (pxl == trans) continue;

					dest[xx] = pxl | color;
					pri[xx] = 1 << group;
				}
			}
		}
	}
}

static INT32 DrvDraw()
{
	if (DrvRecalc) {
		for (INT32 i = 0; i < 0x400; i+=2) {
			update_palette_entry(i);
		}
		DrvRecalc = 0;
	}

#ifndef REI_ORIGINAL_DRAW
	if (rei_defer_on) { // render thread draws: capture frame state
		rei_defer_frame();
		return 0;
	}
#endif
	rd_live();
	DrvDraw_now();
	return 0;
}

static void DrvDraw_now()
{
	if ((nBurnLayer & 0xf) != 0xf) BurnTransferClear();

	if ((rdVideoControl[14] & 0x04) == 0) { // video enable
		REI_PROF_BEGIN(layers);
		memset (RamPrioBitmap, 0, nScreenWidth * nScreenHeight);
		if (rdVideoControl[12] & 0x10) { // pf2 enable
			BurnTransferClear();
		} else {
			if (nBurnLayer & 1) draw_layer(1);
		}

		if (nBurnLayer & 2) draw_layer(0);
		REI_PROF_END(layers, REI_PROF_LAYERS);

		REI_PROF_BEGIN(sprites);
		if (nBurnLayer & 4) draw_sprites();
		REI_PROF_END(sprites, REI_PROF_SPRITES);
	} else {
		BurnTransferClear();
	}

	BurnTransferCopy(rdPalette);
}

static void compile_inputs()
{
	memset (DrvInputs, 0xff, 5);

	for (INT32 i = 0; i < 8; i++) {
		DrvInputs[0] ^= (DrvJoy1[i] & 1) << i;
		DrvInputs[1] ^= (DrvJoy2[i] & 1) << i;
		DrvInputs[2] ^= (DrvJoy3[i] & 1) << i;
		DrvInputs[3] ^= (DrvJoy4[i] & 1) << i;
		DrvInputs[4] ^= (DrvJoy5[i] & 1) << i;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// (ReiREM: batched execution) Run V35 for all frame slices first (two VezRun calls around vblank), then Z80, removing the cache thrash
// of alternating CPUs (same idea as d_m72.cpp m72b_*). V35 -> Z80 is only the sound latch (port 0x00); there is no Z80 -> V35.
// Latch writes are queued with their original slice number (first slice whose end target > insn start cycle S) and applied before that Z80 slice.
// Drawing (after slice 120), YM2151, DAC and NMI keep their original order
#define M90B_MAX 4096
unsigned int v25_ins_start_cycles();
static INT32 m90b_on, m90b_merged, m90b_slice, m90b_gend, m90b_done0, m90b_inter, m90b_total0, m90b_total1;
static INT32 m90b_n, m90b_pos, m90b_z80_done, m90b_z80_cyc, m90b_snd_pos;
static UINT32 m90b_t0;
static UINT8 m90b_data[M90B_MAX];
static INT32 m90b_ev[M90B_MAX];
static INT32 m90b_cur()
{
	if (m90b_merged) {
		const INT32 S = m90b_done0 + (INT32)(v25_ins_start_cycles() - m90b_t0);
		while (m90b_slice < m90b_gend - 1 && S >= (m90b_slice + 1) * m90b_total0 / m90b_inter)
			m90b_slice++;
	}
	return m90b_slice;
}
static void m90b_apply_upto(INT32 slice)
{
	while (m90b_pos < m90b_n && m90b_ev[m90b_pos] <= slice) {
		*soundlatch = m90b_data[m90b_pos++];
		setvector_callback(Z80_ASSERT);
	}
}
// YM2151 render at the end of slice k (original loop: fixed length every two slices; timers count inside synthesis)
static void m90b_ym_part(INT32 k)
{
	if (pBurnSoundOut && (k & 1)) {
		const INT32 nSegmentLength = nBurnSoundLen / (m90b_inter / 2);
		INT16* pSoundBuf = pBurnSoundOut + (m90b_snd_pos << 1);
		BurnYM2151Render(pSoundBuf, nSegmentLength);
		m90b_snd_pos += nSegmentLength;
	}
}
// Work at the end of slice k (where the Z80 stopped): sample or NMI, then YM
static void m90b_slice_end(INT32 k)
{
	if (is_bbmanw) {
		if (DrvSndROM[sample_address & 0x3ffff]) {
			DACSignedWrite(0, DrvSndROM[sample_address & 0x3ffff]);
			sample_address = (sample_address + 1) & 0x3ffff;
		}
	} else {
		ZetNmi();
	}
	m90b_ym_part(k);
}
// (ReiREM: slice merging, same idea as d_m72 m72b_ev) Slices without V30 writes run the Z80 in one go, doing each slice-end task via an
// event right after the insn that crosses that slice end. NMI is taken there; the YM render that followed ZetNmi happens after the two
// insns following the NMI (z80_nmi_post_cb). Also emulates the late-visible cycles during ZetNmi's deferred cycles (z80_nmi_lag, until the next slice end).
// Disable with harness REI_NO_Z80EV
static INT32 m90b_ev_on = 1, m90b_ev_k, m90b_ev_last, m90b_ev_defer, m90b_ev_base, m90b_post_k;
static struct m90b_ev_env { m90b_ev_env() { if (getenv("REI_NO_Z80EV")) m90b_ev_on = 0; } } m90b_ev_env_init;
static void m90b_ev_fire();
static void m90b_ev_arm()
{
	while (m90b_ev_k <= m90b_ev_last) {
		const INT32 t = m90b_ev_base + (m90b_ev_k + 1) * m90b_total1 / m90b_inter;
		if (t > ZetEventNow()) { ZetEventAt(t, m90b_ev_fire); return; }
		m90b_slice_end(m90b_ev_k++); // (already passed: originally done immediately without running)
	}
	ZetEventClear();
}
static void m90b_post() { m90b_ym_part(m90b_post_k); }
static void m90b_ev_fire()
{
	if (Z80RunEnding()) { // this insn ends the run: do it the original way outside the run
		m90b_ev_defer = 1;
		ZetEventClear();
		return;
	}
	const INT32 k = m90b_ev_k++;
	Z80NmiLagEnd(); // (originally the first run after the previous NMI ended here)
	ZetDelayedSettle();
	if (is_bbmanw) {
		m90b_slice_end(k); // (DAC sample, YM: same spot even mid-run)
	} else {
		Z80TakeNmiNow();
		m90b_post_k = k;
		z80_nmi_post_cb = m90b_post;
	}
	m90b_ev_arm();
}
// Run Z80 slices [m90b_z80_done, end) exactly as the Z80 part of the original loop
static void m90b_z80_run_to(INT32 end)
{
	const INT32 nInterleave = m90b_inter;
	INT32 nCyclesTotal[2] = { 0, m90b_total1 };
	INT32 nCyclesDone[2] = { 0, m90b_z80_cyc };
	for (INT32 i = m90b_z80_done; i < end; i++) {
		m90b_apply_upto(i);
		INT32 j = i;
		if (m90b_ev_on)
			while (j < end - 1 && !(m90b_pos < m90b_n && m90b_ev[m90b_pos] <= j + 1))
				j++;
		if (j > i) {
			m90b_ev_base = ZetTotalCycles() - nCyclesDone[1]; // (outside the run: offset between real cycles and slice base)
			m90b_ev_k = i;
			m90b_ev_last = j - 1;
			m90b_ev_defer = 0;
			m90b_ev_arm();
			nCyclesDone[1] += ZetRun(((j + 1) * nCyclesTotal[1] / nInterleave) - nCyclesDone[1]);
			ZetEventClear();
			z80_nmi_post_cb = NULL;
			if (m90b_ev_defer) { m90b_ev_defer = 0; m90b_slice_end(m90b_ev_k++); }
			while (m90b_ev_k <= m90b_ev_last) m90b_slice_end(m90b_ev_k++); // (leftovers; should be none)
			i = j;
		} else {
			CPU_RUN(1, Zet);
		}
		m90b_slice_end(i);
	}
	m90b_z80_cyc = nCyclesDone[1];
	if (end > m90b_z80_done) m90b_z80_done = end;
}
static INT32 m90b_push(UINT8 data)
{
	if (!m90b_on) return 0;
	if (m90b_n >= M90B_MAX) { // (queue full) catch the Z80 up to just before this slice and drain; same order
		const INT32 s = m90b_cur();
		m90b_z80_run_to(s);
		m90b_apply_upto(s);
		m90b_n = m90b_pos = 0;
	}
	m90b_data[m90b_n] = data;
	m90b_ev[m90b_n++] = m90b_cur();
	return 1;
}
// ---------------------------------------------------------------------------------------------------------------------

static INT32 DrvFrame()
{
	if (DrvReset) {
		DrvDoReset();
	}

	compile_inputs();

	INT32 nInterleave = 128; // nmi pulses for sound cpu
	INT32 nCyclesTotal[2];
	INT32 nCyclesDone[2];
	INT32 nSoundBufferPos = 0;

	// overclocking...
	nCyclesTotal[0] = (INT32)((INT64)(8000000 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesTotal[1] = (INT32)((INT64)(3579545 / 60) * nBurnCPUSpeedAdjust / 0x0100);
	nCyclesDone[0] = nCyclesDone[1] = 0;

	VezNewFrame();
	ZetNewFrame();

	VezOpen(0);
	ZetOpen(0);

	vblank = 0;

#ifndef REI_NO_BATCH
	{ // (ReiREM: batched execution, see m90b_*)
		m90b_inter = nInterleave;
		m90b_total0 = nCyclesTotal[0];
		m90b_total1 = nCyclesTotal[1];
		m90b_n = m90b_pos = m90b_z80_done = 0;
		m90b_z80_cyc = 0;
		m90b_snd_pos = 0;
		m90b_on = 1;
		ZetIdleKeepAcrossRuns(1); // (memory seen by the Z80 is not changed externally: the latch is a port)
		REI_PROF_BEGIN(ph1);
		// V35: slices 0~120 (then vblank) and 121~127, each in one VezRun
		for (INT32 part = 0; part < 2; part++) {
			const INT32 s0 = part ? 121 : 0, gend = part ? nInterleave : 121;
			const INT32 todo = gend * nCyclesTotal[0] / nInterleave - nCyclesDone[0];
			if (todo > 0) {
				m90b_slice = s0;
				m90b_gend = gend;
				m90b_done0 = nCyclesDone[0];
				m90b_t0 = (UINT32)VezTotalCycles();
				m90b_merged = 1;
				nCyclesDone[0] += VezRun(todo);
				m90b_merged = 0;
			}
			if (part == 0) {
				m90b_slice = 120;
				if (rei_defer_on || pBurnDraw) {
					DrvDraw();
				}
				vblank = 0x80;
				VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff, CPU_IRQSTATUS_ACK);
				VezRun(0);
				VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff, CPU_IRQSTATUS_NONE);
			}
		}
		m90b_slice = nInterleave;
		REI_PROF_END(ph1, REI_PROF_V33);
		REI_PROF_BEGIN(ph2);
		m90b_z80_run_to(nInterleave);
		REI_PROF_END(ph2, REI_PROF_V35);
		m90b_on = 0;
		ZetIdleKeepAcrossRuns(0);
		nSoundBufferPos = m90b_snd_pos;
	} if (0)
#endif
	for (INT32 i = 0; i < nInterleave; i++)
	{
		CPU_RUN(0, Vez);

		if (i == 120)
		{
			if (rei_defer_on || pBurnDraw) {
				DrvDraw();
			}

			vblank = 0x80;
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff, CPU_IRQSTATUS_ACK);
			VezRun(0);
			VezSetIRQLineAndVector(NEC_INPUT_LINE_INTP0, 0xff, CPU_IRQSTATUS_NONE);
		}

		CPU_RUN(1, Zet);

		if (is_bbmanw) {
			if (DrvSndROM[sample_address & 0x3ffff]) {
				DACSignedWrite(0, DrvSndROM[sample_address & 0x3ffff]);
				sample_address = (sample_address + 1) & 0x3ffff;
			}
		} else {
			ZetNmi();
		}

		if (pBurnSoundOut && i&1) {
			INT32 nSegmentLength = nBurnSoundLen / (nInterleave / 2);
			INT16* pSoundBuf = pBurnSoundOut + (nSoundBufferPos << 1);
			BurnYM2151Render(pSoundBuf, nSegmentLength);
			nSoundBufferPos += nSegmentLength;
		}
	}

	if (pBurnSoundOut) {
		INT32 nSegmentLength = nBurnSoundLen - nSoundBufferPos;
		INT16* pSoundBuf = pBurnSoundOut + (nSoundBufferPos << 1);
		if (nSegmentLength) {
			BurnYM2151Render(pSoundBuf, nSegmentLength);
		}
		DACUpdate(pBurnSoundOut, nBurnSoundLen);
	}

	VezClose();
	ZetClose();

	return 0;
}

static INT32 DrvScan(INT32 nAction, INT32 *pnMin)
{
	struct BurnArea ba;
	
	if (pnMin != NULL) {			// Return minimum compatible version
		*pnMin = 0x029705;
	}

	if (nAction & ACB_MEMORY_RAM) {
		memset(&ba, 0, sizeof(ba));
		ba.Data	  = AllRam;
		ba.nLen	  = RamEnd-AllRam;
		ba.szName = "All Ram";
		BurnAcb(&ba);
	}

	if (nAction & ACB_DRIVER_DATA) {
		VezScan(nAction);
		ZetScan(nAction);

		BurnYM2151Scan(nAction, pnMin);
		DACScan(nAction, pnMin);

		SCAN_VAR(sample_address);
		SCAN_VAR(irqvector);
	}

	return 0;
}


// Hasamu (Japan)

static struct BurnRomInfo hasamuRomDesc[] = {
	{ "hasc-p1.ic62",	0x20000, 0x53df9834, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "hasc-p0.ic65",	0x20000, 0xdff0ba6e, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "hasc-sp.ic23",	0x10000, 0x259b1687, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "hasc-c0.ic66",	0x20000, 0xdd5a2174, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "hasc-c1.ic67",	0x20000, 0x76b8217c, 3 | BRF_GRA },           //  4
	{ "hasc-c2.ic68",	0x20000, 0xd90f9a68, 3 | BRF_GRA },           //  5
	{ "hasc-c3.ic69",	0x20000, 0x6cfe0d39, 3 | BRF_GRA },           //  6
};

STD_ROM_PICK(hasamu)
STD_ROM_FN(hasamu)

static INT32 hasamuInit()
{
	video_offsets[0] = 48;
	video_offsets[1] = 136;
	return DrvInit(0x40000, 0x80000, 0x00000, 0, 0, gunforce_decryption_table);
}

struct BurnDriver BurnDrvHasamu = {
	"hasamu", NULL, NULL, NULL, "1991",
	"Hasamu (Japan)\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAHJONG, 0,
	NULL, hasamuRomInfo, hasamuRomName, NULL, NULL, NULL, NULL, p2commonInputInfo, HasamuDIPInfo,
	hasamuInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	384, 240, 4, 3
};


// Dynablaster / Bomber Man

static struct BurnRomInfo dynablstRomDesc[] = {
	{ "bbm-cp1e.ic62",	0x20000, 0x27667681, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm-cp0e.ic65",	0x20000, 0x95db7a67, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm-sp.ic23",	0x10000, 0x251090cd, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm-c0.ic66",	0x40000, 0x695d2019, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm-c1.ic67",	0x40000, 0x4c7c8bbc, 3 | BRF_GRA },           //  4
	{ "bbm-c2.ic68",	0x40000, 0x0700d406, 3 | BRF_GRA },           //  5
	{ "bbm-c3.ic69",	0x40000, 0x3c3613af, 3 | BRF_GRA },           //  6

	{ "bbm-v0.ic20",	0x20000, 0x0fa803fe, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(dynablst)
STD_ROM_FN(dynablst)

static INT32 dynablstInit()
{
	video_offsets[0] = 80;
	video_offsets[1] = 136;
	return DrvInit(0x40000, 0x100000, 0x20000, 0, 0, bomberman_decryption_table);
}

struct BurnDriver BurnDrvDynablst = {
	"dynablst", NULL, NULL, NULL, "1991",
	"Dynablaster / Bomber Man\0", NULL, "Irem (licensed from Hudson Soft)", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, dynablstRomInfo, dynablstRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, DynablstDIPInfo,
	dynablstInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Bomber Man (Japan)

static struct BurnRomInfo bombrmanRomDesc[] = {
	{ "bbm-p1.ic62",	0x20000, 0x982bd166, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm-p0.ic65",	0x20000, 0x0a20afcc, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm-sp.ic23",	0x10000, 0x251090cd, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm-c0.ic66",	0x40000, 0x695d2019, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm-c1.ic67",	0x40000, 0x4c7c8bbc, 3 | BRF_GRA },           //  4
	{ "bbm-c2.ic68",	0x40000, 0x0700d406, 3 | BRF_GRA },           //  5
	{ "bbm-c3.ic69",	0x40000, 0x3c3613af, 3 | BRF_GRA },           //  6

	{ "bbm-v0.ic20",	0x20000, 0x0fa803fe, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(bombrman)
STD_ROM_FN(bombrman)

struct BurnDriver BurnDrvBombrman = {
	"bombrman", "dynablst", NULL, NULL, "1991",
	"Bomber Man (Japan)\0", NULL, "Irem (licensed from Hudson Soft)", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, bombrmanRomInfo, bombrmanRomName, NULL, NULL, NULL, NULL, p2commonInputInfo, BombrmanDIPInfo,
	dynablstInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Atomic Punk (US)

static struct BurnRomInfo atompunkRomDesc[] = {
	{ "bbm-cp0d.ic65",	0x20000, 0x860c0479, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm-cp1d.ic62",	0x20000, 0xbe57bf74, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm-sp.ic23",	0x10000, 0x251090cd, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm-c0.ic66",	0x40000, 0x695d2019, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm-c1.ic67",	0x40000, 0x4c7c8bbc, 3 | BRF_GRA },           //  4
	{ "bbm-c2.ic68",	0x40000, 0x0700d406, 3 | BRF_GRA },           //  5
	{ "bbm-c3.ic69",	0x40000, 0x3c3613af, 3 | BRF_GRA },           //  6

	{ "bbm-v0.20",		0x20000, 0x0fa803fe, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(atompunk)
STD_ROM_FN(atompunk)

struct BurnDriver BurnDrvAtompunk = {
	"atompunk", "dynablst", NULL, NULL, "1991",
	"Atomic Punk (US)\0", NULL, "Irem America (licensed from Hudson Soft)", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 4, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, atompunkRomInfo, atompunkRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, AtompunkDIPInfo,
	dynablstInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Bomber Man World / New Dyna Blaster - Global Quest

static struct BurnRomInfo bbmanwRomDesc[] = {
	{ "bbm2-h0-b.ic77",	0x40000, 0x567d3709, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm2-l0-b.ic79",	0x40000, 0xe762c22b, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm2-sp.ic33",	0x10000, 0x6bc1689e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm2-c0.ic81",	0x40000, 0xe7ce058a, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm2-c1.ic82",	0x40000, 0x636a78a9, 3 | BRF_GRA },           //  4
	{ "bbm2-c2.ic83",	0x40000, 0x9ac2142f, 3 | BRF_GRA },           //  5
	{ "bbm2-c3.ic84",	0x40000, 0x47af1750, 3 | BRF_GRA },           //  6

	{ "bbm2-v0.ic30",	0x20000, 0x4ad889ed, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(bbmanw)
STD_ROM_FN(bbmanw)

static INT32 bbmanwInit()
{
	video_offsets[0] = 80;
	video_offsets[1] = 136;
	is_bbmanw = 1;

	return DrvInit(0x80000, 0x100000, 0x20000, 0, 0, dynablaster_decryption_table);
}

static INT32 bbmanwjaInit()
{
	video_offsets[0] = 80;
	video_offsets[1] = 136;

	return DrvInit(0x80000, 0x100000, 0x20000, 0, 0, dynablaster_decryption_table);
}

struct BurnDriver BurnDrvBbmanw = {
	"bbmanw", NULL, NULL, NULL, "1992",
	"Bomber Man World / New Dyna Blaster - Global Quest\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, bbmanwRomInfo, bbmanwRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, BbmanwDIPInfo,
	bbmanwInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Bomber Man World (Japan)

static struct BurnRomInfo bbmanwjRomDesc[] = {
	{ "bbm2-h0.ic77",	0x40000, 0xe1407b91, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm2-l0.ic79",	0x40000, 0x20873b49, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm2-sp-a.ic33",	0x10000, 0xa4b0a66e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm2-c0.ic81",	0x40000, 0xe7ce058a, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm2-c1.ic82",	0x40000, 0x636a78a9, 3 | BRF_GRA },           //  4
	{ "bbm2-c2.ic83",	0x40000, 0x9ac2142f, 3 | BRF_GRA },           //  5
	{ "bbm2-c3.ic84",	0x40000, 0x47af1750, 3 | BRF_GRA },           //  6

	{ "bbm2-v0-b.ic30",	0x20000, 0x0ae655ff, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(bbmanwj)
STD_ROM_FN(bbmanwj)

struct BurnDriver BurnDrvBbmanwj = {
	"bbmanwj", "bbmanw", NULL, NULL, "1992",
	"Bomber Man World (Japan)\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, bbmanwjRomInfo, bbmanwjRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, BbmanwjDIPInfo,
	bbmanwInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Bomber Man World (Japan, revised sound hardware)

static struct BurnRomInfo bbmanwjaRomDesc[] = {
	{ "bbm2-h0.ic77",	0x40000, 0xe1407b91, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm2-l0.ic79",	0x40000, 0x20873b49, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm2-sp-b.ic33",	0x10000, 0xb8d8108c, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm2-c0.ic81",	0x40000, 0xe7ce058a, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm2-c1.ic82",	0x40000, 0x636a78a9, 3 | BRF_GRA },           //  4
	{ "bbm2-c2.ic83",	0x40000, 0x9ac2142f, 3 | BRF_GRA },           //  5
	{ "bbm2-c3.ic84",	0x40000, 0x47af1750, 3 | BRF_GRA },           //  6

	{ "bbm2-v0-b.ic30",	0x20000, 0x0ae655ff, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(bbmanwja)
STD_ROM_FN(bbmanwja)

struct BurnDriver BurnDrvBbmanwja = {
	"bbmanwja", "bbmanw", NULL, NULL, "1992",
	"Bomber Man World (Japan, revised sound hardware)\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, bbmanwjaRomInfo, bbmanwjaRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, BbmanwjDIPInfo,
	bbmanwjaInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// New Atomic Punk - Global Quest (US)

static struct BurnRomInfo newapunkRomDesc[] = {
	{ "bbm2-h0-a.ic77",	0x40000, 0x7d858682, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "bbm2-l0-a.ic79",	0x40000, 0xc7568031, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "bbm2-sp.ic33",	0x10000, 0x6bc1689e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "bbm2-c0.ic81",	0x40000, 0xe7ce058a, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "bbm2-c1.ic82",	0x40000, 0x636a78a9, 3 | BRF_GRA },           //  4
	{ "bbm2-c2.ic83",	0x40000, 0x9ac2142f, 3 | BRF_GRA },           //  5
	{ "bbm2-c3.ic84",	0x40000, 0x47af1750, 3 | BRF_GRA },           //  6

	{ "bbm2-v0.ic30",	0x20000, 0x4ad889ed, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(newapunk)
STD_ROM_FN(newapunk)

struct BurnDriver BurnDrvNewapunk = {
	"newapunk", "bbmanw", NULL, NULL, "1992",
	"New Atomic Punk - Global Quest (US)\0", NULL, "Irem America", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_MAZE, 0,
	NULL, newapunkRomInfo, newapunkRomName, NULL, NULL, NULL, NULL, p4commonInputInfo, BbmanwjDIPInfo,
	bbmanwInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Quiz F1 1-2 Finish (Japan)

static struct BurnRomInfo quizf1RomDesc[] = {
	{ "qf1-h0-.ic77",	0x40000, 0x280e3049, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "qf1-l0-.ic79",	0x40000, 0x94588a6f, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "qf1-sp-.ic33",	0x10000, 0x0664fa9f, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "qf1-c0-.ic81",	0x80000, 0xc26b521e, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "qf1-c1-.ic82",	0x80000, 0xdb9d7394, 3 | BRF_GRA },           //  4
	{ "qf1-c2-.ic83",	0x80000, 0x0b1460ae, 3 | BRF_GRA },           //  5
	{ "qf1-c3-.ic84",	0x80000, 0x2d32ff37, 3 | BRF_GRA },           //  6

	{ "qf1-v0-.ic30",	0x40000, 0xb8d16e7c, 4 | BRF_SND },           //  7 Samples

	{ "qf1-h1-.ic78",	0x80000, 0xc6c2eb2b, 5 | BRF_PRG | BRF_ESS }, //  8 V30 banked code
	{ "qf1-l1-.ic80",	0x80000, 0x3132c144, 5 | BRF_PRG | BRF_ESS }, //  9
};

STD_ROM_PICK(quizf1)
STD_ROM_FN(quizf1)

static INT32 quizf1Init()
{
	video_offsets[0] = 48;
	video_offsets[1] = 128;
	return DrvInit(0x80000, 0x200000, 0x40000, 0x100000, 0, lethalth_decryption_table);
}

struct BurnDriver BurnDrvQuizf1 = {
	"quizf1", NULL, NULL, NULL, "1992",
	"Quiz F1 1-2 Finish (Japan)\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING, 2, HARDWARE_IREM_M90, GBF_QUIZ, 0,
	NULL, quizf1RomInfo, quizf1RomName, NULL, NULL, NULL, NULL, Quizf1InputInfo, Quizf1DIPInfo,
	quizf1Init, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	384, 240, 4, 3
};


// Risky Challenge

static struct BurnRomInfo riskchalRomDesc[] = {
	{ "l4-a-h0-b.ic77",	0x40000, 0x4c9b5344, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "l4-a-l0-b.ic79",	0x40000, 0x0455895a, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "l4_a-sp.ic33",	0x10000, 0xbb80094e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "rc_c0.ic81",		0x80000, 0x84d0b907, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "rc_c1.ic82",		0x80000, 0xcb3784ef, 3 | BRF_GRA },           //  4
	{ "rc_c2.ic83",		0x80000, 0x687164d7, 3 | BRF_GRA },           //  5
	{ "rc_c3.ic84",		0x80000, 0xc86be6af, 3 | BRF_GRA },           //  6

	{ "rc_v0.ic30",		0x40000, 0xcddac360, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(riskchal)
STD_ROM_FN(riskchal)

static INT32 riskchalInit()
{
	video_offsets[0] = 80;
	video_offsets[1] = 136;

	return DrvInit(0x80000, 0x200000, 0x40000, 0, 0, gussun_decryption_table);
}

struct BurnDriver BurnDrvRiskchal = {
	"riskchal", NULL, NULL, NULL, "1993",
	"Risky Challenge\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_PUZZLE, 0,
	NULL, riskchalRomInfo, riskchalRomName, NULL, NULL, NULL, NULL, p2commonInputInfo, RiskchalDIPInfo,
	riskchalInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Gussun Oyoyo (Japan)

static struct BurnRomInfo gussunRomDesc[] = {
	{ "l4_a-h0.ic77",	0x40000, 0x9d585e61, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "l4_a-l0.ic79",	0x40000, 0xc7b4c519, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "l4_a-sp.ic33",	0x10000, 0xbb80094e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "rc_c0.ic81",		0x80000, 0x84d0b907, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "rc_c1.ic82",		0x80000, 0xcb3784ef, 3 | BRF_GRA },           //  4
	{ "rc_c2.ic83",		0x80000, 0x687164d7, 3 | BRF_GRA },           //  5
	{ "rc_c3.ic84",		0x80000, 0xc86be6af, 3 | BRF_GRA },           //  6

	{ "rc_v0.ic30",		0x40000, 0xcddac360, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(gussun)
STD_ROM_FN(gussun)

struct BurnDriver BurnDrvGussun = {
	"gussun", "riskchal", NULL, NULL, "1993",
	"Gussun Oyoyo (Japan)\0", NULL, "Irem", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_PUZZLE, 0,
	NULL, gussunRomInfo, gussunRomName, NULL, NULL, NULL, NULL, p2commonInputInfo, RiskchalDIPInfo,
	riskchalInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};


// Match It II

static struct BurnRomInfo matchit2RomDesc[] = {
	{ "sis2-h0-b.ic77",	0x40000, 0x9a2556ac, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "sis2-l0-b.ic79",	0x40000, 0xd35d948a, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "sis2-sp-.ic33",	0x10000, 0x6fc0ff3a, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "sis2_c0.ic81",	0x80000, 0x5a7cb88f, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "sis2_c1.ic82",	0x80000, 0x54a7852c, 3 | BRF_GRA },           //  4
	{ "sis2_c2.ic83",	0x80000, 0x2bd65dc6, 3 | BRF_GRA },           //  5
	{ "sis2_c3.ic84",	0x80000, 0x876d5fdb, 3 | BRF_GRA },           //  6
};

STD_ROM_PICK(matchit2)
STD_ROM_FN(matchit2)

static INT32 matchit2Init()
{
	video_offsets[0] = 48;
	video_offsets[1] = 128;
	return DrvInit(0x80000, 0x200000, 0, 0, 0, matchit2_decryption_table);
}

struct BurnDriver BurnDrvMatchit2 = {
	"matchit2", NULL, NULL, NULL, "1993",
	"Match It II\0", NULL, "Tamtex", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING, 2, HARDWARE_IREM_M90, GBF_PUZZLE, 0,
	NULL, matchit2RomInfo, matchit2RomName, NULL, NULL, NULL, NULL, Matchit2InputInfo, Matchit2DIPInfo,
	matchit2Init, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	384, 256, 4, 3
};


// Shisensho II

static struct BurnRomInfo shisen2RomDesc[] = {
	{ "sis2-h0-.rom",	0x40000, 0x6fae0aea, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "sis2-l0-.rom",	0x40000, 0x2af25182, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "sis2-sp-.rom",	0x10000, 0x6fc0ff3a, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "sis2_c0.ic81",	0x80000, 0x5a7cb88f, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "sis2_c1.ic82",	0x80000, 0x54a7852c, 3 | BRF_GRA },           //  4
	{ "sis2_c2.ic83",	0x80000, 0x2bd65dc6, 3 | BRF_GRA },           //  5
	{ "sis2_c3.ic84",	0x80000, 0x876d5fdb, 3 | BRF_GRA },           //  6
};

STD_ROM_PICK(shisen2)
STD_ROM_FN(shisen2)

struct BurnDriver BurnDrvShisen2 = {
	"shisen2", "matchit2", NULL, NULL, "1993",
	"Shisensho II\0", NULL, "Tamtex", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_CLONE, 2, HARDWARE_IREM_M90, GBF_PUZZLE, 0,
	NULL, shisen2RomInfo, shisen2RomName, NULL, NULL, NULL, NULL, Matchit2InputInfo, Shisen2DIPInfo,
	matchit2Init, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	384, 256, 4, 3
};

// Dice - The Dice Game!

static struct BurnRomInfo dicegameRomDesc[] = {
	{ "dice-p1.ic61",	0x20000, 0x8f2257d8, 1 | BRF_PRG | BRF_ESS }, //  0 V30 Code
	{ "dice-p0.ic65",	0x20000, 0x9c191d18, 1 | BRF_PRG | BRF_ESS }, //  1

	{ "dice-sp.ic23",	0x20000, 0x73e8796e, 2 | BRF_PRG | BRF_ESS }, //  2 Z80 Code

	{ "dice-c0.ic66",	0x20000, 0xa1eda342, 3 | BRF_GRA },           //  3 Tiles and sprites
	{ "dice-c1.ic67",	0x20000, 0x20850a15, 3 | BRF_GRA },           //  4
	{ "dice-c2.ic68",	0x20000, 0x6c39915f, 3 | BRF_GRA },           //  5
	{ "dice-c3.ic69",	0x20000, 0x81d58e68, 3 | BRF_GRA },           //  6

	{ "dice-v0.ic20",	0x20000, 0x04dc9196, 4 | BRF_SND },           //  7 Samples
};

STD_ROM_PICK(dicegame)
STD_ROM_FN(dicegame)

static INT32 DicegameInit()
{
	video_offsets[0] = 80;
	video_offsets[1] = 136;

	return DrvInit(0x40000, 0x80000, 0x20000, 0, 0, NULL);
}

struct BurnDriver BurnDrvDicegame = {
	"dicegame", NULL, NULL, NULL, "1991",
	"Dice - The Dice Game!\0", NULL, "bootleg (Tuning)", "Irem M90",
	NULL, NULL, NULL, NULL,
	BDF_GAME_WORKING | BDF_BOOTLEG | BDF_HISCORE_SUPPORTED, 2, HARDWARE_IREM_M90, GBF_PUZZLE, 0,
	NULL, dicegameRomInfo, dicegameRomName, NULL, NULL, NULL, NULL, p2commonInputInfo, DicegameDIPInfo,
	DicegameInit, DrvExit, DrvFrame, DrvDraw, DrvScan, &DrvRecalc, 0x200,
	320, 240, 4, 3
};

static void m90_prepare() // (ReiREM: build the tile pen table used by the render on main ahead, same args as drawing; PSP ME render cannot allocate)
{
#if !defined(REI_ORIGINAL_DRAW) && !defined(__ARM_NEON)
	m90_tile_pens(DrvGfxROM0, code_mask[0] + 1);
#endif
}
