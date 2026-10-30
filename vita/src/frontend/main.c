// ReiREM Vita port: frontend entry point
// One process runs the game list, then a game, then the list again (in-app game switching, no relaunch): each game is
// loaded on a loader thread while the main thread draws the loading screen, and is shut down completely (sound, render
// and YM threads stopped, core exit, game textures and audio port released) before the next one
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include "frontend/audio.h"
#include "frontend/input.h"
#include "frontend/menu.h"
#include "common/re_pad.h"
#include "frontend/settings.h"
#include "frontend/states.h"
#include "frontend/threads.h"
#include "frontend/touch.h"
#include "frontend/update.h"
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

#ifdef REI_ENABLE_LOG
// test (log builds): REI_DATA_DIR switchtest = "frames rounds" then set names, one per line. Each game runs that many
// frames (a quick save at half, its load at three quarters) and goes back to the list as "Select game" does; the sets
// take turns for that many rounds (in-app game switching, docs/DECISIONS.md)
static struct { char set[32][24]; int n, frames, total, done, active; } s_sw;
static void switchtest_load(void)
{
	FILE *f = fopen(REI_DATA_DIR "switchtest", "r");
	if (!f)
		return;
	int rounds = 0;
	if (fscanf(f, "%d %d", &s_sw.frames, &rounds) == 2)
		while (s_sw.n < 32 && fscanf(f, "%23s", s_sw.set[s_sw.n]) == 1)
			s_sw.n++;
	fclose(f);
	s_sw.total = s_sw.n * rounds;
	REI_LOG("switchtest: %d sets, %d frames each, %d games", s_sw.n, s_sw.frames, s_sw.total);
}
#endif

// pick a game in the list (log builds: the dev loop's REI_DATA_DIR rei_game file runs that game once at start)
static const rei_game_found *choose_game(const char *notice)
{
	if (g_settings.show_list) { // (old options flag; the list is always shown now, so just clear it)
		g_settings.show_list = 0;
		rei_settings_save();
	}
#ifdef REI_ENABLE_LOG
	s_sw.active = 0;
	if (s_sw.done < s_sw.total) {
		static rei_game_found found[REI_GAMES_MAX];
		int present = 0;
		const int n = rei_games_scan(k_rom_dirs, ROM_DIRS, found, REI_GAMES_MAX, &present);
		const char *want = s_sw.set[s_sw.done % s_sw.n];
		s_sw.done++;
		for (int i = 0; i < n; i++)
			if (found[i].present && !strcmp(rei_core_game_set(found[i].game), want)) {
				REI_LOG("switchtest: game %d/%d %s", s_sw.done, s_sw.total, want);
				s_sw.active = 1;
				return &found[i];
			}
		REI_LOG("switchtest: %s not found", want);
	}
	static int autostart_done;
	if (!autostart_done) {
		autostart_done = 1;
		FILE *f = fopen(REI_DATA_DIR "rei_game", "r");
		if (f) {
			static rei_game_found found[REI_GAMES_MAX];
			char want[24] = { 0 };
			if (fgets(want, sizeof(want), f))
				want[strcspn(want, "\r\n")] = 0;
			fclose(f);
			int present = 0;
			const int n = rei_games_scan(k_rom_dirs, ROM_DIRS, found, REI_GAMES_MAX, &present);
			for (int i = 0; i < n; i++)
				if (found[i].present && !strcmp(rei_core_game_set(found[i].game), want)) {
					REI_LOG("games: %s (rei_game)", want);
					return &found[i];
				}
		}
	}
#endif
	return rei_game_select(k_rom_dirs, ROM_DIRS, ROM_DIR, notice);
}

static void load_progress(int permille, void *user)
{
	(void)user;
	rei_loading_set(permille);
}

