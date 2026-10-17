// ReiREM Vita port: frontend entry point
// M2: single-threaded emulation of 1 frame -> texture display, SceAudio output, SceCtrl input
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>

#include "frontend/audio.h"
#include "frontend/input.h"
#include "frontend/menu.h"
#include "frontend/settings.h"
#include "frontend/states.h"
#include "frontend/threads.h"
#include "frontend/video.h"
#include "rei_core.h"
#include "rei_games.h"
#include "rei_prof.h"
#include "platform/clock.h"
#include "platform/hud.h"
#include "platform/log.h"
#include "platform/perf.h"

#define ROM_DIR      REI_DATA_DIR "roms/" // where the user puts ROMs (created at startup)
// ROM folder candidates: new roms/, new folder, legacy nbbatman-only folders (roms/, folder); first hit per game (rei_games.c)
static const char *const k_rom_dirs[] = { ROM_DIR, REI_DATA_DIR, REI_OLD_DATA_DIR "roms/", REI_OLD_DATA_DIR };
#define ROM_DIRS ((int)(sizeof(k_rom_dirs) / sizeof(k_rom_dirs[0])))

#ifndef REI_INTERLEAVE_MUL
#define REI_INTERLEAVE_MUL 0
#endif
#define CLEAR_COLOR RGBA8(0x00, 0x00, 0x00, 0xFF)

// the core uses ~20MB for graphics ROM decoding etc., so reserve a generous newlib heap
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;

static void core_log(const char *msg)
{
	REI_LOG("core: %s", msg);
	(void)msg;
}


// show a fatal error on screen, exit on START (default PGF font has no CJK glyphs, so English)
static void fatal_screen(const char *msg)
{
	REI_LOG("fatal: %s", msg);
	vita2d_pgf *font = vita2d_load_default_pgf();
	char games[160] = "";
	for (int i = 0, len = 0; i < rei_core_game_count() && len < (int)sizeof(games) - 16; i++)
		len += snprintf(games + len, sizeof(games) - len, "%s%s.zip", i ? ", " : "", rei_core_game_set(i));
	for (;;) {
		if (rei_input_poll() & SCE_CTRL_START)
			break;
		vita2d_start_drawing();
		vita2d_clear_screen();
		vita2d_pgf_draw_text(font, 40, 180, RGBA8(0xFF, 0x60, 0x60, 0xFF), 1.2f, msg);
		vita2d_pgf_draw_text(font, 40, 240, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 1.0f, "Put ROM zip files in " ROM_DIR);
		vita2d_pgf_draw_text(font, 40, 280, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 0.8f, games);
		vita2d_pgf_draw_text(font, 40, 310, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), 0.8f,
			"Clone sets (e.g. nbbatmanu) may be split: missing files are read from the parent zip.");
		vita2d_pgf_draw_text(font, 40, 360, RGBA8(0xA0, 0xA8, 0xC0, 0xFF), 1.0f, "START: exit");
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}
	vita2d_free_pgf(font);
}

// list all supported games (greyed out without ROM) and pick one (cursor on last game). Same as the PSP version
static const rei_game_found *choose_game(void)
{
	static rei_game_found found[REI_GAMES_MAX];
	int present = 0;
	const int n = rei_games_scan(k_rom_dirs, ROM_DIRS, found, REI_GAMES_MAX, &present);
	REI_LOG("games: %d supported, %d with ROM", n, present);
	if (n <= 0)
		return NULL;
	if (g_settings.show_list) { // (options screen Select game flag; the list is always shown now, so just clear it)
		g_settings.show_list = 0;
		rei_settings_save();
	}
#ifdef REI_ENABLE_LOG
	{ // dev loop: if REI_DATA_DIR rei_game holds a set name, run that game without the list (remote testing, log builds only)
		FILE *f = fopen(REI_DATA_DIR "rei_game", "r");
		if (f) {
			char want[24] = { 0 };
			if (fgets(want, sizeof(want), f))
				want[strcspn(want, "\r\n")] = 0;
			fclose(f);
			for (int i = 0; i < n; i++)
				if (found[i].present && !strcmp(rei_core_game_set(found[i].game), want)) {
					REI_LOG("games: %s (rei_game)", want);
					return &found[i];
				}
		}
	}
#endif
	int initial = -1;
	for (int i = 0; i < n; i++)
		if (!strcmp(rei_core_game_set(found[i].game), g_settings.last_game))
			initial = i;
	if (initial < 0) // no last game: first game with a ROM
		for (int i = 0; i < n && initial < 0; i++)
			if (found[i].present)
				initial = i;
	return &found[rei_game_select(found, n, initial, ROM_DIR)];
}

