#include "frontend/menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <vita2d.h>

#include "frontend/settings.h"
#include "frontend/states.h"
#include "frontend/video.h"
#include "rei_core.h"
#include "rei_games.h"
#include "rei_version.h" // generated at build time (tools/git_version.cmake)
#include "platform/timer.h"

#define MENU_OPEN_COMBO (SCE_CTRL_SELECT | SCE_CTRL_START)

#define SCREEN_W 960
#define SCREEN_H 544
#define PANEL_W  640
#define LINE_H   30
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

#define COL_BG      RGBA8(0x00, 0x00, 0x00, 0xC0)
#define COL_PANEL   RGBA8(0x18, 0x20, 0x48, 0xF0)
#define COL_TEXT    RGBA8(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_DIM     RGBA8(0xA0, 0xA8, 0xC0, 0xFF)
#define COL_SEL     RGBA8(0xF0, 0x90, 0x20, 0xFF)
#define COL_WARN    RGBA8(0xFF, 0x70, 0x60, 0xFF)

enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_DIPS, PAGE_CHEATS, PAGE_ABOUT };
enum { M_RESUME, M_RESET, M_STATES, M_DISPLAY, M_CONTROLS, M_DIPS, M_CHEATS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT, M_BACK, M_COUNT };
// Game list options (START in the game list): no game loaded, only Display / Controls / Back, global settings only
static int s_list_mode;
// Main line count and line → item (with per-game settings only Delete shows, otherwise only Save; Back only in list mode)
static int main_rows(void)
{
	return s_list_mode ? 3 : M_COUNT - 2;
}
static int main_kind(int row)
{
	if (s_list_mode) {
		static const int k[3] = { M_DISPLAY, M_CONTROLS, M_BACK };
		return k[row];
	}
	const int hidden = rei_settings_has_game() ? M_GAME_SAVE : M_GAME_DEL;
	return row >= hidden ? row + 1 : row;
}
enum { S_SLOT, S_SAVE, S_LOAD, S_BACK, S_COUNT };
enum { D_FPS, D_ASPECT, D_FILTER, D_SCAN, D_SCAN_STR, D_BORDER, D_BORDER_COL, D_BACK, D_COUNT };
// Controls lines: game buttons (as many as the game uses, rei_core_button_count; 6 with no game) · coin · start · turbo key ·
// turbo button · turbo speed · confirm button · defaults · back (games with 3+ buttons get more lines)
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_CONFIRM, CK_DEFAULTS, CK_BACK };
static int ctrl_buttons(void)
{
	if (s_list_mode)
		return 6;
	const int b = rei_core_button_count();
	return b < 2 ? 2 : (b > 6 ? 6 : b);
}
static int ctrl_rows(void)
{
	return ctrl_buttons() + 2 + 6;
}
// Type of line i. For game button/coin/start lines, *act = REI_ACT_*
static int ctrl_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (i < b) {
		*act = i < 2 ? i : REI_ACT_B3 + (i - 2);
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? REI_ACT_COIN : REI_ACT_START;
		return CK_ACT;
	}
	return CK_TURBO_KEY + (i - b - 2);
}
static const char *act_name(int act)
{
	static const char *const n[REI_ACT_COUNT] = { "Button 1", "Button 2", "Coin", "Start", "Button 3", "Button 4", "Button 5", "Button 6" };
	return n[act];
}
// Cheat page lines: (nbbatman family) two built-in cheats + cheats from the user cheat file (cheats/<set>.ini, FBNeo format) + Back. Scrolls when long
enum { HK_STAGE, HK_HEALTH, HK_FILE, HK_BACK };
#define CHEAT_VISIBLE 12
static int s_cheat_top;
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

// About: vertically scrolling text (empty string = blank line, leading '#' = heading line)
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
static int s_about_scroll;

static vita2d_pgf *s_font;
static int s_page, s_sel;
static int s_main_sel; // main cursor position when returning from a subpage
static int s_reset_armed; // first Reset press waits for confirmation
static int s_state_slot;  // Save/Load page slot (0-9)

// bottom-center status display
static char s_osd[96];
static uint64_t s_osd_until;
static uint32_t s_prev_buttons;

// FPS: computed every 0.5 s from frames presented
static uint64_t s_fps_t0;
static int s_fps_frames;
static float s_fps;

void rei_menu_init(void)
{
	s_font = vita2d_load_default_pgf();
	s_fps_t0 = rei_time_us();
}

void rei_menu_fini(void)
{
	if (s_font) {
		vita2d_free_pgf(s_font);
		s_font = NULL;
	}
}

int rei_menu_should_open(uint32_t buttons)
{
	int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	int open = combo && !prev_combo;
	s_prev_buttons = buttons; // copy current state to previous so the same press doesn't act immediately inside the menu
	if (open) {
		s_page = PAGE_MAIN;
		s_sel = 0;
		s_reset_armed = 0;
	}
	return open;
}

