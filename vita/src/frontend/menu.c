// In-game menu, game list, loading screen, notices, FPS box, touch buttons and the PS button. Ported from ReSys12
// vita/src/frontend/menu.c (common UI of the Re series of Vita emulators): same layout, controls, animations and
// features (themes, choice windows, toggle switches, smooth scrolling, blinking focus, star favorites, touch layout
// editor, update window), with ReiREM's board tabs, DIP switches, cheats and per-game settings.
// Drawing is in ui.c (system default PGF font drawn to a CPU canvas, shown as vita2d overlay layers, video.h).
#include "frontend/menu.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include <taihen.h>
#include <psp2/display.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/re_pad.h"
#include "frontend/settings.h"
#include "frontend/states.h"
#include "frontend/testpad.h"
#include "frontend/touch.h"
#include "frontend/ui.h"
#include "frontend/update.h"
#include "frontend/video.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "rei_core.h"
#include "rei_games.h"
#include "rei_kctrl.h"
#include "rei_version.h" // generated at build time (tools/git_version.cmake)
// Device choice list: each source with its device ("Controller 1: Switch Pro")
static char s_devtext[RE_PAD_SRC_COUNT][40];

#define MENU_OPEN_COMBO (SCE_CTRL_SELECT | SCE_CTRL_START)

#define SCREEN_W 960
#define SCREEN_H 544
#define PANEL_W  640
#define LINE_H   40 // menu rows (pages taller than the screen scroll)
#define ABOUT_LH 30 // About text lines
#define GL_LINE  44 // game list rows (the list scrolls)
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

// Colour themes (Display > Theme; ReST-V's layout and shapes). Graphite is the ReiREM default, Red ReSys12's, Blue ReST-V's. A theme
// is a gradient (top, bottom), the panel, the accent (selection), dim text, row text and the outline colour; the other
// colours are made from these
typedef struct { uint8_t top[3], bot[3], panel[3], sel[3], dim[3], item[3], edge[3]; } ThemeDef;
static const ThemeDef s_theme_def[REI_THEME_COUNT] = {
	{ { 70, 6, 14 }, { 180, 22, 40 }, { 0x46, 0x0A, 0x12 }, { 0xFF, 0x7C, 0x7C }, { 0xD8, 0xA4, 0xAC }, { 0xFA, 0xDC, 0xE0 }, { 0xE0, 0x4C, 0x5C } },
	{ { 6, 22, 70 }, { 20, 86, 180 }, { 0x0A, 0x1C, 0x46 }, { 0x6C, 0xD0, 0xFF }, { 0x9C, 0xB0, 0xD4 }, { 0xD6, 0xE2, 0xFA }, { 0x4C, 0x8C, 0xE0 } },
	{ { 6, 50, 24 }, { 20, 136, 66 }, { 0x0A, 0x36, 0x1C }, { 0x7C, 0xF0, 0xA0 }, { 0xA0, 0xCC, 0xAC }, { 0xDA, 0xF6, 0xE0 }, { 0x4C, 0xC0, 0x78 } },
	{ { 40, 8, 70 }, { 110, 40, 180 }, { 0x2C, 0x0E, 0x4A }, { 0xD0, 0x9C, 0xFF }, { 0xC0, 0xA8, 0xD8 }, { 0xEC, 0xDE, 0xFA }, { 0xA0, 0x6C, 0xE0 } },
	{ { 70, 30, 4 }, { 186, 88, 20 }, { 0x48, 0x22, 0x08 }, { 0xFF, 0xC4, 0x6C }, { 0xDC, 0xBC, 0x9C }, { 0xFA, 0xEA, 0xD6 }, { 0xE8, 0x96, 0x40 } },
	{ { 24, 24, 28 }, { 70, 72, 82 }, { 0x26, 0x28, 0x2E }, { 0xE0, 0xE6, 0xF0 }, { 0xA8, 0xAC, 0xB4 }, { 0xE2, 0xE4, 0xE8 }, { 0x90, 0x94, 0xA0 } },
};
static const char *const s_theme_name[REI_THEME_COUNT] = { "Red", "Blue", "Green", "Purple", "Orange", "Graphite" };
typedef struct {
	uint32_t bg, panel, item, dim, sel, hilite, edge, thumb, focus_still, focus_bright, focus_glow, shade, band, sep, toggle_on,
		toggle_off, grey, grey_sel, bar_track, bar_top, bar_bot, glow1, glow2, glow3, spinner;
	uint8_t panel_rgb[3];
} Theme;
static Theme s_th;
static int s_theme_cur = -1, s_theme_gen; // theme the colours were made for; counts changes (redraw what was kept)
static uint32_t rgb(const uint8_t *c, unsigned a) { return UI_RGBA(c[0], c[1], c[2], a); }
static uint32_t mix(const uint8_t *a, const uint8_t *b, int t, unsigned al) // a..b, t/256 towards b
{
	return UI_RGBA(a[0] + ((b[0] - a[0]) * t >> 8), a[1] + ((b[1] - a[1]) * t >> 8), a[2] + ((b[2] - a[2]) * t >> 8), al);
}
static void theme_make(int k)
{
	static const uint8_t white[3] = { 0xFF, 0xFF, 0xFF }, black[3] = { 0, 0, 0 };
	const ThemeDef *d = &s_theme_def[k];
	Theme *t = &s_th;
	t->bg = mix(d->top, black, 180, 0xC0);
	t->panel = rgb(d->panel, 0xFF); // opaque: filled without reading what is under it (fast)
	memcpy(t->panel_rgb, d->panel, 3);
	t->item = rgb(d->item, 0xFF);
	t->dim = rgb(d->dim, 0xFF);
	t->sel = rgb(d->sel, 0xFF);
	t->hilite = mix(d->sel, white, 40, 0x30);
	t->edge = rgb(d->edge, 0xFF);
	t->thumb = mix(d->sel, white, 50, 0xB0);
	t->focus_still = rgb(d->sel, 0xB0);
	t->focus_bright = mix(d->sel, white, 190, 0xFF);
	t->focus_glow = rgb(d->sel, 0x50);
	t->shade = mix(d->top, black, 200, 0x30);
	t->band = mix(d->top, black, 170, 0xFF);
	t->sep = mix(d->edge, white, 60, 0xFF);
	t->toggle_on = mix(d->edge, d->sel, 100, 0xFF);
	t->toggle_off = mix(d->dim, d->panel, 128, 0xFF);
	t->grey = mix(d->dim, d->panel, 70, 0xFF);
	t->grey_sel = mix(d->dim, d->item, 128, 0xFF);
	t->bar_track = mix(d->panel, black, 80, 0xE0);
	t->bar_top = mix(d->sel, white, 40, 0xFF);
	t->bar_bot = mix(d->edge, black, 40, 0xFF);
	t->glow1 = rgb(d->sel, 0x28);
	t->glow2 = mix(d->sel, white, 60, 0x60);
	t->glow3 = mix(d->sel, white, 220, 0xFF);
	t->spinner = mix(d->sel, white, 110, 0xFF);
	s_theme_cur = k;
	s_theme_gen++;
}
static const Theme *theme(void)
{
	const int k = g_settings.theme >= 0 && g_settings.theme < REI_THEME_COUNT ? g_settings.theme : REI_THEME_GRAPHITE;
	if (k != s_theme_cur)
		theme_make(k);
	return &s_th;
}
uint32_t rei_menu_accent(void) { return theme()->edge; }
// "iREM": the theme's accent; Graphite (the default, its accent grey) has it yellow
static uint32_t brand_col(void)
{
	const uint32_t a = rei_menu_accent();
	return s_theme_cur == REI_THEME_GRAPHITE ? UI_RGBA(0xFF, 0xCC, 0x33, 0xFF) : a;
}
#define COL_BG    (theme()->bg)
#define COL_PANEL (theme()->panel)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_ITEM  (theme()->item) // rows that are not selected
#define COL_DIM   (theme()->dim)
#define COL_SEL   (theme()->sel)
#define COL_BRAND brand_col()
#define COL_WARN  UI_RGBA(0xFF, 0xC0, 0x40, 0xFF)
#define COL_HILITE (theme()->hilite) // selected row bar
#define COL_EDGE  (theme()->edge)   // panel outline, separators
#define COL_THUMB (theme()->thumb)  // scroll bar

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"


// rounded rectangle drawn row by row (no overlapping parts, so translucent colours stay even)
static void rrect(UiCanvas *c, int x, int y, int w, int h, int r, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
	}
}
// outline of a rounded rectangle, t px thick (rows of the outer shape minus the inner one: no overlap)
static void rrect_outline(UiCanvas *c, int x, int y, int w, int h, int r, int t, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	const int ri = r - t > 0 ? r - t : 0;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		if (yy < t || yy >= h - t) {
			ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
			continue;
		}
		int ii = 0; // inner shape inset on this row (inner rect is t smaller on every side)
		const int yi = yy - t, hi = h - 2 * t, di = yi < ri ? ri - yi : (yi >= hi - ri ? yi - (hi - ri - 1) : 0);
		if (di > 0 && ri > 0) {
			const float dy = (float)di - 0.5f;
			ii = ri - (int)sqrtf((float)(ri * ri) - dy * dy);
		}
		const int l0 = x + in, l1 = x + t + ii, r0 = x + w - t - ii, r1 = x + w - in;
		if (l1 > l0) ui_rect(c, l0, y + yy, l1 - l0, 1, col);
		if (r1 > r0) ui_rect(c, r0, y + yy, r1 - r0, 1, col);
	}
}
// The focus outline blinks. It is a small layer of its own (slot 1, over the menu canvas), drawn once per position; the
// blink is that layer's opacity, set every frame (no CPU drawing). Only the topmost focus blinks; a focus under an open
// window keeps a steady outline
#define COL_FOCUS_STILL (theme()->focus_still)
static UiCanvas *s_focus_cv;
static int s_focus_x = -1, s_focus_y, s_focus_w, s_focus_h, s_focus_on, s_focus_want, s_focus_gen;
static int s_focus_cy0, s_focus_cy1 = SCREEN_H; // rows the focus layer may cover (the list / menu rows area)
static void focus_at(UiCanvas *c, int x, int y, int w, int h)
{
	enum { CW = SCREEN_W, CH = 56 };
	if (y - 2 < s_focus_cy0 || y + h + 2 > s_focus_cy1) {
		// partly past the rows area (while scrolling): a steady outline in the canvas, cut there like the rows
		rrect_outline(c, x, y, w, h, 6, 2, COL_FOCUS_STILL);
		return;
	}
	s_focus_want = 1;
	if (!s_focus_cv)
		s_focus_cv = ui_canvas(1, 0, 0, CW, CH);
	if (!s_focus_cv || w + 4 > CW || h + 4 > CH)
		return;
	if (x == s_focus_x && y == s_focus_y && w == s_focus_w && h == s_focus_h && s_focus_on && s_focus_gen == s_theme_gen)
		return;
	if (w != s_focus_w || h != s_focus_h || s_focus_x < 0 || s_focus_gen != s_theme_gen) {
		ui_clear(s_focus_cv);
		rrect_outline(s_focus_cv, 2, 2, w, h, 6, 2, theme()->focus_bright);
		rrect_outline(s_focus_cv, 0, 0, w + 4, h + 4, 8, 2, theme()->focus_glow); // soft glow outside
		s_focus_gen = s_theme_gen;
	}
	s_focus_x = x;
	s_focus_y = y;
	s_focus_w = w;
	s_focus_h = h;
	ui_canvas_move(s_focus_cv, x - 2, y - 2);
	ui_commit(s_focus_cv);
	s_focus_on = 1;
}
static void focus_off(void)
{
	if (s_focus_on)
		ui_hide(s_focus_cv);
	s_focus_on = 0;
}
static void focus_begin(void)
{
	s_focus_want = 0;
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
}
static void focus_end(void)
{
	if (!s_focus_want)
		focus_off();
}
static void focus_tick(void)
{
	if (!s_focus_on)
		return;
	const float p = 0.5f + 0.5f * sinf((float)(rei_time_us() % 1000000) * 6.2832f / 1e6f);
	rei_video_layer_alpha(1, 0.12f + 0.88f * p);
}
// favourites as a star: five points, edges smoothed (4 x 4 samples a pixel)
static void star(UiCanvas *c, float cx, float cy, float r, uint32_t rgb)
{
	float vx[10], vy[10];
	for (int k = 0; k < 10; k++) {
		const float a = -1.5708f + k * 3.14159f / 5.0f, rr = (k & 1) ? r * 0.42f : r;
		vx[k] = cx + rr * cosf(a);
		vy[k] = cy + rr * sinf(a);
	}
	for (int py = (int)(cy - r) - 1; py <= (int)(cy + r) + 1; py++)
		for (int px = (int)(cx - r) - 1; px <= (int)(cx + r) + 1; px++) {
			int hits = 0;
			for (int s = 0; s < 16; s++) {
				const float sx = px + 0.125f + 0.25f * (s & 3), sy = py + 0.125f + 0.25f * (s >> 2);
				int in = 0;
				for (int a = 0, b = 9; a < 10; b = a++)
					if ((vy[a] > sy) != (vy[b] > sy) && sx < (vx[b] - vx[a]) * (sy - vy[a]) / (vy[b] - vy[a]) + vx[a])
						in = !in;
				hits += in;
			}
			if (hits)
				ui_rect(c, px, py, 1, 1, (rgb & 0x00FFFFFFu) | ((uint32_t)(hits * 255 / 16) << 24));
		}
}
#define COL_STAR UI_RGBA(0xFF, 0xD2, 0x48, 0xFF)
// windows with slightly rounded corners and a soft shadow: drop shadow of a window (offset 4, 6, soft over 9 px), only
// outside it: straight edges as one-colour runs, the corners a pixel at a time
enum { SH_OX = 4, SH_OY = 6, SH_B = 9, SH_A = 0x58 };
static uint32_t sh_col(int dx, int dy)
{
	const int d2 = dx * dx + dy * dy;
	return d2 >= SH_B * SH_B ? 0 : UI_RGBA(0x04, 0x04, 0x06, SH_A * (SH_B * SH_B - d2) / (SH_B * SH_B));
}
static void sh_block(UiCanvas *c, int x0, int y0, int x1, int y1, int sx0, int sy0, int sx1, int sy1)
{
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			const uint32_t col = sh_col(xx < sx0 ? sx0 - xx : (xx >= sx1 ? xx - sx1 + 1 : 0), yy < sy0 ? sy0 - yy : (yy >= sy1 ? yy - sy1 + 1 : 0));
			if (col)
				ui_rect(c, xx, yy, 1, 1, col);
		}
}
static void window_box(UiCanvas *c, int x, int y, int w, int h, uint32_t edge, uint32_t fill)
{
	const int sx0 = x + SH_OX, sy0 = y + SH_OY, sx1 = x + w + SH_OX, sy1 = y + h + SH_OY, B = SH_B;
	const int ex = x + w + 1, ey = y + h + 1; // first column / row past the window and its outline
	for (int xx = ex; xx < sx1 + B; xx++) { // right edge (rows beside the window)
		const uint32_t col = sh_col(xx >= sx1 ? xx - sx1 + 1 : 0, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int xx = sx0 - B; xx < x - 1; xx++) { // left edge
		const uint32_t col = sh_col(sx0 - xx, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int yy = ey; yy < sy1 + B; yy++) { // bottom edge
		const uint32_t col = sh_col(0, yy >= sy1 ? yy - sy1 + 1 : 0);
		if (col)
			ui_rect(c, sx0, yy, ex - sx0, 1, col);
	}
	sh_block(c, ex, sy0 - B, sx1 + B, sy0, sx0, sy0, sx1, sy1); // corners
	sh_block(c, ex, ey, sx1 + B, sy1 + B, sx0, sy0, sx1, sy1);
	sh_block(c, sx0 - B, ey, sx0, sy1 + B, sx0, sy0, sx1, sy1);
	rrect_outline(c, x - 1, y - 1, w + 2, h + 2, 7, 1, edge);
	rrect(c, x, y, w, h, 6, fill);
}
// the menu background: one colour per row (vertical gradient; the list also gets its header and footer bands). Filled
// row by row: copying a whole-screen image took 8 ms on the device
static uint32_t blend_px(uint32_t d, uint32_t c)
{
	const unsigned a = c >> 24;
	uint32_t o = 0xFF000000u;
	for (int k = 0; k < 24; k += 8)
		o |= ((((d >> k) & 0xFF) * (255 - a) + ((c >> k) & 0xFF) * a + 127) / 255) << k;
	return o;
}
#define LIST_FOOT_H 40 // the list's footer band (one line of button help)
static const uint32_t *theme_rows(int bands)
{
	static uint32_t rows[2][SCREEN_H];
	static int made_gen = -1;
	const Theme *th = theme();
	if (made_gen != s_theme_gen) {
		made_gen = s_theme_gen;
		const ThemeDef *d = &s_theme_def[s_theme_cur];
		const uint32_t band = th->band & 0xFFFFFFu, sep = th->sep & 0xFFFFFFu;
		for (int y = 0; y < SCREEN_H; y++) {
			const int t = y * 256 / (SCREEN_H - 1);
			rows[0][y] = rows[1][y] = mix(d->top, d->bot, t, 0xFF); // deep -> bright
			if (y < TITLE_H - 8)
				rows[1][y] = blend_px(rows[1][y], band | 0x50000000u);
			else if (y == TITLE_H - 8 || y == SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], sep | 0x60000000u);
			else if (y > SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], band | 0x60000000u);
		}
	}
	return rows[bands ? 1 : 0];
}

// test (log builds): the file ux0:data/reirem/shot (shotload: the loading screen's) makes the menu / list / loading
// canvas an image (ux0:data/reirem/shot.ppm, the canvas over black) and is removed
static void shot_check_as(UiCanvas *c, const char *trigger)
{
#ifdef REI_ENABLE_LOG
	static unsigned tick;
	SceIoStat st;
	if (++tick % 15 == 0 && sceIoGetstat(trigger, &st) >= 0) {
		sceIoRemove(trigger);
		ui_save_ppm(c, REI_DATA_DIR "shot.tmp", UI_RGBA(0, 0, 0, 0xFF));
		sceIoRemove(REI_DATA_DIR "shot.ppm");
		sceIoRename(REI_DATA_DIR "shot.tmp", REI_DATA_DIR "shot.ppm"); // (complete when it appears)
		REI_LOG("menu: shot.ppm written");
	}
	// shotfb: the screen as shown (the canvas with everything the GPU draws over it, such as the list's row strips)
	if (tick % 15 == 7 && sceIoGetstat(REI_DATA_DIR "shotfb", &st) >= 0) {
		sceIoRemove(REI_DATA_DIR "shotfb");
		SceDisplayFrameBuf dfb;
		memset(&dfb, 0, sizeof(dfb));
		dfb.size = sizeof(dfb);
		const uint32_t *fb = sceDisplayGetFrameBuf(&dfb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 ? (const uint32_t *)dfb.base : NULL;
		const int pitch = fb ? (int)dfb.pitch : SCREEN_W;
		FILE *f = fb ? fopen(REI_DATA_DIR "shot.tmp", "wb") : NULL;
		if (f) {
			static uint8_t row[SCREEN_W * 3];
			fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
			for (int y = 0; y < SCREEN_H; y++) {
				for (int x = 0; x < SCREEN_W; x++) {
					const uint32_t p = fb[y * pitch + x];
					row[x * 3] = (uint8_t)p;
					row[x * 3 + 1] = (uint8_t)(p >> 8);
					row[x * 3 + 2] = (uint8_t)(p >> 16);
				}
				fwrite(row, 1, sizeof(row), f);
			}
			fclose(f);
			sceIoRemove(REI_DATA_DIR "shot.ppm");
			sceIoRename(REI_DATA_DIR "shot.tmp", REI_DATA_DIR "shot.ppm");
			REI_LOG("menu: shot.ppm written (screen)");
		}
	}
#else
	(void)c;
	(void)trigger;
#endif
}
#define shot_check(c) shot_check_as(c, REI_DATA_DIR "shot")

// ================================ menu state ================================
static char s_set[32], s_title[96];
static UiCanvas *s_cv, *s_osd_cv, *s_fps_cv;
static int s_changed; // settings changed since the menu opened (saved on close only then)
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_DIPS, PAGE_CHEATS, PAGE_ABOUT, PAGE_COMBO, PAGE_TURBO,
	PAGE_TOUCH, PAGE_TEDIT, PAGE_PORT };
static int s_page;
// rows in groups (a gap before M_STATES, M_DISPLAY and M_ABOUT): game, states, settings, about / leave.
// M_REFRESH (read the ROM folders again) only in the game list options
enum { M_RESUME, M_RESET, M_STATES, M_DISPLAY, M_CONTROLS, M_DIPS, M_CHEATS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME,
	M_EXIT, M_BACK, M_REFRESH, M_COUNT };
#define GROUP_GAP 10
// game list options (START in the list): no game. Only Display / Controls / Refresh / Back, global settings only
static int s_list_mode;
// main menu rows -> items (Delete only if per-game settings exist, else Save only. Back only in list options)
static int main_list(int *k)
{
	int n = 0;
	if (s_list_mode) {
		k[n++] = M_DISPLAY;
		k[n++] = M_CONTROLS;
		k[n++] = M_REFRESH;
		k[n++] = M_BACK;
		return n;
	}
	for (int m = 0; m < M_BACK; m++) {
		if (m == (rei_settings_has_game() ? M_GAME_SAVE : M_GAME_DEL))
			continue;
		k[n++] = m;
	}
	return n;
}
static int main_rows(void) { int k[M_COUNT]; return main_list(k); }
static int main_kind(int row)
{
	int k[M_COUNT];
	const int n = main_list(k);
	return row >= 0 && row < n ? k[row] : M_RESUME;
}
enum { S_SLOT, S_SAVE, S_LOAD, S_BACK, S_COUNT };
enum { D_FPS, D_ASPECT, D_ROTATE, D_FILTER, D_SCAN, D_SCAN_STR, D_BORDER, D_BORDER_COL, D_THEME, D_BACK, D_COUNT };
// Controls (RetroArch style): Port 1 Controls, Port 2 Controls, Touch controls, menu confirm button, defaults (every
// port), back.
// Port N Controls (PAGE_PORT, s_kp = the port): device, analog stick, Set all controls, the game buttons (as many as the
// game uses, 6 in list options), coin, start, Turbo, Combo keys, reset this port, back. A key row: left / right step
// through the keys, confirm waits for the button to press (key binding).
// Turbo page: turbo key, turbo button, turbo speed, back (the port's). Touch page: on, show, edit, clear, back
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_TURBO, CK_STICK, CK_CONFIRM, CK_COMBO, CK_DEFAULTS, CK_BACK,
	CK_TOUCH, CK_T_ON, CK_T_SHOW, CK_T_EDIT, CK_T_CLEAR, CK_PORT1, CK_PORT2, CK_DEVICE, CK_BIND_ALL, CK_RESET_PORT, CK_ENGINE };
