#ifndef REI_PSP_MENU_H
#define REI_PSP_MENU_H

#include <stdint.h>

// In-game menu (SELECT+START), game list, loading screen, notices and the FPS box. Same layout, controls and animations as the
// Vita version (vita/src/frontend/menu.c, the ReSys12 UI shared by the Re series) at half the size, drawn with ui.c in the
// overlay stage of the screen composition (video_set_overlay(rei_menu_overlay)). The game pauses while the menu is open.

typedef enum {
	REI_MENU_STAY,  // keep open
	REI_MENU_CLOSE, // close and return to game
	REI_MENU_RESET, // reset game, then close
	REI_MENU_SAVE,  // save to selected slot (stay open)
	REI_MENU_LOAD,  // load selected slot, then close
	REI_MENU_EXIT,  // exit app
	REI_MENU_SELECT_GAME, // back to the game list (same process)
} rei_menu_result;

// save state slots: 0-9, quick save (REI_STATE_QUICK). Files are <set name>_<slot>.sav in the ROM folder
#define REI_STATE_SLOTS 10
#define REI_STATE_QUICK (-1)
void rei_state_path(int slot, char *buf, int len);
int rei_state_exists(int slot);
const char *rei_state_slot_name(int slot);

// decide from this frame's buttons whether to open (moment SELECT+START is pressed)
int rei_menu_should_open(uint32_t buttons);
rei_menu_result rei_menu_update(uint32_t buttons);
void rei_menu_set_open(int open);
int rei_menu_state_slot(void);
// the theme's accent colour (0xAABBGGRR): "iREM" in the header, the app's blank border artwork is tinted with it
uint32_t rei_menu_accent(void);

#include "rei_games.h"
// Game list (all rei_games_scan results; board tabs with L/R, SELECT filter Playable / All / Favorites, Square favourite (star),
// Triangle About, START options: Display, Controls, Refresh ROM list - global settings). Games without ROM are grey and show a
// notice if picked. Returns the picked index (a game with ROM), REI_LIST_STOP (dev loop stop request) or REI_LIST_REFRESH
// (Refresh ROM list: scan again and call again; rei_game_select_cursor = the game under the cursor)
#define REI_LIST_STOP (-1)
#define REI_LIST_REFRESH (-2)
#define REI_LIST_QUIT (-4)
int rei_game_select(const rei_game_found *found, int n, int initial, const char *rom_dir);
// called every list frame (HOME exit, power switch while the list is up); nonzero ends the list with REI_LIST_QUIT
void rei_game_select_set_poll(int (*poll)(void));
int rei_game_select_cursor(void);
// Loading screen: the app's background, game name, stage with its percentage, spinner, moving bar. The loader thread sets the
// progress (rei_loading_set, 0-1000, any thread); the main thread draws one frame per vblank (rei_loading_frame).
// rei_loading_progress = set and draw at once (main thread), rei_loading_done when finished
void rei_loading_title(const char *title); // game name shown on the loading screen ("Loading..." if NULL)
void rei_loading_set(int permille);
void rei_loading_frame(void);
void rei_loading_progress(int permille);
void rei_loading_done(void);
// notice screen (missing ROM etc.). Until START is pressed
void rei_message_screen(const char *title, const char *body);

// last stage of screen compositing (video_set_overlay): FPS, notices, menu if open
void rei_menu_overlay(void);

// notice at bottom center of the screen (2 s)
void rei_osd_show(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// FPS display value (main loop updates every 0.5 s)
void rei_fps_set(float fps);
#ifdef REI_LOG_ENABLED
uint32_t rei_test_press(void); // (log builds) remote key press from host0:/rei_press (hex mask), once per call per frame
#endif

#endif
