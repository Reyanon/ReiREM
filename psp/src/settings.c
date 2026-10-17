// User settings (settings.cfg, same format as the Vita version vita/src/frontend/settings.c)
#include "settings.h"

#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

rei_settings g_settings = {
	.show_fps = 0,
	.aspect = REI_ASPECT_4_3,
	.filter = REI_FILTER_LINEAR,
	.border = REI_BORDER_ARTWORK,
	.border_color = REI_BCOL_NAVY,
	.menu_confirm = REI_CONFIRM_CROSS,
	.start_stage = 0,
	.keymap = { REI_KEY_SQUARE, REI_KEY_CROSS, REI_KEY_SELECT, REI_KEY_START, REI_KEY_CIRCLE, REI_KEY_TRIANGLE, REI_KEY_L, REI_KEY_R },
	.turbo_key = REI_KEY_NONE,
	.turbo_btn = 1,
	.turbo_speed = REI_TURBO_FAST,
	.cheat_health = 0,
	.dip_count = 0,
};

static char s_dir[96] = "ms0:/PSP/GAME/REIREM/";
static const char *const s_keymap_keys[REI_ACT_COUNT] = { "key_attack", "key_jump", "key_coin", "key_start",
	"key_b3", "key_b4", "key_b5", "key_b6" };

void rei_settings_set_dir(const char *dir)
{
	strncpy(s_dir, dir, sizeof(s_dir) - 1);
	s_dir[sizeof(s_dir) - 1] = 0;
}

const char *rei_settings_dir(void)
{
	return s_dir;
}

void rei_settings_default_keys(void)
{
	g_settings.keymap[REI_ACT_ATTACK] = REI_KEY_SQUARE;
	g_settings.keymap[REI_ACT_JUMP] = REI_KEY_CROSS;
	g_settings.keymap[REI_ACT_COIN] = REI_KEY_SELECT;
	g_settings.keymap[REI_ACT_START] = REI_KEY_START;
	g_settings.keymap[REI_ACT_B3] = REI_KEY_CIRCLE;
	g_settings.keymap[REI_ACT_B4] = REI_KEY_TRIANGLE;
	g_settings.keymap[REI_ACT_B5] = REI_KEY_L;
	g_settings.keymap[REI_ACT_B6] = REI_KEY_R;
	g_settings.turbo_key = REI_KEY_NONE;
	g_settings.turbo_btn = 1;
	g_settings.turbo_speed = REI_TURBO_FAST;
}

int rei_turbo_period(void)
{
	static const int p[REI_TURBO_SPEED_COUNT] = { 4, 6, 8 }; // 15/10/7.5 presses per second at 60fps
	return p[g_settings.turbo_speed];
}

int rei_turbo_act(void)
{
	static const int a[REI_TURBO_BUTTONS] = { REI_ACT_ATTACK, REI_ACT_JUMP, REI_ACT_B3, REI_ACT_B4, REI_ACT_B5, REI_ACT_B6 };
	const int b = g_settings.turbo_btn;
	return a[(b >= 1 && b <= REI_TURBO_BUTTONS ? b : 1) - 1];
}

uint32_t rei_key_mask(int key)
{
	static const uint32_t m[REI_KEY_COUNT] = {
		PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE,
		PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_SELECT, PSP_CTRL_START,
	};
	return (key >= 0 && key < REI_KEY_COUNT) ? m[key] : 0;
}