static int dip_items(void)
{
	return rei_core_dip_count() + 2; // + defaults, back
}

static int page_items(void)
{
	switch (s_page) {
	case PAGE_DISPLAY: return D_COUNT;
	case PAGE_CONTROLS: return ctrl_rows();
	case PAGE_DIPS: return dip_items();
	case PAGE_CHEATS: return cheat_rows();
	case PAGE_STATES: return S_COUNT;
	case PAGE_ABOUT: return ABOUT_VISIBLE; // fixed line count (scrolls)
	default: return main_rows();
	}
}

static void save_dips(void)
{
	g_settings.dip_count = rei_core_dip_raw(g_settings.dip, REI_DIP_BYTES_MAX);
}

static void go_page(int page)
{
	if (s_page == PAGE_MAIN)
		s_main_sel = s_sel;
	s_page = page;
	s_sel = (page == PAGE_MAIN) ? s_main_sel : 0;
	s_reset_armed = 0;
}

// Repeat while a direction is held (after ~0.4 s, every 0.07 s; call once per screen update). Adds repeats to pressed
#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
static uint32_t key_repeat(uint32_t held, uint32_t pressed, int *count)
{
	const uint32_t dir = held & REPEAT_KEYS;
	if (!dir || (pressed & REPEAT_KEYS)) {
		*count = 0;
		return pressed;
	}
	++*count;
	if (*count >= 24 && (*count - 24) % 4 == 0)
		pressed |= dir;
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

// Apply display settings to the game video (in the game list there is no game video yet: rei_video_init applies them)
static void apply_video(void)
{
	if (!s_list_mode)
		rei_video_apply_settings();
}

// Change value (left/right dir = -1/+1, confirm button +1)
static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_settings.show_fps = !g_settings.show_fps; break;
		case D_ASPECT:
			g_settings.aspect = wrap(g_settings.aspect + dir, REI_ASPECT_COUNT);
			apply_video(); // scanline height changes
			break;
		case D_FILTER:
			g_settings.filter = wrap(g_settings.filter + dir, REI_FILTER_COUNT);
			apply_video();
			break;
		case D_SCAN:
			g_settings.scan_type = wrap(g_settings.scan_type + dir, REI_SCAN_COUNT);
			apply_video();
			break;
		case D_SCAN_STR:
			g_settings.scan_strength = wrap(g_settings.scan_strength + dir, REI_SCAN_STRENGTH_COUNT);
			apply_video();
			break;
		case D_BORDER: g_settings.border = wrap(g_settings.border + dir, REI_BORDER_COUNT); break;
		case D_BORDER_COL: g_settings.border_color = wrap(g_settings.border_color + dir, REI_BCOL_COUNT); break;
		}
		break;
	case PAGE_CONTROLS:
	{
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_ACT) // buttons 3-6 also allow None (unassigned)
			g_settings.keymap[act] = wrap(g_settings.keymap[act] + dir, act >= REI_ACT_B3 ? REI_KEY_COUNT + 1 : REI_KEY_COUNT);
		else if (k == CK_TURBO_KEY)
			g_settings.turbo_key = wrap(g_settings.turbo_key + dir, REI_KEY_COUNT + 1); // + None
		else if (k == CK_TURBO_BTN) // 1..game button count
			g_settings.turbo_btn = wrap(g_settings.turbo_btn - 1 + dir, ctrl_buttons()) + 1;
		else if (k == CK_TURBO_SPEED)
			g_settings.turbo_speed = wrap(g_settings.turbo_speed + dir, REI_TURBO_SPEED_COUNT);
		else if (k == CK_CONFIRM)
			g_settings.menu_confirm = wrap(g_settings.menu_confirm + dir, REI_CONFIRM_COUNT);
	}
		break;
	case PAGE_DIPS:
		if (s_sel < rei_core_dip_count()) {
			int n = rei_core_dip_options(s_sel), cur = rei_core_dip_get(s_sel);
			if (n > 0) {
				rei_core_dip_set(s_sel, wrap((cur < 0 ? 0 : cur) + dir, n));
				save_dips();
			}
		}
		break;
	case PAGE_STATES:
		if (s_sel == S_SLOT)
			s_state_slot = wrap(s_state_slot + dir, REI_STATE_SLOTS);
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
		} else if (k == HK_FILE && rei_core_cheat_options(ci) > 0) {
			rei_core_cheat_set(ci, wrap(rei_core_cheat_get(ci) + dir, rei_core_cheat_options(ci)));
		}
		break;
	}
	}
}

