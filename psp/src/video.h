// PSP display output (GE). psp/docs/M92_VIDEO.md G1: the GE converts the core's palette index buffer via CLUT and draws it.
#ifndef REI_PSP_VIDEO_H
#define REI_PSP_VIDEO_H

#include <stdint.h>

#include "ge_layers.h"
#include "ge_m107.h"

void video_init(void);
// Per-game border art <dir>art/<game>_43.r565/_11.r565 into VRAM (after the game is chosen). Without it the border is a solid color
void video_load_art(const char *dir, const char *game, const char *parent); // user artwork/<game>.png etc.
void video_shutdown(void);

// Core palette entry (color callback rei_psp_color format = GE 5551) -> color for the CLUT
uint32_t video_core_color(int r, int g, int b);

// Send the drawn frame to the GE (no wait). index: 320×240 palette indices, pal: 0x801 entries
void video_submit(const uint16_t *index, const uint32_t *pal, int blank);
// G2: build the tile atlas (once at startup). 0 = success
int video_layers_init(const rei_tile_info *ti);
const void *video_atlas_addr(void); // diagnostic: tile atlas address
// G2: send one frame to the GE from layer run list + sprite buffer + palette. Returns 1 if drawing was cut short by buffer space
int video_submit_layers(const ge_frame *fr, const uint16_t *spr, const uint32_t *pal, int blank);
// The same in three pieces (in order, after video_sync): CLUT, sprite buffer -> VRAM, submit draw list
void video_submit_clut(const uint32_t *pal);
void video_submit_sprites(const uint16_t *spr);
// If st is non-NULL (G3), the GE draws from this tile list instead of copying the sprite buffer to VRAM (video_submit_sprites skipped)
int video_submit_list(const ge_frame *fr, const rei_spr_tile *st, int nst, const uint8_t *sgfx, const uint32_t *pal, int blank);
// Wait until previously submitted drawing finishes (before the next frame overwrites the index buffer)
void video_sync(void);
// Show the drawn screen (no vsync wait)
void video_flip(void);
// Developer text at top-left (newlines allowed, empty string turns it off)
void video_set_hud(const char *text);

// Screen composition (settings: aspect, filter, scanlines, border). Drawn with each game frame; while the game is paused
// (e.g. menu), video_present redraws over the last game frame (waits for the previous list, flips, then submits the new one)
void video_present(void);
// Text callback run at the end of composition (menu, FPS, notices). Use video_text/video_rect only inside it
void video_set_overlay(void (*fn)(void));
// Text (8×8 font × scale, color 0xAABBGGRR, '\n' newline). Returns the drawn width
int video_text(int x, int y, uint32_t abgr, int scale, const char *s);
void video_rect(int x, int y, int w, int h, uint32_t abgr);
// Other boards: GE 5650 (rei_psp_color565) buffer drawn by the core on CPU as the game screen (main memory, 16-byte aligned, stride multiple of 8,
// flags = REI_SCREEN_VERTICAL/FLIPPED). NULL = M92 offscreen. Flush the buffer from D-cache every frame, then video_present
void video_set_game_buffer(const uint16_t *buf, int w, int h, int stride, int flags);
// Other-board index mode: idx = color indices (width w, main RAM), clut = nclut 16-bit (5650) palette entries. The GE applies colors
int video_index_fits(int w, int h); // do the offscreen/index copies fit in VRAM
void video_set_game_index(const uint16_t *idx, const uint16_t *clut, int nclut, int w, int h, int flags);
// M107 GE mode (ge_m107.c): a = sprite A (to VRAM like index mode), b = sprite B, clut = 16-bit (5650) palette,
// list = layer list built by the ME (CALL), res = that buffer's result
void video_set_game_gem(const uint16_t *a, const uint16_t *b, const uint16_t *clut, const uint32_t *list, const gem_result *res,
			int w, int h, int flags);
void video_gem_cluts(uint32_t out[2]); // layer CLUT addresses: [0] transparent (color 0 alpha 0), [1] opaque — 2048 entries 5551
int video_gem_atlas_init(const rei_tile_info *ti); // M107 GE mode atlas (T4). 0 = success
// M72-family GE mode (ge_m72.c): build atlases (T4) for both layers, their GE addresses into out. 0 = success
int video_gm72_atlas_init(const uint8_t *const gfx[2], const int ntiles[2], uint32_t out[2]);
uint32_t video_gm72_clut_addr(void);          // per-mask CLUTs + sprite CLUT slot (GM72_CLUT_BYTES apart)
uint32_t video_index_copy_addr(int w);        // GE address of the index-mode VRAM copy (width video_index_copy_tbw)
int video_index_copy_tbw(int w);
// a = color index buffer (sprites only, to VRAM like index mode), clut = 16-bit (5650) palette, list = list built by the ME (CALL),
// masks = nmasks pen masks from gm72_masks (CLUTs filled with that frame's palette)
void video_set_game_gm72(const uint16_t *a, const uint16_t *clut, const uint32_t *list, const uint16_t *masks, int nmasks, int w, int h,
			 int flags);
void video_layers_free(void);          // frees the video_layers_init atlases
// game screen rectangle for the current aspect ratio
void video_game_rect(int *x, int *y, int *w, int *h);

// Verify: read back the last drawn screen and compare with CPU colors from index/pal. Returns the number of differing pixels.
// If first_diff is non-NULL, writes the first difference (x, y, expected, actual). Call after video_sync, before video_flip.
// Diagnostic: write the current screen (320×240 RGBA), indices and palette (0x801) to one file
void video_dump(const char *path, const uint16_t *index, const uint32_t *pal);
int video_verify(const uint16_t *index, const uint32_t *pal, int blank, char *first_diff, int n);

#endif
