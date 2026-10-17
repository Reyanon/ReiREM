// --ge-m107: interpret the M107 tile layer GE list (psp/src/ge_m107.c) and compare its screen with the CPU screen (pTransDraw, color index) every
// frame. Only GE commands used by the list are interpreted (scissor, stencil value, CLUT load/mode, texture addr, vertex addr, sprites). CLUT entries hold
// palette indices instead of colors (transparent CLUT: color 0 with alpha 0), so the result is the color index. Sprites overlay A/B buffers per rei_m107ge.h rules.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_m107.h"

extern uint16_t *pTransDraw;

#define W 320
#define H 240
#define AT_BASE 0x08000000u
#define CL_BASE 0x09000000u // [0] transparent +0, [1] opaque +0x10000
#define VX_BASE 0x0a000000u
#define CMD_MAX 65536
#define VTX_MAX (1 << 17)

static uint8_t *g_atlas;
static uint32_t g_atlas_bytes;
static uint16_t g_clut[2][0x8000]; // (0x10000 byte stride)
static uint32_t g_cmd[CMD_MAX];
static gem_vtx g_vtx[VTX_MAX];
static uint16_t g_a[W * H], g_b[W * H];
static gem_result g_res;
static gem_slot g_slot;
static uint16_t g_scr[W * H];
static uint8_t g_st[W * H];
static long g_frames, g_bad_frames, g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf, g_nb_frames, g_nb_px, g_ab_frames;
static int g_frame_no, g_reported;
static int g_sab = -1; // (self-check: with GEM_SABOTAGE, ignore occlusion — verifies the compare catches mismatches)

static gem_slot *get_slot(void) { return &g_slot; }

static void *ge_ptr(uint32_t a)
{
	if (a >= AT_BASE && a < AT_BASE + g_atlas_bytes)
		return g_atlas + (a - AT_BASE);
	if (a >= CL_BASE && a < CL_BASE + 0x20000)
		return (uint8_t *)g_clut[(a - CL_BASE) >> 16] + ((a - CL_BASE) & 0xffff);
	if (a >= VX_BASE && a < VX_BASE + sizeof(g_vtx))
		return (uint8_t *)g_vtx + (a - VX_BASE);
	return NULL;
}