// Confirm button
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
		case M_DIPS: go_page(PAGE_DIPS); break;
		case M_CHEATS:
			if (any_cheats()) {
				go_page(PAGE_CHEATS);
				s_cheat_top = 0;
			} else
				rei_osd_show("No cheats: put %s.ini in cheats/", rei_core_set_name());
			break;
		case M_GAME_SAVE: // this game only (global settings file not written)
			if (rei_settings_save_game())
				rei_osd_show("Saved settings for %s", rei_core_set_name());
			else
				rei_osd_show("Could not save game settings");
			break;
		case M_GAME_DEL:
			if (rei_settings_delete_game()) {
				rei_osd_show("Deleted settings for %s", rei_core_set_name());
			} else
				rei_osd_show("Could not delete game settings");
			break;
		case M_ABOUT:
			go_page(PAGE_ABOUT);
			s_about_scroll = 0;
			break;
		case M_SELECT_GAME: return REI_MENU_SELECT_GAME;
		case M_EXIT: return REI_MENU_EXIT;
		case M_BACK: return REI_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS:
	{
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_BACK) go_page(PAGE_MAIN);
		else if (k == CK_DEFAULTS) rei_settings_default_keys();
		else change(+1);
	}
		break;
	case PAGE_DIPS:
		if (s_sel == dip_items() - 1) go_page(PAGE_MAIN);
		else if (s_sel == dip_items() - 2) {
			rei_core_dip_defaults();
			save_dips();
		} else change(+1);
		break;
	case PAGE_CHEATS: {
		int ci = 0;
		if (cheat_kind(s_sel, &ci) == HK_BACK) go_page(PAGE_MAIN);
		else change(+1);
	}
		break;
	case PAGE_ABOUT:
		go_page(PAGE_MAIN);
		break;
	case PAGE_STATES:
		if (s_sel == S_SAVE) return REI_MENU_SAVE;
		if (s_sel == S_LOAD) {
			if (rei_state_exists(s_state_slot))
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
	static int rep_count;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_count);
	s_prev_buttons = buttons;
	rei_menu_result r = REI_MENU_STAY;
	const int n = page_items();

	if (s_page == PAGE_ABOUT) { // up/down scrolls
		const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
		if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
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
	// Confirm/back buttons follow the setting (x/o). Use the setting at press time even if it changes during this input pass
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = REI_MENU_CLOSE;
		else
			go_page(PAGE_MAIN);
	}
	if (pressed & SCE_CTRL_START)
		r = REI_MENU_CLOSE;

	if (r != REI_MENU_STAY)
		rei_settings_save();
	return r;
}

// One line: name on the left, "< value >" on the right (if any)
static void item(int px, int ty, int i, const char *name, const char *value)
{
	const unsigned int col = (i == s_sel) ? COL_SEL : COL_TEXT;
	if (i == s_sel)
		vita2d_pgf_draw_text(s_font, px + 16, ty, COL_SEL, 1.0f, ">");
	vita2d_pgf_draw_text(s_font, px + 40, ty, col, 1.0f, name);
	if (value) {
		char buf[96];
		snprintf(buf, sizeof(buf), "< %s >", value);
		vita2d_pgf_draw_text(s_font, px + VALUE_X, ty, col, 1.0f, buf);
	}
}

// --- button symbols (the default PGF font lacks ○□△, so they are drawn as shapes) ---
static void thick_line(float x0, float y0, float x1, float y1, unsigned int c)
{
	for (int dx = -1; dx <= 1; dx++)
		for (int dy = -1; dy <= 1; dy++)
			if (dx * dx + dy * dy <= 1)
				vita2d_draw_line(x0 + dx * 0.7f, y0 + dy * 0.7f, x1 + dx * 0.7f, y1 + dy * 0.7f, c);
}

// Draw a symbol starting at (x, cy), vertically centered on cy; returns the width used. Text buttons (L, R, SELECT, START) are drawn as text
static int draw_key(int x, int cy, int ty, int key, unsigned int text_col)
{
	const float r = 9.0f;
	const float cx = x + r + 1;
	switch (key) {
	case REI_KEY_CROSS: {
		const unsigned int c = RGBA8(0x7C, 0xB4, 0xF0, 0xFF);
		thick_line(cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, c);
		thick_line(cx - r * 0.8f, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.8f, c);
		return (int)(r * 2 + 2);
	}
	case REI_KEY_CIRCLE:
		vita2d_draw_fill_circle(cx, cy, r, RGBA8(0xFF, 0x6E, 0x6E, 0xFF));
		vita2d_draw_fill_circle(cx, cy, r - 2.5f, COL_PANEL);
		return (int)(r * 2 + 2);
	case REI_KEY_SQUARE: {
		const unsigned int c = RGBA8(0xF0, 0xA0, 0xD8, 0xFF);
		const float h = r * 0.85f;
		vita2d_draw_rectangle(cx - h, cy - h, h * 2, 2.5f, c);
		vita2d_draw_rectangle(cx - h, cy + h - 2.5f, h * 2, 2.5f, c);
		vita2d_draw_rectangle(cx - h, cy - h, 2.5f, h * 2, c);
		vita2d_draw_rectangle(cx + h - 2.5f, cy - h, 2.5f, h * 2, c);
		return (int)(r * 2 + 2);
	}
	case REI_KEY_TRIANGLE: {
		const unsigned int c = RGBA8(0x50, 0xE0, 0xC0, 0xFF);
		const float top = cy - r, bot = cy + r * 0.75f, half = r * 1.0f;
		thick_line(cx, top, cx - half, bot, c);
		thick_line(cx, top, cx + half, bot, c);
		thick_line(cx - half, bot, cx + half, bot, c);
		return (int)(r * 2 + 2);
	}
	default: {
		const char *name = rei_key_name(key);
		vita2d_pgf_draw_text(s_font, x, ty, text_col, 1.0f, name);
		return vita2d_pgf_text_width(s_font, 1.0f, name);
	}
	}
}

