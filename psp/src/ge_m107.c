// M107 tile layers → GE display list (ge_m107.h). Same rules as FBNeo d_m107.cpp DrawLayers·draw_layer_byline:
//   layer 3 used: L3 group0 (opaque) → L3 group1 (opaque, high) → L2 group0, else L2 group0 (opaque)
//   → L1 group0 → L0 group0 → L2 group1 (high) → L1 group1 (high) → L0 group1 (high)
// Transparent passes skip color 0 and code-0 tiles. Priority of drawn pixels (high = group 1 pass) goes to stencil.
// Tiles within a pass never overlap, so they are sorted by (CLUT bank, texture window, palette) and same-state runs drawn at once.
// On PSP the ME calls this during render: render-side variables here live in .rei_rdr (no cache lines shared with main's variables,
// psp/docs/HANDOFF.md regression rule 7). Settings (g_cfg) are written once by main before render starts; render only reads.
#include "ge_m107.h"

#include <string.h>

#ifdef __psp__
#define GEM_RDR __attribute__((section(".rei_rdr_data")))
#else
#define GEM_RDR
#endif

#define SCR_W 320
#define SCR_H 240
#define VTYPE_2D 0x800102u // GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D

// GE commands (same values pspsdk sceGu emits — the PSP dev build checks them against sceGu output at startup: gem_selftest)
enum {
	GE_VADDR = 0x01, GE_PRIM = 0x04, GE_BASE = 0x10, GE_VTYPE = 0x12, GE_RET = 0x0b,
	GE_TBP0 = 0xa0, GE_TBW0 = 0xa8, GE_CBP = 0xb0, GE_CBW = 0xb1, GE_TSIZE0 = 0xb8, GE_CLOAD = 0xc4, GE_CMODE = 0xc5,
	GE_TFLUSH = 0xcb, GE_SCISSOR1 = 0xd4, GE_SCISSOR2 = 0xd5, GE_STST = 0xdc, GE_SOP = 0xdd,
};

static struct {
	const uint8_t *gfx;
	const uint16_t *rowpens;
	int tile_mask;
	uint32_t atlas_ge, clut_ge[2];
	gem_slot *(*get_slot)(void);
} g_cfg;

#define MAXT 10240 // tiles/strips per pass (one layer, one band), incl. line scroll 240 lines × 41 columns
typedef struct {
	int16_t x, y;
	uint16_t tile;
	uint8_t pal, flags; // flags bit0 flipx, bit1 flipy
	uint8_t h, row;     // lines to draw, first line's row within the tile (before flip)
} gt;
static gt g_t[MAXT] GEM_RDR;
static uint32_t g_ka[MAXT] GEM_RDR, g_kb[MAXT] GEM_RDR;
static struct {
	gem_slot *s;
	int ncmd, nvtx, overflow, ntiles, nt;
	uint32_t cur_clut, cur_tex;
	int cur_csa;
} g_f GEM_RDR;

int gem_init(const rei_m107_tiles *t, uint32_t atlas_ge, const uint32_t clut_ge[2], gem_slot *(*get_slot)(void))
{
	g_cfg.gfx = t->gfx;
	g_cfg.rowpens = t->rowpens;
	g_cfg.tile_mask = t->tile_mask;
	g_cfg.atlas_ge = atlas_ge;
	g_cfg.clut_ge[0] = clut_ge[0];
	g_cfg.clut_ge[1] = clut_ge[1];
	g_cfg.get_slot = get_slot;
	return 0;
}

static inline void cmd(int op, uint32_t arg)
{
	if (g_f.ncmd < g_f.s->cmd_max - 1) // (last word reserved for RET)
		g_f.s->cmd[g_f.ncmd++] = ((uint32_t)op << 24) | (arg & 0xffffff);
	else
		g_f.overflow = 1;
}

