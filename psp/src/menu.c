// Options screen (Vita vita/src/frontend/menu.c ported to PSP 480x272 with the GE font). Same items and behavior
#include "menu.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rei_core.h"
#include "rei_games.h" // REI_GAMES_MAX
#include "rei_version.h" // generated at build time (tools/git_version.cmake)
#include "settings.h"
#include "video.h"

#define MENU_OPEN_COMBO (PSP_CTRL_SELECT | PSP_CTRL_START)

#define SCREEN_W 480
#define SCREEN_H 272
#define PANEL_W  464
#define LINE_H   12
#define TITLE_H  24
#define FOOT_H   20
#define VALUE_X  232 // from panel left edge to the value column

// 0xAABBGGRR
#define COL_BG    0xc0000000u
#define COL_PANEL 0xf0482018u
#define COL_TEXT  0xffffffffu
#define COL_DIM   0xffc0a8a0u
#define COL_SEL   0xff2090f0u
#define COL_WARN  0xff6070ffu

enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_DIPS, PAGE_CHEATS, PAGE_ABOUT };
enum { M_RESUME, M_RESET, M_STATES, M_DISPLAY, M_CONTROLS, M_DIPS, M_CHEATS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT, M_BACK, M_COUNT };
// Game list options (START in the game list): no game loaded, only Display / Controls / Back, global settings only
static int s_list_mode;
// Main row count and row -> item (with per-game settings only Delete shows, otherwise only Save; Back only in list mode)
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
enum { D_FPS, D_ASPECT, D_FILTER, D_BORDER, D_BORDER_COL, D_BACK, D_COUNT };
// Controls rows: game buttons (as many as the game uses, rei_core_button_count; 6 with no game), coin, start, turbo key,
// turbo button, turbo rate, confirm button, defaults, back (games with 3+ buttons get more rows)
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
// Kind of row i. For game button/coin/start rows, *act = REI_ACT_*
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
// Cheat page rows: (nbbatman family) two built-in cheats + cheats from the user cheat file (cheats/<set>.ini, FBNeo format) + Back. Scrolls if long
enum { HK_STAGE, HK_HEALTH, HK_FILE, HK_BACK };
#define CHEAT_VISIBLE 14
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

// About: vertically scrolling text (empty string = blank line, leading '#' = heading)
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
	"psp-media-engine-custom-core (mcidclan, MIT)",
	"  github.com/mcidclan/psp-media-engine-custom-core",
	"stb_image (Sean Barrett, public domain) - artwork",
	"  https://github.com/nothings/stb",
	"zlib - ROM zip reading",
	"  https://github.com/madler/zlib",
	"newlib - C library",
	"  https://sourceware.org/newlib",
	"",
	"License texts: LICENSE-FBNeo.txt in the source repository.",
};
#define ABOUT_LINES ((int)(sizeof(s_about) / sizeof(s_about[0])))
#define ABOUT_VISIBLE 14
static int s_about_scroll;

static int s_open;
static int s_page, s_sel;
static int s_main_sel;    // main cursor position when returning from a subpage
static int s_reset_armed; // first Reset press waits for confirmation
static int s_state_slot;  // Save/Load page slot (0~9)

// Bottom-center notice
static char s_osd[80];
static SceInt64 s_osd_until;
static uint32_t s_prev_buttons;
static float s_fps;

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
	int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	int open = combo && !prev_combo;
	s_prev_buttons = buttons; // treat the current state as previous so the same press does not act inside the menu
	if (open) {
		s_page = PAGE_MAIN;
		s_sel = 0;
		s_reset_armed = 0;
	}
	return open;
}

