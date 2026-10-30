// M107 tile layers via GE (PSP, psp/src/ge_m107.c). Instead of drawing pixels per draw segment, the render side (ME on PSP) hands layer
// state to the frontend, which builds a GE draw list (tile quads). The render side still draws sprites, but tile
// priority (hidden by "high" pixels) is resolved by the GE stencil:
//   A buffer = first sprite drawn covering the pixel (0x800 | color | hidden flag 0x1000),
//   B buffer = when A is a "hidden" sprite, the first "not hidden" sprite after it (0x800 | color, rare).
// Final = A if present and (not hidden or tile not high), else B if present, else the tile.
// Same result as FBNeo RenderTilePrio's "hidden pixels do not claim the pixel" (checked by tools/harness --ge-m107).
#ifndef REI_M107GE_H
#define REI_M107GE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int enable, rowscroll;            // rowscroll bit0 horizontal (per line), bit1 vertical (per line)
	int scrollx, scrolly;             // raw register values (FBNeo draw_layer_byline adjusts them)
	const uint16_t *vram;             // 64×64 cells, each (code, attr)
	const uint16_t *xscroll, *yscroll; // row scroll tables (VRAM 0xe000 / 0xe800 + 0x200×layer)
} rei_m107_layer;

typedef struct {
	int start, finish;                // screen lines [start, finish)
	int l3_on;                        // layer 3 enabled (~pf_control[3][4] & 0x80)
	int layer_mask, sprite_mask;      // nBurnLayer, nSpriteEnable
	rei_m107_layer l[4];
} rei_m107_seg;

typedef struct {
	uint16_t *a, *b;                  // sprite A/B buffers (320 wide, 240 tall). Render clears A and draws at frame end
	// Line ranges [y0, y1) filled by render (none if y0 >= y1): per A color group (color >> 8), A hidden flags, B, B color group bits
	int16_t ay0[8], ay1[8], hy0, hy1, by0, by1;
	int bbanks;
	// Filled by the frontend in frame_begin: lines [clr_y0, clr_y1) to clear in A - sprite lines of the frame that last used this buffer
	// (other lines are already 0). Render clears only those (it used to clear 150KB per frame)
	int16_t clr_y0, clr_y1;
} rei_m107_spr;

typedef struct {
	// Render: frame start. Returns the sprite buffers (PSP: the buffer being drawn)
	void (*frame_begin)(rei_m107_spr *out);
	void (*segment)(const rei_m107_seg *s);
	// Render: frame end (after all sprites). nb = pixels written to B, ab = 1 if A has hidden pixels
	void (*frame_end)(const rei_m107_spr *spr, int nb, int ab);
} rei_m107_ge_ops;

// Tile graphics (8×8, 1 byte per pixel, 64 bytes per tile), row table (bit r = nonzero pixel in row r), tile masks
typedef struct {
	const uint8_t *gfx;
	const uint16_t *rowpens;
	int tile_mask;
} rei_m107_tiles;

// Core (d_m107.cpp): render-side GE mode. ops NULL = draw on CPU. check 1 = also draw on CPU (harness compare)
void rei_m107_set_ge(const rei_m107_ge_ops *ops, int check);
int rei_m107_get_tiles(rei_m107_tiles *out); // 0 = success (-1 if not M107)

#ifdef __cplusplus
}
#endif

#endif