// Tiles for one screen line width (41 columns) (FBNeo: sx = 0, 8, .., 320, sxx = (scrollx_1 + sx) & 0x1ff)
static void emit_row(const rei_m107_layer *L, int map_row, int scrollx_1, int y, int h, int row, int group, int opaque)
{
	const uint16_t *r = L->vram + (size_t)(map_row & 63) * 64 * 2;
	const int x0 = -(scrollx_1 & 7), col0 = scrollx_1 >> 3;
	if (g_f.nt + 41 > MAXT) {
		g_f.overflow = 1;
		return;
	}
	gt *o = &g_t[g_f.nt];
	for (int k = 0; k < 41; k++) {
		const int c = (col0 + k) & 63;
		const int attr = r[c * 2 + 1];
		if (((attr >> 9) & 1) != group)
			continue;
		const int code = r[c * 2] | ((attr & 0x1000) << 4);
		const int tile = code & g_cfg.tile_mask;
		if (!opaque && (code == 0 || (g_cfg.rowpens[tile] & 0xff) == 0))
			continue;
		o->x = (int16_t)(8 * k + x0);
		o->y = (int16_t)y;
		o->tile = (uint16_t)tile; // (atlas window is tile >> 12: up to 17 bits in the key below — tile kept in 16 bits, window bits separate)
		o->pal = (uint8_t)(attr & 0x7f);
		o->flags = (uint8_t)(((attr & 0x0400) ? 1 : 0) | ((attr & 0x0800) ? 2 : 0) | ((tile >> 16) << 2));
		o->h = (uint8_t)h;
		o->row = (uint8_t)row;
		o++;
	}
	g_f.nt = (int)(o - g_t);
}

// Collect one group's tiles of one layer (band [start, finish))
static void collect(const rei_m107_seg *s, int layer, int group, int opaque)
{
	const rei_m107_layer *L = &s->l[layer];
	g_f.nt = 0;
	if (!L->enable)
		return;
	const int scrolly = (L->scrolly + 136) & 0x1ff;
	const int scrollx = L->scrollx + 80 + (3 - 2 * layer);
	if (!L->rowscroll) {
		for (int top = s->start - ((scrolly + s->start) & 7); top < s->finish; top += 8)
			emit_row(L, ((scrolly + top) & 0x1ff) >> 3, scrollx, top, 8, 0, group, opaque);
		return;
	}
	// Line scroll: one strip per line. If x scroll matches the line above and it is the next row of the same tile row, extend that strip by one line
	int prev_x = -1, prev_y = -2, prev_first = 0;
	for (int sy = s->start; sy < s->finish; sy++) {
		int scrolly_1 = (scrolly + sy) & 0x1ff;
		if (L->rowscroll & 2)
			scrolly_1 = (scrolly_1 + L->yscroll[sy + 8]) & 0x1ff;
		int scrollx_1 = scrollx;
		if (L->rowscroll & 1)
			scrollx_1 = (scrollx_1 + L->xscroll[((scrolly_1 + 136 + 0xff80) - scrolly) & 0x1ff]) & 0x1ff;
		if (scrollx_1 == prev_x && scrolly_1 == prev_y + 1 && (scrolly_1 & 7) != 0) {
			for (int i = prev_first; i < g_f.nt; i++)
				g_t[i].h++;
		} else {
			prev_first = g_f.nt;
			emit_row(L, scrolly_1 >> 3, scrollx_1, sy, 1, scrolly_1 & 7, group, opaque);
			prev_x = scrollx_1;
		}
		prev_y = scrolly_1;
	}
}

static inline void put_vtx(gem_vtx *v, const gt *t)
{
	const int j = t->tile & 4095;
	uint16_t u0 = (uint16_t)((j & 63) * 8), v0 = (uint16_t)((j >> 6) * 8);
	uint16_t u1 = u0 + 8, v1;
	if (t->flags & 1) {
		const uint16_t k = u0;
		u0 = u1;
		u1 = k;
	}
	const int h = t->h, row = t->row;
	if (t->flags & 2) { // (flip: screen line i is tile row 7 - row - i)
		v1 = (uint16_t)(v0 + 8 - row - h);
		v0 = (uint16_t)(v0 + 8 - row);
	} else {
		v0 = (uint16_t)(v0 + row);
		v1 = (uint16_t)(v0 + h);
	}
	v[0] = (gem_vtx){ u0, v0, t->x, t->y, 0 };
	v[1] = (gem_vtx){ u1, v1, (int16_t)(t->x + 8), (int16_t)(t->y + h), 0 };
}

