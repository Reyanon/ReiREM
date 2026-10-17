// Tile layers -> GE draw lists (psp/docs/M92_VIDEO.md G2). Platform-independent C — shared by the PSP renderer and the PC model
// (psp/tools/ge_model).
//
// Takes the core's segment callback (rei_layer_seg) and builds a "run" list. Run = one layer's 8x8 tiles clipped to screen lines
// [y0, y1). Draw order is run order (segment order; within a segment pf2 fill -> pf2 -> pf1 -> pf0).
// Consecutive segments with the same registers/VRAM merge into one run. Line-scroll layers emit tile strips (8x1) per line, also as one run
// (per-line scissor/draw batches made list building exceed the frame budget on line-scroll screens).
#ifndef REI_GE_LAYERS_H
#define REI_GE_LAYERS_H

#include <stdint.h>

#include "rei_core.h"

// Transparency pattern: draw only color indices (0-15) whose bit c is set. Two passes per tile, low and high (high pixels mark stencil "high")
enum { GE_PAT_NONE = -1, GE_PAT_1_15, GE_PAT_1_7, GE_PAT_8_15, GE_PAT_0_15, GE_PAT_0_7, GE_PAT_0, GE_PAT_COUNT };
extern const uint16_t ge_pat_bits[GE_PAT_COUNT];
// [is pf2][group 0-2][0 = low, 1 = high] -> pattern (complement of FBNeo transmask)
extern const int8_t ge_pat_of[2][3][2];

typedef struct {
	int16_t x, y;      // tile top-left (game screen coords, may be negative)
	uint16_t tile;     // tile number (tile_mask applied)
	uint8_t pal;       // palette 0-127 (color = pal*16 + pixel)
	uint8_t flags;     // bit0 flipx, bit1 flipy, bit2-3 group (0-2), bit7 strip (GE_TILE_STRIP)
	uint8_t h, pad;    // strip line count (1-8)
} ge_tile;
// Strip: a few tile lines (8xh) of a line-scroll layer. y is the first screen line, bit4-6 the position within the tile of the first line (before flip, 0-7).
// Adjacent lines with the same horizontal scroll and tile row merge into one strip
#define GE_TILE_STRIP 0x80
#define GE_STRIP_ROW(f) (((f) >> 4) & 7)

typedef struct {
	int16_t y0, y1;    // screen lines to clip
	uint8_t layer;     // 0~2
	uint8_t fill;      // 1 = fill with palette color 0 instead of tiles (pf2 off)
	uint8_t strips;    // 1 = line-scroll layer (tiles are strips)
	int first, count;  // tiles[first .. first+count)
} ge_run;

typedef struct {
	ge_tile *tiles;
	int ntiles;
	ge_run *runs;
	int nruns;
	int overflow;      // 1 if anything was dropped for lack of buffer space
} ge_frame;

// Once at startup: build per-tile color presence bits from tile graphics (skip empty tiles). 0 on success
int ge_layers_init(const rei_tile_info *info);
void ge_layers_begin(void);
void ge_layers_segment(const rei_layer_seg *seg, void *user); // pass as rei_layer_fn
const ge_frame *ge_layers_end(void);

#endif