static int s_nbuttons = REI_TURBO_BUTTONS;
static int ctrl_buttons(void) { return s_list_mode ? REI_TURBO_BUTTONS : s_nbuttons; }
// the port whose page is open (0 = Port 1 = 1P, 1 = Port 2 = 2P): its settings
static int s_kp;
// the controls the page shows: the profile of the device the player reads now (they belong to the device)
static rei_port *prt(void) { return &g_settings.port[re_pad_player_profile(s_kp)]; }
// key binding (the rows that assign a key): confirm on the row waits for the button to use. Left / right step through
// the keys as before (no wait)
static int s_bind;          // waiting for a button
static int s_bind_all;      // Set all controls: on to the next key row after each button, until Start
static int s_bind_ready;    // every button was released since the wait began (not the confirm press itself)
static uint64_t s_bind_t;   // when the wait began: 5 s without a button cancels
#define BIND_US 5000000
// game button rows (button 1..6) -> REI_ACT_*
static int button_act(int i) { return i < 2 ? i : REI_ACT_B3 + (i - 2); }
// Controls page rows
static const int s_ctrl_list[] = { CK_PORT1, CK_PORT2, CK_ENGINE, CK_TOUCH, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
#define CTRL_ROWS ((int)(sizeof(s_ctrl_list) / sizeof(s_ctrl_list[0])))
// Controls rows now: Port 2 only while there is a second device to play with (re_pad_devices). k may be NULL
static int ctrl_list(int *k)
{
	int n = 0;
	for (int j = 0; j < (int)(sizeof(s_ctrl_list) / sizeof(s_ctrl_list[0])); j++)
		if (s_ctrl_list[j] != CK_PORT2 || re_pad_player_shown(1)) { // (hidden while 2P's controller is away)
			if (k)
				k[n] = s_ctrl_list[j];
			n++;
		}
	return n;
}
static int ctrl_rows(void) { return ctrl_list(NULL); }
// Port page: rows before the buttons, and after the buttons, coin and start
static const int s_port_head[] = { CK_DEVICE, CK_STICK, CK_BIND_ALL };
static const int s_port_tail[] = { CK_TURBO, CK_COMBO, CK_RESET_PORT, CK_BACK };
#define PORT_HEAD ((int)(sizeof(s_port_head) / sizeof(s_port_head[0])))
#define PORT_TAIL ((int)(sizeof(s_port_tail) / sizeof(s_port_tail[0])))
// the player reads no device (Device None, or its controller away with nothing free): its page has only Device and Back
// (the controls belong to a device: with none there is nothing to set)
static int port_none(void) { return re_pad_port(s_kp) < 0; }
static int port_rows(void) { return port_none() ? 2 : PORT_HEAD + ctrl_buttons() + 2 + PORT_TAIL; }
static int port_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (port_none())
		return i == 0 ? CK_DEVICE : CK_BACK;
	if (i < PORT_HEAD)
		return s_port_head[i];
	i -= PORT_HEAD;
	if (i < b) {
		*act = button_act(i);
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? REI_ACT_COIN : REI_ACT_START;
		return CK_ACT;
	}
	return i - b - 2 < PORT_TAIL ? s_port_tail[i - b - 2] : CK_BACK;
}
// row of a key action on the Port page
static int port_row_of_act(int act)
{
	const int b = ctrl_buttons();
	if (act == REI_ACT_COIN)
		return PORT_HEAD + b;
	if (act == REI_ACT_START)
		return PORT_HEAD + b + 1;
	return PORT_HEAD + (act < REI_ACT_B3 ? act : 2 + act - REI_ACT_B3);
}
// row of a kind on the Controls page (coming back from a sub page) or the Port page
static int ctrl_row_of(int kind)
{
	int k[CTRL_ROWS];
	const int n = ctrl_list(k);
	for (int j = 0; j < n; j++)
		if (k[j] == kind)
			return j;
	return 0;
}
static int port_row_of(int kind)
{
	for (int j = 0; j < PORT_TAIL; j++)
		if (s_port_tail[j] == kind)
			return PORT_HEAD + ctrl_buttons() + 2 + j;
	return 0;
}
// current row's kind: Controls, Port, Turbo or Touch
static int row_kind(int i, int *act)
{
	*act = -1;
	if (s_page == PAGE_TURBO)
		return i < 3 ? CK_TURBO_KEY + i : CK_BACK;
	if (s_page == PAGE_PORT)
		return port_kind(i, act);
	if (s_page == PAGE_TOUCH) {
		static const int k[5] = { CK_T_ON, CK_T_SHOW, CK_T_EDIT, CK_T_CLEAR, CK_BACK };
		return i >= 0 && i < 5 ? k[i] : CK_BACK;
	}
	int k[CTRL_ROWS];
	const int n = ctrl_list(k);
	return i >= 0 && i < n ? k[i] : CK_BACK;
}
#define CTRL_PAGE (s_page == PAGE_CONTROLS || s_page == PAGE_TURBO || s_page == PAGE_TOUCH || s_page == PAGE_PORT)
static const char *act_name(int act)
{
	static const char *const n[REI_ACT_COUNT] = { "Button 1", "Button 2", "Coin", "Start", "Button 3", "Button 4", "Button 5",
		"Button 6" };
	return n[act];
}

// combo keys in screen order (Square, Triangle, Cross, Circle, L, R) and the choices for one key: None, then every pair
static const int s_combo_order[REI_COMBO_SRC] = { REI_KEY_SQUARE, REI_KEY_TRIANGLE, REI_KEY_CROSS, REI_KEY_CIRCLE,
	REI_KEY_L, REI_KEY_R, REI_KEY_L2, REI_KEY_R2, REI_KEY_L3, REI_KEY_R3 };
#define COMBO_CHOICES (1 + REI_COMBO_SRC * (REI_COMBO_SRC - 1) / 2)
#define COMBO_ROWS (REI_COMBO_SRC + 1)
static int combo_choice(int i) // choice index -> key mask
{
	if (i <= 0)
		return 0;
	for (int a = 0, n = 1; a < REI_COMBO_SRC; a++)
		for (int b = a + 1; b < REI_COMBO_SRC; b++, n++)
			if (n == i)
				return (1 << s_combo_order[a]) | (1 << s_combo_order[b]);
	return 0;
}
static int combo_index(int mask) // key mask -> choice index (0 if not a pair)
{
	for (int i = 1; i < COMBO_CHOICES; i++)
		if (combo_choice(i) == mask)
			return i;
	return 0;
}

// ---- duplicate keys: one Vita key that drives two different things in the running game (list options: the 6-button
// layout). func = what the key presses
enum { KF_TURBO = 16, KF_COMBO = 32 };
typedef struct { int key, func; const char *what; } KeyUse;
static int key_uses(KeyUse *u)
{
	int n = 0;
	for (int i = 0; i < ctrl_buttons(); i++) {
		const int a = button_act(i);
		if (prt()->keymap[a] < REI_KEY_COUNT)
			u[n++] = (KeyUse){ prt()->keymap[a], a, act_name(a) };
	}
	u[n++] = (KeyUse){ prt()->keymap[REI_ACT_COIN], REI_ACT_COIN, "Coin" };
	u[n++] = (KeyUse){ prt()->keymap[REI_ACT_START], REI_ACT_START, "Start" };
	if (prt()->turbo_key < REI_KEY_COUNT)
		u[n++] = (KeyUse){ prt()->turbo_key, KF_TURBO, "Turbo key" };
	for (int k = 0; k < REI_KEY_COUNT; k++)
		if (prt()->combo[k])
			u[n++] = (KeyUse){ k, KF_COMBO + k, "a combo key" };
	return n;
}
// another use of key that presses something else, or NULL
static const char *key_clash(int key, int func)
{
	KeyUse u[32];
	const int n = key >= 0 && key < REI_KEY_COUNT ? key_uses(u) : 0;
	for (int j = 0; j < n; j++)
		if (u[j].key == key && u[j].func != func)
			return u[j].what;
	return NULL;
}
// key and func of a Controls / Turbo row that assigns a key (0 = not such a row)
static int row_key(int kind, int act, int *key, int *func)
{
	switch (kind) {
	case CK_ACT: *key = prt()->keymap[act]; *func = act; return 1;
	case CK_TURBO_KEY: *key = prt()->turbo_key; *func = KF_TURBO; return 1;
	default: return 0;
	}
}
// set the key of such a row
static void row_set_key(int kind, int act, int key)
{
	if (kind == CK_ACT)
		prt()->keymap[act] = key;
	else if (kind == CK_TURBO_KEY)
		prt()->turbo_key = key;
}
// name of such a row
static const char *row_key_name(int kind, int act) { return kind == CK_ACT ? act_name(act) : "Turbo key"; }
// notice after a key change that made a duplicate (not while the choice window steps to a value)
static int s_quiet;
static void warn_clash(int key, int func)
{
	(void)key; // (a duplicate is shown on its row, "! duplicate": no notice, notices are for what matters)
	(void)func;
}

// DIP switch page: the groups, Restore defaults, Back. Cheat page: (nbbatman family) two built-in cheats, the user cheat
// file's cheats (cheats/<set>.ini, FBNeo format), Back
static int dip_items(void) { return rei_core_dip_count() + 2; }
enum { HK_STAGE, HK_HEALTH, HK_FILE, HK_BACK };
static int cheat_rows(void) { return (rei_core_has_cheats() ? 2 : 0) + rei_core_cheat_count() + 1; }
static int cheat_kind(int row, int *idx)
{
	const int b = rei_core_has_cheats() ? 2 : 0;
	if (row < b)
		return row == 0 ? HK_STAGE : HK_HEALTH;
	row -= b;
	if (row < rei_core_cheat_count()) {
		*idx = row;
		return HK_FILE;
	}
	return HK_BACK;
}
static int any_cheats(void) { return rei_core_has_cheats() || rei_core_cheat_count() > 0; }
static void save_dips(void)
{
	g_settings.dip_count = rei_core_dip_raw(g_settings.dip, REI_DIP_BYTES_MAX);
}

static int s_sel, s_main_sel, s_reset_armed, s_state_slot, s_about_scroll, s_dirty = 1;
// the footer line on the Controls pages: what the selected row does (NULL: the page's own footer)
static const char *row_help(int kind)
{
	switch (kind) {
	case CK_PORT1: return "1P Controller: its device, buttons, analog stick, turbo and combo keys";
	case CK_PORT2: return "2P Controller: its device, buttons, analog stick, turbo and combo keys";
	case CK_ENGINE: return "Native: the Vita's own controls. RePad: Bluetooth controllers through the RePad plugin that comes "
		"with the app (a controller as 2P). VitaControl: the VitaControl plugin installed on the Vita (1P only)";
	case CK_TOUCH: return "On-screen touch buttons: on or off, shown or not, their layout";
	case CK_CONFIRM: return "The button that confirms in the menus (the other one goes back)";
	case CK_DEFAULTS: return "Every player's buttons, analog stick, turbo and combo keys back to the defaults";
	case CK_DEVICE: return "The device this player reads. Auto: the Vita for 1P, then the controllers in order";
	case CK_STICK: return "The analog stick that works as the d-pad too";
	case CK_BIND_ALL: return "Press each button in turn, from Button 1 to Start";
	case CK_ACT: return "Confirm, then press the button to use. Left / Right: step through the buttons. Square: None (where it can be)";
	case CK_TURBO: return "A key that repeats a game button while it is held";
	case CK_COMBO: return "Keys that press two keys together";
	case CK_RESET_PORT: return "This player's buttons, analog stick, turbo and combo keys back to the defaults";
	case CK_TURBO_KEY: return "The key that repeats the turbo button while held (None: off). Confirm, then press the key. Square: None";
	case CK_TURBO_BTN: return "The game button the turbo key repeats";
	case CK_TURBO_SPEED: return "How fast the turbo button repeats";
	case CK_T_ON: return "The touch buttons press their keys";
	case CK_T_SHOW: return "The touch buttons are drawn over the game";
	case CK_T_EDIT: return "Add, move and resize touch buttons and give them keys";
	case CK_T_CLEAR: return "Remove every touch button (press twice)";
	default: return NULL;
	}
}
// the other pages' rows
static const char *page_row_help(void)
{
	switch (s_page) {
	case PAGE_MAIN:
		switch (main_kind(s_sel)) {
		case M_RESUME: return "Back to the game";
		case M_RESET: return "Start the game again from power-on";
		case M_STATES: return "Save the game at this point or load it again";
		case M_DISPLAY: return "Screen size, rotation, filter, scanlines, border, FPS and menu colors";
		case M_CONTROLS: return "Controllers, input engine, buttons, analog stick, turbo, combo keys and touch buttons";
		case M_DIPS: return "The board's DIP switches: difficulty, lives, coins and the game's other settings";
		case M_CHEATS: return "Start stage, infinite health and the cheats of cheats/<set>.ini";
		case M_GAME_SAVE: return "Keep the display and control settings as they are now for this game only";
		case M_GAME_DEL: return "This game uses the global settings again";
		case M_ABOUT: return "Version, credits and licenses";
		case M_SELECT_GAME: return "Back to the game list";
		case M_EXIT: return "Close the app";
		case M_BACK: return "Close the options";
		case M_REFRESH: return "Read the ROM folders again (ROMs copied while the app runs)";
		}
		return NULL;
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: return "Frames per second in a corner of the game picture";
		case D_ASPECT: return "4:3, the full screen, or the largest whole-number scale";
		case D_ROTATE: return "Auto: vertical games upright. Vertical L / R: hold the Vita turned, the controls turn with it";
		case D_FILTER: return "Smooth or sharp pixels";
		case D_SCAN: return "Scanlines over the game picture, as on an arcade monitor";
		case D_SCAN_STR: return "How dark the scanlines are";
		case D_BORDER: return "Beside the game picture (4:3, Integer): black, the game's artwork, or a color";
		case D_BORDER_COL: return "The border color (Border: Color)";
		case D_THEME: return "The colors of the menus";
		}
		return NULL;
	case PAGE_STATES:
		switch (s_sel) {
		case S_SLOT: return "The slot to save to or load from";
		case S_SAVE: return "Save the game in this slot (in the game: SELECT+L)";
		case S_LOAD: return "Load this slot (in the game: SELECT+R)";
		}
		return NULL;
	case PAGE_COMBO:
		return s_sel < REI_COMBO_SRC ? "This key presses the two keys picked here together (None: it is itself). Square: None" : NULL;
	case PAGE_DIPS:
		if (s_sel < rei_core_dip_count())
			return "A DIP switch of the board, as on the cabinet. Some settings take effect after Reset game";
		return s_sel == dip_items() - 2 ? "Every DIP switch back to the game's defaults" : NULL;
	case PAGE_CHEATS: {
		int ci = 0;
		switch (cheat_kind(s_sel, &ci)) {
		case HK_STAGE: return "The stage a new game starts at (it applies when a new game starts)";
		case HK_HEALTH: return "1P never loses health";
		case HK_FILE: return "A cheat of cheats/<set>.ini (FBNeo format): off, or one of its values";
		default: return NULL;
		}
	}
	default:
		return NULL;
	}
}
static int s_help_scroll;   // the footer help is longer than the line: it scrolls (the menu is drawn every frame)
static int s_help_key = -1; // page and row the help belongs to (scrolling starts over on another row)
static uint64_t s_help_t0;
// VitaControl keeps RePad out: the window that offers to switch it off and restart the Vita
// a restart the chosen input engine needs, asked when Back leaves Controls (0 = none):
// 1 RePad chosen, VitaControl keeps it out: switch VitaControl off and restart
// 2 VitaControl chosen, its plugin list line is commented out: switch it on and restart
// 3 VitaControl chosen and listed, but not running since the last restart: restart
static int s_vc_ask;
static int engine_restart_ask(void)
{
	if (g_settings.input_engine == RE_PAD_ENGINE_REPAD)
		return !re_pad_plugin() && re_pad_vitacontrol_listed() ? 1 : 0;
	if (g_settings.input_engine == RE_PAD_ENGINE_VITACONTROL) {
		const int st = re_pad_vitacontrol_state();
		if (st == RE_PAD_VC_OFF)
			return 2;
		if (st == RE_PAD_VC_ON && !re_pad_vitacontrol_running())
			return 3;
	}
	return 0;
}
static int s_drawn_h; // panel height currently drawn on the menu canvas (0 = other content)
static uint32_t s_prev_buttons;
static uint32_t s_bind_prev; // the bound player's buttons last frame
static void bind_begin(int all)
{
	s_bind = 1;
	s_bind_all = all;
	s_bind_ready = 0;
	s_bind_t = rei_time_us();
	s_bind_prev = ~0u; // (pressed only after a release)
	s_dirty = 1;
}

// About: vertically scrolling text (empty string = blank line, starting with '#' = heading line)
static const char *const s_about[] = {
	"#ReiREM for Vita",
	"Version " REI_VERSION " (" REI_VERSION_DATE ")",
	"Developed by Reyanon",
	"  https://github.com/Reyanon",
	"",
	"Irem arcade board emulator",
	"Games (c) Irem. ROM images are not included.",
	"Not affiliated with or endorsed by Irem.",
	"Not for sale. Non-commercial use only (FB Neo license).",
	"",
	"#Open source used",
	"FinalBurn Neo (FB Neo) - Irem drivers, CPU and sound cores",
	"  https://github.com/finalburnneo/FBNeo",
	"MAME - CPU and sound chip cores (via FB Neo)",
	"  https://github.com/mamedev/mame",
	"VitaSDK - toolchain, newlib, system stubs",
	"  https://github.com/vitasdk",
	"libvita2d - graphics",
	"  https://github.com/xerpi/libvita2d",
	"Mbed TLS - update check (HTTPS)",
	"  https://github.com/Mbed-TLS/mbedtls",
	"libpng (used by libvita2d)",
	"  https://github.com/pnggroup/libpng",
	"libjpeg-turbo (used by libvita2d)",
	"  https://github.com/libjpeg-turbo/libjpeg-turbo",
	"zlib - ROM zip reading",
	"  https://github.com/madler/zlib",
	"newlib - C library",
	"  https://sourceware.org/newlib",
	"",
	"License texts: LICENSE-FBNeo.txt in the source repository.",
};
#define ABOUT_LINES ((int)(sizeof(s_about) / sizeof(s_about[0])))
#define ABOUT_VISIBLE 12

static void ensure_canvases(void)
{
	ui_init();
	if (!s_cv)
		s_cv = ui_canvas(0, 0, 0, SCREEN_W, SCREEN_H);
	if (!s_osd_cv)
		s_osd_cv = ui_canvas(2, 0, 0, SCREEN_W, 40);
	if (!s_fps_cv)
		s_fps_cv = ui_canvas(3, SCREEN_W - 4 - 240, 4, 240, 32); // top right (rei_fps_tick aligns the box right)
}

void rei_menu_init(void)
{
	ensure_canvases();
}

void rei_menu_fini(void)
{
	ui_fini();
}

void rei_menu_set_game(const char *set, const char *title, int buttons)
{
	s_nbuttons = buttons < 2 ? 2 : buttons > REI_TURBO_BUTTONS ? REI_TURBO_BUTTONS : buttons;
	for (int i = 0; i < REI_PROFILES; i++)
		if (g_settings.port[i].turbo_btn > s_nbuttons)
			g_settings.port[i].turbo_btn = 1;
	snprintf(s_set, sizeof(s_set), "%s", set);
	snprintf(s_title, sizeof(s_title), "%s", title ? title : set);
	s_page = PAGE_MAIN;
	s_sel = s_main_sel = 0;
}

// save state slots of the running game: used or not, and when (read when the menu opens and after a save; the
// emulation is stopped then)
static int s_slot_used[REI_STATE_SLOTS];
static SceDateTime s_slot_time[REI_STATE_SLOTS];

void rei_menu_states_changed(void)
{
	for (int k = 0; k < REI_STATE_SLOTS; k++) {
		char path[128];
		SceIoStat st;
		rei_state_path(k, path, sizeof(path));
		s_slot_used[k] = s_set[0] && sceIoGetstat(path, &st) >= 0 && st.st_size > 0;
		if (s_slot_used[k])
			s_slot_time[k] = st.st_mtime;
	}
	s_dirty = 1;
}

static int s_keep_place, s_pop, s_pop_sel; // menu closed with SELECT+START (keep its page); choice window open, its row
// The PS button belongs to the app everywhere (locked for the system from the start). One press opens the menu (in a
// game) or the options (in the game list) and closes them again like SELECT+START; nothing while loading. Only a
// double press goes to the home screen: the app lets go of the lock and the kernel helper presses PS for the system
#define REI_KCTRL_LOADED 0x8002D013u // taiLoadStartKernelModule: already loaded (device)
static int s_kctrl = -1;    // kernel helper usable (module/rei_kctrl.skprx): 1 yes, -1 no
static int s_ps_locked;     // the system's PS button handling locked by the app
static unsigned s_ps_one, s_ps_two; // helper counts already acted on (single, double presses)
static uint64_t s_ps_home_t; // when a double press sent the app home (lock taken again a while later)
static void ps_lock(int on)
{
	if (s_kctrl < 0 || on == s_ps_locked)
		return;
	const int r = on ? sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2) : sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
	REI_LOG("menu: PS button %s (%08x)", on ? "locked" : "unlocked", (unsigned)r);
	(void)r;
	s_ps_locked = on;
	if (on) { // (presses made while unlocked are not acted on)
		const unsigned st = reiKCtrlPsState();
		s_ps_one = st & 0x7FFF;
		s_ps_two = (st >> 15) & 0x7FFF;
	}
}
// before the app exits or relaunches itself: give the PS button back to the system (a relaunch with the shell lock
// still held sometimes left the new process unable to start, error C1-2569-2)
void rei_ps_release(void)
{
	ps_lock(0);
}
// Load the kernel helper at start (module/rei_kctrl.skprx in the VPK). Its syscall is linked into this app only if the module was loaded before the app
// started (a call right after a first load crashed on the device), so a first load since boot restarts the app once
// (argument "kctrl"); the module then stays loaded until the Vita reboots
void rei_kctrl_init(int restarted)
{
	const SceUID m = taiLoadStartKernelModule("ux0:app/M92A00001/module/rei_kctrl.skprx", 0, NULL, 0); // (the kernel cannot open app0:)
	// (the module name has a version, rei_kctrl_v1: a module of an older build may still be loaded until reboot)
	if (m >= 0 && !restarted) {
		static char *const av[] = { "kctrl", NULL };
		sceAppMgrLoadExec("app0:eboot.bin", av, NULL);
	}
	s_kctrl = (unsigned)m == REI_KCTRL_LOADED ? 1 : -1;
	REI_LOG("kctrl: %08x (%s)", (unsigned)m, s_kctrl > 0 ? "ready" : "not available");
	if (s_kctrl > 0) {
		sceShellUtilInitEvents(0);
		ps_lock(1);
	}
}
int rei_kctrl_ready(void) { return s_kctrl > 0; } // (the update installs through it)
// call every frame anywhere: 1 once per single PS press, when PS is let go (PS + START takes a screenshot, the menu must
// not cover it: START while PS is held drops that press); a double press goes home
static int s_ps_pending; // a single press counted, PS still held
static int ps_poll(void)
{
	if (s_kctrl < 0)
		return 0;
	if (!s_ps_locked) {
		if (rei_time_us() - s_ps_home_t > 1500000)
			ps_lock(1); // (back from the home screen)
		return 0;
	}
	const unsigned st = reiKCtrlPsState(), one = st & 0x7FFF, two = (st >> 15) & 0x7FFF;
	if (two != s_ps_two) {
		s_ps_two = two;
		s_ps_one = one;
		s_ps_pending = 0;
		ps_lock(0);
		reiKCtrlGoHome();
		s_ps_home_t = rei_time_us();
		return 0;
	}
	if (one != s_ps_one) {
		s_ps_one = one;
		s_ps_pending = 1;
	}
	if (!s_ps_pending)
		return 0;
	if (st & (1u << 30)) { // still held: START with it is the system's screenshot
		SceCtrlData pad;
		if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & SCE_CTRL_START))
			s_ps_pending = 0;
		return 0;
	}
	s_ps_pending = 0;
	return 1;
}

