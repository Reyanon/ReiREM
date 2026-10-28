// Test input script (log builds only). See testpad.h
#include "frontend/testpad.h"

#ifdef REI_ENABLE_LOG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "frontend/settings.h"
#include "platform/log.h"

#define KEYS_PATH REI_DATA_DIR "keys"
#define MAX_STEPS 512

typedef struct { uint32_t buttons; int frames, gap; int touch, x0, y0, x1, y1; } Step;
static Step s_step[MAX_STEPS];
static int s_n, s_i, s_t;   // steps, current step, frame within it
static unsigned s_poll;     // frames since the file was last looked for
static int s_tx, s_ty, s_ton;

static uint32_t key_bits(const char *name)
{
	static const struct { const char *n; uint32_t b; } k[] = {
		{ "up", SCE_CTRL_UP }, { "down", SCE_CTRL_DOWN }, { "left", SCE_CTRL_LEFT }, { "right", SCE_CTRL_RIGHT },
		{ "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE }, { "square", SCE_CTRL_SQUARE },
		{ "triangle", SCE_CTRL_TRIANGLE }, { "l", SCE_CTRL_LTRIGGER }, { "r", SCE_CTRL_RTRIGGER },
		{ "start", SCE_CTRL_START }, { "select", SCE_CTRL_SELECT },
	};
	uint32_t b = 0;
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", name);
	for (char *p = strtok(buf, "+"); p; p = strtok(NULL, "+"))
		for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++)
			if (!strcmp(p, k[i].n))
				b |= k[i].b;
	return b;
}

static void load(void)
{
	FILE *f = fopen(KEYS_PATH, "r");
	if (!f)
		return;
	char tok[96];
	s_n = s_i = s_t = 0;
	while (s_n < MAX_STEPS && fscanf(f, "%95s", tok) == 1) {
		Step st = { 0, 4, 8, 0, 0, 0, 0, 0 };
		int rep = 1, a, b, c, d, e;
		char name[64];
		if (sscanf(tok, "wait:%d", &a) == 1) {
			st.frames = 0;
			st.gap = a;
		} else if (sscanf(tok, "touch:%d:%d:%d", &a, &b, &c) == 3) {
			st.touch = 1;
			st.x0 = st.x1 = a;
			st.y0 = st.y1 = b;
			st.frames = c;
		} else if (sscanf(tok, "drag:%d:%d:%d:%d:%d", &a, &b, &c, &d, &e) == 5) {
			st.touch = 1;
			st.x0 = a; st.y0 = b; st.x1 = c; st.y1 = d;
			st.frames = e;
		} else if (sscanf(tok, "hold:%63[^:]:%d", name, &a) == 2) {
			st.buttons = key_bits(name);
			st.frames = a;
		} else {
			char *star = strchr(tok, '*');
			if (star) {
				*star = 0;
				rep = atoi(star + 1);
			}
			st.buttons = key_bits(tok);
		}
		for (int r = 0; r < rep && s_n < MAX_STEPS; r++)
			s_step[s_n++] = st;
	}
	fclose(f);
	sceIoRemove(KEYS_PATH);
	REI_LOG("testpad: %d steps", s_n);
}

uint32_t rei_testpad_step(void)
{
	s_ton = 0;
	if (s_i >= s_n) {
		if (++s_poll % 30 == 0) {
			SceIoStat st;
			if (sceIoGetstat(KEYS_PATH, &st) >= 0)
				load();
		}
		return 0;
	}
	const Step *st = &s_step[s_i];
	uint32_t b = 0;
	if (s_t < st->frames) {
		b = st->buttons;
		if (st->touch) {
			const int k = st->frames > 1 ? st->frames - 1 : 1;
			s_tx = st->x0 + (st->x1 - st->x0) * s_t / k;
			s_ty = st->y0 + (st->y1 - st->y0) * s_t / k;
			s_ton = 1;
		}
	}
	if (++s_t >= st->frames + st->gap) {
		s_i++;
		s_t = 0;
		if (s_i == s_n)
			REI_LOG("testpad: done");
	}
	return b;
}

int rei_testpad_touch(int *x, int *y)
{
	if (!s_ton)
		return 0;
	*x = s_tx;
	*y = s_ty;
	return 1;
}
#endif