// One line: name, "< button >" on the right (button as a symbol)
static void item_key(int px, int ty, int i, const char *name, int key)
{
	const unsigned int col = (i == s_sel) ? COL_SEL : COL_TEXT;
	item(px, ty, i, name, NULL);
	int x = px + VALUE_X;
	vita2d_pgf_draw_text(s_font, x, ty, col, 1.0f, "<");
	x += vita2d_pgf_text_width(s_font, 1.0f, "< ");
	x += draw_key(x, ty - 7, ty, key, col);
	vita2d_pgf_draw_text(s_font, x + 6, ty, col, 1.0f, ">");
}

static void text_keys(int x, int ty, unsigned int col, const char *s);

void rei_menu_draw(void)
{
	if (!s_font)
		return;
	static const char *const aspect[] = { "4:3", "Full screen", "Integer 2x" };
	static const char *const filter[] = { "Smooth", "Sharp" };
	static const char *const scan[] = { "Off", "CRT soft", "CRT sharp", "Every 2 px", "Every 3 px", "Every 4 px" };
	static const char *const scan_str[] = { "Light", "Medium", "Heavy" };
	static const char *const border[] = { "Black", "Artwork", "Color" };
	static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
	static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
	static const char *const stages[REI_STAGE_COUNT] = {
		"1 Seattle", "2 San Francisco", "3 Las Vegas", "4 Texas", "5 Florida", "6 Chicago", "Final New York",
	};
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? "\x11" : "\x12", *back_name = o_ok ? "\x12" : "\x11"; // (button symbols, drawn by text_keys)
	char foot_buf[128];

	int n = page_items();
	if (s_page == PAGE_CHEATS) { // scroll so the cursor stays visible when there are many lines
		if (s_sel < s_cheat_top) s_cheat_top = s_sel;
		if (s_sel >= s_cheat_top + CHEAT_VISIBLE) s_cheat_top = s_sel - CHEAT_VISIBLE + 1;
		if (n > CHEAT_VISIBLE) n = CHEAT_VISIBLE;
	}
	const int h = TITLE_H + n * LINE_H + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
	vita2d_draw_rectangle(px, py, PANEL_W, h, COL_PANEL);

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY: title = "DISPLAY"; break;
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
		break;
	}
	char tbuf[128];
	snprintf(tbuf, sizeof(tbuf), "%s", s_page == PAGE_MAIN && !s_list_mode ? rei_core_game_name() : title);
	vita2d_pgf_draw_text(s_font, px + 24, py + 36, COL_TEXT, 1.1f, tbuf);

	for (int i = 0; i < n; i++) {
		const int ty = py + TITLE_H + i * LINE_H + 22;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = {
				"Resume game", "Reset game", "Save / Load state  >", "Display  >", "Controls  >", "DIP switches  >",
				"Cheats  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit", "Back",
			};
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[64];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", o_ok ? "Circle" : "Cross");
				name = rbuf;
			}
			if (k == M_CHEATS && !any_cheats())
				name = "Cheats (none for this game)";
			item(px, ty, i, name, NULL);
			break;
		}
		case PAGE_DISPLAY: {
			const char *v = NULL;
			static const char *const names[D_COUNT] = {
				"Show FPS", "Screen", "Filter", "Scanlines", "Scanline strength",
				"Border (4:3 / 2x)", "Border color", "Back",
			};
			switch (i) {
			case D_FPS: v = g_settings.show_fps ? "On" : "Off"; break;
			case D_ASPECT: v = aspect[g_settings.aspect]; break;
			case D_FILTER: v = filter[g_settings.filter]; break;
			case D_SCAN: v = scan[g_settings.scan_type]; break;
			case D_SCAN_STR: v = scan_str[g_settings.scan_strength]; break;
			case D_BORDER: v = border[g_settings.border]; break;
			case D_BORDER_COL: v = border_col[g_settings.border_color]; break;
			}
			item(px, ty, i, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		{
			int act;
			const int k = ctrl_kind(i, &act);
			if (k == CK_ACT)
				item_key(px, ty, i, act_name(act), g_settings.keymap[act]);
			else if (k == CK_TURBO_KEY)
				item_key(px, ty, i, "Turbo key", g_settings.turbo_key);
			else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", g_settings.turbo_btn);
				item(px, ty, i, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(px, ty, i, "Turbo speed", turbo_speed[g_settings.turbo_speed]);
			else if (k == CK_CONFIRM)
				item_key(px, ty, i, "Menu confirm button", o_ok ? REI_KEY_CIRCLE : REI_KEY_CROSS);
			else
				item(px, ty, i, k == CK_DEFAULTS ? "Restore defaults" : "Back", NULL);
		}
			break;
		case PAGE_DIPS:
			if (i < rei_core_dip_count()) {
				int o = rei_core_dip_get(i);
				item(px, ty, i, rei_core_dip_name(i), o < 0 ? "?" : rei_core_dip_option_name(i, o));
			} else {
				item(px, ty, i, i == n - 2 ? "Restore defaults" : "Back", NULL);
			}
			break;
		case PAGE_STATES:
			if (i == S_SLOT) {
				char sbuf[32];
				snprintf(sbuf, sizeof(sbuf), "%d  (%s)", s_state_slot, rei_state_exists(s_state_slot) ? "used" : "empty");
				item(px, ty, i, "Slot", sbuf);
			} else {
				static const char *const names[S_COUNT] = { "", "Save state", "Load state", "Back" };
				item(px, ty, i, names[i], NULL);
			}
			break;
		case PAGE_ABOUT: {
			const int k = s_about_scroll + i;
			if (k < ABOUT_LINES && s_about[k][0]) {
				const int head = s_about[k][0] == '#';
				vita2d_pgf_draw_text(s_font, px + 24, ty, head ? COL_SEL : (s_about[k][0] == ' ' ? COL_DIM : COL_TEXT),
					head ? 1.0f : 0.85f, s_about[k] + head);
			}
			break;
		}
		case PAGE_CHEATS: {
			const int row = s_cheat_top + i;
			int ci = 0;
			const int k = cheat_kind(row, &ci);
			if (k == HK_STAGE)
				item(px, ty, row, "Start stage", stages[g_settings.start_stage]);
			else if (k == HK_HEALTH)
				item(px, ty, row, "Infinite health (1P)", g_settings.cheat_health ? "On" : "Off");
			else if (k == HK_FILE)
				item(px, ty, row, rei_core_cheat_name(ci), rei_core_cheat_option_name(ci, rei_core_cheat_get(ci)));
			else
				item(px, ty, row, "Back", NULL);
			break;
		}
		}
	}
	const unsigned int fcol = (s_page == PAGE_DIPS) ? COL_WARN : COL_DIM;
	text_keys(px + 24, py + h - 18, fcol, foot);
}