int rei_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int ps = ps_poll();
	const int open = (combo && !prev_combo) || ps;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		if (!s_keep_place) { // (closed with SELECT+START or PS: opens where it was, sub pages too)
			s_page = PAGE_MAIN;
			s_sel = 0;
		}
		s_keep_place = 0;
		s_pop = 0;
		s_bind = 0;
		s_reset_armed = 0;
		s_dirty = 1;
		s_changed = 0;
		rei_menu_states_changed();
	}
	return open;
}

int rei_menu_state_slot(void)
{
	return s_state_slot;
}

static int page_items(void)
{
	switch (s_page) {
	case PAGE_DISPLAY: return D_COUNT;
	case PAGE_CONTROLS: return ctrl_rows();
	case PAGE_TURBO: return 4;
	case PAGE_TOUCH: return 5;
	case PAGE_PORT: return port_rows();
	case PAGE_TEDIT: return 1;
	case PAGE_COMBO: return COMBO_ROWS;
	case PAGE_DIPS: return dip_items();
	case PAGE_CHEATS: return cheat_rows();
	case PAGE_STATES: return S_COUNT;
	case PAGE_ABOUT: return ABOUT_VISIBLE;
	default: return main_rows();
	}
}

static void go_page(int page)
{
	if (s_page == PAGE_MAIN)
		s_main_sel = s_sel;
	s_page = page;
	s_sel = (page == PAGE_MAIN) ? s_main_sel : 0;
	if (page == PAGE_MAIN && s_sel >= main_rows())
		s_sel = 0;
	s_reset_armed = 0;
}
// back one level: Turbo and Combo keys return to their row in the Port page; a Port page and Touch to their row in
// Controls; the touch editor to the Touch page; other pages to the main page
static void page_back(void)
{
	const int from = s_page;
	if (from == PAGE_TEDIT) {
		go_page(PAGE_TOUCH);
		s_sel = 2; // (Edit touch layout)
		return;
	}
	if (from == PAGE_TURBO || from == PAGE_COMBO) {
		go_page(PAGE_PORT);
		s_sel = port_row_of(from == PAGE_TURBO ? CK_TURBO : CK_COMBO);
		return;
	}
	if (from == PAGE_TOUCH || from == PAGE_PORT) {
		go_page(PAGE_CONTROLS);
		s_sel = ctrl_row_of(from == PAGE_PORT ? (s_kp ? CK_PORT2 : CK_PORT1) : CK_TOUCH);
		return;
	}
	go_page(PAGE_MAIN);
}

#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
// held directions repeat after 300 ms, then every 60 ms. Timed by the clock, not by loop passes: a slow redraw made a
// pass-counted repeat late and slow
static uint32_t key_repeat(uint32_t held, uint32_t pressed, uint64_t *next)
{
	const uint32_t dir = held & REPEAT_KEYS;
	const uint64_t now = rei_time_us();
	if (!dir || (pressed & REPEAT_KEYS)) {
		*next = now + 300000;
		return pressed;
	}
	if (now >= *next) {
		*next = now + 60000;
		pressed |= dir;
	}
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

// value names (items and the choice window)
static const char *const aspect[] = { "4:3", "Full screen", "Integer 2x" };
static const char *const filter[] = { "Smooth", "Sharp" };
static const char *const rotate[] = { "Auto", "Off", "Vertical L", "Vertical R" };
static const char *const scan[] = { "Off", "CRT soft", "CRT sharp", "Every 2 px", "Every 3 px", "Every 4 px" };
static const char *const scan_str[] = { "Light", "Medium", "Heavy" };
static const char *const border[] = { "Black", "Artwork", "Color" };
static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
static const char *const stick[] = { "Off", "Left", "Right", "Both" };
static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
static const char *const stages[REI_STAGE_COUNT] = {
	"1 Seattle", "2 San Francisco", "3 Las Vegas", "4 Texas", "5 Florida", "6 Chicago", "Final New York",
};

// Apply display settings to the game video (in the game list there is no game video yet: rei_video_init applies them)
static void apply_video(void)
{
	if (!s_list_mode)
		rei_video_apply_settings();
}

static void change(int dir);
static void engine_pick(int engine);
static int engine_list(int *e);
// an item with several values opens a window listing them all on the confirm button. The values of the selected item:
// names, count (0 = not such an item), current index
#define MAX_CHOICES 24
enum { CH_TEXT, CH_KEYS, CH_COMBO }; // how the choice window draws the values: names, button icons, key pairs
// the combo page's window: -1 = its first step (None or the first key), else the first key picked (the second step)
static int s_combo_first = -1, s_pop_keep;
// that window's key at row i: first step None (-1) then the ten keys, second step the keys but the first
static int combo_pick(int i)
{
	if (s_combo_first < 0)
		return i <= 0 ? -1 : s_combo_order[i - 1];
	for (int k = 0, n = 0; k < REI_COMBO_SRC; k++)
		if (s_combo_order[k] != s_combo_first && n++ == i)
			return s_combo_order[k];
	return -1;
}
static int s_choice_kind; // the last item_choices() listed (drawn that way)
static int item_choices(const char **out, int *cur, const char **title)
{
	static char num[MAX_CHOICES][24];
	int n = 0;
	*cur = 0;
	*title = "";
	s_choice_kind = CH_TEXT;
	if (s_page == PAGE_DISPLAY) {
		const char *const *v = NULL;
		switch (s_sel) {
		case D_ASPECT: v = aspect; n = REI_ASPECT_COUNT; *cur = g_settings.aspect; *title = "Screen"; break;
		case D_ROTATE: v = rotate; n = REI_ROT_COUNT; *cur = g_settings.rotate; *title = "Rotate"; break;
		case D_FILTER: v = filter; n = REI_FILTER_COUNT; *cur = g_settings.filter; *title = "Filter"; break;
		case D_SCAN: v = scan; n = REI_SCAN_COUNT; *cur = g_settings.scan_type; *title = "Scanlines"; break;
		case D_SCAN_STR: v = scan_str; n = REI_SCAN_STRENGTH_COUNT; *cur = g_settings.scan_strength; *title = "Scanline strength"; break;
		case D_BORDER: v = border; n = REI_BORDER_COUNT; *cur = g_settings.border; *title = "Border"; break;
		case D_BORDER_COL: v = border_col; n = REI_BCOL_COUNT; *cur = g_settings.border_color; *title = "Border color"; break;
		case D_THEME: v = s_theme_name; n = REI_THEME_COUNT; *cur = g_settings.theme; *title = "Theme"; break;
		default: return 0;
		}
		for (int i = 0; i < n; i++)
			out[i] = v[i];
	} else if (CTRL_PAGE) {
		int act;
		const int k = row_kind(s_sel, &act);
		int key, func;
		if (row_key(k, act, &key, &func)) {
			// buttons 3-6 and the turbo key can also be None
			const int with_none = k == CK_TURBO_KEY || (act >= REI_ACT_B3);
			n = REI_KEY_COUNT + with_none;
			for (int i = 0; i < n; i++)
				out[i] = rei_key_name(i);
			*cur = key;
			s_choice_kind = CH_KEYS;
			*title = row_key_name(k, act);
		} else if (k == CK_ENGINE) {
			int e[RE_PAD_ENGINE_COUNT];
			n = engine_list(e);
			for (int i = 0; i < n; i++) {
				out[i] = re_pad_engine_name(e[i]);
				if (e[i] == g_settings.input_engine)
					*cur = i;
			}
			*title = "Input engine";
		} else if (k == CK_DEVICE) {
			// the sources there are now (connected controllers only), with their devices
			int src[RE_PAD_SRC_COUNT];
			n = re_pad_sources(s_kp, src, cur);
			for (int i = 0; i < n; i++)
				out[i] = re_pad_source_text(src[i], s_devtext[i], sizeof(s_devtext[i]));
			*title = "Device";
		} else if (k == CK_TURBO_BTN) {
			n = ctrl_buttons();
			for (int i = 0; i < n; i++) {
				snprintf(num[i], sizeof(num[i]), "Button %d", i + 1);
				out[i] = num[i];
			}
			*cur = prt()->turbo_btn - 1;
			*title = "Turbo button";
		} else if (k == CK_TURBO_SPEED) {
			n = REI_TURBO_SPEED_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = turbo_speed[i];
			*cur = prt()->turbo_speed;
			*title = "Turbo speed";
		} else if (k == CK_STICK) {
			n = REI_STICK_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = stick[i];
			*cur = prt()->stick;
			*title = "Analog stick";
		}
	} else if (s_page == PAGE_STATES && s_sel == S_SLOT) {
		n = REI_STATE_SLOTS < MAX_CHOICES ? REI_STATE_SLOTS : MAX_CHOICES;
		for (int i = 0; i < n; i++) {
			snprintf(num[i], sizeof(num[i]), "Slot %d%s", i, s_slot_used[i] ? "" : "  (empty)");
			out[i] = num[i];
		}
		*cur = s_state_slot;
		*title = "Save slot";
	} else if (s_page == PAGE_COMBO && s_sel < REI_COMBO_SRC) {
		// two steps (the pairs are too many for one list): None or the first key, then the second key
		const int v = prt()->combo[s_combo_order[s_sel]];
		n = s_combo_first < 0 ? 1 + REI_COMBO_SRC : REI_COMBO_SRC - 1;
		*cur = 0;
		for (int i = 0, found = 0; i < n; i++) {
			const int key = combo_pick(i);
			out[i] = key < 0 ? "None" : rei_key_name(key);
			if (key >= 0 && (v & (1 << key)) && !found && (s_combo_first < 0 || (v & (1 << s_combo_first)))) {
				*cur = i;
				found = 1;
			}
		}
		s_choice_kind = CH_COMBO;
		*title = "Combo for";
	} else if (s_page == PAGE_DIPS && s_sel < rei_core_dip_count()) {
		n = rei_core_dip_options(s_sel);
		if (n > MAX_CHOICES)
			n = MAX_CHOICES;
		for (int i = 0; i < n; i++)
			out[i] = rei_core_dip_option_name(s_sel, i);
		*cur = rei_core_dip_get(s_sel) < 0 ? 0 : rei_core_dip_get(s_sel);
		*title = rei_core_dip_name(s_sel);
	} else if (s_page == PAGE_CHEATS) {
		int ci = 0;
		const int k = cheat_kind(s_sel, &ci);
		if (k == HK_STAGE) {
			n = REI_STAGE_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = stages[i];
			*cur = g_settings.start_stage;
			*title = "Start stage";
		} else if (k == HK_FILE) {
			n = rei_core_cheat_options(ci);
			if (n > MAX_CHOICES)
				n = MAX_CHOICES;
			for (int i = 0; i < n; i++)
				out[i] = rei_core_cheat_option_name(ci, i);
			*cur = rei_core_cheat_get(ci);
			*title = rei_core_cheat_name(ci);
		}
	}
	return n >= 3 ? n : 0; // two values: the button just flips them
}
static void set_choice(int idx)
{
	const char *names[MAX_CHOICES], *title;
	int cur, n = item_choices(names, &cur, &title);
	int eact;
	if (s_page == PAGE_COMBO && s_sel < REI_COMBO_SRC) { // the combo window's two steps
		const int key = combo_pick(idx), src = s_combo_order[s_sel];
		if (s_combo_first < 0 && key >= 0) {
			s_combo_first = key; // on to the second key: the window stays
			s_pop_keep = 1;
			return;
		}
		prt()->combo[src] = key < 0 || s_combo_first < 0 ? 0 : (1 << s_combo_first) | (1 << key);
		s_combo_first = -1;
		return;
	}
	if (CTRL_PAGE && row_kind(s_sel, &eact) == CK_DEVICE) { // the source picked, set even when it is the one shown
		g_settings.pad_src[s_kp] = re_pad_pick_source(s_kp, idx);
		return;
	}
	if (CTRL_PAGE && row_kind(s_sel, &eact) == CK_ENGINE) {
		// the engine directly (stepping would pass RePad on the way and load it)
		int e[RE_PAD_ENGINE_COUNT];
		const int ne = engine_list(e);
		if (idx >= 0 && idx < ne && e[idx] != g_settings.input_engine)
			engine_pick(e[idx]);
		return;
	}
	s_quiet = 1; // (the steps between pass other values: warn once at the end)
	for (int guard = 0; n && cur != idx && guard < MAX_CHOICES + 2; guard++) {
		change(+1); // the same steps as the right button (wraps, applies)
		n = item_choices(names, &cur, &title);
	}
	s_quiet = 0;
	int act, key, func;
	if (CTRL_PAGE && row_key(row_kind(s_sel, &act), act, &key, &func))
		warn_clash(key, func);
	else if (s_page == PAGE_COMBO && s_sel < REI_COMBO_SRC && prt()->combo[s_combo_order[s_sel]])
		warn_clash(s_combo_order[s_sel], KF_COMBO + s_combo_order[s_sel]);
}

// the input engine picked (left / right or its choice window): at once, either way. VitaControl keeping RePad out is
// asked about when Back leaves Controls (switch it off and restart, or back to Native)
static void engine_pick(int engine)
{
	g_settings.input_engine = engine;
	re_pad_engine_set(engine);
	if (engine == RE_PAD_ENGINE_REPAD && !re_pad_plugin() && !re_pad_vitacontrol_listed())
		rei_osd_show("RePad could not start: another controller plugin is running");
	s_changed = 1;
}
// the engines offered: Native, RePad, VitaControl when a plugin list names it
static int engine_list(int *e)
{
	int n = 0;
	e[n++] = RE_PAD_ENGINE_NATIVE;
	e[n++] = RE_PAD_ENGINE_REPAD;
	if (re_pad_vitacontrol_state() != RE_PAD_VC_NONE)
		e[n++] = RE_PAD_ENGINE_VITACONTROL;
	return n;
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_settings.show_fps = !g_settings.show_fps; break;
		case D_ASPECT: g_settings.aspect = wrap(g_settings.aspect + dir, REI_ASPECT_COUNT); break;
		case D_ROTATE: g_settings.rotate = wrap(g_settings.rotate + dir, REI_ROT_COUNT); break;
		case D_FILTER: g_settings.filter = wrap(g_settings.filter + dir, REI_FILTER_COUNT); break;
		case D_SCAN: g_settings.scan_type = wrap(g_settings.scan_type + dir, REI_SCAN_COUNT); break;
		case D_SCAN_STR: g_settings.scan_strength = wrap(g_settings.scan_strength + dir, REI_SCAN_STRENGTH_COUNT); break;
		case D_BORDER: g_settings.border = wrap(g_settings.border + dir, REI_BORDER_COUNT); break;
		case D_BORDER_COL: g_settings.border_color = wrap(g_settings.border_color + dir, REI_BCOL_COUNT); break;
		case D_THEME:
			g_settings.theme = wrap(g_settings.theme + dir, REI_THEME_COUNT);
			s_drawn_h = 0; // (every colour changes: the whole screen again)
			break;
		default: return;
		}
		apply_video();
		s_changed = 1;
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO:
	case PAGE_TOUCH:
	case PAGE_PORT: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_ACT && act >= 0) // buttons 3-6 can also be None
			prt()->keymap[act] = wrap(prt()->keymap[act] + dir, act >= REI_ACT_B3 ? REI_KEY_COUNT + 1 : REI_KEY_COUNT);
		else if (k == CK_ENGINE)
			engine_pick(re_pad_engine_step(g_settings.input_engine, dir)); // (VitaControl only when a list names it)
		else if (k == CK_DEVICE) // (through the sources there are now)
			g_settings.pad_src[s_kp] = re_pad_step_source(s_kp, dir);
		else if (k == CK_PORT1 || k == CK_PORT2) // (the player's device, as its Device row)
			g_settings.pad_src[k == CK_PORT2] = re_pad_step_source(k == CK_PORT2, dir);
		else if (k == CK_TURBO_KEY)
			prt()->turbo_key = wrap(prt()->turbo_key + dir, REI_KEY_COUNT + 1); // + None (off)
		else if (k == CK_TURBO_BTN) // 1..number of game buttons
			prt()->turbo_btn = wrap(prt()->turbo_btn - 1 + dir, ctrl_buttons()) + 1;
		else if (k == CK_TURBO_SPEED)
			prt()->turbo_speed = wrap(prt()->turbo_speed + dir, REI_TURBO_SPEED_COUNT);
		else if (k == CK_STICK)
			prt()->stick = wrap(prt()->stick + dir, REI_STICK_COUNT);
		else if (k == CK_T_ON)
			g_settings.touch_keys = !g_settings.touch_keys;
		else if (k == CK_T_SHOW)
			g_settings.touch_show = !g_settings.touch_show;
		else if (k == CK_CONFIRM)
			g_settings.menu_confirm = wrap(g_settings.menu_confirm + dir, REI_CONFIRM_COUNT);
		else
			return;
		s_changed = 1;
		int key, func;
		if (row_key(k, act, &key, &func))
			warn_clash(key, func);
		break;
	}
	case PAGE_STATES:
		if (s_sel == S_SLOT)
			s_state_slot = wrap(s_state_slot + dir, REI_STATE_SLOTS);
		break;
	case PAGE_COMBO: {
		if (s_sel >= REI_COMBO_SRC)
			return;
		int *v = &prt()->combo[s_combo_order[s_sel]];
		*v = combo_choice(wrap(combo_index(*v) + dir, COMBO_CHOICES));
		s_changed = 1;
		if (*v)
			warn_clash(s_combo_order[s_sel], KF_COMBO + s_combo_order[s_sel]);
		break;
	}
	case PAGE_DIPS:
		if (s_sel < rei_core_dip_count()) {
			const int n = rei_core_dip_options(s_sel), cur = rei_core_dip_get(s_sel);
			if (n > 0) {
				rei_core_dip_set(s_sel, wrap((cur < 0 ? 0 : cur) + dir, n));
				save_dips();
				s_changed = 1;
			}
		}
		break;
	case PAGE_CHEATS: {
		int ci = 0;
		const int k = cheat_kind(s_sel, &ci);
		if (k == HK_STAGE) {
			g_settings.start_stage = wrap(g_settings.start_stage + dir, REI_STAGE_COUNT);
			rei_core_set_start_stage(g_settings.start_stage);
		} else if (k == HK_HEALTH) {
			g_settings.cheat_health = !g_settings.cheat_health;
			rei_core_set_infinite_health(g_settings.cheat_health);
		} else if (k == HK_FILE && rei_core_cheat_options(ci) > 0)
			rei_core_cheat_set(ci, wrap(rei_core_cheat_get(ci) + dir, rei_core_cheat_options(ci)));
		break;
	}
	}
}

