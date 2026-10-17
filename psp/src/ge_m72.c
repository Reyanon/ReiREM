// M72-family tile layers -> GE display list (ge_m72.h). Rules follow FBNeo d_m72.cpp dodrawline/draw_layer exactly:
//   back pass: layer 1 (forcelayer 1) -> layer 0 (forcelayer 1) -> sprites -> front pass: layer 1 (0) -> layer 0 (0)
// Each cell picks a pen mask by priority (0xffff = not drawn in that pass); masked pens have alpha 0 in that mask's CLUT.
// Cells within one layer/pass never overlap, so they are grouped by (mask, texture window, palette) and drawn per state in one go.
// On PSP the ME calls this while rendering: render-side variables live in .rei_rdr (regression log rule 7). Config (g_cfg) is written
// once by main before starting the renderer; the renderer only reads it.
#include "ge_m72.h"

#include <string.h>

#ifdef __psp__
#define GM_RDR __attribute__((section(".rei_rdr_data")))
#else
#define GM_RDR
#endif

#define VTYPE_2D 0x800102u // GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D

// GE commands (same values as ge_m107.c + texture formats)
enum {
	GE_VADDR = 0x01, GE_PRIM = 0x04, GE_BASE = 0x10, GE_VTYPE = 0x12, GE_RET = 0x0b,
	GE_TBP0 = 0xa0, GE_TBW0 = 0xa8, GE_CBP = 0xb0, GE_CBW = 0xb1, GE_TSIZE0 = 0xb8, GE_TPSM = 0xc3, GE_CLOAD = 0xc4,
	GE_CMODE = 0xc5, GE_TFLUSH = 0xcb, GE_SCISSOR1 = 0xd4, GE_SCISSOR2 = 0xd5,
};
#define PSM_T4 4
#define PSM_T16 6

static struct {
	rei_m72_tiles t;
	gm72_config c;
	uint16_t masks[GM72_MAX_MASKS];
	int nmasks;
	int w, h, spr_tw, spr_th; // screen and sprite texture size (power-of-2 exponents)
} g_cfg;

#define MAXT 14000 // tiles/strips for one pass (one layer, one section) (incl. line scroll 256 lines x 49 cells)
typedef struct {
	int16_t x, y;
	uint16_t tile;
	uint8_t pal, flags; // flags bit0 flipx, bit1 flipy, bit2+ mask index
	uint8_t h, row;     // lines to draw, position within the tile of the first line (before flip)
} gt;
static gt g_t[MAXT] GM_RDR;
static uint32_t g_ka[MAXT] GM_RDR, g_kb[MAXT] GM_RDR;
static struct {
	gem_slot *s;
	int ncmd, nvtx, overflow, ntiles, nt;
	uint32_t cur_clut, cur_tex;
	int cur_csa, cur_psm;
} g_f GM_RDR;

static int log2_at_least(int v)
{
	int k = 0;
	while ((1 << k) < v)
		k++;
	return k;
}

int gm72_init(const rei_m72_tiles *t, const gm72_config *c)
{
	g_cfg.t = *t;
	g_cfg.c = *c;
	g_cfg.w = t->width;
	g_cfg.h = t->height;
	g_cfg.spr_tw = log2_at_least(c->spr_tbw > t->width ? c->spr_tbw : t->width);
	g_cfg.spr_th = log2_at_least(t->height);
	g_cfg.nmasks = 0;
	for (int l = 0; l < 2; l++)
		for (int p = 0; p < 3; p++)
			for (int f = 0; f < 2; f++) {
				const uint16_t m = t->transmask[l][p][f];
				int k = 0;
				if (m == 0xffff)
					continue;
				while (k < g_cfg.nmasks && g_cfg.masks[k] != m)
					k++;
				if (k == g_cfg.nmasks) {
					if (k == GM72_MAX_MASKS)
						return -1;
					g_cfg.masks[g_cfg.nmasks++] = m;
				}
			}
	return 0;
}

int gm72_masks(uint16_t *out)
{
	for (int i = 0; i < g_cfg.nmasks; i++)
		out[i] = g_cfg.masks[i];
	return g_cfg.nmasks;
}

static inline int mask_index(uint16_t m)
{
	for (int k = 0; k < g_cfg.nmasks; k++)
		if (g_cfg.masks[k] == m)
			return k;
	return 0; // (gm72_init filled everything)
}

static inline void cmd(int op, uint32_t arg)
{
	if (g_f.ncmd < g_f.s->cmd_max - 1) // (last word reserved for RET)
		g_f.s->cmd[g_f.ncmd++] = ((uint32_t)op << 24) | (arg & 0xffffff);
	else
		g_f.overflow = 1;
}