void rei_menu_set_open(int open)
{
	s_open = open;
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
	case PAGE_ABOUT: return ABOUT_VISIBLE; // fixed row count (scrolls)
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

// Repeat while a direction is held (after ~0.4s, every 0.07s; call once per screen refresh). Adds repeats to pressed
#define REPEAT_KEYS (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)
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

// Change a value (left/right dir = -1/+1, confirm = +1). Display settings apply on the next composite
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
		}
		break;
	case PAGE_CONTROLS:
	{
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_ACT) // buttons 3~6 also allow None (unassigned)
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
		case M_GAME_SAVE: // this game only (global settings file is not written)
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
		if ((pressed & PSP_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & PSP_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~(PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
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
	// Confirm/back buttons follow the setting (X/O). Uses the setting at press time even if it changes during this input pass
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS, back = o_ok ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = REI_MENU_CLOSE;
		else
			go_page(PAGE_MAIN);
	}
	if (pressed & PSP_CTRL_START)
		r = REI_MENU_CLOSE;

	if (r != REI_MENU_STAY)
		rei_settings_save();
	return r;
}

// Button symbol glyphs (video.c font_init, drawn in button colors: text_keys)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"
static void text_keys(int x, int y, uint32_t col, const char *s);

// Button names: ○×□△ are symbol glyphs, the rest (L, R, SELECT, START) are text
static const char *key_label(int key)
{
	switch (key) {
	case REI_KEY_CROSS: return G_CRO;
	case REI_KEY_CIRCLE: return G_CIR;
	case REI_KEY_SQUARE: return G_SQU;
	case REI_KEY_TRIANGLE: return G_TRI;
	default: return rei_key_name(key);
	}
}

// One row: name on the left, optional "< value >" on the right (button symbols inside the value in button colors)
static void item(int px, int ty, int i, const char *name, const char *value)
{
	const uint32_t col = (i == s_sel) ? COL_SEL : COL_TEXT;
	if (i == s_sel)
		video_text(px + 8, ty, COL_SEL, 1, ">");
	text_keys(px + 20, ty, col, name);
	if (value) {
		char buf[64];
		snprintf(buf, sizeof(buf), "< %s >", value);
		text_keys(px + VALUE_X, ty, col, buf);
	}
}

static void draw_menu(void)
{
	static const char *const aspect[] = { "4:3", "Full screen", "1:1 (320x240)" };
	static const char *const filter[] = { "Smooth", "Sharp" };
	static const char *const border[] = { "Black", "Artwork", "Color" };
	static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
	static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
	static const char *const stages[REI_STAGE_COUNT] = {
		"1 Seattle", "2 San Francisco", "3 Las Vegas", "4 Texas", "5 Florida", "6 Chicago", "Final New York",
	};
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR; // (button symbols, drawn with text_keys)
	char foot_buf[96];

	int n = page_items();
	if (s_page == PAGE_CHEATS) { // scroll to keep the cursor visible when there are many rows
		if (s_sel < s_cheat_top) s_cheat_top = s_sel;
		if (s_sel >= s_cheat_top + CHEAT_VISIBLE) s_cheat_top = s_sel - CHEAT_VISIBLE + 1;
		if (n > CHEAT_VISIBLE) n = CHEAT_VISIBLE;
	}
	const int h = TITLE_H + n * LINE_H + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	video_rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
	video_rect(px, py, PANEL_W, h, COL_PANEL);

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select  Left/Right/%s: change  %s: back", ok_name, back_name);
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
	video_text(px + 12, py + 8, COL_TEXT, 1, s_page == PAGE_MAIN && !s_list_mode ? rei_core_game_name() : title);

	for (int i = 0; i < n; i++) {
		const int ty = py + TITLE_H + i * LINE_H + 2;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = {
				"Resume game", "Reset game", "Save / Load state  >", "Display  >", "Controls  >", "DIP switches  >",
				"Cheats  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit", "Back",
			};
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[48];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", ok_name);
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
				"Show FPS", "Screen", "Filter", "Border (4:3 / 1:1)", "Border color", "Back",
			};
			switch (i) {
			case D_FPS: v = g_settings.show_fps ? "On" : "Off"; break;
			case D_ASPECT: v = aspect[g_settings.aspect]; break;
			case D_FILTER: v = filter[g_settings.filter]; break;
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
				item(px, ty, i, act_name(act), key_label(g_settings.keymap[act]));
			else if (k == CK_TURBO_KEY)
				item(px, ty, i, "Turbo key", key_label(g_settings.turbo_key));
			else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", g_settings.turbo_btn);
				item(px, ty, i, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(px, ty, i, "Turbo speed", turbo_speed[g_settings.turbo_speed]);
			else if (k == CK_CONFIRM)
				item(px, ty, i, "Menu confirm button", o_ok ? G_CIR : G_CRO);
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
				video_text(px + 8, ty, head ? COL_SEL : (s_about[k][0] == ' ' ? COL_DIM : COL_TEXT), 1, s_about[k] + head);
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
	text_keys(px + 12, py + h - 13, s_page == PAGE_DIPS ? COL_WARN : COL_DIM, foot);
}

int rei_menu_state_slot(void)
{
	return s_state_slot;
}

void rei_osd_show(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_osd, sizeof(s_osd), fmt, ap);
	va_end(ap);
	s_osd_until = sceKernelGetSystemTimeWide() + 2000000;
}

void rei_fps_set(float fps)
{
	s_fps = fps;
}

// --- Start screen: game list, notices, loading gauge (before launching a game, drawn with video_present) ---
// Game list has one tab per board (system), in board order like Vita: L/R (or left/right) switch tabs, up/down pick a game. SELECT cycles filter
// (Playable: only games with ROMs that run on this device / All: every supported game, unrunnable ones grayed with a notice / Favorites).
// □ toggles favorite, △ is About
#define MAX_TABS 16
#define COL_GREY 0xff847470u
#define COL_GREY_SEL 0xff5080b0u
static const rei_game_found *s_gl_found; // game list (NULL = not in list)
static int s_gl_playable;
static const char *s_gl_tabs[MAX_TABS];
static int s_gl_ntabs, s_gl_tab;
static int s_gl_idx[REI_GAMES_MAX], s_gl_cnt, s_gl_sel; // game indices of the current tab, cursor
static int s_gl_nvis;      // games in the current filter (all tabs)
static const char *const s_gl_filter_name[REI_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };
static int s_gl_about;     // viewing About
static char s_gl_msg[256]; // notice box (empty = none)
static const char *s_msg_title, *s_msg_body; // notice screen (NULL = none)
static int s_loading = -1; // loading gauge 0~1000 (-1 = none)
static char s_loading_title[64]; // game name on the loading screen (empty = "Loading...")

void rei_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
}

static int playable(const rei_game_found *g) { return g->present && g->supported; }

// Game passes the current filter
static int gl_visible(const rei_game_found *g)
{
	if (g_settings.list_filter == REI_LIST_ALL)
		return 1;
	if (g_settings.list_filter == REI_LIST_FAVORITES)
		return rei_fav_is(rei_core_game_set(g->game));
	return playable(g);
}

// One line of text: symbol glyphs (0x10~0x13) in button colors, the rest in col
static void text_keys(int x, int y, uint32_t col, const char *s)
{
	char run[96];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char c = (unsigned char)*p;
		if (c >= 0x10 && c <= 0x13) {
			run[k] = 0;
			x += video_text(x, y, col, 1, run);
			k = 0;
			static const uint32_t kc[4] = { 0xffc0e050u, 0xff6e6effu, 0xfff0b47cu, 0xffd8a0f0u }; // △ ○ × □ (ABGR)
			const char g[2] = { (char)c, 0 };
			x += video_text(x, y, kc[c - 0x10], 1, g);
			continue;
		}
		if (!c || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			x += video_text(x, y, col, 1, run);
			k = 0;
			if (!c)
				break;
		}
		run[k++] = (char)c;
	}
}

