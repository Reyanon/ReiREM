// PSP display output (GE), psp/docs/M92_VIDEO.md G1/G2.
//
// G2 (video_submit_layers): the GE draws tile layers. Tiles in an 8bpp atlas (16 windows of 512x512, 4MB), colors via per-pattern
// CLUTs (alpha bit keeps only drawn colors, alpha test), high-pass pixels set stencil bit7. Sprites: the CPU-built buffer
// (rei_core_sprite_frame) is overlaid as in G1 below, but pixels flagged "hidden by high tiles" with stencil bit7 set are
// skipped (marked with stencil bit6). Rules and PC verification: psp/src/ge_layers.c, psp/tools/ge_model.
//
// G1 (video_submit): convert the core's final index buffer to colors.
// The core only produces 320x240 palette indices (0-0x7ff, 16-bit). 2048 palette colors don't fit one GE CLUT access
// (CLUT index = (val >> shift & 0xff) | csa << 4), so split into 8 banks of 256 colors.
//  1) Read as a 5551 texture, index bit8/9 = top 2 bits of G, bit10 = lowest bit of B.
//     Color test keeps only bank b pixels and writes b to the stencil (8888 framebuffer alpha) (8 passes).
//  2) Per bank, load its 256 colors into the CLUT and draw the index buffer as a T16 (16-bit index) texture,
//     only where stencil == b (8 passes).
// Game palette RAM (xBGR555) has the same bit layout as GE 5551, so it goes into the CLUT as-is (alpha bit set).
#define STB_IMAGE_IMPLEMENTATION // user outside-screen artwork (pspdev's stb_image.h)
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>
#include "video.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "ge_layers.h"
#include "ge_m72.h"
#include "menu.h"
#include "settings.h"

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define GAME_W 320
#define GAME_H 240
// The game draws to a 320x240 offscreen in VRAM (8888, alpha = stencil); compose draws border, scaling, scanlines, text
// to the display buffer (psp/docs/RULES.md 4-2). Game screen position in the offscreen is 0,0
#define GAME_X 0
#define GAME_Y 0
// VRAM 2MB: 3 display buffers (5650; game colors are 555 so nothing is lost) | offscreen 8888 | index texture 320x240x2 |
// 2 border images (5650) = ~1.95MB. The GE reading a main-RAM texture 16 times is slow (PP2 ~25ms), so indices are moved to VRAM.
// With 2 display buffers, right after requesting a flip (happens next vblank) the next frame was drawn into the still-visible one, so menus
// flickered badly and settings changes looked overlapped: draw into a third buffer, avoiding the visible and pending ones
#define FB_COUNT 3
#define FB_BYTES (BUF_W * SCR_H * 2)
#define OFF_OFF (FB_BYTES * FB_COUNT)
#define OFF_BYTES (BUF_W * GAME_H * 4)
#define INDEX_OFF (OFF_OFF + OFF_BYTES)
#define INDEX_BYTES (GAME_W * GAME_H * 2)
#define ART_OFF ((INDEX_OFF + INDEX_BYTES + 0xfff) & ~0xfff)
#define ART_BYTES (BUF_W * SCR_H * 2)
#define OFF_TEX ((const void *)(0x04000000u + OFF_OFF))
// Scissor while drawing to the offscreen (240 lines: at screen size, clears would overwrite the index texture)
#define SCISSOR_GAME() sceGuScissor(0, 0, GAME_W, GAME_H)

#define LIST_WORDS 0x40000 // 1MB. Line-scroll frames exceed 20k tiles
// (64-byte aligned: sceGu writes the list via uncached addresses — sharing a cache line with another global (e.g. g_list_cur just before)
//  meant main cache writeback overwrote the list head with stale data: broken game list screen, sprite flicker. Only showed in a build shifted by 16 bytes,
//  2026-10-17 recurrence note 17. Size 2MB is a multiple of 64 so the end aligns too)
static unsigned int __attribute__((aligned(64))) g_list[2][LIST_WORDS];
static int g_list_cur;
static uint16_t __attribute__((aligned(16))) g_clut[2][0x800];
static void *g_draw_fb; // VRAM-relative address, buffer being drawn
static int g_draw_i, g_disp_i; // indices of the draw buffer and the visible (or flip-pending) buffer
unsigned g_flip_waits; // times we waited because the previous flip had not reached the screen (log)

struct vtx {
	uint16_t u, v;
	int16_t x, y, z;
};
#define VTX_TYPE (GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D)

struct cvtx {
	uint32_t color;
	int16_t x, y, z, pad;
};

// G2 resources
#define ATLAS_WIN_BYTES (512 * 512) // T8 (4bpp T4 showed broken tiles on hardware)
#define MAX_VTX 200000
static uint8_t *g_atlas;
static int g_atlas_wins;
static uint16_t __attribute__((aligned(16))) g_pat_clut[GE_PAT_COUNT][0x800];
static uint16_t __attribute__((aligned(16))) g_spr_clut[0x800];
// G3: sprite tile -> index buffer value CLUT. [hidden 0/1][color 128][pixel 16] = 0x800 | color*16 | pixel | SPR_HIDDEN, pixel 0 is 0.
// The hidden flag lives in color bit12 (5551 B bit2 -> 8-bit color 0x200000), not the alpha bit: drawing to 5551 with stencil test off,
// hardware did not write the alpha bit, so sprites meant to be hidden were drawn over high tiles (PLAYER SELECT text)
#define SPR_HIDDEN 0x1000
#define SPR_HIDDEN_COLOR 0x200000u
static uint16_t __attribute__((aligned(16))) g_spr_idx[2 * 128 * 16];
static struct vtx __attribute__((aligned(16))) g_vpool[MAX_VTX];
static int g_nvtx;
static int g_overflow;

uint32_t video_core_color(int r, int g, int b)
{
	return 0x8000u | ((uint32_t)(b >> 3) << 10) | ((uint32_t)(g >> 3) << 5) | (uint32_t)(r >> 3);
}

static void font_init(void);
static void compose(void);
static int g_sent; // a draw list has been sent (whether video_present flips)
static int g_art_ok[2]; // game border image uploaded to VRAM (4:3, 1:1). Solid color if not

// User outside-screen artwork <dir>artwork/<game>.png (.jpg, .bmp, else parent set name), scaled to screen size (480x272, 565)
// into VRAM (same image for both aspects). No artwork is bundled (copyright). Same as the Vita's rei_video_set_art.
// Call after picking a game, before loading the core (decoding briefly uses memory)
static int load_art_file(const char *dir, const char *name, uint16_t *out)
{
	static const char *const ext[] = { "png", "jpg", "bmp" };
	for (int e = 0; e < 3; e++) {
		char path[192];
		snprintf(path, sizeof(path), "%sartwork/%s.%s", dir, name, ext[e]);
		int w, h, c;
		unsigned char *img = stbi_load(path, &w, &h, &c, 3);
		if (!img)
			continue;
		for (int y = 0; y < SCR_H; y++) { // nearest-neighbor scaling (good enough for a border image)
			const unsigned char *row = img + (size_t)(y * h / SCR_H) * w * 3;
			for (int x = 0; x < SCR_W; x++) {
				const unsigned char *px = row + (x * w / SCR_W) * 3;
				out[y * BUF_W + x] = (uint16_t)((px[0] >> 3) | ((px[1] >> 2) << 5) | ((px[2] >> 3) << 11));
			}
		}
		stbi_image_free(img);
		return 1;
	}
	return 0;
}

// Games without their own artwork: the app's blank artwork (bundled, shapes only), tinted with the theme's accent (its
// brightness times the colour, as on the Vita) and tinted again when the theme changes. Its brightness at screen size is
// kept (130KB) for that
#include "rei_blank_png.c" // vita/sce_sys_src/blankArtwork.png (psp/CMakeLists.txt)
static uint8_t *g_blank_lum;
static int g_art_blank;     // the border image is the blank artwork
static uint32_t g_blank_acc; // accent it was tinted with
static void blank_tint(void)
{
	const uint32_t acc = rei_menu_accent();
	const unsigned tr = acc & 0xFF, tg = (acc >> 8) & 0xFF, tb = (acc >> 16) & 0xFF;
	for (int i = 0; i < 2; i++) {
		uint16_t *out = (uint16_t *)(0x44000000u + ART_OFF + i * ART_BYTES);
		for (int y = 0; y < SCR_H; y++)
			for (int x = 0; x < SCR_W; x++) {
				const unsigned l = g_blank_lum[y * SCR_W + x];
				unsigned r = l * tr / 128, g = l * tg / 128, b = l * tb / 128;
				r = r > 255 ? 255 : r;
				g = g > 255 ? 255 : g;
				b = b > 255 ? 255 : b;
				out[y * BUF_W + x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
			}
		g_art_ok[i] = 1;
	}
	g_blank_acc = acc;
}
static void load_blank(void)
{
	if (!g_blank_lum) {
		int w, h, c;
		unsigned char *img = stbi_load_from_memory(rei_blank_png, size_rei_blank_png, &w, &h, &c, 3);
		if (!img)
			return;
		g_blank_lum = (uint8_t *)malloc(SCR_W * SCR_H);
		if (g_blank_lum) // (the average of each source block: its thin grid lines would drop out picking single pixels)
			for (int y = 0; y < SCR_H; y++) {
				const int y0 = y * h / SCR_H, y1 = (y + 1) * h / SCR_H > y0 ? (y + 1) * h / SCR_H : y0 + 1;
				for (int x = 0; x < SCR_W; x++) {
					const int x0 = x * w / SCR_W, x1 = (x + 1) * w / SCR_W > x0 ? (x + 1) * w / SCR_W : x0 + 1;
					unsigned sum = 0, n = 0;
					for (int yy = y0; yy < y1; yy++)
						for (int xx = x0; xx < x1; xx++, n++) {
							const unsigned char *px = img + ((size_t)yy * w + xx) * 3;
							sum += (px[0] * 77u + px[1] * 150u + px[2] * 29u) >> 8;
						}
					g_blank_lum[y * SCR_W + x] = (uint8_t)(sum / n);
				}
			}
		stbi_image_free(img);
		if (!g_blank_lum)
			return;
	}
	blank_tint();
	g_art_blank = 1;
}

void video_load_art(const char *dir, const char *game, const char *parent)
{
	g_art_ok[0] = g_art_ok[1] = 0;
	g_art_blank = 0;
	uint16_t *buf = (uint16_t *)calloc(1, ART_BYTES);
	if (!buf)
		return;
	if (load_art_file(dir, game, buf) || (parent && load_art_file(dir, parent, buf))) {
		sceKernelDcacheWritebackAll();
		for (int i = 0; i < 2; i++) {
			memcpy((void *)(0x44000000u + ART_OFF + i * ART_BYTES), buf, ART_BYTES); // (not decoded straight into VRAM)
			g_art_ok[i] = 1;
		}
	}
	free(buf);
	if (!g_art_ok[0])
		load_blank();
}

void video_init(void)
{
	font_init();
	sceGuInit();
	sceGuStart(GU_DIRECT, g_list[0]);
	sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_W);
	sceGuDispBuffer(SCR_W, SCR_H, (void *)FB_BYTES, BUF_W);
	sceGuDepthBuffer((void *)OFF_OFF, BUF_W); // (depth unused: test and write both off)
	sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
	sceGuViewport(2048, 2048, SCR_W, SCR_H);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_DITHER);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuClearColor(0xff000000);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);
	// clear both display buffers to black
	memset((void *)0x44000000u, 0, FB_BYTES * FB_COUNT);
	memset((void *)(0x44000000u + OFF_OFF), 0, OFF_BYTES); // also blacken the offscreen that compose reads before a game starts (game list)
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	// after sceGuDispBuffer, buffer 1 is visible and buffer 0 is drawn
	g_disp_i = 1;
	g_draw_i = 0;
	g_draw_fb = (void *)0;
}

