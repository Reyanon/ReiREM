// In-game menu, game list, loading screen, notices and the FPS box. Ported from the Vita version (vita/src/frontend/menu.c,
// the ReSys12 UI of the Re series of emulators): same layout, controls, animations and features (themes, choice windows, toggle
// switches, smooth scrolling, blinking focus, star favourites, group gaps, fades and scroll bars), with ReiREM's board tabs,
// DIP switches, cheats, per-game settings and the Turbo page. Sizes are the Vita's halved for the 480x272 screen.
// The PSP redraws a menu screen in full every frame with the GE (ui.c) inside the overlay stage of video_present.
#include "menu.h"

#include <math.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb_image.h> // (implementation in video.c)

#include "log.h"
#include "rei_core.h"
#include "rei_games.h" // REI_GAMES_MAX
#include "rei_version.h" // generated at build time (tools/git_version.cmake)
#include "settings.h"
#include "ui.h"
#include "video.h"
#include "rei_bg_png.c" // the app's background art (vita/sce_sys/livearea/contents/bg.png, psp/CMakeLists.txt)

#define MENU_OPEN_COMBO (PSP_CTRL_SELECT | PSP_CTRL_START)

#define SCREEN_W 480
#define SCREEN_H 272
#define PANEL_W  320
#define LINE_H   22 // menu rows (pages taller than the screen scroll)
#define ABOUT_LH 13 // About text lines
#define GL_LINE  24 // game list rows (the list scrolls)
#define TITLE_H  31
#define FOOT_H   22
#define ROWS_PAD_T 4 // menu rows area: room above the first and below the last row for the focus bar and its glow (the bar sits 3 px high)
#define ROWS_PAD_B 5
#define PANEL_MARGIN 20 // space kept above and below a menu window (fixed, whatever the text size)
#define ABOUT_PW 456      // About in the game menu: its own wider, taller window so every line fits (owner 2026-10-28)
#define ABOUT_MARGIN 6
#define VALUE_X  170 // from panel left edge to the value column
#define TY       3   // text baseline below a row's centre

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

// Colour themes (Display > Theme; same as the Vita). A theme is a gradient (top, bottom), the panel, the accent (selection), dim
// text, row text and the outline colour; the other colours are made from these
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
		toggle_off, grey, grey_sel, bar_track, bar_top, bar_bot, glow1, glow2, glow3, spinner, top, bot;
	uint8_t panel_rgb[3];
} Theme;
static Theme s_th;
static int s_theme_cur = -1;
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
	t->panel = rgb(d->panel, 0xFF);
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
	t->band = mix(d->top, black, 170, 0x50);
	t->sep = mix(d->edge, white, 60, 0x60);
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
	t->top = rgb(d->top, 0xFF);
	t->bot = rgb(d->bot, 0xFF);
	s_theme_cur = k;
}
static const Theme *theme(void)
{
	const int k = g_settings.theme >= 0 && g_settings.theme < REI_THEME_COUNT ? g_settings.theme : REI_THEME_GRAPHITE;
	if (k != s_theme_cur)
		theme_make(k);
	return &s_th;
}
uint32_t rei_menu_accent(void) { return theme()->edge; }
#define COL_BG    (theme()->bg)
#define COL_PANEL (theme()->panel)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_ITEM  (theme()->item) // rows that are not selected
#define COL_DIM   (theme()->dim)
#define COL_SEL   (theme()->sel)
// "iREM" in the theme's accent (Orange: the app art's orange); Graphite, the default theme, keeps the app art's orange-yellow (owner)
#define COL_BRAND (theme(), s_theme_cur == REI_THEME_GRAPHITE ? UI_RGBA(0xCB, 0x7A, 0x1B, 0xFF) : theme()->edge)
#define COL_WARN  UI_RGBA(0xFF, 0xC0, 0x40, 0xFF)
#define COL_HILITE (theme()->hilite) // selected row bar
#define COL_EDGE  (theme()->edge)   // panel outline, separators
#define COL_THUMB (theme()->thumb)  // scroll bar
#define COL_FOCUS_STILL (theme()->focus_still)
#define COL_STAR  UI_RGBA(0xFF, 0xD2, 0x48, 0xFF)
#define COL_GREY     (theme()->grey)
#define COL_GREY_SEL (theme()->grey_sel)

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

// The focus outline blinks (only the topmost one: a focus under an open window keeps a steady outline). The PSP draws every
// frame, so the blink is the outline's opacity
static void focus_at(int x, int y, int w, int h)
{
	const float p = 0.5f + 0.5f * sinf((float)(now_us() % 1000000) * 6.2832f / 1e6f);
	const float a = 0.12f + 0.88f * p;
	const uint32_t br = theme()->focus_bright, gl = theme()->focus_glow;
	ui_rrect_outline(x, y, w, h, 3, 1, (br & 0xFFFFFFu) | ((uint32_t)((br >> 24) * a) << 24));
	ui_rrect_outline(x - 1, y - 1, w + 2, h + 2, 4, 1, (gl & 0xFFFFFFu) | ((uint32_t)((gl >> 24) * a) << 24)); // soft glow outside
}

// the menu / list background: the theme's vertical gradient; the list also gets its header and footer bands
#define LIST_FOOT_H 22
static void theme_bg(int bands)
{
	const Theme *t = theme();
	ui_grad(0, 0, SCREEN_W, SCREEN_H, t->top, t->bot);
	if (bands) {
		ui_rect(0, 0, SCREEN_W, TITLE_H - 4, t->band);
		ui_rect(0, TITLE_H - 4, SCREEN_W, 1, t->sep);
		ui_rect(0, SCREEN_H - LIST_FOOT_H, SCREEN_W, 1, t->sep);
		ui_rect(0, SCREEN_H - LIST_FOOT_H + 1, SCREEN_W, LIST_FOOT_H - 1, (t->band & 0xFFFFFFu) | 0x60000000u);
	}
}

// ================================ menu state ================================
static int s_open;
static int s_changed; // settings changed since the menu opened (saved on close only then)
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_DIPS, PAGE_CHEATS, PAGE_ABOUT, PAGE_TURBO };
static int s_page;
// rows in groups (a gap before M_STATES, M_DISPLAY and M_ABOUT): game, states, settings, about / leave.
// M_REFRESH (read the ROM folders again) only in the game list options
enum { M_RESUME, M_RESET, M_STATES, M_DISPLAY, M_CONTROLS, M_DIPS, M_CHEATS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME,
	M_EXIT, M_BACK, M_REFRESH, M_COUNT };
#define GROUP_GAP 5
// game list options (START in the list): no game. Only Display / Controls / Refresh / Back, global settings only
static int s_list_mode;
static int s_list_refresh; // (list options: Refresh ROM list chosen)
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
enum { D_FPS, D_ASPECT, D_FILTER, D_BORDER, D_BORDER_COL, D_THEME, D_BACK, D_COUNT };
// Controls rows: game buttons (as many as the game uses, 6 in list options), coin, start, Turbo (sub page), confirm button,
// defaults, back. Turbo page: turbo key, turbo button, turbo speed, back
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_TURBO, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
static int ctrl_buttons(void)
{
	if (s_list_mode)
		return REI_TURBO_BUTTONS;
	const int b = rei_core_button_count();
	return b < 2 ? 2 : (b > REI_TURBO_BUTTONS ? REI_TURBO_BUTTONS : b);
}
static int button_act(int i) { return i < 2 ? i : REI_ACT_B3 + (i - 2); }
static const int s_ctrl_tail[] = { CK_TURBO, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
#define CTRL_TAIL ((int)(sizeof(s_ctrl_tail) / sizeof(s_ctrl_tail[0])))
static int ctrl_rows(void) { return ctrl_buttons() + 2 + CTRL_TAIL; }
static int ctrl_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (i < b) {
		*act = button_act(i);
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? REI_ACT_COIN : REI_ACT_START;
		return CK_ACT;
	}
	return i - b - 2 < CTRL_TAIL ? s_ctrl_tail[i - b - 2] : CK_BACK;
}
static int ctrl_row_of(int kind)
{
	for (int j = 0; j < CTRL_TAIL; j++)
		if (s_ctrl_tail[j] == kind)
			return ctrl_buttons() + 2 + j;
	return 0;
}
static int row_kind(int i, int *act)
{
	*act = -1;
	if (s_page == PAGE_TURBO)
		return i < 3 ? CK_TURBO_KEY + i : CK_BACK;
	return ctrl_kind(i, act);
}
#define CTRL_PAGE (s_page == PAGE_CONTROLS || s_page == PAGE_TURBO)
static const char *act_name(int act)
{
	static const char *const n[REI_ACT_COUNT] = { "Button 1", "Button 2", "Coin", "Start", "Button 3", "Button 4", "Button 5",
		"Button 6" };
	return n[act];
}

