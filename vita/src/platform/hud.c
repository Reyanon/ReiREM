#include "platform/hud.h"

#include <psp2/ctrl.h>
#include <vita2d.h>

#include "platform/clock.h"
#include "platform/log.h"
#include "platform/perf.h"

#define STATS_PERIOD  30 // stats recompute interval (frames)
#define LOG_PERIOD    60 // UDP log interval (frames)

#define HUD_X         8
#define HUD_Y         20
#define HUD_LINE_H    18
#define HUD_W         330
#define HUD_BG        RGBA8(0x00, 0x00, 0x00, 0xA0)
#define HUD_FG        RGBA8(0xFF, 0xFF, 0xFF, 0xFF)
#define HUD_WARN      RGBA8(0xFF, 0x50, 0x50, 0xFF)

#define FRAME_BUDGET_MS 16.67f

static vita2d_pgf *s_font;
static int s_visible;
static uint32_t s_frame;
static rei_perf_stats s_stats[REI_PERF_COUNT];

void rei_hud_init(void)
{
	s_font = vita2d_load_default_pgf();
}

void rei_hud_fini(void)
{
	if (s_font) {
		vita2d_free_pgf(s_font);
		s_font = NULL;
	}
}

static void refresh_stats(void)
{
	for (int i = 0; i < REI_PERF_COUNT; i++)
		rei_perf_stats_get((rei_perf_slot)i, &s_stats[i]);
}

void rei_hud_update(uint32_t buttons)
{
	// SELECT+L toggle became quick save. HUD is toggled in options Display > Performance HUD
	(void)buttons;

	s_frame++;
	if (s_frame % STATS_PERIOD == 0)
		refresh_stats();

#ifdef REI_ENABLE_LOG
	if (s_frame % LOG_PERIOD == 0) {
		const rei_perf_stats *st = s_stats;
		REI_LOG("perf: emu %.2f/%.2f/%.2f sound %.2f/%.2f/%.2f "
			"render %.2f/%.2f/%.2f frame %.2f/%.2f/%.2f",
			st[0].avg_ms, st[0].max_ms, st[0].p99_ms,
			st[1].avg_ms, st[1].max_ms, st[1].p99_ms,
			st[2].avg_ms, st[2].max_ms, st[2].p99_ms,
			st[3].avg_ms, st[3].max_ms, st[3].p99_ms);
		if (s_frame % (LOG_PERIOD * 5) == 0)
			rei_clock_log(0);
	}
#endif
}

void rei_hud_draw(void)
{
	if (!s_visible || !s_font)
		return;

	vita2d_draw_rectangle(HUD_X - 4, HUD_Y - 16, HUD_W,
		HUD_LINE_H * (REI_PERF_COUNT + 1) + 4, HUD_BG);
	vita2d_pgf_draw_text(s_font, HUD_X, HUD_Y, HUD_FG, 0.8f,
		"slot     avg    max    p99 (ms)");

	for (int i = 0; i < REI_PERF_COUNT; i++) {
		const rei_perf_stats *st = &s_stats[i];
		// frame interval slot warns on over-budget (= frame drop); others warn when max exceeds budget
		float limit = (i == REI_PERF_FRAME) ? FRAME_BUDGET_MS + 1.0f : FRAME_BUDGET_MS;
		unsigned int color = st->max_ms > limit ? HUD_WARN : HUD_FG;
		vita2d_pgf_draw_textf(s_font, HUD_X, HUD_Y + HUD_LINE_H * (i + 1), color, 0.8f,
			"%-6s %6.2f %6.2f %6.2f",
			rei_perf_slot_name((rei_perf_slot)i), st->avg_ms, st->max_ms, st->p99_ms);
	}
}

void rei_hud_set_visible(int on)
{
	s_visible = on ? 1 : 0;
}

int rei_hud_visible(void)
{
	return s_visible;
}
