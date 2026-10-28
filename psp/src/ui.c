// GE drawing for the menus (ui.h). Solid shapes are coloured vertices with texturing off; text is the bundled bitmap font
// "ReiREM UI" (Noto Sans pre-rendered at the exact pixel sizes, psp/src/font_ui.c, SIL OFL 1.1: psp/font/OFL.txt).
// Everything is emitted into the GE list being built by video_present's compose (overlay stage), so it costs nothing while no
// menu, notice or FPS box is on screen.
#include "ui.h"

#include <math.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <string.h>

#include "font_ui.h"

#define SCR_W 480
#define SCR_H 272

// Text: the system PGF font through intraFont was drawn by scaling its glyph texture and stayed soft at every size the owner
// asked for (2026-10-28). The glyphs are now drawn by FreeType at each size the UI uses (psp/tools/make_font.py) and put on the
// screen texel for texel (nearest filter, whole-pixel pen positions): sharp. Three sizes (font_size).
// Small shapes (favourites star, on/off switch) come from the same atlas
static uint32_t __attribute__((aligned(16))) s_font_clut[256]; // white, alpha = the atlas value
static int s_tex;            // texturing is on (text was drawn last)
static int s_cy0, s_cy1 = SCR_H; // rows drawing is limited to

struct cv { // coloured vertex (through mode)
	uint32_t c;
	float x, y, z;
};
#define CV_TYPE (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
struct tcv {
	float u, v;
	uint32_t c;
	float x, y, z;
};
#define TCV_TYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

void ui_init(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	for (int i = 0; i < 256; i++)
		s_font_clut[i] = 0x00FFFFFFu | ((uint32_t)i << 24);
	sceKernelDcacheWritebackRange(s_font_clut, sizeof(s_font_clut));
}

static void solid(void)
{
	sceGuDisable(GU_TEXTURE_2D);
	s_tex = 0;
}

// the font atlas as the texture: T8 with the white alpha ramp, tinted by the vertex colour, texel for texel
static void font_tex(void)
{
	sceGuEnable(GU_TEXTURE_2D);
	s_tex = 1;
	sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
	sceGuClutLoad(256 / 8, s_font_clut);
	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuTexImage(0, REI_FONT_ATLAS_W, REI_FONT_ATLAS_H, REI_FONT_ATLAS_W, rei_font_atlas);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFlush();
}

void ui_begin(void)
{
	static const ScePspIMatrix4 dither = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuShadeModel(GU_SMOOTH);
	sceGuSetDither(&dither); // (on only for gradients: ui_grad)
	sceGuDisable(GU_DITHER);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_TEXTURE_2D);
	s_tex = 0;
	// (scissor state is per sceGu context: in the send list's context it was never enabled, so sceGuScissor emitted nothing)
	sceGuEnable(GU_SCISSOR_TEST);
	ui_clip_y(0, SCR_H);
}

void ui_end(void)
{
	// back to the state the game drawing and compose had after the old text stage (texture on, replace, nearest, blending off)
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_DITHER);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuClutMode(GU_PSM_5551, 0, 0xff, 0);
	sceGuScissor(0, 0, SCR_W, SCR_H);
	s_tex = 1;
}

void ui_clip_y(int y0, int y1)
{
	s_cy0 = y0 < 0 ? 0 : y0;
	s_cy1 = y1 > SCR_H ? SCR_H : y1;
	if (s_cy1 <= s_cy0)
		s_cy1 = s_cy0 + 1;
	sceGuScissor(0, s_cy0, SCR_W, s_cy1 - s_cy0); // (pspgu: x, y, width, height)
}

// a batch of rectangles (sprites: two corner vertices each)
static struct cv *s_bv;
static int s_bn, s_bmax;
static void batch_begin(int max)
{
	s_bv = (struct cv *)sceGuGetMemory((unsigned)max * 2 * sizeof(struct cv));
	s_bn = 0;
	s_bmax = max;
}
static inline void batch_rect(float x, float y, float w, float h, uint32_t col)
{
	if (s_bn >= s_bmax || w <= 0 || h <= 0)
		return;
	struct cv *v = s_bv + s_bn * 2;
	v[0] = (struct cv){ col, x, y, 0 };
	v[1] = (struct cv){ col, x + w, y + h, 0 };
	s_bn++;
}
static void batch_end(void)
{
	solid();
	if (s_bn)
		sceGuDrawArray(GU_SPRITES, CV_TYPE, s_bn * 2, NULL, s_bv);
}