// ---- duplicate keys: one PSP button that drives two different things in the running game (list options: the 6-button layout)
enum { KF_TURBO = 16 };
typedef struct { int key, func; const char *what; } KeyUse;
static int key_uses(KeyUse *u)
{
	int n = 0;
	for (int i = 0; i < ctrl_buttons(); i++) {
		const int a = button_act(i);
		if (g_settings.keymap[a] < REI_KEY_COUNT)
			u[n++] = (KeyUse){ g_settings.keymap[a], a, act_name(a) };
	}
	u[n++] = (KeyUse){ g_settings.keymap[REI_ACT_COIN], REI_ACT_COIN, "Coin" };
	u[n++] = (KeyUse){ g_settings.keymap[REI_ACT_START], REI_ACT_START, "Start" };
	if (g_settings.turbo_key < REI_KEY_COUNT)
		u[n++] = (KeyUse){ g_settings.turbo_key, KF_TURBO, "Turbo key" };
	return n;
}
static const char *key_clash(int key, int func)
{
	KeyUse u[16];
	const int n = key >= 0 && key < REI_KEY_COUNT ? key_uses(u) : 0;
	for (int j = 0; j < n; j++)
		if (u[j].key == key && u[j].func != func)
			return u[j].what;
	return NULL;
}
static int row_key(int kind, int act, int *key, int *func)
{
	switch (kind) {
	case CK_ACT: *key = g_settings.keymap[act]; *func = act; return 1;
	case CK_TURBO_KEY: *key = g_settings.turbo_key; *func = KF_TURBO; return 1;
	default: return 0;
	}
}
static int s_quiet;
static void warn_clash(int key, int func)
{
	const char *other = s_quiet ? NULL : key_clash(key, func);
	if (other)
		rei_osd_show("%s is also used for %s", rei_key_name(key), other);
}

// DIP switch page: the groups, Restore defaults, Back. Cheat page: (nbbatman family) two built-in cheats, the user cheat file's
// cheats (cheats/<set>.ini, FBNeo format), Back
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

static int s_sel, s_main_sel, s_reset_armed, s_state_slot, s_about_scroll;
static uint32_t s_prev_buttons;
static int s_keep_place, s_pop, s_pop_sel; // menu closed with SELECT+START (keep its page); choice window open, its row

// About: vertically scrolling text (empty string = blank line, starting with '#' = heading line)
static const char *const s_about[] = {
	"#ReiREM for PSP",
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
	"PSPSDK (pspdev) - toolchain, system libraries",
	"  https://github.com/pspdev/pspsdk",
	"Noto Sans (SIL OFL 1.1) - menu font, as bitmap \"ReiREM UI\"",
	"  https://github.com/notofonts/latin-greek-cyrillic",
	"psp-media-engine-custom-core (mcidclan, MIT)",
	"  github.com/mcidclan/psp-media-engine-custom-core",
	"stb_image (Sean Barrett, public domain) - artwork",
	"  https://github.com/nothings/stb",
	"zlib - ROM zip reading",
	"  https://github.com/madler/zlib",
	"newlib - C library",
	"  https://sourceware.org/newlib",
	"",
	"License texts: LICENSE-FBNeo.txt, psp/font/OFL.txt in the source repository.",
};
#define ABOUT_LINES ((int)(sizeof(s_about) / sizeof(s_about[0])))
#define ABOUT_VISIBLE 14

// save state slots of the running game: used or not, and when (read when the menu opens and after a save)
static int s_slot_used[REI_STATE_SLOTS];
static ScePspDateTime s_slot_time[REI_STATE_SLOTS];
static void states_changed(void)
{
	for (int k = 0; k < REI_STATE_SLOTS; k++) {
		char path[160];
		SceIoStat st;
		rei_state_path(k, path, sizeof(path));
		s_slot_used[k] = sceIoGetstat(path, &st) >= 0 && st.st_size > 0;
		if (s_slot_used[k])
			s_slot_time[k] = st.sce_st_mtime;
	}
}

void rei_state_path(int slot, char *buf, int len)
{
	if (slot == REI_STATE_QUICK)
		snprintf(buf, len, "%s%s_quick.sav", rei_settings_dir(), rei_core_set_name());
	else
		snprintf(buf, len, "%s%s_%d.sav", rei_settings_dir(), rei_core_set_name(), slot);
}

int rei_state_exists(int slot)
{
	char path[160];
	rei_state_path(slot, path, sizeof(path));
	return rei_core_state_exists(path);
}

const char *rei_state_slot_name(int slot)
{
	static char buf[16];
	if (slot == REI_STATE_QUICK)
		return "quick";
	snprintf(buf, sizeof(buf), "slot %d", slot);
	return buf;
}

int rei_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int open = combo && !prev_combo;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		if (!s_keep_place) { // (closed with SELECT+START: opens where it was, sub pages too)
			s_page = PAGE_MAIN;
			s_sel = 0;
		}
		s_keep_place = 0;
		s_pop = 0;
		s_reset_armed = 0;
		s_changed = 0;
		states_changed();
	}
	return open;
}

