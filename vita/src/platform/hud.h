#ifndef REI_PLATFORM_HUD_H
#define REI_PLATFORM_HUD_H

#include <stdint.h>

// Performance HUD (docs/RULES.md 5-1)
// - Toggled in options Display > Performance HUD (SELECT+L is quick save).
// - Draws per-slot avg/max/p99 ms in the top-left corner.
// - Sends the same numbers over UDP every second, whether or not the HUD is shown.

void rei_hud_init(void);
void rei_hud_fini(void);

// Once per frame on the main thread. buttons = SceCtrlData.buttons
void rei_hud_update(uint32_t buttons);

// Call between vita2d_start_drawing() and vita2d_end_drawing()
void rei_hud_draw(void);

// Toggle from the options screen
void rei_hud_set_visible(int on);
int rei_hud_visible(void);

#endif