int rei_menu_state_slot(void)
{
	return s_state_slot;
}

// Game list: tabs per board (system), in board-number order. L/R (or left/right) switch tabs, up/down pick games. SELECT cycles the filter (Playable: games
// with ROMs only / All: every supported game, ROM-less ones grayed, picking one shows a notice / Favorites). □ toggles favorite, △ is About
// (same behavior as PSP psp/src/menu.c)
#define MAX_TABS 16
#define COL_GREY RGBA8(0x70, 0x74, 0x84, 0xFF)
#define COL_GREY_SEL RGBA8(0xB0, 0x80, 0x50, 0xFF)
// symbol characters in text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

// One text line (scale 0.8): symbol chars (0x10-0x13) drawn as button images
static void text_keys(int x, int ty, unsigned int col, const char *s)
{
	char run[128];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char c = (unsigned char)*p;
		if ((c >= 0x10 && c <= 0x13) || !c || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			if (k) {
				vita2d_pgf_draw_text(s_font, x, ty, col, 0.8f, run);
				x += vita2d_pgf_text_width(s_font, 0.8f, run);
			}
			k = 0;
			if (!c)
				break;
			if (c >= 0x10 && c <= 0x13) {
				static const int keys[4] = { REI_KEY_TRIANGLE, REI_KEY_CIRCLE, REI_KEY_CROSS, REI_KEY_SQUARE };
				x += draw_key(x, ty - 6, ty, keys[c - 0x10], col) + 2;
				continue;
			}
		}
		run[k++] = (char)c;
	}
}

static int playable(const rei_game_found *g) { return g->present && g->supported; }

