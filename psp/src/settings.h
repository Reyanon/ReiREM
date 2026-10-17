#ifndef REI_PSP_SETTINGS_H
#define REI_PSP_SETTINGS_H

#include <stdint.h>

// User settings (changed in the options screen, saved to settings.cfg in the ROM folder). Same items as the Vita version (vita/src/frontend/settings.h),
// only aspect ratios adapted to the PSP screen (480x272)

enum {
	REI_ASPECT_4_3,      // 4:3 fit to 272 height (362x272, default)
	REI_ASPECT_FULL,     // stretched to 480x272
	REI_ASPECT_1X,       // original size (320x240, centered)
	REI_ASPECT_COUNT
};

enum {
	REI_FILTER_LINEAR,   // smooth (default)
	REI_FILTER_POINT,    // sharp
	REI_FILTER_COUNT
};

// Margin outside the game screen (4:3, 1:1). Artwork is a user image <app dir>artwork/<set>.png·.jpg·.bmp (video_load_art)
enum { REI_BORDER_BLACK, REI_BORDER_ARTWORK, REI_BORDER_COLOR, REI_BORDER_COUNT };
enum { REI_BCOL_NAVY, REI_BCOL_GRAY, REI_BCOL_MAROON, REI_BCOL_GREEN, REI_BCOL_PURPLE, REI_BCOL_BRICK, REI_BCOL_COUNT };

// Options screen confirm button (the other one is back)
enum { REI_CONFIRM_CROSS, REI_CONFIRM_CIRCLE, REI_CONFIRM_COUNT };

// Mappable game buttons
enum {
	REI_ACT_ATTACK,      // game button 1 (nbbatman attack)
	REI_ACT_JUMP,        // game button 2 (nbbatman jump)
	REI_ACT_COIN,
	REI_ACT_START,
	REI_ACT_B3, REI_ACT_B4, REI_ACT_B5, REI_ACT_B6, // game buttons 3-6 (shown in the menu only when the game uses them, REI_KEY_NONE allowed)
	REI_ACT_COUNT
};

// Selectable PSP buttons (cycled left/right in the options screen)
enum {
	REI_KEY_CROSS, REI_KEY_CIRCLE, REI_KEY_SQUARE, REI_KEY_TRIANGLE,
	REI_KEY_L, REI_KEY_R, REI_KEY_SELECT, REI_KEY_START,
	REI_KEY_COUNT
};

// Turbo: one PSP button (REI_KEY_NONE = off) fires one game button (1..REI_TURBO_BUTTONS)
#define REI_KEY_NONE REI_KEY_COUNT
#define REI_TURBO_BUTTONS 6
enum { REI_TURBO_FAST, REI_TURBO_MEDIUM, REI_TURBO_SLOW, REI_TURBO_SPEED_COUNT };

#define REI_DIP_BYTES_MAX 8

// Game list filter (SELECT cycles in this order)
enum { REI_LIST_PLAYABLE, REI_LIST_ALL, REI_LIST_FAVORITES, REI_LIST_FILTER_COUNT };

typedef struct {
	int show_fps;
	int aspect;
	int filter;
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
	int show_list;                      // show the list on next start even with one game (options screen Select game)
} rei_settings;

extern rei_settings g_settings;

// dir: app folder ("ms0:/PSP/GAME/REIREM/" etc., ends with '/'). Settings (settings.cfg, per-game <set>.cfg) and save states go here
void rei_settings_set_dir(const char *dir);
const char *rei_settings_dir(void);
void rei_settings_load(void);
void rei_settings_save(void);
// After picking a game: its DIPs (if none, nbbatman family uses the old setting value, other games driver defaults)
void rei_settings_load_game(const char *set);

// Per-game settings (<set>_game.cfg: screen, filter, margin, FPS display, button mapping, turbo). If present, rei_settings_load_game
// overlays them on the global settings. While active, rei_settings_save writes those items only to the per-game file and keeps global values in the global file
int rei_settings_has_game(void);     // current game has per-game settings
int rei_settings_save_game(void);    // save current values for this game only (global file untouched). 1 on success
int rei_settings_delete_game(void);  // delete per-game settings and revert to global values. 1 on success
// Game list options: drop the current game's settings (if any) so menus edit and rei_settings_save writes only the global
// values. The next rei_settings_load_game applies that game's file again
void rei_settings_use_global(void);

// Favorite games (favorites.cfg next to settings.cfg, one set name per line; loaded by rei_settings_load)
int rei_fav_is(const char *set);
void rei_fav_toggle(const char *set); // add or remove, then save

// Default button mapping (attack □, jump ×, coin SELECT, start START, turbo off)
void rei_settings_default_keys(void);

// Frames per turbo cycle (half pressed, half released)
int rei_turbo_period(void);
// REI_ACT_* of the game button fired by turbo (turbo_btn)
int rei_turbo_act(void);

// REI_KEY_* → PSP_CTRL_* bit, display name (REI_KEY_NONE is 0, "None")
uint32_t rei_key_mask(int key);
const char *rei_key_name(int key);

#endif