// one cell (same attribute decoding as draw_layer). 0 if not drawn in this pass
static inline int emit_cell(int layer, int force, int type, const uint16_t *vram, int offs, int x, int y, int h, int row)
{
	int code = vram[offs * 2 + 0];
	const int color = vram[offs * 2 + 1];
	int flipy, flipx, prio;
	if (type == 1 || type == 3) {
		flipy = color & 0x0040;
		flipx = color & 0x0020;
		prio = (color & 0x0100) ? 2 : (color & 0x80) ? 1 : 0;
	} else {
		flipy = code & 0x8000;
		flipx = code & 0x4000;
		prio = (color & 0x80) ? 2 : ((color & 0x40) ? 1 : 0);
	}
	const uint16_t mask = g_cfg.t.transmask[layer][prio][force];
	if (mask == 0xffff)
		return 0;
	code &= g_cfg.t.ntiles[layer] - 1;
	const uint16_t *tp = g_cfg.t.tpens[layer];
	if (tp && !(tp[code] & (uint16_t)~mask))
		return 0; // (whole cell transparent in this pass)
	if (g_f.nt >= MAXT) {
		g_f.overflow = 1;
		return 0;
	}
	gt *o = &g_t[g_f.nt++];
	o->x = (int16_t)x;
	o->y = (int16_t)y;
	o->tile = (uint16_t)code;
	o->pal = (uint8_t)(color & 0x0f);
	o->flags = (uint8_t)((flipx ? 1 : 0) | (flipy ? 2 : 0) | (mask_index(mask) << 2));
	o->h = (uint8_t)h;
	o->row = (uint8_t)row;
	return 1;
}

