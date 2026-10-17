// M72-family (M72/M81/M82/M84/M85) tile layers via GE (PSP, psp/src/ge_m72.c). Instead of the render side (ME on PSP) drawing layers
// to pixels per draw segment, it passes layer state and the frontend builds a GE display list (tile quads). Sprites are still drawn
// by the render side (color index buffer, 0 = none). Same order as FBNeo d_m72.cpp dodrawline, per segment:
//   back pass (layer 1, layer 0 - forcelayer 1) -> sprites -> front pass (layer 1, layer 0 - forcelayer 0)
// Pass tiles skip pens in the pen mask (transmask) chosen by tile priority - GE uses one CLUT per mask (those pens alpha 0).
// Output matches CPU drawing color indices (verified by tools/harness --ge-m72).
#ifndef REI_M72GE_H
#define REI_M72GE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int start, finish;          // screen lines [start, finish)
	int layer_mask, sprite_on;  // nBurnLayer, draw sprites (nSpriteEnable & 1)
	int type;                   // m72_video_type (1·2·3)
	int scrolly[2], scrollx[2]; // values adjusted by draw_layer (layer 0 = front, 1 = back)
	int rowscroll;              // majtitle: layer 1 row scroll (type 3)
	int video_offset1;          // video_offsets[1] (row scroll calculation)
	const uint16_t *vram[2];    // per tile (code, color)
	const uint16_t *xscroll;    // row scroll table (when rowscroll)
} rei_m72_seg;

typedef struct {
	// render: frame start - returns the sprite buffer (color indices, screen size). NULL = pTransDraw (PSP: index buffer = picture)
	uint16_t *(*frame_begin)(void);
	void (*segment)(const rei_m72_seg *s);
	void (*frame_end)(void);
} rei_m72_ge_ops;

// Tile graphics (8x8, 1 byte per pixel, 64 bytes per tile), tile count, pens used per tile (bits, may be NULL), pen mask table [layer][priority][forcelayer]
typedef struct {
	const uint8_t *gfx[2];
	int ntiles[2];
	const uint16_t *tpens[2];
	uint16_t transmask[2][3][2];
	int width, height;
	int type;                   // m72_video_type (3 = majtitle: row scroll layer)
} rei_m72_tiles;

// Core (d_m72.cpp): render-side GE mode. ops NULL = draw with CPU. check 1 = also draw with CPU (harness comparison)
void rei_m72_set_ge(const rei_m72_ge_ops *ops, int check);
int rei_m72_get_tiles(rei_m72_tiles *out); // 0 on success (-1 if not an M72 driver)

#ifdef __cplusplus
}
#endif

#endif