void rei_menu_set_open(int open)
{
	s_open = open;
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
// back one level: Turbo returns to its row in Controls, other pages to the main page
static void page_back(void)
{
	if (s_page == PAGE_TURBO) {
		go_page(PAGE_CONTROLS);
		s_sel = ctrl_row_of(CK_TURBO);
		return;
	}
	go_page(PAGE_MAIN);
}

#define REPEAT_KEYS (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)
// held directions repeat after 300 ms, then every 60 ms (by the clock, not by loop passes)
static uint32_t key_repeat(uint32_t held, uint32_t pressed, uint64_t *next)
{
	const uint32_t dir = held & REPEAT_KEYS;
	const uint64_t now = now_us();
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
static const char *const aspect[] = { "4:3", "Full screen", "1:1 (320x240)" };
static const char *const filter[] = { "Smooth", "Sharp" };
static const char *const border[] = { "Black", "Artwork", "Color" };
static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
static const char *const stages[REI_STAGE_COUNT] = {
	"1 Seattle", "2 San Francisco", "3 Las Vegas", "4 Texas", "5 Florida", "6 Chicago", "Final New York",
};

static void change(int dir);
// an item with several values opens a window listing them all on the confirm button. The values of the selected item:
// names, count (0 = not such an item), current index
#define MAX_CHOICES 24
enum { CH_TEXT, CH_KEYS }; // how the choice window draws the values: names, button icons
static int s_choice_kind;
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
		case D_FILTER: v = filter; n = REI_FILTER_COUNT; *cur = g_settings.filter; *title = "Filter"; break;
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
			const int with_none = k == CK_TURBO_KEY || (act >= REI_ACT_B3); // buttons 3-6 and the turbo key can also be None
			n = REI_KEY_COUNT + with_none;
			for (int i = 0; i < n; i++)
				out[i] = rei_key_name(i);
			*cur = key;
			s_choice_kind = CH_KEYS;
			*title = k == CK_ACT ? act_name(act) : "Turbo key";
		} else if (k == CK_TURBO_BTN) {
			n = ctrl_buttons();
			for (int i = 0; i < n; i++) {
				snprintf(num[i], sizeof(num[i]), "Button %d", i + 1);
				out[i] = num[i];
			}
			*cur = g_settings.turbo_btn - 1;
			*title = "Turbo button";
		} else if (k == CK_TURBO_SPEED) {
			n = REI_TURBO_SPEED_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = turbo_speed[i];
			*cur = g_settings.turbo_speed;
			*title = "Turbo speed";
		}
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
	s_quiet = 1; // (the steps between pass other values: warn once at the end)
	for (int guard = 0; n && cur != idx && guard < MAX_CHOICES + 2; guard++) {
		change(+1); // the same steps as the right button (wraps, applies)
		n = item_choices(names, &cur, &title);
	}
	s_quiet = 0;
	int act, key, func;
	if (CTRL_PAGE && row_key(row_kind(s_sel, &act), act, &key, &func))
		warn_clash(key, func);
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_settings.show_fps = !g_settings.show_fps; break;
		case D_ASPECT: g_settings.aspect = wrap(g_settings.aspect + dir, REI_ASPECT_COUNT); break;
		case D_FILTER: g_settings.filter = wrap(g_settings.filter + dir, REI_FILTER_COUNT); break;
		case D_BORDER: g_settings.border = wrap(g_settings.border + dir, REI_BORDER_COUNT); break;
		case D_BORDER_COL: g_settings.border_color = wrap(g_settings.border_color + dir, REI_BCOL_COUNT); break;
		case D_THEME: g_settings.theme = wrap(g_settings.theme + dir, REI_THEME_COUNT); break;
		default: return;
		}
		s_changed = 1;
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_ACT && act >= 0) // buttons 3-6 can also be None
			g_settings.keymap[act] = wrap(g_settings.keymap[act] + dir, act >= REI_ACT_B3 ? REI_KEY_COUNT + 1 : REI_KEY_COUNT);
		else if (k == CK_TURBO_KEY)
			g_settings.turbo_key = wrap(g_settings.turbo_key + dir, REI_KEY_COUNT + 1); // + None (off)
		else if (k == CK_TURBO_BTN) // 1..number of game buttons
			g_settings.turbo_btn = wrap(g_settings.turbo_btn - 1 + dir, ctrl_buttons()) + 1;
		else if (k == CK_TURBO_SPEED)
			g_settings.turbo_speed = wrap(g_settings.turbo_speed + dir, REI_TURBO_SPEED_COUNT);
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
				rei_osd_show("No DIP switches for %s", rei_core_set_name());
			break;
		case M_CHEATS:
			if (any_cheats())
				go_page(PAGE_CHEATS);
			else
				rei_osd_show("No cheats: put %s.ini in cheats/", rei_core_set_name());
			break;
		case M_GAME_SAVE: // this game only (the global settings file is not written)
			if (rei_settings_save_game())
				rei_osd_show("Saved settings for %s", rei_core_set_name());
			else
				rei_osd_show("Could not save game settings");
			break;
		case M_GAME_DEL:
			if (rei_settings_delete_game())
				rei_osd_show("Deleted settings for %s", rei_core_set_name());
			else
				rei_osd_show("Could not delete game settings");
			break;
		case M_ABOUT:
			go_page(PAGE_ABOUT);
			s_about_scroll = 0;
			break;
		case M_SELECT_GAME: return REI_MENU_SELECT_GAME;
		case M_REFRESH:
			s_list_refresh = 1;
			return REI_MENU_CLOSE;
		case M_EXIT: return REI_MENU_EXIT;
		case M_BACK: return REI_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_BACK) page_back();
		else if (k == CK_TURBO) go_page(PAGE_TURBO);
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
	static uint64_t rep_next;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_next);
	s_prev_buttons = buttons;
	rei_menu_result r = REI_MENU_STAY;
	const int n = page_items();
	{
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS, back = o_ok ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
		const char *names[MAX_CHOICES], *title;
		int cur;
		const int nc = item_choices(names, &cur, &title);
		if (s_pop) { // choice window: up/down, confirm sets, back closes
			if (!nc)
				s_pop = 0;
			else if (pressed & PSP_CTRL_UP)
				s_pop_sel = wrap(s_pop_sel - 1, nc);
			else if (pressed & PSP_CTRL_DOWN)
				s_pop_sel = wrap(s_pop_sel + 1, nc);
			else if (pressed & ok) {
				set_choice(s_pop_sel);
				s_pop = 0;
				if (s_page != PAGE_CHEATS) // (cheats are not saved)
					s_changed = 1;
			} else if (pressed & (back | PSP_CTRL_START))
				s_pop = 0;
			return REI_MENU_STAY;
		}
		if ((pressed & ok) && nc) {
			s_pop = 1;
			s_pop_sel = cur;
			return REI_MENU_STAY;
		}
	}
	if (s_page == PAGE_ABOUT) { // up/down scroll
		const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
		if ((pressed & PSP_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & PSP_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~REPEAT_KEYS;
	}
	if (pressed & PSP_CTRL_UP) {
		s_sel = wrap(s_sel - 1, n);
		s_reset_armed = 0;
	}
	if (pressed & PSP_CTRL_DOWN) {
		s_sel = wrap(s_sel + 1, n);
		s_reset_armed = 0;
	}
	if (pressed & PSP_CTRL_LEFT)
		change(-1);
	if (pressed & PSP_CTRL_RIGHT)
		change(+1);
	// confirm/back follow the setting (Cross/Circle); the setting at press time is used if it changes during this input
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS, back = o_ok ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = REI_MENU_CLOSE;
		else
			page_back();
	}
	if (pressed & PSP_CTRL_START) {
		r = REI_MENU_CLOSE;
		s_keep_place = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO && !s_list_mode;
	}
	// memory stick write only when the user changed something
	if (r != REI_MENU_STAY && r != REI_MENU_SAVE && s_changed) {
		rei_settings_save();
		s_changed = 0;
	}
	if (r == REI_MENU_SAVE)
		s_slot_used[s_state_slot] = 1; // (the slot times are read again when the menu opens)
	return r;
}

// ================================ drawing ================================
// "Re" white + "iREM" in the brand colour (Re franchise naming rule, as the app art). Returns the width
static int draw_brand(int x, int y, float scale)
{
	int w = ui_text(x, y, COL_TEXT, scale, "Re");
	w += ui_text(x + w, y, COL_BRAND, scale, "iREM");
	return w;
}

// one About line (heading lines start with '#'; the "ReiREM" heading uses the brand colours)
static void about_line(int x, int y, const char *t)
{
	if (!t[0])
		return;
	if (!strncmp(t, "#ReiREM", 7)) {
		const int w = draw_brand(x, y, 0.9f);
		ui_text(x + w, y, COL_TEXT, 0.9f, t + 7);
		return;
	}
	const int head = t[0] == '#';
	ui_text(x, y, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT), head ? 0.9f : 0.85f, t + head);
}

#define VAL_ON "\x01on"   // item value: draw a toggle switch (two-state items only)
#define VAL_OFF "\x01off"
// the selected row: a soft bar with a shadow and a blinking outline (a steady one under the choice window)
static void sel_bar(int x, int by, int w, int h)
{
	ui_rrect(x, by + 2, w, h, 3, theme()->shade);
	ui_rrect(x, by, w, h, 3, COL_HILITE);
	if (s_pop)
		ui_rrect_outline(x, by, w, h, 3, 1, COL_FOCUS_STILL);
	else
		focus_at(x, by, w, h);
}
static void item(int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel)
		sel_bar(px + 6, ty - TY - (LINE_H - 4) / 2 - 3, PANEL_W - 20, LINE_H - 1); // bar on the text, 2 px more room above than below (owner)
	ui_text(px + 20, ty, i == sel ? COL_TEXT : COL_ITEM, 1.0f, name);
	if (value && value[0] == 1) {
		// two-state item as a toggle switch (28x14, anti-aliased): on = accent track, white knob on the right;
		// off = dark track, grey knob on the left (owner 2026-10-28: the small switch was hard to read; no ON/OFF text)
		const int on = value[2] == 'n';
		ui_toggle(px + VALUE_X, (float)ty - 5.0f, on, on ? theme()->toggle_on : theme()->toggle_off,
			on ? UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF) : UI_RGBA(0x90, 0x90, 0x94, 0xFF));
	} else if (value) {
		char buf[96];
		snprintf(buf, sizeof(buf), "< %s >", value);
		ui_text_clip(px + VALUE_X, ty, col, 1.0f, buf, px + VALUE_X, px + PANEL_W - 14);
	}
}

// Draw a button symbol starting at (x, cy), vertically centred on cy; returns the width used. L, R, Select, Start, None as text
static int draw_key(int x, float cy, int ty, int key, uint32_t text_col)
{
	const float r = 3.5f;
	const float cx = (float)x + r + 0.5f;
	switch (key) {
	case REI_KEY_CROSS: {
		const uint32_t col = UI_RGBA(0x7C, 0xB4, 0xF0, 0xFF);
		ui_line(cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, 1.3f, col);
		ui_line(cx - r * 0.8f, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.8f, 1.3f, col);
		return (int)(r * 2 + 1);
	}
	case REI_KEY_CIRCLE:
		ui_circle(cx, cy, r, UI_RGBA(0xFF, 0x6E, 0x6E, 0xFF));
		ui_circle(cx, cy, r - 1.3f, COL_PANEL);
		return (int)(r * 2 + 1);
	case REI_KEY_SQUARE: {
		const uint32_t col = UI_RGBA(0xF0, 0xA0, 0xD8, 0xFF);
		const int h = (int)(r * 0.85f + 0.5f), ix = (int)(cx - h), iy = (int)(cy - h);
		ui_rect(ix, iy, h * 2, 1, col);
		ui_rect(ix, iy + h * 2 - 1, h * 2, 1, col);
		ui_rect(ix, iy, 1, h * 2, col);
		ui_rect(ix + h * 2 - 1, iy, 1, h * 2, col);
		return (int)(r * 2 + 1);
	}
	case REI_KEY_TRIANGLE: {
		const uint32_t col = UI_RGBA(0x50, 0xE0, 0xC0, 0xFF);
		const float top = cy - r, bot = cy + r * 0.75f, half = r;
		ui_line(cx, top, cx - half, bot, 1.2f, col);
		ui_line(cx, top, cx + half, bot, 1.2f, col);
		ui_line(cx - half, bot, cx + half, bot, 1.2f, col);
		return (int)(r * 2 + 1);
	}
	default: {
		const char *name = rei_key_name(key);
		return ui_text(x, ty, text_col, 1.0f, name);
	}
	}
}