static void interpret(void)
{
	uint32_t base = 0, tbp = 0, tbw_hi = 0, cbp = 0, cbw_hi = 0, vaddr = 0;
	uint16_t clut[512];
	int csa = 0, mask = 0xff, shift = 0, ref = 0, x0 = 0, y0 = 0, x1 = W - 1, y1 = H - 1;
	memset(clut, 0, sizeof(clut));
	for (int i = 0; i < CMD_MAX; i++) {
		const uint32_t c = g_cmd[i], op = c >> 24, arg = c & 0xffffff;
		switch (op) {
		case 0x0b: return; // RET
		case 0x12: break;  // VTYPE (16-bit texture/coords, fixed 2D)
		case 0x10: base = arg; break;
		case 0x01: vaddr = ((base & 0xf0000) << 8) | arg; break;
		case 0xa0: tbp = arg; break;
		case 0xa8: tbw_hi = arg & 0x0f0000; if ((arg & 0xffff) != 512) fprintf(stderr, "gem: tbw %u\n", arg & 0xffff); break;
		case 0xb8: case 0xcb: break;
		case 0xb0: cbp = arg; break;
		case 0xb1: cbw_hi = arg & 0xf0000; break;
		case 0xc4: {
			const uint16_t *p = (const uint16_t *)ge_ptr((cbw_hi << 8) | cbp);
			if (!p || arg * 16 > 512) { fprintf(stderr, "gem: bad clut load\n"); break; }
			memcpy(clut, p, arg * 16 * 2);
			break;
		}
		case 0xc5: shift = (arg >> 2) & 31; mask = (arg >> 8) & 0xff; csa = (arg >> 16) & 31; break;
		case 0xd4: x0 = arg & 0x3ff; y0 = (arg >> 10) & 0x3ff; break;
		case 0xd5: x1 = arg & 0x3ff; y1 = (arg >> 10) & 0x3ff; break;
		case 0xdc: ref = (arg >> 8) & 0xff; if ((arg & 0xff) != 1) fprintf(stderr, "gem: stencil func %u\n", arg & 0xff); break;
		case 0xdd: break;
		case 0x04: {
			const int n = arg & 0xffff;
			const gem_vtx *v = (const gem_vtx *)ge_ptr(vaddr);
			const uint8_t *tex = (const uint8_t *)ge_ptr((tbw_hi << 8) | tbp);
			if (((arg >> 16) & 7) != 6 || !v || !tex) { fprintf(stderr, "gem: bad prim\n"); break; }
			for (int k = 0; k + 1 < n; k += 2) {
				const gem_vtx *a = &v[k], *b = &v[k + 1];
				const int w = b->x - a->x, h = b->y - a->y;
				for (int yy = 0; yy < h; yy++) {
					const int sy = a->y + yy;
					if (sy < y0 || sy > y1) continue;
					const int tv = b->v > a->v ? a->v + yy : a->v - 1 - yy;
					for (int xx = 0; xx < w; xx++) {
						const int sx = a->x + xx;
						if (sx < x0 || sx > x1) continue;
						const int tu = b->u > a->u ? a->u + xx : a->u - 1 - xx;
						const int t = (tex[(tv * 512 + tu) >> 1] >> ((tu & 1) * 4)) & 15;
						const uint16_t e = clut[((t >> shift) & mask) | (csa << 4)];
						if (!(e & 0x8000)) continue;
						g_scr[sy * W + sx] = e & 0x7fff;
						g_st[sy * W + sx] = (uint8_t)ref;
					}
				}
			}
			break;
		}
		default:
			fprintf(stderr, "gem: unknown command %08x\n", c);
		}
	}
}

static void chk_begin(rei_m107_spr *out) { gem_ops.frame_begin(out); }

static long g_range_bad;
static void chk_end(const rei_m107_spr *spr, int nb, int ab)
{
	gem_ops.frame_end(spr, nb, ab);
	for (int i = 0; i < W * H; i++) { // (do the line ranges cover every pixel)
		const int y = i / W, a = g_a[i], b = g_b[i];
		if (a && (y < g_res.ay0[(a >> 8) & 7] || y >= g_res.ay1[(a >> 8) & 7])) g_range_bad++;
		if ((a & 0x1000) && (y < g_res.hy0 || y >= g_res.hy1)) g_range_bad++;
		if (b && (y < g_res.by0 || y >= g_res.by1 || !((g_res.bbanks >> ((b >> 8) & 7)) & 1))) g_range_bad++;
	}
	g_frame_no++;
	g_frames++;
	if (g_res.ntiles > g_max_tiles) g_max_tiles = g_res.ntiles;
	if (g_res.ncmd > g_max_cmd) g_max_cmd = g_res.ncmd;
	if (g_res.nvtx > g_max_vtx) g_max_vtx = g_res.nvtx;
	g_ovf += g_res.overflow != 0;
	if (nb) { g_nb_frames++; g_nb_px += nb; }
	g_ab_frames += ab != 0;
	if (g_sab < 0)
		g_sab = getenv("GEM_SABOTAGE") != NULL;
	memset(g_st, 0, sizeof(g_st));
	interpret();
	long bad = 0;
	int fx = -1, fy = -1;
	for (int i = 0; i < W * H; i++) {
		const uint16_t a = g_a[i], b = g_b[i];
		uint16_t v = g_scr[i];
		if (a && !((a & 0x1000) && g_st[i] == 0x80 && !g_sab))
			v = a & 0x7ff;
		else if (b)
			v = b & 0x7ff;
		g_scr[i] = v;
		if (v != pTransDraw[i]) {
			if (!bad) { fx = i % W; fy = i / W; }
			bad++;
		}
	}
	if (bad) {
		g_bad_frames++;
		g_bad_px += bad;
		if (g_reported < 10) {
			const int i = fy * W + fx;
			fprintf(stderr, "gem: frame %d diff %ld px, first (%d,%d) ge %03x cpu %03x A %04x B %04x st %02x\n", g_frame_no, bad, fx, fy,
				g_scr[i], pTransDraw[i], g_a[i], g_b[i], g_st[i]);
			g_reported++;
		}
		memcpy(g_scr, pTransDraw, sizeof(g_scr)); // (keep a mismatch from spreading to the next frame)
	}
	if (g_frame_no % 600 == 0)
		fprintf(stderr, "gem: %d frames, bad %ld (px %ld), max tiles %ld cmd %ld vtx %ld, overflow %ld, B frames %ld (px %ld), A-hidden frames %ld\n",
			g_frame_no, g_bad_frames, g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf, g_nb_frames, g_nb_px, g_ab_frames);
}

