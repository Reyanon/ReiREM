#ifndef REI_FRONTEND_MENU_H
#define REI_FRONTEND_MENU_H

#include <stdint.h>

// Options screen. Opened with SELECT+START. The game pauses while it is open.
// Pages: main / Display / Controls / DIP switches. Up/down select, left/right/× change, ○ back.

void rei_menu_init(void);
// Game list at startup (all rei_games_scan results; games without ROMs grayed with a notice, △ About, START global options). Returns the chosen index (game with ROM)
#include "rei_games.h"
int rei_game_select(const rei_game_found *found, int n, int initial, const char *rom_dir);
// Game loading gauge (0~1000)
void rei_loading_progress(int permille);
void rei_loading_title(const char *title); // game name shown on the loading screen (NULL = "Loading...")
void rei_menu_fini(void);

// Whether to open the menu (SELECT+START newly pressed). Called every frame from the main loop
int rei_menu_should_open(uint32_t buttons);

typedef enum {
	REI_MENU_STAY,   // stay in menu
	REI_MENU_CLOSE,  // return to game
	REI_MENU_EXIT,   // exit app
	REI_MENU_SELECT_GAME, // to game list (relaunches the app)
	REI_MENU_RESET,  // reset game, then return
	REI_MENU_SAVE,   // save to rei_menu_state_slot() slot (stay in menu)
	REI_MENU_LOAD,   // load from rei_menu_state_slot() slot, then return
} rei_menu_result;

// Slot chosen on the Save/Load page (0~9)
int rei_menu_state_slot(void);

// Status message at bottom center (~2s). Shown on top even while the menu is open
void rei_osd_show(const char *fmt, ...);
void rei_osd_draw(void);

// Called every frame while the menu is open: input handling (buttons is SceCtrlData.buttons)
rei_menu_result rei_menu_update(uint32_t buttons);

// Call between vita2d_start_drawing() and vita2d_end_drawing()
void rei_menu_draw(void);

// FPS display (drawn only when the setting is on)
void rei_fps_tick(void);
void rei_fps_draw(void);

#endif