void video_shutdown(void)
{
	sceGuTerm();
}

// Rectangle covering game screen lines [y0, y1) (texture = 320x240 buffer at the same position)
static void draw_game_rows(int y0, int y1)
{
	if (y1 <= y0)
		return;
	// draw in 64-texel-wide strips (friendly to the GE texture cache)
	const int strips = GAME_W / 64;
	struct vtx *v = (struct vtx *)sceGuGetMemory(strips * 2 * sizeof(struct vtx));
	for (int i = 0; i < strips; i++) {
		v[i * 2 + 0] = (struct vtx){ (uint16_t)(i * 64), (uint16_t)y0, (int16_t)(GAME_X + i * 64), (int16_t)(GAME_Y + y0), 0 };
		v[i * 2 + 1] = (struct vtx){ (uint16_t)(i * 64 + 64), (uint16_t)y1, (int16_t)(GAME_X + i * 64 + 64), (int16_t)(GAME_Y + y1), 0 };
	}
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, strips * 2, NULL, v);
}

static void draw_game_quad(void)
{
	draw_game_rows(0, GAME_H);
}

// G3 sprite compose range: per bank (color >> 4), lines [y0, y1) with sprites, and lines of hidden-flag sprites. Outside them the
// bank has no pixels, so composing only those lines gives the same result (full screen 17 times -> only used banks/lines)
static struct { int y0, y1; } g_sbank[8], g_shid;
static void sprite_ranges(const rei_spr_tile *t, int n)
{
	for (int b = 0; b < 8; b++) {
		g_sbank[b].y0 = GAME_H;
		g_sbank[b].y1 = 0;
	}
	g_shid.y0 = GAME_H;
	g_shid.y1 = 0;
	for (int i = 0; i < n; i++) {
		int y0 = t[i].y, y1 = t[i].y + 16;
		if (y0 < 0) y0 = 0;
		if (y1 > GAME_H) y1 = GAME_H;
		if (y1 <= y0)
			continue;
		const int b = (t[i].color >> 4) & 7;
		if (y0 < g_sbank[b].y0) g_sbank[b].y0 = y0;
		if (y1 > g_sbank[b].y1) g_sbank[b].y1 = y1;
		if (t[i].flags & 4) {
			if (y0 < g_shid.y0) g_shid.y0 = y0;
			if (y1 > g_shid.y1) g_shid.y1 = y1;
		}
	}
}
static void sprite_ranges_full(void)
{
	for (int b = 0; b < 8; b++) {
		g_sbank[b].y0 = 0;
		g_sbank[b].y1 = GAME_H;
	}
	g_shid.y0 = 0;
	g_shid.y1 = GAME_H;
}

// Position of index bit8/9/10 in the 8-bit color (0x00BBGGRR) when read as 5551 (M92 sprite stage)
#define BANK_MASK 0x0008c000u
static uint32_t bank_color(int b)
{
	return ((b & 1) ? 0x4000u : 0) | ((b & 2) ? 0x8000u : 0) | ((b & 4) ? 0x080000u : 0);
}
// Bank number -> stencil, once per bank bit: write only that stencil bit where the bit is set (color test) (PixelMask blocks the other
// bits and RGB). Stencil = bank number. Previously once per bank (M107 8 -> 3 passes, M72 2 -> 1)
static void bank_bits_to_stencil(int banks, void (*draw)(void))
{
	static const uint32_t bitcol[3] = { 0x4000u, 0x8000u, 0x080000u }; // (bit positions of bank_color)
	const int nbits = banks > 4 ? 3 : banks > 2 ? 2 : 1;
	sceGuEnable(GU_COLOR_TEST);
	sceGuEnable(GU_STENCIL_TEST);
	sceGuStencilFunc(GU_ALWAYS, 0xff, 0xff);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
	for (int k = 0; k < nbits; k++) {
		sceGuPixelMask(~(1u << (24 + k)));
		sceGuColorFunc(GU_EQUAL, bitcol[k], bitcol[k]);
		draw();
	}
	sceGuPixelMask(0);
	sceGuDisable(GU_COLOR_TEST);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
}

void video_submit(const uint16_t *index, const uint32_t *pal, int blank)
{
	// indices to VRAM (written via uncached addresses). Previous frame's drawing has finished via video_sync
	uint16_t *vidx = (uint16_t *)(0x44000000u + INDEX_OFF);
	memcpy(vidx, index, INDEX_BYTES);
	const void *tex = (const void *)(0x04000000u + INDEX_OFF);
	uint16_t *clut = g_clut[g_list_cur];
	for (int i = 0; i < 0x800; i++)
		clut[i] = (uint16_t)pal[i];
	sceKernelDcacheWritebackRange(clut, sizeof(g_clut[0]));

	sceGuStart(GU_DIRECT, g_list[g_list_cur]);
	g_list_cur ^= 1;
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, BUF_W);
	sceGuEnable(GU_SCISSOR_TEST);
	SCISSOR_GAME();
	sceGuClearColor(0xff000000);
	sceGuClearStencil(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);

	if (!blank) {
		// 1) bank number -> stencil (once per bit — bank_bits_to_stencil)
		sceGuTexMode(GU_PSM_5551, 0, 0, 0);
		sceGuTexImage(0, 512, 256, GAME_W, tex);
		sceGuTexFlush();
		bank_bits_to_stencil(8, draw_game_quad);
		sceGuEnable(GU_STENCIL_TEST);

		// 2) color via per-bank CLUT
		sceGuTexMode(GU_PSM_T16, 0, 0, 0);
		sceGuTexImage(0, 512, 256, GAME_W, tex);
		sceGuTexFlush();
		sceGuClutMode(GU_PSM_5551, 0, 0xff, 0);
		sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
		for (int b = 0; b < 8; b++) {
			sceGuClutLoad(256 / 16, clut + b * 256);
			sceGuStencilFunc(GU_EQUAL, b, 0x07);
			draw_game_quad();
		}
		sceGuDisable(GU_STENCIL_TEST);
	}
	compose();
	sceGuFinish();
	g_sent = 1;
}

static uint32_t expand(uint32_t c5);

static uint32_t color_8888(uint32_t c5551)
{
	return 0xff000000u | expand(c5551 & 31) | (expand((c5551 >> 5) & 31) << 8) | (expand((c5551 >> 10) & 31) << 16);
}

int video_layers_init(const rei_tile_info *ti)
{
	int ntiles = ti->tile_mask + 1;
	g_atlas_wins = (ntiles + 4095) / 4096;
	g_atlas = (uint8_t *)memalign(16, (size_t)g_atlas_wins * ATLAS_WIN_BYTES);
	if (!g_atlas)
		return -1;
	memset(g_atlas, 0, (size_t)g_atlas_wins * ATLAS_WIN_BYTES);
	for (int t = 0; t < ntiles; t++) {
		uint8_t *base = g_atlas + (size_t)(t >> 12) * ATLAS_WIN_BYTES;
		int j = t & 4095, tx = (j & 63) * 8, ty = (j >> 6) * 8;
		const uint8_t *src = ti->tiles + (size_t)t * 64;
		for (int y = 0; y < 8; y++) {
			uint8_t *row = base + (ty + y) * 512 + tx;
			for (int x = 0; x < 8; x++)
				row[x] = src[y * 8 + x] & 15;
		}
	}
	for (int m = 0; m < 2; m++)
		for (int c = 0; c < 128; c++)
			for (int p = 0; p < 16; p++)
				g_spr_idx[(m * 128 + c) * 16 + p] = p ? (uint16_t)(0x800 | (c << 4) | p | (m ? SPR_HIDDEN : 0)) : 0;
	sceKernelDcacheWritebackAll();
	return 0;
}

// M107 GE mode atlas: 4bpp (T4, two texels per byte — low nibble is left), 512x512 window = 128KB holds 4096 tiles. Half of T8 —
// dsoccr94 (130k tiles) needed 8MB with T8, leaving no memory to grow the dynarec code cache (fell back to interpreter, 35ms)
int video_gem_atlas_init(const rei_tile_info *ti)
{
	const int ntiles = ti->tile_mask + 1;
	g_atlas_wins = (ntiles + 4095) / 4096;
	const size_t bytes = (size_t)g_atlas_wins * GEM_ATLAS_WIN_BYTES;
	g_atlas = (uint8_t *)memalign(16, bytes);
	if (!g_atlas)
		return -1;
	memset(g_atlas, 0, bytes);
	for (int t = 0; t < ntiles; t++) {
		uint8_t *base = g_atlas + (size_t)(t >> 12) * GEM_ATLAS_WIN_BYTES;
		const int j = t & 4095, tx = (j & 63) * 8, ty = (j >> 6) * 8;
		const uint8_t *src = ti->tiles + (size_t)t * 64;
		for (int y = 0; y < 8; y++) {
			uint8_t *row = base + ((ty + y) * 512 + tx) / 2;
			for (int x = 0; x < 8; x += 2)
				row[x / 2] = (uint8_t)((src[y * 8 + x] & 15) | ((src[y * 8 + x + 1] & 15) << 4));
		}
	}
	for (int m = 0; m < 2; m++)
		for (int c = 0; c < 128; c++)
			for (int p = 0; p < 16; p++)
				g_spr_idx[(m * 128 + c) * 16 + p] = p ? (uint16_t)(0x800 | (c << 4) | p | (m ? SPR_HIDDEN : 0)) : 0;
	sceKernelDcacheWritebackAll();
	return 0;
}

#ifdef REI_PROFILE
static uint32_t g_vbatches; // diag: layer draw batches (DrawArray)
static uint32_t g_vprof[2]; // diag: sprite target list, layer list (us accumulated)
void video_prof_take(uint32_t *out)
{
	out[0] = g_vprof[0];
	out[1] = g_vprof[1];
	out[2] = g_vbatches;
	g_vprof[0] = g_vprof[1] = 0;
	g_vbatches = 0;
}
#endif

// Screen HUD (dev perf text, top left)
static char g_hud[48];

const void *video_atlas_addr(void) { return g_atlas; }

void video_set_hud(const char *text)
{
	strncpy(g_hud, text, sizeof(g_hud) - 1);
	g_hud[sizeof(g_hud) - 1] = 0;
}

// --- text/rectangles (in compose's overlay stage) ---
// Font: pspsdk debug screen font (msx, 8x8, 256 chars, one row per byte, bit7 = left) into a 128x128 8888 texture.
// Cell 0 is replaced with solid white for drawing rectangles
extern unsigned char msx[];
static uint32_t __attribute__((aligned(16))) g_font[128 * 128];