// ---- touch layout editor (Controls > Touch controls > Edit touch layout) ----
// Drag on an empty spot: a new button (then its key). Tap a button: its key, or Delete. Drag a button: move it; drag its
// lower right corner: its size. Square adds one in the middle, Triangle opens the selected one's keys (no touch needed).
// START, the back button or Done leave (the layout is saved then)
#define TE_MIN 42    // smallest button side (sizes go in TE_STEP steps)
#define TE_STEP 6
#define TE_GRAB 22   // an edge or corner is grabbed this far from it, inside or outside the button
#define TE_POP_N (REI_TKEY_COUNT + 3) // the keys, Combo >, Duplicate, then Delete
#define TE_ROW_COMBO REI_TKEY_COUNT
#define TE_ROW_DUP (REI_TKEY_COUNT + 1)
#define TE_ROW_DEL (REI_TKEY_COUNT + 2)
#define TE_CPOP_N (te_cpop == 2 ? REI_COMBO_SRC - 1 : REI_COMBO_SRC) // a pair: its first key, then its second (e.g. L, R)
#define TE_CPOP_RH 26
#define TE_POP_RH 30 // (with Duplicate the window still fits the screen)
enum { TE_IDLE, TE_NEW, TE_MOVE, TE_SIZE };
enum { TE_L = 1, TE_R = 2, TE_T = 4, TE_B = 8 }; // edges being dragged (TE_SIZE)
static int te_sel = -1, te_mode, te_prev_n, te_sx, te_sy, te_ox, te_oy, te_ow, te_oh, te_moved, te_pop = -1, te_pop_sel;
static int te_cpop, te_cpop_sel, te_cfirst; // the pair lists (from the key window's Combo row): 1 its first key, 2 its second
static int te_ckey(int i) // the pair list's key at row i (the second list leaves the first key out)
{
	for (int k = 0, n = 0; k < REI_COMBO_SRC; k++)
		if ((te_cpop == 1 || s_combo_order[k] != te_cfirst) && n++ == i)
			return s_combo_order[k];
	return -1;
}
static int te_edges;
static int te_snap(int v) // a size in TE_STEP steps, at least TE_MIN
{
	v = (v + TE_STEP / 2) / TE_STEP * TE_STEP;
	return v < TE_MIN ? TE_MIN : v;
}
// the edges of button a a touch grabs (0: none). Within TE_GRAB of an edge, outside too; inside a small button at
// most a quarter of its side, so its middle still moves it
static int te_edges_at(const ReiTouchArea *a, int x, int y)
{
	const int gx = a->w / 4 < TE_GRAB ? a->w / 4 : TE_GRAB, gy = a->h / 4 < TE_GRAB ? a->h / 4 : TE_GRAB;
	if (x < a->x - TE_GRAB || x >= a->x + a->w + TE_GRAB || y < a->y - TE_GRAB || y >= a->y + a->h + TE_GRAB)
		return 0;
	int e = 0;
	if (x < a->x + gx)
		e |= TE_L;
	else if (x >= a->x + a->w - gx)
		e |= TE_R;
	if (y < a->y + gy)
		e |= TE_T;
	else if (y >= a->y + a->h - gy)
		e |= TE_B;
	return e;
}
static int te_nx, te_ny, te_nw, te_nh; // the button being drawn (TE_NEW)
static void tedit_open(void)
{
	te_sel = -1;
	te_mode = TE_IDLE;
	te_prev_n = 1; // (a finger already down does nothing until it is lifted)
	te_pop = -1;
	s_drawn_h = 0;
	s_dirty = 1;
}
static void te_pop_rect(int *x, int *y, int *w, int *h)
{
	*w = 320;
	*h = 64 + (te_cpop ? TE_CPOP_N * TE_CPOP_RH : TE_POP_N * TE_POP_RH) + 12;
	*x = (SCREEN_W - *w) / 2;
	*y = (SCREEN_H - *h) / 2;
}
static int te_done_hit(int x, int y) { return x >= SCREEN_W - 130 && y < 46; }
static void te_pop_open(int i)
{
	const ReiTouchArea *a = rei_touch_area(i);
	if (!a)
		return;
	te_pop = i;
	te_cpop = 0;
	te_pop_sel = REI_TKEY_IS_COMBO(a->key) ? TE_ROW_COMBO : a->key;
}
static void te_pop_apply(int row)
{
	ReiTouchArea *a = rei_touch_area(te_pop);
	if (te_cpop) { // a key pair: the first key, then the second
		const int key = row >= 0 && row < TE_CPOP_N ? te_ckey(row) : -1;
		if (te_cpop == 1 && key >= 0) {
			te_cfirst = key;
			te_cpop = 2;
			te_cpop_sel = 0;
			return;
		}
		if (a && key >= 0) {
			a->key = (int16_t)(REI_TKEY_COMBO + (1 << te_cfirst) + (1 << key));
			rei_touch_changed();
		}
		te_cpop = 0;
		te_pop = -1;
		return;
	}
	if (row == TE_ROW_COMBO) { // the pair lists (entered from the key window)
		te_cpop = 1;
		te_cpop_sel = 0;
		for (int i = REI_COMBO_SRC - 1; i >= 0; i--) // (on the button's first key, if it has a pair)
			if (a && REI_TKEY_IS_COMBO(a->key) && ((a->key - REI_TKEY_COMBO) & (1 << s_combo_order[i])))
				te_cpop_sel = i;
		return;
	}
	if (row == TE_ROW_DUP) { // a copy beside it (same size and key), then the copy's key
		if (a) {
			const int w = a->w, h = a->h, key = a->key;
			int nx = a->x + 24, ny = a->y + 24;
			if (nx + w > SCREEN_W)
				nx = a->x >= 24 ? a->x - 24 : 0;
			if (ny + h > SCREEN_H)
				ny = a->y >= 24 ? a->y - 24 : 0;
			const int i = rei_touch_add(nx, ny, w, h, key);
			if (i >= 0) {
				te_sel = i;
				te_pop_open(i);
				return;
			}
		}
		te_pop = -1;
		return;
	}
	if (row == TE_ROW_DEL) { // Delete
		rei_touch_remove(te_pop);
		te_sel = -1;
	} else if (a && row >= 0 && row < REI_TKEY_COUNT) {
		a->key = (int8_t)row;
		rei_touch_changed();
	}
	te_pop = -1;
}
static void tedit_leave(void)
{
	rei_touch_save();
	te_pop = -1;
	page_back();
	s_drawn_h = 0;
	s_dirty = 1;
}
static void tedit_update(uint32_t pressed)
{
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	int x[6], y[6];
	const int n = rei_touch_read(x, y);
	const int down = n > 0 && te_prev_n == 0, held = n > 0, up = n == 0 && te_prev_n > 0;
	te_prev_n = n;
	if (te_pop >= 0) { // key window: touch a row, or up/down and confirm; back or a touch outside closes it
		int *sel = te_cpop ? &te_cpop_sel : &te_pop_sel;
		const int rows = te_cpop ? TE_CPOP_N : TE_POP_N, rh = te_cpop ? TE_CPOP_RH : TE_POP_RH;
		if (pressed & SCE_CTRL_UP) *sel = wrap(*sel - 1, rows);
		if (pressed & SCE_CTRL_DOWN) *sel = wrap(*sel + 1, rows);
		if (pressed & ok)
			te_pop_apply(*sel);
		else if (pressed & (back | SCE_CTRL_START)) {
			if (te_cpop)
				te_cpop--; // (back to the first key, or to the key window)
			else
				te_pop = -1;
		} else if (down) {
			int wx, wy, ww, wh;
			te_pop_rect(&wx, &wy, &ww, &wh);
			const int row = (y[0] - wy - 60) / rh;
			if (x[0] >= wx && x[0] < wx + ww && y[0] >= wy + 60 && row >= 0 && row < rows)
				te_pop_apply(row);
			else if (!(x[0] >= wx && x[0] < wx + ww && y[0] >= wy && y[0] < wy + wh)) {
				te_cpop = 0;
				te_pop = -1;
			}
		}
		if (pressed || down)
			s_dirty = 1;
		return;
	}
	if (pressed & (back | SCE_CTRL_START)) {
		tedit_leave();
		return;
	}
	if (pressed & SCE_CTRL_SQUARE) { // a new button in the middle, then its key
		const int i = rei_touch_add(SCREEN_W / 2 - 50, SCREEN_H / 2 - 50, 100, 100, REI_KEY_CROSS);
		if (i >= 0) {
			te_sel = i;
			te_pop_open(i);
		}
	}
	if ((pressed & SCE_CTRL_TRIANGLE) && te_sel >= 0)
		te_pop_open(te_sel);
	if (down) {
		if (te_done_hit(x[0], y[0])) {
			tedit_leave();
			return;
		}
		te_sx = x[0];
		te_sy = y[0];
		te_moved = 0;
		int hit = -1, edges = 0;
		// an edge or corner of the selected button first (its grab zone reaches outside it), then any button on top
		if (te_sel >= 0 && rei_touch_area(te_sel)) {
			edges = te_edges_at(rei_touch_area(te_sel), x[0], y[0]);
			if (edges)
				hit = te_sel;
		}
		for (int i = rei_touch_count() - 1; i >= 0 && hit < 0; i--) { // (the last drawn is on top)
			const ReiTouchArea *a = rei_touch_area(i);
			if (x[0] >= a->x && x[0] < a->x + a->w && y[0] >= a->y && y[0] < a->y + a->h) {
				hit = i;
				edges = te_edges_at(a, x[0], y[0]);
			}
		}
		if (hit >= 0) {
			const ReiTouchArea *a = rei_touch_area(hit);
			te_sel = hit;
			te_edges = edges;
			te_mode = edges ? TE_SIZE : TE_MOVE;
			te_ox = a->x;
			te_oy = a->y;
			te_ow = a->w;
			te_oh = a->h;
		} else {
			te_mode = TE_NEW;
			te_sel = -1;
			te_nx = x[0];
			te_ny = y[0];
			te_nw = te_nh = 0;
		}
		s_dirty = 1;
	} else if (held && te_mode != TE_IDLE) {
		const int dx = x[0] - te_sx, dy = y[0] - te_sy;
		if (dx > 8 || dx < -8 || dy > 8 || dy < -8)
			te_moved = 1;
		ReiTouchArea *a = rei_touch_area(te_sel);
		if (te_mode == TE_NEW) { // (from the first touch, in TE_STEP steps)
			te_nw = (dx < 0 ? -dx : dx) / TE_STEP * TE_STEP;
			te_nh = (dy < 0 ? -dy : dy) / TE_STEP * TE_STEP;
			te_nx = dx < 0 ? te_sx - te_nw : te_sx;
			te_ny = dy < 0 ? te_sy - te_nh : te_sy;
		} else if (a && te_moved && te_mode == TE_MOVE) {
			const int nx = te_ox + dx, ny = te_oy + dy;
			a->x = (int16_t)(nx < 0 ? 0 : nx > SCREEN_W - a->w ? SCREEN_W - a->w : nx);
			a->y = (int16_t)(ny < 0 ? 0 : ny > SCREEN_H - a->h ? SCREEN_H - a->h : ny);
			rei_touch_changed();
		} else if (a && te_mode == TE_SIZE) { // the grabbed edges follow the finger, the others stay; TE_STEP steps
			int x0 = te_ox, x1 = te_ox + te_ow, y0 = te_oy, y1 = te_oy + te_oh;
			if (te_edges & TE_L) {
				const int lim = x0 + dx < 0 ? -x0 : dx;
				x0 = x1 - te_snap(te_ow - lim);
				if (x0 < 0)
					x0 = x1 - (x1 / TE_STEP * TE_STEP < TE_MIN ? TE_MIN : x1 / TE_STEP * TE_STEP);
			} else if (te_edges & TE_R) {
				x1 = x0 + te_snap(te_ow + dx);
				if (x1 > SCREEN_W)
					x1 = x0 + (SCREEN_W - x0) / TE_STEP * TE_STEP;
			}
			if (te_edges & TE_T) {
				const int lim = y0 + dy < 0 ? -y0 : dy;
				y0 = y1 - te_snap(te_oh - lim);
				if (y0 < 0)
					y0 = y1 - (y1 / TE_STEP * TE_STEP < TE_MIN ? TE_MIN : y1 / TE_STEP * TE_STEP);
			} else if (te_edges & TE_B) {
				y1 = y0 + te_snap(te_oh + dy);
				if (y1 > SCREEN_H)
					y1 = y0 + (SCREEN_H - y0) / TE_STEP * TE_STEP;
			}
			a->x = (int16_t)x0;
			a->y = (int16_t)y0;
			a->w = (int16_t)(x1 - x0);
			a->h = (int16_t)(y1 - y0);
			rei_touch_changed();
		}
		s_dirty = 1;
	} else if (up) {
		if (te_mode == TE_NEW && te_nw >= TE_MIN && te_nh >= TE_MIN) {
			const int i = rei_touch_add(te_nx, te_ny, te_nw, te_nh, REI_KEY_CROSS);
			if (i >= 0) {
				te_sel = i;
				te_pop_open(i);
			}
		} else if (te_mode == TE_MOVE && !te_moved)
			te_pop_open(te_sel); // a tap: its key
		te_mode = TE_IDLE;
		s_dirty = 1;
	}
}

// Back out of a page (the back button or the Back row): leaving Controls itself, first the restart the chosen input
// engine needs, if any (its window: restart, or back to Native)
static void back_out(void)
{
	if (s_page == PAGE_CONTROLS && (s_vc_ask = engine_restart_ask()) != 0)
		s_drawn_h = 0;
	else
		page_back();
}

static rei_menu_result confirm(void)
{
	switch (s_page) {
	case PAGE_MAIN:
		switch (main_kind(s_sel)) {
		case M_RESUME: return REI_MENU_CLOSE;
		case M_RESET:
			if (!s_reset_armed) {
				s_reset_armed = 1;
				return REI_MENU_STAY;
			}
			return REI_MENU_RESET;
		case M_STATES: go_page(PAGE_STATES); break;
		case M_DISPLAY: go_page(PAGE_DISPLAY); break;
		case M_CONTROLS: go_page(PAGE_CONTROLS); break;
		case M_DIPS:
			if (rei_core_dip_count() > 0)
				go_page(PAGE_DIPS);
			else
				rei_osd_show("No DIP switches for %s", s_set);
			break;
		case M_CHEATS:
			if (any_cheats())
				go_page(PAGE_CHEATS);
			else
				rei_osd_show("No cheats: put %s.ini in cheats/", s_set);
			break;
		case M_GAME_SAVE: // this game only (the global settings file is not written)
			if (rei_settings_save_game())
				rei_osd_show("Saved settings for %s", s_set);
			else
				rei_osd_show("Could not save game settings");
			break;
		case M_GAME_DEL:
			if (rei_settings_delete_game()) {
				apply_video(); // (global values right away)
				rei_osd_show("Deleted settings for %s", s_set);
			} else
				rei_osd_show("Could not delete game settings");
			break;
		case M_ABOUT:
			go_page(PAGE_ABOUT);
			s_about_scroll = 0;
			break;
		case M_SELECT_GAME: return REI_MENU_SELECT_GAME;
		case M_REFRESH: return REI_MENU_REFRESH;
		case M_EXIT: return REI_MENU_EXIT;
		case M_BACK: return REI_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO:
	case PAGE_TOUCH:
	case PAGE_PORT: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_PORT1 || k == CK_PORT2) {
			s_kp = k == CK_PORT2;
			go_page(PAGE_PORT);
			break;
		}
		if (k == CK_BIND_ALL) { // every key row in turn, from button 1
			s_sel = port_row_of_act(REI_ACT_ATTACK);
			bind_begin(1);
			break;
		}
		if (k == CK_RESET_PORT) {
			rei_settings_default_port(re_pad_player_profile(s_kp)); // (the device's controls)
			s_changed = 1;
			rei_osd_show("%dP controls reset", s_kp + 1);
			break;
		}
		if (k == CK_T_EDIT) {
			go_page(PAGE_TEDIT);
			tedit_open();
			break;
		}
		if (k == CK_T_CLEAR) {
			if (!s_reset_armed) {
				s_reset_armed = 1; // (press again to clear)
				break;
			}
			s_reset_armed = 0;
			rei_touch_clear();
			rei_touch_save();
			rei_osd_show("Touch layout cleared");
			break;
		}
		if (k == CK_TOUCH) go_page(PAGE_TOUCH);
		else if (k == CK_BACK) back_out();
		else if (k == CK_TURBO) go_page(PAGE_TURBO);
		else if (k == CK_COMBO) go_page(PAGE_COMBO);
		else if (k == CK_DEFAULTS) {
			rei_settings_default_keys();
			s_changed = 1;
		} else change(+1);
		break;
	}
	case PAGE_DIPS:
		if (s_sel == dip_items() - 1) go_page(PAGE_MAIN);
		else if (s_sel == dip_items() - 2) {
			rei_core_dip_defaults();
			save_dips();
			s_changed = 1;
		} else change(+1);
		break;
	case PAGE_CHEATS: {
		int ci = 0;
		if (cheat_kind(s_sel, &ci) == HK_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	}
	case PAGE_ABOUT:
		go_page(PAGE_MAIN);
		break;
	case PAGE_COMBO:
		if (s_sel == COMBO_ROWS - 1) page_back();
		else change(+1);
		break;
	case PAGE_STATES:
		if (s_sel == S_SAVE) return REI_MENU_SAVE;
		if (s_sel == S_LOAD) {
			if (s_slot_used[s_state_slot])
				return REI_MENU_LOAD;
			rei_osd_show("Slot %d is empty", s_state_slot);
		}
		if (s_sel == S_BACK) go_page(PAGE_MAIN);
		if (s_sel == S_SLOT) change(+1);
		break;
	}
	return REI_MENU_STAY;
}

rei_menu_result rei_menu_update(uint32_t buttons)
{
	const int ps_closed = ps_poll();
	if (ps_closed)
		s_pop = 0;
	static uint64_t rep_next;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_next);
	s_prev_buttons = buttons;
	if (pressed)
		s_dirty = 1;
	if (re_pad_changed()) { // a controller came or went: the Port pages and the Device list show them, the whole panel again
		s_dirty = 1;
		s_drawn_h = 0;
		if (!re_pad_player_shown(s_kp) && (s_page == PAGE_PORT || s_page == PAGE_TURBO || s_page == PAGE_COMBO)) {
			// the device of this port went away (its row is gone): back to Controls
			s_pop = s_bind = 0;
			go_page(PAGE_CONTROLS);
			s_kp = 0;
		}
		if (s_sel >= page_items())
			s_sel = page_items() - 1;
	}
	rei_menu_result r = REI_MENU_STAY;
	if (s_page == PAGE_TEDIT && !ps_closed) { // touch layout editor: touch and its own buttons
		tedit_update(pressed);
		return REI_MENU_STAY;
	}
	const int n = page_items();
	if (s_vc_ask) {
		// VitaControl off: confirm = its lines commented out in the plugin lists (copies kept), then the Vita restarts
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if ((pressed & ok) && !ps_closed) {
			const int kind = s_vc_ask;
			s_vc_ask = 0;
			const int r = kind == 1 ? re_pad_vitacontrol_off() : kind == 2 ? re_pad_vitacontrol_on() : 1;
			REI_LOG("input engine restart %d: %d", kind, r);
			if (r > 0) {
				rei_settings_save(); // (the chosen engine stays)
				scePowerRequestColdReset();
			} else
				rei_osd_show(r < 0 ? "Could not change the plugin list (config.txt)" : "VitaControl is not in the plugin list");
		} else if ((pressed & (back | SCE_CTRL_START)) || ps_closed) {
			s_vc_ask = 0; // cancelled: Native again (the chosen engine cannot run without the restart), and on out of Controls
			g_settings.input_engine = RE_PAD_ENGINE_NATIVE;
			re_pad_engine_set(g_settings.input_engine);
			s_changed = 1;
			page_back();
		}
		if (!s_vc_ask) {
			s_dirty = 1;
			s_drawn_h = 0;
		}
		return REI_MENU_STAY;
	}
	if (s_bind) {
		// key binding: wait until every button is up (the confirm press), then the first button pressed is the key.
		// The d-pad, the PS button or 5 s without a button cancel
		int act, key = -1, cancel = ps_closed || !CTRL_PAGE;
		const int k = row_kind(s_sel, &act);
		uint32_t keys = 0;
		for (int i = 0; i < REI_KEY_COUNT; i++)
			keys |= rei_key_mask(i);
		// a player's own page: its own device alone (another controller's buttons are not taken). Keys are looked for
		// from the last: a controller's L2 / R2 is also L / R in the menus, the key is L2 / R2
		uint32_t bb = buttons, bp = pressed;
		if (s_page == PAGE_PORT || s_page == PAGE_TURBO) {
			SceCtrlData pd;
			re_pad_read(s_kp, &pd);
			bb = pd.buttons;
			bp = bb & ~s_bind_prev;
			s_bind_prev = bb;
		}
		if (!s_bind_ready) {
			if (!((buttons | bb) & (keys | REPEAT_KEYS)))
				s_bind_ready = 1;
		} else {
			for (int i = REI_KEY_COUNT - 1; i >= 0 && key < 0; i--)
				if (bp & rei_key_mask(i))
					key = i;
			if (key < 0 && ((pressed | bp) & REPEAT_KEYS))
				cancel = 1;
		}
		if (key < 0 && rei_time_us() - s_bind_t > BIND_US)
			cancel = 1;
		int ck, cf;
		if (!row_key(k, act, &ck, &cf))
			cancel = 1;
		if (key >= 0 && !cancel) {
			row_set_key(k, act, key);
			s_changed = 1;
			warn_clash(key, cf);
			if (s_bind_all && k == CK_ACT && act != REI_ACT_START) { // Set all controls: the next key row
				s_sel++;
				s_bind_ready = 0;
				s_bind_t = rei_time_us();
			} else
				s_bind = 0;
			s_dirty = 1;
		} else if (cancel) {
			s_bind = 0;
			s_dirty = 1;
		}
		return REI_MENU_STAY;
	}
	{
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		const char *names[MAX_CHOICES], *title;
		int cur;
		const int nc = item_choices(names, &cur, &title);
		if (s_pop && !ps_closed) { // choice window: up/down, confirm sets, back closes
			if (s_pop_sel >= nc) // (the Device list got shorter)
				s_pop_sel = nc > 0 ? nc - 1 : 0;
			if (!nc)
				s_pop = 0;
			else if (pressed & SCE_CTRL_UP)
				s_pop_sel = wrap(s_pop_sel - 1, nc);
			else if (pressed & SCE_CTRL_DOWN)
				s_pop_sel = wrap(s_pop_sel + 1, nc);
			else if (pressed & ok) {
				set_choice(s_pop_sel);
				if (s_pop_keep) { // (the combo window's second key next)
					s_pop_keep = 0;
					s_pop_sel = 0;
				} else
					s_pop = 0;
				if (s_page != PAGE_CHEATS) // (cheats are not saved)
					s_changed = 1;
			} else if (pressed & (back | SCE_CTRL_START)) {
				if (s_combo_first >= 0) { // the combo window: back to its first key
					for (int i = 0; i < REI_COMBO_SRC; i++)
						if (s_combo_order[i] == s_combo_first)
							s_pop_sel = i + 1;
					s_combo_first = -1;
				} else
					s_pop = 0;
			}
			if (!s_pop) {
				s_drawn_h = 0; // the whole panel again (the window covered part of it)
				s_combo_first = -1;
			}
			return REI_MENU_STAY;
		}
		if ((pressed & SCE_CTRL_SQUARE) && s_page == PAGE_COMBO && s_sel < REI_COMBO_SRC && !ps_closed) { // Square: no combo
			prt()->combo[s_combo_order[s_sel]] = 0;
			s_changed = 1;
			s_dirty = 1;
			return REI_MENU_STAY;
		}
		if ((pressed & SCE_CTRL_SQUARE) && CTRL_PAGE && !ps_closed) { // a key row that can be none: Square sets None
			int act, ck, cf;
			const int k = row_kind(s_sel, &act);
			if (row_key(k, act, &ck, &cf) && (k == CK_TURBO_KEY || act >= REI_ACT_B3)) {
				row_set_key(k, act, REI_KEY_NONE);
				s_changed = 1;
				s_dirty = 1;
				return REI_MENU_STAY;
			}
		}
		if ((pressed & ok) && CTRL_PAGE && !ps_closed) { // a key row: wait for the button (no choice window)
			int act, ck, cf;
			if (row_key(row_kind(s_sel, &act), act, &ck, &cf)) {
				bind_begin(0);
				return REI_MENU_STAY;
			}
		}
		if ((pressed & ok) && nc && !ps_closed) {
			s_pop = 1;
			s_pop_sel = cur;
			s_drawn_h = 0;
			return REI_MENU_STAY;
		}
	}
	if (s_page == PAGE_ABOUT) { // up/down scroll
		const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
		if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~REPEAT_KEYS;
	}
	if (pressed & SCE_CTRL_UP) {
		s_sel = wrap(s_sel - 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_DOWN) {
		s_sel = wrap(s_sel + 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_LEFT)
		change(-1);
	if (pressed & SCE_CTRL_RIGHT)
		change(+1);
	// confirm/back follow the setting (Cross/Circle). If the setting changes during this input, use the setting at press time
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = REI_MENU_CLOSE;
		else
			back_out();
	}
	if (pressed & SCE_CTRL_START) {
		r = REI_MENU_CLOSE;
		s_keep_place = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO && !s_list_mode;
	}
	if (ps_closed) { // PS closes the menu as SELECT+START does (opens again where it was)
		if (s_page == PAGE_TEDIT) {
			rei_touch_save();
			page_back();
		}
		r = REI_MENU_CLOSE;
		s_keep_place = !s_list_mode;
	}
	// memory card write only when the user changed something (no card I/O otherwise while a game is loaded)
	if (r != REI_MENU_STAY && r != REI_MENU_SAVE && s_changed) {
		rei_settings_save();
		s_changed = 0;
	}
	return r;
}

