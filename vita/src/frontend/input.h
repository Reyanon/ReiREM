#ifndef REI_FRONTEND_INPUT_H
#define REI_FRONTEND_INPUT_H

#include <stdint.h>

// SceCtrl → core input (1P). Button mapping settings are added in M5.
//   D-pad/left stick: move, □: attack (B1), ×: jump (B2)
//   SELECT: coin, START: start. SELECT+L toggles HUD, SELECT+START opens options (not passed to the game)

void rei_input_init(void);

// Read the pad, pass it to the core, return raw button bits (for HUD toggle detection)
uint32_t rei_input_poll(void);

#endif
