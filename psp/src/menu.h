#ifndef REI_PSP_MENU_H
#define REI_PSP_MENU_H

#include <stdint.h>

// options screen (SELECT+START). Same pages/items as Vita vita/src/frontend/menu.c, drawn with the GE font (video_text)

typedef enum {
	REI_MENU_STAY,  // keep open
	REI_MENU_CLOSE, // close and return to game
	REI_MENU_RESET, // reset game, then close
	REI_MENU_SAVE,  // save to selected slot (stay open)
	REI_MENU_LOAD,  // load selected slot, then close
	REI_MENU_EXIT,  // exit app
	REI_MENU_SELECT_GAME, // back to game list (relaunches the app)
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

#include "rei_games.h"
// game list (all rei_games_scan results; games without ROM greyed out with a notice if picked, △ About, START global options). Returns picked index (game with ROM)
int rei_game_select(const rei_game_found *found, int n, int initial, const char *rom_dir);
// game loading gauge (0-1000), rei_loading_done when finished
void rei_loading_progress(int permille);
void rei_loading_title(const char *title); // game name shown on the loading screen ("Loading..." if NULL)
void rei_loading_done(void);
// notice screen (missing ROM etc.). Until START is pressed
void rei_message_screen(const char *title, const char *body);

// last stage of screen compositing (video_set_overlay): FPS, notices, menu if open
void rei_menu_overlay(void);

// notice at bottom center of the screen (2 s)
void rei_osd_show(const char *fmt, ...);
// FPS display value (main loop updates every 0.5 s)
void rei_fps_set(float fps);

#endif
