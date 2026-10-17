// M107 tile layers -> GE display list (src/core/shim/rei_m107ge.h). Platform-independent C: on PSP the ME calls it while rendering,
// the PC harness (--ge-m107) interprets the same list and compares it with the CPU frame.
//
// Per frame the renderer writes GE commands (ending in RET; the main list CALLs it) and vertices into one slot (gem_slot). Draw buffer,
// texture format (T8), alpha test and stencil test enable are assumed set by main. Layer pixels leave priority in stencil:
// high (group 1) pixels 0x80, low 0 (FBNeo RamPrioBitmap 0/1). Texture is an 8bpp atlas (512x512 window, 4096 tiles),
// CLUT is two sets (transparent: color 0 has alpha 0; opaque) x 2048 colors 5551, filled by main with that frame's palette.
#ifndef REI_GE_M107_H
#define REI_GE_M107_H

#include <stdint.h>

#include "rei_m107ge.h"

#ifdef __cplusplus
extern "C" {
#endif

// Atlas: T4 (4bpp, low nibble = left texel), 512x512 window = 4096 tiles
#define GEM_ATLAS_WIN_BYTES (512 * 512 / 2)

typedef struct {
	uint16_t u, v;
	int16_t x, y, z;
} gem_vtx; // GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D

// Results the renderer leaves in a slot (main reads them after render reports the slot done)
typedef struct {
	int32_t ncmd, nvtx, overflow; // command words, vertices, 1 if anything was dropped for lack of buffer
	int32_t nb, ab;               // B buffer pixel count, 1 if A has occlusion marks
	int32_t b_dirty;              // B buffer has nonzero values left (cleared next time this slot is used)
	int32_t ntiles, bbanks;
	int16_t ay0[8], ay1[8], hy0, hy1, by0, by1; // sprite line ranges (as in rei_m107_spr)
} gem_result;

typedef struct {
	uint32_t *cmd;      // commands (CPU address)
	uint32_t cmd_ge;    // GE address of the same buffer
	int cmd_max;
	gem_vtx *vtx;
	uint32_t vtx_ge;
	int vtx_max;
	uint16_t *spr_a, *spr_b; // 320×240
	gem_result *res;
} gem_slot;

// Main, once (before starting render): tile table, GE addresses of atlas and CLUT (clut_ge[0] transparent, [1] opaque; 2048 16-bit entries each),
// and a function returning the slot to use per render frame
int gem_init(const rei_m107_tiles *t, uint32_t atlas_ge, const uint32_t clut_ge[2], gem_slot *(*get_slot)(void));
// (dev check) a few fixed commands encoded this file's way; the PSP dev build compares them with sceGu output. Word count (<= 16)
int gem_selftest_words(uint32_t *out);
// ops to pass to rei_m107_set_ge
extern const rei_m107_ge_ops gem_ops;

#ifdef __cplusplus
}
#endif

#endif