static void font_init(void)
{
	// button glyphs (menu.h REI_GLYPH_*): 0x10 △, 0x11 ○, 0x12 ×, 0x13 □ (overwrite MSX graphic char slots)
	static const unsigned char glyph[4][8] = {
		{ 0x18, 0x18, 0x24, 0x24, 0x42, 0x42, 0xff, 0x00 },
		{ 0x3c, 0x42, 0x81, 0x81, 0x81, 0x81, 0x42, 0x3c },
		{ 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },
		{ 0xff, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xff },
	};
	for (int c = 0; c < 256; c++) {
		const int cx = (c & 15) * 8, cy = (c >> 4) * 8;
		for (int y = 0; y < 8; y++) {
			const unsigned bits = (c >= 0x10 && c <= 0x13) ? glyph[c - 0x10][y] : c ? msx[c * 8 + y] : 0xff;
			for (int x = 0; x < 8; x++)
				g_font[(cy + y) * 128 + cx + x] = (bits & (0x80u >> x)) ? 0xffffffffu : 0x00ffffffu;
		}
	}
	sceKernelDcacheWritebackRange(g_font, sizeof(g_font));
}

struct tcvtx {
	uint16_t u, v;
	uint32_t color;
	int16_t x, y, z, pad;
};
#define TCVTX_TYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)

static void text_begin(void)
{
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 128, 128, 128, g_font);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFlush();
}

static void text_end(void)
{
	sceGuDisable(GU_BLEND);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
}

void video_rect(int x, int y, int w, int h, uint32_t abgr)
{
	struct tcvtx *v = (struct tcvtx *)sceGuGetMemory(2 * sizeof(struct tcvtx));
	v[0] = (struct tcvtx){ 0, 0, abgr, (int16_t)x, (int16_t)y, 0, 0 };
	v[1] = (struct tcvtx){ 8, 8, abgr, (int16_t)(x + w), (int16_t)(y + h), 0, 0 };
	sceGuDrawArray(GU_SPRITES, TCVTX_TYPE, 2, NULL, v);
}

int video_text(int x, int y, uint32_t abgr, int scale, const char *s)
{
	int n = 0;
	for (const char *p = s; *p; p++)
		if (*p != ' ' && *p != '\n')
			n++;
	const int x0 = x;
	if (n) {
		struct tcvtx *v = (struct tcvtx *)sceGuGetMemory(n * 2 * sizeof(struct tcvtx));
		int k = 0;
		for (const char *p = s; *p; p++) {
			const unsigned char c = (unsigned char)*p;
			if (c == '\n') {
				x = x0;
				y += 10 * scale;
				continue;
			}
			if (c != ' ') {
				const uint16_t u = (uint16_t)((c & 15) * 8), t = (uint16_t)((c >> 4) * 8);
				v[k++] = (struct tcvtx){ u, t, abgr, (int16_t)x, (int16_t)y, 0, 0 };
				v[k++] = (struct tcvtx){ (uint16_t)(u + 8), (uint16_t)(t + 8), abgr, (int16_t)(x + 8 * scale), (int16_t)(y + 8 * scale), 0, 0 };
			}
			x += 8 * scale;
		}
		sceGuDrawArray(GU_SPRITES, TCVTX_TYPE, k, NULL, v);
	}
	return x - x0;
}

// --- compose: border -> game screen (scale/filter) -> scanlines -> text ---
static void (*g_overlay)(void);

void video_set_overlay(void (*fn)(void))
{
	g_overlay = fn;
}

// Other boards (main.c run_other_board): GE 5650 buffer drawn by the core on the CPU (main memory, rotation flags REI_SCREEN_*)
static const uint16_t *g_ext_buf;
static int g_ext_w, g_ext_h, g_ext_stride, g_ext_flags;
static const uint16_t *g_ext_idx, *g_ext_clut; // (index mode, see video_set_game_index below)
// M107 GE mode (ge_m107.c, 2026-10-15): index buffer = sprite A, tile layers CALL the GE list built by the ME
static const uint32_t *g_gem_list;
static const uint16_t *g_gem_b;
static gem_result g_gem_res;
static const uint32_t *g_gm72_list; // (M72 family GE mode, see video_set_game_gm72 below)

void video_set_game_buffer(const uint16_t *buf, int w, int h, int stride, int flags)
{
	g_ext_buf = buf;
	g_ext_w = w;
	g_ext_h = h;
	g_ext_stride = stride;
	g_ext_flags = flags;
	g_ext_idx = NULL;
	g_gem_list = NULL;
	g_gm72_list = NULL;
}

// Other-board index mode (ME draws color indices, GE colors them via CLUT — removes the ME palette transfer, 2026-10-14):
// move indices to VRAM (GE copy), then as in the M92 path bank (index bit8-10) -> stencil, per-bank 256-color CLUT into the 8888 offscreen,
// then compose that offscreen to the screen with rotation. Offscreen and index copy use the M92 offscreen area (OFF_OFF to ART_OFF)
static int g_ext_banks, g_ext_os;
int g_idx_nocopy; // (diag)
int g_present_wb; // (test) video_present writes back the data cache before sending the list (host0:/rei_presentwb)
int g_ge_skip; // (diag, wrong output) skip GE compose stages — 1 bank stencil, 2 bank color (first bank only), 4 compose, 8 offscreen clear // bank count, offscreen width (multiple of 64)
static unsigned g_ext_idx_off;    // index copy location in VRAM
int video_index_fits(int w, int h)
{
	const int os = (w + 63) & ~63;
	const unsigned need = (unsigned)os * SCR_H * 4 + 2048 + (unsigned)os * h * 2; // (clear paints 480x272 regardless of scissor)
	return (w & 7) == 0 && w <= 512 && h <= 256 && OFF_OFF + need <= (unsigned)ART_OFF;
}
void video_set_game_index(const uint16_t *idx, const uint16_t *clut, int nclut, int w, int h, int flags)
{
	g_ext_buf = idx; // (compose path selection: other boards)
	g_gem_list = NULL;
	g_gm72_list = NULL;
	g_ext_w = w;
	g_ext_h = h;
	g_ext_flags = flags;
	g_ext_idx = idx;
	g_ext_clut = clut;
	g_ext_banks = (nclut + 255) / 256;
	if (g_ext_banks > 8) g_ext_banks = 8;
	if (g_ext_banks < 1) g_ext_banks = 1;
	g_ext_os = (w + 63) & ~63;
	// (clear paints 480x272 regardless of scissor: if the offscreen is narrower, it reaches further — the copy goes after 272 lines + 2KB.
	//  placed right after, the copy's top lines got cleared and cosmccop's sky was gray, 2026-10-14)
	g_ext_idx_off = (OFF_OFF + (unsigned)g_ext_os * SCR_H * 4 + 2048 + 63) & ~63u;
}
void video_set_game_gem(const uint16_t *a, const uint16_t *b, const uint16_t *clut, const uint32_t *list, const gem_result *res,
			int w, int h, int flags)
{
	video_set_game_index(a, clut, 0x800, w, h, flags);
	g_gem_list = list;
	g_gem_b = b;
	// (ME-written results: drop stale main cache lines before reading — main never writes this memory)
	sceKernelDcacheInvalidateRange((void *)((uintptr_t)res & ~63u), 128);
	g_gem_res = *res;
}
// M72 family GE mode (ge_m72.c, 2026-10-17): index buffer = sprites (color index), tile layers CALL the list built by the ME.
// The list draws while switching layer atlas (T4), per-mask CLUTs and sprite texture (VRAM copy of indices, T16)
static uint16_t g_gm72_masks[GM72_MAX_MASKS];
static int g_gm72_nmasks;
static uint16_t __attribute__((aligned(64))) g_gm72_clut[(GM72_MAX_MASKS + 1) * 256];
static uint8_t *g_gm72_atlas;
int video_gm72_atlas_init(const uint8_t *const gfx[2], const int ntiles[2], uint32_t out[2])
{
	const int wins0 = (ntiles[0] + 4095) / 4096, wins1 = (ntiles[1] + 4095) / 4096;
	const size_t bytes = (size_t)(wins0 + wins1) * GEM_ATLAS_WIN_BYTES;
	free(g_gm72_atlas);
	g_gm72_atlas = (uint8_t *)memalign(64, bytes);
	if (!g_gm72_atlas)
		return -1;
	memset(g_gm72_atlas, 0, bytes);
	for (int l = 0; l < 2; l++) {
		uint8_t *at = g_gm72_atlas + (l ? (size_t)wins0 * GEM_ATLAS_WIN_BYTES : 0);
		for (int t = 0; t < ntiles[l]; t++) {
			uint8_t *base = at + (size_t)(t >> 12) * GEM_ATLAS_WIN_BYTES;
			const int j = t & 4095, tx = (j & 63) * 8, ty = (j >> 6) * 8;
			const uint8_t *src = gfx[l] + (size_t)t * 64;
			for (int y = 0; y < 8; y++) {
				uint8_t *row = base + ((ty + y) * 512 + tx) / 2;
				for (int x = 0; x < 8; x += 2)
					row[x / 2] = (uint8_t)((src[y * 8 + x] & 15) | ((src[y * 8 + x + 1] & 15) << 4));
			}
		}
	}
	out[0] = (uint32_t)(uintptr_t)g_gm72_atlas;
	out[1] = (uint32_t)(uintptr_t)(g_gm72_atlas + (size_t)wins0 * GEM_ATLAS_WIN_BYTES);
	sceKernelDcacheWritebackAll();
	return 0;
}
uint32_t video_gm72_clut_addr(void) { return (uint32_t)(uintptr_t)g_gm72_clut; }
int video_index_copy_tbw(int w) { return (w + 63) & ~63; }
uint32_t video_index_copy_addr(int w) // (same computation as g_ext_idx_off in video_set_game_index)
{
	const unsigned os = (unsigned)video_index_copy_tbw(w);
	return 0x04000000u + ((OFF_OFF + os * SCR_H * 4 + 2048 + 63) & ~63u);
}
void video_set_game_gm72(const uint16_t *a, const uint16_t *clut, const uint32_t *list, const uint16_t *masks, int nmasks, int w, int h,
			 int flags)
{
	video_set_game_index(a, clut, 0x200, w, h, flags);
	g_gm72_list = list;
	g_gm72_nmasks = nmasks > GM72_MAX_MASKS ? GM72_MAX_MASKS : nmasks;
	for (int i = 0; i < g_gm72_nmasks; i++)
		g_gm72_masks[i] = masks[i];
}
#ifdef REI_LOG_ENABLED
// (dev) check that ge_m107.c's hand-built GE commands match pspsdk sceGu: do the same via sceGu into a CALL list and return the words
int video_gem_sgu_words(uint32_t *out, int max)
{
	static unsigned int __attribute__((aligned(64))) tl[256]; // (64: same reason as g_list above)
	memset(tl, 0, sizeof(tl));
	sceKernelDcacheWritebackInvalidateRange(tl, sizeof(tl)); // (sceGu writes lists via uncached addresses)
	sceGuStart(GU_CALL, tl);
	sceGuEnable(GU_SCISSOR_TEST); // (sceGuScissor emits nothing in a context where it is disabled)
	sceGuScissor(0, 10, 320, 30);
	sceGuStencilFunc(GU_ALWAYS, 0x80, 0xff);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
	sceGuClutLoad(32, (const void *)0x08912340);
	sceGuClutMode(GU_PSM_5551, 0, 0x0f, 7);
	sceGuTexImage(0, 512, 512, 512, (const void *)0x09876540);
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, (const void *)0x09abcde0);
	sceGuFinish();
	const volatile unsigned int *u = (const volatile unsigned int *)((uintptr_t)tl | 0x40000000u);
	int k = 0;
	for (int i = 0; i < 256 && k < max && (i < 4 || u[i] || u[i - 1]); i++)
		out[k++] = u[i];
	return k;
}
#endif
void video_gem_cluts(uint32_t out[2])
{
	out[0] = (uint32_t)(uintptr_t)g_pat_clut[GE_PAT_1_15];
	out[1] = (uint32_t)(uintptr_t)g_pat_clut[GE_PAT_0_15];
}
void video_layers_free(void)
{
	free(g_atlas);
	g_atlas = NULL;
	free(g_gm72_atlas);
	g_gm72_atlas = NULL;
}
static int g_ext_rect_w, g_ext_rect_h;
static void ext_rect(int w, int h);
static void ext_rect_cur(void) { ext_rect(g_ext_rect_w, g_ext_rect_h); }
static void ext_rect(int w, int h) // offscreen top-left w x h with the texture at the same position (64-texel strips)
{
	const int strips = (w + 63) / 64;
	struct vtx *v = (struct vtx *)sceGuGetMemory(strips * 2 * sizeof(struct vtx));
	for (int i = 0; i < strips; i++) {
		const int x0 = i * 64, x1 = (i + 1) * 64 < w ? (i + 1) * 64 : w;
		v[i * 2 + 0] = (struct vtx){ (uint16_t)x0, 0, (int16_t)x0, 0, 0 };
		v[i * 2 + 1] = (struct vtx){ (uint16_t)x1, (uint16_t)h, (int16_t)x1, (int16_t)h, 0 };
	}
	sceGuDrawArray(GU_SPRITES, VTX_TYPE, strips * 2, NULL, v);
}
static int pow2_at_least(int v);
static void draw_sprites(const void *tex);