static void draw_start_screens(void)
{
	if (s_loading >= 0) {
		const int w = 320, h = 10, x = (SCREEN_W - w) / 2, y = SCREEN_H / 2;
		video_rect(0, 0, SCREEN_W, SCREEN_H, 0xff000000u);
		video_text(x, y - 16, COL_TEXT, 1, s_loading_title[0] ? s_loading_title : "Loading...");
		video_rect(x - 1, y - 1, w + 2, h + 2, COL_DIM);
		video_rect(x, y, w, h, COL_PANEL);
		video_rect(x, y, w * s_loading / 1000, h, COL_SEL);
		return;
	}
	const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
	if (s_gl_found) {
		// Scroll to keep the cursor visible if it does not fit (row count is fixed regardless of tab so the panel does not jump when switching)
		const int rows = 15, vis = s_gl_cnt < rows ? s_gl_cnt : rows;
		static int top;
		if (s_gl_sel < top) top = s_gl_sel;
		if (s_gl_sel >= top + vis) top = s_gl_sel - vis + 1;
		if (top > s_gl_cnt - vis) top = s_gl_cnt - vis;
		if (top < 0) top = 0;
		const int tab_h = 16;
		const int h = TITLE_H + tab_h + rows * LINE_H + FOOT_H + 10; // (two footer lines)
		const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
		video_rect(0, 0, SCREEN_W, SCREEN_H, 0xff000000u);
		video_rect(px, py, PANEL_W, h, COL_PANEL);
		if (s_gl_about) {
			video_text(px + 12, py + 8, COL_TEXT, 1, "ABOUT");
			for (int i = 0; i < rows + 1 && s_about_scroll + i < ABOUT_LINES; i++) {
				const char *t = s_about[s_about_scroll + i];
				if (!t[0])
					continue;
				const int head = t[0] == '#';
				video_text(px + 8, py + TITLE_H + i * LINE_H, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT), 1, t + head);
			}
			text_keys(px + 12, py + h - 13, COL_DIM, o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			return;
		}
		char title[48];
		snprintf(title, sizeof(title), "REIREM - SELECT GAME (%d/%d)", s_gl_cnt ? s_gl_sel + 1 : 0, s_gl_cnt);
		video_text(px + 12, py + 8, COL_TEXT, 1, title);
		char have[32];
		snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_settings.list_filter], s_gl_nvis);
		video_text(px + PANEL_W - 12 - (int)strlen(have) * 8, py + 8, COL_DIM, 1, have);
		{ // tab row: current tab highlighted, shifted to stay visible on overflow
			int first = 0, total = 0;
			for (int t = 0; t < s_gl_ntabs; t++)
				total += (int)strlen(s_gl_tabs[t]) * 8 + 16;
			while (total > PANEL_W - 24 && first < s_gl_tab) {
				total -= (int)strlen(s_gl_tabs[first]) * 8 + 16;
				first++;
			}
			int tx = px + 10;
			for (int t = first; t < s_gl_ntabs; t++) {
				const int tw = (int)strlen(s_gl_tabs[t]) * 8 + 10;
				if (tx + tw > px + PANEL_W - 10)
					break;
				if (t == s_gl_tab)
					video_rect(tx, py + TITLE_H - 3, tw, 13, COL_SEL);
				video_text(tx + 5, py + TITLE_H, t == s_gl_tab ? COL_PANEL : COL_DIM, 1, s_gl_tabs[t]);
				tx += tw + 6;
			}
		}
		if (!s_gl_cnt && g_settings.list_filter == REI_LIST_FAVORITES)
			text_keys(px + 26, py + TITLE_H + tab_h + 2, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
		else if (!s_gl_cnt)
			video_text(px + 26, py + TITLE_H + tab_h + 2, COL_DIM, 1, "No playable games. SELECT: show all");
		for (int i = top; i < top + vis; i++) {
			const int ty = py + TITLE_H + tab_h + (i - top) * LINE_H + 2;
			const rei_game_found *g = &s_gl_found[s_gl_idx[i]];
			const int ok_g = playable(g);
			const uint32_t col = (i == s_gl_sel) ? (ok_g ? COL_SEL : COL_GREY_SEL) : (ok_g ? COL_TEXT : COL_GREY);
			if (i == s_gl_sel)
				video_text(px + 8, ty, col, 1, ">");
			if (rei_fav_is(rei_core_game_set(g->game))) // favorite mark
				video_text(px + 17, ty, COL_WARN, 1, "*");
			video_text(px + 26, ty, col, 1, rei_core_game_title(g->game));
		}
		char foot[96];
		snprintf(foot, sizeof(foot), "%s%s play  " G_TRI " about", s_gl_ntabs > 1 ? "L/R system  <> page  " : "<> page  ",
			o_ok ? G_CIR : G_CRO);
		text_keys(px + 12, py + h - 23, COL_DIM, foot);
		snprintf(foot, sizeof(foot), "START options  " G_SQU " favorite  SELECT %s",
			s_gl_filter_name[wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT)]);
		text_keys(px + 12, py + h - 13, COL_DIM, foot);
		if (s_gl_msg[0]) { // notice box (over the list)
			const int mw = 440, mh = 64, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
			video_rect(mx - 1, my - 1, mw + 2, mh + 2, COL_WARN);
			video_rect(mx, my, mw, mh, COL_PANEL);
			char line[256];
			snprintf(line, sizeof(line), "%s", s_gl_msg);
			char *nl = strchr(line, '\n');
			if (nl)
				*nl = 0;
			video_text(mx + 8, my + 8, COL_WARN, 1, line);
			if (nl)
				video_text(mx + 8, my + 24, COL_TEXT, 1, nl + 1);
			video_text(mx + 8, my + mh - 14, COL_DIM, 1, "Press any button");
		}
	}
	if (s_msg_title) {
		video_rect(0, 0, SCREEN_W, SCREEN_H, 0xff000000u);
		video_text(16, 40, COL_WARN, 1, s_msg_title);
		video_text(16, 64, COL_TEXT, 1, s_msg_body);
		video_text(16, SCREEN_H - 24, COL_DIM, 1, "START: exit");
	}
}

