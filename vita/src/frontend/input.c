#include "frontend/input.h"

#include <psp2/ctrl.h>

#include "common/re_pad.h"
#include "frontend/settings.h"
#include "frontend/testpad.h"
#include "frontend/touch.h"
#include "frontend/video.h"
#include "rei_core.h"

#define STICK_DEAD 48 // distance from center (128) required to count as a direction

void rei_input_init(void)
{
	re_pad_init();
}

// game inputs of each player, in REI_ACT_* order
static const int s_act_input[2][REI_ACT_COUNT] = {
	{ REI_IN_P1_B1, REI_IN_P1_B2, REI_IN_P1_COIN, REI_IN_P1_START, REI_IN_P1_B3, REI_IN_P1_B4, REI_IN_P1_B5, REI_IN_P1_B6 },
	{ REI_IN_P2_B1, REI_IN_P2_B2, REI_IN_P2_COIN, REI_IN_P2_START, REI_IN_P2_B3, REI_IN_P2_B4, REI_IN_P2_B5, REI_IN_P2_B6 },
};
static const int s_dir_input[2][4] = {
	{ REI_IN_P1_UP, REI_IN_P1_DOWN, REI_IN_P1_LEFT, REI_IN_P1_RIGHT },
	{ REI_IN_P2_UP, REI_IN_P2_DOWN, REI_IN_P2_LEFT, REI_IN_P2_RIGHT },
};

// one player: its controller (re_pad) with its own port settings (keys, stick, turbo, combo keys). raw = the
// controller's buttons (system combos), b = with the touch buttons (1P)
static void map_player(int pl, const SceCtrlData *pad, uint32_t raw, uint32_t b)
{
	// this player's buttons, stick, turbo and combo keys: those of the device it reads (they go with the device)
	const rei_port *pr = &g_settings.port[re_pad_player_profile(pl)];
	const int *km = pr->keymap;
	int up = (b & SCE_CTRL_UP) != 0, down = (b & SCE_CTRL_DOWN) != 0;
	int left = (b & SCE_CTRL_LEFT) != 0, right = (b & SCE_CTRL_RIGHT) != 0;
	if (pr->stick == REI_STICK_LEFT || pr->stick == REI_STICK_BOTH) {
		up |= pad->ly < 128 - STICK_DEAD;
		down |= pad->ly > 128 + STICK_DEAD;
		left |= pad->lx < 128 - STICK_DEAD;
		right |= pad->lx > 128 + STICK_DEAD;
	}
	if (pr->stick == REI_STICK_RIGHT || pr->stick == REI_STICK_BOTH) {
		up |= pad->ry < 128 - STICK_DEAD;
		down |= pad->ry > 128 + STICK_DEAD;
		left |= pad->rx < 128 - STICK_DEAD;
		right |= pad->rx > 128 + STICK_DEAD;
	}
	if (rei_input_rot && re_pad_port(pl) == 0 && !re_pad_is_tv()) {
		// the Vita is held turned (Rotate Vertical L / R): screen directions back to the game's
		// (90: screen right = game up; 270: screen left = game up). Its own controls only, not a controller
		const int u = up, d = down, l = left, r = right;
		if (rei_input_rot == 90) {
			up = r; down = l; left = u; right = d;
		} else {
			up = l; down = r; left = d; right = u;
		}
	}
	rei_core_set_input(s_dir_input[pl][0], up);
	rei_core_set_input(s_dir_input[pl][1], down);
	rei_core_set_input(s_dir_input[pl][2], left);
	rei_core_set_input(s_dir_input[pl][3], right);

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
	for (int k = 0; k < REI_KEY_COUNT; k++)
		if (pr->combo[k] && (game & rei_key_mask(k))) {
			game &= ~rei_key_mask(k);
			for (int j = 0; j < REI_KEY_COUNT; j++)
				if (pr->combo[k] & (1 << j))
					add |= rei_key_mask(j);
		}
	game |= add;

	// turbo: while the turbo key is held, the chosen game button is pressed only during the first half of each period.
	// counting starts at the press frame, so the first press registers immediately
	static int turbo_t[2];
	int turbo_on = 0;
	const uint32_t tm = rei_key_mask(pr->turbo_key);
	if (tm && (game & tm)) {
		const int period = rei_turbo_period(pl);
		turbo_on = turbo_t[pl] < period / 2;
		turbo_t[pl] = (turbo_t[pl] + 1) % period;
	} else
		turbo_t[pl] = 0;
	const int turbo_act = rei_turbo_act(pl);

	for (int a = 0; a < REI_ACT_COUNT; a++) {
		// if the turbo key is also mapped as a normal key, turbo wins (its normal mapping is ignored)
		int on = ((game & ~tm) & rei_key_mask(km[a])) != 0;
		if (a == turbo_act)
			on |= turbo_on;
		rei_core_set_input(s_act_input[pl][a], on);
	}
}

uint32_t rei_input_poll(void)
{
	// the players' controllers from the settings (loaded, or changed in the menu)
	static int src[2] = { -1, -1 };
	for (int p = 0; p < 2; p++)
		if (src[p] != g_settings.pad_src[p]) {
			src[p] = g_settings.pad_src[p];
			re_pad_set_source(p, src[p]);
		}
	re_pad_poll();
	SceCtrlData p1, p2, all;
	re_pad_read(0, &p1);
	re_pad_read(1, &p2);
	re_pad_read_all(&all);
	const uint32_t test = rei_testpad_step(); // (log builds: the test input script)
	p1.buttons |= test;
	all.buttons |= test;
	uint32_t b = p1.buttons;
	if (g_settings.touch_keys) // touch buttons press their Vita keys (in the game only: the value returned is the pads alone)
		b |= rei_touch_buttons();
	map_player(0, &p1, p1.buttons, b);
	map_player(1, &p2, p2.buttons, p2.buttons);
	return all.buttons;
}

void rei_input_release(void)
{
	for (int i = 0; i < REI_IN_DIGITAL_COUNT; i++)
		rei_core_set_input(i, 0);
}
