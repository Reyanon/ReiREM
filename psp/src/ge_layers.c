// Tile layers -> GE draw list. Rules follow FBNeo d_m92.cpp draw_layer_byline (pipelined ReiDrawLayerMerged) exactly
// (psp/docs/M92_VIDEO.md G2).
#include "ge_layers.h"

#include <stdlib.h>
#include <string.h>

const uint16_t ge_pat_bits[GE_PAT_COUNT] = { 0xfffe, 0x00fe, 0xff00, 0xffff, 0x00ff, 0x0001 };

// FBNeo transmask[layer][group][forcelayer] is the "skip" color set. forcelayer 1 = low, 0 = high
//   pf0·pf1: {0xffff,0x0001} {0x00ff,0xff01} {0x0001,0xffff}   pf2: {0xffff,0x0000} {0x00ff,0xff00} {0x0001,0xfffe}
const int8_t ge_pat_of[2][3][2] = {
	{ { GE_PAT_1_15, GE_PAT_NONE }, { GE_PAT_1_7, GE_PAT_8_15 }, { GE_PAT_NONE, GE_PAT_1_15 } }, // pf0·pf1
	{ { GE_PAT_0_15, GE_PAT_NONE }, { GE_PAT_0_7, GE_PAT_8_15 }, { GE_PAT_0, GE_PAT_1_15 } },    // pf2
};

#define MAX_TILES 49152
#define MAX_RUNS 4096
#define LAYER_TILES 12288 // per layer of one open group (incl. row scroll 240 lines × 42 cells)

static ge_tile g_tiles[MAX_TILES];
static ge_run g_runs[MAX_RUNS];
static ge_frame g_frame = { g_tiles, 0, g_runs, 0, 0 };

// Per-tile color presence in three classes: bit0 color 0, bit1 colors 1-7, bit2 colors 8-15 (every transparency pattern is a union of
// these, so "any pixel to draw" matches the 16-bit table). 4 bits per tile, two tiles per byte: 32KB table (the 16-bit table was 128KB
// and missed cache on every random lookup)
static uint8_t *g_cls;
static uint8_t g_pat_cls[2][3]; // [is pf2][group] -> classes covered by the drawn patterns
static const uint8_t *g_gfx;
static int g_tile_mask, g_ofs_x, g_ofs_y;

// Open group of segments (not yet moved to the frame list)
struct open_layer {
	ge_tile tiles[LAYER_TILES];
	int ntiles;
	int next_top;                 // no row scroll: screen y of the next tile row to emit
	int last_first, last_count;   // row scroll: strips of the last line (tiles[last_first ..))
	int last_scrollx, last_y;     // that line's x scroll and map y (can the next line merge?)
	int lines;                    // layer without row scroll emitting strips because its scroll changed within the group
};
static struct {
	int open;
	rei_layer_seg seg;           // for register compare (start = group start, finish = current end)
	struct open_layer l[3];
} g_grp;

int ge_layers_init(const rei_tile_info *info)
{
	g_gfx = info->tiles;
	g_tile_mask = info->tile_mask;
	g_ofs_x = info->ofs_x;
	g_ofs_y = info->ofs_y;
	free(g_cls);
	g_cls = (uint8_t *)calloc(((size_t)g_tile_mask + 2) / 2, 1);
	if (!g_cls)
		return -1;
	for (int t = 0; t <= g_tile_mask; t++) {
		const uint8_t *p = g_gfx + (size_t)t * 64;
		unsigned c = 0;
		for (int i = 0; i < 64; i++) {
			int v = p[i] & 15;
			c |= v == 0 ? 1u : v < 8 ? 2u : 4u;
		}
		g_cls[t >> 1] |= (uint8_t)(c << ((t & 1) * 4));
	}
	for (int l2 = 0; l2 < 2; l2++)
		for (int g = 0; g < 3; g++) {
			unsigned m = 0;
			for (int k = 0; k < 2; k++) {
				int pat = ge_pat_of[l2][g][k];
				if (pat < 0)
					continue;
				uint16_t b = ge_pat_bits[pat];
				m |= ((b & 0x0001) ? 1u : 0) | ((b & 0x00fe) ? 2u : 0) | ((b & 0xff00) ? 4u : 0);
			}
			g_pat_cls[l2][g] = (uint8_t)m;
		}
	return 0;
}