static const char *s_gl_tabs[MAX_TABS];
static int s_gl_ntabs, s_gl_tab, s_gl_cnt, s_gl_sel;
static int s_gl_idx[REI_GAMES_MAX]; // game indices in the current tab
static int s_gl_nvis;               // games in the current filter (all tabs)
static const char *const s_gl_filter_name[REI_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };

// Game passes the current filter
static int gl_visible(const rei_game_found *g)
{
	if (g_settings.list_filter == REI_LIST_ALL)
		return 1;
	if (g_settings.list_filter == REI_LIST_FAVORITES)
		return rei_fav_is(rei_core_game_set(g->game));
	return playable(g);
}

// Rebuild tabs from games matching the filter and move to the tab/position of want (game index, may be -1)
static void gl_build(const rei_game_found *found, int n, int want)
{
	const char *cur = (s_gl_tab < s_gl_ntabs) ? s_gl_tabs[s_gl_tab] : NULL;
	s_gl_ntabs = 0;
	s_gl_nvis = 0;
	for (int i = 0; i < n; i++) {
		if (!gl_visible(&found[i]))
			continue;
		s_gl_nvis++;
		const char *bd = rei_core_game_board(found[i].game);
		int t = 0;
		while (t < s_gl_ntabs && strcmp(s_gl_tabs[t], bd) != 0)
			t++;
		if (t == s_gl_ntabs && s_gl_ntabs < MAX_TABS)
			s_gl_tabs[s_gl_ntabs++] = bd;
	}
	s_gl_tab = 0;
	const char *tb = want >= 0 && gl_visible(&found[want]) ? rei_core_game_board(found[want].game) : cur;
	for (int t = 0; tb && t < s_gl_ntabs; t++)
		if (!strcmp(s_gl_tabs[t], tb))
			s_gl_tab = t;
	s_gl_cnt = 0;
	s_gl_sel = 0;
	for (int i = 0; i < n && s_gl_cnt < REI_GAMES_MAX && s_gl_ntabs; i++) {
		if (!gl_visible(&found[i]) || strcmp(rei_core_game_board(found[i].game), s_gl_tabs[s_gl_tab]))
			continue;
		if (i == want)
			s_gl_sel = s_gl_cnt;
		s_gl_idx[s_gl_cnt++] = i;
	}
}