// M107 GE mode: layers (ME list) -> sprite B (rare, main-RAM texture) -> sprite A (VRAM copy, same compose as M92 G2).
// Stencil bit7 = layer high pixel. B keeps bit7 and writes bank + 1 to the low bits for coloring; A's draw_sprites
// skips "hidden sprite && high" pixels -> those show B (if any) or the layer (rei_m107ge.h rules, harness --ge-m107)
static void ext_gem_offscreen(const void *vidx)
{
	const int w = g_ext_w, h = g_ext_h;
	{ // palette: ME-written 5650 -> 5551 (pattern and sprite CLUTs rebuilt only for changed ranges)
		static uint32_t pal[0x800];
		sceKernelDcacheInvalidateRange((void *)g_ext_clut, 0x800 * 2);
		for (int i = 0; i < 0x800; i++) {
			const uint32_t v = g_ext_clut[i];
			pal[i] = 0x8000u | (v & 31) | (((v >> 6) & 31) << 5) | (((v >> 11) & 31) << 10);
		}
		video_submit_clut(pal);
	}
	// (the list changes the scissor: scissor must be enabled in this context so later sceGuScissor emits commands — in a disabled context
	//  nothing was emitted and the list's scissor persisted into compose)
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, g_ext_os);
	sceGuScissor(0, 0, w, h);
	sceGuClearStencil(0);
	sceGuClear(GU_STENCIL_BUFFER_BIT); // (keep color: areas no layer covers keep the previous frame, like FBNeo)
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_T4, 0, 0, 0); // (atlas T4, video_gem_atlas_init)
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_BLEND);
	sceGuEnable(GU_ALPHA_TEST);
	sceGuAlphaFunc(GU_GREATER, 0, 0xff);
	sceGuEnable(GU_STENCIL_TEST); // (8888 alpha is the stencil: left enabled, the list picks values)
	sceGuPixelMask(0);
	if (!(g_ge_skip & 16))
		sceGuCallList(g_gem_list);
	sceGuScissor(0, 0, w, h);
	sceGuDisable(GU_ALPHA_TEST);
	const gem_result *r = &g_gem_res;
	if (r->nb > 0 && r->by1 > r->by0) { // sprite B
		sceGuTexMode(GU_PSM_5551, 0, 0, 0);
		sceGuTexImage(0, 512, 256, GAME_W, g_gem_b);
		sceGuTexFlush();
		sceGuEnable(GU_COLOR_TEST);
		sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
		sceGuPixelMask(0x80ffffffu); // (RGB and stencil bit7 untouched)
		for (int b = 0; b < 8; b++) {
			if (!((r->bbanks >> b) & 1))
				continue;
			sceGuColorFunc(GU_EQUAL, 0x100000 | bank_color(b), 0x100000 | BANK_MASK);
			sceGuStencilFunc(GU_ALWAYS, b + 1, 0xff);
			draw_game_rows(r->by0, r->by1);
		}
		sceGuPixelMask(0);
		sceGuDisable(GU_COLOR_TEST);
		sceGuTexMode(GU_PSM_T16, 0, 0, 0);
		sceGuTexImage(0, 512, 256, GAME_W, g_gem_b);
		sceGuTexFlush();
		sceGuClutMode(GU_PSM_5551, 0, 0xff, 0);
		sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
		for (int b = 0; b < 8; b++) {
			if (!((r->bbanks >> b) & 1))
				continue;
			sceGuClutLoad(256 / 16, g_spr_clut + b * 256);
			sceGuStencilFunc(GU_EQUAL, b + 1, 0x7f);
			draw_game_rows(r->by0, r->by1);
		}
	}
	for (int b = 0; b < 8; b++) {
		g_sbank[b].y0 = r->ay0[b];
		g_sbank[b].y1 = r->ay1[b];
	}
	g_shid.y0 = r->hy0;
	g_shid.y1 = r->hy1;
	draw_sprites(vidx);
	sceGuDisable(GU_STENCIL_TEST);
}

// M72 family GE mode: clear offscreen to color 0 and CALL the ME list (layer back pass -> sprites -> front pass, ge_m72.c).
// CLUT from that frame's palette (ME-written 5650): per mask, layer palette 0x100-0x1ff (masked pens alpha 0), then sprites 0x000-0x0ff
static void ext_gm72_offscreen(void)
{
	const int w = g_ext_w, h = g_ext_h, nm = g_gm72_nmasks;
	static uint16_t p[0x200];
	sceKernelDcacheInvalidateRange((void *)((uintptr_t)g_ext_clut & ~63u), 0x200 * 2 + 64);
	for (int i = 0; i < 0x200; i++) {
		const uint32_t v = g_ext_clut[i];
		p[i] = (uint16_t)((v & 31) | (((v >> 6) & 31) << 5) | (((v >> 11) & 31) << 10));
	}
	for (int k = 0; k < nm; k++) {
		uint16_t *c = g_gm72_clut + k * 256;
		const uint32_t m = g_gm72_masks[k];
		for (int i = 0; i < 256; i++)
			c[i] = (uint16_t)(p[0x100 + i] | (((m >> (i & 15)) & 1) ? 0 : 0x8000));
	}
	{
		uint16_t *c = g_gm72_clut + nm * 256;
		for (int i = 0; i < 256; i++)
			c[i] = (uint16_t)(p[i] | ((i & 15) ? 0x8000 : 0));
	}
	sceKernelDcacheWritebackRange(g_gm72_clut, sizeof(g_gm72_clut));
	const uint32_t r5 = p[0] & 31, g5 = (p[0] >> 5) & 31, b5 = (p[0] >> 10) & 31;
	const uint32_t c0 = 0xff000000u | (((b5 << 3) | (b5 >> 2)) << 16) | (((g5 << 3) | (g5 >> 2)) << 8) | ((r5 << 3) | (r5 >> 2));
	sceGuEnable(GU_SCISSOR_TEST); // (the list changes the scissor — see ext_gem_offscreen)
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, g_ext_os);
	sceGuScissor(0, 0, w, h);
	sceGuClearColor(c0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_T4, 0, 0, 0);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuEnable(GU_ALPHA_TEST);
	sceGuAlphaFunc(GU_GREATER, 0, 0xff);
	sceGuPixelMask(0);
	if (!(g_ge_skip & 16))
		sceGuCallList(g_gm72_list);
	sceGuScissor(0, 0, w, h);
	sceGuDisable(GU_ALPHA_TEST);
}

static void ext_index_to_offscreen(void)
{
	const int w = g_ext_w, h = g_ext_h;
	const void *vidx = (const void *)(0x04000000u + g_ext_idx_off);
	const int tw = pow2_at_least(w), th = pow2_at_least(h);
	int tbw = g_ext_os; // (width of the VRAM copy)
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	if (g_idx_nocopy) {
		vidx = g_ext_idx; // (diag: main RAM directly)
		tbw = w;
	} else {
		// indices -> VRAM: read as 5551 and draw into a 5551 buffer (5551 -> 8888 -> 5551 preserves values). Reading a main-RAM texture many times
		// is slow. (sceGuCopyImage left top lines as 0 — 2026-10-14 on hardware, gray band in cosmccop's sky)
		sceGuDrawBufferList(GU_PSM_5551, (void *)g_ext_idx_off, g_ext_os);
		sceGuScissor(0, 0, w, h);
		sceGuTexMode(GU_PSM_5551, 0, 0, 0);
		sceGuTexImage(0, tw, th, w, g_ext_idx);
		sceGuTexFlush();
		ext_rect(w, h);
		sceGuTexSync();
	}
	if (g_gem_list) {
		ext_gem_offscreen(vidx);
		return;
	}
	if (g_gm72_list) {
		ext_gm72_offscreen();
		return;
	}
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, g_ext_os);
	sceGuScissor(0, 0, w, h);
	sceGuClearColor(0xff000000);
	sceGuClearStencil(0);
	if (!(g_ge_skip & 8))
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
	if (g_ext_banks > 1 && !(g_ge_skip & 1)) { // 1) bank number -> stencil (not needed with one bank)
		sceGuTexMode(GU_PSM_5551, 0, 0, 0);
		sceGuTexImage(0, tw, th, tbw, vidx);
		sceGuTexFlush();
		g_ext_rect_w = w, g_ext_rect_h = h;
		bank_bits_to_stencil(g_ext_banks, ext_rect_cur);
	}
	// 2) color via per-bank CLUT
	sceGuTexMode(GU_PSM_T16, 0, 0, 0);
	sceGuTexImage(0, tw, th, tbw, vidx);
	sceGuTexFlush();
	sceGuClutMode(GU_PSM_5650, 0, 0xff, 0);
	if (g_ext_banks > 1)
		sceGuEnable(GU_STENCIL_TEST);
	for (int b = 0; b < ((g_ge_skip & 2) ? 1 : g_ext_banks); b++) {
		sceGuClutLoad(256 / 16, g_ext_clut + b * 256);
		if (g_ext_banks > 1)
			sceGuStencilFunc(GU_EQUAL, b, 0x07);
		ext_rect(w, h);
	}
	sceGuDisable(GU_STENCIL_TEST);
}