void ge_layers_begin(void)
{
	g_frame.ntiles = 0;
	g_frame.nruns = 0;
	g_frame.overflow = 0;
	g_grp.open = 0;
}

static int layer_scrolly(const rei_layer_regs *r)
{
	return (r->scrolly + 136 - g_ofs_y) & 0x1ff;
}

// Same C integer arithmetic as FBNeo (including negative division)
static int layer_scrollx(const rei_layer_regs *r, int layer)
{
	return (((r->rowscroll ? 0 : r->scrollx) - g_ofs_x) - (2 * layer - ((r->wide & 0x80) << 1))) + 80;
}

// strip >= 0: single strip (top = its screen line, strip = line within the tile)
static int add_tile(struct open_layer *ol, int layer, const rei_layer_regs *r, int map_row, int scrollx_1, int sx, int top,
		    int strip)
{
	int wide = r->wide;
	int offs = (map_row * wide) + (((scrollx_1 + sx) / 8) & (wide - 1));
	int attr = r->vram[offs * 2 + 1];
	int code = r->vram[offs * 2 + 0] | ((attr & 0x8000) << 1);
	int group = (attr & 0x0180) ? ((attr & 0x0100) ? 2 : 1) : 0;
	int tile = code & g_tile_mask;
	if (((g_cls[tile >> 1] >> ((tile & 1) * 4)) & g_pat_cls[layer == 2][group]) == 0)
		return 0; // nothing to draw
	if (ol->ntiles >= LAYER_TILES)
		return -1;
	ge_tile *t = &ol->tiles[ol->ntiles++];
	t->x = (int16_t)(sx - (scrollx_1 & 7));
	t->y = (int16_t)top;
	t->tile = (uint16_t)tile;
	t->pal = (uint8_t)(attr & 0x7f);
	t->flags = (uint8_t)(((attr & 0x0200) ? 1 : 0) | ((attr & 0x0400) ? 2 : 0) | (group << 2));
	t->h = 8;
	t->pad = 0;
	if (strip >= 0) {
		t->flags |= (uint8_t)(GE_TILE_STRIP | (strip << 4));
		t->h = 1;
	}
	return 1;
}

// A full screen row (41 cells) of tiles at once: same as add_tile with sx = 0, 8, .., 320. With scrollx_1 >= 0,
// (scrollx_1 + sx) / 8 = scrollx_1 / 8 + k, so no per-cell division/address math (negatives differ in C division: add_tile).
// Returns 0 when space runs short so add_tile handles overflow. Row-scroll layers have thousands of tiles (PERF PP19)
static int emit_tile_row(struct open_layer *ol, int layer, const rei_layer_regs *r, int map_row, int scrollx_1, int top, int strip)
{
	if (scrollx_1 < 0 || ol->ntiles + 41 > LAYER_TILES)
		return 0;
	const int wm = r->wide - 1, x0 = -(scrollx_1 & 7), col0 = scrollx_1 >> 3;
	const uint16_t *row = r->vram + (size_t)map_row * r->wide * 2;
	const uint8_t *pc = g_pat_cls[layer == 2];
	const uint8_t sflag = strip >= 0 ? (uint8_t)(GE_TILE_STRIP | (strip << 4)) : 0;
	const uint8_t h = strip >= 0 ? 1 : 8;
	ge_tile *t = &ol->tiles[ol->ntiles];
	for (int k = 0; k < 41; k++) {
		const int c = (col0 + k) & wm;
		const int attr = row[c * 2 + 1];
		const int code = row[c * 2] | ((attr & 0x8000) << 1);
		const int group = (attr & 0x0180) ? ((attr & 0x0100) ? 2 : 1) : 0;
		const int tile = code & g_tile_mask;
		if (((g_cls[tile >> 1] >> ((tile & 1) * 4)) & pc[group]) == 0)
			continue;
		t->x = (int16_t)(8 * k + x0);
		t->y = (int16_t)top;
		t->tile = (uint16_t)tile;
		t->pal = (uint8_t)(attr & 0x7f);
		t->flags = (uint8_t)(((attr & 0x0200) ? 1 : 0) | ((attr & 0x0400) ? 2 : 0) | (group << 2) | sflag);
		t->h = h;
		t->pad = 0;
		t++;
	}
	ol->ntiles = (int)(t - ol->tiles);
	return 1;
}

