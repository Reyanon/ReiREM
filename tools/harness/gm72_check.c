// --ge-m72: interpret the M72-family tile layer GE list (psp/src/ge_m72.c) and compare its drawn screen with the CPU-drawn one (pTransDraw, color indices) every frame.
// Only GE commands the list uses are interpreted (scissor, CLUT load/mode, texture address/format, vertex address, rectangle). CLUT entries hold
// palette indices instead of colors (masked pens alpha 0), so the result is the color index. Sprite texture is the core's separately drawn sprite buffer.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_m72.h"

extern uint16_t *pTransDraw;

#define MAXW 512
#define MAXH 256
#define AT_BASE 0x08000000u // layer 0; layer 1 is + 0x01000000
#define CL_BASE 0x0a000000u
#define SP_BASE 0x0b000000u
#define VX_BASE 0x0c000000u
#define CMD_MAX 65536
#define VTX_MAX (1 << 17)

static uint8_t *g_atlas[2];
static uint32_t g_atlas_bytes[2];
static uint16_t g_clut[(GM72_MAX_MASKS + 1) * 256];
static uint32_t g_cmd[CMD_MAX];
static gem_vtx g_vtx[VTX_MAX];
static uint16_t g_spr[MAXW * MAXH];
static gem_result g_res;
static gem_slot g_slot;
static uint16_t g_scr[MAXW * MAXH];
static int g_w, g_h;
static long g_frames, g_bad_frames, g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf;
static int g_frame_no, g_reported;

static gem_slot *get_slot(void) { return &g_slot; }
static uint16_t *spr_buf(void) { memset(g_spr, 0, sizeof(g_spr)); return g_spr; }

static void *ge_ptr(uint32_t a)
{
	for (int l = 0; l < 2; l++) {
		const uint32_t b = AT_BASE + (uint32_t)l * 0x01000000u;
		if (a >= b && a < b + g_atlas_bytes[l])
			return g_atlas[l] + (a - b);
	}
	if (a >= CL_BASE && a < CL_BASE + sizeof(g_clut))
		return (uint8_t *)g_clut + (a - CL_BASE);
	if (a >= SP_BASE && a < SP_BASE + sizeof(g_spr))
		return (uint8_t *)g_spr + (a - SP_BASE);
	if (a >= VX_BASE && a < VX_BASE + sizeof(g_vtx))
		return (uint8_t *)g_vtx + (a - VX_BASE);
	return NULL;
}

static void interpret(void)
{
	uint32_t base = 0, tbp = 0, tbw_hi = 0, cbp = 0, cbw_hi = 0, vaddr = 0;
	int tbw = 512, psm = 4;
	uint16_t clut[256];
	int csa = 0, mask = 0xff, shift = 0, x0 = 0, y0 = 0, x1 = g_w - 1, y1 = g_h - 1;
	memset(clut, 0, sizeof(clut));
	for (int i = 0; i < CMD_MAX; i++) {
		const uint32_t c = g_cmd[i], op = c >> 24, arg = c & 0xffffff;
		switch (op) {
		case 0x0b: return; // RET
		case 0x12: break;  // VTYPE
		case 0x10: base = arg; break;
		case 0x01: vaddr = ((base & 0xf0000) << 8) | arg; break;
		case 0xa0: tbp = arg; break;
		case 0xa8: tbw_hi = arg & 0x0f0000; tbw = (int)(arg & 0xffff); break;
		case 0xb8: case 0xcb: break;
		case 0xc3: psm = (int)arg; if (psm != 4 && psm != 6) fprintf(stderr, "gm72: psm %d\n", psm); break;
		case 0xb0: cbp = arg; break;
		case 0xb1: cbw_hi = arg & 0xf0000; break;
		case 0xc4: {
			const uint16_t *p = (const uint16_t *)ge_ptr((cbw_hi << 8) | cbp);
			if (!p || arg * 16 > 256) { fprintf(stderr, "gm72: bad clut load\n"); break; }
			memcpy(clut, p, arg * 16 * 2);
			break;
		}
		case 0xc5: shift = (arg >> 2) & 31; mask = (arg >> 8) & 0xff; csa = (arg >> 16) & 31; break;
		case 0xd4: x0 = arg & 0x3ff; y0 = (arg >> 10) & 0x3ff; break;
		case 0xd5: x1 = arg & 0x3ff; y1 = (arg >> 10) & 0x3ff; break;
		case 0x04: {
			const int n = arg & 0xffff;
			const gem_vtx *v = (const gem_vtx *)ge_ptr(vaddr);
			const uint8_t *tex = (const uint8_t *)ge_ptr((tbw_hi << 8) | tbp);
			if (((arg >> 16) & 7) != 6 || !v || !tex) { fprintf(stderr, "gm72: bad prim\n"); break; }
			for (int k = 0; k + 1 < n; k += 2) {
				const gem_vtx *a = &v[k], *b = &v[k + 1];
				const int w = b->x - a->x, h = b->y - a->y;
				for (int yy = 0; yy < h; yy++) {
					const int sy = a->y + yy;
					if (sy < y0 || sy > y1 || sy >= g_h) continue;
					const int tv = b->v > a->v ? a->v + yy : a->v - 1 - yy;
					for (int xx = 0; xx < w; xx++) {
						const int sx = a->x + xx;
						if (sx < x0 || sx > x1 || sx < 0 || sx >= g_w) continue;
						const int tu = b->u > a->u ? a->u + xx : a->u - 1 - xx;
						int t;
						if (psm == 4)
							t = (tex[(tv * tbw + tu) >> 1] >> ((tu & 1) * 4)) & 15;
						else
							t = ((const uint16_t *)tex)[tv * tbw + tu];
						const uint16_t e = clut[(((t >> shift) & mask) | (csa << 4)) & 255];
						if (!(e & 0x8000)) continue;
						g_scr[sy * g_w + sx] = e & 0x7fff;
					}
				}
			}
			break;
		}
		default:
			fprintf(stderr, "gm72: unknown command %08x\n", c);
		}
	}
}