void ui_rect(int x, int y, int w, int h, uint32_t col)
{
	if (w <= 0 || h <= 0)
		return;
	batch_begin(1);
	batch_rect((float)x, (float)y, (float)w, (float)h, col);
	batch_end();
}

void ui_grad(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
	if (w <= 0 || h <= 0)
		return;
	solid();
	struct cv *v = (struct cv *)sceGuGetMemory(4 * sizeof(struct cv));
	v[0] = (struct cv){ top, (float)x, (float)y, 0 };
	v[1] = (struct cv){ top, (float)(x + w), (float)y, 0 };
	v[2] = (struct cv){ bot, (float)x, (float)(y + h), 0 };
	v[3] = (struct cv){ bot, (float)(x + w), (float)(y + h), 0 };
	sceGuEnable(GU_DITHER); // (the screen is 16-bit: a smooth gradient bands without it. Text and edges stay undithered — dots around text)
	sceGuDrawArray(GU_TRIANGLE_STRIP, CV_TYPE, 4, NULL, v);
	sceGuDisable(GU_DITHER);
}

// inset of a rounded rectangle's row yy (0..h-1) for corner radius r
static int rr_inset(int yy, int h, int r)
{
	const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
	if (d <= 0)
		return 0;
	const float dy = (float)d - 0.5f;
	return r - (int)sqrtf((float)(r * r) - dy * dy);
}

void ui_rrect(int x, int y, int w, int h, int r, uint32_t col)
{
	if (w <= 0 || h <= 0)
		return;
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	batch_begin(2 * r + 1);
	for (int yy = 0; yy < r; yy++) { // top corner rows
		const int in = rr_inset(yy, h, r);
		batch_rect((float)(x + in), (float)(y + yy), (float)(w - 2 * in), 1, col);
	}
	batch_rect((float)x, (float)(y + r), (float)w, (float)(h - 2 * r), col);
	for (int yy = h - r; yy < h; yy++) {
		const int in = rr_inset(yy, h, r);
		batch_rect((float)(x + in), (float)(y + yy), (float)(w - 2 * in), 1, col);
	}
	batch_end();
}

void ui_rrect_outline(int x, int y, int w, int h, int r, int t, uint32_t col)
{
	if (w <= 0 || h <= 0)
		return;
	if (r * 2 > h) r = h / 2;
	const int ri = r - t > 0 ? r - t : 0;
	batch_begin(4 * (r + t) + 8);
	for (int yy = 0; yy < h; yy++) {
		const int in = rr_inset(yy, h, r);
		if (yy < t || yy >= h - t) {
			batch_rect((float)(x + in), (float)(y + yy), (float)(w - 2 * in), 1, col);
			continue;
		}
		const int yi = yy - t, hi = h - 2 * t;
		const int ii = ri > 0 ? rr_inset(yi, hi, ri) : 0;
		const int l0 = x + in, l1 = x + t + ii, r0 = x + w - t - ii, r1 = x + w - in;
		if (in == 0 && ii == 0 && yy > r && yy < h - r - 1) { // straight part: the two sides as one tall run each
			int end = yy;
			while (end + 1 < h - r - 1 && end + 1 < h - t)
				end++;
			batch_rect((float)l0, (float)(y + yy), (float)(l1 - l0), (float)(end - yy + 1), col);
			batch_rect((float)r0, (float)(y + yy), (float)(r1 - r0), (float)(end - yy + 1), col);
			yy = end;
			continue;
		}
		batch_rect((float)l0, (float)(y + yy), (float)(l1 - l0), 1, col);
		batch_rect((float)r0, (float)(y + yy), (float)(r1 - r0), 1, col);
	}
	batch_end();
}