// clash: the key also does something else in this game (shown in the warning colour with "!")
static void item_key_w(int px, int ty, int i, int sel, const char *name, int key, int clash)
{
	const uint32_t col = clash ? COL_WARN : (i == sel) ? COL_SEL : COL_TEXT;
	item(px, ty, i, sel, name, NULL);
	int x = px + VALUE_X;
	ui_text(x, ty, col, 1.0f, "<");
	x += ui_text_width(1.0f, "< ");
	x += draw_key(x, (float)ty - 3.0f, ty, key, col);
	x += 3 + ui_text(x + 3, ty, col, 1.0f, ">");
	if (clash)
		ui_text(x + 4, ty, COL_WARN, 0.85f, "! duplicate");
}

// one line of button help (size 0.8: 11 px): symbol chars (0x10-0x13) drawn as button icons
static void text_keys(int x, int ty, uint32_t col, const char *s)
{
	char run[160];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char ch = (unsigned char)*p;
		if ((ch >= 0x10 && ch <= 0x13) || !ch || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			if (k)
				x += ui_text(x, ty, col, 0.8f, run);
			k = 0;
			if (!ch)
				break;
			if (ch >= 0x10 && ch <= 0x13) {
				static const int keys[4] = { REI_KEY_TRIANGLE, REI_KEY_CIRCLE, REI_KEY_CROSS, REI_KEY_SQUARE };
				x += draw_key(x, (float)ty - 3.0f, ty, keys[ch - 0x10], col) + 1;
				continue;
			}
		}
		run[k++] = (char)ch;
	}
}

// the menu panel (in the game over the dimmed last frame, in the list over the dimmed list)
static float s_mscr, s_mtgt; // menu rows scroll (px): drawn, target
static void menu_paint(void)
{
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
	// rows need not fit: the row area is at most the screen minus a margin; taller pages scroll smoothly (the selected row kept a
	// row away from the edges), fading at a cut edge, with a scroll bar
	const int lh = s_page == PAGE_ABOUT ? ABOUT_LH : LINE_H;
	const int pw = s_page == PAGE_ABOUT ? ABOUT_PW : PANEL_W;
	const int gmax = s_page == PAGE_MAIN && !s_list_mode && n > 0 ? gap_at[n - 1] : 0;
	const int content = ROWS_PAD_T + n * lh + gmax + ROWS_PAD_B, maxv = SCREEN_H - 2 * (s_page == PAGE_ABOUT ? ABOUT_MARGIN : PANEL_MARGIN) - TITLE_H - FOOT_H;
	const int view = content < maxv ? content : maxv;
	const int h = TITLE_H + view + FOOT_H;
	const int px = (SCREEN_W - pw) / 2, py = (SCREEN_H - h) / 2;
	{
		static int last_page = -1, last_n = -1;
		const float maxs = (float)(content - view);
		const int sy = ROWS_PAD_T + s_sel * lh + (s_page == PAGE_MAIN && !s_list_mode && s_sel < n ? gap_at[s_sel] : 0);
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
		if (fabsf(s_mtgt - s_mscr) < 0.25f)
			s_mscr = s_mtgt;
	}
	ui_window(px, py, pw, h, COL_EDGE, COL_PANEL); // rounded panel, outline and shadow

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
		title = "TURBO";
		foot = "Hold the turbo key: the turbo button repeats";
		break;
	case PAGE_CONTROLS:
		title = "CONTROLS";
		foot = s_list_mode ? "Left/Right: choose button   (global settings)" : "Left/Right: choose button   Menu: SELECT+START";
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
	{
		// title: cut with "..." when it overflows the panel (long game titles)
		char tbuf[128];
		snprintf(tbuf, sizeof(tbuf), "%s", s_page == PAGE_MAIN && !s_list_mode ? rei_core_game_name() : title);
		int len = (int)strlen(tbuf);
		while (len > 4 && ui_text_width(1.1f, tbuf) > pw - 24) {
			len--;
			memcpy(tbuf + len - 3, "...", 4);
		}
		ui_text(px + 12, py + 20, COL_TEXT, 1.1f, tbuf);
	}

	const int ra = py + TITLE_H, rb = ra + view, scr = (int)floorf(s_mscr + 0.5f);
	ui_clip_y(ra, rb);
	for (int i = 0; i < n; i++) {
		const int g = s_page == PAGE_MAIN && !s_list_mode ? gap_at[i] : 0;
		const int ty = ra + ROWS_PAD_T + i * lh + g - scr + lh / 2 + TY;
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
			item(px, ty, i, s_sel, name, NULL);
			break;
		}
		case PAGE_DISPLAY: {
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Filter", "Border (4:3 / 1:1)", "Border color",
				"Theme", "Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_settings.show_fps ? VAL_ON : VAL_OFF; break;
			case D_ASPECT: v = aspect[g_settings.aspect]; break;
			case D_FILTER: v = filter[g_settings.filter]; break;
			case D_BORDER: v = border[g_settings.border]; break;
			case D_BORDER_COL: v = border_col[g_settings.border_color]; break;
			case D_THEME: v = s_theme_name[g_settings.theme]; break;
			}
			item(px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		case PAGE_TURBO: {
			int act, rk, rf;
			const int k = row_kind(i, &act);
			const int clash = row_key(k, act, &rk, &rf) && key_clash(rk, rf) != NULL;
			if (k == CK_ACT)
				item_key_w(px, ty, i, s_sel, act_name(act), g_settings.keymap[act], clash);
			else if (k == CK_TURBO_KEY)
				item_key_w(px, ty, i, s_sel, "Turbo key", g_settings.turbo_key, clash);
			else if (k == CK_TURBO)
				item(px, ty, i, s_sel, "Turbo  >", NULL);
			else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", g_settings.turbo_btn);
				item(px, ty, i, s_sel, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(px, ty, i, s_sel, "Turbo speed", turbo_speed[g_settings.turbo_speed]);
			else if (k == CK_CONFIRM)
				item_key_w(px, ty, i, s_sel, "Menu confirm button", o_ok ? REI_KEY_CIRCLE : REI_KEY_CROSS, 0);
			else
				item(px, ty, i, s_sel, k == CK_DEFAULTS ? "Restore defaults" : "Back", NULL);
			break;
		}
		case PAGE_DIPS:
			if (i < rei_core_dip_count()) {
				const int o = rei_core_dip_get(i);
				item(px, ty, i, s_sel, rei_core_dip_name(i), o < 0 ? "?" : rei_core_dip_option_name(i, o));
			} else
				item(px, ty, i, s_sel, i == n - 2 ? "Restore defaults" : "Back", NULL);
			break;
		case PAGE_CHEATS: {
			int ci = 0;
			const int k = cheat_kind(i, &ci);
			if (k == HK_STAGE)
				item(px, ty, i, s_sel, "Start stage", stages[g_settings.start_stage]);
			else if (k == HK_HEALTH)
				item(px, ty, i, s_sel, "Infinite health (1P)", g_settings.cheat_health ? VAL_ON : VAL_OFF);
			else if (k == HK_FILE) {
				const char *v = rei_core_cheat_option_name(ci, rei_core_cheat_get(ci));
				item(px, ty, i, s_sel, rei_core_cheat_name(ci), v ? v : "?");
			} else
				item(px, ty, i, s_sel, "Back", NULL);
			break;
		}
		case PAGE_STATES:
			if (i == S_SLOT) {
				char sbuf[64];
				const int k = s_state_slot;
				if (s_slot_used[k]) {
					const ScePspDateTime *t = &s_slot_time[k];
					snprintf(sbuf, sizeof(sbuf), "%d   used  %02d-%02d %02d:%02d", k, t->month, t->day, t->hour, t->minute);
				} else
					snprintf(sbuf, sizeof(sbuf), "%d   empty", k);
				item(px, ty, i, s_sel, "Slot", sbuf);
			} else {
				static const char *const names[S_COUNT] = { "", "Save state", "Load state", "Back" };
				item(px, ty, i, s_sel, names[i], NULL);
			}
			break;
		case PAGE_ABOUT: {
			const int k = s_about_scroll + i;
			if (k < ABOUT_LINES)
				about_line(px + 12, ty, s_about[k]);
			break;
		}
		}
	}
	if (content > view) { // fades where rows are cut (towards the panel colour)
		enum { FADE = 7 };
		const uint8_t *pr = theme()->panel_rgb;
		for (int k = 0; k < FADE; k++) {
			const unsigned al = (unsigned)(0xE0 - k * 0xE0 / FADE);
			if (scr > 0)
				ui_rect(px + 1, ra + k, pw - 11, 1, UI_RGBA(pr[0], pr[1], pr[2], al));
			if (scr < content - view)
				ui_rect(px + 1, rb - 1 - k, pw - 11, 1, UI_RGBA(pr[0], pr[1], pr[2], al));
		}
	}
	ui_clip_y(0, SCREEN_H);
	if (s_page != PAGE_ABOUT) { // scroll bar (display only)
		const int tx = px + pw - 8, t0 = ra + 2, th = view - 4;
		int bh = content > 0 ? th * view / content : th, by = content > 0 ? t0 + (int)(th * s_mscr / content) : t0;
		if (bh < 8) bh = 8;
		if (by + bh > t0 + th) by = t0 + th - bh;
		ui_rrect(tx, t0, 2, th, 1, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		ui_rrect(tx, by, 2, bh, 1, COL_THUMB);
	}
	text_keys(px + 12, py + h - 8, s_page == PAGE_DIPS ? COL_WARN : COL_DIM, foot);
	if (s_pop) { // choice window over the panel
		const char *names[MAX_CHOICES], *ptitle;
		int cur;
		const int nc = item_choices(names, &cur, &ptitle);
		if (nc) {
			// rows of LINE_H; many values (long DIP lists) get shorter rows so the window fits the screen
			const int plh = 28 + nc * LINE_H + 6 <= SCREEN_H - 2 * PANEL_MARGIN ? LINE_H : (SCREEN_H - 2 * PANEL_MARGIN - 34) / nc;
			const int ww = 210, wh = 28 + nc * plh + 6, wx = (SCREEN_W - ww) / 2, wy = (SCREEN_H - wh) / 2;
			ui_rect(px, py, pw, h, UI_RGBA(0x00, 0x00, 0x00, 0x80)); // dim the panel under it
			ui_window(wx, wy, ww, wh, COL_SEL, COL_PANEL);
			char tb[64];
			snprintf(tb, sizeof(tb), "%s", ptitle);
			int tl = (int)strlen(tb);
			while (tl > 4 && ui_text_width(1.0f, tb) > ww - 24) { // (long DIP / cheat names)
				tl--;
				memcpy(tb + tl - 3, "...", 4);
			}
			ui_text(wx + 12, wy + 15, COL_TEXT, 1.0f, tb);
			ui_rect(wx + 10, wy + 20, ww - 20, 1, COL_DIM);
			const float ts = plh < 15 ? 0.8f : 1.0f; // (short rows: smaller text)
			for (int i = 0; i < nc; i++) {
				const int ty = wy + 24 + i * plh + plh / 2 + TY, by = ty - TY - (plh - 4) / 2 - 3;
				const uint32_t col = i == s_pop_sel ? COL_SEL : COL_TEXT;
				if (i == s_pop_sel) {
					ui_rrect(wx + 6, by + 2, ww - 12, plh - 1, 3, theme()->shade);
					ui_rrect(wx + 6, by, ww - 12, plh - 1, 3, COL_HILITE);
					focus_at(wx + 6, by, ww - 12, plh - 1);
				}
				if (s_choice_kind == CH_KEYS) // button icons as in the item, not their names
					draw_key(wx + 22, (float)ty - 3.0f, ty, i, col);
				else
					ui_text_clip(wx + 22, ty, col, ts, names[i] ? names[i] : "?", wx + 22, wx + ww - 8);
				if (i == cur)
					ui_circle((float)(wx + 14), (float)ty - 3.0f, 2.5f, col);
			}
		}
	}
}

// ---- notice (bottom centre, 2 s) and the FPS box ----
static char s_osd[96];
static SceInt64 s_osd_until;
static float s_fps;

void rei_osd_show(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_osd, sizeof(s_osd), fmt, ap);
	va_end(ap);
	REI_LOG("notice: %s\n", s_osd);
	s_osd_until = sceKernelGetSystemTimeWide() + 2000000;
}