int rei_game_select(const rei_game_found *found, int n, int initial, const char *rom_dir)
{
	int nplay = 0;
	for (int i = 0; i < n; i++)
		nplay += playable(&found[i]);
	if (!nplay && g_settings.list_filter == REI_LIST_PLAYABLE)
		g_settings.list_filter = REI_LIST_ALL; // (show all if no runnable game)
	s_gl_ntabs = s_gl_tab = 0;
	gl_build(found, n, initial);
	int top = 0;
	int about = 0;         // viewing About
	char msg[256] = { 0 }; // notice box (none if empty string)
	uint32_t prev = 0xffffffffu; // ignore until first release so the press that launched the app doesn't select
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t b = pad.buttons;
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		static int rep_count;
		const uint32_t pressed = key_repeat(b, b & ~prev, &rep_count);
		prev = b;
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls, global settings; rei_menu_update saves on close)
			if (rei_menu_update(b) != REI_MENU_STAY)
				s_list_mode = 0;
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
			if (s_gl_cnt && (pressed & SCE_CTRL_UP)) s_gl_sel = wrap(s_gl_sel - 1, s_gl_cnt);
			if (s_gl_cnt && (pressed & SCE_CTRL_DOWN)) s_gl_sel = wrap(s_gl_sel + 1, s_gl_cnt);
			// left/right: page by screen, L/R: board (tab)
			if (s_gl_cnt && (pressed & SCE_CTRL_LEFT)) s_gl_sel = s_gl_sel >= 12 ? s_gl_sel - 12 : 0;
			if (s_gl_cnt && (pressed & SCE_CTRL_RIGHT)) s_gl_sel = s_gl_sel + 12 < s_gl_cnt ? s_gl_sel + 12 : s_gl_cnt - 1;
			if (s_gl_ntabs > 1 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				const char *tb = s_gl_tabs[s_gl_tab];
				int first = -1; // first game in that tab
				for (int i = 0; i < n && first < 0; i++)
					if (gl_visible(&found[i]) && !strcmp(rei_core_game_board(found[i].game), tb))
						first = i;
				gl_build(found, n, first);
				top = 0;
				continue;
			}
			if (pressed & SCE_CTRL_SELECT) { // filter: Playable -> All -> Favorites (keep the current game if visible)
				g_settings.list_filter = wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT);
				rei_settings_save();
				gl_build(found, n, s_gl_cnt ? s_gl_idx[s_gl_sel] : -1);
				top = 0;
				continue;
			}
			if (s_gl_cnt && (pressed & SCE_CTRL_SQUARE)) { // favorite on/off (in Favorites the game leaves: cursor to the next, or previous at the end)
				const int cur = s_gl_idx[s_gl_sel];
				rei_fav_toggle(rei_core_game_set(found[cur].game));
				int want = cur;
				if (g_settings.list_filter == REI_LIST_FAVORITES && !gl_visible(&found[cur]))
					want = s_gl_sel + 1 < s_gl_cnt ? s_gl_idx[s_gl_sel + 1] : (s_gl_sel > 0 ? s_gl_idx[s_gl_sel - 1] : -1);
				gl_build(found, n, want);
				continue;
			}
			if (pressed & SCE_CTRL_TRIANGLE) {
				about = 1;
				s_about_scroll = 0;
			} else if (pressed & SCE_CTRL_START) { // options: edit and save the global settings only
				rei_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_prev_buttons = b; // (the START press does not act inside the options)
			}
			if (s_gl_cnt && (pressed & ok) && !s_list_mode) {
				const rei_game_found *g = &found[s_gl_idx[s_gl_sel]];
				if (playable(g))
					return s_gl_idx[s_gl_sel];
				const char *par = rei_core_game_parent(g->game);
				snprintf(msg, sizeof(msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", rei_core_game_set(g->game),
					par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
			}
		}

		const int rows = 12, vis = s_gl_cnt < rows ? s_gl_cnt : rows;
		if (s_gl_sel < top) top = s_gl_sel;
		if (s_gl_sel >= top + vis) top = s_gl_sel - vis + 1;
		if (top > s_gl_cnt - vis) top = s_gl_cnt - vis;
		if (top < 0) top = 0;
		const int tab_h = 40;
		const int h = TITLE_H + tab_h + rows * LINE_H + FOOT_H, w = 800;
		const int px = (SCREEN_W - w) / 2, py = (SCREEN_H - h) / 2;
		vita2d_start_drawing();
		vita2d_clear_screen();
		vita2d_draw_rectangle(px, py, w, h, COL_PANEL);
		if (s_font && about) {
			vita2d_pgf_draw_text(s_font, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
			for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++) {
				const char *t = s_about[s_about_scroll + i];
				if (!t[0])
					continue;
				const int head = t[0] == '#';
				vita2d_pgf_draw_text(s_font, px + 24, py + TITLE_H + i * LINE_H + 22,
					head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT), head ? 1.0f : 0.85f, t + head);
			}
			text_keys(px + 24, py + h - 18, COL_DIM, o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
		} else if (s_font) {
			char title[64];
			snprintf(title, sizeof(title), "REIREM - SELECT GAME (%d/%d)", s_gl_cnt ? s_gl_sel + 1 : 0, s_gl_cnt);
			vita2d_pgf_draw_text(s_font, px + 24, py + 36, COL_TEXT, 1.1f, title);
			char have[40];
			snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_settings.list_filter], s_gl_nvis);
			vita2d_pgf_draw_text(s_font, px + w - 24 - vita2d_pgf_text_width(s_font, 0.85f, have), py + 34, COL_DIM, 0.85f, have);
			{ // tab row: current tab highlighted, scrolled to keep it visible when overflowing
				int tx = px + 16, first = 0, total = 0;
				for (int t = 0; t < s_gl_ntabs; t++)
					total += vita2d_pgf_text_width(s_font, 0.95f, s_gl_tabs[t]) + 28;
				while (total > w - 32 && first < s_gl_tab) {
					total -= vita2d_pgf_text_width(s_font, 0.95f, s_gl_tabs[first]) + 28;
					first++;
				}
				for (int t = first; t < s_gl_ntabs; t++) {
					const int tw = vita2d_pgf_text_width(s_font, 0.95f, s_gl_tabs[t]) + 20;
					if (tx + tw > px + w - 16)
						break;
					if (t == s_gl_tab)
						vita2d_draw_rectangle(tx, py + TITLE_H - 4, tw, 32, COL_SEL);
					vita2d_pgf_draw_text(s_font, tx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_PANEL : COL_DIM, 0.95f, s_gl_tabs[t]);
					tx += tw + 8;
				}
			}
			if (!s_gl_cnt && g_settings.list_filter == REI_LIST_FAVORITES)
				text_keys(px + 48, py + TITLE_H + tab_h + 22, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
			else if (!s_gl_cnt)
				vita2d_pgf_draw_text(s_font, px + 48, py + TITLE_H + tab_h + 22, COL_DIM, 1.0f, "No playable games. SELECT: show all");
			for (int i = top; i < top + vis; i++) {
				const int ty = py + TITLE_H + tab_h + (i - top) * LINE_H + 22;
				const rei_game_found *g = &found[s_gl_idx[i]];
				const int ok_g = playable(g);
				const unsigned int col = (i == s_gl_sel) ? (ok_g ? COL_SEL : COL_GREY_SEL) : (ok_g ? COL_TEXT : COL_GREY);
				if (i == s_gl_sel)
					vita2d_pgf_draw_text(s_font, px + 16, ty, col, 1.0f, ">");
				if (rei_fav_is(rei_core_game_set(g->game))) // favorite mark
					vita2d_pgf_draw_text(s_font, px + 32, ty, COL_WARN, 1.0f, "*");
				vita2d_pgf_draw_text(s_font, px + 48, ty, col, 1.0f, rei_core_game_title(g->game));
			}
			char foot[128];
			snprintf(foot, sizeof(foot), "%sLeft/Right: page   %s: play   " G_TRI ": about", s_gl_ntabs > 1 ? "L/R: system   " : "",
				o_ok ? G_CIR : G_CRO);
			text_keys(px + 24, py + h - 28, COL_DIM, foot);
			snprintf(foot, sizeof(foot), "START: options   " G_SQU ": favorite   SELECT: %s",
				s_gl_filter_name[wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT)]);
			text_keys(px + 24, py + h - 8, COL_DIM, foot);
			if (msg[0]) { // notice box (over the list)
				const int mw = 720, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
				vita2d_draw_rectangle(mx - 2, my - 2, mw + 4, mh + 4, COL_WARN);
				vita2d_draw_rectangle(mx, my, mw, mh, COL_PANEL);
				char line[256];
				snprintf(line, sizeof(line), "%s", msg);
				char *nl = strchr(line, '\n');
				if (nl)
					*nl = 0;
				vita2d_pgf_draw_text(s_font, mx + 24, my + 44, COL_WARN, 1.0f, line);
				if (nl)
					vita2d_pgf_draw_text(s_font, mx + 24, my + 84, COL_TEXT, 0.9f, nl + 1);
				vita2d_pgf_draw_text(s_font, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Press any button");
			}
			if (s_list_mode) // options over the list
				rei_menu_draw();
		}
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}
}

