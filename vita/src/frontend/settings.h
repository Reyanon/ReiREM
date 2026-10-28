#ifndef REI_FRONTEND_SETTINGS_H
#define REI_FRONTEND_SETTINGS_H

#include <stdint.h>

// User settings (changed in the options screen, saved to ux0:data/reirem/settings.cfg; per-game DIP in <set>.cfg)

enum {
	REI_ASPECT_4_3,      // 4:3 fitted to 544 height (default)
	REI_ASPECT_FULL,     // stretched to 960x544
	REI_ASPECT_INT2X,    // integer 2x (640x480)
	REI_ASPECT_COUNT
};

enum {
	REI_FILTER_LINEAR,   // smooth (default)
	REI_FILTER_POINT,    // sharp
	REI_FILTER_COUNT
};

// Scanline types. CRT types follow game lines (240), PX types draw evenly at screen-pixel spacing
enum {
	REI_SCAN_OFF,
	REI_SCAN_CRT_SOFT,   // darken game line borders with a soft curve (linear filter)
	REI_SCAN_CRT_SHARP,  // darken the lower half of each game line
	REI_SCAN_PX2,        // 1 line every 2 screen pixels
	REI_SCAN_PX3,        // 1 line every 3 pixels
	REI_SCAN_PX4,        // 1 line every 4 pixels
	REI_SCAN_COUNT
};

enum { REI_SCAN_LIGHT, REI_SCAN_MEDIUM, REI_SCAN_HEAVY, REI_SCAN_STRENGTH_COUNT };

// Margin outside the game screen (4:3 / Integer 2x). Artwork: the user's artwork/<set>.png, else the app's blank artwork
enum { REI_BORDER_BLACK, REI_BORDER_ARTWORK, REI_BORDER_COLOR, REI_BORDER_COUNT };
// Screen rotation: Auto = vertical games upright on the landscape screen; Off = the game's buffer as it is; Vertical L / R =
// upright with the Vita turned left (right edge up) / right (left edge up), directions turned with it
enum { REI_ROT_AUTO, REI_ROT_OFF, REI_ROT_VL, REI_ROT_VR, REI_ROT_COUNT };
// Menu colour theme (global; Graphite is the ReiREM default, Red ReSys12's, Blue ReST-V's)
enum { REI_THEME_RED, REI_THEME_BLUE, REI_THEME_GREEN, REI_THEME_PURPLE, REI_THEME_ORANGE, REI_THEME_GRAPHITE, REI_THEME_COUNT };
// Analog stick used as the joystick
enum { REI_STICK_OFF, REI_STICK_LEFT, REI_STICK_RIGHT, REI_STICK_BOTH, REI_STICK_COUNT };
enum { REI_BCOL_NAVY, REI_BCOL_GRAY, REI_BCOL_MAROON, REI_BCOL_GREEN, REI_BCOL_PURPLE, REI_BCOL_BRICK, REI_BCOL_COUNT };

// Options screen confirm button (the other one is back)
enum { REI_CONFIRM_CROSS, REI_CONFIRM_CIRCLE, REI_CONFIRM_COUNT };

// Mappable game buttons
enum {
	REI_ACT_ATTACK,      // game button 1 (nbbatman attack)
	REI_ACT_JUMP,        // game button 2 (nbbatman jump)
	REI_ACT_COIN,
	REI_ACT_START,
	REI_ACT_B3, REI_ACT_B4, REI_ACT_B5, REI_ACT_B6, // game buttons 3-6 (shown in the menu only if the game uses them, REI_KEY_NONE allowed)
	REI_ACT_COUNT
};

// Selectable Vita buttons (cycled left/right in the options screen)
enum {
	REI_KEY_CROSS, REI_KEY_CIRCLE, REI_KEY_SQUARE, REI_KEY_TRIANGLE,
	REI_KEY_L, REI_KEY_R, REI_KEY_SELECT, REI_KEY_START,
	REI_KEY_COUNT
};

// Turbo: one Vita button (REI_KEY_NONE = off) fires one game button (1..REI_TURBO_BUTTONS)
#define REI_KEY_NONE REI_KEY_COUNT
// Combo keys: a Vita button (Cross, Circle, Square, Triangle, L, R = REI_KEY_CROSS..REI_KEY_R) can act as two other Vita
// buttons pressed together. combo[key] = bit mask (1 << REI_KEY_*) of those two, 0 = normal
#define REI_COMBO_KEYS 6
#define REI_TURBO_BUTTONS 6
enum { REI_TURBO_FAST, REI_TURBO_MEDIUM, REI_TURBO_SLOW, REI_TURBO_SPEED_COUNT };