static uint32_t read_pad(void)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);
	uint32_t b = pad.Buttons;
	if (pad.Ly < 128 - 48) b |= PSP_CTRL_UP;
	if (pad.Ly > 128 + 48) b |= PSP_CTRL_DOWN;
	if (pad.Lx < 128 - 48) b |= PSP_CTRL_LEFT;
	if (pad.Lx > 128 + 48) b |= PSP_CTRL_RIGHT;
	return b;
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
	s_gl_playable = 0;
	for (int i = 0; i < n; i++)
		s_gl_playable += playable(&found[i]);
	if (!s_gl_playable && g_settings.list_filter == REI_LIST_PLAYABLE)
		g_settings.list_filter = REI_LIST_ALL; // (show everything if no game can run)
	s_gl_about = 0;
	s_gl_msg[0] = 0;
	s_gl_ntabs = s_gl_tab = 0;
	gl_build(found, n, initial);
	uint32_t prev = 0xffffffffu; // ignore until released once so the press that launched the app does not select
	for (;;) {
		s_gl_found = found;
		static int rep_count;
		const uint32_t b = read_pad(), pressed = key_repeat(b, b & ~prev, &rep_count);
		prev = b;
		const int o_ok = g_settings.menu_confirm == REI_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS, back = o_ok ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls, global settings; rei_menu_update saves on close)
			if (rei_menu_update(b) != REI_MENU_STAY)
				s_list_mode = 0;
		} else if (s_gl_msg[0]) { // notice box: any button closes it
			if (pressed & (ok | back | PSP_CTRL_START | PSP_CTRL_TRIANGLE))
				s_gl_msg[0] = 0;
		} else if (s_gl_about) {
			const int max = ABOUT_LINES > 16 ? ABOUT_LINES - 16 : 0;
			if ((pressed & PSP_CTRL_UP) && s_about_scroll > 0) s_about_scroll--;
			if ((pressed & PSP_CTRL_DOWN) && s_about_scroll < max) s_about_scroll++;
			if (pressed & (ok | back | PSP_CTRL_TRIANGLE | PSP_CTRL_START))
				s_gl_about = 0;
		} else {
			if (s_gl_cnt && (pressed & PSP_CTRL_UP)) s_gl_sel = wrap(s_gl_sel - 1, s_gl_cnt);
			if (s_gl_cnt && (pressed & PSP_CTRL_DOWN)) s_gl_sel = wrap(s_gl_sel + 1, s_gl_cnt);
			// Left/right: page by screen, L/R: board (tab)
			if (s_gl_cnt && (pressed & PSP_CTRL_LEFT)) s_gl_sel = s_gl_sel >= 15 ? s_gl_sel - 15 : 0;
			if (s_gl_cnt && (pressed & PSP_CTRL_RIGHT)) s_gl_sel = s_gl_sel + 15 < s_gl_cnt ? s_gl_sel + 15 : s_gl_cnt - 1;
			if (s_gl_ntabs > 1 && (pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & PSP_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				const char *tb = s_gl_tabs[s_gl_tab];
				int first = -1; // first game of that tab
				for (int i = 0; i < n && first < 0; i++)
					if (gl_visible(&found[i]) && !strcmp(rei_core_game_board(found[i].game), tb))
						first = i;
				gl_build(found, n, first);
				continue;
			}
			if (pressed & PSP_CTRL_SELECT) { // filter: Playable -> All -> Favorites (keep the current game if visible)
				g_settings.list_filter = wrap(g_settings.list_filter + 1, REI_LIST_FILTER_COUNT);
				rei_settings_save();
				gl_build(found, n, s_gl_cnt ? s_gl_idx[s_gl_sel] : -1);
				continue;
			}
			if (s_gl_cnt && (pressed & PSP_CTRL_SQUARE)) { // favorite on/off (in Favorites the game leaves: cursor to the next, or previous at the end)
				const int cur = s_gl_idx[s_gl_sel];
				rei_fav_toggle(rei_core_game_set(found[cur].game));
				int want = cur;
				if (g_settings.list_filter == REI_LIST_FAVORITES && !gl_visible(&found[cur]))
					want = s_gl_sel + 1 < s_gl_cnt ? s_gl_idx[s_gl_sel + 1] : (s_gl_sel > 0 ? s_gl_idx[s_gl_sel - 1] : -1);
				gl_build(found, n, want);
				continue;
			}
			if (pressed & PSP_CTRL_TRIANGLE) {
				s_gl_about = 1;
				s_about_scroll = 0;
			} else if (pressed & PSP_CTRL_START) { // options: edit and save the global settings only
				rei_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_prev_buttons = b; // (the START press does not act inside the options)
			}
			if (s_gl_cnt && (pressed & ok) && !s_list_mode) {
				const rei_game_found *g = &found[s_gl_idx[s_gl_sel]];
				if (playable(g))
					break;
				const char *par = rei_core_game_parent(g->game);
				if (!g->supported)
					snprintf(s_gl_msg, sizeof(s_gl_msg), "%s board is not supported on PSP yet\n(Vita version runs it)",
						rei_core_game_board(g->game));
				else
					snprintf(s_gl_msg, sizeof(s_gl_msg), "ROM not found: %s.zip%s%s%s\nPut it in %s", rei_core_game_set(g->game),
						par ? " (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
			}
		}
		video_present();
		sceDisplayWaitVblankStart();
	}
	s_gl_found = NULL;
	return s_gl_idx[s_gl_sel];
}