static char s_loading_title[64]; // game name on the loading screen ("Loading..." if empty)

void rei_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
}

// Game loading gauge (rei_core_config.progress etc., 0-1000). Not drawn too often (each draw waits for vblank)
void rei_loading_progress(int permille)
{
	static uint64_t last_t;
	static int last_p = -1000;
	const uint64_t now = rei_time_us();
	if (permille > 0 && permille < 1000 && permille - last_p < 20 && now - last_t < 100000)
		return;
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	last_p = permille;
	last_t = now;
	const int w = 600, h = 18, x = (SCREEN_W - w) / 2, y = SCREEN_H / 2;
	vita2d_start_drawing();
	vita2d_clear_screen();
	if (s_font)
		vita2d_pgf_draw_text(s_font, x, y - 20, COL_TEXT, 1.0f, s_loading_title[0] ? s_loading_title : "Loading...");
	vita2d_draw_rectangle(x - 2, y - 2, w + 4, h + 4, COL_DIM);
	vita2d_draw_rectangle(x, y, w, h, COL_PANEL);
	vita2d_draw_rectangle(x, y, w * permille / 1000, h, COL_SEL);
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

void rei_osd_show(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_osd, sizeof(s_osd), fmt, ap);
	va_end(ap);
	s_osd_until = rei_time_us() + 2000000;
}

void rei_osd_draw(void)
{
	if (!s_font || !s_osd[0] || rei_time_us() > s_osd_until)
		return;
	const int w = vita2d_pgf_text_width(s_font, 1.0f, s_osd) + 40, h = 36;
	const int x = (SCREEN_W - w) / 2, y = SCREEN_H - h - 16;
	vita2d_draw_rectangle(x, y, w, h, RGBA8(0x00, 0x00, 0x00, 0xC8));
	vita2d_draw_rectangle(x, y, w, 2, COL_SEL);
	vita2d_pgf_draw_text(s_font, x + 20, y + 26, COL_TEXT, 1.0f, s_osd);
}

void rei_fps_tick(void)
{
	s_fps_frames++;
	uint64_t now = rei_time_us();
	uint64_t dt = now - s_fps_t0;
	if (dt >= 500000) {
		s_fps = s_fps_frames * 1000000.0f / (float)dt;
		s_fps_frames = 0;
		s_fps_t0 = now;
	}
}

void rei_fps_draw(void)
{
	if (!g_settings.show_fps || !s_font)
		return;
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f FPS", s_fps);
	vita2d_draw_rectangle(SCREEN_W - 132, 4, 128, 30, RGBA8(0x00, 0x00, 0x00, 0xA0));
	unsigned int col = s_fps >= 59.0f ? RGBA8(0x60, 0xFF, 0x60, 0xFF) : RGBA8(0xFF, 0xC0, 0x40, 0xFF);
	vita2d_pgf_draw_text(s_font, SCREEN_W - 124, 26, col, 1.0f, buf);
}