// ================================ drawing ================================
// "Re" white + "iREM" in the brand colour (Re franchise naming rule, as the app art). Returns the width
static int draw_brand(UiCanvas *c, int x, int y, float scale)
{
	int w = ui_text(c, x, y, COL_TEXT, scale, "Re");
	w += ui_text(c, x + w, y, COL_BRAND, scale, "iREM");
	return w;
}

// one About line (heading lines start with '#'; the "ReiREM" heading uses the brand colours)
static void about_line(UiCanvas *c, int x, int y, const char *t)
{
	if (!t[0])
		return;
	if (!strncmp(t, "#ReiREM", 7)) {
		const int w = draw_brand(c, x, y, 1.0f);
		ui_text(c, x + w, y, COL_TEXT, 1.0f, t + 7);
		return;
	}
	const int head = t[0] == '#';
	ui_text(c, x, y, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT), head ? 1.0f : 0.85f, t + head);
}

#define VAL_ON "\x01on"   // item value: draw a toggle switch (two-state items only)
#define VAL_OFF "\x01off"
// the selected row: a soft bar with a shadow and a blinking outline (a steady one under the choice window)
static void sel_bar(UiCanvas *c, int x, int by, int w, int h)
{
	rrect(c, x, by + 3, w, h, 6, theme()->shade);
	rrect(c, x, by, w, h, 6, COL_HILITE);
	if (s_pop) // the choice window is over it
		rrect_outline(c, x, by, w, h, 6, 2, COL_FOCUS_STILL);
	else
		focus_at(c, x, by, w, h);
}
static void item(UiCanvas *c, int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel)
		sel_bar(c, px + 12, ty - 7 - (LINE_H - 6) / 2, PANEL_W - 40, LINE_H - 6); // bar centred on the text
	ui_text(c, px + 40, ty, i == sel ? COL_TEXT : COL_ITEM, 1.0f, name);
	if (value && value[0] == 1) {
		// two-state item as a toggle switch: a pill with the knob on the right (on, accent colour) or left (off, grey)
		const int on = value[2] == 'n';
		const float r = 10.0f, x0 = (float)(px + VALUE_X) + r, x1 = x0 + 26.0f, cy = (float)ty - 7.0f;
		const uint32_t track = on ? theme()->toggle_on : theme()->toggle_off;
		ui_circle(c, x0, cy, r, track);
		ui_circle(c, x1, cy, r, track);
		ui_rect(c, (int)x0, (int)(cy - r), (int)(x1 - x0), (int)(2 * r), track);
		ui_circle(c, on ? x1 : x0, cy, r - 3.0f, UI_RGBA(0xF4, 0xF4, 0xF4, 0xFF));
	} else if (value) {
		char buf[96];
		snprintf(buf, sizeof(buf), "< %s >", value);
		ui_text(c, px + VALUE_X, ty, col, 1.0f, buf);
	}
}

static void thick_line(UiCanvas *c, float x0, float y0, float x1, float y1, uint32_t col)
{
	ui_line(c, x0, y0, x1, y1, 2.4f, col);
}

// Draw a button symbol starting at (x, cy), vertically centered on cy, and return the width used. L, R, Select, Start, None as text
static int draw_key(UiCanvas *c, int x, int cy, int ty, int key, uint32_t text_col)
{
	const float r = 9.0f;
	const float cx = x + r + 1;
	switch (key) {
	case REI_KEY_CROSS: {
		const uint32_t col = UI_RGBA(0x7C, 0xB4, 0xF0, 0xFF);
		thick_line(c, cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, col);
		thick_line(c, cx - r * 0.8f, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.8f, col);
		return (int)(r * 2 + 2);
	}
	case REI_KEY_CIRCLE:
		ui_circle(c, cx, cy, r, UI_RGBA(0xFF, 0x6E, 0x6E, 0xFF));
		ui_circle(c, cx, cy, r - 2.5f, COL_PANEL);
		return (int)(r * 2 + 2);
	case REI_KEY_SQUARE: {
		const uint32_t col = UI_RGBA(0xF0, 0xA0, 0xD8, 0xFF);
		const float h = r * 0.85f;
		ui_rect(c, (int)(cx - h), (int)(cy - h), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy + h - 2), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy - h), 2, (int)(h * 2), col);
		ui_rect(c, (int)(cx + h - 2), (int)(cy - h), 2, (int)(h * 2), col);
		return (int)(r * 2 + 2);
	}
	case REI_KEY_TRIANGLE: {
		const uint32_t col = UI_RGBA(0x50, 0xE0, 0xC0, 0xFF);
		const float top = cy - r, bot = cy + r * 0.75f, half = r;
		thick_line(c, cx, top, cx - half, bot, col);
		thick_line(c, cx, top, cx + half, bot, col);
		thick_line(c, cx - half, bot, cx + half, bot, col);
		return (int)(r * 2 + 2);
	}
	default: {
		const char *name = rei_key_name(key);
		ui_text(c, x, ty, text_col, 1.0f, name);
		return ui_text_width(1.0f, name);
	}
	}
}

// a combo value: "None" or two key glyphs joined by "+". Returns the width
static int draw_combo(UiCanvas *c, int x, int ty, int mask, uint32_t col)
{
	if (!mask)
		return ui_text(c, x, ty, col, 1.0f, "None");
	const int x0 = x;
	int first = 1;
	for (int k = 0; k < REI_COMBO_SRC; k++) {
		const int key = s_combo_order[k];
		if (!(mask & (1 << key)))
			continue;
		if (!first)
			x += ui_text(c, x + 4, ty, col, 1.0f, "+") + 10;
		x += draw_key(c, x, ty - 7, ty, key, col);
		first = 0;
	}
	return x - x0;
}
static int combo_width(int mask) // (draw_combo's width without drawing)
{
	int w = 0, n = 0;
	for (int k = 0; k < REI_KEY_COUNT; k++)
		if (mask & (1 << k)) {
			w += k <= REI_KEY_TRIANGLE ? 20 : ui_text_width(1.0f, rei_key_name(k));
			n++;
		}
	return w + (n > 1 ? ui_text_width(1.0f, "+") + 10 : 0);
}

// clash: the key also does something else in this game (shown in the warning colour with "!")
static void item_key_w(UiCanvas *c, int px, int ty, int i, int sel, const char *name, int key, int clash)
{
	const uint32_t col = clash ? COL_WARN : (i == sel) ? COL_SEL : COL_TEXT;
	item(c, px, ty, i, sel, name, NULL);
	int x = px + VALUE_X;
	ui_text(c, x, ty, col, 1.0f, "<");
	x += ui_text_width(1.0f, "< ");
	x += draw_key(c, x, ty - 7, ty, key, col);
	x += 6 + ui_text(c, x + 6, ty, col, 1.0f, ">");
	if (clash)
		ui_text(c, x + 8, ty, COL_WARN, 1.0f, "! duplicate");
}

static void item_key(UiCanvas *c, int px, int ty, int i, int sel, const char *name, int key)
{
	item_key_w(c, px, ty, i, sel, name, key, 0);
}

// one line of text (size 0.8): symbol chars (0x10-0x13) drawn as button icons
static void text_keys(UiCanvas *c, int x, int ty, uint32_t col, const char *s)
{
	char run[128];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char ch = (unsigned char)*p;
		if ((ch >= 0x10 && ch <= 0x13) || !ch || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			if (k) {
				ui_text(c, x, ty, col, 0.8f, run);
				x += ui_text_width(0.8f, run);
			}
			k = 0;
			if (!ch)
				break;
			if (ch >= 0x10 && ch <= 0x13) {
				static const int keys[4] = { REI_KEY_TRIANGLE, REI_KEY_CIRCLE, REI_KEY_CROSS, REI_KEY_SQUARE };
				x += draw_key(c, x, ty - 6, ty, keys[ch - 0x10], col) + 2;
				continue;
			}
		}
		run[k++] = (char)ch;
	}
}

// Draw the menu panel to the canvas (caller uploads). overlay = over the game list (the dimmed list kept in s_under)
static int s_paint_x0, s_paint_y0, s_paint_x1, s_paint_y1;
static float s_mscr, s_mtgt; // menu rows scroll (px): drawn, target
static const uint32_t *s_under; // game list options: the dimmed list under the window (NULL: just drawn in full)
static int menu_anim(void) { return s_mscr != s_mtgt || s_help_scroll; } // (the menu is still moving: draw again)
static void tedit_paint(UiCanvas *c);
static void menu_paint(UiCanvas *c, int overlay)
{
	if (s_page == PAGE_TEDIT) {
		tedit_paint(c);
		return;
	}
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR;
	char foot_buf[128];
	const int n = page_items();
	// group gaps on the main page (rows y move down by the gaps above them)
	int gap_at[M_COUNT + 1] = { 0 };
	if (s_page == PAGE_MAIN && !s_list_mode)
		for (int i = 1; i <= n && i <= M_COUNT; i++) {
			const int k = i < n ? main_kind(i) : -1;
			gap_at[i] = gap_at[i - 1] + ((k == M_STATES || k == M_DISPLAY || k == M_ABOUT) ? GROUP_GAP : 0);
		}
	// rows need not fit. The row area is at most the screen minus a margin; taller pages scroll smoothly (the selected
	// row kept a row away from the edges), fading at a cut edge, with a scroll bar
	const int lh = s_page == PAGE_ABOUT ? ABOUT_LH : LINE_H;
	const int gmax = s_page == PAGE_MAIN && !s_list_mode && n > 0 ? gap_at[n - 1] : 0;
	const int content = n * lh + gmax, maxv = SCREEN_H - 32 - TITLE_H - FOOT_H;
	const int view = content < maxv ? content : maxv;
	const int h = TITLE_H + view + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	{
		static int last_page = -1, last_n = -1;
		const float maxs = (float)(content - view);
		const int sy = s_sel * lh + (s_page == PAGE_MAIN && !s_list_mode && s_sel < n ? gap_at[s_sel] : 0);
		if (s_page == PAGE_ABOUT)
			s_mtgt = 0;
		else {
			if (sy - lh < s_mtgt)
				s_mtgt = (float)(sy - lh);
			if (sy + 2 * lh > s_mtgt + view)
				s_mtgt = (float)(sy + 2 * lh - view);
		}
		if (s_mtgt > maxs) s_mtgt = maxs;
		if (s_mtgt < 0) s_mtgt = 0;
		if (s_page != last_page || n != last_n)
			s_mscr = s_mtgt; // another page: no animation
		last_page = s_page;
		last_n = n;
		s_mscr += (s_mtgt - s_mscr) * 0.3f;
		if (fabsf(s_mtgt - s_mscr) < 0.5f)
			s_mscr = s_mtgt;
	}
	const int q0 = s_paint_x0, q1 = s_paint_y0, q2 = s_paint_x1, q3 = s_paint_y1; // what the last paint covered
	s_paint_x0 = px - 8; // area this paint covers (panel and shadow; the choice window adds its own below)
	s_paint_y0 = py - 8;
	s_paint_x1 = px + PANEL_W + 14;
	s_paint_y1 = py + h + 16;
	if (overlay) { // over the game list: it is already dimmed there
		s_drawn_h = 0;
		if (s_under) {
			// the last paint's area outside this window, from the kept list (the opaque window covers its inside;
			// its rounded corners come back too)
			const int ix0 = px + 8, iy0 = py + 8, ix1 = px + PANEL_W - 8, iy1 = py + h - 8;
			if (q1 < iy0) ui_blit_rect(c, s_under, q0, q1, q2 - q0, iy0 - q1);
			if (q3 > iy1) ui_blit_rect(c, s_under, q0, iy1, q2 - q0, q3 - iy1);
			const int m0 = q1 > iy0 ? q1 : iy0, m1 = q3 < iy1 ? q3 : iy1;
			if (m1 > m0) {
				if (q0 < ix0) ui_blit_rect(c, s_under, q0, m0, ix0 - q0, m1 - m0);
				if (q2 > ix1) ui_blit_rect(c, s_under, ix1, m0, q2 - ix1, m1 - m0);
			}
		}
	} else if (s_drawn_h == h) {
		// if the panel size is unchanged, redraw only the panel (do not recompose and copy the whole dimmed screen every
		// time), with the shadow and the choice window of the last paint: they are outside the panel
		const int x0 = q0 < s_paint_x0 ? q0 : s_paint_x0, y0 = q1 < s_paint_y0 ? q1 : s_paint_y0;
		const int x1 = q2 > s_paint_x1 ? q2 : s_paint_x1, y1 = q3 > s_paint_y1 ? q3 : s_paint_y1;
		ui_fill(c, x0, y0, x1 - x0, y1 - y0, COL_BG);
	} else {
		ui_fill(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG); // (set, not blended: a blend reads the whole screen, ~10 ms)
		s_drawn_h = h;
	}
	window_box(c, px, py, PANEL_W, h, COL_EDGE, COL_PANEL); // rounded panel, outline and shadow

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY:
		title = "DISPLAY";
		if (s_list_mode)
			foot = "Left/Right: change   (global settings)";
		break;
	case PAGE_TURBO:
		title = s_kp ? "2P TURBO" : "1P TURBO";
		foot = "Hold the turbo key: the turbo button repeats";
		break;
	case PAGE_TOUCH:
		title = "TOUCH CONTROLS";
		foot = s_reset_armed ? "Press again to clear every touch button" : "Touch buttons on the screen press the keys you give them";
		break;
	case PAGE_PORT: {
		// connected controllers
		int m = snprintf(foot_buf, sizeof(foot_buf), "Connected: %s", re_pad_port_name(0));
		for (int i = 1; i < RE_PAD_PORTS && m < (int)sizeof(foot_buf); i++)
			if (re_pad_connected(i))
				m += snprintf(foot_buf + m, sizeof(foot_buf) - m, ", %s (%d)", re_pad_port_name(i), i);
		{ // with the device whose controls these are
			static char tb[48];
			snprintf(tb, sizeof(tb), "%dP CONTROLLER  (%s)", s_kp + 1, port_none() ? "no device" :
				re_pad_profile_name(re_pad_player_profile(s_kp)));
			title = tb;
		}
		foot = foot_buf;
		break;
	}
	case PAGE_CONTROLS:
		title = "CONTROLS";
		foot = s_list_mode ? "Each player: its device, buttons, stick, turbo, combo keys   (global settings)"
			: "Each player: its device, buttons, stick, turbo, combo keys";
		break;
	case PAGE_COMBO:
		title = s_kp ? "2P COMBO KEYS" : "1P COMBO KEYS";
		snprintf(foot_buf, sizeof(foot_buf), "A key acts as two keys pressed together   Left/Right: change   %s: list   " G_SQU ": none",
			ok_name);
		break;
	case PAGE_DIPS:
		title = "DIP SWITCHES";
		foot = "Some settings take effect after Reset game";
		break;
	case PAGE_CHEATS:
		title = "CHEATS";
		foot = rei_core_has_cheats() ? "Start stage applies when a new game starts" : "Left/Right: change   Cheat files: cheats/<set>.ini";
		break;
	case PAGE_STATES:
		title = "SAVE / LOAD STATE";
		foot = "Quick save: SELECT+L   Quick load: SELECT+R";
		break;
	case PAGE_ABOUT:
		title = "ABOUT";
		snprintf(foot_buf, sizeof(foot_buf), "Up/Down: scroll (%d/%d)   %s: back", s_about_scroll + 1,
			ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE + 1 : 1, back_name);
		break;
	default:
		if (!s_list_mode && rei_settings_has_game())
			foot = "This game uses its own settings (Delete game settings to undo)";
		break;
	}
	if (s_bind) {
		int act;
		const int k = row_kind(s_sel, &act);
		snprintf(foot_buf, sizeof(foot_buf), "%sPress the button for %s   (D-pad or 5 s: cancel)",
			s_bind_all ? "Set all controls: " : "", row_key_name(k, act));
		foot = foot_buf;
	} else if (s_page == PAGE_PORT || s_page == PAGE_TURBO) {
		int act, ck, cf;
		if (row_key(row_kind(s_sel, &act), act, &ck, &cf)) {
			snprintf(foot_buf, sizeof(foot_buf), "%s: press a button to set it   " G_SQU ": none   Left/Right: change", ok_name);
			foot = foot_buf;
		}
	}
	{
		// title: cut with "..." when it overflows the panel (long game titles)
		char tbuf[128];
		snprintf(tbuf, sizeof(tbuf), "%s", s_page == PAGE_MAIN && !s_list_mode ? s_title : title);
		int len = (int)strlen(tbuf);
		while (len > 4 && ui_text_width(1.1f, tbuf) > PANEL_W - 48) {
			len--;
			memcpy(tbuf + len - 3, "...", 4);
		}
		ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, tbuf);
	}

	const int ra = py + TITLE_H, rb = ra + view, scr = (int)floorf(s_mscr + 0.5f);
	ui_clip_y(c, ra, rb);
	s_focus_cy0 = ra;
	s_focus_cy1 = rb;
	for (int i = 0; i < n; i++) {
		const int g = s_page == PAGE_MAIN && !s_list_mode ? gap_at[i] : 0;
		const int ty = ra + i * lh + g - scr + lh / 2 + 7;
		if (ty + lh < ra || ty - lh > rb)
			continue;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = { "Resume game", "Reset game", "Save / Load state  >", "Display  >",
				"Controls  >", "DIP switches  >", "Cheats  >", "Save game settings", "Delete game settings", "About  >",
				"Select game", "Exit", "Back", "Refresh ROM list" };
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[64];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", o_ok ? "Circle" : "Cross");
				name = rbuf;
			}
			if (k == M_CHEATS && !any_cheats())
				name = "Cheats (none for this game)";
			if (k == M_DIPS && rei_core_dip_count() <= 0)
				name = "DIP switches (none)";
			item(c, px, ty, i, s_sel, name, NULL);
			break;
		}
		case PAGE_DISPLAY: {
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Rotate", "Filter", "Scanlines",
				"Scanline strength", "Border (4:3 / 2x)", "Border color", "Theme", "Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_settings.show_fps ? VAL_ON : VAL_OFF; break;
			case D_ASPECT: v = aspect[g_settings.aspect]; break;
			case D_ROTATE: v = rotate[g_settings.rotate]; break;
			case D_FILTER: v = filter[g_settings.filter]; break;
			case D_SCAN: v = scan[g_settings.scan_type]; break;
			case D_SCAN_STR: v = scan_str[g_settings.scan_strength]; break;
			case D_BORDER: v = border[g_settings.border]; break;
			case D_BORDER_COL: v = border_col[g_settings.border_color]; break;
			case D_THEME: v = s_theme_name[g_settings.theme]; break;
			}
			item(c, px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		case PAGE_TURBO:
		case PAGE_TOUCH:
		case PAGE_PORT: {
			int act, rk, rf;
			const int k = row_kind(i, &act);
			const int is_key = row_key(k, act, &rk, &rf);
			const int clash = is_key && key_clash(rk, rf) != NULL;
			if (is_key && s_bind && i == s_sel)
				item(c, px, ty, i, s_sel, row_key_name(k, act), "Press a button...");
			else if (k == CK_ACT)
				item_key_w(c, px, ty, i, s_sel, act_name(act), prt()->keymap[act], clash);
			else if (k == CK_PORT1 || k == CK_PORT2) {
				char pbuf[48], lbuf[24];
				snprintf(lbuf, sizeof(lbuf), "%dP Controller  >", k == CK_PORT2 ? 2 : 1);
				item(c, px, ty, i, s_sel, lbuf, re_pad_player_text(k == CK_PORT2, pbuf, sizeof(pbuf)));
			} else if (k == CK_ENGINE)
				item(c, px, ty, i, s_sel, "Input engine", re_pad_engine_text(g_settings.input_engine));
			else if (k == CK_DEVICE) {
				char pbuf[48];
				item(c, px, ty, i, s_sel, "Device", re_pad_player_text(s_kp, pbuf, sizeof(pbuf)));
			} else if (k == CK_BIND_ALL)
				item(c, px, ty, i, s_sel, "Set all controls", NULL);
			else if (k == CK_RESET_PORT)
				item(c, px, ty, i, s_sel, "Reset to default controls", NULL);
			else if (k == CK_TURBO_KEY)
				item_key_w(c, px, ty, i, s_sel, "Turbo key", prt()->turbo_key, clash);
			else if (k == CK_TURBO)
				item(c, px, ty, i, s_sel, "Turbo  >", NULL);
			else if (k == CK_TOUCH)
				item(c, px, ty, i, s_sel, "Touch controls  >", NULL);
			else if (k == CK_T_ON)
				item(c, px, ty, i, s_sel, "Touch buttons", g_settings.touch_keys ? VAL_ON : VAL_OFF);
			else if (k == CK_T_SHOW)
				item(c, px, ty, i, s_sel, "Show touch buttons", g_settings.touch_show ? VAL_ON : VAL_OFF);
			else if (k == CK_T_EDIT) {
				char eb[48];
				snprintf(eb, sizeof(eb), "Edit touch layout  >   (%d)", rei_touch_count());
				item(c, px, ty, i, s_sel, eb, NULL);
			} else if (k == CK_T_CLEAR)
				item(c, px, ty, i, s_sel, s_reset_armed && i == s_sel ? "Clear touch layout - press again" : "Clear touch layout", NULL);
			else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", prt()->turbo_btn);
				item(c, px, ty, i, s_sel, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(c, px, ty, i, s_sel, "Turbo speed", turbo_speed[prt()->turbo_speed]);
			else if (k == CK_STICK)
				item(c, px, ty, i, s_sel, "Analog stick to d-pad", stick[prt()->stick]);
			else if (k == CK_CONFIRM)
				item_key(c, px, ty, i, s_sel, "Menu confirm button", o_ok ? REI_KEY_CIRCLE : REI_KEY_CROSS);
			else if (k == CK_COMBO)
				item(c, px, ty, i, s_sel, "Combo keys  >", NULL);
			else
				item(c, px, ty, i, s_sel, k == CK_DEFAULTS ? "Restore defaults (every player)" : "Back", NULL);
			break;
		}
		case PAGE_COMBO:
			if (i == COMBO_ROWS - 1) {
				item(c, px, ty, i, s_sel, "Back", NULL);
			} else {
				// row: the key itself (glyph), value: < its combo >
				const int key = s_combo_order[i];
				const uint32_t col = (i == s_sel) ? COL_SEL : COL_TEXT;
				item(c, px, ty, i, s_sel, "", NULL);
				draw_key(c, px + 40, ty - 7, ty, key, i == s_sel ? COL_TEXT : COL_ITEM);
				int x = px + VALUE_X;
				x += ui_text(c, x, ty, col, 1.0f, "< ");
				x += draw_combo(c, x, ty, prt()->combo[key], col);
				ui_text(c, x + 6, ty, col, 1.0f, ">");
			}
			break;
		case PAGE_DIPS:
			if (i < rei_core_dip_count()) {
				const int o = rei_core_dip_get(i);
				item(c, px, ty, i, s_sel, rei_core_dip_name(i), o < 0 ? "?" : rei_core_dip_option_name(i, o));
			} else
				item(c, px, ty, i, s_sel, i == n - 2 ? "Restore defaults" : "Back", NULL);
			break;
		case PAGE_CHEATS: {
			int ci = 0;
			const int k = cheat_kind(i, &ci);
			if (k == HK_STAGE)
				item(c, px, ty, i, s_sel, "Start stage", stages[g_settings.start_stage]);
			else if (k == HK_HEALTH)
				item(c, px, ty, i, s_sel, "Infinite health (1P)", g_settings.cheat_health ? VAL_ON : VAL_OFF);
			else if (k == HK_FILE) {
				const char *v = rei_core_cheat_option_name(ci, rei_core_cheat_get(ci));
				item(c, px, ty, i, s_sel, rei_core_cheat_name(ci), v ? v : "?");
			} else
				item(c, px, ty, i, s_sel, "Back", NULL);
			break;
		}
		case PAGE_STATES:
			if (i == S_SLOT) {
				char sbuf[64];
				const int k = s_state_slot;
				if (s_slot_used[k]) { // file times are UTC
					SceDateTime lt;
					SceRtcTick tick;
					sceRtcGetTick(&s_slot_time[k], &tick);
					sceRtcConvertUtcToLocalTime(&tick, &tick);
					sceRtcSetTick(&lt, &tick);
					snprintf(sbuf, sizeof(sbuf), "%d   used  %02d-%02d %02d:%02d", k, lt.month, lt.day, lt.hour, lt.minute);
				} else
					snprintf(sbuf, sizeof(sbuf), "%d   empty", k);
				item(c, px, ty, i, s_sel, "Slot", sbuf);
			} else {
				static const char *const names[S_COUNT] = { "", "Save state", "Load state", "Back" };
				item(c, px, ty, i, s_sel, names[i], NULL);
			}
			break;
		case PAGE_ABOUT: {
			const int k = s_about_scroll + i;
			if (k < ABOUT_LINES)
				about_line(c, px + 24, ty, s_about[k]);
			break;
		}
		}
	}
	if (content > view) { // fades where rows are cut (towards the panel colour)
		enum { FADE = 14 };
		for (int k = 0; k < FADE; k++) {
			const unsigned al = (unsigned)(0xE0 - k * 0xE0 / FADE);
			if (scr > 0)
				ui_rect(c, px + 2, ra + k, PANEL_W - 22, 1, UI_RGBA(theme()->panel_rgb[0], theme()->panel_rgb[1], theme()->panel_rgb[2], al));
			if (scr < content - view)
				ui_rect(c, px + 2, rb - 1 - k, PANEL_W - 22, 1, UI_RGBA(theme()->panel_rgb[0], theme()->panel_rgb[1], theme()->panel_rgb[2], al));
		}
	}
	ui_clip_y(c, 0, SCREEN_H);
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
	if (s_page != PAGE_ABOUT) { // scroll bar (display only)
		const int tx = px + PANEL_W - 16, t0 = ra + 4, th = view - 8;
		int bh = content > 0 ? th * view / content : th, by = content > 0 ? t0 + (int)(th * s_mscr / content) : t0;
		if (bh < 16) bh = 16;
		if (by + bh > t0 + th) by = t0 + th - bh;
		rrect(c, tx, t0, 4, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		rrect(c, tx, by, 4, bh, 2, COL_THUMB);
	}
	{
		// the footer: the selected row's help (it scrolls left when longer than the line: 1 s,
		// then 60 px/s to its end, 1 s, from the start), else the page's own line
		int act;
		const char *help = s_bind || s_pop || s_vc_ask || s_page == PAGE_ABOUT ? NULL : CTRL_PAGE ?
			row_help(row_kind(s_sel, &act)) : page_row_help();
		s_help_scroll = 0;
		if (help) {
			const int x0 = px + 24, avail = PANEL_W - 48, w = ui_text_width(0.8f, help), key = s_page * 256 + s_sel;
			const uint64_t now = rei_time_us();
			if (key != s_help_key) {
				s_help_key = key;
				s_help_t0 = now;
			}
			int off = 0;
			if (w > avail) {
				const int over = w - avail, hold = 1000, travel = over * 1000 / 60, ms = (int)((now - s_help_t0) / 1000);
				const int tm = ms % (hold + travel + hold);
				off = tm < hold ? 0 : tm < hold + travel ? (tm - hold) * 60 / 1000 : over;
				s_help_scroll = 1;
			}
			ui_text_clip(c, x0 - off, py + h - 18, s_page == PAGE_DIPS ? COL_WARN : COL_DIM, 0.8f, help, x0, x0 + avail);
		} else
			text_keys(c, px + 24, py + h - 18, s_page == PAGE_DIPS ? COL_WARN : COL_DIM, foot);
	}
	if (s_vc_ask) { // the engine's restart? (over the panel)
		s_focus_want = 0; // (the row's blinking outline is a layer above the canvas: off while the window is up)
		const int ww = 560, wh = 170, wx = (SCREEN_W - ww) / 2, wy = (SCREEN_H - wh) / 2;
		ui_rect(c, px, py, PANEL_W, h, UI_RGBA(0x00, 0x00, 0x00, 0x80));
		window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
		static const char *const head[3] = { "VitaControl keeps RePad out", "VitaControl is switched off",
			"VitaControl starts after a restart" };
		static const char *const line1[3] = { "Switch VitaControl off in the plugin list (config.txt, a copy is kept)",
			"Switch VitaControl on in the plugin list (config.txt, a copy is kept)", "It is in the plugin list but not running yet." };
		static const char *const act[3] = { "switch off and restart", "switch on and restart", "restart" };
		const int k = s_vc_ask - 1;
		ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, head[k]);
		ui_text(c, wx + 24, wy + 72, COL_TEXT, 0.8f, line1[k]);
		ui_text(c, wx + 24, wy + 96, COL_TEXT, 0.8f, k < 2 ? "and restart the Vita now?" : "Restart the Vita now?");
		char kb[80];
		snprintf(kb, sizeof(kb), "%s: %s   %s: cancel (Native)", ok_name, act[k], back_name);
		text_keys(c, wx + 24, wy + 140, COL_DIM, kb);
	}
	if (s_pop) { // choice window over the panel
		const char *names[MAX_CHOICES], *ptitle;
		int cur;
		const int nc = item_choices(names, &cur, &ptitle);
		if (nc) {
			// rows of LINE_H; many values (the combo pairs, long DIP lists) get shorter rows so the window fits the screen
			const int plh = 64 + nc * LINE_H + 12 <= SCREEN_H - 24 ? LINE_H : (SCREEN_H - 24 - 76) / nc;
			const int ww = 420, wh = 64 + nc * plh + 12, wx = (SCREEN_W - ww) / 2, wy = (SCREEN_H - wh) / 2;
			if (wy - 8 < s_paint_y0) s_paint_y0 = wy - 8;
			if (wy + wh + 16 > s_paint_y1) s_paint_y1 = wy + wh + 16;
			ui_rect(c, px, py, PANEL_W, h, UI_RGBA(0x00, 0x00, 0x00, 0x80)); // dim the panel under it
			window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
			char tb[64];
			snprintf(tb, sizeof(tb), "%s", ptitle);
			int tl = (int)strlen(tb);
			while (tl > 4 && ui_text_width(1.0f, tb) > ww - 48) { // (long DIP / cheat names)
				tl--;
				memcpy(tb + tl - 3, "...", 4);
			}
			const int tw = ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, tb);
			if (s_choice_kind == CH_COMBO) // "Combo for" the key
			{
				int x = wx + 24 + tw + 8;
				x += draw_key(c, x, wy + 31, wy + 38, s_combo_order[s_sel], COL_TEXT);
				if (s_combo_first >= 0) { // (second step: "= first +")
					x += ui_text(c, x + 10, wy + 38, COL_TEXT, 1.0f, "=") + 20;
					x += draw_key(c, x, wy + 31, wy + 38, s_combo_first, COL_TEXT);
					ui_text(c, x + 6, wy + 38, COL_TEXT, 1.0f, "+");
				}
			}
			ui_rect(c, wx + 20, wy + 50, ww - 40, 1, COL_DIM);
			const float ts = plh < 30 ? 0.8f : 1.0f; // (short rows: smaller text)
			for (int i = 0; i < nc; i++) {
				const int ty = wy + 60 + i * plh + plh / 2 + 7, by = ty - 7 - (plh - 6) / 2;
				const uint32_t col = i == s_pop_sel ? COL_SEL : COL_TEXT;
				if (i == s_pop_sel) {
					rrect(c, wx + 12, by + 3, ww - 24, plh - 6, 6, theme()->shade);
					rrect(c, wx + 12, by, ww - 24, plh - 6, 6, COL_HILITE);
					focus_at(c, wx + 12, by, ww - 24, plh - 6);
				}
				if (s_choice_kind == CH_KEYS) // button icons as in the item, not their names
					draw_key(c, wx + 44, ty - 7, ty, i, col);
				else if (s_choice_kind == CH_COMBO) {
					const int key = combo_pick(i);
					if (key < 0)
						ui_text(c, wx + 44, ty, col, 1.0f, "None");
					else
						draw_key(c, wx + 44, ty - 7, ty, key, col);
				}
				else
					ui_text_clip(c, wx + 44, ty, col, ts, names[i] ? names[i] : "?", wx + 44, wx + ww - 16);
				if (i == cur)
					ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, col);
			}
		}
	}
}