// Draw collected tiles: stencil value ref (0 low, 0x80 high), color 0 too if opaque
static void emit_pass(int opaque, int ref)
{
	const int n = g_f.nt;
	if (!n)
		return;
	g_f.ntiles += n;
	cmd(GE_STST, 1u | ((uint32_t)ref << 8) | (0xffu << 16)); // GU_ALWAYS
	cmd(GE_SOP, 0u | (0u << 8) | (2u << 16));                 // KEEP, KEEP, REPLACE
	// key = CLUT bank (palette >> 5) | texture window | palette & 31 — 12 bits, two 6-bit radix passes (stable)
	for (int i = 0; i < n; i++) {
		const gt *t = &g_t[i];
		const uint32_t win = (uint32_t)((t->tile >> 12) | ((t->flags >> 2) << 4));
		const uint32_t key = ((uint32_t)(t->pal >> 5) << 10) | (win << 5) | (t->pal & 31);
		g_ka[i] = (key << 16) | (uint32_t)i;
	}
	for (int shift = 16; shift < 28; shift += 6) {
		uint32_t *src = shift == 16 ? g_ka : g_kb, *dst = shift == 16 ? g_kb : g_ka;
		int cnt[64];
		memset(cnt, 0, sizeof(cnt));
		for (int i = 0; i < n; i++)
			cnt[(src[i] >> shift) & 63]++;
		for (int i = 0, sum = 0; i < 64; i++) {
			const int c = cnt[i];
			cnt[i] = sum;
			sum += c;
		}
		for (int i = 0; i < n; i++)
			dst[cnt[(src[i] >> shift) & 63]++] = src[i];
	}
	gem_slot *const s = g_f.s;
	for (int k = 0; k < n;) {
		const uint32_t key = g_ka[k] >> 16;
		int e = k + 1;
		while (e < n && (g_ka[e] >> 16) == key)
			e++;
		const int cnt = e - k;
		if (g_f.nvtx + 2 * cnt > s->vtx_max) {
			g_f.overflow = 1;
			return;
		}
		const uint32_t clut = g_cfg.clut_ge[opaque] + (key >> 10) * 512 * 2;
		if (clut != g_f.cur_clut) {
			cmd(GE_CBP, clut & 0xffffff);
			cmd(GE_CBW, (clut >> 8) & 0xf0000);
			cmd(GE_CLOAD, 32); // 512 entries (block = 16 x 16-bit colors)
			g_f.cur_clut = clut;
		}
		const uint32_t tex = g_cfg.atlas_ge + ((key >> 5) & 31) * GEM_ATLAS_WIN_BYTES;
		if (tex != g_f.cur_tex) {
			cmd(GE_TBP0, tex & 0xffffff);
			cmd(GE_TBW0, ((tex >> 8) & 0x0f0000) | 512);
			cmd(GE_TSIZE0, (9u << 8) | 9u);
			cmd(GE_TFLUSH, 0);
			g_f.cur_tex = tex;
		}
		const int csa = (int)(key & 31);
		if (csa != g_f.cur_csa) {
			cmd(GE_CMODE, 1u | (0u << 2) | (0x0fu << 8) | ((uint32_t)csa << 16)); // GU_PSM_5551, shift 0, mask 0x0f
			g_f.cur_csa = csa;
		}
		gem_vtx *v = s->vtx + g_f.nvtx;
		for (int i = k; i < e; i++, v += 2)
			put_vtx(v, &g_t[g_ka[i] & 0xffff]);
		const uint32_t va = s->vtx_ge + (uint32_t)g_f.nvtx * sizeof(gem_vtx);
		g_f.nvtx += 2 * cnt;
		cmd(GE_BASE, (va >> 8) & 0xf0000);
		cmd(GE_VADDR, va & 0xffffff);
		cmd(GE_PRIM, (6u << 16) | (uint32_t)(2 * cnt)); // GU_SPRITES
		k = e;
	}
}

static void pass(const rei_m107_seg *s, int layer, int group, int opaque, int ref)
{
	collect(s, layer, group, opaque);
	emit_pass(opaque, ref);
}

static void gem_frame_begin(rei_m107_spr *out)
{
	gem_slot *s = g_cfg.get_slot();
	g_f.s = s;
	g_f.ncmd = g_f.nvtx = g_f.overflow = g_f.ntiles = 0;
	g_f.cur_clut = g_f.cur_tex = 0xffffffffu;
	g_f.cur_csa = -1;
	if (s->res->b_dirty) {
		memset(s->spr_b, 0, SCR_W * SCR_H * sizeof(uint16_t));
		s->res->b_dirty = 0;
	}
	out->a = s->spr_a;
	out->b = s->spr_b;
	{ // (lines to clear in A: union of sprite lines of the frame that last wrote this buffer — none for a first-written buffer)
		int y0 = 240, y1 = 0;
		for (int i = 0; i < 8; i++) {
			if (s->res->ay1[i] > s->res->ay0[i]) {
				if (s->res->ay0[i] < y0) y0 = s->res->ay0[i];
				if (s->res->ay1[i] > y1) y1 = s->res->ay1[i];
			}
		}
		out->clr_y0 = (int16_t)y0;
		out->clr_y1 = (int16_t)y1;
	}
	cmd(GE_VTYPE, VTYPE_2D);
}