const char *rei_key_name(int key)
{
	static const char *const n[REI_KEY_COUNT] = {
		"Cross", "Circle", "Square", "Triangle", "L", "R", "Select", "Start",
	};
	return (key >= 0 && key < REI_KEY_COUNT) ? n[key] : "None";
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void path(char *buf, int len)
{
	snprintf(buf, len, "%ssettings.cfg", s_dir);
}

// One "key=value" line (integer values, except last_game which is text). Returns 1 on success
static int read_kv(FILE *f, char *key, int klen, char *val, int vlen)
{
	char line[96];
	if (!fgets(line, sizeof(line), f))
		return 0;
	char *eq = strchr(line, '=');
	if (!eq) {
		key[0] = 0;
		val[0] = 0;
		return 1;
	}
	*eq = 0;
	snprintf(key, klen, "%s", line);
	char *v = eq + 1;
	v[strcspn(v, "\r\n")] = 0;
	snprintf(val, vlen, "%s", v);
	return 1;
}

static void read_dips(const char *key, int val)
{
	if (!strcmp(key, "dip_count"))
		g_settings.dip_count = clampi(val, 0, REI_DIP_BYTES_MAX);
	else if (!strncmp(key, "dip", 3) && key[3] >= '0' && key[3] <= '9') {
		int i = key[3] - '0';
		if (i < REI_DIP_BYTES_MAX)
			g_settings.dip[i] = (uint8_t)val;
	}
}

// Per-game overridable items (global values kept in s_global)
typedef struct {
	int show_fps, aspect, filter, border, border_color;
	int keymap[REI_ACT_COUNT], turbo_key, turbo_btn, turbo_speed;
} game_part;
static game_part s_global;   // values from the global settings file (written to the global file while per-game settings are on)
static int s_game_on;        // per-game settings are on for the current game
static char s_game[16];      // current game's set name (per-game files <set>.cfg, <set>_game.cfg)
static void part_get(game_part *g)
{
	g->show_fps = g_settings.show_fps; g->aspect = g_settings.aspect; g->filter = g_settings.filter;
	g->border = g_settings.border; g->border_color = g_settings.border_color;
	memcpy(g->keymap, g_settings.keymap, sizeof(g->keymap));
	g->turbo_key = g_settings.turbo_key; g->turbo_btn = g_settings.turbo_btn;
	g->turbo_speed = g_settings.turbo_speed;
}
static void part_set(const game_part *g)
{
	g_settings.show_fps = g->show_fps; g_settings.aspect = g->aspect; g_settings.filter = g->filter;
	g_settings.border = g->border; g_settings.border_color = g->border_color;
	memcpy(g_settings.keymap, g->keymap, sizeof(g->keymap));
	g_settings.turbo_key = g->turbo_key; g_settings.turbo_btn = g->turbo_btn;
	g_settings.turbo_speed = g->turbo_speed;
}
// Old turbo lines (turbo_attack / turbo_jump, one key per button) seen in the file being read. If the file has no turbo_key,
// parse_end maps them to the single turbo key: attack -> button 1, else jump -> button 2
static int s_old_turbo[2], s_new_turbo;
static void parse_begin(void)
{
	s_old_turbo[0] = s_old_turbo[1] = -1;
	s_new_turbo = 0;
}
static void parse_end(void)
{
	if (s_new_turbo)
		return;
	if (s_old_turbo[0] >= 0 && s_old_turbo[0] != REI_KEY_NONE) {
		g_settings.turbo_key = s_old_turbo[0];
		g_settings.turbo_btn = 1;
	} else if (s_old_turbo[1] >= 0 && s_old_turbo[1] != REI_KEY_NONE) {
		g_settings.turbo_key = s_old_turbo[1];
		g_settings.turbo_btn = 2;
	} else if (s_old_turbo[0] >= 0 || s_old_turbo[1] >= 0)
		g_settings.turbo_key = REI_KEY_NONE;
}
// One overridable item line (shared by global and per-game files). Returns 1 if recognized
static int read_part(const char *key, int val)
{
	if (!strcmp(key, "show_fps")) g_settings.show_fps = val != 0;
	else if (!strcmp(key, "aspect")) g_settings.aspect = clampi(val, 0, REI_ASPECT_COUNT - 1);
	else if (!strcmp(key, "filter")) g_settings.filter = clampi(val, 0, REI_FILTER_COUNT - 1);
	else if (!strcmp(key, "border")) g_settings.border = clampi(val, 0, REI_BORDER_COUNT - 1);
	else if (!strcmp(key, "border_color")) g_settings.border_color = clampi(val, 0, REI_BCOL_COUNT - 1);
	else if (!strcmp(key, "turbo_key")) {
		g_settings.turbo_key = clampi(val, 0, REI_KEY_NONE);
		s_new_turbo = 1;
	} else if (!strcmp(key, "turbo_btn")) g_settings.turbo_btn = clampi(val, 1, REI_TURBO_BUTTONS);
	else if (!strcmp(key, "turbo_attack")) s_old_turbo[0] = clampi(val, 0, REI_KEY_NONE);
	else if (!strcmp(key, "turbo_jump")) s_old_turbo[1] = clampi(val, 0, REI_KEY_NONE);
	else if (!strcmp(key, "turbo_speed")) g_settings.turbo_speed = clampi(val, 0, REI_TURBO_SPEED_COUNT - 1);
	else {
		for (int a = 0; a < REI_ACT_COUNT; a++)
			if (!strcmp(key, s_keymap_keys[a])) {
				g_settings.keymap[a] = clampi(val, 0, a >= REI_ACT_B3 ? REI_KEY_NONE : REI_KEY_COUNT - 1);
				return 1;
			}
		return 0;
	}
	return 1;
}
static void write_part(FILE *f, const game_part *g)
{
	fprintf(f, "show_fps=%d\naspect=%d\nfilter=%d\n", g->show_fps, g->aspect, g->filter);
	fprintf(f, "border=%d\nborder_color=%d\n", g->border, g->border_color);
	for (int a = 0; a < REI_ACT_COUNT; a++)
		fprintf(f, "%s=%d\n", s_keymap_keys[a], g->keymap[a]);
	fprintf(f, "turbo_key=%d\nturbo_btn=%d\nturbo_speed=%d\n", g->turbo_key, g->turbo_btn, g->turbo_speed);
}

// Favorite games: set names in favorites.cfg, one per line
#define FAV_MAX 256
static char s_fav[FAV_MAX][16];
static int s_fav_n;

int rei_fav_is(const char *set)
{
	for (int i = 0; i < s_fav_n; i++)
		if (!strcmp(s_fav[i], set))
			return 1;
	return 0;
}

static void fav_load(void)
{
	char p[128];
	snprintf(p, sizeof(p), "%sfavorites.cfg", s_dir);
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	char line[64];
	s_fav_n = 0;
	while (s_fav_n < FAV_MAX && fgets(line, sizeof(line), f)) {
		const size_t len = strcspn(line, "\r\n");
		line[len] = 0;
		if (len && len < sizeof(s_fav[0]) && !rei_fav_is(line))
			memcpy(s_fav[s_fav_n++], line, len + 1);
	}
	fclose(f);
}

void rei_fav_toggle(const char *set)
{
	int i = 0;
	while (i < s_fav_n && strcmp(s_fav[i], set) != 0)
		i++;
	if (i < s_fav_n) {
		memmove(s_fav[i], s_fav[i + 1], (size_t)(s_fav_n - i - 1) * sizeof(s_fav[0]));
		s_fav_n--;
	} else if (s_fav_n < FAV_MAX && strlen(set) < sizeof(s_fav[0]))
		strcpy(s_fav[s_fav_n++], set);
	char p[128];
	snprintf(p, sizeof(p), "%sfavorites.cfg", s_dir);
	FILE *f = fopen(p, "w");
	if (!f) {
		REI_LOG("settings: save failed %s\n", p);
		return;
	}
	for (i = 0; i < s_fav_n; i++)
		fprintf(f, "%s\n", s_fav[i]);
	fclose(f);
}

void rei_settings_load(void)
{
	fav_load();
	char p[128];
	path(p, sizeof(p));
	FILE *f = fopen(p, "r");
	if (!f) // first run: carry over settings from the old nbbatman-only install folder (saved to the current folder from then on)
		f = fopen("ms0:/PSP/GAME/NBBATMAN/settings.cfg", "r");
	if (!f)
		return;
	char key[32], sval[32];
	parse_begin();
	while (read_kv(f, key, sizeof(key), sval, sizeof(sval))) {
		const int val = atoi(sval);
		if (read_part(key, val)) continue;
		else if (!strcmp(key, "menu_confirm")) g_settings.menu_confirm = clampi(val, 0, REI_CONFIRM_COUNT - 1);
		else if (!strcmp(key, "last_game")) snprintf(g_settings.last_game, sizeof(g_settings.last_game), "%s", sval);
		else if (!strcmp(key, "show_list")) g_settings.show_list = val != 0;
		else if (!strcmp(key, "list_filter")) g_settings.list_filter = clampi(val, 0, REI_LIST_FILTER_COUNT - 1);
		else if (!strcmp(key, "list_all")) g_settings.list_filter = val ? REI_LIST_ALL : REI_LIST_PLAYABLE; // (old two-state filter)
		else if (!strncmp(key, "dip", 3)) read_dips(key, val); // (old nbbatman-only file: nbbatman DIPs)
	}
	fclose(f);
	parse_end();
	part_get(&s_global);
	REI_LOG("settings: %s fps %d aspect %d filter %d border %d keys %d/%d/%d/%d last %s\n", p,
		g_settings.show_fps, g_settings.aspect, g_settings.filter, g_settings.border, g_settings.keymap[0],
		g_settings.keymap[1], g_settings.keymap[2], g_settings.keymap[3], g_settings.last_game);
}

void rei_settings_load_game(const char *set)
{
	snprintf(s_game, sizeof(s_game), "%s", set);
	snprintf(g_settings.last_game, sizeof(g_settings.last_game), "%s", set);
	char p[128];
	// per-game settings: if the previous game's were on, revert to global values, then apply this game's if present
	if (s_game_on) {
		part_set(&s_global);
		s_game_on = 0;
	} else
		part_get(&s_global);
	snprintf(p, sizeof(p), "%s%s_game.cfg", s_dir, s_game);
	FILE *f = fopen(p, "r");
	if (f) {
		char key[32], sval[32];
		parse_begin();
		while (read_kv(f, key, sizeof(key), sval, sizeof(sval)))
			read_part(key, atoi(sval));
		fclose(f);
		parse_end();
		s_game_on = 1;
		REI_LOG("settings: game settings %s\n", p);
	}
	snprintf(p, sizeof(p), "%s%s.cfg", s_dir, s_game);
	f = fopen(p, "r");
	if (!f) {
		// no per-game file: nbbatman family uses values from the old settings file (if any), other games use driver defaults
		if (strncmp(set, "nbbatman", 8) != 0)
			g_settings.dip_count = 0;
		return;
	}
	g_settings.dip_count = 0;
	char key[32], sval[32];
	while (read_kv(f, key, sizeof(key), sval, sizeof(sval)))
		read_dips(key, atoi(sval));
	fclose(f);
}

void rei_settings_save(void)
{
	char p[128];
	path(p, sizeof(p));
	FILE *f = fopen(p, "w");
	if (!f) {
		REI_LOG("settings: save failed %s\n", p);
		return;
	}
	// with per-game settings on, those items go to the per-game file (below); the global file keeps the global values
	if (!s_game_on)
		part_get(&s_global);
	write_part(f, &s_global);
	fprintf(f, "menu_confirm=%d\n", g_settings.menu_confirm);
	// cheats (start stage, infinite health) are not saved: always start off
	if (g_settings.last_game[0])
		fprintf(f, "last_game=%s\n", g_settings.last_game);
	if (g_settings.show_list)
		fprintf(f, "show_list=1\n");
	fprintf(f, "list_filter=%d\n", g_settings.list_filter);
	fclose(f);
	if (!s_game[0])
		return;
	if (s_game_on)
		rei_settings_save_game();
	snprintf(p, sizeof(p), "%s%s.cfg", s_dir, s_game);
	f = fopen(p, "w");
	if (!f)
		return;
	fprintf(f, "dip_count=%d\n", g_settings.dip_count);
	for (int i = 0; i < g_settings.dip_count; i++)
		fprintf(f, "dip%d=%d\n", i, g_settings.dip[i]);
	fclose(f);
}

int rei_settings_has_game(void)
{
	return s_game_on;
}

void rei_settings_use_global(void)
{
	if (s_game_on) {
		part_set(&s_global);
		s_game_on = 0;
	}
	s_game[0] = 0; // (no per-game files written until the next rei_settings_load_game)
}

int rei_settings_save_game(void)
{
	if (!s_game[0])
		return 0;
	char p[128];
	snprintf(p, sizeof(p), "%s%s_game.cfg", s_dir, s_game);
	FILE *f = fopen(p, "w");
	if (!f) {
		REI_LOG("settings: game save failed %s\n", p);
		return 0;
	}
	game_part g;
	part_get(&g);
	write_part(f, &g);
	fclose(f);
	s_game_on = 1;
	return 1;
}

int rei_settings_delete_game(void)
{
	if (!s_game[0] || !s_game_on)
		return 0;
	char p[128];
	snprintf(p, sizeof(p), "%s%s_game.cfg", s_dir, s_game);
	if (remove(p) != 0) {
		REI_LOG("settings: game delete failed %s\n", p);
		return 0;
	}
	s_game_on = 0;
	part_set(&s_global); // (straight to global values)
	return 1;
}
