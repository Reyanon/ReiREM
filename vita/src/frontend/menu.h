#ifndef REI_FRONTEND_MENU_H
#define REI_FRONTEND_MENU_H

#include <stdint.h>

#include "rei_games.h"

// In-game menu, game list, loading screen, notices, FPS box, touch buttons and the PS button. Ported from ReSys12
// vita/src/frontend/menu.c (common UI of the Re series of Vita emulators: same layout, controls, animations); drawn
// with ui.c on overlay layers (video.h). The in-game menu opens with SELECT+START or PS; the game pauses while it is open.

void rei_menu_init(void);
void rei_menu_fini(void);
// the running game (after rei_core_init): set name, title, game buttons (2..6)
void rei_menu_set_game(const char *set, const char *title, int buttons);

// Game list (all supported games, ROM-less ones grey; board tabs, SELECT filter, Square favorite (star), START or PS
// options (Display, Controls, Refresh ROM list; global settings), Triangle About; a newer release is offered once:
// Update / Close / Skip). Scans dirs for the ROM zips; the cursor starts on g_settings.last_game. notice: shown in a box
// first (NULL = none). Returns the picked game (a playable one; the pointer stays valid until the next call)
const rei_game_found *rei_game_select(const char *const *dirs, int ndirs, const char *rom_dir, const char *notice);

// Loading screen: the app's background, game name, stage, moving bar. The loader thread sets the progress
// (rei_loading_set, 0-1000, any thread); the main thread draws one frame per vblank (rei_loading_frame)
void rei_loading_title(const char *title);
void rei_loading_set(int permille);
void rei_loading_frame(void);
void rei_loading_progress(int permille); // set and draw at once
void rei_loading_done(void);

// 1 at the moment SELECT+START or PS is pressed (called every frame in the game)
int rei_menu_should_open(uint32_t buttons);

typedef enum {
	REI_MENU_STAY,        // stay in menu
	REI_MENU_CLOSE,       // return to game
	REI_MENU_EXIT,        // exit app
	REI_MENU_SELECT_GAME, // to the game list (same process)
	REI_MENU_RESET,       // reset game, then return
	REI_MENU_SAVE,        // save to rei_menu_state_slot() slot (stay in menu)
	REI_MENU_LOAD,        // load from rei_menu_state_slot() slot, then return
	REI_MENU_REFRESH,     // game list options: read the ROM folders again
} rei_menu_result;

// Called every frame while the menu is open: input handling (buttons = SceCtrlData.buttons)
rei_menu_result rei_menu_update(uint32_t buttons);
// draws the menu canvas when something changed (the overlay layers are drawn by rei_video_draw_layers)
void rei_menu_draw(void);
void rei_menu_hide(void);
// the theme's accent colour (0xAABBGGRR): "iREM" in the header, the app's blank artwork is tinted with it
uint32_t rei_menu_accent(void);
// Slot chosen on the Save/Load page (0~9)
int rei_menu_state_slot(void);
void rei_menu_states_changed(void); // a state was saved: read the slots again

// Status message at bottom center (~2s). Shown on top even while the menu is open
void rei_osd_show(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rei_osd_tick(void); // every frame (hides it after 2 s)

// FPS box in the game picture's top-right corner (when the setting is on): once per presented game frame
void rei_fps_tick(void);
void rei_fps_hide(void); // while the menu is open

// touch buttons over the game (redrawn only when the layout or the settings change); hidden = 1 while the menu is open
void rei_touch_overlay_update(int hidden);

// kernel helper for the PS button and the update install (module/rei_kctrl.skprx; may restart the app once after boot:
// restarted = the app was started again for it, argument "kctrl")
void rei_kctrl_init(int restarted);
int rei_kctrl_ready(void);
void rei_ps_release(void); // before exit: the PS button back to the system

#endif