static uint16_t *chk_begin(void) { return gm72_ops.frame_begin(); }

static void chk_end(void)
{
	gm72_ops.frame_end();
	g_frame_no++;
	g_frames++;
	if (g_res.ntiles > g_max_tiles) g_max_tiles = g_res.ntiles;
	if (g_res.ncmd > g_max_cmd) g_max_cmd = g_res.ncmd;
	if (g_res.nvtx > g_max_vtx) g_max_vtx = g_res.nvtx;
	g_ovf += g_res.overflow != 0;
	memset(g_scr, 0, sizeof(g_scr)); // (cleared screen = color index 0)
	interpret();
	long bad = 0;
	int fx = -1, fy = -1;
	for (int i = 0; i < g_w * g_h; i++) {
		if (g_scr[i] != pTransDraw[i]) {
			if (!bad) { fx = i % g_w; fy = i / g_w; }
			bad++;
		}
	}
	if (bad) {
		g_bad_frames++;
		g_bad_px += bad;
		if (g_reported < 10) {
			const int i = fy * g_w + fx;
			fprintf(stderr, "gm72: frame %d diff %ld px, first (%d,%d) ge %03x cpu %03x spr %03x\n", g_frame_no, bad, fx, fy, g_scr[i],
				pTransDraw[i], g_spr[i]);
			g_reported++;
		}
	}
	if (g_frame_no % 600 == 0)
		fprintf(stderr, "gm72: %d frames, bad %ld (px %ld), max tiles %ld cmd %ld vtx %ld, overflow %ld\n", g_frame_no, g_bad_frames,
			g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf);
}

static rei_m72_ge_ops g_ops;

int gm72_check_setup(void)
{
	rei_m72_tiles t;
	if (rei_m72_get_tiles(&t) != 0) {
		fprintf(stderr, "--ge-m72: not an M72-family game\n");
		return -1;
	}
	g_w = t.width;
	g_h = t.height;
	if (g_w > MAXW || g_h > MAXH) {
		fprintf(stderr, "--ge-m72: screen %dx%d\n", g_w, g_h);
		return -1;
	}
	for (int l = 0; l < 2; l++) {
		const int ntiles = t.ntiles[l], wins = (ntiles + 4095) / 4096;
		g_atlas_bytes[l] = (uint32_t)wins * GEM_ATLAS_WIN_BYTES;
		g_atlas[l] = (uint8_t *)calloc(g_atlas_bytes[l], 1);
		for (int k = 0; k < ntiles; k++) {
			uint8_t *b = g_atlas[l] + (size_t)(k >> 12) * GEM_ATLAS_WIN_BYTES;
			const int j = k & 4095, tx = (j & 63) * 8, ty = (j >> 6) * 8;
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++) // (T4: low nibble is the left texel)
					b[((ty + y) * 512 + tx + x) / 2] |= (uint8_t)((t.gfx[l][(size_t)k * 64 + y * 8 + x] & 15) << ((x & 1) * 4));
		}
	}
	gm72_config c;
	memset(&c, 0, sizeof(c));
	c.atlas_ge[0] = AT_BASE;
	c.atlas_ge[1] = AT_BASE + 0x01000000u;
	c.clut_ge = CL_BASE;
	c.spr_ge = SP_BASE;
	c.spr_tbw = g_w;
	c.spr_buf = spr_buf;
	c.get_slot = get_slot;
	if (gm72_init(&t, &c) != 0) {
		fprintf(stderr, "--ge-m72: too many masks\n");
		return -1;
	}
	uint16_t masks[GM72_MAX_MASKS];
	const int nm = gm72_masks(masks);
	for (int k = 0; k < nm; k++)
		for (int i = 0; i < 256; i++)
			g_clut[k * 256 + i] = (uint16_t)((((masks[k] >> (i & 15)) & 1) ? 0 : 0x8000) | (0x100 + i));
	for (int i = 0; i < 256; i++)
		g_clut[nm * 256 + i] = (uint16_t)(((i & 15) ? 0x8000 : 0) | i);
	if (getenv("GM72_SABOTAGE")) // (self-check: make all sprite CLUTs opaque - verifies the comparison catches mismatches)
		for (int i = 0; i < 256; i++)
			g_clut[nm * 256 + i] |= 0x8000;
	fprintf(stderr, "gm72: %dx%d, tiles %d %d, masks %d\n", g_w, g_h, t.ntiles[0], t.ntiles[1], nm);
	g_slot.cmd = g_cmd;
	g_slot.cmd_max = CMD_MAX;
	g_slot.vtx = g_vtx;
	g_slot.vtx_ge = VX_BASE;
	g_slot.vtx_max = VTX_MAX;
	g_slot.res = &g_res;
	g_ops = gm72_ops;
	g_ops.frame_begin = chk_begin;
	g_ops.frame_end = chk_end;
	rei_m72_set_ge(&g_ops, 1);
	return 0;
}

void gm72_check_report(void)
{
	fprintf(stderr, "gm72: total %d frames, bad %ld (px %ld), max tiles %ld cmd %ld vtx %ld, overflow %ld\n", g_frame_no, g_bad_frames,
		g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf);
}