#define REI_DIP_BYTES_MAX 8

// Game list filter (SELECT cycles in this order)
enum { REI_LIST_PLAYABLE, REI_LIST_ALL, REI_LIST_FAVORITES, REI_LIST_FILTER_COUNT };

typedef struct {
	int show_fps;
	int aspect;
	int filter;
	int scan_type;                      // REI_SCAN_*
	int scan_strength;                  // REI_SCAN_LIGHT..HEAVY
	int border;                         // REI_BORDER_*
	int border_color;                   // REI_BCOL_*
	int menu_confirm;                   // REI_CONFIRM_*
	int start_stage;                    // 0 = Stage 1 ~ 6 = Final (rei_core_set_start_stage)
	int keymap[REI_ACT_COUNT];          // REI_KEY_*
	int turbo_key;                      // REI_KEY_* or REI_KEY_NONE (off)
	int turbo_btn;                      // game button fired by turbo, 1..REI_TURBO_BUTTONS
	int turbo_speed;                   // REI_TURBO_FAST..SLOW
	int cheat_health;                   // 1P infinite health
	int dip_count;                      // number of saved DIP bytes (0 = driver defaults). Per-game file <set>.cfg
	uint8_t dip[REI_DIP_BYTES_MAX];
	char last_game[16];                 // set name of the last game played (game list cursor)
	int list_filter;                    // game list filter: REI_LIST_*
	int show_list;                      // show the list next start even with one game (options screen Select game)
	int rotate;                         // REI_ROT_*
	int stick;                          // REI_STICK_*
	int combo[REI_COMBO_KEYS];          // combo keys (see REI_COMBO_KEYS)
	int touch_keys;                     // the touch screen buttons press their keys
	int touch_show;                     // the touch buttons are drawn over the game
	int theme;                          // REI_THEME_* (global only)
} rei_settings;

extern rei_settings g_settings;

// Settings folder (ux0:data/reirem/). App-wide settings.cfg, per-game DIP <set>.cfg
#define REI_DATA_DIR "ux0:data/reirem/"
#define REI_OLD_DATA_DIR "ux0:data/nbbatman/" // folder from the nbbatman-only era (settings/ROMs carried over)
#define REI_M92_DATA_DIR "ux0:data/m92arcade" // folder from the "M92 Arcade" era: renamed to REI_DATA_DIR on first start
void rei_settings_load(void);
void rei_settings_save(void);
// After picking a game: its DIP (if none, 157/247 for the nbbatman family, driver defaults for other games)
void rei_settings_load_game(const char *set);
// Per-game settings (<set>_game.cfg: display, filter, scanlines, margin, FPS, button mapping, turbo). If present, rei_settings_load_game
// overlays them on the global settings. While active, rei_settings_save writes those items only to that file and keeps global values in the global file
int rei_settings_has_game(void);     // current game has its own settings
int rei_settings_save_game(void);    // save current values as this game's settings (global file untouched). 1 on success
int rei_settings_delete_game(void);  // delete per-game settings and revert to global values. 1 on success
// Game list options: drop the current game's settings (if any) so menus edit and rei_settings_save writes only the global
// values. The next rei_settings_load_game applies that game's file again
void rei_settings_use_global(void);

// the touch button layout file that goes with the settings in use: the game's (<set>_touch.cfg, when it has its own settings)
// or the global touch.cfg (keep = 1: the layout now is written to it; else it is read). Called by the settings functions
void rei_settings_touch_follow(int keep);

// Favorite games (favorites.cfg next to settings.cfg, one set name per line; loaded by rei_settings_load)
int rei_fav_is(const char *set);
void rei_fav_toggle(const char *set); // add or remove, then save

// Default button mapping (attack □, jump △, coin SELECT, start START, turbo off)
void rei_settings_default_keys(void);

// Frames per turbo cycle (half pressed, half released)
int rei_turbo_period(void);
// REI_ACT_* of the game button fired by turbo (turbo_btn)
int rei_turbo_act(void);

// REI_KEY_* -> SCE_CTRL_* bit, display name (REI_KEY_NONE is 0, "None")
uint32_t rei_key_mask(int key);
const char *rei_key_name(int key);

#endif