void rei_fps_set(float fps)
{
	s_fps = fps;
}

static void osd_paint(void)
{
	if (!s_osd[0] || sceKernelGetSystemTimeWide() >= s_osd_until)
		return;
	const int w = ui_text_width(0.9f, s_osd) + 20, h = 19;
	const int x = (SCREEN_W - w) / 2, y = SCREEN_H - h - 8;
	ui_rect(x, y, w, h, UI_RGBA(0x00, 0x00, 0x00, 0xC8));
	ui_rect(x, y, w, 1, COL_SEL);
	ui_text(x + 10, y + 13, COL_TEXT, 0.9f, s_osd);
}

// FPS box flush in the game picture's top-right corner, no margin (release builds round: 59.8 -> 60)
static void fps_paint(void)
{
	char buf[32];
#ifdef REI_LOG_ENABLED
	snprintf(buf, sizeof(buf), "%.1f FPS", (double)s_fps);
#else
	snprintf(buf, sizeof(buf), "%d FPS", (int)(s_fps + 0.5f));
#endif
	const int w = ui_text_width(1.0f, buf) + 8, h = 13;
	int gx, gy, gw, gh;
	video_game_rect(&gx, &gy, &gw, &gh);
	int x = gx + gw - w;
	if (x < 0) x = 0;
	if (x > SCREEN_W - w) x = SCREEN_W - w;
	const int y = gy < 0 ? 0 : gy;
	const uint32_t col = s_fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_rect(x, y, w, h, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(x + 4, y + 10, col, 1.0f, buf);
}

// ================================ loading screen ================================
// The app's background art 20% darker, the game name, the stage with its percentage, a spinner and a capsule bar with stripes and a
// glowing head (same look as the Vita / ReSys12). The ROM load runs on its own thread and only sets the value (rei_loading_set);
// the main thread draws every vblank (rei_loading_frame), so the spinner and the bar move at 60 fps whatever the loader does.
// The background is decoded into the game offscreen in VRAM (no game draws there yet), not into the heap the loader needs
static char s_loading_title[96];
static int s_loading = -1;            // drawing the loading screen (-1 = no)
static volatile int s_loading_target; // set by any thread
static int s_loading_shown;           // drawn value, eases toward the target
static int s_bg_ok;                   // background in the spare VRAM
static uint64_t s_loading_t0;

static void bg_decode(void)
{
	s_bg_ok = 0;
	int w, h, c;
	unsigned char *img = stbi_load_from_memory(rei_bg_png, size_rei_bg_png, &w, &h, &c, 3);
	if (!img)
		return;
	uint16_t *dst = video_spare565();
	video_sync(); // (the GE may read that area)
	static uint16_t row[SCREEN_W];
	// Scaled to the screen by averaging the source pixels each screen pixel covers (smooth edges on the logo and grid lines — nearest
	// sampling dropped lines and left jagged letters, owner 2026-10-28), 80% brightness (a background, softened by the bar text)
	for (int y = 0; y < SCREEN_H; y++) {
		const int y0 = y * h / SCREEN_H, y1 = (y + 1) * h / SCREEN_H > y0 ? (y + 1) * h / SCREEN_H : y0 + 1;
		for (int x = 0; x < SCREEN_W; x++) {
			const int x0 = x * w / SCREEN_W, x1 = (x + 1) * w / SCREEN_W > x0 ? (x + 1) * w / SCREEN_W : x0 + 1;
			unsigned sr = 0, sg = 0, sb = 0, n = 0;
			for (int sy = y0; sy < y1; sy++) {
				const unsigned char *p = img + ((size_t)sy * w + x0) * 3;
				for (int sx = x0; sx < x1; sx++, p += 3) {
					sr += p[0];
					sg += p[1];
					sb += p[2];
					n++;
				}
			}
			const int r = (int)(sr / n) * 205 >> 8, g = (int)(sg / n) * 205 >> 8, b = (int)(sb / n) * 205 >> 8;
			row[x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
		}
		memcpy(dst + y * 512, row, sizeof(row)); // (VRAM is written uncached, a row at a time)
	}
	stbi_image_free(img);
	s_bg_ok = 1;
}

void rei_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
}

void rei_loading_set(int permille)
{
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	s_loading_target = permille;
}

void rei_loading_progress(int permille)
{
	rei_loading_set(permille);
	s_loading_shown = s_loading_target; // (jump: called at the start and the end)
	rei_loading_frame();
}

void rei_loading_frame(void)
{
	if (s_loading < 0) {
		s_loading = 0;
		s_loading_t0 = now_us();
		bg_decode();
	}
	const int target = s_loading_target;
	if (s_loading_shown < target)
		s_loading_shown += (target - s_loading_shown + 3) / 4; // (the bar glides instead of jumping between steps)
	else
		s_loading_shown = target;
	s_loading = s_loading_shown;
	static int frames;
	if (++frames % 30 == 0)
		scePowerTick(PSP_POWER_TICK_ALL); // (no input for a while while loading: keep the screen on)
	video_present();
}

static void loading_paint(void)
{
	const uint64_t now = now_us();
	const int permille = s_loading;
	if (s_bg_ok)
		ui_image565(video_spare565_tex(), SCREEN_W, SCREEN_H, 512, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFFu);
	else
		ui_rect(0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
	// bar at the bottom with a small margin left, right and below; the title above it
	const int m = 18, h = 7, w = SCREEN_W - 2 * m, x = m, y = SCREEN_H - m - h;
	const int sb = y - 25; // stage text baseline
	const char *title = s_loading_title[0] ? s_loading_title : "Loading...";
	const int ttw = ui_text_width(0.9f, title), tx = ttw < w ? (SCREEN_W - ttw) / 2 : x;
	ui_text_clip(tx, sb - 9 - 10, COL_DIM, 0.9f, title, x, x + w);
	// capsule bar with a vertical gradient and gloss, stripes moving inside the filled part, a glowing head
	const float r = h * 0.5f;
	const int fw = w * permille / 1000;
	ui_rect(x - 2, y - 2, w + 4, h + 4, UI_RGBA(0x00, 0x00, 0x00, 0x70)); // shade under the bar
	ui_rrect(x, y, w, h, 3, theme()->bar_track);
	if (fw > 2) {
		ui_clip_y(y, y + h);
		// fill: gradient top to bottom, rounded on the left (and on the right once past the radius)
		ui_grad(x + 2, y, fw - (fw > 2 * r ? 4 : 2), h, theme()->bar_top, theme()->bar_bot);
		ui_rrect(x, y, fw > 6 ? 6 : fw, h, 3, theme()->bar_top);
		ui_rect(x + 2, y, fw - 2, h / 3, UI_RGBA(0xFF, 0xFF, 0xFF, 0x38)); // gloss
		if (fw > 2 * r) { // stripes drifting to the right
			const int step = 11, off = (int)((now / 25000) % step);
			for (int sx = -h + off; sx < fw; sx += step) {
				const float x0 = (float)(x + sx), x1 = x0 + h - 2;
				if (x0 >= x + r && x1 <= x + fw - r)
					ui_line(x0, (float)(y + h - 1), x1, (float)(y + 1), 3.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
			}
		}
		ui_clip_y(0, SCREEN_H);
	}
	if (fw > 0) { // glowing head
		const float hx = (float)(x + fw), hy = y + r;
		const float pulse = 0.5f + 0.5f * sinf((float)(now % 1000000) * 6.2832f / 1e6f);
		ui_circle(hx, hy, r + 3.5f + pulse, theme()->glow1);
		ui_circle(hx, hy, r + 1.5f, theme()->glow2);
		ui_circle(hx, hy, r - 0.5f, theme()->glow3);
	}
	// the stage with its percentage and the loading icon after it, centred as one group
	const char *stg = permille < 950 ? "Loading ROMs" : "Starting";
	char stage[80];
	snprintf(stage, sizeof(stage), "%s   %d%%", stg, permille / 10);
	const float icon_r = 5.5f;
	const int tw = ui_text_width(0.9f, stage), gap = 13, gw = tw + gap + 14, gx0 = (SCREEN_W - gw) / 2;
	ui_text(gx0, sb, COL_DIM, 0.9f, stage);
	const float scx = (float)(gx0 + tw + gap + 7), scy = (float)(sb - 3);
	const int ph = (int)((now / 70000) % 12);
	for (int i = 0; i < 12; i++) {
		const float a = i * 6.2832f / 12.0f;
		const int age = (ph - i + 12) % 12, al = 0xFF - age * 0x14;
		ui_circle(scx + icon_r * sinf(a), scy - icon_r * cosf(a), 1.5f, (theme()->spinner & 0xFFFFFFu) | ((uint32_t)(al < 0x20 ? 0x20 : al) << 24));
	}
}

void rei_loading_done(void)
{
	if (s_loading < 0)
		return;
	REI_LOG("loading screen: %u ms\n", (unsigned)((now_us() - s_loading_t0) / 1000));
	s_loading = -1;
	s_loading_shown = s_loading_target = 0;
	if (s_bg_ok)
		video_spare_clear(); // (the game draws its first frames there)
	s_bg_ok = 0;
}

// ================================ game list ================================
// Same look and behaviour as the ReSys12 game select: tabs (here: the Irem board), switched with L/R. SELECT = filter
// (Playable: games with ROMs / All: everything, games without ROMs grey, picking one shows a notice / Favorites).
// Square = favourite on/off (star), START = options (Display, Controls, Refresh ROM list; global settings), Triangle = About.
// The cursor starts on the last game; nothing starts on its own
#define MAX_TABS 16
static const rei_game_found *s_found;
static int s_gl_n;
static int gl_playable(int i) { return s_found[i].present && s_found[i].supported; }
static const char *gl_set(int i) { return rei_core_game_set(s_found[i].game); }
static const char *gl_title(int i) { return rei_core_game_title(s_found[i].game); }
static const char *gl_board(int i) { return rei_core_game_board(s_found[i].game); }
static const char *const s_gl_filter_name[REI_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };
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
static int s_gl_idx[REI_GAMES_MAX], s_gl_cnt, s_gl_sel;
// list screen state shared with the overlay
static int s_gl_on, s_gl_about;
static float s_gl_scr = -1, s_gl_tgt;
static uint64_t s_gl_mq_t0; // when the cursor came to this row (long title scroll)
static int s_gl_mq_row = -1;
static char s_gl_msg[256]; // notice box (empty = none)
static const char *s_msg_title, *s_msg_body; // notice screen (NULL = none)

// Rebuild tabs from games matching the filter and move to the tab/position of want (list index, may be -1)
static void gl_build(int want)
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
	s_gl_cnt = 0;
	s_gl_sel = 0;
	for (int i = 0; i < s_gl_n && s_gl_ntabs; i++) {
		if (!gl_visible(i) || strcmp(gl_board(i), s_gl_tabs[s_gl_tab]))
			continue;
		if (i == want)
			s_gl_sel = s_gl_cnt;
		s_gl_idx[s_gl_cnt++] = i;
	}
	s_gl_scr = -1; // (new list: no scroll animation)
}

// one row of the game list (title at x + 24): the selected one with a soft shadow below, a bar and the blinking focus outline;
// favourites get a star; off = how far a long title has scrolled. selected: 0 no, 1 focus (blinks), 2 under a window (steady)
static void gl_row(int px, int w, int ty, int g, int selected, int off)
{
	const int okg = gl_playable(g);
	const uint32_t col = selected ? (okg ? COL_TEXT : COL_GREY_SEL) : (okg ? COL_ITEM : COL_GREY);
	if (selected) {
		const int by = ty - TY - (GL_LINE - 4) / 2 - 3; // bar on the text, 2 px more room above than below (owner)
		ui_rrect(px + 6, by + 2, w - 12, GL_LINE - 1, 3, theme()->shade); // shadow (below only)
		ui_rrect(px + 7, by + 3, w - 13, GL_LINE - 1, 3, (theme()->shade & 0xFFFFFFu) | 0x20000000u);
		ui_rrect(px + 6, by, w - 12, GL_LINE - 1, 3, COL_HILITE);
		if (selected == 2)
			ui_rrect_outline(px + 6, by, w - 12, GL_LINE - 1, 3, 1, COL_FOCUS_STILL);
		else
			focus_at(px + 6, by, w - 12, GL_LINE - 1);
	}
	if (rei_fav_is(gl_set(g)))
		ui_star((float)(px + 15), (float)ty - 5.0f, 5.5f, COL_STAR);
	ui_text_clip(px + 24 - off, ty, col, 1.0f, gl_title(g), px + 24, px + w - 12);
}

static void list_paint(void)
{
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const int w = SCREEN_W, h = SCREEN_H, px = 0, py = 0;
	const int n = s_gl_cnt, sel = s_gl_sel;
	theme_bg(1);
	if (s_gl_about) {
		ui_text(px + 12, py + 20, COL_TEXT, 1.1f, "ABOUT");
		for (int i = 0; i < ABOUT_VISIBLE + 2 && s_about_scroll + i < ABOUT_LINES; i++)
			about_line(px + 12, py + TITLE_H + i * ABOUT_LH + 11, s_about[s_about_scroll + i]);
		text_keys(px + 12, py + h - 7, COL_DIM,
			o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
		return;
	}
	// the list scrolls smoothly and fades out at its top and bottom edges; rows need not fit
	const int tab_h = 20, la = TITLE_H + tab_h, lb = SCREEN_H - LIST_FOOT_H, lpad = 9, fade = 10;
	const float rows_f = (float)(lb - la - 2 * lpad) / GL_LINE;
	{
		float maxt = (float)n - rows_f;
		if (maxt < 0)
			maxt = 0;
		if (sel < s_gl_tgt + 1)
			s_gl_tgt = (float)sel - 1;
		if (sel > s_gl_tgt + rows_f - 2)
			s_gl_tgt = (float)sel - rows_f + 2;
		if (s_gl_tgt > maxt) s_gl_tgt = maxt;
		if (s_gl_tgt < 0) s_gl_tgt = 0;
		if (s_gl_scr < 0)
			s_gl_scr = s_gl_tgt;
		else if (s_gl_scr - s_gl_tgt > 1.5f * rows_f || s_gl_tgt - s_gl_scr > 1.5f * rows_f) // long jumps: ease over the last half page only
			s_gl_scr = s_gl_scr < s_gl_tgt ? s_gl_tgt - 0.5f * rows_f : s_gl_tgt + 0.5f * rows_f;
		if (s_gl_scr != s_gl_tgt && !s_list_mode) {
			s_gl_scr += (s_gl_tgt - s_gl_scr) * 0.3f;
			if (fabsf(s_gl_tgt - s_gl_scr) < 0.01f)
				s_gl_scr = s_gl_tgt;
		}
	}
	const int rw = w - 8; // (room for the scroll bar)
	const int still = s_list_mode || s_gl_msg[0]; // (no blinking focus under a window)
	ui_clip_y(la, lb);
	const int i0 = (int)floorf(s_gl_scr), i1 = (int)ceilf(s_gl_scr + rows_f + 2.0f * lpad / GL_LINE);
	for (int i = i0 < 0 ? 0 : i0; i < n && i <= i1; i++) {
		const int ty = la + lpad + (int)floorf(((float)i - s_gl_scr) * GL_LINE + 0.5f) + GL_LINE / 2 + TY;
		int off = 0;
		if (i == sel && !still) { // titles longer than the row scroll: 1 s pause, 30 px/s to the end, 1 s pause, from the start
			if (s_gl_mq_row != s_gl_idx[i]) {
				s_gl_mq_row = s_gl_idx[i];
				s_gl_mq_t0 = now_us();
			}
			const int avail = rw - 36, over = ui_text_width(1.0f, gl_title(s_gl_idx[i])) - avail;
			if (over > 0) {
				const int hold = 60, cyc = hold + over * 2 + hold;
				const int t = (int)((now_us() - s_gl_mq_t0) / 16667 % (uint64_t)cyc);
				off = t < hold ? 0 : t < hold + over * 2 ? (t - hold) / 2 : over;
			}
		}
		gl_row(px, rw, ty, s_gl_idx[i], i == sel ? (still ? 2 : 1) : 0, off);
	}
	ui_clip_y(0, SCREEN_H);
	{ // fade bands: the background over the rows, opaque at the edge (the gradient's colour at those rows)
		const Theme *t = theme();
		for (int k = 0; k < fade; k++) {
			const unsigned al = (unsigned)(255 - (k + 1) * 255 / (fade + 1));
			const int ya = la + k, yb = lb - 1 - k;
			const uint8_t *tp = s_theme_def[s_theme_cur].top, *bt = s_theme_def[s_theme_cur].bot;
			ui_rect(0, ya, rw, 1, mix(tp, bt, ya * 256 / (SCREEN_H - 1), al));
			ui_rect(0, yb, rw, 1, mix(tp, bt, yb * 256 / (SCREEN_H - 1), al));
		}
		(void)t;
	}
	if (n) { // scroll bar (display only)
		const int tx = SCREEN_W - 7, ty0 = la + 4, th = lb - la - 8;
		const float vis = rows_f < (float)n ? rows_f : (float)n;
		int bh = (int)(th * vis / n), by = ty0 + (int)(th * s_gl_scr / n);
		if (bh < 8) bh = 8;
		if (by + bh > ty0 + th) by = ty0 + th - bh;
		ui_rrect(tx, ty0, 3, th, 1, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		ui_rrect(tx, by, 3, bh, 1, COL_THUMB);
	}
	{ // header: the app name in its colours, the version, the position in the list
		char title[96];
		const int tx = px + 12 + draw_brand(px + 12, py + 19, 1.1f);
		snprintf(title, sizeof(title), " (" REI_VERSION ") - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
		ui_text_clip(tx, py + 19, COL_TEXT, 1.1f, title, tx, w - 70);
	}
	char have[40];
	snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_settings.list_filter], s_gl_nvis);
	ui_text(px + w - 12 - ui_text_width(0.85f, have), py + 18, COL_DIM, 0.85f, have);
	{ // tab row: current tab bright with an accent underline, scrolled to keep it visible when the tabs overflow
		int tbx = px + 8, first = 0, total = 0;
		for (int t = 0; t < s_gl_ntabs; t++)
			total += ui_text_width(0.95f, s_gl_tabs[t]) + 14;
		while (total > w - 16 && first < s_gl_tab) {
			total -= ui_text_width(0.95f, s_gl_tabs[first]) + 14;
			first++;
		}
		for (int t = first; t < s_gl_ntabs; t++) {
			const char *tn = s_gl_tabs[t];
			const int tw = ui_text_width(0.95f, tn) + 10;
			if (tbx + tw > px + w - 8)
				break;
			if (t == s_gl_tab)
				ui_rrect(tbx + 3, py + TITLE_H + 15, tw - 6, 2, 1, COL_SEL);
			ui_text(tbx + 5, py + TITLE_H + 12, t == s_gl_tab ? COL_TEXT : COL_DIM, 0.95f, tn);
			tbx += tw + 4;
		}
	}
	if (!n && g_settings.list_filter == REI_LIST_FAVORITES)
		text_keys(px + 24, la + 20, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
	else if (!n)
		ui_text(px + 24, la + 20, COL_DIM, 0.9f, "No playable games. SELECT: show all, START: Refresh ROM list");
	{ // button help on one line
		char foot[192];
		snprintf(foot, sizeof(foot), "%s: play  " G_SQU ": favorite  " G_TRI ": about  %sSELECT: %s  START: options",
			o_ok ? G_CIR : G_CRO, s_gl_ntabs > 1 ? "L/R: board  " : "",
			s_gl_filter_name[wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT)]);
		text_keys(px + 8, py + h - 7, COL_DIM, foot);
	}
	if (s_gl_msg[0]) { // notice box (over the list)
		const int mw = 380, mh = 75, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
		ui_window(mx, my, mw, mh, COL_EDGE, COL_PANEL);
		char line[256];
		snprintf(line, sizeof(line), "%s", s_gl_msg);
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		ui_text_clip(mx + 12, my + 22, COL_WARN, 0.9f, line, mx + 12, mx + mw - 8);
		if (nl)
			ui_text_clip(mx + 12, my + 42, COL_TEXT, 0.9f, nl + 1, mx + 12, mx + mw - 8);
		ui_text(mx + 12, my + mh - 10, COL_DIM, 0.8f, "Press any button");
	}
	if (s_list_mode) { // options over the dimmed list
		ui_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
		if (!s_gl_about)
			menu_paint();
	}
}

#ifdef REI_LOG_ENABLED
// (log builds) remote key press for device tests: host0:/rei_press holds a hex button mask (PSP_CTRL_*); it is pressed for a few
// frames, then the file is removed (menu and list screenshots without hands on the device)
uint32_t rei_test_press(void)
{
	static int calls, hold;
	static uint32_t mask;
	if (hold > 0) {
		hold--;
		return hold > 2 ? mask : 0; // (3 frames down, 3 up: one press)
	}
	if (++calls % 6)
		return 0;
	SceIoStat st;
	if (sceIoGetstat("host0:/rei_press", &st) < 0)
		return 0;
	char b[16] = { 0 };
	const SceUID fd = sceIoOpen("host0:/rei_press", PSP_O_RDONLY, 0);
	if (fd >= 0) {
		sceIoRead(fd, b, sizeof(b) - 1);
		sceIoClose(fd);
	}
	sceIoRemove("host0:/rei_press");
	mask = (uint32_t)strtoul(b, NULL, 16);
	hold = 6;
	return 0;
}
#endif

static uint32_t read_pad(void)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);
	uint32_t b = pad.Buttons;
#ifdef REI_LOG_ENABLED
	b |= rei_test_press();
#endif
	if (pad.Ly < 128 - 48) b |= PSP_CTRL_UP;
	if (pad.Ly > 128 + 48) b |= PSP_CTRL_DOWN;
	if (pad.Lx < 128 - 48) b |= PSP_CTRL_LEFT;
	if (pad.Lx > 128 + 48) b |= PSP_CTRL_RIGHT;
	return b;
}

static int (*s_gl_poll)(void); // (main: every list frame; nonzero ends the list with REI_LIST_QUIT)
int rei_game_select(const rei_game_found *found, int n, int initial, const char *rom_dir)
{
	s_found = found;
	s_gl_n = n;
	int nplay = 0;
	for (int i = 0; i < n; i++)
		nplay += gl_playable(i);
	static int auto_all; // the filter went to All only because no game could run
	if (!nplay && g_settings.list_filter == REI_LIST_PLAYABLE) {
		g_settings.list_filter = REI_LIST_ALL; // (show everything if no game can run)
		auto_all = 1;
	} else if (nplay && auto_all && g_settings.list_filter == REI_LIST_ALL) {
		g_settings.list_filter = REI_LIST_PLAYABLE; // (ROMs now there: back to the games that run)
		auto_all = 0;
	}
	if (initial >= 0 && initial < n && gl_playable(initial) && !gl_visible(initial))
		g_settings.list_filter = REI_LIST_PLAYABLE; // (the cursor goes to the last game, so it must be on screen)
	s_gl_about = 0;
	if (s_list_refresh) { // (back from Refresh ROM list)
		s_list_refresh = 0;
		snprintf(s_gl_msg, sizeof(s_gl_msg), "ROM list refreshed: %d game%s ready\nin %s", nplay, nplay == 1 ? "" : "s", rom_dir);
	} else
		s_gl_msg[0] = 0;
	s_gl_ntabs = s_gl_tab = 0;
	s_list_mode = 0;
	s_open = 0;
	gl_build(initial);
	s_gl_on = 1;
	uint64_t rep = 0;
	int result = -3;
	uint32_t prev = 0xffffffffu; // ignore until released once, so the press that launched the app (or picked the last game) does not act
	for (int frame = 0; result == -3; frame++) {
		const uint32_t b = read_pad();
		uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS, back = o_ok ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls / Refresh, global settings; saved by rei_menu_update on close)
			if (rei_menu_update(b) != REI_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				if (s_list_refresh)
					result = REI_LIST_REFRESH;
			}
		} else if (s_gl_msg[0]) { // notice box: any button closes it
			if (pressed & (ok | back | PSP_CTRL_START | PSP_CTRL_TRIANGLE))
				s_gl_msg[0] = 0;
		} else if (s_gl_about) {
			const int max = ABOUT_LINES > ABOUT_VISIBLE + 2 ? ABOUT_LINES - ABOUT_VISIBLE - 2 : 0;
			if ((pressed & PSP_CTRL_UP) && s_about_scroll > 0) s_about_scroll--;
			if ((pressed & PSP_CTRL_DOWN) && s_about_scroll < max) s_about_scroll++;
			if (pressed & (ok | back | PSP_CTRL_TRIANGLE | PSP_CTRL_START))
				s_gl_about = 0;
		} else {
			const int cnt = s_gl_cnt;
			if (cnt && (pressed & PSP_CTRL_UP)) s_gl_sel = wrap(s_gl_sel - 1, cnt);
			if (cnt && (pressed & PSP_CTRL_DOWN)) s_gl_sel = wrap(s_gl_sel + 1, cnt);
			// left/right: one page, L/R: board tab
			if (cnt && (pressed & PSP_CTRL_LEFT)) s_gl_sel = s_gl_sel >= 8 ? s_gl_sel - 8 : 0;
			if (cnt && (pressed & PSP_CTRL_RIGHT)) s_gl_sel = s_gl_sel + 8 < cnt ? s_gl_sel + 8 : cnt - 1;
			if (s_gl_ntabs > 1 && (pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & PSP_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				int first = -1; // first game of that tab
				for (int i = 0; i < s_gl_n && first < 0; i++)
					if (gl_visible(i) && !strcmp(gl_board(i), s_gl_tabs[s_gl_tab]))
						first = i;
				gl_build(first);
			} else if (pressed & PSP_CTRL_SELECT) { // filter: Playable -> All -> Favorites (current game kept if visible)
				g_settings.list_filter = wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT);
				auto_all = 0;
				rei_settings_save();
				gl_build(cnt ? s_gl_idx[s_gl_sel] : -1);
			} else if (cnt && (pressed & PSP_CTRL_SQUARE)) { // favourite on/off (if it drops out of Favorites the cursor moves on)
				const int cur = s_gl_idx[s_gl_sel];
				rei_fav_toggle(gl_set(cur));
				int w2 = cur;
				if (g_settings.list_filter == REI_LIST_FAVORITES && !gl_visible(cur))
					w2 = s_gl_sel + 1 < cnt ? s_gl_idx[s_gl_sel + 1] : (s_gl_sel > 0 ? s_gl_idx[s_gl_sel - 1] : -1);
				const float keep = s_gl_scr;
				gl_build(w2);
				s_gl_scr = keep;
			} else if (pressed & PSP_CTRL_TRIANGLE) {
				s_gl_about = 1;
				s_about_scroll = 0;
			} else if (pressed & PSP_CTRL_START) { // options: edit and save global settings only
				rei_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_pop = 0;
				s_changed = 0;
				s_prev_buttons = b; // (so the START press does not act inside options)
			} else if (cnt && (pressed & ok)) {
				const int g = s_gl_idx[s_gl_sel];
				if (gl_playable(g))
					result = g;
				else {
					const char *par = rei_core_game_parent(s_found[g].game);
					if (!s_found[g].supported)
						snprintf(s_gl_msg, sizeof(s_gl_msg), "%s board is not supported on PSP yet\n(the Vita version runs it)",
							gl_board(g));
					else
						snprintf(s_gl_msg, sizeof(s_gl_msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", gl_set(g),
							par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
				}
			}
		}
		video_present();
		sceDisplayWaitVblankStart();
		if (frame % 30 == 29)
			scePowerTick(PSP_POWER_TICK_ALL); // (no input for a while in the list: keep the screen on)
		if (s_gl_poll && s_gl_poll())
			result = REI_LIST_QUIT;
#ifdef REI_LOG_ENABLED
		{ // dev loop: stop request (host0:/rei_stop) while the list is up
			SceIoStat st;
			if (frame % 30 == 0 && sceIoGetstat("host0:/rei_stop", &st) >= 0)
				result = REI_LIST_STOP;
		}
#endif
	}
	s_gl_on = 0;
	return result;
}

void rei_game_select_set_poll(int (*poll)(void))
{
	s_gl_poll = poll;
}

int rei_game_select_cursor(void)
{
	return s_gl_cnt ? s_gl_idx[s_gl_sel] : -1;
}

// ---- notice screen (missing ROM, init failure): over the theme background until START ----
static void message_paint(void)
{
	theme_bg(0);
	const int mw = 440, mh = 230, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
	ui_window(mx, my, mw, mh, COL_EDGE, COL_PANEL);
	ui_text_clip(mx + 12, my + 20, COL_WARN, 1.1f, s_msg_title, mx + 12, mx + mw - 8);
	ui_rect(mx + 10, my + 27, mw - 20, 1, COL_DIM);
	char line[160];
	int y = my + 44;
	for (const char *p = s_msg_body; p && *p && y < my + mh - 20;) {
		const char *e = strchr(p, '\n');
		const int len = e ? (int)(e - p) : (int)strlen(p);
		snprintf(line, sizeof(line), "%.*s", len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 1, p);
		ui_text_clip(mx + 12, y, COL_TEXT, 0.85f, line, mx + 12, mx + mw - 8);
		y += 13;
		p = e ? e + 1 : p + len;
	}
	ui_text(mx + 12, my + mh - 9, COL_DIM, 0.8f, "START: continue");
}

void rei_message_screen(const char *title, const char *body)
{
	s_msg_title = title;
	s_msg_body = body;
	uint32_t prev = 0xffffffffu;
	for (int n = 0;; n++) {
		const uint32_t b = read_pad(), pressed = b & ~prev;
		prev = b;
		if (pressed & PSP_CTRL_START)
			break;
		if (s_gl_poll && s_gl_poll()) // (HOME exit while the notice is up)
			break;
#ifdef REI_LOG_ENABLED
		// Dev loop: also close on stop request (host0:/rei_stop); unattended runs got stuck on this screen (2026-10-13)
		SceIoStat st;
		if (n % 30 == 29 && sceIoGetstat("host0:/rei_stop", &st) >= 0)
			break;
#endif
#if defined(REI_LOG_ENABLED) || defined(REI_PSP_TESTGAME)
		if (n == 180) { // (tests: closes by itself after 3 s, the run goes on)
			SceIoStat st;
			if (sceIoGetstat("host0:/rei_game", &st) >= 0)
				break;
		}
#endif
		video_present();
		sceDisplayWaitVblankStart();
	}
	s_msg_title = NULL;
}

// ---- the overlay stage of every composed screen ----
void rei_menu_overlay(void)
{
	const int list = s_gl_on, msg = s_msg_title != NULL, loading = s_loading >= 0;
	const int fps = g_settings.show_fps && !s_open && !list && !msg && !loading;
	const int osd = s_osd[0] && sceKernelGetSystemTimeWide() < s_osd_until;
	if (!list && !msg && !loading && !s_open && !fps && !osd)
		return; // (in the game with nothing on screen: no GE work at all)
	ui_init();
	ui_begin();
	if (loading)
		loading_paint();
	else if (msg)
		message_paint();
	else if (list)
		list_paint();
	else {
		if (fps)
			fps_paint();
		if (s_open) {
			ui_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG); // the game's last frame dimmed under the panel
			menu_paint();
		}
	}
	osd_paint();
	ui_end();
}