static int pow2_at_least(int v)
{
	int p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

void video_game_rect(int *x, int *y, int *w, int *h)
{
	if (g_ext_buf) { // other boards: based on visible size (after rotation)
		const int vert = (g_ext_flags & REI_SCREEN_VERTICAL) != 0;
		const int dw = vert ? g_ext_h : g_ext_w, dh = vert ? g_ext_w : g_ext_h;
		switch (g_settings.aspect) {
		case REI_ASPECT_FULL:
			*x = 0, *y = 0, *w = SCR_W, *h = SCR_H;
			break;
		case REI_ASPECT_1X:
			*w = dw < SCR_W ? dw : SCR_W;
			*h = dh < SCR_H ? dh : SCR_H;
			*x = (SCR_W - *w) / 2, *y = (SCR_H - *h) / 2;
			break;
		default: // fit 272 height, 4:3 (3:4 for vertical games)
			*h = SCR_H;
			*w = vert ? SCR_H * 3 / 4 : SCR_H * 4 / 3;
			*x = (SCR_W - *w) / 2, *y = 0;
			break;
		}
		return;
	}
	switch (g_settings.aspect) {
	case REI_ASPECT_FULL:
		*x = 0, *y = 0, *w = SCR_W, *h = SCR_H;
		break;
	case REI_ASPECT_1X:
		*x = (SCR_W - GAME_W) / 2, *y = (SCR_H - GAME_H) / 2, *w = GAME_W, *h = GAME_H;
		break;
	default: // fit 272 height, 4:3 -> 362x272 (matches the game area of the border image)
		*x = 59, *y = 0, *w = 362, *h = SCR_H;
		break;
	}
}

uint16_t *video_spare565(void) { return (uint16_t *)(0x44000000u + OFF_OFF); }
const uint16_t *video_spare565_tex(void) { return (const uint16_t *)(0x04000000u + OFF_OFF); }
void video_spare_clear(void)
{
	video_sync(); // (the GE may still read it)
	memset((void *)(0x44000000u + OFF_OFF), 0, OFF_BYTES);
}

static uint32_t border_color(void)
{
	static const uint32_t c[REI_BCOL_COUNT] = { // 0xAABBGGRR
		0xff3c1814u, 0xff2c2828u, 0xff141048u, 0xff1c3410u, 0xff48142cu, 0xff18245au,
	};
	return c[g_settings.border_color];
}

// Stretch a texture onto a screen rectangle in 64-texel strips (friendly to the GE texture cache)
static void draw_stretched(int tw, int th, int x, int y, int w, int h)
{
	const int strips = (tw + 63) / 64;
	struct vtx *v = (struct vtx *)sceGuGetMemory(strips * 2 * sizeof(struct vtx));
	for (int i = 0; i < strips; i++) {
		const int u0 = i * 64, u1 = (i + 1) * 64 < tw ? (i + 1) * 64 : tw;
		v[i * 2 + 0] = (struct vtx){ (uint16_t)u0, 0, (int16_t)(x + u0 * w / tw), (int16_t)y, 0 };
		v[i * 2 + 1] = (struct vtx){ (uint16_t)u1, (uint16_t)th, (int16_t)(x + u1 * w / tw), (int16_t)(y + h), 0 };
	}
	sceGuDrawArray(GU_SPRITES, VTX_TYPE, strips * 2, NULL, v);
}

static void compose(void)
{
	int gx, gy, gw, gh;
	if (g_ext_idx)
		ext_index_to_offscreen(); // (index mode: color the offscreen, then put it on screen)
	video_game_rect(&gx, &gy, &gw, &gh);
	sceGuDrawBufferList(GU_PSM_5650, g_draw_fb, BUF_W);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_COLOR_TEST);
	sceGuDisable(GU_BLEND);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	// border (fullscreen: the game covers everything)
	if (g_settings.aspect != REI_ASPECT_FULL) {
		const int art = g_settings.aspect == REI_ASPECT_1X ? 1 : 0;
		if (g_settings.border == REI_BORDER_ARTWORK && g_art_blank && rei_menu_accent() != g_blank_acc)
			blank_tint(); // (the theme changed in the menu)
		if (g_settings.border == REI_BORDER_ARTWORK && g_art_ok[art]) {
			sceGuTexMode(GU_PSM_5650, 0, 0, 0);
			sceGuTexImage(0, 512, 512, BUF_W, (const void *)(0x04000000u + ART_OFF + art * ART_BYTES));
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			sceGuTexFlush();
			draw_stretched(SCR_W, SCR_H, 0, 0, SCR_W, SCR_H);
		} else {
			sceGuClearColor(g_settings.border == REI_BORDER_COLOR ? border_color() : 0xff000000u);
			sceGuClear(GU_COLOR_BUFFER_BIT);
		}
	}
	if (g_ext_buf) { // other boards: rotate the CPU-drawn buffer (same orientation as Vita: vertical 90 deg CCW, flipped 180 deg)
		if (g_ext_idx) {
			sceGuTexMode(GU_PSM_8888, 0, 0, 0);
			sceGuTexImage(0, pow2_at_least(g_ext_os), pow2_at_least(g_ext_h), g_ext_os, OFF_TEX);
		} else {
			sceGuTexMode(GU_PSM_5650, 0, 0, 0);
			sceGuTexImage(0, pow2_at_least(g_ext_stride), pow2_at_least(g_ext_h), g_ext_stride, g_ext_buf);
		}
		sceGuTexFilter(g_settings.filter == REI_FILTER_LINEAR ? GU_LINEAR : GU_NEAREST,
			g_settings.filter == REI_FILTER_LINEAR ? GU_LINEAR : GU_NEAREST);
		sceGuTexFlush();
		const int W = g_ext_w, H = g_ext_h;
		// texture corners for the four screen corners (top-left, top-right, bottom-left, bottom-right)
		int u[4] = { 0, W, 0, W }, v[4] = { 0, 0, H, H };
		const int vert = (g_ext_flags & REI_SCREEN_VERTICAL) != 0, flip = (g_ext_flags & REI_SCREEN_FLIPPED) != 0;
		if (vert && !flip) { // 90 deg CCW: image top-right is screen top-left
			const int uu[4] = { W, W, 0, 0 }, vv[4] = { 0, H, 0, H };
			for (int i = 0; i < 4; i++) u[i] = uu[i], v[i] = vv[i];
		} else if (vert && flip) { // 90 deg CW
			const int uu[4] = { 0, 0, W, W }, vv[4] = { H, 0, H, 0 };
			for (int i = 0; i < 4; i++) u[i] = uu[i], v[i] = vv[i];
		} else if (flip) { // 180 deg
			const int uu[4] = { W, 0, W, 0 }, vv[4] = { H, H, 0, 0 };
			for (int i = 0; i < 4; i++) u[i] = uu[i], v[i] = vv[i];
		}
		// Split screen width into 64-texel texture strips (same reason as the M92 path's draw_stretched: stretching one rectangle made the GE
		// sweep the main-RAM texture widely per screen line, flushing the texture cache — rtype compose 14ms, 2026-10-13 on hardware).
		// One strip per 64 texels along the texture axis that varies with screen x (u for horizontal, v for vertical screens), two triangles each
		const int T = vert ? H : W; // texels traversed along screen width
		const int strips = (T + 63) / 64;
		struct vtx *q = (struct vtx *)sceGuGetMemory((size_t)strips * 6 * sizeof(struct vtx));
		for (int k = 0; k < strips; k++) {
			const int t0 = k * 64, t1 = (k + 1) * 64 < T ? (k + 1) * 64 : T;
			const int x0 = gx + t0 * gw / T, x1 = gx + t1 * gw / T;
			// texture coords at t0, t1 along the top (0->1) and bottom (2->3) edges
			const int ut0 = u[0] + (u[1] - u[0]) * t0 / T, vt0 = v[0] + (v[1] - v[0]) * t0 / T;
			const int ut1 = u[0] + (u[1] - u[0]) * t1 / T, vt1 = v[0] + (v[1] - v[0]) * t1 / T;
			const int ub0 = u[2] + (u[3] - u[2]) * t0 / T, vb0 = v[2] + (v[3] - v[2]) * t0 / T;
			const int ub1 = u[2] + (u[3] - u[2]) * t1 / T, vb1 = v[2] + (v[3] - v[2]) * t1 / T;
			struct vtx *s = q + k * 6;
			s[0] = (struct vtx){ (uint16_t)ut0, (uint16_t)vt0, (int16_t)x0, (int16_t)gy, 0 };
			s[1] = (struct vtx){ (uint16_t)ut1, (uint16_t)vt1, (int16_t)x1, (int16_t)gy, 0 };
			s[2] = (struct vtx){ (uint16_t)ub0, (uint16_t)vb0, (int16_t)x0, (int16_t)(gy + gh), 0 };
			s[3] = s[1];
			s[4] = (struct vtx){ (uint16_t)ub1, (uint16_t)vb1, (int16_t)x1, (int16_t)(gy + gh), 0 };
			s[5] = s[2];
		}
		if (!(g_ge_skip & 4))
			sceGuDrawArray(GU_TRIANGLES, VTX_TYPE, strips * 6, NULL, q);
	} else {
	// game screen (offscreen 320x240 -> per aspect ratio)
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 512, 256, BUF_W, OFF_TEX);
	if (g_settings.filter == REI_FILTER_LINEAR)
		sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	else
		sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFlush();
	draw_stretched(GAME_W, GAME_H, gx, gy, gw, gh);
	}
	// text (dev HUD, menu, FPS, notices)
	if (g_hud[0] || g_overlay) {
		text_begin();
		if (g_hud[0]) {
			video_rect(0, 0, 8 * 8 + 4, 34, 0xa0000000u);
			video_text(2, 2, 0xff40ffffu, 1, g_hud);
		}
		if (g_overlay)
			g_overlay();
		text_end();
	}
}