static const rei_m107_ge_ops chk_ops = { chk_begin, NULL, chk_end };
static rei_m107_ge_ops g_ops;

int gem_check_setup(void)
{
	rei_m107_tiles t;
	if (rei_m107_get_tiles(&t) != 0) {
		fprintf(stderr, "--ge-m107: not an M107 game\n");
		return -1;
	}
	const int ntiles = t.tile_mask + 1, wins = (ntiles + 4095) / 4096;
	g_atlas_bytes = (uint32_t)wins * GEM_ATLAS_WIN_BYTES;
	g_atlas = (uint8_t *)calloc(g_atlas_bytes, 1);
	for (int k = 0; k < ntiles; k++) {
		uint8_t *b = g_atlas + (size_t)(k >> 12) * GEM_ATLAS_WIN_BYTES;
		const int j = k & 4095, tx = (j & 63) * 8, ty = (j >> 6) * 8;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) // (T4: low nibble is the left texel — same as video_gem_atlas_init)
				b[((ty + y) * 512 + tx + x) / 2] |= (uint8_t)((t.gfx[(size_t)k * 64 + y * 8 + x] & 15) << ((x & 1) * 4));
	}
	for (int e = 0; e < 2048; e++) {
		g_clut[0][e] = (uint16_t)(((e & 15) ? 0x8000 : 0) | e);
		g_clut[1][e] = (uint16_t)(0x8000 | e);
	}
	g_slot.cmd = g_cmd;
	g_slot.cmd_ge = 0; // (the interpreter reads the command buffer directly)
	g_slot.cmd_max = CMD_MAX;
	g_slot.vtx = g_vtx;
	g_slot.vtx_ge = VX_BASE;
	g_slot.vtx_max = VTX_MAX;
	g_slot.spr_a = g_a;
	g_slot.spr_b = g_b;
	g_slot.res = &g_res;
	const uint32_t cl[2] = { CL_BASE, CL_BASE + 0x10000 };
	gem_init(&t, AT_BASE, cl, get_slot);
	g_ops = gem_ops;
	g_ops.frame_begin = chk_ops.frame_begin;
	g_ops.frame_end = chk_ops.frame_end;
	rei_m107_set_ge(&g_ops, 1);
	return 0;
}

void gem_check_report(void)
{
	fprintf(stderr, "gem: range misses %ld\n", g_range_bad);
	fprintf(stderr, "gem: total %d frames, bad %ld (px %ld), max tiles %ld cmd %ld vtx %ld, overflow %ld, B frames %ld (px %ld), A-hidden frames %ld\n",
		g_frame_no, g_bad_frames, g_bad_px, g_max_tiles, g_max_cmd, g_max_vtx, g_ovf, g_nb_frames, g_nb_px, g_ab_frames);
}
