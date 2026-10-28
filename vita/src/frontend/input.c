#include "frontend/input.h"

#include <psp2/ctrl.h>

#include "frontend/settings.h"
#include "frontend/testpad.h"
#include "frontend/touch.h"
#include "frontend/video.h"
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
	const uint32_t raw = pad.buttons | rei_testpad_step(); // (log builds: the test input script)
	uint32_t b = raw;
	if (g_settings.touch_keys) // touch buttons press their Vita keys (in the game only: the value returned is the pad alone)
		b |= rei_touch_buttons();

	int up = (b & SCE_CTRL_UP) != 0, down = (b & SCE_CTRL_DOWN) != 0;
	int left = (b & SCE_CTRL_LEFT) != 0, right = (b & SCE_CTRL_RIGHT) != 0;
	if (g_settings.stick == REI_STICK_LEFT || g_settings.stick == REI_STICK_BOTH) {
		up |= pad.ly < 128 - STICK_DEAD;
		down |= pad.ly > 128 + STICK_DEAD;
		left |= pad.lx < 128 - STICK_DEAD;
		right |= pad.lx > 128 + STICK_DEAD;
	}
	if (g_settings.stick == REI_STICK_RIGHT || g_settings.stick == REI_STICK_BOTH) {
		up |= pad.ry < 128 - STICK_DEAD;
		down |= pad.ry > 128 + STICK_DEAD;
		left |= pad.rx < 128 - STICK_DEAD;
		right |= pad.rx > 128 + STICK_DEAD;
	}
	if (rei_input_rot) {
		// the Vita is held turned (Rotate Vertical L / R): screen directions back to the game's
		// (90: screen right = game up; 270: screen left = game up)
		const int u = up, d = down, l = left, r = right;
		if (rei_input_rot == 90) {
			up = r; down = l; left = u; right = d;
		} else {
			up = l; down = r; left = d; right = u;
		}
	}
	rei_core_set_input(REI_IN_P1_UP, up);
	rei_core_set_input(REI_IN_P1_DOWN, down);
	rei_core_set_input(REI_IN_P1_LEFT, left);
	rei_core_set_input(REI_IN_P1_RIGHT, right);

	// system combos are not passed to the game: SELECT+START options, SELECT+L quick save, SELECT+R quick load
	uint32_t blocked = 0;
	if ((raw & SCE_CTRL_SELECT) && (raw & SCE_CTRL_START))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_START;
	if ((raw & SCE_CTRL_SELECT) && (raw & SCE_CTRL_LTRIGGER))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER;
	if ((raw & SCE_CTRL_SELECT) && (raw & SCE_CTRL_RTRIGGER))
		blocked |= SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
	uint32_t game = b & ~blocked;

	// combo keys: a key with a combo acts as its two keys pressed together (instead of its own mapping)
	uint32_t add = 0;
	for (int k = 0; k < REI_COMBO_KEYS; k++)
		if (g_settings.combo[k] && (game & rei_key_mask(k))) {
			game &= ~rei_key_mask(k);
			for (int j = 0; j < REI_COMBO_KEYS; j++)
				if (g_settings.combo[k] & (1 << j))
					add |= rei_key_mask(j);
		}
	game |= add;

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
	return raw;
}

void rei_input_release(void)
{
	for (int i = 0; i < REI_IN_DIGITAL_COUNT; i++)
		rei_core_set_input(i, 0);
}