#ifdef REI_LOG_ENABLED
// (diag) host0:/rei_dcap "start count": from the start-th present for count frames, collect the GE-composited screen (565, 480x272) in memory, then
// write host0:/dc_<n>.bin (8-byte header: vcount passed, video_dcap_tag). Investigates visible flicker — frames are not delayed while dumping
static uint8_t *g_dci; // (diag) index copy / stencil buffer (dcap)
static int g_dci_n;
uint32_t video_dcap_tag; // (main: tag of the frame this present sends — ME frame number etc. Stored in the capture header)
static void dcap(void)
{
	static int from = -2, n, cnt;
	static uint32_t prev_tag, *tags;
	static uint16_t *buf;
	static uint32_t *vc;
	if (from == -2) {
		from = -1;
		SceUID fd = sceIoOpen("host0:/rei_dcap", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[32] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			if (sscanf(b, "%d %d", &from, &n) != 2 || n <= 0) from = -1;
		}
	}
	cnt++;
	const uint32_t tag = prev_tag; // (the screen now fully composited is from the list the previous present sent)
	prev_tag = video_dcap_tag;
	if (from < 0 || cnt < from)
		return;
	if (!buf) {
		buf = (uint16_t *)malloc((size_t)n * SCR_W * SCR_H * 2);
		vc = (uint32_t *)malloc((size_t)n * 4);
		tags = (uint32_t *)malloc((size_t)n * 4);
		if (!buf || !vc || !tags) {
			from = -1;
			return;
		}
	}
	const int k = cnt - from;
	if (k < n) {
		const uint16_t *src = (const uint16_t *)(0x44000000u + g_draw_i * FB_BYTES);
		for (int y = 0; y < SCR_H; y++)
			memcpy(buf + ((size_t)k * SCR_H + y) * SCR_W, src + y * BUF_W, SCR_W * 2);
		vc[k] = (uint32_t)sceDisplayGetVcount();
		tags[k] = tag;
		if (g_ext_idx && !g_gem_list && !g_idx_nocopy && g_ext_w * g_ext_h <= 512 * 256 && k < 6) {
			// (index mode) collect the index copy the GE moved to VRAM and the offscreen stencil (8888 alpha) in memory, at the end
			// write host0:/dci_<n>.bin (header: w h, copy w*h*2, stencil w*h). File writes during the dump changed timing and hid the symptom
			if (!g_dci)
				g_dci = (uint8_t *)malloc((size_t)6 * 512 * 256 * 3);
			if (g_dci) {
				uint8_t *o = g_dci + (size_t)k * 512 * 256 * 3;
				const uint16_t *s = (const uint16_t *)(0x44000000u + g_ext_idx_off);
				const uint32_t *st = (const uint32_t *)(0x44000000u + OFF_OFF);
				for (int y = 0; y < g_ext_h; y++) {
					memcpy(o + (size_t)y * g_ext_w * 2, s + y * g_ext_os, g_ext_w * 2);
					for (int x = 0; x < g_ext_w; x++)
						o[(size_t)g_ext_w * g_ext_h * 2 + y * g_ext_w + x] = (uint8_t)(st[y * g_ext_os + x] >> 24);
				}
				g_dci_n = k + 1;
			}
		}
		return;
	}
	for (int i = 0; i < g_dci_n && g_dci; i++) {
		char p[48];
		snprintf(p, sizeof(p), "host0:/dci_%04d.bin", i);
		SceUID o = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if (o < 0)
			continue;
		const int hdr[2] = { g_ext_w, g_ext_h };
		sceIoWrite(o, hdr, sizeof(hdr));
		sceIoWrite(o, g_dci + (size_t)i * 512 * 256 * 3, (size_t)g_ext_w * g_ext_h * 3);
		sceIoClose(o);
	}
	free(g_dci);
	g_dci = NULL;
	for (int i = 0; i < n; i++) {
		char p[48];
		snprintf(p, sizeof(p), "host0:/dc_%04d.bin", i);
		SceUID o = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if (o < 0)
			continue;
		const uint32_t hdr[2] = { vc[i], tags[i] };
		sceIoWrite(o, hdr, sizeof(hdr));
		sceIoWrite(o, buf + (size_t)i * SCR_W * SCR_H, SCR_W * SCR_H * 2);
		sceIoClose(o);
	}
	free(buf);
	free(vc);
	free(tags);
	buf = NULL;
	from = -1;
}
#endif

void video_present(void)
{
	video_sync();
#ifdef REI_LOG_ENABLED
	if (g_sent)
		dcap();
#endif
	if (g_sent)
		video_flip();
	unsigned int *list = g_list[g_list_cur];
	sceGuStart(GU_SEND, list);
	g_list_cur ^= 1;
	compose();
	sceGuFinish();
	if (g_present_wb)
		sceKernelDcacheWritebackAll();
	sceGuSendList(GU_TAIL, list, NULL);
	g_sent = 1;
}

// G3: the GE draws sprites into the index buffer (VRAM, drawn as 5551). Drawing in reverse core order leaves the first-drawn
// sprite on top (same result as core RenderTileS "first-claimed pixel can't be overwritten" — verified with tools/harness --ge-steps
// over 3600 frames). The CLUT maps pixel -> index value; value 0 (transparent) is discarded by color test. 5551 -> 5551 writes unchanged
static void draw_sprite_target(const rei_spr_tile *t, int n, const uint8_t *gfx)
{
	sceGuDrawBufferList(GU_PSM_5551, (void *)INDEX_OFF, GAME_W);
	sceGuScissor(0, 0, GAME_W, GAME_H);
	sceGuDisable(GU_STENCIL_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuClearColor(0);
	sceGuClearStencil(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuEnable(GU_COLOR_TEST);
	sceGuColorFunc(GU_NOTEQUAL, 0, 0x100000); // transparent without bit 0x800 (B bit4)
	int cur_win = -1, cur_blk = -1, cur_csa = -1;
	for (int i = n - 1; i >= 0; i--) {
		const rei_spr_tile *s = &t[i];
		if (sceGuCheckList() > (int)sizeof(g_list[0]) - 8192) {
			g_overflow = 1;
			break;
		}
		int win = (int)(s->code >> 5); // 16x512 texture = 32 tiles
		int blk = ((s->flags & 4) ? 4 : 0) | (s->color >> 5); // 512 CLUT entries = 32 colors
		int csa = s->color & 31;
		if (win != cur_win) {
			sceGuTexImage(0, 16, 512, 16, gfx + (size_t)win * 32 * 256);
			cur_win = win;
		}
		if (blk != cur_blk) {
			sceGuClutLoad(512 / 16, g_spr_idx + blk * 512);
			cur_blk = blk;
		}
		if (csa != cur_csa) {
			sceGuClutMode(GU_PSM_5551, 0, 0x0f, csa);
			cur_csa = csa;
		}
		uint16_t u0 = 0, u1 = 16, v0 = (uint16_t)((s->code & 31) * 16), v1 = v0 + 16;
		if (s->flags & 1) {
			u0 = 16;
			u1 = 0;
		}
		if (s->flags & 2) {
			uint16_t k = v0;
			v0 = v1;
			v1 = k;
		}
		struct vtx *v = (struct vtx *)sceGuGetMemory(2 * sizeof(struct vtx));
		v[0] = (struct vtx){ u0, v0, s->x, s->y, 0 };
		v[1] = (struct vtx){ u1, v1, (int16_t)(s->x + 16), (int16_t)(s->y + 16), 0 };
		sceGuDrawArray(GU_SPRITES, VTX_TYPE, 2, NULL, v);
	}
	sceGuDisable(GU_COLOR_TEST);
	sceGuTexSync();
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, BUF_W);
	SCISSOR_GAME();
}

#ifdef REI_PROFILE
#endif
// Draw consecutive tiles with the same texture window/palette in one go
static int g_batch_first, g_batch_count;
static void batch_flush(void)
{
	if (g_batch_count) {
#ifdef REI_PROFILE
		g_vbatches++;
#endif
		// (vertices are written back to memory all at once before sending the list: video_submit_list)
		sceGuDrawArray(GU_SPRITES, VTX_TYPE, g_batch_count * 2, NULL, &g_vpool[g_batch_first]);
	}
	g_batch_first = g_nvtx;
	g_batch_count = 0;
}

// Layer run sort: entry = key (14 bits) << 16 | tile index in run. A run's tiles never exceed the open layer limit (LAYER_TILES)
#define LAYER_SORT_MAX 12288
static uint32_t g_sort_a[LAYER_SORT_MAX], g_sort_b[LAYER_SORT_MAX];
static void radix_sort(int m)
{
	for (int shift = 16; shift < 30; shift += 7) {
		uint32_t *src = shift == 16 ? g_sort_a : g_sort_b, *dst = shift == 16 ? g_sort_b : g_sort_a;
		int cnt[128] = { 0 };
		for (int i = 0; i < m; i++)
			cnt[(src[i] >> shift) & 127]++;
		for (int i = 0, sum = 0; i < 128; i++) {
			int c = cnt[i];
			cnt[i] = sum;
			sum += c;
		}
		for (int i = 0; i < m; i++)
			dst[cnt[(src[i] >> shift) & 127]++] = src[i];
	}
}

static void draw_fill(int y0, int y1, uint32_t color)
{
	struct cvtx *v = (struct cvtx *)sceGuGetMemory(2 * sizeof(struct cvtx));
	v[0] = (struct cvtx){ color, GAME_X, (int16_t)(GAME_Y + y0), 0, 0 };
	v[1] = (struct cvtx){ color, GAME_X + GAME_W, (int16_t)(GAME_Y + y1), 0, 0 };
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_ALPHA_TEST);
}

// Layer draw state (texture window/CLUT/palette last sent to the GE)
static struct {
	int win, csa;
	const void *clut;
} g_ls;

static void layer_set_clut(const uint16_t *clut)
{
	if (clut != g_ls.clut) {
		sceGuClutLoad(512 / 16, clut); // block = 32 bytes = 16 16-bit colors
		g_ls.clut = clut;
	}
}

// Switch to the tile's texture window/palette (flush the previous batch if changed)
static inline void layer_set_tex(int win, int csa)
{
	if (win != g_ls.win || csa != g_ls.csa) {
		batch_flush();
		if (win != g_ls.win) {
			sceGuTexImage(0, 512, 512, 512, g_atlas + (size_t)win * ATLAS_WIN_BYTES);
			g_ls.win = win;
		}
		if (csa != g_ls.csa) {
			sceGuClutMode(GU_PSM_5551, 0, 0x0f, csa);
			g_ls.csa = csa;
		}
	}
}

// Two vertices for one tile (GU_SPRITES)
static inline void layer_vtx(struct vtx *v, const ge_tile *t)
{
	int j = t->tile & 4095;
	uint16_t u0 = (uint16_t)((j & 63) * 8), v0 = (uint16_t)((j >> 6) * 8);
	uint16_t u1 = u0 + 8, v1 = v0 + 8;
	int h = 8;
	if (t->flags & 1) {
		uint16_t k = u0;
		u0 = u1;
		u1 = k;
	}
	if (t->flags & GE_TILE_STRIP) { // strip: only tile rows [row, row+h) (v reversed if flipped)
		int row = GE_STRIP_ROW(t->flags);
		h = t->h;
		if (t->flags & 2) {
			v1 = (uint16_t)(v0 + 8 - row - h);
			v0 = (uint16_t)(v0 + 8 - row);
		} else {
			v0 = (uint16_t)(v0 + row);
			v1 = (uint16_t)(v0 + h);
		}
	} else if (t->flags & 2) {
		uint16_t k = v0;
		v0 = v1;
		v1 = k;
	}
	v[0] = (struct vtx){ u0, v0, (int16_t)(GAME_X + t->x), (int16_t)(GAME_Y + t->y), 0 };
	v[1] = (struct vtx){ u1, v1, (int16_t)(GAME_X + t->x + 8), (int16_t)(GAME_Y + t->y + h), 0 };
}

static inline void layer_put(const ge_tile *t)
{
	layer_vtx(&g_vpool[g_nvtx], t);
	g_nvtx += 2;
	g_batch_count++;
}

