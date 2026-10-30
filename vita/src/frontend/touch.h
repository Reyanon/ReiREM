// Front touch screen: on-screen key areas (any number, made in the menu's touch layout editor). Ported from ReSys12
#ifndef REI_FRONTEND_TOUCH_H
#define REI_FRONTEND_TOUCH_H

#include <stdint.h>

// keys an area can press: REI_KEY_* (Cross .. Start), then the d-pad
enum { REI_TKEY_UP = 8, REI_TKEY_DOWN, REI_TKEY_LEFT, REI_TKEY_RIGHT, REI_TKEY_COUNT };
// a touch button can also press two keys together (its own, apart from Controls > Combo keys):
// key = REI_TKEY_COMBO + bit mask (1 << REI_KEY_*) of the two (as in ReST-V)
#define REI_TKEY_COMBO 256
#define REI_TKEY_IS_COMBO(k) ((k) >= REI_TKEY_COMBO)

typedef struct { int16_t x, y, w, h, key; } ReiTouchArea; // screen pixels (960x544)

void rei_touch_init(void);                 // start sampling, read the layout (ux0:data/reirem/touch.cfg)
int rei_touch_count(void);
ReiTouchArea *rei_touch_area(int i);
int rei_touch_add(int x, int y, int w, int h, int key); // index of the new area (-1: no memory)
void rei_touch_remove(int i);
void rei_touch_clear(void);
void rei_touch_save(void);                 // to the layout file in use
// the layout file in use: the game's (per-game settings on; game_path) or the global one (NULL). keep: the layout now
// is saved to it (per-game settings made); else it is read (a game without its own yet: the global layout)
void rei_touch_select(const char *game_path, int keep);
int rei_touch_on_area(int x, int y);       // a touch point on a button
int rei_touch_gen(void);                   // changes whenever the layout changes
void rei_touch_changed(void);              // an area was edited in place
uint32_t rei_touch_key_code(int key);      // REI_TKEY / REI_KEY -> SCE_CTRL_* bits
const char *rei_touch_key_name(int key);

// the touches now (screen pixels, up to 6); returns the count
int rei_touch_read(int *x, int *y);
// SCE_CTRL_* bits of the areas touched now
uint32_t rei_touch_buttons(void);

#endif