void ui_circle(float cx, float cy, float r, uint32_t col)
{
	if (r <= 0)
		return;
	const int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
	const unsigned a = col >> 24;
	batch_begin((y1 - y0 + 1) * 3);
	for (int py = y0; py < y1; py++) {
		const float dy = (float)py + 0.5f - cy;
		if (dy * dy >= r * r)
			continue;
		const float hw = sqrtf(r * r - dy * dy), xl = cx - hw, xr = cx + hw;
		const int il = (int)ceilf(xl), ir = (int)floorf(xr);
		if (ir > il)
			batch_rect((float)il, (float)py, (float)(ir - il), 1, col);
		// soft edge: the partly covered pixel on each side
		const float fl = (float)il - xl, fr = xr - (float)ir;
		if (fl > 0.05f)
			batch_rect((float)(il - 1), (float)py, 1, 1, (col & 0xFFFFFFu) | ((uint32_t)(a * fl) << 24));
		if (fr > 0.05f)
			batch_rect((float)ir, (float)py, 1, 1, (col & 0xFFFFFFu) | ((uint32_t)(a * fr) << 24));
	}
	batch_end();
}

void ui_line(float x0, float y0, float x1, float y1, float width, uint32_t col)
{
	const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.01f)
		return;
	const float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
	solid();
	struct cv *v = (struct cv *)sceGuGetMemory(4 * sizeof(struct cv));
	v[0] = (struct cv){ col, x0 + nx, y0 + ny, 0 };
	v[1] = (struct cv){ col, x1 + nx, y1 + ny, 0 };
	v[2] = (struct cv){ col, x0 - nx, y0 - ny, 0 };
	v[3] = (struct cv){ col, x1 - nx, y1 - ny, 0 };
	sceGuDrawArray(GU_TRIANGLE_STRIP, CV_TYPE, 4, NULL, v);
}

// Small shapes are anti-aliased bitmaps in the font atlas (make_font.py): GE polygons this small came out ragged (owner
// 2026-10-28). Drawn centred on the nearest pixel to (cx, cy), tinted by col
static void shape(const rei_font_glyph *g, float cx, float cy, uint32_t col)
{
	const float x = floorf(cx + 0.5f) + g->xoff, y = floorf(cy + 0.5f) + g->yoff;
	struct tcv *v = (struct tcv *)sceGuGetMemory(2 * sizeof(struct tcv));
	v[0] = (struct tcv){ (float)g->u, (float)g->v, col, x, y, 0 };
	v[1] = (struct tcv){ (float)(g->u + g->w), (float)(g->v + g->h), col, x + g->w, y + g->h, 0 };
	font_tex();
	sceGuDrawArray(GU_SPRITES, TCV_TYPE, 2, NULL, v);
}

// (r is kept for the caller; the bitmap has one size, 11 px)
void ui_star(float cx, float cy, float r, uint32_t col)
{
	(void)r;
	shape(&rei_font_star, cx, cy, col);
}

int ui_toggle(int x, float cy, int on, uint32_t track, uint32_t knob)
{
	const rei_font_glyph *p = &rei_font_pill, *k = &rei_font_knob;
	const float cx = (float)x + p->w / 2, kd = (float)(p->w - p->h) / 2;
	shape(p, cx, cy, track);
	shape(k, on ? cx + kd : cx - kd, cy, knob);
	return p->w;
}

// soft drop shadow (offset 2, 3; about 5 px wide): stacked translucent rounded rectangles, each a pixel larger — the window
// covers the inside, so outside the alpha falls off towards the edge
void ui_window(int x, int y, int w, int h, uint32_t edge, uint32_t fill)
{
	enum { OX = 2, OY = 3, B = 5 };
	for (int k = B; k >= 1; k--)
		ui_rrect(x + OX - k, y + OY - k, w + 2 * k, h + 2 * k, 3 + k, UI_RGBA(0x04, 0x04, 0x06, 0x16));
	ui_rrect_outline(x - 1, y - 1, w + 2, h + 2, 4, 1, edge);
	ui_rrect(x, y, w, h, 3, fill);
}