// a touch button's key: the PlayStation symbols as icons, the others as names, centred on (cx, cy)
static void tkey_label(UiCanvas *c, int cx, int cy, int key, uint32_t col)
{
	if (key >= REI_KEY_CROSS && key <= REI_KEY_TRIANGLE)
		draw_key(c, cx - 10, cy, cy + 7, key, col);
	else if (REI_TKEY_IS_COMBO(key))
		draw_combo(c, cx - combo_width(key - REI_TKEY_COMBO) / 2, cy + 7, key - REI_TKEY_COMBO, col);
	else {
		const char *nm = rei_touch_key_name(key);
		ui_text(c, cx - ui_text_width(1.0f, nm) / 2, cy + 7, col, 1.0f, nm);
	}
}
// one touch button: a translucent rounded box with its key (sel: the selected one in the editor)
static void tarea(UiCanvas *c, const ReiTouchArea *a, int sel, int edit)
{
	const int r = a->w < a->h ? a->w / 5 : a->h / 5;
	rrect(c, a->x, a->y, a->w, a->h, r, sel ? COL_HILITE : UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0x38 : 0x24));
	rrect_outline(c, a->x, a->y, a->w, a->h, r, 2, sel ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0xB0 : 0x70));
	tkey_label(c, a->x + a->w / 2, a->y + a->h / 2, a->key, UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0xFF : 0xC0));
	if (edit) { // size handles: every corner, and the middle of every edge of the selected one
		const uint32_t hc = sel ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, 0xB0);
		const int hs = sel ? 12 : 8;
		const int cx[3] = { a->x, a->x + a->w / 2, a->x + a->w }, cy[3] = { a->y, a->y + a->h / 2, a->y + a->h };
		for (int j = 0; j < 3; j++)
			for (int i = 0; i < 3; i++)
				if ((i != 1 || j != 1) && (sel || (i != 1 && j != 1)))
					ui_rect(c, cx[i] - hs / 2, cy[j] - hs / 2, hs, hs, hc);
		if (sel) { // its size (in TE_STEP steps)
			char sz[16];
			snprintf(sz, sizeof(sz), "%dx%d", a->w, a->h);
			ui_text(c, a->x + 4, a->y + a->h - 6, hc, 0.6f, sz);
		}
	}
}
static void tedit_paint(UiCanvas *c)
{
	// the whole screen: the dimmed list (game list options) or the game behind a light shade
	if (s_under)
		ui_blit(c, s_under);
	else
		ui_fill(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0x00, 0x00, 0x00, 0x70));
	s_paint_x0 = s_paint_y0 = 0;
	s_paint_x1 = SCREEN_W;
	s_paint_y1 = SCREEN_H;
	s_drawn_h = 0;
	focus_off();
	for (int i = 0; i < rei_touch_count(); i++)
		tarea(c, rei_touch_area(i), i == te_sel, 1);
	if (te_mode == TE_NEW && te_nw > 2 && te_nh > 2)
		rrect_outline(c, te_nx, te_ny, te_nw, te_nh, 6, 2, te_nw >= TE_MIN && te_nh >= TE_MIN ? COL_SEL : COL_DIM);
	// header with Done, footer with the help
	ui_rect(c, 0, 0, SCREEN_W, 46, UI_RGBA(0x00, 0x00, 0x00, 0xA8));
	char t[64];
	snprintf(t, sizeof(t), "TOUCH LAYOUT   (%d button%s)", rei_touch_count(), rei_touch_count() == 1 ? "" : "s");
	ui_text(c, 20, 31, COL_TEXT, 1.0f, t);
	rrect(c, SCREEN_W - 120, 7, 108, 32, 6, COL_SEL);
	ui_text(c, SCREEN_W - 120 + (108 - ui_text_width(0.9f, "Done")) / 2, 29, COL_PANEL, 0.9f, "Done");
	ui_rect(c, 0, SCREEN_H - 52, SCREEN_W, 52, UI_RGBA(0x00, 0x00, 0x00, 0xA8));
	text_keys(c, 20, SCREEN_H - 30, COL_DIM, "Drag on an empty spot: new button   Tap a button: its key, Duplicate or Delete   Drag it: move   Drag an edge or corner: size (6px steps)");
	text_keys(c, 20, SCREEN_H - 10, COL_DIM, G_SQU ": new button   " G_TRI ": key of the selected one   START: done");
	if (te_pop >= 0) {
		int wx, wy, ww, wh;
		te_pop_rect(&wx, &wy, &ww, &wh);
		const ReiTouchArea *a = rei_touch_area(te_pop);
		window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
		ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, te_cpop == 1 ? "Combo: first key" : te_cpop ? "Combo: second key" : "Button key");
		ui_rect(c, wx + 20, wy + 50, ww - 40, 1, COL_DIM);
		for (int i = 0; te_cpop && i < TE_CPOP_N; i++) {
			const int ty = wy + 60 + i * TE_CPOP_RH + TE_CPOP_RH / 2 + 7, by = ty - 7 - (TE_CPOP_RH - 4) / 2;
			const uint32_t col = i == te_cpop_sel ? COL_SEL : COL_TEXT;
			if (i == te_cpop_sel) {
				rrect(c, wx + 12, by, ww - 24, TE_CPOP_RH - 4, 6, COL_HILITE);
				rrect_outline(c, wx + 12, by, ww - 24, TE_CPOP_RH - 4, 6, 2, COL_FOCUS_STILL);
			}
			const int key = te_ckey(i);
			draw_key(c, wx + 44, ty - 7, ty, key, col);
			if (a && REI_TKEY_IS_COMBO(a->key) && ((a->key - REI_TKEY_COMBO) & (1 << key)) &&
				(te_cpop == 1 || ((a->key - REI_TKEY_COMBO) & (1 << te_cfirst))))
				ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, col);
		}
		for (int i = 0; !te_cpop && i < TE_POP_N; i++) {
			const int ty = wy + 60 + i * TE_POP_RH + TE_POP_RH / 2 + 7, by = ty - 7 - (TE_POP_RH - 6) / 2;
			const uint32_t col = i == te_pop_sel ? COL_SEL : COL_TEXT;
			if (i == te_pop_sel) {
				rrect(c, wx + 12, by, ww - 24, TE_POP_RH - 6, 6, COL_HILITE);
				rrect_outline(c, wx + 12, by, ww - 24, TE_POP_RH - 6, 6, 2, COL_FOCUS_STILL);
			}
			if (i == TE_ROW_DEL)
				ui_text(c, wx + 44, ty, COL_WARN, 1.0f, "Delete");
			else if (i == TE_ROW_DUP)
				ui_text(c, wx + 44, ty, col, 1.0f, "Duplicate");
			else if (i == TE_ROW_COMBO) { // the button's pair, if it has one
				int x = wx + 44;
				ui_text(c, x, ty, col, 1.0f, "Combo  >");
				x += ui_text_width(1.0f, "Combo  >  ");
				if (a && REI_TKEY_IS_COMBO(a->key))
					draw_combo(c, x, ty, a->key - REI_TKEY_COMBO, col);
			}
			else if (i <= REI_KEY_TRIANGLE)
				draw_key(c, wx + 44, ty - 7, ty, i, col);
			else
				ui_text(c, wx + 44, ty, col, 1.0f, rei_touch_key_name(i));
			if (a && (i == a->key || (i == TE_ROW_COMBO && REI_TKEY_IS_COMBO(a->key))))
				ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, col);
		}
	}
}

// ---- touch buttons over the game (slot 4, under the other layers): drawn again only when the layout changes ----
static UiCanvas *s_touch_cv;
static int s_touch_drawn = -1, s_touch_vis;
void rei_touch_overlay_update(int hidden)
{
	const int want = !hidden && g_settings.touch_keys && g_settings.touch_show && rei_touch_count() > 0;
	if (!want) {
		if (s_touch_vis)
			ui_hide(s_touch_cv);
		s_touch_vis = 0;
		return;
	}
	if (s_touch_vis && s_touch_drawn == rei_touch_gen())
		return;
	if (!s_touch_cv)
		s_touch_cv = ui_canvas(4, 0, 0, SCREEN_W, SCREEN_H);
	if (!s_touch_cv)
		return;
	ui_clear(s_touch_cv);
	for (int i = 0; i < rei_touch_count(); i++)
		tarea(s_touch_cv, rei_touch_area(i), 0, 0);
	ui_commit(s_touch_cv);
	s_touch_drawn = rei_touch_gen();
	s_touch_vis = 1;
}

void rei_menu_draw(void)
{
	focus_tick(); // (the blink: the focus layer's opacity only)
	if (s_cv)
		shot_check(s_cv);
	if (menu_anim())
		s_dirty = 1;
	if (!s_dirty || !s_cv)
		return;
	s_dirty = 0;
	focus_begin();
	menu_paint(s_cv, 0);
	focus_end();
	ui_commit(s_cv);
}

void rei_menu_hide(void)
{
	focus_off();
	if (s_cv)
		ui_hide(s_cv);
	s_dirty = 1;
	s_drawn_h = 0;
}

// ---- loading screen: the app's background art, the game name, the stage with its percentage, a spinner and a moving
// bar (same look as ReSys12 / ReST-V) ----
static char s_loading_title[96];
static uint64_t s_loading_us; // time spent drawing (logged at the end)
static uint32_t s_loading_gap_max; // longest time between two shown frames
static int s_loading_gaps, s_loading_gap_at; // frames over 40 ms apart; progress at the longest
static uint32_t s_loading_gap_draw; // draw + present time of that frame
static uint32_t s_loading_ps_max, s_loading_present_max; // longest PS button poll, longest present
static int s_loading_draws;
// The background and the game name are drawn once (kept in s_load_base); each later draw puts back and redraws only the
// band with the stage, the spinner and the bar, so only that band is uploaded.
// The ROM load runs on its own thread and only sets the value (rei_loading_set); the main thread draws every frame
// (rei_loading_frame), so the spinner and the bar move at 60 fps whatever the loader is doing (owner: the screen
// stuttered when the loader drew between its steps)
static uint32_t *s_load_base;
static int s_load_base_ok;
void rei_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
	s_load_base_ok = 0;
}

