// M72-family tile layers -> GE display list (src/core/shim/rei_m72ge.h). Platform-independent C: on PSP the ME calls it while rendering,
// the PC harness (--ge-m72) interprets the same list and compares with the CPU screen. Slots (gem_slot) and vertex format match M107 GE (ge_m107.h).
//
// The list assumes main has set up the draw buffer (8888 offscreen, cleared with color index 0), alpha test (GREATER 0), texturing on and texture function (REPLACE).
// Tile textures are a T4 atlas per layer (512x512 windows, 4096 tiles). CLUTs of 256 entries (palette 0x100-0x1ff),
// one set per pen mask (masked pens alpha 0) + one set for sprites (palette 0x000-0x0ff, pen 0 alpha 0); main fills them with that frame's
// palette (gm72_masks order, sprites last). The sprite texture is T16 (VRAM copy of the color index buffer).
#ifndef REI_GE_M72_H
#define REI_GE_M72_H

#include <stdint.h>

#include "ge_m107.h"
#include "rei_m72ge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM72_MAX_MASKS 8
#define GM72_CLUT_BYTES (256 * 2) // one CLUT set

typedef struct {
	uint32_t atlas_ge[2];   // layer 0/1 atlases (GEM_ATLAS_WIN_BYTES per window)
	uint32_t clut_ge;       // first CLUT set (GM72_CLUT_BYTES apart, 64-byte aligned)
	uint32_t spr_ge;        // sprite texture (T16)
	int spr_tbw;            // its width (texels)
	uint16_t *(*spr_buf)(void); // buffer the renderer draws sprites into (pTransDraw if NULL)
	gem_slot *(*get_slot)(void);
} gm72_config;

// main, once (before starting the renderer)
int gm72_init(const rei_m72_tiles *t, const gm72_config *c);
// pen masks to fill CLUTs with (after gm72_init). Returns the count; CLUT set i < n is out[i], set n is sprites
int gm72_masks(uint16_t *out);
// ops to pass to rei_m72_set_ge
extern const rei_m72_ge_ops gm72_ops;

#ifdef __cplusplus
}
#endif

#endif