// Runs with few tiles: per (pass, pattern, palette bank) combo, scan the run and emit in screen order
static void draw_run_scan(const ge_tile *tiles, int count, int l2)
{
	uint64_t combos = 0;
	for (int i = 0; i < count; i++) {
		int grp = (tiles[i].flags >> 2) & 3, q = tiles[i].pal >> 5;
		for (int pass = 0; pass < 2; pass++) {
			int p = ge_pat_of[l2][grp][pass];
			if (p >= 0)
				combos |= 1ull << ((pass * GE_PAT_COUNT + p) * 4 + q);
		}
	}
	for (int pass = 0; pass < 2; pass++) {
		uint64_t pass_bits = (combos >> (pass * GE_PAT_COUNT * 4)) & ((1ull << (GE_PAT_COUNT * 4)) - 1);
		if (!pass_bits)
			continue;
		sceGuStencilOp(GU_KEEP, GU_KEEP, pass ? GU_REPLACE : GU_KEEP);
		for (int p = 0; p < GE_PAT_COUNT; p++)
			for (int q = 0; q < 4; q++) {
				if (!((pass_bits >> (p * 4 + q)) & 1))
					continue;
				layer_set_clut(g_pat_clut[p] + q * 512);
				g_batch_first = g_nvtx;
				g_batch_count = 0;
				for (int i = 0; i < count; i++) {
					const ge_tile *t = &tiles[i];
					if (ge_pat_of[l2][(t->flags >> 2) & 3][pass] != p || (t->pal >> 5) != q)
						continue;
					if (g_nvtx + 2 > MAX_VTX || sceGuCheckList() > (int)sizeof(g_list[0]) - 4096) {
						g_overflow = 1;
						break;
					}
					layer_set_tex(t->tile >> 12, t->pal & 31);
					layer_put(t);
				}
				batch_flush();
			}
	}
}

// Runs with many tiles (line-scroll strips): per pass, sort by (pattern, palette bank, texture window, palette) to batch identical state.
// Tiles of one layer never overlap, so order doesn't affect the result. 14-bit key, two 7-bit radix passes
static void draw_run_radix(const ge_tile *tiles, int count, int l2)
{
	int n = count < LAYER_SORT_MAX ? count : LAYER_SORT_MAX;
	if (count > n)
		g_overflow = 1;
	for (int pass = 0; pass < 2; pass++) {
		int m = 0;
		for (int i = 0; i < n; i++) {
			const ge_tile *t = &tiles[i];
			int p = ge_pat_of[l2][(t->flags >> 2) & 3][pass];
			if (p < 0)
				continue;
			uint32_t key = ((uint32_t)(p * 4 + (t->pal >> 5)) << 9) | ((uint32_t)(t->tile >> 12) << 5) | (t->pal & 31);
			g_sort_a[m++] = (key << 16) | (uint32_t)i;
		}
		if (!m)
			continue;
		radix_sort(m);
		sceGuStencilOp(GU_KEEP, GU_KEEP, pass ? GU_REPLACE : GU_KEEP);
		uint32_t cur_key = 0xffffffffu;
		g_batch_first = g_nvtx;
		g_batch_count = 0;
		for (int k = 0; k < m; k++) {
			uint32_t key = g_sort_a[k] >> 16;
			if (key != cur_key) {
				batch_flush();
				if (g_nvtx + 2 * (m - k) > MAX_VTX || sceGuCheckList() > (int)sizeof(g_list[0]) - 4096) {
					g_overflow = 1;
					break;
				}
				layer_set_clut(g_pat_clut[key >> 11] + ((key >> 9) & 3) * 512);
				layer_set_tex((key >> 5) & 15, key & 31);
				cur_key = key;
			}
			layer_put(&tiles[g_sort_a[k] & 0xffff]);
		}
		batch_flush();
	}
}

// Same order without sorting: per pass, count per key, split vertex slots in key order, then read tiles in order and write in place.
// Radix sort is stable, so order within a key is tile order — GE commands are identical to draw_run_radix. Removes re-reading tiles in
// sorted (random) order and two sort passes (line-scroll layers have thousands of tiles, PERF PP19). With many key kinds, falls back to radix sort
#define BUCKET_MAX 256
static uint8_t g_key_slot[1 << 14];        // key -> slot number + 1 (only keys seen this pass, reset to 0 after)
static uint16_t g_slot_key[BUCKET_MAX];
static uint32_t g_slot_n[BUCKET_MAX], g_slot_pos[BUCKET_MAX];
static uint8_t g_slot_order[BUCKET_MAX];
static void draw_run_sorted(const ge_tile *tiles, int count, int l2)
{
	int n = count < LAYER_SORT_MAX ? count : LAYER_SORT_MAX;
	const int ovf0 = g_overflow;
	if (count > n)
		g_overflow = 1;
	for (int pass = 0; pass < 2; pass++) {
		int ns = 0, m = 0, many = 0;
		for (int i = 0; i < n; i++) {
			const ge_tile *t = &tiles[i];
			int p = ge_pat_of[l2][(t->flags >> 2) & 3][pass];
			if (p < 0)
				continue;
			uint32_t key = ((uint32_t)(p * 4 + (t->pal >> 5)) << 9) | ((uint32_t)(t->tile >> 12) << 5) | (t->pal & 31);
			int sl = g_key_slot[key];
			if (!sl) {
				if (ns == BUCKET_MAX - 1) {
					many = 1;
					break;
				}
				g_slot_key[ns] = (uint16_t)key;
				g_slot_n[ns] = 0;
				g_key_slot[key] = (uint8_t)(++ns);
				sl = ns;
			}
			g_slot_n[sl - 1]++;
			m++;
		}
		if (many) { // (too many key kinds: radix sort the whole run — only if earlier passes emitted nothing yet)
			for (int k = 0; k < ns; k++)
				g_key_slot[g_slot_key[k]] = 0;
			if (pass == 0) {
				g_overflow = ovf0;
				draw_run_radix(tiles, count, l2);
				return;
			}
			g_overflow = 1;
			break;
		}
		if (!m)
			continue;
		// key order (at most a few dozen slots: insertion sort)
		for (int k = 0; k < ns; k++) {
			int j = k;
			while (j > 0 && g_slot_key[g_slot_order[j - 1]] > g_slot_key[k]) {
				g_slot_order[j] = g_slot_order[j - 1];
				j--;
			}
			g_slot_order[j] = (uint8_t)k;
		}
		if (g_nvtx + 2 * m > MAX_VTX) {
			for (int k = 0; k < ns; k++)
				g_key_slot[g_slot_key[k]] = 0;
			g_overflow = 1;
			break;
		}
		const int base = g_nvtx;
		uint32_t pos = (uint32_t)base;
		for (int k = 0; k < ns; k++) {
			const int sl = g_slot_order[k];
			g_slot_pos[sl] = pos;
			pos += 2 * g_slot_n[sl];
		}
		for (int i = 0; i < n; i++) {
			const ge_tile *t = &tiles[i];
			int p = ge_pat_of[l2][(t->flags >> 2) & 3][pass];
			if (p < 0)
				continue;
			uint32_t key = ((uint32_t)(p * 4 + (t->pal >> 5)) << 9) | ((uint32_t)(t->tile >> 12) << 5) | (t->pal & 31);
			const int sl = g_key_slot[key] - 1;
			layer_vtx(&g_vpool[g_slot_pos[sl]], t);
			g_slot_pos[sl] += 2;
		}
		g_nvtx = (int)pos;
		sceGuStencilOp(GU_KEEP, GU_KEEP, pass ? GU_REPLACE : GU_KEEP);
		uint32_t start = (uint32_t)base;
		for (int k = 0; k < ns; k++) {
			const int sl = g_slot_order[k];
			const uint32_t key = g_slot_key[sl];
			g_key_slot[key] = 0;
			if (sceGuCheckList() > (int)sizeof(g_list[0]) - 4096) {
				g_overflow = 1;
				for (int r = k + 1; r < ns; r++)
					g_key_slot[g_slot_key[g_slot_order[r]]] = 0;
				break;
			}
			g_batch_count = 0;
			layer_set_clut(g_pat_clut[key >> 11] + ((key >> 9) & 3) * 512);
			layer_set_tex((key >> 5) & 15, key & 31);
			g_batch_first = (int)start;
			g_batch_count = (int)g_slot_n[sl];
			batch_flush();
			start += 2 * g_slot_n[sl];
		}
		g_batch_first = g_nvtx;
		g_batch_count = 0;
	}
}