static void load_progress(int permille, void *user)
{
	(void)user;
	rei_loading_progress(permille);
}

static int s_ym_worker; // other boards: YM2151 synthesis worker thread in use

// reset/load: stop sound and render threads, change core state, then restart them
static int with_threads_stopped(const rei_core_config *cfg, uint64_t *emu_frames, const char *load_path)
{
	if (cfg->threaded || s_ym_worker)
		rei_threads_stop();
	if (cfg->render_threaded)
		rei_video_stop_render_thread();
	int rc = 0;
	if (load_path) {
		rc = rei_core_state_load(load_path);
		if (rc != 0)
			rei_core_reset(); // the sound side ran one extra line while stopping threads, so it can't just continue
	} else {
		rei_core_reset();
	}
	*emu_frames = 0;
	if (cfg->render_threaded) {
		rei_video_reset_rendered();
		rei_video_start_render_thread();
	}
	if (cfg->threaded)
		rei_threads_start_sound();
	if (s_ym_worker)
		rei_threads_start_ym();
	return rc;
}

static void reset_game(const rei_core_config *cfg, uint64_t *emu_frames)
{
	with_threads_stopped(cfg, emu_frames, NULL);
	REI_LOG("reset");
}

// save state (slot 0-9 or REI_STATE_QUICK). Result shown at the bottom of the screen
static void save_state(int slot)
{
	char path[128];
	rei_state_path(slot, path, sizeof(path));
	int rc = rei_core_state_save(path);
	REI_LOG("state: save %s rc %d", path, rc);
	if (rc == 0)
		rei_osd_show("Saved: %s", rei_state_slot_name(slot));
	else
		rei_osd_show("Save failed: %s", rei_state_slot_name(slot));
}

static void load_state(const rei_core_config *cfg, uint64_t *emu_frames, int slot)
{
	char path[128];
	rei_state_path(slot, path, sizeof(path));
	if (!rei_core_state_exists(path)) {
		rei_osd_show("No state: %s", rei_state_slot_name(slot));
		return;
	}
	int rc = with_threads_stopped(cfg, emu_frames, path);
	REI_LOG("state: load %s rc %d", path, rc);
	if (rc == 0)
		rei_osd_show("Loaded: %s", rei_state_slot_name(slot));
	else
		rei_osd_show("Load failed (game was reset): %s", rei_state_slot_name(slot));
}