// The core init (ROM zip reading, graphics decoding) runs on this thread (CPU1, idle until the game starts); the main
// thread only draws the loading screen, one frame per vblank, until it is done (as ReSys12 ADR-048)
static struct {
	const rei_core_config *cfg;
	int rc;
	volatile int done;
} s_ld;
static int load_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_ld.rc = rei_core_init(s_ld.cfg);
	__atomic_store_n(&s_ld.done, 1, __ATOMIC_RELEASE);
	return sceKernelExitDeleteThread(0);
}
static int core_init_shown(const rei_core_config *cfg)
{
	s_ld.cfg = cfg;
	s_ld.rc = -1;
	s_ld.done = 0;
	const SceUID th = sceKernelCreateThread("rei_load", load_thread, 0x60, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) { // (no thread: load here, the screen waits)
		if (th >= 0)
			sceKernelDeleteThread(th);
		REI_LOG("load: no loader thread (0x%08x)", (unsigned)th);
		return rei_core_init(cfg);
	}
	while (!__atomic_load_n(&s_ld.done, __ATOMIC_ACQUIRE))
		rei_loading_frame();
	sceKernelWaitThreadEnd(th, NULL, NULL);
	return s_ld.rc;
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
	rei_menu_states_changed();
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

// keep the system from auto-suspending while running without input (demo viewing, remote measurement); same as PSP
// scePowerTick. Without it the system paused the app after ~10s of no input
static void power_tick(void)
{
	static unsigned tick;
	if (++tick % 30 == 0)
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

enum { GAME_LIST, GAME_EXIT };
static char s_notice[256]; // shown in the game list next time (a game that did not start)

// one game from loading to its end. Returns GAME_LIST (back to the game list) or GAME_EXIT (the app exits)
static int run_game(const rei_game_found *game)
{
	const char *set = rei_core_game_set(game->game);
	REI_LOG("game: %s", set);
	rei_settings_load_game(set);
	rei_settings_save(); // remember last game (even without opening the menu)
	rei_loading_title(rei_core_game_title(game->game));
	rei_loading_progress(0);
	rei_core_config cfg = {
		.zip_path = game->zip,
		.set_name = set,
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
	const uint64_t t_load = rei_time_us();
	if (core_init_shown(&cfg) != 0) {
		REI_LOG("core: %s init failed: %s", set, rei_core_error());
		snprintf(s_notice, sizeof(s_notice), "%s did not start\n%s", set, rei_core_error());
		rei_loading_done();
		return GAME_LIST;
	}
	REI_LOG("core: %s, init %u ms", rei_core_game_name(), (unsigned)((rei_time_us() - t_load) / 1000));
	(void)t_load;
	rei_loading_set(950);
	REI_LOG("core: cheats %d (" REI_DATA_DIR "cheats/)", rei_core_cheats_load(REI_DATA_DIR "cheats/"));
	if (g_settings.dip_count > 0)
		rei_core_dip_set_raw(g_settings.dip, g_settings.dip_count);
	rei_core_set_start_stage(g_settings.start_stage);
	rei_core_set_infinite_health(g_settings.cheat_health);
	int sw = REI_SCREEN_W, sh = REI_SCREEN_H, sflags = 0;
	if (!rei_core_is_m92()) {
		// other boards: the core runs the original FBNeo frame (sound inside run_frame). If the driver supports deferred
		// drawing, the CPU2 render thread draws (rei_defer, ADR-018); otherwise run_frame draws directly.
		// boards supporting the main/sound pipeline (M107, ADR-019) run the sound side on a CPU1 thread
		cfg.pipelined = rei_core_pipelined();
		cfg.threaded = rei_core_sound_threaded();
		cfg.render_threaded = rei_core_render_threaded();
		rei_core_screen(&sw, &sh, &sflags);
		REI_LOG("core: board %s, screen %dx%d flags %d", rei_core_board(), sw, sh, sflags);
	}
	rei_video_set_screen(sw, sh, sflags);
	rei_menu_set_game(set, rei_core_game_name(), rei_core_button_count());
	rei_loading_progress(1000);
	rei_loading_done();

	if (rei_video_init() != 0) {
		snprintf(s_notice, sizeof(s_notice), "%s did not start\nvideo init failed", set);
		rei_video_fini();
		rei_core_exit();
		return GAME_LIST;
	}
	// bezel artwork (user artwork/<set>.png, else the parent set, else the app's blank artwork)
	rei_video_set_art(set, rei_core_game_parent(game->game));
	rei_audio_init();
	s_ym_worker = 0;
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
	int menu_open = 0, result = GAME_LIST;
#ifdef REI_ENABLE_LOG
	uint64_t tframes = 0; // (switchtest: frames of this game, a state load included)
#endif
	for (;;) {
		uint64_t t0 = rei_time_us();
		rei_perf_record(REI_PERF_FRAME, (uint32_t)(t0 - prev_frame));
		prev_frame = t0;
		power_tick();
		uint32_t buttons = rei_input_poll();

		// options screen: the game is paused while open and the menu is drawn over the last frame
		if (!menu_open && rei_menu_should_open(buttons)) {
			menu_open = 1;
			REI_LOG("menu: open f%llu", (unsigned long long)emu_frames);
			rei_fps_hide(); // (it covered the menu, e.g. Done of the touch layout editor)
			rei_touch_overlay_update(1);
		}
		if (menu_open) {
			rei_menu_result r = rei_menu_update(buttons);
			rei_menu_draw();
			vita2d_start_drawing();
			vita2d_clear_screen();
			if (cfg.render_threaded)
				rei_video_draw_rendered(emu_frames);
			else
				rei_video_redraw();
			rei_video_draw_layers();
			vita2d_end_drawing();
			vita2d_swap_buffers();
			rei_osd_tick();
			if (r == REI_MENU_EXIT) {
				result = GAME_EXIT;
				break;
			}
			if (r == REI_MENU_SELECT_GAME)
				break;
			if (r == REI_MENU_RESET)
				reset_game(&cfg, &emu_frames);
			if (r == REI_MENU_SAVE)
				save_state(rei_menu_state_slot());
			if (r == REI_MENU_LOAD)
				load_state(&cfg, &emu_frames, rei_menu_state_slot());
			if (r != REI_MENU_STAY)
				REI_LOG("menu: result %d", (int)r);
			if (r == REI_MENU_CLOSE || r == REI_MENU_RESET || r == REI_MENU_LOAD) {
				menu_open = 0;
				rei_menu_hide();
				rei_video_apply_settings(); // (rotation, scanlines may have changed)
			}
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
#ifdef REI_ENABLE_LOG
		if (s_sw.active && s_sw.frames > 0) { // (switchtest: autoplay input, a save and a load, then back to the list)
			rei_core_autoplay((int)emu_frames);
			tframes++;
			if (tframes == (uint64_t)s_sw.frames / 2)
				save_state(REI_STATE_QUICK);
			if (tframes == (uint64_t)s_sw.frames * 3 / 4)
				load_state(&cfg, &emu_frames, REI_STATE_QUICK);
			if (tframes >= (uint64_t)s_sw.frames)
				break;
		}
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

		rei_touch_overlay_update(0);
		vita2d_start_drawing();
		vita2d_clear_screen();
		if (cfg.render_threaded)
			rei_video_draw_rendered(emu_frames - 1); // show the previous frame the renderer finished
		else
			rei_video_draw();
		rei_video_draw_layers();
		vita2d_end_drawing();
		vita2d_swap_buffers();
		rei_fps_tick();
		rei_osd_tick();

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

	// game end: everything this game started stops, then the core and its buffers go (the next game in this process
	// starts from scratch, as a fresh launch would; the harness checks it hashes the same, REI_PRERUN)
	rei_menu_hide();
	rei_fps_hide();
	rei_touch_overlay_update(1);
	rei_threads_stop();
	rei_video_stop_render_thread();
	s_ym_worker = 0;
	rei_input_release();
	free(audio);
	rei_audio_fini();
	rei_video_fini();
	rei_core_exit();
	{
		SceKernelFreeMemorySizeInfo fm;
		fm.size = sizeof(fm);
		struct mallinfo mi = mallinfo(); // (newlib heap in use: a leak per game would grow it)
		if (sceKernelGetFreeMemorySize(&fm) < 0)
			fm.size_user = fm.size_cdram = 0;
		REI_LOG("game end (%s): free user %u KB cdram %u KB, heap in use %u KB", result == GAME_EXIT ? "exit" : "list",
			fm.size_user >> 10, fm.size_cdram >> 10, (unsigned)mi.uordblks >> 10);
		(void)mi;
	}
	return result;
}

int main(int argc, char *argv[])
{
	rei_log_init();
	rei_threads_pin_main();
#ifdef REI_CLOCK_SWEEP
	rei_clock_sweep();
#endif
	rei_clock_boost();
	{
		int restarted = 0; // started again by rei_kctrl_init ("kctrl")
		for (int i = 0; i < argc; i++)
			restarted |= argv[i] && !strcmp(argv[i], "kctrl");
		rei_kctrl_init(restarted); // (a first start after boot loads the kernel helper and restarts once)
		// it fails when the system has run long with many kernel modules loaded (0x80024302, no free physical page):
		// the PS button then goes straight home, so say why
		if (!rei_kctrl_ready())
			snprintf(s_notice, sizeof(s_notice), "PS button menu not available (system memory is low)\n"
				"Restart the Vita to use the PS button for the menu");
	}
	rei_update_before_exit = rei_ps_release;

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
	// pre-create the folders users fill
	sceIoMkdir(REI_DATA_DIR, 0777);
	sceIoMkdir(ROM_DIR, 0777);
	sceIoMkdir(REI_DATA_DIR "cheats", 0777);  // user cheat file <set>.ini (FBNeo format)
	sceIoMkdir(REI_DATA_DIR "artwork", 0777); // user bezel artwork <set>.png (.jpg .bmp)
	rei_touch_init();
	rei_settings_load();
	// input engine (setting): RePad = the Bluetooth controller plugin from the app, unless it already runs (ur0:tai, or
	// another Re app); Native = whatever the system has
	g_settings.input_engine = re_pad_engine_init(g_settings.input_engine, "ux0:app/M92A00001/module/repad.skprx"); // (the kernel cannot open app0:)
	REI_LOG("input engine: %s, RePad %s", re_pad_engine_name(g_settings.input_engine), re_pad_plugin() ? "running" : "not running");
	rei_menu_init();
#ifdef REI_ENABLE_LOG
	switchtest_load();
#endif
	// GitHub releases, on its own thread (the game list offers a newer one once per run)
	rei_update_check_start();

	for (;;) {
		const rei_game_found *game = choose_game(s_notice[0] ? s_notice : NULL);
		s_notice[0] = 0;
		if (!game || run_game(game) == GAME_EXIT)
			break;
	}

	REI_LOG("exit");
	rei_audio_shutdown();
	rei_menu_fini();
	rei_hud_fini();
	vita2d_fini();
	rei_ps_release();
	rei_log_shutdown();
	sceKernelExitProcess(0);
	return 0;
}