static void gem_segment(const rei_m107_seg *s)
{
	if (s->finish <= s->start)
		return;
	cmd(GE_SCISSOR1, ((uint32_t)s->start << 10) | 0);
	cmd(GE_SCISSOR2, ((uint32_t)(s->finish - 1) << 10) | (SCR_W - 1));
	if (s->l3_on) {
		if (s->layer_mask & 8) {
			pass(s, 3, 0, 1, 0);
			pass(s, 3, 1, 1, 0x80);
		}
		if (s->layer_mask & 4)
			pass(s, 2, 0, 0, 0);
	} else if (s->layer_mask & 4) {
		pass(s, 2, 0, 1, 0);
	}
	if (s->layer_mask & 2)
		pass(s, 1, 0, 0, 0);
	if (s->layer_mask & 1)
		pass(s, 0, 0, 0, 0);
	if (s->sprite_mask & 4)
		pass(s, 2, 1, 0, 0x80);
	if (s->sprite_mask & 2)
		pass(s, 1, 1, 0, 0x80);
	if (s->sprite_mask & 1)
		pass(s, 0, 1, 0, 0x80);
}

static void gem_frame_end(const rei_m107_spr *spr, int nb, int ab)
{
	gem_slot *s = g_f.s;
	s->cmd[g_f.ncmd++] = (uint32_t)GE_RET << 24;
	gem_result *r = s->res;
	r->ncmd = g_f.ncmd;
	r->nvtx = g_f.nvtx;
	r->overflow = g_f.overflow;
	r->ntiles = g_f.ntiles;
	r->nb = nb;
	r->ab = ab;
	r->b_dirty = nb > 0;
	for (int i = 0; i < 8; i++) {
		r->ay0[i] = spr->ay0[i];
		r->ay1[i] = spr->ay1[i];
	}
	r->hy0 = spr->hy0;
	r->hy1 = spr->hy1;
	r->by0 = spr->by0;
	r->by1 = spr->by1;
	r->bbanks = spr->bbanks;
}

const rei_m107_ge_ops gem_ops = { gem_frame_begin, gem_segment, gem_frame_end };

// (dev check) do what video_gem_sgu_words emits via sceGu, the way this file does it: scissor (0,10)-(319,39), stencil 0x80 replace,
// CLUT 0x08912340 32 blocks, CLUT mode csa 7, texture 0x09876540, 4 vertices at 0x09abcde0
int gem_selftest_words(uint32_t *out)
{
	const uint32_t w[] = {
		((uint32_t)GE_SCISSOR1 << 24) | (10u << 10), ((uint32_t)GE_SCISSOR2 << 24) | (39u << 10) | 319u,
		((uint32_t)GE_STST << 24) | 1u | (0x80u << 8) | (0xffu << 16), ((uint32_t)GE_SOP << 24) | (2u << 16),
		((uint32_t)GE_CBP << 24) | 0x912340u, ((uint32_t)GE_CBW << 24) | 0x080000u, ((uint32_t)GE_CLOAD << 24) | 32u,
		((uint32_t)GE_CMODE << 24) | 1u | (0x0fu << 8) | (7u << 16),
		((uint32_t)GE_TBP0 << 24) | 0x876540u, ((uint32_t)GE_TBW0 << 24) | 0x090000u | 512u, ((uint32_t)GE_TSIZE0 << 24) | 0x909u,
		((uint32_t)GE_TFLUSH << 24), ((uint32_t)GE_VTYPE << 24) | VTYPE_2D, ((uint32_t)GE_BASE << 24) | 0x090000u,
		((uint32_t)GE_VADDR << 24) | 0xabcde0u, ((uint32_t)GE_PRIM << 24) | (6u << 16) | 4u,
	};
	const int n = (int)(sizeof(w) / sizeof(w[0]));
	for (int i = 0; i < n; i++)
		out[i] = w[i];
	return n;
}