// Layer without row scroll: emit tile rows covering screen lines [.., until) starting at next_top
static void emit_rows(struct open_layer *ol, int layer, const rei_layer_regs *r, int until)
{
	int scrolly = layer_scrolly(r);
	int scrollx_1 = layer_scrollx(r, layer);
	while (ol->next_top < until) {
		int top = ol->next_top;
		int map_row = ((scrolly + top) & 0x1ff) >> 3;
		if (!emit_tile_row(ol, layer, r, map_row, scrollx_1, top, -1))
		for (int sx = 0; sx < 320 + 8; sx += 8)
			if (add_tile(ol, layer, r, map_row, scrollx_1, sx, top, -1) < 0) {
				g_frame.overflow = 1;
				return;
			}
		ol->next_top = top + 8;
	}
}

// Row-scroll layer (or one whose scroll changed within the group): screen lines [from, until) as per-line strips. If the line above
// has the same x scroll and is the previous line of the same tile row (same tiles), extend its strip by one line
static void emit_lines(struct open_layer *ol, int layer, const rei_layer_regs *r, int from, int until)
{
	int scrolly = layer_scrolly(r);
	int base = layer_scrollx(r, layer);
	for (int sy = from; sy < until; sy++) {
		int scrollx_1 = base + (r->rowscroll ? r->scroll[(sy + scrolly) & 0x1ff] : 0);
		int y = (scrolly + sy) & 0x1ff;
		// (sy == from continues the last line of the previous segment in the group. A new group has last_y = -1)
		if (!((y & 7) && y == ol->last_y + 1 && scrollx_1 == ol->last_scrollx)) {
			ol->last_first = ol->ntiles;
			int map_row = y >> 3;
			if (!emit_tile_row(ol, layer, r, map_row, scrollx_1, sy, y & 7))
			for (int sx = 0; sx < 320 + 8; sx += 8)
				if (add_tile(ol, layer, r, map_row, scrollx_1, sx, sy, y & 7) < 0) {
					g_frame.overflow = 1;
					return;
				}
			ol->last_count = ol->ntiles - ol->last_first;
			ol->last_scrollx = scrollx_1;
			ol->last_y = y;
			continue;
		}
		for (int i = 0; i < ol->last_count; i++)
			ol->tiles[ol->last_first + i].h++;
		ol->last_y = y;
	}
}

static int layer_on(const rei_layer_seg *s, int layer)
{
	if (!s->l[layer].enable)
		return 0;
	if (layer == 2)
		return !s->pf2_off && (s->layer_mask & 1);
	if (layer == 1)
		return (s->layer_mask & 2) != 0;
	return (s->layer_mask & 0xc) != 0;
}

static void push_run(int y0, int y1, int layer, int fill, int strips, const ge_tile *tiles, int count)
{
	if (g_frame.nruns >= MAX_RUNS || g_frame.ntiles + count > MAX_TILES) {
		g_frame.overflow = 1;
		return;
	}
	ge_run *r = &g_frame.runs[g_frame.nruns++];
	r->y0 = (int16_t)y0;
	r->y1 = (int16_t)y1;
	r->layer = (uint8_t)layer;
	r->fill = (uint8_t)fill;
	r->strips = (uint8_t)strips;
	r->first = g_frame.ntiles;
	r->count = count;
	memcpy(&g_frame.tiles[g_frame.ntiles], tiles, (size_t)count * sizeof(ge_tile));
	g_frame.ntiles += count;
}