// gather one pass of tiles for one layer (section [start, finish))
static void collect(const rei_m72_seg *s, int layer, int force)
{
	g_f.nt = 0;
	const int type = s->type, W = g_cfg.w;
	const uint16_t *vram = s->vram[layer];
	const int wide = (type == 3 && layer == 1);
	const int scrolly = s->scrolly[layer], scrollx = s->scrollx[layer];
	if (!(s->rowscroll && wide)) {
		for (int sy0 = s->start; sy0 < s->finish;) {
			const int scrolly0 = (scrolly + sy0) & 0x1ff;
			int sy1 = sy0 + 8 - (scrolly0 & 7);
			if (sy1 > s->finish)
				sy1 = s->finish;
			const int rowbase = wide ? ((scrolly0 >> 3) << 8) : ((scrolly0 >> 3) << 6);
			for (int sx = 0; sx < W + 8; sx += 8) {
				int scrollx1 = scrollx + sx;
				if (wide)
					scrollx1 = (scrollx1 + 256) & 0x3ff;
				else
					scrollx1 &= 0x1ff;
				emit_cell(layer, force, type, vram, rowbase | (scrollx1 >> 3), sx - (scrollx1 & 7), sy0, sy1 - sy0, scrolly0 & 7);
			}
			sy0 = sy1;
		}
		return;
	}
	// line scroll (majtitle layer 1): one strip per line. If the scroll matches the line above and it's the next line of the same tile row, extend the strip above
	int prev_x = -1, prev_y = -2, prev_first = 0;
	for (int sy = s->start; sy < s->finish; sy++) {
		const int scrolly1 = (scrolly + sy) & 0x1ff;
		const int xs = s->xscroll[scrolly1];
		if (xs == prev_x && scrolly1 == prev_y + 1 && (scrolly1 & 7) != 0) {
			for (int i = prev_first; i < g_f.nt; i++)
				g_t[i].h++;
		} else {
			prev_first = g_f.nt;
			for (int sx = 0; sx < W + 8; sx += 8) {
				const int scrollx1 = (256 + xs + sx + 64 + s->video_offset1) & 0x3ff;
				emit_cell(layer, force, type, vram, ((scrolly1 >> 3) << 8) | (scrollx1 >> 3), sx - (scrollx1 & 7), sy, 1, scrolly1 & 7);
			}
			prev_x = xs;
		}
		prev_y = scrolly1;
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
	if (t->flags & 2) { // (flipped: screen line i is tile line 7 - row - i)
		v1 = (uint16_t)(v0 + 8 - row - h);
		v0 = (uint16_t)(v0 + 8 - row);
	} else {
		v0 = (uint16_t)(v0 + row);
		v1 = (uint16_t)(v0 + h);
	}
	v[0] = (gem_vtx){ u0, v0, t->x, t->y, 0 };
	v[1] = (gem_vtx){ u1, v1, (int16_t)(t->x + 8), (int16_t)(t->y + h), 0 };
}

static void set_psm(int psm)
{
	if (psm != g_f.cur_psm) {
		cmd(GE_TPSM, (uint32_t)psm);
		g_f.cur_psm = psm;
	}
}

static void load_clut(uint32_t clut)
{
	if (clut != g_f.cur_clut) {
		cmd(GE_CBP, clut & 0xffffff);
		cmd(GE_CBW, (clut >> 8) & 0xf0000);
		cmd(GE_CLOAD, 16); // 256 entries (block = 16 16-bit colors)
		g_f.cur_clut = clut;
	}
}

// draw the gathered tiles
static void emit_pass(int layer)
{
	const int n = g_f.nt;
	if (!n)
		return;
	g_f.ntiles += n;
	set_psm(PSM_T4);
	// key = mask index (3 bits) | texture window (5 bits) | palette (4 bits): 12 bits, two 6-bit radix passes (stable)
	for (int i = 0; i < n; i++) {
		const gt *t = &g_t[i];
		const uint32_t key = ((uint32_t)(t->flags >> 2) << 9) | ((uint32_t)(t->tile >> 12) << 4) | t->pal;
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
		load_clut(g_cfg.c.clut_ge + (key >> 9) * GM72_CLUT_BYTES);
		const uint32_t tex = g_cfg.c.atlas_ge[layer] + ((key >> 4) & 31) * GEM_ATLAS_WIN_BYTES;
		if (tex != g_f.cur_tex) {
			cmd(GE_TBP0, tex & 0xffffff);
			cmd(GE_TBW0, ((tex >> 8) & 0x0f0000) | 512);
			cmd(GE_TSIZE0, (9u << 8) | 9u);
			cmd(GE_TFLUSH, 0);
			g_f.cur_tex = tex;
		}
		const int csa = (int)(key & 15);
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

// draw lines [y0, y1) of the sprite buffer (T16 VRAM copy) (64-texel strips)
static void emit_sprites(int y0, int y1)
{
	gem_slot *const s = g_f.s;
	const int W = g_cfg.w, strips = (W + 63) / 64;
	if (g_f.nvtx + 2 * strips > s->vtx_max) {
		g_f.overflow = 1;
		return;
	}
	set_psm(PSM_T16);
	const uint32_t tex = g_cfg.c.spr_ge;
	if (tex != g_f.cur_tex) {
		cmd(GE_TBP0, tex & 0xffffff);
		cmd(GE_TBW0, ((tex >> 8) & 0x0f0000) | (uint32_t)g_cfg.c.spr_tbw);
		cmd(GE_TSIZE0, ((uint32_t)g_cfg.spr_th << 8) | (uint32_t)g_cfg.spr_tw);
		cmd(GE_TFLUSH, 0);
		g_f.cur_tex = tex;
	}
	load_clut(g_cfg.c.clut_ge + (uint32_t)g_cfg.nmasks * GM72_CLUT_BYTES);
	if (g_f.cur_csa != 0x100) {
		cmd(GE_CMODE, 1u | (0u << 2) | (0xffu << 8)); // GU_PSM_5551, shift 0, mask 0xff, csa 0
		g_f.cur_csa = 0x100;
	}
	gem_vtx *v = s->vtx + g_f.nvtx;
	for (int i = 0; i < strips; i++) {
		const int x0 = i * 64, x1 = (i + 1) * 64 < W ? (i + 1) * 64 : W;
		v[i * 2 + 0] = (gem_vtx){ (uint16_t)x0, (uint16_t)y0, (int16_t)x0, (int16_t)y0, 0 };
		v[i * 2 + 1] = (gem_vtx){ (uint16_t)x1, (uint16_t)y1, (int16_t)x1, (int16_t)y1, 0 };
	}
	const uint32_t va = s->vtx_ge + (uint32_t)g_f.nvtx * sizeof(gem_vtx);
	g_f.nvtx += 2 * strips;
	cmd(GE_BASE, (va >> 8) & 0xf0000);
	cmd(GE_VADDR, va & 0xffffff);
	cmd(GE_PRIM, (6u << 16) | (uint32_t)(2 * strips));
}

static uint16_t *gm72_frame_begin(void)
{
	gem_slot *s = g_cfg.c.get_slot();
	g_f.s = s;
	g_f.ncmd = g_f.nvtx = g_f.overflow = g_f.ntiles = 0;
	g_f.cur_clut = g_f.cur_tex = 0xffffffffu;
	g_f.cur_csa = g_f.cur_psm = -1;
	cmd(GE_VTYPE, VTYPE_2D);
	return g_cfg.c.spr_buf ? g_cfg.c.spr_buf() : NULL;
}

static void gm72_segment(const rei_m72_seg *s)
{
	if (s->finish <= s->start)
		return;
	cmd(GE_SCISSOR1, ((uint32_t)s->start << 10) | 0);
	cmd(GE_SCISSOR2, ((uint32_t)(s->finish - 1) << 10) | (uint32_t)(g_cfg.w - 1));
	if (s->layer_mask & 1) { collect(s, 1, 1); emit_pass(1); }
	if (s->layer_mask & 2) { collect(s, 0, 1); emit_pass(0); }
	if (s->sprite_on)
		emit_sprites(s->start, s->finish);
	if (s->layer_mask & 4) { collect(s, 1, 0); emit_pass(1); }
	if (s->layer_mask & 8) { collect(s, 0, 0); emit_pass(0); }
}

static void gm72_frame_end(void)
{
	gem_slot *s = g_f.s;
	s->cmd[g_f.ncmd++] = (uint32_t)GE_RET << 24;
	gem_result *r = s->res;
	r->ncmd = g_f.ncmd;
	r->nvtx = g_f.nvtx;
	r->overflow = g_f.overflow;
	r->ntiles = g_f.ntiles;
}

const rei_m72_ge_ops gm72_ops = { gm72_frame_begin, gm72_segment, gm72_frame_end };