static volatile int s_loading_target; // set by any thread
static int s_loading_shown;           // drawn value, eases toward the target
void rei_loading_set(int permille)
{
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	__atomic_store_n(&s_loading_target, permille, __ATOMIC_RELEASE);
}
void rei_loading_progress(int permille)
{
	rei_loading_set(permille);
	s_loading_shown = s_loading_target; // (jump: called at the start and the end)
	rei_loading_frame();
}
void rei_loading_frame(void)
{
	const uint64_t t_ps = rei_time_us();
	ps_poll(); // (loading: a single PS press does nothing, a double press still goes home)
	const uint64_t now = rei_time_us();
	if (now - t_ps > s_loading_ps_max)
		s_loading_ps_max = (uint32_t)(now - t_ps);
	const int target = __atomic_load_n(&s_loading_target, __ATOMIC_ACQUIRE);
	if (s_loading_shown < target)
		s_loading_shown += (target - s_loading_shown + 3) / 4; // (the bar glides instead of jumping between steps)
	else
		s_loading_shown = target;
	const int permille = s_loading_shown;
	s_loading_draws++;
	ensure_canvases();
	UiCanvas *c = s_cv;
	// bar at the bottom with a small margin left, right and below; the title above it
	const int m = 36, h = 14, w = SCREEN_W - 2 * m, x = m, y = SCREEN_H - m - h;
	const int sb = y - 50, band = sb - 24; // stage text baseline; first row of the redrawn band
	s_drawn_h = 0;
	if (!s_load_base_ok) {
		ui_clear(c);
		// the app's background art 20% darker (80% brightness), loaded once
		static uint8_t *bg;
		static int bg_w, bg_h, bg_tried;
		if (!bg_tried) {
			bg_tried = 1;
			bg = rei_video_load_bg(&bg_w, &bg_h);
		}
		if (bg)
			ui_image(c, bg, bg_w, bg_h, 205);
		else
			ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
		const char *title = s_loading_title[0] ? s_loading_title : "Loading...";
		const int ttw = ui_text_width(0.9f, title), tx = ttw < w ? (SCREEN_W - ttw) / 2 : x;
		ui_text_clip(c, tx, sb - 18 - 20, COL_DIM, 0.9f, title, x, x + w);
		if (!s_load_base)
			s_load_base = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
		if (s_load_base) {
			memcpy(s_load_base, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
			s_load_base_ok = 1;
		}
	} else
		ui_blit_rect(c, s_load_base, 0, band, SCREEN_W, SCREEN_H - band);
	// capsule bar with a vertical gradient and gloss, stripes moving inside the filled part, a glowing head, a spinner and
	// the stage with its percentage
	const float r = h * 0.5f;
	const int fw = w * permille / 1000;
	ui_rect(c, x - 3, y - 3, w + 6, h + 6, UI_RGBA(0x00, 0x00, 0x00, 0x70)); // shade under the bar
	for (int yy = 0; yy < h; yy++) { // track and fill row by row (rounded ends)
		const float dy = yy + 0.5f - r, in = r - sqrtf(r * r - dy * dy);
		const int t0 = x + (int)in, t1 = x + w - (int)in;
		ui_rect(c, t0, y + yy, t1 - t0, 1, theme()->bar_track);
		if (fw > 2) {
			const int f1 = x + fw - (fw > 2 * r ? (int)in : 0);
			const int g = yy * 255 / (h - 1);
			const uint32_t ca = theme()->bar_top, cb = theme()->bar_bot;
			const uint32_t col = UI_RGBA((int)(ca & 0xFF) + (((int)(cb & 0xFF) - (int)(ca & 0xFF)) * g >> 8),
				(int)((ca >> 8) & 0xFF) + (((int)((cb >> 8) & 0xFF) - (int)((ca >> 8) & 0xFF)) * g >> 8),
				(int)((ca >> 16) & 0xFF) + (((int)((cb >> 16) & 0xFF) - (int)((ca >> 16) & 0xFF)) * g >> 8), 0xFF);
			if (f1 > t0)
				ui_rect(c, t0, y + yy, f1 - t0, 1, col);
			if (yy < h / 3 && f1 > t0) // gloss
				ui_rect(c, t0, y + yy, f1 - t0, 1, UI_RGBA(0xFF, 0xFF, 0xFF, 0x38));
		}
	}
	if (fw > 2 * r) { // stripes drifting to the right
		const int step = 22, off = (int)((now / 25000) % step);
		for (int sx = -h + off; sx < fw; sx += step) {
			const float x0 = (float)(x + sx), x1 = x0 + h - 4;
			if (x0 >= x + r && x1 <= x + fw - r)
				ui_line(c, x0, (float)(y + h - 2), x1, (float)(y + 2), 6.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
		}
	}
	if (fw > 0) { // glowing head
		const float hx = (float)(x + fw), hy = y + r;
		const float pulse = 0.5f + 0.5f * sinf((float)(now % 1000000) * 6.2832f / 1e6f);
		ui_circle(c, hx, hy, r + 7 + 2 * pulse, theme()->glow1);
		ui_circle(c, hx, hy, r + 3, theme()->glow2);
		ui_circle(c, hx, hy, r - 1, theme()->glow3);
	}
	// centred text with room around everything. From the bottom: the bar, 50 px, the stage with its percentage and the
	// loading icon after it (26 px apart, centred as one group), 18 px, the game name (centred, clipped to the bar)
	const char *stg = permille < 950 ? "Loading ROMs" : "Starting";
	char stage[80];
	snprintf(stage, sizeof(stage), "%s   %d%%", stg, permille / 10);
	const float icon_r = 11.0f; // dots on this circle, 3 px each: the icon is 28 px wide
	const int tw = ui_text_width(0.9f, stage), gap = 26, gw = tw + gap + 28, gx0 = (SCREEN_W - gw) / 2;
	ui_text(c, gx0, sb, COL_DIM, 0.9f, stage);
	const float scx = (float)(gx0 + tw + gap + 14), scy = (float)(sb - 5);
	const int ph = (int)((now / 70000) % 12);
	for (int i = 0; i < 12; i++) {
		const float a = i * 6.2832f / 12.0f;
		const int age = (ph - i + 12) % 12, al = 0xFF - age * 0x14;
		ui_circle(c, scx + icon_r * sinf(a), scy - icon_r * cosf(a), 3.0f, (theme()->spinner & 0xFFFFFFu) | ((uint32_t)(al < 0x20 ? 0x20 : al) << 24));
	}
	ui_commit(c);
	rei_video_strips(NULL, 0, 0, 0); // (the game list's rows, left on the screen until this first frame)
	shot_check_as(c, REI_DATA_DIR "shotload");
	if (s_loading_draws % 30 == 0)
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	const uint64_t t_pr = rei_time_us();
	s_loading_us += t_pr - now;
	rei_video_present();
	static uint64_t last_end;
	const uint64_t end = rei_time_us();
	if (end - t_pr > s_loading_present_max)
		s_loading_present_max = (uint32_t)(end - t_pr);
	if (s_loading_draws > 1 && end - last_end > 40000) {
		s_loading_gaps++;
		if (end - last_end > s_loading_gap_max) {
			s_loading_gap_max = (uint32_t)(end - last_end);
			s_loading_gap_at = permille;
			s_loading_gap_draw = (uint32_t)(end - now);
		}
	}
	last_end = end;
}

void rei_loading_done(void)
{
	REI_LOG("loading screen: %d draws, %llu ms drawing, %d gaps over 40 ms, longest %u us at %d (its draw %u us), "
		"PS poll max %u us, present max %u us", s_loading_draws, (unsigned long long)(s_loading_us / 1000), s_loading_gaps,
		(unsigned)s_loading_gap_max, s_loading_gap_at, (unsigned)s_loading_gap_draw, (unsigned)s_loading_ps_max,
		(unsigned)s_loading_present_max);
	s_loading_ps_max = s_loading_present_max = 0;
	s_loading_us = 0;
	s_loading_gap_max = 0;
	s_loading_gaps = 0;
	s_loading_draws = 0;
	s_load_base_ok = 0;
	s_loading_shown = s_loading_target = 0;
	if (s_cv)
		ui_hide(s_cv);
}

// ---- notice (bottom center, 2 s) ----
static uint64_t s_osd_until;
static int s_osd_on;

void rei_osd_show(const char *fmt, ...)
{
	char msg[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	REI_LOG("notice: %s", msg);
	ensure_canvases();
	UiCanvas *c = s_osd_cv;
	const int w = ui_text_width(1.0f, msg) + 40, h = 36;
	const int x = (SCREEN_W - w) / 2;
	ui_clear(c);
	ui_rect(c, x, 0, w, h, UI_RGBA(0x00, 0x00, 0x00, 0xC8));
	ui_rect(c, x, 0, w, 2, COL_SEL);
	ui_text(c, x + 20, 26, COL_TEXT, 1.0f, msg);
	ui_canvas_move(c, 0, SCREEN_H - h - 16);
	ui_commit(c);
	s_osd_until = rei_time_us() + 2000000;
	s_osd_on = 1;
}

void rei_osd_tick(void)
{
	if (s_osd_on && rei_time_us() > s_osd_until) {
		ui_hide(s_osd_cv);
		s_osd_on = 0;
	}
}

// ---- FPS (box in the game picture's top-right corner; log builds with one decimal) ----
static int s_fps_shown;
static uint64_t s_fps_t0;
static int s_fps_frames;
static float s_fps;
static unsigned s_fps_count; // frames since the box was last drawn
// hidden while the menu is open (it covered Done of the touch layout editor); rei_fps_tick draws it again
void rei_fps_hide(void)
{
	if (s_fps_shown && s_fps_cv)
		ui_hide(s_fps_cv);
	s_fps_shown = 0;
}

void rei_fps_tick(void)
{
	// presented frames per second, over the last 0.5 s
	const uint64_t now = rei_time_us();
	if (!s_fps_t0)
		s_fps_t0 = now;
	s_fps_frames++;
	if (now - s_fps_t0 >= 500000) {
		s_fps = s_fps_frames * 1000000.0f / (float)(now - s_fps_t0);
		s_fps_frames = 0;
		s_fps_t0 = now;
	}
	if (!g_settings.show_fps) {
		rei_fps_hide();
		return;
	}
	if (s_fps_shown && ++s_fps_count < 6) // (the box is drawn again every 0.1 s)
		return;
	s_fps_count = 0;
	s_fps_shown = 1;
	ensure_canvases();
	UiCanvas *c = s_fps_cv;
	char buf[48];
#ifdef REI_ENABLE_LOG
	snprintf(buf, sizeof(buf), "%.1f FPS", (double)s_fps);
#else
	snprintf(buf, sizeof(buf), "%d FPS", (int)(s_fps + 0.5f)); // release builds round (59.8 -> 60)
#endif
	int w = ui_text_width(1.0f, buf) + 16;
	if (w > 240)
		w = 240;
	const uint32_t col = s_fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_clear(c);
	ui_rect(c, 240 - w, 0, w, 30, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(c, 240 - w + 8, 22, col, 1.0f, buf);
	// flush in the game picture's top-right corner, no margin (the whole screen before the first frame); held upright (Vertical L / R)
	// the reader's top-right corner, turned with the picture (the menu stays as it is)
	int px, py, pw, ph;
	if (!rei_video_picture_rect(&px, &py, &pw, &ph)) {
		px = py = 0;
		pw = SCREEN_W;
		ph = SCREEN_H;
	}
	int cx, cy;
	if (rei_input_rot == 90) {
		rei_video_layer_rot(3, 1);
		cx = px + pw - 32; // (turned: canvas row 0 is the right edge)
		cy = py + ph - 240;
	} else if (rei_input_rot == 270) {
		rei_video_layer_rot(3, 3);
		cx = px;
		cy = py;
	} else {
		rei_video_layer_rot(3, 0);
		cx = px + pw - 240;
		cy = py;
	}
	cx = cx < 0 ? 0 : cx > SCREEN_W - 32 ? SCREEN_W - 32 : cx;
	cy = cy < 0 ? 0 : cy > SCREEN_H - 30 ? SCREEN_H - 30 : cy;
	ui_canvas_move(c, cx, cy);
	ui_commit(c);
}

// ================================ game list ================================
// Same look and behaviour as the ReSys12 game select: tabs (here: the Irem board), switched with L/R. SELECT = filter
// (Playable: games with ROMs / All: everything, games without ROMs grey, picking one shows a notice / Favorites).
// Square = favorite on/off (star), START or PS = options (Display, Controls, Refresh ROM list; global settings),
// Triangle = About. The cursor starts on the last game; nothing starts on its own
#define COL_GREY     (theme()->grey)
#define COL_GREY_SEL (theme()->grey_sel)
#define MAX_TABS 16
static rei_game_found s_found[REI_GAMES_MAX];
static int s_gl_n;
static int gl_playable(int i) { return s_found[i].present && s_found[i].supported; }
static const char *gl_set(int i) { return rei_core_game_set(s_found[i].game); }
static const char *gl_title(int i) { return rei_core_game_title(s_found[i].game); }
static const char *gl_board(int i) { return rei_core_game_board(s_found[i].game); }
static const char *const s_gl_filter_name[REI_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };

// games visible under the current filter
static int gl_visible(int i)
{
	if (g_settings.list_filter == REI_LIST_ALL)
		return 1;
	if (g_settings.list_filter == REI_LIST_FAVORITES)
		return rei_fav_is(gl_set(i));
	return gl_playable(i);
}

static const char *s_gl_tabs[MAX_TABS];
static int s_gl_ntabs, s_gl_tab, s_gl_nvis;

// Rebuild tabs from games matching the filter and move to the tab/position of want (list index, may be -1). Indices of the current tab go in idx
static int gl_build(int *idx, int want, int *sel)
{
	const char *cur = s_gl_tab < s_gl_ntabs ? s_gl_tabs[s_gl_tab] : NULL;
	s_gl_ntabs = 0;
	s_gl_nvis = 0;
	for (int i = 0; i < s_gl_n; i++) {
		if (!gl_visible(i))
			continue;
		s_gl_nvis++;
		const char *bd = gl_board(i);
		int t = 0;
		while (t < s_gl_ntabs && strcmp(s_gl_tabs[t], bd) != 0)
			t++;
		if (t == s_gl_ntabs && s_gl_ntabs < MAX_TABS)
			s_gl_tabs[s_gl_ntabs++] = bd;
	}
	s_gl_tab = 0;
	const char *tb = want >= 0 && gl_visible(want) ? gl_board(want) : cur;
	for (int t = 0; tb && t < s_gl_ntabs; t++)
		if (!strcmp(s_gl_tabs[t], tb))
			s_gl_tab = t;
	int n = 0;
	*sel = 0;
	for (int i = 0; i < s_gl_n && s_gl_ntabs; i++) {
		if (!gl_visible(i) || strcmp(gl_board(i), s_gl_tabs[s_gl_tab]))
			continue;
		if (i == want)
			*sel = n;
		idx[n++] = i;
	}
	return n;
}

// the listed games and which ROM zips are in the ROM folders (start, and Refresh ROM list: ROMs copied while the app
// runs). Returns the games that can run
static int gl_scan(const char *const *dirs, int ndirs)
{
	int present = 0;
	s_gl_n = rei_games_scan(dirs, ndirs, s_found, REI_GAMES_MAX, &present);
	if (s_gl_n < 0)
		s_gl_n = 0;
	int nplay = 0;
	for (int i = 0; i < s_gl_n; i++)
		nplay += gl_playable(i);
	REI_LOG("games: %d supported, %d with ROM", s_gl_n, nplay);
	return nplay;
}

// one row of the game list (title at x + 48): the selected one with a soft shadow below, a bar and the blinking focus
// outline; favourites get a star; off = how far a long title has scrolled.
// selected: 0 no, 1 the focus (blinks), 2 the focus under an open window (steady outline)
static void gl_row(UiCanvas *c, int px, int w, int ty, int g, int selected, int off)
{
	const int okg = gl_playable(g);
	const uint32_t col = selected ? (okg ? COL_TEXT : COL_GREY_SEL) : (okg ? COL_ITEM : COL_GREY);
	if (selected) {
		const int by = ty - 7 - (GL_LINE - 6) / 2; // bar centred on the text
		rrect(c, px + 12, by + 3, w - 24, GL_LINE - 6, 6, theme()->shade); // shadow (below only)
		rrect(c, px + 13, by + 5, w - 26, GL_LINE - 6, 6, (theme()->shade & 0xFFFFFFu) | 0x20000000u);
		rrect(c, px + 12, by, w - 24, GL_LINE - 6, 6, COL_HILITE);
		if (selected == 2)
			rrect_outline(c, px + 12, by, w - 24, GL_LINE - 6, 6, 2, COL_FOCUS_STILL);
		else if (selected == 1)
			focus_at(c, px + 12, by, w - 24, GL_LINE - 6);
		// (3: the bar only, the caller places the focus)
	}
	if (rei_fav_is(gl_set(g)))
		star(c, (float)(px + 36), (float)(ty - 7), 7.0f, COL_STAR);
	ui_text_clip(c, px + 48 - off, ty, col, 1.0f, gl_title(g), px + 48, px + w - 24);
}

// The list rows while nothing is open over the list: each row is drawn once into a small texture of its own and the
// GPU places the rows (rei_video_strips) as the list scrolls. Redrawing and copying the whole 960x544 canvas every
// frame took ~40 ms (the CPU reaches main memory at ~200 MB/s), so the list scrolled at 25 fps
#define ROW_H 48      // row texture: the bar, its shadow and the text
#define ROW_TY 28     // text baseline in it
#define ROW_CACHE 16  // more than the rows on screen at once
static struct { UiCanvas *cv; int g, var, off, fav, gen; unsigned used; } s_rowc[ROW_CACHE];
static unsigned s_row_frame;
static const ReiTex *row_tex(int g, int var, int off, int w)
{
	const int fav = rei_fav_is(gl_set(g)), gen = s_theme_gen;
	int k = -1;
	for (int i = 0; i < ROW_CACHE && k < 0; i++)
		if (s_rowc[i].cv && s_rowc[i].g == g && s_rowc[i].var == var)
			k = i;
	if (k < 0) { // a free entry, else the one shown longest ago (never one shown this frame)
		for (int i = 0; i < ROW_CACHE; i++) {
			if (!s_rowc[i].cv) {
				k = i;
				break;
			}
			if (s_rowc[i].used != s_row_frame && (k < 0 || s_rowc[i].used < s_rowc[k].used))
				k = i;
		}
		if (k < 0)
			return NULL;
		if (!s_rowc[k].cv)
			s_rowc[k].cv = ui_canvas(-1, 0, 0, w, ROW_H);
		if (!s_rowc[k].cv)
			return NULL;
		s_rowc[k].g = g;
		s_rowc[k].var = var;
		s_rowc[k].gen = -1; // (drawn below)
	}
	if (s_rowc[k].off != off || s_rowc[k].fav != fav || s_rowc[k].gen != gen) {
		ui_clear(s_rowc[k].cv);
		gl_row(s_rowc[k].cv, 0, w, ROW_TY, g, var, off);
		ui_commit(s_rowc[k].cv);
		s_rowc[k].off = off;
		s_rowc[k].fav = fav;
		s_rowc[k].gen = gen;
	}
	s_rowc[k].used = s_row_frame;
	return ui_tex(s_rowc[k].cv);
}

// the list's scroll bar (display only), in the 16 px right of the rows; oy: the canvas's top row on the screen
static void gl_scrollbar(UiCanvas *c, int n, float scr, float rows_f, int la, int lb, int ox, int oy)
{
	if (!n)
		return;
	const int tx = SCREEN_W - 14 - ox, ty0 = la + 8 - oy, th = lb - la - 16;
	const float vis = rows_f < (float)n ? rows_f : (float)n;
	int bh = (int)(th * vis / n), by = ty0 + (int)(th * scr / n);
	if (bh < 16) bh = 16;
	if (by + bh > ty0 + th) by = ty0 + th - bh;
	rrect(c, tx, ty0, 5, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
	rrect(c, tx, by, 5, bh, 2, COL_THUMB);
}

// the list's header: the app name in its colours, the version, the position in the list, the filter with its count
static void gl_header(UiCanvas *c, int n, int sel)
{
	char title[96];
	const int tx = 24 + draw_brand(c, 24, 36, 1.1f);
	snprintf(title, sizeof(title), " (" REI_VERSION ") - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
	ui_text(c, tx, 36, COL_TEXT, 1.1f, title);
	char have[40];
	snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_settings.list_filter], s_gl_nvis);
	ui_text(c, SCREEN_W - 24 - ui_text_width(0.85f, have), 34, COL_DIM, 0.85f, have);
}

// ---- update window over the game list (Update / Close / Skip this version) ----
#define UPD_LINES 400
#define UPD_W 840
#define UPD_H 470
#define UPD_TEXT_W (UPD_W - 64)
#define UPD_VIS 13
static char s_upd_line[UPD_LINES][160];
static int s_upd_n, s_upd_scroll, s_upd_btn;
static void upd_wrap(const char *notes) // the notes as lines of the window's width (word wrap)
{
	s_upd_n = 0;
	const char *p = notes;
	while (*p && s_upd_n < UPD_LINES) {
		const char *e = strchr(p, '\n');
		const size_t len = e ? (size_t)(e - p) : strlen(p);
		char para[1024];
		snprintf(para, sizeof(para), "%.*s", (int)(len < sizeof(para) - 1 ? len : sizeof(para) - 1), p);
		if (!para[0])
			s_upd_line[s_upd_n++][0] = 0;
		for (char *q = para; *q && s_upd_n < UPD_LINES;) {
			char line[160];
			int take = 0, last_sp = -1;
			while (q[take] && take < (int)sizeof(line) - 1) {
				line[take] = q[take];
				line[take + 1] = 0;
				if (q[take] == ' ')
					last_sp = take;
				if (ui_text_width(0.8f, line) > UPD_TEXT_W - (para[0] == '#' ? 0 : 16)) {
					if (last_sp > 0)
						take = last_sp;
					break;
				}
				take++;
			}
			snprintf(s_upd_line[s_upd_n++], sizeof(s_upd_line[0]), "%s%.*s", q == para || para[0] == '#' ? "" : "  ", take, q);
			q += take;
			while (*q == ' ')
				q++;
		}
		p = e ? e + 1 : p + len;
	}
}
static void upd_draw(UiCanvas *c, int o_ok)
{
	const int mx = (SCREEN_W - UPD_W) / 2, my = (SCREEN_H - UPD_H) / 2;
	window_box(c, mx, my, UPD_W, UPD_H, COL_SEL, COL_PANEL);
	char t[96];
	snprintf(t, sizeof(t), "Update available: %s  (this version %s)", rei_update_version(), REI_VERSION);
	ui_text(c, mx + 24, my + 40, COL_TEXT, 1.0f, t);
	for (int i = 0; i < UPD_VIS && s_upd_scroll + i < s_upd_n; i++) {
		const char *l = s_upd_line[s_upd_scroll + i];
		const int head = l[0] == '#';
		ui_text(c, mx + 32, my + 80 + i * 24, head ? COL_SEL : COL_TEXT, head ? 0.9f : 0.8f, l + head);
	}
	if (s_upd_n > UPD_VIS) { // scroll bar
		const int th = UPD_VIS * 24, ty = my + 62;
		int bh = th * UPD_VIS / s_upd_n, by = ty + th * s_upd_scroll / s_upd_n;
		if (bh < 16) bh = 16;
		rrect(c, mx + UPD_W - 18, ty, 5, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		rrect(c, mx + UPD_W - 18, by, 5, bh, 2, COL_THUMB);
	}
	static const char *const names[3] = { "Update", "Close", "Skip this version" };
	int bx = mx + 24;
	for (int i = 0; i < 3; i++) {
		const int bw = ui_text_width(0.9f, names[i]) + 40, by = my + UPD_H - 74;
		rrect(c, bx, by, bw, 36, 6, i == s_upd_btn ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, 0x20));
		ui_text(c, bx + 20, by + 25, i == s_upd_btn ? COL_PANEL : COL_TEXT, 0.9f, names[i]);
		bx += bw + 16;
	}
	text_keys(c, mx + 24, my + UPD_H - 16, COL_DIM,
		o_ok ? "Up/Down: notes   Left/Right: choose   " G_CIR ": OK   " G_CRO ": close"
		     : "Up/Down: notes   Left/Right: choose   " G_CRO ": OK   " G_CIR ": close");
}
static void upd_progress(const char *stage, int pct) // (called while the update downloads)
{
	UiCanvas *c = s_cv;
	const int mw = 760, mh = 140, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
	// the screen under the window is kept and put back first (the window's shadow would get darker with every redraw)
	static uint32_t *under;
	if (!under) {
		under = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
		if (under)
			memcpy(under, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
	} else
		ui_blit(c, under);
	window_box(c, mx, my, mw, mh, COL_SEL, COL_PANEL);
	char t[96];
	if (pct == -2)
		snprintf(t, sizeof(t), "%s", stage);
	else if (pct >= 0)
		snprintf(t, sizeof(t), "%s %s  %d%%", stage, rei_update_version(), pct);
	else
		snprintf(t, sizeof(t), "%s %s ...", stage, rei_update_version());
	ui_text(c, mx + 24, my + 46, COL_TEXT, pct == -2 ? 0.9f : 1.0f, t);
	if (pct != -2)
		rrect(c, mx + 24, my + 74, mw - 48, 14, 6, UI_RGBA(0xFF, 0xFF, 0xFF, 0x20));
	if (pct > 0)
		rrect(c, mx + 24, my + 74, (mw - 48) * (pct > 100 ? 100 : pct) / 100, 14, 6, COL_SEL);
	ui_text(c, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Do not turn the Vita off");
	ui_commit(c);
	rei_video_present();
}

const rei_game_found *rei_game_select(const char *const *dirs, int ndirs, const char *rom_dir, const char *notice)
{
	ensure_canvases();
	UiCanvas *c = s_cv;
	int want = -1;
	const int nplay = gl_scan(dirs, ndirs);
	for (int i = 0; i < s_gl_n; i++)
		if (g_settings.last_game[0] && !strcmp(g_settings.last_game, gl_set(i)))
			want = i;
	for (int i = 0; i < s_gl_n && want < 0; i++) // (no last game: the first game with a ROM)
		if (gl_playable(i))
			want = i;
	int auto_all = 0; // the filter went to All only because no game could run
	if (!nplay && g_settings.list_filter == REI_LIST_PLAYABLE) {
		g_settings.list_filter = REI_LIST_ALL; // (show everything if no game can run)
		auto_all = 1;
	}
	if (want >= 0 && gl_playable(want) && !gl_visible(want))
		g_settings.list_filter = REI_LIST_PLAYABLE; // (the cursor goes to the last game, so it must be on screen)
	static int idx[REI_GAMES_MAX];
	int sel = 0, about = 0, dirty = 1, upd = 0;
	static int upd_offered; // (once per run)
	uint64_t rep = 0;
	float scr = -1, tgt = 0; // list scroll (rows): drawn position easing to the target
	static uint32_t *under; // the dimmed list under the options window
	int under_ok = 0;
	int mq_sel = -1, mq_t = 0, mq_tw = 0; // scrolling title of the selected row (marquee)
	int fast_ok = 0, fast_key[7] = { 0 }, hdr_sel = -1, bar_n = -1, sync_left = 0; // rows as strips
	float bar_scr = -1;
	s_gl_ntabs = s_gl_tab = 0;
	s_list_mode = 0;
	int n = gl_build(idx, want, &sel);
	ui_clear(c); // whole canvas the first time (what loading/menu left there)
	char msg[256] = { 0 };
	if (notice)
		snprintf(msg, sizeof(msg), "%s", notice);
	uint32_t prev = 0xffffffffu; // ignore until released once, so the press that launched the app does not pick right away
	for (;;) {
		SceCtrlData pad;
		re_pad_poll();
		re_pad_read_all(&pad);
		uint32_t b = pad.buttons | rei_testpad_step(); // (log builds: the test input script)
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		if (!s_list_mode && ps_poll() && !upd) { // the PS button opens the options here too (rei_menu_update closes them)
			about = 0;
			msg[0] = 0;
			rei_settings_use_global();
			s_list_mode = 1;
			s_page = PAGE_MAIN;
			s_sel = s_main_sel = 0;
			s_pop = 0;
			s_changed = 0;
			s_prev_buttons = b;
			dirty = 1;
		}
		uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		if (pressed)
			dirty = 1;
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (!upd && !upd_offered && !s_list_mode && !about && !msg[0] && rei_update_ready()) {
			upd = upd_offered = 1; // a newer release: Update / Close / Skip this version
			upd_wrap(rei_update_notes());
			s_upd_scroll = s_upd_btn = 0;
			dirty = 1;
		}
		if (upd) {
			extern int rei_update_auto;
			static int auto_step;
			if (rei_update_auto && auto_step++ == 2) { // test: the window drawn, saved, then Update
				ui_save_ppm(c, REI_DATA_DIR "upd.ppm", UI_RGBA(0, 0, 0, 0xFF));
				s_upd_btn = 0;
				pressed |= ok;
			}
			if ((pressed & SCE_CTRL_UP) && s_upd_scroll > 0) s_upd_scroll--;
			if ((pressed & SCE_CTRL_DOWN) && s_upd_scroll + UPD_VIS < s_upd_n) s_upd_scroll++;
			if (pressed & SCE_CTRL_LEFT) s_upd_btn = wrap(s_upd_btn - 1, 3);
			if (pressed & SCE_CTRL_RIGHT) s_upd_btn = wrap(s_upd_btn + 1, 3);
			if (pressed & back)
				upd = 0;
			else if (pressed & ok) {
				upd = 0;
				if (s_upd_btn == 2)
					rei_update_skip();
				else if (s_upd_btn == 0) {
					focus_off();
					const char *err = rei_update_install(upd_progress); // (returns only on failure)
					REI_LOG("update: failed: %s", err);
					snprintf(msg, sizeof(msg), "Update failed: %s\nThis version stays installed", err);
				}
			}
		} else if (s_list_mode) { // options (Display / Controls / Refresh, global settings. Saved by rei_menu_update on close)
			const rei_menu_result mr = rei_menu_update(b);
			// (the touch layout editor changes without a button press, and touching Done leaves it the same way; only the
			// editor's own changes were taken here, so after Done the editor stayed on the screen until a button)
			if (s_dirty) {
				s_dirty = 0;
				dirty = 1;
			}
			if (mr != REI_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				ui_clear(c);
			}
			if (mr == REI_MENU_REFRESH) { // ROMs copied while the app runs
				char cur[24] = "";
				if (n)
					snprintf(cur, sizeof(cur), "%s", gl_set(idx[sel]));
				const int np = gl_scan(dirs, ndirs);
				if (auto_all && np && g_settings.list_filter == REI_LIST_ALL) {
					g_settings.list_filter = REI_LIST_PLAYABLE; // ROMs now there: back to the games that run
					auto_all = 0;
				}
				int w2 = -1;
				for (int i = 0; cur[0] && i < s_gl_n; i++)
					if (!strcmp(cur, gl_set(i)))
						w2 = i;
				n = gl_build(idx, w2, &sel);
				scr = -1; // (new list: no scroll animation)
				snprintf(msg, sizeof(msg), "ROM list refreshed: %d game%s ready\nin %s", np, np == 1 ? "" : "s", rom_dir);
				dirty = 1;
			}
		} else if (msg[0]) { // notice box: any button closes it
			if (pressed & (ok | back | SCE_CTRL_START | SCE_CTRL_TRIANGLE))
				msg[0] = 0;
		} else if (about) {
			const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
			if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0) s_about_scroll--;
			if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max) s_about_scroll++;
			if (pressed & (ok | back | SCE_CTRL_TRIANGLE | SCE_CTRL_START))
				about = 0;
		} else {
			if (n && (pressed & SCE_CTRL_UP)) sel = wrap(sel - 1, n);
			if (n && (pressed & SCE_CTRL_DOWN)) sel = wrap(sel + 1, n);
			// left/right: one page, L/R: board tab
			if (n && (pressed & SCE_CTRL_LEFT)) sel = sel >= 10 ? sel - 10 : 0;
			if (n && (pressed & SCE_CTRL_RIGHT)) sel = sel + 10 < n ? sel + 10 : n - 1;
			if (s_gl_ntabs > 1 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				int first = -1; // first game of that tab
				for (int i = 0; i < s_gl_n && first < 0; i++)
					if (gl_visible(i) && !strcmp(gl_board(i), s_gl_tabs[s_gl_tab]))
						first = i;
				n = gl_build(idx, first, &sel);
				scr = -1;
			} else if (pressed & SCE_CTRL_SELECT) { // filter: Playable -> All -> Favorites (current game kept if visible)
				g_settings.list_filter = wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT);
				auto_all = 0;
				rei_settings_save();
				n = gl_build(idx, n ? idx[sel] : -1, &sel);
				scr = -1;
			} else if (n && (pressed & SCE_CTRL_SQUARE)) { // favorite on/off (if it drops out of Favorites the cursor moves on)
				const int cur = idx[sel];
				rei_fav_toggle(gl_set(cur));
				int w2 = cur;
				if (g_settings.list_filter == REI_LIST_FAVORITES && !gl_visible(cur))
					w2 = sel + 1 < n ? idx[sel + 1] : (sel > 0 ? idx[sel - 1] : -1);
				n = gl_build(idx, w2, &sel);
			} else if (pressed & SCE_CTRL_TRIANGLE) {
				about = 1;
				s_about_scroll = 0;
			} else if (pressed & SCE_CTRL_START) { // options: edit and save global settings only
				rei_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_pop = 0;
				s_changed = 0;
				s_prev_buttons = b; // (so the START press does not act inside options)
			} else if (n && (pressed & ok)) {
				const int g = idx[sel];
				if (gl_playable(g)) {
					// the list stays on the screen until the loading screen's first frame replaces it (it takes the
					// row strips away with it). Hiding it here showed a black screen while the game's settings were
					// read and the loading screen's background was decoded
					s_drawn_h = 0;
					focus_off();
					return &s_found[g];
				}
				const char *par = rei_core_game_parent(s_found[g].game);
				snprintf(msg, sizeof(msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", gl_set(g),
					par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
			}
		}
		focus_tick();
		if (s_list_mode && menu_anim())
			dirty = 1; // the options window is still scrolling
		{ // another theme: the list under the options is drawn again in the new colours
			static int seen_gen = -1;
			theme();
			if (seen_gen != s_theme_gen) {
				seen_gen = s_theme_gen;
				under_ok = 0;
				dirty = 1;
			}
		}
		if (!s_list_mode && under_ok) { // options closed: the list is drawn again in full
			under_ok = 0;
			dirty = 1;
		}
		// the list scrolls smoothly and fades out at its top and bottom edges; rows need not fit
		const int tab_h = 40, la = TITLE_H + tab_h, lb = SCREEN_H - LIST_FOOT_H, lpad = 18, fade = 20;
		const float rows_f = (float)(lb - la - 2 * lpad) / GL_LINE;
		{
			float maxt = (float)n - rows_f;
			if (maxt < 0)
				maxt = 0;
			if (sel < tgt + 1)
				tgt = (float)sel - 1;
			if (sel > tgt + rows_f - 2)
				tgt = (float)sel - rows_f + 2;
			if (tgt > maxt) tgt = maxt;
			if (tgt < 0) tgt = 0;
			if (scr < 0)
				scr = tgt;
			else if (scr - tgt > 1.5f * rows_f || tgt - scr > 1.5f * rows_f) // long jumps: ease over the last half page only
				scr = scr < tgt ? tgt - 0.5f * rows_f : tgt + 0.5f * rows_f;
			if (scr != tgt && !s_list_mode) {
				scr += (tgt - scr) * 0.3f;
				if (fabsf(tgt - scr) < 0.01f)
					scr = tgt;
				dirty = 1;
			}
		}
		// nothing open over the list: the rows are strips (row_tex) and the canvas is drawn again in full only when the
		// header, tabs or footer change; a cursor move redraws the header line, scrolling only the scroll bar
		const int fast = !about && !msg[0] && !upd && !s_list_mode;
		int full = dirty && !fast;
		if (fast) {
			theme();
			const int key[7] = { s_theme_gen, s_gl_tab, s_gl_ntabs, g_settings.list_filter, s_gl_nvis, n, o_ok };
			if (!fast_ok || memcmp(key, fast_key, sizeof(key))) {
				memcpy(fast_key, key, sizeof(key));
				fast_ok = 1;
				full = 1;
			}
		} else if (dirty) {
			fast_ok = 0;
			rei_video_strips(NULL, 0, 0, 0);
		}
		if (full) {
			dirty = 0;
			const int h = SCREEN_H, w = SCREEN_W;
			const int px = 0, py = 0;
			const uint32_t *bgr = theme_rows(1);
			s_drawn_h = 0;
			focus_begin();
			if (s_list_mode && under_ok) {
				// options over the list: menu_paint puts back the dimmed list kept when they opened, around the window
				s_under = under;
			} else if (about) {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
				for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++)
					about_line(c, px + 24, py + TITLE_H + i * ABOUT_LH + 22, s_about[s_about_scroll + i]);
				text_keys(c, px + 24, py + h - 13, COL_DIM,
					o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			} else {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				const int rw = w - 16; // (room for the scroll bar)
				const int still = s_list_mode || msg[0] || upd; // (no blinking focus under a window)
				s_focus_cy0 = la;
				s_focus_cy1 = lb;
				const int i0 = (int)floorf(scr), i1 = (int)ceilf(scr + rows_f + 2.0f * lpad / GL_LINE);
				for (int i = i0 < 0 ? 0 : i0; !fast && i < n && i <= i1; i++) {
					const int ty = la + lpad + (int)floorf(((float)i - scr) * GL_LINE + 0.5f) + GL_LINE / 2 + 7;
					gl_row(c, px, rw, ty, idx[i], i == sel ? (still ? 2 : 1) : 0, 0);
				}
				s_focus_cy0 = 0;
				s_focus_cy1 = SCREEN_H;
				for (int k = 0; !fast && k < fade; k++) { // fade bands: the background over the rows, opaque at the edge
					const unsigned al = (unsigned)(255 - (k + 1) * 255 / (fade + 1));
					ui_rect(c, 0, la + k, rw, 1, (bgr[la + k] & 0xFFFFFFu) | (al << 24));
					ui_rect(c, 0, lb - 1 - k, rw, 1, (bgr[lb - 1 - k] & 0xFFFFFFu) | (al << 24));
				}
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, la); // rows that scrolled past the edges
				ui_fill_rows(c, bgr, 0, lb, SCREEN_W, SCREEN_H - lb);
				if (!fast)
					gl_scrollbar(c, n, scr, rows_f, la, lb, 0, 0);
				gl_header(c, n, sel);
				{ // tab row: current tab bright with an accent underline, scrolled to keep it visible when the tabs overflow
					int tbx = px + 16, first = 0, total = 0;
					for (int t = 0; t < s_gl_ntabs; t++)
						total += ui_text_width(0.95f, s_gl_tabs[t]) + 28;
					while (total > w - 32 && first < s_gl_tab) {
						total -= ui_text_width(0.95f, s_gl_tabs[first]) + 28;
						first++;
					}
					for (int t = first; t < s_gl_ntabs; t++) {
						const char *tn = s_gl_tabs[t];
						const int tw = ui_text_width(0.95f, tn) + 20;
						if (tbx + tw > px + w - 16)
							break;
						if (t == s_gl_tab)
							rrect(c, tbx + 6, py + TITLE_H + 26, tw - 12, 4, 2, COL_SEL);
						ui_text(c, tbx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_TEXT : COL_DIM, 0.95f, tn);
						tbx += tw + 8;
					}
				}
				if (!n && g_settings.list_filter == REI_LIST_FAVORITES)
					text_keys(c, px + 48, la + 40, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
				else if (!n)
					ui_text(c, px + 48, la + 40, COL_DIM, 1.0f, "No playable games. SELECT: show all, START: Refresh ROM list");
				{ // button help on one line
					char foot[192];
					snprintf(foot, sizeof(foot), "%s: play   " G_SQU ": favorite   " G_TRI ": about   %sLeft/Right: page   SELECT: %s   START: options",
						o_ok ? G_CIR : G_CRO, s_gl_ntabs > 1 ? "L/R: board   " : "",
						s_gl_filter_name[wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT)]);
					text_keys(c, px + 24, py + h - 13, COL_DIM, foot);
				}
				if (msg[0]) { // notice box (over the list)
					const int mw = 760, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
					window_box(c, mx, my, mw, mh, COL_EDGE, COL_PANEL);
					char line[256];
					snprintf(line, sizeof(line), "%s", msg);
					char *nl = strchr(line, '\n');
					if (nl)
						*nl = 0;
					ui_text_clip(c, mx + 24, my + 44, COL_WARN, 1.0f, line, mx + 24, mx + mw - 16);
					if (nl)
						ui_text_clip(c, mx + 24, my + 84, COL_TEXT, 0.9f, nl + 1, mx + 24, mx + mw - 16);
					ui_text(c, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Press any button");
				}
				if (upd)
					upd_draw(c, o_ok);
				s_under = NULL;
				if (s_list_mode) { // options just opened: dim the list once and keep it (the window redraws over it)
					ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
					if (!under)
						under = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
					if (under) {
						memcpy(under, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
						under_ok = 1;
					}
				}
			}
			if (s_list_mode && !about)
				menu_paint(c, 1);
			if (!fast) {
				focus_end();
				ui_commit(c);
			}
		}
		if (fast) {
			dirty = 0;
			const int rw = SCREEN_W - 16;
			const uint32_t *bgr = theme_rows(1);
			int drawn = full;
			if (!full && hdr_sel != sel) { // the position in the header
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, TITLE_H - 8);
				gl_header(c, n, sel);
				drawn = 1;
			}
			static UiCanvas *bar_cv; // the scroll bar: a strip of its own (with the header in the canvas, one copy spanned both)
			if (!bar_cv)
				bar_cv = ui_canvas(-1, 0, 0, SCREEN_W - rw, lb - la);
			if (bar_cv && (full || bar_scr != scr || bar_n != n)) {
				ui_clear(bar_cv);
				gl_scrollbar(bar_cv, n, scr, rows_f, la, lb, rw, la);
				ui_commit(bar_cv);
			}
			hdr_sel = sel;
			bar_scr = scr;
			bar_n = n;
			// the fade bands at the top and bottom of the rows: the background over them, opaque at the edge
			static UiCanvas *fade_cv[2];
			static int fade_gen = -1;
			if (fade_gen != s_theme_gen) {
				for (int e = 0; e < 2; e++) {
					if (!fade_cv[e])
						fade_cv[e] = ui_canvas(-1, 0, 0, rw, fade);
					if (!fade_cv[e])
						continue;
					ui_clear(fade_cv[e]);
					for (int k = 0; k < fade; k++) {
						const unsigned al = (unsigned)(255 - (k + 1) * 255 / (fade + 1));
						const int y = e ? fade - 1 - k : k;
						ui_rect(fade_cv[e], 0, y, rw, 1, (bgr[e ? lb - 1 - k : la + k] & 0xFFFFFFu) | (al << 24));
					}
					ui_commit(fade_cv[e]);
				}
				fade_gen = s_theme_gen;
			}
			// rows on screen, the selected one with its focus (blinking while fully inside the rows area, else a
			// steady outline in the row) and its scrolling title
			s_row_frame++;
			ReiStrip st[REI_NSTRIP];
			int ns = 0;
			focus_begin();
			const int i0 = (int)floorf(scr), i1 = (int)ceilf(scr + rows_f + 2.0f * lpad / GL_LINE);
			for (int i = i0 < 0 ? 0 : i0; i < n && i <= i1 && ns < REI_NSTRIP - 3; i++) {
				const int ty = la + lpad + (int)floorf(((float)i - scr) * GL_LINE + 0.5f) + GL_LINE / 2 + 7;
				int var = 0, off = 0;
				if (i == sel) {
					const int by = ty - 7 - (GL_LINE - 6) / 2;
					const int inside = by - 2 >= la && by + GL_LINE - 6 + 2 <= lb;
					var = inside ? 3 : 2;
					if (inside) {
						s_focus_cy0 = la;
						s_focus_cy1 = lb;
						focus_at(c, 12, by, rw - 24, GL_LINE - 6);
						s_focus_cy0 = 0;
						s_focus_cy1 = SCREEN_H;
					}
					if (mq_sel != idx[i]) {
						mq_sel = idx[i];
						mq_t = 0;
						mq_tw = ui_text_width(1.0f, gl_title(idx[i]));
					}
					const int avail = rw - 72, over = mq_tw - avail, hold = 60;
					if (over > 0) { // 1 s pause -> 60 px/s to the end -> 1 s pause -> from the start
						const int t = mq_t++ % (hold + over + hold);
						off = t < hold ? 0 : t < hold + over ? t - hold : over;
					}
				}
				const ReiTex *t = row_tex(idx[i], var, off, rw);
				if (t)
					st[ns++] = (ReiStrip){ t, 0, ty - ROW_TY };
			}
			if (!n)
				mq_sel = -1;
			for (int e = 0; e < 2; e++)
				if (fade_cv[e] && ui_tex(fade_cv[e]))
					st[ns++] = (ReiStrip){ ui_tex(fade_cv[e]), 0, e ? lb - fade : la };
			if (bar_cv && ui_tex(bar_cv) && ns < REI_NSTRIP)
				st[ns++] = (ReiStrip){ ui_tex(bar_cv), rw, la };
			rei_video_strips(st, ns, la, lb);
			focus_end();
			// after a full redraw the other two textures still need the whole canvas: copied on frames with nothing
			// else to do (the screen is still then), not on the next cursor move
			if (drawn || sync_left > 0) {
				ui_commit(c);
				sync_left = full ? 2 : sync_left > 0 ? sync_left - 1 : 0;
			}
		}
		rei_osd_tick(); // (notices from the options, such as a key used twice, time out here too)
		rei_video_present();
		shot_check(c);
		{ // keep the system from suspending the app after a while without input (as in the game, main.c)
			static unsigned pt;
			if (++pt % 30 == 0)
				sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
		}
#ifdef REI_ENABLE_LOG
		static unsigned beat;
		static uint64_t beat_t;
		static uint64_t last_t;
		static uint32_t gap_max;
		const uint64_t now = rei_time_us();
		if (last_t && now - last_t > gap_max)
			gap_max = (uint32_t)(now - last_t);
		last_t = now;
		if (++beat % 120 == 0) {
			REI_LOG("list: %u frames, %.1f ms/frame, longest %u us", beat, (now - beat_t) / 120000.0, (unsigned)gap_max);
			beat_t = now;
			gap_max = 0;
		}
#endif
	}
}