// Game loading gauge (rei_core_config.progress etc., 0~1000). Not drawn too often (each draw waits for vblank)
void rei_loading_progress(int permille)
{
	static uint32_t last_t;
	static int last_p = -1000;
	const uint32_t now = sceKernelGetSystemTimeLow();
	if (permille > 0 && permille < 1000 && permille - last_p < 20 && now - last_t < 100000)
		return;
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	last_p = permille;
	last_t = now;
	s_loading = permille;
	video_present();
}

void rei_loading_done(void)
{
	s_loading = -1;
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
#ifdef REI_LOG_ENABLED
		// Dev loop: also close on stop request (host0:/rei_stop); unattended runs got stuck on this screen (2026-10-13)
		SceIoStat st;
		if (n % 30 == 29 && sceIoGetstat("host0:/rei_stop", &st) >= 0)
			break;
#endif
		video_present();
		sceDisplayWaitVblankStart();
	}
	s_msg_title = NULL;
}

void rei_menu_overlay(void)
{
	if (s_gl_found || s_msg_title || s_loading >= 0) {
		draw_start_screens();
		if (s_gl_found && s_list_mode) // game list options over the list
			draw_menu();
		return;
	}
	if (g_settings.show_fps && !s_open) {
		char buf[24];
		const int f10 = (int)(s_fps * 10.0f + 0.5f);
		snprintf(buf, sizeof(buf), "%d.%d FPS", f10 / 10, f10 % 10);
		video_rect(SCREEN_W - 84, 2, 82, 12, 0xa0000000u);
		video_text(SCREEN_W - 80, 4, s_fps >= 59.0f ? 0xff60ff60u : 0xff40c0ffu, 1, buf);
	}
	if (s_open)
		draw_menu();
	if (s_osd[0] && sceKernelGetSystemTimeWide() < s_osd_until) {
		int len = 0;
		while (s_osd[len])
			len++;
		const int w = len * 8 + 24, hh = 18;
		const int x = (SCREEN_W - w) / 2, y = SCREEN_H - hh - 8;
		video_rect(x, y, w, hh, 0xc8000000u);
		video_rect(x, y, w, 1, COL_SEL);
		video_text(x + 12, y + 6, COL_TEXT, 1, s_osd);
	}
}