static void close_group(void)
{
	if (!g_grp.open)
		return;
	const rei_layer_seg *s = &g_grp.seg;
	if (s->pf2_off)
		push_run(s->start, s->finish, 2, 1, 0, NULL, 0);
	for (int layer = 2; layer >= 0; layer--) {
		if (!layer_on(s, layer))
			continue;
		struct open_layer *ol = &g_grp.l[layer];
		push_run(s->start, s->finish, layer, 0, s->l[layer].rowscroll != 0 || ol->lines, ol->tiles, ol->ntiles);
	}
	g_grp.open = 0;
}

// Can the group continue: differing only in scroll values (scrollx, scrolly) is fine (that layer is emitted as strips)
static int same_regs(const rei_layer_seg *a, const rei_layer_seg *b)
{
	if (a->pf2_off != b->pf2_off || a->layer_mask != b->layer_mask)
		return 0;
	for (int i = 0; i < 3; i++) {
		const rei_layer_regs *x = &a->l[i], *y = &b->l[i];
		if (x->enable != y->enable || x->wide != y->wide || x->rowscroll != y->rowscroll || x->vram != y->vram ||
		    x->scroll != y->scroll)
			return 0;
	}
	return 1;
}

// Switch a layer without row scroll to strips from screen line y: the last emitted tile row extending below y is cut to a [top, y) strip
static void to_lines(struct open_layer *ol, int y)
{
	for (int i = 0; i < ol->ntiles; i++) {
		ge_tile *t = &ol->tiles[i];
		if (t->y + 8 > y) {
			t->flags |= GE_TILE_STRIP; // tile rows 0 .. y - top
			t->h = (uint8_t)(y - t->y);
		}
	}
	ol->lines = 1;
	ol->last_y = -1;
}

void ge_layers_segment(const rei_layer_seg *seg, void *user)
{
	(void)user;
	if (seg->finish <= seg->start)
		return;
	if (g_grp.open && seg->start == g_grp.seg.finish && same_regs(seg, &g_grp.seg)) {
		// Contiguous segment with the same state: extend the group, emit only newly covered parts. Layers whose scroll changed use strips from here.
		// Same for segments where VRAM changed: emitted tiles hold the VRAM of that time, so only cut those extending below this line; from this
		// line on use strips (not joined to the strip above). The guide map writes VRAM every line, opening a new group per line (PERF PP19)
		for (int layer = 2; layer >= 0; layer--) {
			if (!layer_on(seg, layer))
				continue;
			struct open_layer *ol = &g_grp.l[layer];
			const rei_layer_regs *r = &seg->l[layer], *p = &g_grp.seg.l[layer];
			if (!r->rowscroll && !ol->lines && (seg->vram_changed || r->scrollx != p->scrollx || r->scrolly != p->scrolly))
				to_lines(ol, g_grp.seg.finish);
			else if (seg->vram_changed)
				ol->last_y = -1;
			if (r->rowscroll || ol->lines)
				emit_lines(ol, layer, r, g_grp.seg.finish, seg->finish);
			else
				emit_rows(ol, layer, r, seg->finish);
		}
		const int start = g_grp.seg.start;
		g_grp.seg = *seg; // next compare uses the last segment's scroll
		g_grp.seg.start = start;
		return;
	}
	close_group();
	g_grp.open = 1;
	g_grp.seg = *seg;
	for (int layer = 2; layer >= 0; layer--) {
		struct open_layer *ol = &g_grp.l[layer];
		ol->ntiles = 0;
		ol->last_y = -1;
		ol->lines = 0;
		if (!layer_on(seg, layer))
			continue;
		if (seg->l[layer].rowscroll) {
			emit_lines(ol, layer, &seg->l[layer], seg->start, seg->finish);
		} else {
			int y = (layer_scrolly(&seg->l[layer]) + seg->start) & 0x1ff;
			ol->next_top = seg->start - (y & 7);
			emit_rows(ol, layer, &seg->l[layer], seg->finish);
		}
	}
}

const ge_frame *ge_layers_end(void)
{
	close_group();
	return &g_frame;
}