void ui_image565(const uint16_t *px, int tw, int th, int stride, int x, int y, int w, int h, uint32_t tint)
{
	if (!px)
		return;
	int pw = 1, ph = 1;
	while (pw < tw) pw <<= 1;
	while (ph < th) ph <<= 1;
	sceGuEnable(GU_TEXTURE_2D);
	s_tex = 1;
	sceGuTexMode(GU_PSM_5650, 0, 0, 0);
	sceGuTexImage(0, pw > 512 ? 512 : pw, ph > 512 ? 512 : ph, stride, px);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexFlush();
	const int strips = (tw + 63) / 64; // (64-texel strips: friendly to the GE texture cache)
	struct tcv *v = (struct tcv *)sceGuGetMemory((unsigned)strips * 2 * sizeof(struct tcv));
	for (int i = 0; i < strips; i++) {
		const int u0 = i * 64, u1 = (i + 1) * 64 < tw ? (i + 1) * 64 : tw;
		v[i * 2 + 0] = (struct tcv){ (float)u0, 0, tint, (float)x + (float)u0 * w / tw, (float)y, 0 };
		v[i * 2 + 1] = (struct tcv){ (float)u1, (float)th, tint, (float)x + (float)u1 * w / tw, (float)(y + h), 0 };
	}
	sceGuDrawArray(GU_SPRITES, TCV_TYPE, strips * 2, NULL, v);
}

// Three sizes (owner 2026-10-28): 13 px for ui scale 1 and above (titles, list and menu rows), 12 px down to 0.85 (tabs, notes,
// About), 11 px below (button help)
static int font_size(float scale)
{
	if (scale > 0.99f)
		return 2;
	if (scale > 0.82f)
		return 1;
	return 0;
}

// next character of a UTF-8 or Latin-1 string (U+0020..U+00FF; others become '?'); 0 at the end
static unsigned next_char(const char **ps)
{
	const unsigned char *p = (const unsigned char *)*ps;
	unsigned c = *p++;
	if (c >= 0xC2 && c <= 0xC3 && (*p & 0xC0) == 0x80) // (UTF-8 for U+0080..U+00FF)
		c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
	else if (c >= 0x80 && c < 0xA0)
		c = '?';
	else if (c >= 0xC4 && (*p & 0xC0) == 0x80) { // (other UTF-8 sequences: skip the continuation bytes)
		while ((*p & 0xC0) == 0x80)
			p++;
		c = '?';
	}
	*ps = (const char *)p;
	return c;
}

static const rei_font_glyph *glyph(int size, unsigned c)
{
	if (c < REI_FONT_FIRST || c >= REI_FONT_FIRST + REI_FONT_COUNT)
		c = '?';
	return &rei_font_glyphs[size][c - REI_FONT_FIRST];
}

static int print_clipped(int x, int y, uint32_t col, float scale, const char *s, int cx0, int cx1)
{
	if (!s || !s[0] || cx1 <= cx0 || s_cy1 <= s_cy0)
		return 0;
	const int size = font_size(scale);
	int n = 0;
	for (const char *p = s; *p;)
		if (next_char(&p) != ' ')
			n++;
	struct tcv *v = (struct tcv *)sceGuGetMemory((unsigned)n * 2 * sizeof(struct tcv));
	int k = 0, pen = x;
	for (const char *p = s; *p;) {
		const rei_font_glyph *g = glyph(size, next_char(&p));
		if (g->w && k < n * 2) {
			const float gx = (float)(pen + g->xoff), gy = (float)(y + g->yoff);
			v[k++] = (struct tcv){ (float)g->u, (float)g->v, col, gx, gy, 0 };
			v[k++] = (struct tcv){ (float)(g->u + g->w), (float)(g->v + g->h), col, gx + g->w, gy + g->h, 0 };
		}
		pen += g->adv;
	}
	if (k) {
		font_tex();
		sceGuScissor(cx0, s_cy0, cx1 - cx0, s_cy1 - s_cy0);
		sceGuDrawArray(GU_SPRITES, TCV_TYPE, k, NULL, v);
		sceGuScissor(0, s_cy0, SCR_W, s_cy1 - s_cy0);
	}
	return pen - x;
}

int ui_text(int x, int y, uint32_t col, float scale, const char *s)
{
	return print_clipped(x, y, col, scale, s, 0, SCR_W);
}

int ui_text_width(float scale, const char *s)
{
	if (!s)
		return 0;
	const int size = font_size(scale);
	int w = 0;
	for (const char *p = s; *p;)
		w += glyph(size, next_char(&p))->adv;
	return w;
}

int ui_text_clip(int x, int y, uint32_t col, float scale, const char *s, int cx0, int cx1)
{
	return print_clipped(x, y, col, scale, s, cx0 < 0 ? 0 : cx0, cx1 > SCR_W ? SCR_W : cx1);
}
