#include "frontend/input.h"

#include <psp2/ctrl.h>

#include "frontend/settings.h"
#include "rei_core.h"

#define STICK_DEAD 48 // distance from center (128) required to count as a direction

void rei_input_init(void)
{
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
}

uint32_t rei_input_poll(void)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	uint32_t b = pad.buttons;

	int up = (b & SCE_CTRL_UP) || pad.ly < 128 - STICK_DEAD;
	int down = (b & SCE_CTRL_DOWN) || pad.ly > 128 + STICK_DEAD;
	int left = (b & SCE_CTRL_LEFT) || pad.lx < 128 - STICK_DEAD;
	int right = (b & SCE_CTRL_RIGHT) || pad.lx > 128 + STICK_DEAD;

	rei_core_set_input(REI_IN_P1_UP, up);
	rei_core_set_input(REI_IN_P1_DOWN, down);
	rei_core_set_input(REI_IN_P1_LEFT, left);
	rei_core_set_input(REI_IN_P1_RIGHT, right);

	// system combos are not passed to the game: SELECT+START options, SELECT+L quick save, SELECT+R quick load
	uint32_t blocked = 0;
	if ((b & SCE_CTRL_SELECT) && (b & SCE_CTRL_START))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_START;
	if ((b & SCE_CTRL_SELECT) && (b & SCE_CTRL_LTRIGGER))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER;
	if ((b & SCE_CTRL_SELECT) && (b & SCE_CTRL_RTRIGGER))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
	const uint32_t game = b & ~blocked;

	// turbo: while the turbo key is held, the chosen game button is pressed only during the first half of each period.
	// counting starts at the press frame, so the first press registers immediately
	static int turbo_t;
	int turbo_on = 0;
	const uint32_t tm = rei_key_mask(g_settings.turbo_key);
	if (tm && (game & tm)) {
		const int period = rei_turbo_period();
		turbo_on = turbo_t < period / 2;
		turbo_t = (turbo_t + 1) % period;
	} else
		turbo_t = 0;
	const int turbo_act = rei_turbo_act();

	static const int act_input[REI_ACT_COUNT] = { REI_IN_P1_B1, REI_IN_P1_B2, REI_IN_P1_COIN, REI_IN_P1_START,
		REI_IN_P1_B3, REI_IN_P1_B4, REI_IN_P1_B5, REI_IN_P1_B6 };
	for (int a = 0; a < REI_ACT_COUNT; a++) {
		// if the turbo key is also mapped as a normal key, turbo wins (its normal mapping is ignored)
		int on = ((game & ~tm) & rei_key_mask(g_settings.keymap[a])) != 0;
		if (a == turbo_act)
			on |= turbo_on;
		rei_core_set_input(act_input[a], on);
	}
	return b;
}