int main(void)
{
	rei_log_init();
	rei_threads_pin_main();
#ifdef REI_CLOCK_SWEEP
	rei_clock_sweep();
#endif
	rei_clock_boost();

	vita2d_init();
	vita2d_set_clear_color(CLEAR_COLOR);
	vita2d_set_vblank_wait(1);
	rei_hud_init();
	rei_input_init();
	{ // app renamed to ReiREM: if the old folder (settings/ROMs/saves) exists and the new one doesn't, just rename it
		SceIoStat st;
		if (sceIoGetstat(REI_DATA_DIR, &st) < 0 && sceIoGetstat(REI_M92_DATA_DIR, &st) >= 0)
			sceIoRename(REI_M92_DATA_DIR, "ux0:data/reirem");
	}
	rei_settings_load();
	rei_menu_init();

	// games: find supported games with ROMs in roms/ etc. and pick one (list if more than one). Pre-create the folders users fill
	sceIoMkdir(REI_DATA_DIR, 0777);
	sceIoMkdir(ROM_DIR, 0777);
	sceIoMkdir(REI_DATA_DIR "cheats", 0777);  // user cheat file <set>.ini (FBNeo format)
	sceIoMkdir(REI_DATA_DIR "artwork", 0777); // user bezel artwork <set>.png (.jpg .bmp)
	const rei_game_found *game = choose_game();
	if (!game) {
		fatal_screen("ROM not found");
		goto out;
	}
	rei_settings_load_game(rei_core_game_set(game->game));
	rei_loading_title(rei_core_game_title(game->game));
	rei_settings_save(); // remember last game (even without opening the menu)
	rei_core_config cfg = {
		.zip_path = game->zip,
		.set_name = rei_core_game_set(game->game),
		.parent_zip_path = game->parent[0] ? game->parent : NULL,
		.sample_rate = REI_AUDIO_RATE,
		.bytes_per_pixel = 2,
		.color = rei_video_color,
		.log = core_log,
		.clock_us = rei_time_us,
		.interleave_mul = REI_INTERLEAVE_MUL,
		.progress = load_progress,
#ifdef REI_PIPELINED
		.pipelined = 1,
#ifdef REI_THREADED
		.threaded = 1, // sound side runs on a CPU1 thread (threads.c)
#endif
#ifdef REI_RENDER_THREAD
		.render_threaded = 1, // drawing runs on a CPU2 thread (video.c, ADR-007)
#endif
#endif
	};
	uint64_t t_load = rei_time_us();
	if (rei_core_init(&cfg) != 0) {
		fatal_screen(rei_core_error());
		goto out;
	}
	REI_LOG("core: %s, init %u ms", rei_core_game_name(), (unsigned)((rei_time_us() - t_load) / 1000));
	rei_loading_progress(950);
	REI_LOG("core: cheats %d (" REI_DATA_DIR "cheats/)", rei_core_cheats_load(REI_DATA_DIR "cheats/"));
	(void)t_load;
	if (g_settings.dip_count > 0)
		rei_core_dip_set_raw(g_settings.dip, g_settings.dip_count);
	rei_core_set_start_stage(g_settings.start_stage);
	rei_core_set_infinite_health(g_settings.cheat_health);
	if (!rei_core_is_m92()) {
		// other boards: the core runs the original FBNeo frame (sound inside run_frame). If the driver supports deferred
		// drawing, the CPU2 render thread draws (rei_defer, ADR-018); otherwise run_frame draws directly.
		// boards supporting the main/sound pipeline (M107, ADR-019) run the sound side on a CPU1 thread
		cfg.pipelined = rei_core_pipelined();
		cfg.threaded = rei_core_sound_threaded();
		cfg.render_threaded = rei_core_render_threaded();
		int sw, sh, sflags;
		rei_core_screen(&sw, &sh, &sflags);
		rei_video_set_screen(sw, sh, sflags);
		REI_LOG("core: board %s, screen %dx%d flags %d", rei_core_board(), sw, sh, sflags);
	}

	if (rei_video_init() != 0) {
		fatal_screen("video init failed");
		goto out_core;
	}
	// bezel artwork (user artwork/<set>.png, else the parent set)
	rei_video_set_art(rei_core_game_set(game->game), rei_core_game_parent(game->game));
	rei_audio_init();
	if (cfg.render_threaded && rei_video_start_render_thread() != 0)
		REI_LOG("video: continuing without render thread (no video)");
	if (cfg.threaded && rei_threads_start_sound() != 0)
		REI_LOG("threads: continuing without sound thread (no sound)");
	if (!cfg.threaded && rei_threads_start_ym() == 0) // other boards: YM2151 synthesis on CPU1 (same result)
		s_ym_worker = 1;

	const int alen = rei_core_audio_len();
	int16_t *audio = (int16_t *)calloc((size_t)alen * 2 + 16, sizeof(int16_t));

	uint64_t prev_frame = rei_time_us();
	uint32_t frame = 0;
	uint64_t emu_frames = 0;
	int menu_open = 0;
	for (;;) {
		uint64_t t0 = rei_time_us();
		rei_perf_record(REI_PERF_FRAME, (uint32_t)(t0 - prev_frame));
		prev_frame = t0;

		{ // keep the system from auto-suspending while running without input (demo viewing, remote measurement); same as PSP scePowerTick.
			// without it the system paused the app after ~10s of no input
			static unsigned tick;
			if (++tick % 30 == 0)
				sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
		}
		uint32_t buttons = rei_input_poll();

		// options screen: the game is paused while open and the menu is drawn over the last frame
		if (!menu_open && rei_menu_should_open(buttons))
			menu_open = 1;
		if (menu_open) {
			rei_menu_result r = rei_menu_update(buttons);
			vita2d_start_drawing();
			vita2d_clear_screen();
			if (cfg.render_threaded)
				rei_video_draw_rendered(emu_frames);
			else
				rei_video_redraw();
			rei_menu_draw();
			rei_osd_draw();
			vita2d_end_drawing();
			vita2d_swap_buffers();
			if (r == REI_MENU_EXIT)
				break;
			if (r == REI_MENU_SELECT_GAME) {
				// back to game list: show the list on next start and relaunch the app (per-game state from scratch)
				g_settings.show_list = 1;
				rei_settings_save();
				rei_threads_stop();
				rei_video_stop_render_thread();
				sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
			}
			if (r == REI_MENU_RESET)
				reset_game(&cfg, &emu_frames);
			if (r == REI_MENU_SAVE)
				save_state(rei_menu_state_slot());
			if (r == REI_MENU_LOAD)
				load_state(&cfg, &emu_frames, rei_menu_state_slot());
			if (r == REI_MENU_CLOSE || r == REI_MENU_RESET || r == REI_MENU_LOAD)
				menu_open = 0;
			continue;
		}
		// quick save SELECT+L, quick load SELECT+R (once per press)
		{
			static uint32_t prev;
			const uint32_t qs = SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER, ql = SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
			if ((buttons & qs) == qs && (prev & qs) != qs)
				save_state(REI_STATE_QUICK);
			if ((buttons & ql) == ql && (prev & ql) != ql)
				load_state(&cfg, &emu_frames, REI_STATE_QUICK);
			prev = buttons;
		}
#ifdef REI_AUTOPLAY
		rei_core_autoplay((int)emu_frames); // for measurement: same input as harness --autoplay
#endif
		rei_hud_update(buttons);

		int pitch = 0;
		void *fb = cfg.render_threaded ? NULL : rei_video_target(&pitch);
		if (cfg.threaded) {
			rei_core_run_frame(fb, pitch, NULL); // a CPU1 thread pushes sound into the ring buffer
		} else {
			rei_core_run_frame(fb, pitch, audio);
			rei_audio_push(audio, alen);
		}
		emu_frames++;
		rei_perf_end(REI_PERF_EMU, t0);
#if defined(REI_ENABLE_LOG) && defined(REI_PROFILE)
		{
			// slow frames (main > 17ms) each log their cause: sound/render wait times
			static uint64_t ws_prev, wr_prev;
			uint64_t ws, wr;
			rei_core_waits(&ws, &wr);
			uint32_t emu_us = (uint32_t)(rei_time_us() - t0);
			if (emu_us > 17000)
				REI_LOG("slow: f%llu emu %.2f wait_sound %.2f wait_render %.2f",
					(unsigned long long)emu_frames, emu_us / 1000.0,
					(ws - ws_prev) / 1000.0, (wr - wr_prev) / 1000.0);
			ws_prev = ws;
			wr_prev = wr;
		}
#endif

		vita2d_start_drawing();
		vita2d_clear_screen();
		if (cfg.render_threaded)
			rei_video_draw_rendered(emu_frames - 1); // show the previous frame the renderer finished
		else
			rei_video_draw();
		rei_fps_draw();
		rei_osd_draw();
		vita2d_end_drawing();
		vita2d_swap_buffers();
		rei_fps_tick();

#if defined(REI_ENABLE_LOG) && defined(REI_PROFILE)
		if ((frame + 1) % 60 == 0) {
			// average ms/frame per section over the last 60 frames
			uint64_t acc[REI_PROF_COUNT];
			int n = rei_core_prof_take(acc, REI_PROF_COUNT);
			char line[256];
			int len = 0;
			for (int i = 0; i < n && len < (int)sizeof(line); i++)
				len += snprintf(line + len, sizeof(line) - len, " %s %.2f",
					rei_core_prof_name(i), acc[i] / 60.0 / 1000.0);
			REI_LOG("prof:%s", line);
		}
#endif
#ifdef REI_ENABLE_LOG
		if (++frame % 300 == 0) {
			rei_audio_stats as;
			rei_audio_get_stats(&as);
			REI_LOG("audio: fill %d underrun %u overflow %u drc %dppm", as.fill, as.underruns, as.overflows, as.drc_ppm);
		}
#else
		(void)frame;
#endif
	}

	// exit is via the PS button (system). Cleanup below is for when the loop exits
	rei_threads_stop();
	rei_video_stop_render_thread();
	rei_menu_fini();
	free(audio);
	rei_audio_fini();
	rei_video_fini();
out_core:
	rei_core_exit();
out:
	REI_LOG("exit");
	rei_hud_fini();
	vita2d_fini();
	rei_log_shutdown();
	sceKernelExitProcess(0);
	return 0;
}