// Normal runs with at least this many tiles also use per-key slot assignment (8x8 tiles of one layer never overlap, order doesn't matter).
// Default is strip runs only (PERF PP14: sorting all runs was a loss — radix sort back then)
#ifndef REI_PSP_SORT_MIN
#define REI_PSP_SORT_MIN 0x7fffffff
#endif
static void draw_layers(const ge_frame *fr, uint32_t pal0)
{
	g_ls.win = -1;
	g_ls.csa = -1;
	g_ls.clut = NULL;
	sceGuEnable(GU_ALPHA_TEST);
	sceGuAlphaFunc(GU_GREATER, 0, 0xff);
	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	// The 8888 framebuffer alpha is the stencil. With stencil test off, pixel alpha (0xff) is written as-is and breaks
	// the "high" marks, so keep it on and use KEEP when not changing it
	sceGuEnable(GU_STENCIL_TEST);
	sceGuStencilFunc(GU_ALWAYS, 0x80, 0xff);
	for (int r = 0; r < fr->nruns; r++) {
		const ge_run *run = &fr->runs[r];
		// (this pspsdk's sceGuScissor takes x, y, width, height — passing end coords left right/bottom unclipped and
		// a row of tiles showed outside the game area)
		sceGuScissor(GAME_X, GAME_Y + run->y0, GAME_W, run->y1 - run->y0);
		if (run->fill) {
			draw_fill(run->y0, run->y1, pal0);
			continue;
		}
		if (run->strips || run->count >= REI_PSP_SORT_MIN)
			draw_run_sorted(&fr->tiles[run->first], run->count, run->layer == 2);
		else
			draw_run_scan(&fr->tiles[run->first], run->count, run->layer == 2);
	}
	sceGuDisable(GU_STENCIL_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	SCISSOR_GAME();
}

static void draw_sprites(const void *tex)
{
	sceGuTexMode(GU_PSM_5551, 0, 0, 0);
	sceGuTexImage(0, 512, 256, GAME_W, tex);
	sceGuTexFlush();
	sceGuEnable(GU_STENCIL_TEST);
	sceGuEnable(GU_COLOR_TEST);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
	// The mark stage writes stencil only: unmasked RGB would leave index values as colors on hidden pixels (CLUT stage doesn't cover them)
	sceGuPixelMask(0x00ffffff);
	// Hidden sprites (SPR_HIDDEN) over high (bit7) pixels get bit6. 0x800 = sprite present (B bit4)
	sceGuDisable(GU_ALPHA_TEST);
	sceGuColorFunc(GU_EQUAL, 0x100000 | SPR_HIDDEN_COLOR, 0x100000 | SPR_HIDDEN_COLOR);
	sceGuStencilFunc(GU_EQUAL, 0xc0, 0x80);
	draw_game_rows(g_shid.y0, g_shid.y1);
	// bank number -> stencil (excluding bit6-marked pixels)
	for (int b = 0; b < 8; b++) {
		if (g_sbank[b].y1 <= g_sbank[b].y0)
			continue;
		sceGuColorFunc(GU_EQUAL, 0x100000 | bank_color(b), 0x100000 | BANK_MASK);
		sceGuStencilFunc(GU_EQUAL, b + 1, 0x40);
		draw_game_rows(g_sbank[b].y0, g_sbank[b].y1);
	}
	sceGuPixelMask(0);
	sceGuDisable(GU_COLOR_TEST);
	// color via per-bank CLUT
	sceGuTexMode(GU_PSM_T16, 0, 0, 0);
	sceGuTexImage(0, 512, 256, GAME_W, tex);
	sceGuTexFlush();
	sceGuClutMode(GU_PSM_5551, 0, 0xff, 0);
	sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
	for (int b = 0; b < 8; b++) {
		if (g_sbank[b].y1 <= g_sbank[b].y0)
			continue;
		sceGuClutLoad(256 / 16, g_spr_clut + b * 256);
		sceGuStencilFunc(GU_EQUAL, b + 1, 0xff);
		draw_game_rows(g_sbank[b].y0, g_sbank[b].y1);
	}
	sceGuDisable(GU_STENCIL_TEST);
}

static const ge_frame *g_last_frame;

void video_submit_clut(const uint32_t *pal)
{
	// rebuild only ranges changed from the previous frame (palette mostly unchanged during play)
	static uint16_t last[0x800];
	static int valid;
	int lo = 0, hi = 0x7ff;
	if (valid) {
		while (lo < 0x800 && last[lo] == (uint16_t)pal[lo])
			lo++;
		if (lo == 0x800)
			return;
		while (last[hi] == (uint16_t)pal[hi])
			hi--;
	}
	valid = 1;
	for (int i = lo; i <= hi; i++)
		last[i] = (uint16_t)pal[i];
	for (int p = 0; p < GE_PAT_COUNT; p++) {
		uint16_t bits = ge_pat_bits[p];
		uint16_t *c = g_pat_clut[p];
		for (int i = lo; i <= hi; i++)
			c[i] = (uint16_t)((pal[i] & 0x7fff) | (((bits >> (i & 15)) & 1) << 15));
		sceKernelDcacheWritebackRange(c + lo, (hi - lo + 1) * sizeof(uint16_t));
	}
	for (int i = lo; i <= hi; i++)
		g_spr_clut[i] = (uint16_t)(pal[i] | 0x8000);
	sceKernelDcacheWritebackRange(g_spr_clut + lo, (hi - lo + 1) * sizeof(uint16_t));
}

void video_submit_sprites(const uint16_t *spr)
{
	// also map the core buffer's hidden flag (0x8000) to SPR_HIDDEN (draw_sprites checks color bits)
	uint16_t *dst = (uint16_t *)(0x44000000u + INDEX_OFF);
	for (int i = 0; i < GAME_W * GAME_H; i++)
		dst[i] = (uint16_t)(spr[i] | ((spr[i] >> 3) & SPR_HIDDEN));
}

int video_submit_list(const ge_frame *fr, const rei_spr_tile *st, int nst, const uint8_t *sgfx, const uint32_t *pal, int blank)
{
	g_last_frame = fr;
	g_overflow = fr->overflow;
	const void *tex = (const void *)(0x04000000u + INDEX_OFF);

	// GU_SEND: send the list in one go after building it. GU_DIRECT issues a GE stall-address update syscall per draw call
	// (~200/frame) and the GE reads memory while building. Also set the draw buffer explicitly at start (only GU_DIRECT start adds it)
	unsigned int *list = g_list[g_list_cur];
	sceGuStart(GU_SEND, list);
	g_list_cur ^= 1;
	sceGuDrawBufferList(GU_PSM_8888, (void *)OFF_OFF, BUF_W);
	// pspsdk tracks scissor enable per list type (context), and sceGuScissor emits nothing in a disabled
	// context (init enabled it only for GU_DIRECT). Without enabling, layers bleed into the margins outside the game area
	sceGuEnable(GU_SCISSOR_TEST);
	SCISSOR_GAME();
	g_nvtx = 0;
#ifdef REI_PROFILE
	uint32_t pt0 = sceKernelGetSystemTimeLow();
#endif
	if (st && !blank)
		draw_sprite_target(st, nst, sgfx);
	if (st)
		sprite_ranges(st, nst);
	else
		sprite_ranges_full(); // (path receiving the whole sprite buffer: range unknown)
#ifdef REI_PROFILE
	uint32_t pt1 = sceKernelGetSystemTimeLow();
	g_vprof[0] += pt1 - pt0;
#endif
	// Display off is black. Game screen areas no layer draws are index 0 in the core (palette color 0)
	sceGuClearColor(0xff000000u);
	sceGuClearStencil(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
	if (!blank) {
		// paint color only, leaving stencil (8888 alpha) at 0
		SCISSOR_GAME();
		sceGuEnable(GU_STENCIL_TEST);
		sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
		draw_fill(0, GAME_H, color_8888(pal[0]));
		draw_layers(fr, color_8888(pal[0]));
#ifdef REI_PROFILE
		g_vprof[1] += sceKernelGetSystemTimeLow() - pt1;
#endif
		draw_sprites(tex);
	}
	compose();
	sceGuFinish();
	if (g_nvtx)
		sceKernelDcacheWritebackRange(g_vpool, g_nvtx * sizeof(struct vtx));
#ifndef REI_PSP_NO_GE_TEST // (diag: don't send lists to the GE — measure without GE memory bus use)
	sceGuSendList(GU_TAIL, list, NULL);
#endif
	g_sent = 1;
	return g_overflow;
}

int video_submit_layers(const ge_frame *fr, const uint16_t *spr, const uint32_t *pal, int blank)
{
	video_submit_clut(pal);
	video_submit_sprites(spr);
	return video_submit_list(fr, NULL, 0, NULL, pal, blank);
}

void video_sync(void)
{
	sceGuSync(0, 0);
}

void video_flip(void)
{
	// If the previous flip hasn't reached the screen (second flip in the same vblank), wait for the next vblank — flipping anyway, the previous
	// frame is never shown (objects drawn on alternate frames vanish or look transparent), and the "draw buffer" computed below becomes the
	// visible one, so we drew over the visible screen (black bottom lines, flicker, when catching up after late frames)
	static uint32_t flip_vc;
	static int flipped;
	if (flipped && (uint32_t)sceDisplayGetVcount() == flip_vc)
		sceDisplayWaitVblankStart();
	// vcount comparison alone isn't enough: if a vblank passes after reading the previous flip's vcount but before the flip call, that flip lands a
	// vblank late; if the next flip comes in between, that frame is never shown and the "draw buffer" becomes the visible one (drawn over
	// the visible screen: in-game horizontal lines, obvious in busy scenes). Check the actually visible frame is the one passed and wait if not
	// (may be unknown, e.g. first flip after something else changed the display, so at most 2 times)
	for (int i = 0; i < 2; i++) {
		void *top = NULL;
		int bw, pf;
		if (sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 ||
			((uintptr_t)top & 0x1fffffu) == (uintptr_t)(g_disp_i * FB_BYTES))
			break;
		g_flip_waits++;
		sceDisplayWaitVblankStart();
	}
	flipped = 1;
	// Show the finished buffer at the next vblank, and draw into the remaining one, avoiding the visible one too (on screen until that vblank)
	sceDisplaySetFrameBuf((void *)(0x04000000u + g_draw_i * FB_BYTES), BUF_W, PSP_DISPLAY_PIXEL_FORMAT_565,
		PSP_DISPLAY_SETBUF_NEXTFRAME);
	flip_vc = (uint32_t)sceDisplayGetVcount(); // (read after the flip call: even if a vblank passes in between, the next flip waits)
	const int old = g_disp_i;
	g_disp_i = g_draw_i;
	g_draw_i = FB_COUNT - old - g_disp_i; // (0+1+2 = 3)
	g_draw_fb = (void *)(uintptr_t)(g_draw_i * FB_BYTES);
}

static uint32_t expand(uint32_t c5)
{
	return (c5 << 3) | (c5 >> 2);
}

void video_dump(const char *path, const uint16_t *index, const uint32_t *pal)
{
	static uint32_t img[GAME_W * GAME_H];
	const uint32_t *fb = (const uint32_t *)(0x44000000u + OFF_OFF);
	for (int y = 0; y < GAME_H; y++)
		memcpy(&img[y * GAME_W], &fb[(GAME_Y + y) * BUF_W + GAME_X], GAME_W * 4);
	SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, img, sizeof(img));
	sceIoWrite(fd, index, GAME_W * GAME_H * 2);
	sceIoWrite(fd, pal, 0x801 * 4);
	if (g_last_frame) {
		sceIoWrite(fd, &g_last_frame->nruns, 4);
		sceIoWrite(fd, &g_last_frame->ntiles, 4);
		sceIoWrite(fd, g_last_frame->runs, g_last_frame->nruns * sizeof(ge_run));
		sceIoWrite(fd, g_last_frame->tiles, g_last_frame->ntiles * sizeof(ge_tile));
	}
	// G3 sprite index buffer (5551). Using it straight from VRAM fails, so copy to img
	memcpy(img, (const void *)(0x44000000u + INDEX_OFF), INDEX_BYTES);
	sceIoWrite(fd, img, INDEX_BYTES);
	sceIoClose(fd);
}

int video_verify(const uint16_t *index, const uint32_t *pal, int blank, char *first_diff, int n)
{
	const uint32_t *fb = (const uint32_t *)(0x44000000u + OFF_OFF); // uncached VRAM
	int diffs = 0;
	for (int y = 0; y < GAME_H; y++) {
		for (int x = 0; x < GAME_W; x++) {
			uint32_t want = 0;
			if (!blank) {
				uint32_t c = pal[index[y * GAME_W + x]];
				want = expand(c & 31) | (expand((c >> 5) & 31) << 8) | (expand((c >> 10) & 31) << 16);
			}
			uint32_t got = fb[(GAME_Y + y) * BUF_W + GAME_X + x] & 0x00ffffffu;
			if (got != want) {
				if (diffs == 0 && first_diff) {
					// which color of the same palette the actual color is, neighbor pixel index (nibble order/flip diagnostics)
					unsigned idx = index[y * GAME_W + x], base = idx & ~15u, hit = 0xfff;
					for (unsigned k = 0; k < 16 && !blank; k++) {
						uint32_t c = pal[base + k];
						uint32_t col = expand(c & 31) | (expand((c >> 5) & 31) << 8) | (expand((c >> 10) & 31) << 16);
						if (col == got) {
							hit = base + k;
							break;
						}
					}
					// spr = G3 sprite index buffer value, st = framebuffer stencil (alpha)
					const uint16_t *sidx = (const uint16_t *)(0x44000000u + INDEX_OFF);
					snprintf(first_diff, n, "x%d y%d idx %03x want %06x got %06x (=idx %03x) left %03x right %03x spr %04x st %02x",
						 x, y, idx, (unsigned)want, (unsigned)got, hit,
						 x > 0 ? (unsigned)index[y * GAME_W + x - 1] : 0xfffu,
						 x < GAME_W - 1 ? (unsigned)index[y * GAME_W + x + 1] : 0xfffu, (unsigned)sidx[y * GAME_W + x],
						 (unsigned)(fb[(GAME_Y + y) * BUF_W + GAME_X + x] >> 24));
				}
				diffs++;
			}
		}
	}
	// (old margin check removed: the game now draws to the offscreen and compose paints the margins)
	return diffs;
}
