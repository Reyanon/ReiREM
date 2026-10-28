#ifndef REI_FRONTEND_INPUT_H
#define REI_FRONTEND_INPUT_H

#include <stdint.h>

// SceCtrl (and the touch buttons) -> core input (1P): button mapping, turbo, combo keys, analog stick, Rotate (settings.h).
// SELECT+START opens the options, SELECT+L / SELECT+R quick save / load (not passed to the game)

void rei_input_init(void);

// Read the pad, pass it to the core, return the pad's raw button bits (no touch buttons; for the menus and combos)
uint32_t rei_input_poll(void);
// every game input released (before a game ends: the next game starts with nothing held)
void rei_input_release(void);

#endif
