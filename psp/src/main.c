// ReiREM PSP port — P1: runs the shared core on the single-thread pipeline path (SOUND_ON_MAIN)
// The core produces palette-index buffers; color conversion and output are done by the GE (psp/docs/M92_VIDEO.md G1).
// For the first VERIFY_FRAMES frames, the CPU computes the same hash as the harness (--mode pipelined --autoplay) and logs it every 60 frames,
// then reads back the GE-drawn screen and compares it pixel by pixel with the CPU conversion. After that, measurement only. START exits.
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspdmac.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <psputils.h>

#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "menu.h"
#include "rei_core.h"
#include "rei_games.h"
#include "settings.h"
#include "ui.h"
#include "video.h"
#include "ge_m72.h"
#ifdef REI_PSP_ME
#include <kubridge.h>
#endif
#ifdef REI_PSP_ME
#include <pspaudio.h>

#include "me_sound.h"
#include "overclock.h"
#include "rei_me.h"
#include "rei_prof.h"
#endif

PSP_MODULE_INFO("reirem", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024); // nearly all remaining user memory goes to the heap (core graphics + ROMs about 15.2MB)

#define AUDIO_RATE 48000
#define OTHER_AUDIO_RATE 24000 // other boards: halve sound chip synthesis (PSP CPU budget, ADR-023)
// (experimental) other boards: core writes the screen buffer via an uncached address (no line fill, no end-of-frame flush, same pixels)
#define OTHER_FB_UNCACHED 1
#ifdef OTHER_FB_UNCACHED
#define OTHER_FB_W(p) ((uint16_t *)((uintptr_t)(p) | 0x40000000u))
#else
#define OTHER_FB_W(p) (p)
#endif
static int g_audio_rate = AUDIO_RATE;
#define HASH_EVERY 60
#ifdef REI_PSP_PLAYER
#define VERIFY_FRAMES 0 // play build: no verification
#else
#define VERIFY_FRAMES 600
#endif
static int g_xverify_from = -1, g_xverify_to = -1, g_xverify_bad; // extra GE check range (host0:/rei_verify)
// Measurement saves (PSP-made only: state format differs from PC build). host0:/rei_save "file frame" = save at start of that frame,
// host0:/rei_load "file frame" = load at startup (before starting the ME) and continue autoplay from that frame
static int g_af_offset;           // frame of the loaded save (added to autoplay input and HUD frame number)
#ifndef REI_PSP_SPAN_MAX
#define REI_PSP_SPAN_MAX 64        // max main line spans (core rei_span_max_lines)
#endif
extern int rei_span_max_lines;
static int g_save_at = -1;
static unsigned g_bench_cpu, g_bench_mem, g_bench_me, g_bench_mec, g_bench_meu, g_bench_mu; // diag: startup speed (us, also in power log line)
static unsigned g_addr_tiles, g_addr_atlas, g_addr_heap, g_addr_code; // diag: large buffer addresses
static char g_save_path[128];
static int read_name_frame(const char *file, char *path, int path_len, int *frame)
{
	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	char b[96] = { 0 };
	sceIoRead(fd, b, sizeof(b) - 1);
	sceIoClose(fd);
	char *sp = strchr(b, ' ');
	if (!sp)
		return -1;
	*sp = 0;
	*frame = (int)strtol(sp + 1, NULL, 10);
	snprintf(path, path_len, "host0:/%s", b);
	return 0;
}

// With sound on the ME, drawing lags one frame: while emulating frame N and waiting on the sound side, main
// draws N-1 in pieces (rei_core_render_step). Otherwise the ME idles while main draws at frame end
#if defined(REI_PSP_ME) && !defined(REI_PSP_G1)
#define REI_PSP_DEFER 1
#else
#define REI_PSP_DEFER 0
#endif

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

void __psp_free_heap(void); // pspsdk C runtime (for dev-loop stop)

#ifdef REI_DRC
// V33 dynarec (ADR-020): flush generated code from D cache and invalidate I cache
extern void (*rei_drc_cache_flush)(void *p, int len);
extern unsigned long long rei_drc_stats[13];
static void drc_cache_flush(void *p, int len)
{
	sceKernelDcacheWritebackRange(p, (unsigned)len);
	sceKernelIcacheInvalidateRange(p, (unsigned)len);
}

// If this frame finished early enough to wait for vblank, use the spare time to build deferred dynarec blocks (core drc_idle_compile).
// left_us = time left until next vblank (approx, -1 if late). Keep a 2.5ms margin
// V35 per-frame compile budget: low after a frame with spare time, late after a late frame (low 0 = fixed 2000). Overflow blocks are built in
// spare time. bombrman scene change, V35 2000 instrs: one frame 34.8 -> 23.1ms (together with V35 auto idle always on — see rei_v25_auto_drc_policy).
// With late = 2000, bombrman spikes remained (frame after a late one had 2000 instrs). Test: host0:/rei_drc35low "low late"
static int g_drc35_low = 500, g_drc35_late = 500;
static int g_drc33_low = 500, g_drc33_late = 2000; // (same scheme for V33/V30, test: host0:/rei_drc33low "low late")
#ifdef REI_PROFILE
static uint64_t g_prof60[REI_PROF_COUNT]; // (profile) other-board loop: span sums per frame, logged and cleared every 60 frames
#endif
#ifdef REI_PSP_ME
static int g_mix; // (see mode 3 below)
#endif
static void drc_idle_work(int left_us)
{
	extern int rei_drc_idle_on;
	extern int rei_drc_idle_compile(unsigned budget_us), rei_drc35_idle_compile(unsigned budget_us);
	extern void rei_drc_set_budget(int n), rei_drc35_set_budget(int n);
	if (!rei_drc_idle_on)
		return;
	// After a late frame (no vblank to wait for, left_us < 0) restore budget to 2000: with no spare time, pending blocks keep
	// running in the interpreter and airass scene change ran 43~49ms for 9 frames (4 frames with budget 2000)
	rei_drc_set_budget(left_us < 0 ? g_drc33_late : g_drc33_low);
#ifdef REI_PSP_ME
	if (g_mix) { // (mode 3: V35 runs on the ME — its state is ME-side data, main does not touch it)
		if (left_us >= 3000)
			rei_drc_idle_compile((unsigned)(left_us - 2500));
		return;
	}
#endif
	if (g_drc35_low > 0)
		rei_drc35_set_budget(left_us < 0 ? g_drc35_late : g_drc35_low);
	if (left_us < 3000)
		return;
	const uint32_t t0 = sceKernelGetSystemTimeLow();
	const int budget = left_us - 2500;
	rei_drc_idle_compile((unsigned)budget);
	const int used = (int)(sceKernelGetSystemTimeLow() - t0);
	if (budget - used > 300)
		rei_drc35_idle_compile((unsigned)(budget - used));
}
#endif

static volatile int g_quit;
static volatile int g_to_list;      // the game ended for Select game: back to the game list in this process
static volatile int g_playing;      // in a game's frame loop (test triggers act only then)
#if defined(REI_LOG_ENABLED) || defined(REI_PSP_TESTGAME)
// (test) host0:/rei_select: Select game as in the options screen, during play (play_check in-app switching; the list then picks
// the game named in host0:/rei_game). Polled about once a second by the watchdog / test thread
static void select_poll(void)
{
	SceIoStat st;
	if (g_playing && !g_quit && sceIoGetstat("host0:/rei_select", &st) >= 0) {
		sceIoRemove("host0:/rei_select");
		g_to_list = 1;
		g_quit = 1;
	}
}
#endif

#ifdef REI_LOG_ENABLED
// Dev-loop watchdog: if frames make no progress for over 6s, or a stop request (host0:/rei_stop) goes unanswered for over 4s,
// exit the game (to XMB). run.sh then relaunches REIREM DEV so the next run continues unattended
static volatile int g_wd_frame = -1, g_wd_beat, g_wd_done; // beat: main loop iterations (keeps counting while the menu pauses the game)
static SceUID g_wd_thread = -1;
static int exit_callback(int arg1, int arg2, void *common);
#ifdef REI_PSP_ME
static void mix_stall_diag(void); // (mode 3 diag, below)
#endif
static int watchdog_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int last = -1, still = 0, stop_wait = 0, init = 0;
	for (;;) { // (whole app: g_wd_done = no game running — the game list, between games)
		sceKernelDelayThread(1000000);
		const int f = g_wd_frame, beat = g_wd_beat;
		still = (f >= 0 && beat == last && !g_wd_done) ? still + 1 : 0;
		init = f < 0 && !g_wd_done ? init + 1 : 0; // startup (ROM load, conversion, overclock) not finished
		last = beat;
		SceIoStat st;
		stop_wait = sceIoGetstat("host0:/rei_stop", &st) >= 0 && !g_wd_done ? stop_wait + 1 : 0;
		select_poll();
		if (sceIoGetstat("host0:/rei_quit", &st) >= 0) // same path as HOME "Quit game" (exit check)
			g_quit = 1;
		if (sceIoGetstat("host0:/rei_home", &st) >= 0) { // (diag) call the HOME > Quit game callback directly (goes to XMB when done)
			sceIoRemove("host0:/rei_home");
			exit_callback(0, 0, NULL);
		}
		if (still == 2 || still == 4) { // diag: frames stalled 2s — main/sound progress (again at 4s: is the ME running or stopped)
			long long md, sd, sw;
			rei_core_pipe_lines(&md, &sd, &sw);
			REI_LOG("reirem: stall at frame %d: main line %lld, sound line %lld, sound waits at %lld, ME frames %u\n", f, md, sd,
				sw, (unsigned)me_sound_frames());
#ifdef REI_DRC
			{ // (diag) ME V35 dynarec stage (v25.cpp rei_drc35_me_diag)
				extern volatile uint32_t rei_drc35_me_diag[4];
				const volatile uint32_t *d = (const volatile uint32_t *)((uintptr_t)rei_drc35_me_diag | 0x40000000u);
				REI_LOG("reirem: V35 on ME: stage %u compiles %u runs %u key %08x\n", (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3]);
			}
#endif
#ifdef REI_PSP_ME
			mix_stall_diag();
#endif
#ifdef REI_PSP_ME
			{ // (diag) where ME rendering stalled (rei_defer.cpp rei_defer_diag)
				extern uint32_t rei_defer_diag[8];
				const volatile uint32_t *d = (const volatile uint32_t *)((uintptr_t)rei_defer_diag | 0x40000000u);
				REI_LOG("reirem: ME render diag: slot %u stage %u nseg %u seg %u first %u..%u, rendered %u, nseg again %u, seg lines %u..%u,"
					" ME frames %u\n", (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3], (unsigned)d[4], (unsigned)d[5],
					(unsigned)me_render_done(), (unsigned)d[6], (unsigned)(d[7] & 0xffff), (unsigned)(d[7] >> 16),
					(unsigned)me_render_frames());
			}
#endif
		}
#ifdef REI_PSP_ME
		{ // Do not exit while stuck in ME startup: exiting with a half-switched ME powered the device off (restore -2, 2026-10-16 bombrman
		  // "rpc cache 1" + 60s) and the stall point was lost. Leave it and inspect with PSPLink thlist/thctx (recurrence log 10)
			extern volatile int me_sound_starting;
			static int told;
			if (me_sound_starting && (init >= 60 || still >= 6)) {
				if (!told++)
					REI_LOG("reirem: watchdog: stuck in ME start, leaving it for inspection (thlist)\n");
				continue;
			}
		}
#endif
		if (!g_wd_done && (still >= 6 || stop_wait >= 4 || init >= 60)) {
			REI_LOG("reirem: watchdog: %s at frame %d, exiting the game\n", still >= 6 ? "no progress" : "stop not handled", f);
			// First release the core wait and let main exit by itself (exiting while main busy-waits stalled the system
			// exit thread at the same priority). If not done within 3s, exit here
			g_quit = 1;
			rei_core_stop();
			for (int i = 0; i < 30 && !g_wd_done; i++)
				sceKernelDelayThread(100000);
			if (g_wd_done)
				break;
			sceKernelDelayThread(200000);
#ifdef REI_PSP_ME
			// Do not exit while the ME runs our code (release core wait and end the ME loop, up to 2s)
			me_sound_stop();
			me_sound_prepare_exit();
#endif
			sceKernelExitGame();
		}
	}
	return 0;
}
#endif

// HOME > Quit game. If the callback only sets a flag and returns, the system proceeds with exit and stops display updates (vblank), and
// main's cleanup running afterwards (rebooting the ME to firmware) waited on vblank forever ("wait" screen, device thlist: user_main in
// SceDisplayVblank wait, 2026-10-12). Per PSP convention, exit inside the callback: if in the game loop, wait for main to leave the loop
// and finish cleanup, then sceKernelExitGame (release main if it is in the loop's vblank wait). Before the game (list/loading:
// ME not yet switched), exit immediately
static SceUID g_main_thid = -1;
static volatile int g_game_loop;    // after switching the ME (cleanup needed)
static volatile int g_loop_left;    // left the game loop
static volatile int g_home_exit;    // HOME exit: main cleans up but leaves the exit to the callback
static volatile int g_cleanup_done; // main's cleanup is done
static int g_list_stop;             // (dev loop) stop request while the game list was up
enum { REI_RUN_DONE, REI_RUN_DEV_STOP }; // how run_m92 / run_other_board ended
static int exit_callback(int arg1, int arg2, void *common)
{
	(void)arg1;
	(void)arg2;
	(void)common;
	REI_LOG("reirem: home: exit callback, game loop %d\n", (int)g_game_loop);
#ifdef REI_PSP_ME
	me_sound_trace("home: callback, game loop", (int)g_game_loop);
#endif
	if (g_game_loop) {
		g_home_exit = 1;
		g_quit = 1;
		for (int i = 0; i < 1000 && !g_cleanup_done; i++) { // (max 10s)
			if (!g_loop_left)
				sceKernelReleaseWaitThread(g_main_thid); // (only releases the loop's vblank wait; cleanup is left alone)
			if (i % 100 == 99)
				REI_LOG("reirem: home: waiting %d s, loop left %d, cleanup %d\n", (i + 1) / 100, (int)g_loop_left,
					(int)g_cleanup_done);
			sceKernelDelayThread(10000);
		}
		REI_LOG("reirem: home: cleanup %d, exit game\n", (int)g_cleanup_done);
	}
#ifdef REI_PSP_ME
	me_sound_trace("home: exit game, cleanup", (int)g_cleanup_done);
#endif
	g_quit = 1;
	sceKernelExitGame();
	return 0;
}

// Sleep (power switch): the ME powers off during sleep and restarts on firmware, losing our ME code (sound/drawing) — on wake
// main waited for the ME forever and hung (reported 2026-10-17). During play, lock power to defer sleep; on switch press,
// main at a frame boundary saves a temp state -> ME to firmware -> unlock and sleep -> on wake restart the ME and load the temp state
static volatile int g_sleep_req, g_resumed, g_power_locked, g_power_flags;
static int power_callback(int unknown, int flags, void *common)
{
	(void)unknown;
	(void)common;
	g_power_flags = flags;
	if (g_power_locked && (flags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)))
		g_sleep_req = 1;
	if (flags & PSP_POWER_CB_RESUME_COMPLETE)
		g_resumed = 1;
	return 0;
}
static void power_lock(int on)
{
	if (on && !g_power_locked) {
		scePowerLock(0);
		g_power_locked = 1;
	} else if (!on && g_power_locked) {
		g_power_locked = 0;
		scePowerUnlock(0);
	}
}

static int callback_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int cbid = sceKernelCreateCallback("exit", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	int pcb = sceKernelCreateCallback("power", power_callback, NULL);
	scePowerRegisterCallback(-1, pcb);
	sceKernelSleepThreadCB();
	return 0;
}

static void core_log(const char *msg)
{
	REI_LOG("core: %s\n", msg);
	(void)msg;
}

// App folder (where the EBOOT is, ends with '/'). ROMs go in roms/ below it (folder created at startup)
static char g_app_dir[96] = "ms0:/PSP/GAME/REIREM/";

// The XMB game list shows folders with newer dates on top. Set the install folder date to 2099-01-01 so it shows first (if the PSP
// clock was wrong at install, e.g. 2011, it ended up at the bottom). path is a folder path not ending in '/'
static void bump_folder_date(const char *path)
{
	SceIoStat st;
	if (sceIoGetstat(path, &st) < 0 || st.sce_st_mtime.year >= 2099)
		return;
	ScePspDateTime t;
	memset(&t, 0, sizeof(t));
	t.year = 2099;
	t.month = 1;
	t.day = 1;
	st.sce_st_ctime = st.sce_st_atime = st.sce_st_mtime = t;
	sceIoChstat(path, &st, 0x0008 | 0x0010 | 0x0020); // FIO_CST_CT | AT | MT
}

static void set_app_dir(int argc, char *argv[])
{
	if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
		snprintf(g_app_dir, sizeof(g_app_dir), "%s", argv[0]);
		strrchr(g_app_dir, '/')[1] = 0;
	}
	if (strncmp(g_app_dir, "host", 4) != 0) { // move install folder to top of XMB list
		char d[96];
		snprintf(d, sizeof(d), "%s", g_app_dir);
		d[strlen(d) - 1] = 0;
		bump_folder_date(d);
	} else { // dev loop: default install location (applied when run once after install)
		bump_folder_date("ms0:/PSP/GAME/REIREM");
	}
	if (strncmp(g_app_dir, "host", 4) != 0) { // (not created for the PSPLink dev loop)
		static const char *const sub[] = { "roms", "cheats", "artwork" }; // ROMs, cheats <set>.ini, off-screen artwork <set>.png
		for (int i = 0; i < 3; i++) {
			char d[112];
			snprintf(d, sizeof(d), "%s%s", g_app_dir, sub[i]);
			sceIoMkdir(d, 0777);
		}
	}
}

// Find supported games with ROMs and pick one (list if more than one, cursor on last game). NULL if none. Call after video_init.
// Folder candidates (first hit per game, rei_games.c): PSPLink host1: (run.sh exports the ROM folder), app folder roms/, app folder,
// default install location, old-name (M92 Arcade, nbbatman only) install location (Memory Stick, PSP go internal memory)
static const rei_game_found *choose_game(void)
{
	static char dirs[12][112];
	static const char *pdirs[12];
	int nd = 0;
	snprintf(dirs[nd++], sizeof(dirs[0]), "host1:/");
	snprintf(dirs[nd++], sizeof(dirs[0]), "%sroms/", g_app_dir);
	snprintf(dirs[nd++], sizeof(dirs[0]), "%s", g_app_dir);
	static const char *const fixed[] = {
		"ms0:/PSP/GAME/REIREM/roms/", "ef0:/PSP/GAME/REIREM/roms/", "ms0:/PSP/GAME/M92ARCADE/roms/", "ef0:/PSP/GAME/M92ARCADE/roms/", "ms0:/PSP/GAME/NBBATMAN/roms/",
		"ms0:/PSP/GAME/NBBATMAN/", "ef0:/PSP/GAME/NBBATMAN/roms/", "ef0:/PSP/GAME/NBBATMAN/",
	};
	for (int i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++)
		snprintf(dirs[nd++], sizeof(dirs[0]), "%s", fixed[i]);
	for (int i = 0; i < nd; i++)
		pdirs[i] = dirs[i];
	static rei_game_found found[REI_GAMES_MAX];
	static int n = -1; // (scanned once; again on Refresh ROM list in the list's options)
	if (n < 0) {
		n = rei_games_scan(pdirs, nd, found, REI_GAMES_MAX, NULL);
		// All boards run (M92 via GE/ME path, others via run_other_board). An M92-only build blocks games that are name entries only
#ifdef REI_M92_ONLY
		for (int i = 0; i < n; i++)
			found[i].supported = !strcmp(rei_core_game_board(found[i].game), "M92");
#endif
		int present = 0;
		for (int i = 0; i < n; i++)
			present += found[i].present && found[i].supported;
		REI_LOG("reirem: %d games, %d playable\n", n, present);
	}
	if (n <= 0)
		return NULL;
	if (g_settings.show_list) { // (options screen Select game flag — the list is always shown now, so just clear it)
		g_settings.show_list = 0;
		rei_settings_save();
	}
#if defined(REI_LOG_ENABLED) || defined(REI_PSP_TESTGAME)
	{ // dev loop: if host0:/rei_game holds a set name, run that game without the list (for measurement/test scripts)
#ifdef REI_PSP_TESTGAME
		// (test builds start as the installed copy: host0: is absent while USB reconnects after a game switch — wait up to 15s)
		for (int i = 0; i < 150; i++) {
			const SceUID d = sceIoDopen("host0:/");
			if (d >= 0) {
				sceIoDclose(d);
				break;
			}
			sceKernelDelayThread(100000);
		}
#endif
		SceUID fd = sceIoOpen("host0:/rei_game", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[16] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			b[strcspn(b, "\r\n ")] = 0;
			for (int i = 0; i < n; i++)
				if (found[i].present && found[i].supported && !strcmp(rei_core_game_set(found[i].game), b))
					return &found[i];
		}
	}
#endif
	int initial = -1;
	for (int i = 0; i < n; i++)
		if (!strcmp(rei_core_game_set(found[i].game), g_settings.last_game))
			initial = i;
	if (initial >= 0 && !found[initial].supported)
		initial = -1;
	if (initial < 0) // no last game: first runnable game
		for (int i = 0; i < n && initial < 0; i++)
			if (found[i].present && found[i].supported)
				initial = i;
	static char rom_dir[112];
	snprintf(rom_dir, sizeof(rom_dir), "%sroms/", g_app_dir);
	for (;;) {
		const int k = rei_game_select(found, n, initial, rom_dir);
		if (k >= 0)
			return &found[k];
		if (k == REI_LIST_STOP) {
			g_list_stop = 1;
			return NULL;
		}
		if (k == REI_LIST_QUIT)
			return NULL; // (HOME exit: g_quit)
		// REI_LIST_REFRESH: read the ROM folders again, the cursor stays on the same game
		const int keep = rei_game_select_cursor();
		const int was = keep >= 0 && keep < n ? found[keep].game : -1;
		n = rei_games_scan(pdirs, nd, found, REI_GAMES_MAX, NULL);
		initial = -1;
		for (int i = 0; i < n; i++)
			if (found[i].game == was)
				initial = i;
		REI_LOG("reirem: ROM list refreshed: %d games\n", n);
	}
}

static void load_progress(int permille, void *user) // (core progress, on the loader thread: the main thread draws)
{
	(void)user;
	rei_loading_set(permille);
}

// Core init on a loader thread while the main thread draws the loading screen every vblank (the core reads and unpacks the ROMs
// for seconds; drawing from the progress callback stuttered). Same CPU, lower priority: the loader runs whenever main waits for
// vblank. The core's per-CPU state needs nothing per thread on the PSP (nec_intf keys the main CPU by the stack, not the thread)
typedef struct {
	const rei_core_config *cfg;
	volatile int done;
	int rc;
} load_job;
static int load_thread(SceSize args, void *argp)
{
	load_job *j = *(load_job **)argp;
	(void)args;
	j->rc = rei_core_init(j->cfg);
	j->done = 1;
	return 0;
}
static int core_init_threaded(const rei_core_config *cfg)
{
	static load_job job;
	load_job *pj = &job;
	job.cfg = cfg;
	job.done = 0;
	job.rc = -1;
	const SceUID t = sceKernelCreateThread("rei_load", load_thread, 0x30, 0x40000, THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
	if (t < 0 || sceKernelStartThread(t, sizeof(pj), &pj) < 0) {
		if (t >= 0)
			sceKernelDeleteThread(t);
		REI_LOG("reirem: loader thread failed %08x, loading on main\n", (unsigned)t);
		return rei_core_init(cfg);
	}
	while (!job.done) {
		rei_loading_frame();
		sceDisplayWaitVblankStart();
	}
	sceKernelWaitThreadEnd(t, NULL);
	sceKernelDeleteThread(t);
	return job.rc;
}

static int on_me(void)
{
	uintptr_t sp;
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	return (sp >> 31) != 0; // Media Engine uses a kernel-segment stack (same check as nec_intf.cpp)
}

#if defined(REI_PSP_ME) && defined(REI_PROFILE)
// The ME has no readable clock (cp0 $9, system clock 0xBC600000, perf counters all appear frozen — tested 2026-10-09).
// Profile builds only: a small main-side thread writes system time to a shared variable every 0.5ms and the ME reads it.
// Coarse resolution, but many samples keep the ms/frame average unbiased. About 1~2% load on main
static volatile uint32_t g_clock_sh REI_ME_SHARED;
static volatile int g_clock_quit;
static SceUID g_clock_thread = -1;
static int clock_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (!g_clock_quit) {
		REI_SH(g_clock_sh) = sceKernelGetSystemTimeLow();
		sceKernelDelayThread(500);
	}
	return 0;
}
// ME only (lines held only in the ME cache)
static uint32_t g_me_clk_last __attribute__((section(".rei_me_local"), aligned(64)));
static uint32_t g_me_clk_high __attribute__((section(".rei_me_local")));
#endif

static uint64_t now_us(void)
{
	if (on_me()) {
#if defined(REI_PSP_ME) && defined(REI_PROFILE)
		uint32_t v = REI_SH(g_clock_sh);
		if (v < g_me_clk_last)
			g_me_clk_high++;
		g_me_clk_last = v;
		return ((uint64_t)g_me_clk_high << 32) | v;
#else
		return 0;
#endif
	}
	return sceKernelGetSystemTimeWide();
}

#ifdef REI_SFCOUNT
// diag: soft double op call counts and call sites (CMake REI_SFCOUNT wraps libgcc functions with --wrap)
#define SF_H 1024
static uint32_t sf_n, sf_ra[SF_H], sf_cnt[SF_H];
static void sf_hit(void *ra)
{
	sf_n++;
	const uint32_t a = (uint32_t)ra;
	for (uint32_t i = 0, h = (a >> 2) & (SF_H - 1); i < SF_H; i++) {
		const uint32_t j = (h + i) & (SF_H - 1);
		if (sf_ra[j] == a) { sf_cnt[j]++; return; }
		if (!sf_ra[j]) { sf_ra[j] = a; sf_cnt[j] = 1; return; }
	}
}
#define SF_WRAP2(name, R, A, B) R __real_##name(A, B); R __wrap_##name(A a, B b) { sf_hit(__builtin_return_address(0)); return __real_##name(a, b); }
#define SF_WRAP1(name, R, A) R __real_##name(A); R __wrap_##name(A a) { sf_hit(__builtin_return_address(0)); return __real_##name(a); }
SF_WRAP2(__muldf3, double, double, double)
SF_WRAP2(__adddf3, double, double, double)
SF_WRAP2(__subdf3, double, double, double)
SF_WRAP2(__divdf3, double, double, double)
SF_WRAP1(__floatsidf, double, int)
SF_WRAP1(__floatunsidf, double, unsigned)
SF_WRAP1(__fixdfsi, int, double)
SF_WRAP1(__fixunsdfsi, unsigned, double)
SF_WRAP1(__extendsfdf2, double, float)
SF_WRAP1(__truncdfsf2, float, double)
SF_WRAP2(__ltdf2, int, double, double)
SF_WRAP2(__gtdf2, int, double, double)
SF_WRAP2(__ledf2, int, double, double)
SF_WRAP2(__gedf2, int, double, double)
SF_WRAP2(__eqdf2, int, double, double)
SF_WRAP2(__nedf2, int, double, double)
SF_WRAP1(__fixdfdi, long long, double)
SF_WRAP1(__fixunsdfdi, unsigned long long, double)
SF_WRAP1(__floatdidf, double, long long)
SF_WRAP1(__floatundidf, double, unsigned long long)
// Every 60 frames: calls per frame, top 8 call sites (offset from main: for psp-addr2line use nm's main address + offset)
int main(int argc, char *argv[]);
static void sf_report(void)
{
	REI_LOG("  softdouble /frame: %u (main %p)\n", sf_n / 60, (void *)main);
	for (int k = 0; k < 12; k++) {
		int best = -1;
		for (int i = 0; i < SF_H; i++)
			if (sf_ra[i] && (best < 0 || sf_cnt[i] > sf_cnt[best])) best = i;
		if (best < 0 || !sf_cnt[best]) break;
		REI_LOG("    %6u ra %08x (main%+d)\n", sf_cnt[best] / 60, (unsigned)sf_ra[best], (int)(sf_ra[best] - (uint32_t)main));
		sf_cnt[best] = 0;
	}
	sf_n = 0;
	memset(sf_ra, 0, sizeof(sf_ra));
	memset(sf_cnt, 0, sizeof(sf_cnt));
}
#endif

#ifdef REI_HWPROF
// diag: switch the core span profiler's time source to hardware perf counters (read by the psp/devstub/kunlock.c kernel module).
// REI_HWPROF = PspDebugProfilerRegs index: 2 cpuck, 3 internal, 4 memory, 9 uncached_load, 11 cached_load,
// 13 i_miss, 14 d_miss, 15 d_writeback. "ms" in the log is units of 1000. 32-bit counters are extended to 64 bits
unsigned int nbbk_counter(int idx); // psp/devstub/kunlock.c (kernel module, syscall)
static uint64_t hw_counter(void)
{
	if (on_me())
		return 0;
	static uint32_t last;
	static uint64_t high;
	uint32_t v = nbbk_counter(REI_HWPROF);
	if (v < last)
		high += 1ull << 32;
	last = v;
	return high | v;
}
static unsigned drc_hw_counter32(void) { return nbbk_counter(REI_HWPROF); }
#endif

// Screen submission state (incl. stats)
static int g_submitted;     // frames sent to the GE
static int g_overflow_logged;
static long g_tiles_acc, g_tiles_max;

#if REI_PSP_DEFER
static int g_cur_frame;     // frame being emulated now
static int g_rendered;      // frames finished drawing
static int g_render_open;   // building the ge_layers list
static int g_sub_stage;     // submission piece: 0 CLUT, 1 sprites, 2 draw list
static int g_ge_sprites;    // G3: the GE draws sprites too (core ge_sprites)

// Draw one piece of the deferred frame. 1 if that frame is done
static int render_piece(void)
{
	if (!g_render_open) {
		ge_layers_begin();
		g_render_open = 1;
	}
	int r = rei_core_render_step();
	if (r == 0) {
		g_render_open = 0;
		g_rendered++;
		return 1;
	}
	if (r < 0)
		g_render_open = 0; // no pending frame (should not happen)
	return 0;
}

// Send one piece of a finished frame to the GE (after the GE finishes the previous frame, flip and send the next)
static void submit_piece(void)
{
	const uint32_t *pal = NULL;
	int blank = 0;
	rei_core_index_frame(&pal, &blank);
	switch (g_sub_stage) {
	case 0:
		video_sync();
		if (g_submitted)
			video_flip();
		video_submit_clut(pal);
		g_sub_stage = 1;
		break;
	case 1:
		g_sub_stage = 2;
		if (!g_ge_sprites) {
			video_submit_sprites(rei_core_sprite_frame());
			break;
		}
		// G3: the GE draws sprites, nothing to copy -> straight to the draw list
		// fall through
	default: {
		const ge_frame *fr = ge_layers_end();
		g_tiles_acc += fr->ntiles;
		if (fr->ntiles > g_tiles_max)
			g_tiles_max = fr->ntiles;
		int nst = 0, sovf = 0;
		const uint8_t *sgfx = NULL;
		const rei_spr_tile *st = g_ge_sprites ? rei_core_sprite_tiles(&nst, &sovf, &sgfx) : NULL;
		if ((video_submit_list(fr, st, nst, sgfx, pal, blank) || sovf) && !g_overflow_logged++)
			REI_LOG("reirem: GE buffers overflowed at f%d (tiles %d sprites %d/%d)\n", g_submitted, fr->ntiles, nst, sovf);
		g_sub_stage = 0;
		g_submitted++;
		break;
	}
	}
}

#ifdef REI_PROFILE
static uint64_t g_piece_us[4]; // diag: draw piece, CLUT (+GE wait, flip), sprites -> VRAM, draw list
#endif

// One piece of deferred work: submitting the finished frame first, then drawing the previous frame
static void defer_piece(void)
{
#ifdef REI_PROFILE
	uint64_t t0 = now_us();
	int k;
	if (g_submitted < g_rendered) {
		k = 1 + g_sub_stage;
		submit_piece();
	} else if (g_rendered < g_cur_frame) {
		k = 0;
		render_piece();
	} else {
		return;
	}
	g_piece_us[k] += now_us() - t0;
#else
	if (g_submitted < g_rendered)
		submit_piece();
	else if (g_rendered < g_cur_frame)
		render_piece();
#endif
}

// Called while the core spins waiting on the other side. The ME runs the same core code, so return immediately on the ME
static void wait_hook(void)
{
	if (on_me()) {
		rei_core_sound_idle(); // ME: apply deferred YM2151 writes while waiting
		me_sound_me_poll();    // ME: cache flush request for save states
		return;
	}
	if (rei_core_main_poll()) // main's share of YM2151 (sound side may wait at frame end)
		return;
	defer_piece();
}
#endif

static uint32_t color_5551(int r, int g, int b)
{
	return video_core_color(r, g, b);
}

// Same screen hash as the harness: index + 5551 palette -> 0x00RRGGBB (same 5->8 bit expansion as FBNeo CalcCol)
static void to_rgb(uint32_t *out, const uint16_t *index, const uint32_t *pal, int blank)
{
	static uint32_t rgb[0x801];
	for (int i = 0; i < 0x801; i++) {
		uint32_t c = pal[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
		rgb[i] = (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
	}
	(void)blank; // screen blanking is already filled with index 0x800 (black)
	for (int i = 0; i < REI_SCREEN_W * REI_SCREEN_H; i++)
		out[i] = rgb[index[i]];
}

static void log_ms(const char *name, uint64_t acc, uint64_t mx)
{
	REI_LOG("  %s avg %u.%u max %u.%u ms\n", name, (unsigned)(acc / HASH_EVERY / 1000),
		(unsigned)(acc / HASH_EVERY / 100 % 10), (unsigned)(mx / 1000), (unsigned)(mx / 100 % 10));
	(void)name;
	(void)acc;
	(void)mx;
}

#ifdef REI_PSP_ME
// Audio output: push ME-made frames (48kHz stereo) into a FIFO and output 512 samples at a time. Silence when empty.
// During verification (VERIFY_FRAMES), build the same audio hash as harness --threads from the popped frames
#define AUDIO_BLOCK 512
static volatile int g_audio_quit;
static volatile uint64_t g_audio_hash = FNV_OFFSET;
static volatile int g_audio_hashed, g_audio_underruns;
static volatile int g_audio_peak; // (diag) peak level popped by the audio thread (read and cleared by the log)
static int g_frame_len;
// Stops the audio thread from touching the ring while load/reset restarts the ME
static volatile int g_audio_hold, g_audio_held;
// Other boards (run_other_board): sound ring filled by main every frame instead of the ME
static int g_other;
static int g_me_render_on; // other boards: drawing on the ME (me_render_start succeeded)

// M107 tile layers on the GE (ge_m107.c): per screen buffer, sprites A/B, result, GE commands, vertices. Main fills these before starting
// the ME and does not write them afterwards (the ME reads them and writes the result)
#define GEM_CMD_WORDS 16384 // (harness max 8,500)
#define GEM_VTX 16384 // (harness max 9,738)
static gem_slot g_gslot[3];
static gem_slot *gem_get_slot(void) // (called on the ME: buffer being drawn = rei_render_index)
{
	extern unsigned short *rei_render_index;
	for (int i = 0; i < 2; i++)
		if ((const void *)g_gslot[i].spr_a == (const void *)rei_render_index)
			return &g_gslot[i];
	return &g_gslot[2];
}
static int g_ym_me_on;     // other boards: YM2151 synthesis on the ME too (moved back to main mid-game if the ME overflows)
// (experimental, off by default) M107: sound side (V35/YM2151/GA20) on the ME too — ME mode 3 (me_mix_start). Core uses the threaded pipeline,
// sound->main 192 lines (M107 games barely use sound->main state: dsoccr94 0 times in 3000 frames, airass 3). Enable: app folder
// m107mesnd, dev host0:/rei_m107mesnd
static int g_mix;
static volatile uint32_t g_main_waits;
static int g_mix_help; // mode 3: main helps draw while waiting (me_mix_main_render)
static unsigned g_late_n; // (diag, other-board loop) iterations that missed vblank /60f
static int g_quiet_log;   // (diag host0:/rei_quietlog) other-board loop log only every 600 frames
static void mix_stall_diag(void)
{
	if (!g_mix)
		return;
	uint32_t p, l;
	me_mix_alive(&p, &l);
	REI_LOG("reirem: ME mix alive: polls %u lines %u renders %u sound frames %u, main waits %u\n", (unsigned)p, (unsigned)l,
		(unsigned)me_mix_renders(), (unsigned)me_sound_frames(), (unsigned)g_main_waits);
}
static void other_wait_hook(void) // (core wait hook, only with g_mix) ME: draw/save/pause while sound waits on main
{
	if (on_me())
		me_mix_poll();
	else {
		g_main_waits++; // (diag) iterations main waited for sound/draw slots
		rei_core_main_poll(); // main: main's share of YM2151 (the ME's sound side waits at frame end)
		if (g_mix_help)
			me_mix_main_render(); // (when the ME overflows and main waits: main does the pending drawing)
	}
}
static int g_ym_split; // mode 3: main synthesizes YM2151 (YM split)
#ifdef REI_LOG_ENABLED
// (diag, host0:/rei_meclock) mode 3: thread feeding time to the ME (me_mix_clock, every 0.1ms) — ME time breakdown
static volatile int g_mclk_quit;
static SceUID g_mclk_th = -1;
static int mix_clock_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (!g_mclk_quit) {
		me_mix_clock(sceKernelGetSystemTimeLow());
		sceKernelDelayThread(100);
	}
	return 0;
}
#endif
#define OR_FRAMES 8
static int16_t g_or_buf[OR_FRAMES][1024 * 2];
static volatile unsigned g_or_w, g_or_r;
static int other_pop(int16_t *dst)
{
	if (g_or_r == g_or_w)
		return 0;
	memcpy(dst, g_or_buf[g_or_r % OR_FRAMES], (size_t)g_frame_len * 4);
	g_or_r++;
	return 1;
}

// output gain (as the Vita version and ReSys12): doubled, with a soft limit above 75% of full scale instead of clipping — past the
// knee the curve bends smoothly towards full scale (same slope at the knee). After the audio hash (frontend only)
static inline int16_t out_gain(int32_t v)
{
	enum { KNEE = 24576, ROOM = 32767 - KNEE };
	v *= 2;
	const int32_t m = v < 0 ? -v : v;
	if (m <= KNEE)
		return (int16_t)v;
	const int32_t e = m - KNEE, y = KNEE + e * ROOM / (e + ROOM);
	return (int16_t)(v < 0 ? -y : y);
}

static int audio_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	static int16_t fifo[8192 * 2];
	static int16_t frame[1024 * 2];
	static int16_t out[2][AUDIO_BLOCK * 2];
	int head = 0, count = 0, cur = 0; // fifo: in samples (stereo pairs)
	// Dynamic rate control (psp/docs/RULES.md 4-4): read slightly faster when fill is above target (~2.5 frames), slightly slower
	// when below (±0.5%, 16.16 fixed point, linear interpolation). When empty, ramp the last sample to 0 to avoid clicks
	uint32_t frac = 0;
	int last_l = 0, last_r = 0;
	sceAudioSRCChRelease(); // previous run (dev loop) may not have released it
	int rc = sceAudioSRCChReserve(AUDIO_BLOCK, g_audio_rate, 2);
	if (rc < 0) {
		REI_LOG("reirem: sceAudioSRCChReserve failed %08x\n", (unsigned)rc);
		return 0;
	}
	while (!g_audio_quit) {
		if (g_audio_hold) {
			g_audio_held = 1;
			count = 0; // drop remaining audio (it does not join up with audio after restart)
			sceKernelDelayThread(2000);
			continue;
		}
		g_audio_held = 0;
		while (count + g_frame_len <= 8192 && (g_other && !g_mix ? other_pop(frame) : me_sound_pop(frame))) {
#ifdef REI_LOG_ENABLED
			for (int i = 0; i < g_frame_len * 2; i++) { // (diag) peak of popped audio (mode 3 log — silence check)
				const int a = frame[i] < 0 ? -frame[i] : frame[i];
				if (a > g_audio_peak)
					g_audio_peak = a;
			}
#endif
			if (g_audio_hashed < VERIFY_FRAMES) {
				g_audio_hash = fnv1a(g_audio_hash, frame, (size_t)g_frame_len * 4);
				g_audio_hashed++;
			}
			for (int i = 0; i < g_frame_len; i++) {
				int at = (head + count + i) % 8192;
				fifo[at * 2] = frame[i * 2];
				fifo[at * 2 + 1] = frame[i * 2 + 1];
			}
			count += g_frame_len;
		}
		int16_t *o = out[cur];
		cur ^= 1;
		const int target = g_frame_len * 5 / 2;
		int dev = count - target; // vs target (samples)
		if (dev > target) dev = target;
		if (dev < -target) dev = -target;
		const uint32_t step = 65536 + (uint32_t)((int64_t)dev * 328 / target); // ±0.5% (328 ≈ 65536×0.005)
		if (count >= AUDIO_BLOCK + 2) {
			for (int i = 0; i < AUDIO_BLOCK; i++) {
				int n = (head + 1) % 8192;
				int f = (int)(frac >> 4); // 12 bits
				int l = fifo[head * 2] + (((fifo[n * 2] - fifo[head * 2]) * f) >> 12);
				int r = fifo[head * 2 + 1] + (((fifo[n * 2 + 1] - fifo[head * 2 + 1]) * f) >> 12);
				o[i * 2] = out_gain(l);
				o[i * 2 + 1] = out_gain(r);
				last_l = l;
				last_r = r;
				frac += step;
				int adv = (int)(frac >> 16);
				frac &= 0xffff;
				head = (head + adv) % 8192;
				count -= adv;
			}
		} else {
			for (int i = 0; i < AUDIO_BLOCK; i++) { // underrun: ramp last sample to 0 (~5ms), then silence
				last_l = last_l * 15 / 16;
				last_r = last_r * 15 / 16;
				o[i * 2] = out_gain(last_l);
				o[i * 2 + 1] = out_gain(last_r);
			}
			g_audio_underruns++;
		}
		sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, o);
	}
	sceAudioSRCChRelease();
	return 0;
}
#endif

// --- Options screen, save states, input (same behavior as Vita vita/src/frontend/main.c/input.c) ---

// Finish deferred drawing and wait for the GE (before opening the menu or changing state: draw and submit everything before frame f)
static void finish_frames(int f)
{
#if REI_PSP_DEFER
	g_cur_frame = f;
	while (g_submitted < f) {
		rei_core_main_poll();
		defer_piece();
	}
#else
	(void)f;
#endif
	video_sync();
}

// Until the sound side has fully caught up with main and is idle (max 2s). Returns 1 then
static int wait_pipe_synced(void)
{
	const uint64_t t_end = now_us() + 2000000;
	while (!rei_core_pipe_synced() && now_us() < t_end) {
		rei_core_main_poll();
		sceKernelDelayThread(100);
	}
	return rei_core_pipe_synced();
}

#ifdef REI_PSP_ME
extern char __rei_me_start[], __rei_me_end[]; // psp/me.ld: data block used by the sound side (ME)

// Discard sound-side data left in the main CPU cache (to read after the ME flushed it to memory)
static void drop_me_lines(void)
{
	sceKernelDcacheInvalidateRange(__rei_me_start, (unsigned)(__rei_me_end - __rei_me_start));
}

// Load/reset: stop audio output and the ME (the ME flushes its cache on exit) -> change state -> me_resume
static void me_halt(void)
{
	if (g_other) {
		if (g_mix && g_me_render_on) { // (mode 3: park the ME in the wait hook — no reboot)
			const int r = me_mix_pause(1);
			REI_LOG("reirem: other: ME mix pause %d\n", r);
			(void)r;
		} else if (g_me_render_on) // (make the ME stop drawing before rewinding render-side counters)
			me_render_pause(1);
		return;
	}
	g_audio_hold = 1;
	for (int i = 0; i < 500 && !g_audio_held; i++)
		sceKernelDelayThread(1000);
	me_sound_stop();
	drop_me_lines();
}

static void me_resume(void)
{
	if (g_other) {
		if (g_mix && g_me_render_on)
			me_mix_pause(0);
		else if (g_me_render_on)
			me_render_pause(0);
		return;
	}
	const int r = me_sound_start(g_frame_len);
	REI_LOG("reirem: media engine restart %d\n", r);
	g_audio_hold = 0;
	if (r < 0) { // cannot continue without sound (main waits on the sound side): exit the game
		rei_core_stop();
		g_quit = 1;
	}
}
#endif

// Unlock, sleep and wait for wake (the ME is on firmware meanwhile). Dev loop host0:/rei_sleeptest tests the same path without sleeping
static int g_sleep_test;
// Sleep stage trace: app folder sleep_trace.txt (play builds too — read next time if it hangs after wake). first = new file
void sleep_trace(const char *what, int v, int first)
{
	char path[128], line[96];
	snprintf(path, sizeof(path), "%ssleep_trace.txt", g_app_dir);
	SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | (first ? PSP_O_TRUNC : PSP_O_APPEND), 0777);
	if (fd < 0)
		return;
	const int n = snprintf(line, sizeof(line), "%s %d (flags %08x)\n", what, v, (unsigned)g_power_flags);
	sceIoWrite(fd, line, n);
	sceIoClose(fd);
	REI_LOG("reirem: sleep: %s", line);
}
static uint64_t rtc_us(void)
{
	u64 t = 0;
	sceRtcGetCurrentTick(&t); // (µs, keeps running during sleep)
	return (uint64_t)t;
}
static void sleep_wait(void)
{
	g_resumed = 0;
	g_sleep_req = 0;
	sleep_trace("suspend", g_sleep_test, 0);
	if (g_sleep_test) {
		sceKernelDelayThread(500000);
		g_sleep_test = 0;
		sleep_trace("(test) resumed", 0, 0);
		return;
	}
	power_lock(0);
	scePowerRequestSuspend(); // (a switch request resumes when unlocked, but sleep anyway in case it was lost)
	// Wake: callback (RESUME_COMPLETE) or wall clock ahead of idle time by over 1s (we slept — don't hang if the callback never comes).
	// Continue if no sleep within 10s
	const uint64_t r0 = rtc_us();
	int slept = 0;
	for (int i = 1; !g_quit; i++) {
		sceKernelDelayThread(20000);
		if (g_resumed)
			break;
		if (rtc_us() - r0 > (uint64_t)i * 20000 + 1000000) {
			slept = 1;
			break;
		}
		if (i >= 500)
			break;
	}
	sleep_trace("woke (resumed, slept)", g_resumed * 10 + slept, 0);
	sceKernelDelayThread(500000); // (until the system has restored all drivers)
	power_lock(1);
}
// Between games the ME stays halted in our code with the MeRpc pass-through, as after a quick load's stop: the next game restarts
// it the same way (me_boot). Returning it to firmware at every switch and resetting it again soon after powered the device off
// in the next game's ME start (play_check SWITCH 2026-10-28 01:50, app folder me_trace last "table file" right after "restore 0").
// So the firmware reboot happens only when leaving the app, sleeping or unloading (me_release), and until then HOME and the power
// switch are handled by main as during a game (g_game_loop, power lock; list_poll)
static int g_me_ours; // the ME runs our (halted) code between games
static void me_release(void)
{
#ifdef REI_PSP_ME
	if (!g_me_ours)
		return;
	const int r = me_sound_prepare_exit();
	REI_LOG("reirem: ME back to firmware %d\n", r);
	(void)r;
	g_me_ours = 0;
#endif
}
static int list_poll(void) // (game list: every frame)
{
	if (g_sleep_req) { // power switch: the ME back to firmware first (it loses power), then sleep as the system wants
		me_release();
		sleep_wait();
		power_lock(0);
		g_game_loop = 0;
	}
	return g_quit; // (HOME exit: main cleans up and the callback exits)
}

static void sleep_state_path(char *buf, int len)
{
	snprintf(buf, len, "%s%s_sleep.sav", rei_settings_dir(), rei_core_set_name());
}

static void reset_game(void)
{
#ifdef REI_PSP_ME
	me_halt();
	rei_core_reset();
	me_resume();
#else
	rei_core_reset();
#endif
	REI_LOG("reirem: reset\n");
}

static int save_state_path(const char *path);
static int load_state_path(const char *path);

// Save state to file (after finish_frames). 0 on success
static int save_state_path(const char *path)
{
	int rc = -3;
	if (g_other) {
		// (other boards: the ME is not sound. With ME rendering, save with drawing paused — me_sound_flush got no reply and failed)
#ifdef REI_PSP_ME
		int paused = 0;
		if (g_mix && g_me_render_on) // (mode 3: after the sound side catches up with main, parks in the wait hook and flushes its cache)
			paused = me_mix_pause(1) == 0;
		else if (g_me_render_on)
			me_render_pause(1);
		if (!g_mix || paused)
#endif
		rc = rei_core_state_save(path);
#ifdef REI_PSP_ME
		if (g_mix && g_me_render_on)
			me_mix_pause(0);
		else if (g_me_render_on)
			me_render_pause(0);
#endif
	} else if (wait_pipe_synced()) {
#ifdef REI_PSP_ME
		if (me_sound_flush() == 0) {
			drop_me_lines();
			rc = rei_core_state_save(path);
		}
#else
		rc = rei_core_state_save(path);
#endif
	}
	REI_LOG("reirem: state save %s rc %d\n", path, rc);
	return rc;
}

// Save state (slot 0~9 or REI_STATE_QUICK). Result shown at bottom of screen. Call after finish_frames
static void save_state(int slot)
{
	char path[160];
	rei_state_path(slot, path, sizeof(path));
	const int rc = save_state_path(path);
	if (rc == 0)
		rei_osd_show("Saved: %s", rei_state_slot_name(slot));
	else
		rei_osd_show("Save failed: %s", rei_state_slot_name(slot));
}

// Load from file (reset the game on failure). 0 on success
static int load_state_path(const char *path)
{
#ifdef REI_PSP_ME
	me_halt();
#endif
	int rc = rei_core_state_load(path);
	if (rc != 0)
		rei_core_reset(); // the sound side ran one more line when stopped, so we cannot continue as is
#ifdef REI_PSP_ME
	me_resume();
#endif
	REI_LOG("reirem: state load %s rc %d\n", path, rc);
	return rc;
}

// M92 path sleep: temp save -> ME to firmware -> sleep -> restart ME and load (after finish_frames)
static void sleep_m92(void)
{
	char path[160];
	sleep_state_path(path, sizeof(path));
	sleep_trace("m92 start", 0, 1);
	const int rc = save_state_path(path);
	sleep_trace("state save", rc, 0);
#ifdef REI_PSP_ME
	me_halt();
	const int er = me_sound_prepare_exit();
	sleep_trace("ME to firmware", er, 0);
#endif
	sleep_wait();
	if (g_quit)
		return;
#ifdef REI_PSP_ME
	const int lr = rc == 0 ? rei_core_state_load(path) : -1;
	sleep_trace("state load", lr, 0);
	if (lr != 0)
		rei_core_reset();
	me_resume();
	sleep_trace("ME restarted", 0, 0);
#endif
	sceIoRemove(path);
}

// Dev loop: if host0:/rei_sleeptest exists, delete it and test the sleep path once (without sleeping)
static void sleep_test_poll(int f)
{
#ifdef REI_LOG_ENABLED
	if (f % 60 == 30) {
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_sleeptest", &st) >= 0) {
			sceIoRemove("host0:/rei_sleeptest");
			g_sleep_test = 1;
			g_sleep_req = 1;
		}
	}
#else
	(void)f;
#endif
}

static void load_state(int slot)
{
	char path[160];
	rei_state_path(slot, path, sizeof(path));
	if (!rei_core_state_exists(path)) {
		rei_osd_show("No state: %s", rei_state_slot_name(slot));
		return;
	}
	const int rc = load_state_path(path);
	if (rc == 0)
		rei_osd_show("Loaded: %s", rei_state_slot_name(slot));
	else
		rei_osd_show("Load failed (game was reset): %s", rei_state_slot_name(slot));
}

// Pad -> game input (button mapping, turbo). System combos (SELECT+START menu, SELECT+L quick save, SELECT+R quick load) are not passed to the game
#define STICK_DEAD 48
static void set_game_input(uint32_t b, int ax, int ay)
{
	rei_core_set_input(REI_IN_P1_UP, (b & PSP_CTRL_UP) || ay < -STICK_DEAD);
	rei_core_set_input(REI_IN_P1_DOWN, (b & PSP_CTRL_DOWN) || ay > STICK_DEAD);
	rei_core_set_input(REI_IN_P1_LEFT, (b & PSP_CTRL_LEFT) || ax < -STICK_DEAD);
	rei_core_set_input(REI_IN_P1_RIGHT, (b & PSP_CTRL_RIGHT) || ax > STICK_DEAD);
	uint32_t blocked = 0;
	if ((b & PSP_CTRL_SELECT) && (b & PSP_CTRL_START))
		blocked |= PSP_CTRL_SELECT | PSP_CTRL_START;
	if ((b & PSP_CTRL_SELECT) && (b & PSP_CTRL_LTRIGGER))
		blocked |= PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER;
	if ((b & PSP_CTRL_SELECT) && (b & PSP_CTRL_RTRIGGER))
		blocked |= PSP_CTRL_SELECT | PSP_CTRL_RTRIGGER;
	const uint32_t game = b & ~blocked;
#ifdef REI_LOG_ENABLED
	{ // (diag) do coin/start presses reach game input
		static uint32_t last;
		if ((game ^ last) & (PSP_CTRL_SELECT | PSP_CTRL_START))
			REI_LOG("reirem: input coin %d start %d (beat %d, frame %d)\n", !!(game & PSP_CTRL_SELECT), !!(game & PSP_CTRL_START),
				g_wd_beat, g_wd_frame);
		last = game;
	}
#endif
	// Turbo: while the turbo key is held, the chosen game button is pressed only for the first half of each period (counted from the press frame)
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
		// If the turbo key is also mapped as a normal key, turbo wins (its normal mapping is ignored)
		int on = ((game & ~tm) & rei_key_mask(g_settings.keymap[a])) != 0;
		if (a == turbo_act)
			on |= turbo_on;
		rei_core_set_input(act_input[a], on);
	}
}

#ifdef REI_LOG_ENABLED
// Dev loop: emulate the pad from "iteration button(hex)" lines in host0:/rei_pad (options screen tests). Iteration is the main loop
// count (keeps counting while the menu is open); the line's buttons are held until the next line
static int g_pad_n, g_pad_at[256];
static uint32_t g_pad_btn[256];

static void pad_script_load(void)
{
	SceUID fd = sceIoOpen("host0:/rei_pad", PSP_O_RDONLY, 0);
	if (fd < 0)
		return;
	static char b[4096];
	const int len = sceIoRead(fd, b, sizeof(b) - 1);
	sceIoClose(fd);
	b[len > 0 ? len : 0] = 0;
	for (char *p = b; *p && g_pad_n < 256;) {
		char *e;
		const long at = strtol(p, &e, 10);
		if (e == p)
			break;
		g_pad_at[g_pad_n] = (int)at;
		g_pad_btn[g_pad_n++] = (uint32_t)strtoul(e, &p, 16);
		while (*p == '\r' || *p == '\n' || *p == ' ')
			p++;
	}
	REI_LOG("reirem: pad script %d steps (host0:/rei_pad)\n", g_pad_n);
}

static uint32_t pad_script(int beat)
{
	uint32_t b = 0;
	for (int i = 0; i < g_pad_n && g_pad_at[i] <= beat; i++)
		b = g_pad_btn[i];
	return b;
}
#endif

// --- Other boards (non-M92, ADR-023): the core runs the original FBNeo frame on main, draws on the CPU (GE 5650 buffer) and makes sound.
// The ME is not used. Video composed via video_set_game_buffer (border/scale/rotate/menu); the audio thread pops sound from a ring.
// Input, options screen, save states and cheats match the M92 path (Vita handles other boards the same way)
static uint32_t color_565(int r, int g, int b)
{
	return (uint32_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); // GE 5650: R in low bits
}

static int other_push(const int16_t *src)
{
	if (g_or_w - g_or_r >= OR_FRAMES)
		return 0; // (audio backed up: drop)
	memcpy(g_or_buf[g_or_w % OR_FRAMES], src, (size_t)g_frame_len * 4);
	g_or_w++;
	return 1;
}

// 0 when done, 1 on dev-loop stop request (unload module only)
// Large span-state copies (VRAM copy) via DMA (rei_defer.h rei_big_copy). 64KB: memcpy 854µs -> DMA 379µs (incl. cache sync,
// device 2026-10-15). Source is flushed to memory, destination lines are discarded from cache (main never reads the dest — render (ME) reads memory)
static void psp_big_copy(void *dst, const void *src, size_t n)
{
	if (((uintptr_t)dst & 63) || (n & 63) || n < 4096) {
		memcpy(dst, src, n);
		return;
	}
	sceKernelDcacheWritebackRange(src, n);
	sceKernelDcacheInvalidateRange(dst, n);
	if (sceDmacMemcpy(dst, src, n) < 0)
		memcpy(dst, src, n);
}

#ifdef REI_DRC
// V35 dynarec cache flush and clock (v25.cpp rei_drc35_*): for the main CPU. Switched to the me_sound side when run on the ME in mode 3
static void drc35_main_hooks(void)
{
	extern void (*rei_drc35_cache_flush)(void *p, int len);
	extern unsigned (*rei_drc35_now_us)(void), (*rei_drc35_clock32)(void), (*rei_drc35_clock_fine)(void);
	extern int rei_drc35_on_me;
	rei_drc35_cache_flush = drc_cache_flush;
	rei_drc35_now_us = (unsigned (*)(void))sceKernelGetSystemTimeLow;
#ifdef REI_LOG_ENABLED
	rei_drc35_clock32 = (unsigned (*)(void))sceKernelGetSystemTimeLow;
#else
	rei_drc35_clock32 = NULL;
#endif
	rei_drc35_clock_fine = NULL;
	rei_drc35_on_me = 0;
}
#ifdef REI_PSP_ME
// (on main, before starting the ME) Sound-side V35 on the ME also uses dynarec: main pre-builds the code area and shared routines (the ME
// cannot allocate), cache flush for the ME, no clock (v25.cpp rei_drc35_*). 1 = on
static int drc35_to_me(void)
{
	extern int rei_drc35_enabled, rei_drc35_on_me;
	extern int rei_drc35_preinit(void);
	extern void (*rei_drc35_cache_flush)(void *p, int len);
	extern unsigned (*rei_drc35_now_us)(void), (*rei_drc35_clock32)(void), (*rei_drc35_clock_fine)(void);
	if (!rei_drc35_enabled || !rei_drc35_preinit())
		return 0;
	{
		extern volatile uint32_t rei_drc35_me_diag[4];
		volatile uint32_t *d = (volatile uint32_t *)((uintptr_t)rei_drc35_me_diag | 0x40000000u);
		d[0] = d[1] = d[2] = d[3] = 0;
	}
	rei_drc35_cache_flush = me_drc_flush;
	rei_drc35_now_us = NULL;
	rei_drc35_clock32 = NULL;
	rei_drc35_clock_fine = NULL;
	rei_drc35_on_me = 1;
	return 1;
}
#endif
#endif

// Per-game state back to what a fresh process has (in-app game switching: the previous game, maybe of another board, set these)
static void game_state_reset(void)
{
	g_submitted = 0;
	g_overflow_logged = 0;
	g_tiles_acc = g_tiles_max = 0;
#if REI_PSP_DEFER
	g_cur_frame = g_rendered = g_render_open = g_sub_stage = 0;
	g_ge_sprites = 0;
#endif
	g_af_offset = 0;
	g_save_at = -1;
	g_xverify_from = g_xverify_to = -1;
	g_xverify_bad = 0;
#ifdef REI_PSP_ME
	g_audio_quit = 0;
	g_audio_hash = FNV_OFFSET;
	g_audio_hashed = g_audio_underruns = 0;
	g_audio_hold = g_audio_held = 0;
	g_other = 0;
	g_mix = 0;
	g_ym_me_on = 0;
	g_ym_split = 0;
	g_me_render_on = 0;
	g_or_w = g_or_r = 0;
	{ // (other boards set these for drawing on the ME; M92 and main-drawn games run without them)
		extern int rei_render_noalloc;
		extern void (*rei_render_done_hook)(void), (*rei_defer_flush_hook)(void), (*rei_sound_line_hook)(void);
		rei_render_noalloc = 0;
		rei_render_done_hook = NULL;
		rei_defer_flush_hook = NULL;
		rei_sound_line_hook = NULL;
	}
#endif
	{
		extern int rei_v25_counter_rule; // (M92 turns it off for the ME's V35)
		rei_v25_counter_rule = 1;
	}
}

static void m92_state_reset(void)
{
	game_state_reset();
	g_audio_rate = AUDIO_RATE;
}

static int run_other_board(const rei_game_found *game, const char *set_name)
{
	game_state_reset();
	g_other = 1;
	rei_core_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.zip_path = game->zip;
	cfg.set_name = set_name;
	cfg.parent_zip_path = game->parent[0] ? game->parent : NULL;
	g_audio_rate = OTHER_AUDIO_RATE;
	cfg.sample_rate = OTHER_AUDIO_RATE;
	cfg.bytes_per_pixel = 2;
	cfg.color = color_565;
	cfg.log = core_log;
#ifdef REI_HWPROF
	cfg.clock_us = hw_counter; // (diag: span value = units of 1000 hardware counter events)
#else
	cfg.clock_us = now_us;
#endif
	cfg.progress = load_progress;
	// M107: the same main/sound pipeline as Vita (ADR-019, sound->main 32 lines later) on one thread — alternating main 32 lines / sound 32 lines
	// avoids the CPU switching cost of the original frame (V33/V35 alternating every 2048 slices). Hash matches harness --mode pipelined.
	// Other boards: the core ignores this value
	cfg.pipelined = 1;
#ifdef REI_PSP_ME
	// Drawing on the ME (PSP-ADR-013): on by default (measured 2026-10-13 — rtype 21.1->8.2ms etc., save/load/reset/menu/exit verified).
	// Disabled if a nomerender file is in the app folder or dev loop host0:/rei_nomerender exists.
	// The core enables deferred drawing if the driver supports it (rei_core_render_threaded)
	int me_render_want = 1;
	{
		SceIoStat st;
		char mp[128];
		snprintf(mp, sizeof(mp), "%snomerender", g_app_dir);
		if (sceIoGetstat(mp, &st) >= 0)
			me_render_want = 0;
#ifdef REI_LOG_ENABLED
		if (sceIoGetstat("host0:/rei_nomerender", &st) >= 0)
			me_render_want = 0;
#endif
	}
#ifdef REI_PROFILE
	{ // (profile build: draw-span timer reads the shared clock on the ME (now_us on_me). Default is main drawing — with host0:/rei_profme
	  //  keep ME drawing and measure main spans in the real configuration)
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_profme", &st) < 0)
			me_render_want = 0;
	}
#endif
	cfg.render_threaded = me_render_want;
	g_mix = 0;
	g_ym_split = 0;
#ifdef REI_DRC
	drc35_main_hooks(); // (previous game may have been mode 3)
#endif
#ifdef REI_LOG_ENABLED
	{
		SceIoStat qst;
		g_quiet_log = sceIoGetstat("host0:/rei_quietlog", &qst) >= 0;
	}
#endif
	if (me_render_want && strcmp(rei_core_game_board(game->game), "M107") == 0) { // (g_mix above: experimental, off by default)
		SceIoStat st;
		char mp[128];
		// On by default (play-tested 2026-10-17) — disable: app folder nom107mesnd, dev host0:/rei_nom107mesnd
		snprintf(mp, sizeof(mp), "%snom107mesnd", g_app_dir);
		g_mix = sceIoGetstat(mp, &st) < 0;
#ifdef REI_LOG_ENABLED
		if (sceIoGetstat("host0:/rei_nom107mesnd", &st) >= 0)
			g_mix = 0;
#endif
		if (g_mix) {
			cfg.threaded = 1;
			cfg.s2m_delay_lines = 192;
			cfg.wait_hook = other_wait_hook;
			// Main helps draw while waiting (me_mix_main_render), and if sound lags over 64 lines the ME skips drawing in the line hook —
			// ME-overloaded firebarr: 12.7 -> 10.9ms (loop 17.1 -> 16.7, underruns 9 -> 0), dsoccr94 12.8, airass 9.5 (2026-10-16).
			// Disable (test): host0:/rei_mixnohelp, change threshold host0:/rei_mixlag N
			g_mix_help = 1;
			me_mix_set_lagmax(64);
#ifdef REI_LOG_ENABLED
			if (sceIoGetstat("host0:/rei_mixnohelp", &st) >= 0)
				g_mix_help = 0;
			{ // (test) host0:/rei_mixlag N: ME skips drawing in the line hook if sound lags over N lines
				SceUID fd = sceIoOpen("host0:/rei_mixlag", PSP_O_RDONLY, 0);
				if (fd >= 0) {
					char b[8] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					sceIoClose(fd);
					me_mix_set_lagmax((uint32_t)atoi(b));
				}
			}
#endif
			// (test host0:/rei_mixsplit) YM2151 synthesis on main (8 channels, YM split): main drains the ring per line, per wait and during vblank wait.
			// A loss — main +2.5~3.5ms, ME waits at frame end for main's share (dsoccr94 12.1 -> 14.2, airass 10.7 -> 14.3ms)
			cfg.ym_main_channels = 0;
#ifdef REI_LOG_ENABLED
			{ // host0:/rei_mixsplit "bits": YM2151 channels main synthesizes (0xff if file is empty)
				SceUID fd = sceIoOpen("host0:/rei_mixsplit", PSP_O_RDONLY, 0);
				if (fd >= 0) {
					char b[8] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					sceIoClose(fd);
					const int m = (int)strtol(b, NULL, 0);
					cfg.ym_main_channels = m > 0 ? m & 0xff : 0xff;
					REI_LOG("reirem: other: YM2151 main channels %02x\n", cfg.ym_main_channels);
				}
			}
			{ // (test) host0:/rei_mixmerge N: max lines per sound line batch (d_m107.cpp, default 32)
				SceUID fd = sceIoOpen("host0:/rei_mixmerge", PSP_O_RDONLY, 0);
				if (fd >= 0) {
					char b[8] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					sceIoClose(fd);
					extern int rei_m107_merge_max;
					rei_m107_merge_max = atoi(b);
					REI_LOG("reirem: other: sound merge max %d\n", rei_m107_merge_max);
				}
			}
#endif
		}
	}
#endif
	const uint64_t t0 = now_us();
#ifdef REI_DRC
	{ // other boards: cut V33/V30 per-frame dynarec compile budget to 500 and build overflow blocks in spare time before vblank (drc_idle_work).
	  // Scene-change spike frame: xmultipl 26.8 -> 22.1, dbreed 25.8 -> 19.2ms (200 was worse, more interpreter time). V35 stays at 2000:
	  // lowering it made kengo/ltswords switch to the interpreter via short-block detection or slow down (16.7 -> 17.2~17.6ms, though bombrman went 36 -> 25)
	  // Disable: app folder nodrcidle (dev host0:/rei_nodrcidle)
		extern void rei_drc_set_budget(int n);
		extern int rei_drc_idle_on;
		SceIoStat ist;
		char ip[128];
		snprintf(ip, sizeof(ip), "%snodrcidle", g_app_dir);
		int on = sceIoGetstat(ip, &ist) < 0;
#ifdef REI_LOG_ENABLED
		if (sceIoGetstat("host0:/rei_nodrcidle", &ist) >= 0)
			on = 0;
#endif
		if (on) {
			rei_drc_set_budget(500);
		}
		rei_drc_idle_on = on;
	}
#endif
#if defined(REI_DRC) && defined(REI_LOG_ENABLED)
	{ // (diag) dev loop: disable V33 dynarec if host0:/rei_nodrc exists
		extern int rei_drc_enabled;
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_nodrc", &st) >= 0) {
			rei_drc_enabled = 0;
			REI_LOG("reirem: other: dynarec off\n");
		}
		if (sceIoGetstat("host0:/rei_nodrc35", &st) >= 0) { // (diag) disable V35 dynarec
			extern int rei_drc35_enabled;
			rei_drc35_enabled = 0;
			REI_LOG("reirem: other: V35 dynarec off\n");
		}
		{ // (diag) cost of one 64-bit division (same shape as timer tick conversion)
			volatile int64_t acc = 0;
			volatile int32_t m = 3579545;
			const uint64_t c0 = now_us();
			for (int i = 1; i <= 20000; i++)
				acc += (int64_t)i * 2048000000LL / m;
			const uint64_t c1 = now_us();
			for (int i = 1; i <= 20000; i++)
				acc += (int64_t)i * 2048000000LL;
			const uint64_t c2 = now_us();
			REI_LOG("reirem: other: divdi3 %u ns each (loop without %u ns)\n", (unsigned)((c1 - c0) * 1000 / 20000),
				(unsigned)((c2 - c1) * 1000 / 20000));
		}
		if (sceIoGetstat("host0:/rei_cpybench", &st) >= 0) { // (diag) 64KB copy speed per method (span state copy)
			uint8_t *a = (uint8_t *)memalign(64, 65536), *b = (uint8_t *)memalign(64, 65536);
			if (a && b) {
				memset(a, 1, 65536); memset(b, 2, 65536);
				uint64_t t0 = now_us();
				for (int i = 0; i < 50; i++) { memcpy(b, a, 65536); a[i] ^= 1; }
				uint64_t t1 = now_us();
				for (int i = 0; i < 50; i++) { // word copy in 32-byte groups
					const uint32_t *s = (const uint32_t *)a; uint32_t *d = (uint32_t *)b;
					for (int k = 0; k < 65536 / 4; k += 8) {
						const uint32_t w0 = s[k], w1 = s[k + 1], w2 = s[k + 2], w3 = s[k + 3], w4 = s[k + 4], w5 = s[k + 5], w6 = s[k + 6], w7 = s[k + 7];
						d[k] = w0; d[k + 1] = w1; d[k + 2] = w2; d[k + 3] = w3; d[k + 4] = w4; d[k + 5] = w5; d[k + 6] = w6; d[k + 7] = w7;
					}
					a[i] ^= 1;
				}
				uint64_t t2 = now_us();
				for (int i = 0; i < 50; i++) { // VFPU 16 bytes x4
					for (int k = 0; k < 65536; k += 64)
						__asm__ volatile("lv.q c000, 0(%0)\n lv.q c010, 16(%0)\n lv.q c020, 32(%0)\n lv.q c030, 48(%0)\n"
							"sv.q c000, 0(%1)\n sv.q c010, 16(%1)\n sv.q c020, 32(%1)\n sv.q c030, 48(%1)\n"
							:: "r"(a + k), "r"(b + k) : "memory");
					a[i] ^= 1;
				}
				uint64_t t3 = now_us();
				int dr = 0;
				for (int i = 0; i < 50; i++) { // DMA (incl. cache sync before/after)
					sceKernelDcacheWritebackRange(a, 65536);
					sceKernelDcacheInvalidateRange(b, 65536);
					dr = sceDmacMemcpy(b, a, 65536);
					a[i] ^= 1;
				}
				uint64_t t4 = now_us();
				REI_LOG("reirem: copy 64KB: memcpy %u us, words %u us, vfpu %u us, dmac %u us (%d)\n", (unsigned)((t1 - t0) / 50),
					(unsigned)((t2 - t1) / 50), (unsigned)((t3 - t2) / 50), (unsigned)((t4 - t3) / 50), dr);
			}
			free(a); free(b);
		}
		extern int rei_z80_drc_enabled; // (Z80 dynarec stage 2, off by default) enable with host0:/rei_z80drc or app folder z80drc
		if (sceIoGetstat("host0:/rei_z80drc", &st) >= 0) {
			rei_z80_drc_enabled = 1;
			REI_LOG("reirem: other: Z80 dynarec on\n");
		}
		extern int rei_drc35_enabled; // (V35 dynarec, ADR-026) disable with host0:/rei_nodrc35
		if (sceIoGetstat("host0:/rei_nodrc35", &st) >= 0) {
			rei_drc35_enabled = 0;
			REI_LOG("reirem: other: V35 dynarec off\n");
		}
		{ // (test) host0:/rei_drcbudget "V33 V35": per-frame dynarec compile budget (instructions, default 500 2000)
			extern void rei_drc_set_budget(int n), rei_drc35_set_budget(int n);
			SceUID fd = sceIoOpen("host0:/rei_drcbudget", PSP_O_RDONLY, 0);
			if (fd >= 0) {
				char b[32] = { 0 };
				sceIoRead(fd, b, sizeof(b) - 1);
				sceIoClose(fd);
				int b33 = 0, b35 = 0;
				sscanf(b, "%d %d", &b33, &b35);
				rei_drc_set_budget(b33);
				rei_drc35_set_budget(b35);
				REI_LOG("reirem: other: dynarec budget %d %d\n", b33, b35);
			}
		}
		{ // (test) host0:/rei_drc35low N: V35 per-frame compile budget is N after a frame with spare time (drc_idle_work)
			SceUID fd = sceIoOpen("host0:/rei_drc35low", PSP_O_RDONLY, 0);
			if (fd >= 0) {
				char b[16] = { 0 };
				sceIoRead(fd, b, sizeof(b) - 1);
				sceIoClose(fd);
				sscanf(b, "%d %d", &g_drc35_low, &g_drc35_late);
				REI_LOG("reirem: other: V35 budget low %d late %d\n", g_drc35_low, g_drc35_late);
			}
			fd = sceIoOpen("host0:/rei_drc33low", PSP_O_RDONLY, 0);
			if (fd >= 0) {
				char b[16] = { 0 };
				sceIoRead(fd, b, sizeof(b) - 1);
				sceIoClose(fd);
				sscanf(b, "%d %d", &g_drc33_low, &g_drc33_late);
				REI_LOG("reirem: other: V33 budget low %d late %d\n", g_drc33_low, g_drc33_late);
			}
		}
		extern int rei_drc35_short_off;
		if (sceIoGetstat("host0:/rei_drc35keep", &st) >= 0) { // (diag) keep V35 dynarec even for short blocks
			rei_drc35_short_off = 0;
			REI_LOG("reirem: other: V35 dynarec kept for short blocks\n");
		}
	}
#endif
#ifdef REI_DRC
	{ // large span-state copies via DMA (dev host0:/rei_nodmacopy for memcpy)
		extern void (*rei_big_copy)(void *dst, const void *src, size_t n);
		rei_big_copy = psp_big_copy;
#ifdef REI_LOG_ENABLED
		SceIoStat dst_;
		if (sceIoGetstat("host0:/rei_nodmacopy", &dst_) >= 0)
			rei_big_copy = NULL;
#endif
		if (!rei_big_copy) { extern void rei_big_copy_reset(void); rei_big_copy_reset(); }
	}
	{ // Z80 dynarec stage 2 (PERF PP39): on by default (play-tested 2026-10-17) — disable: app folder noz80drc, dev host0:/rei_noz80drc
		extern int rei_z80_drc_enabled;
		SceIoStat zst;
		char zp[128];
		snprintf(zp, sizeof(zp), "%snoz80drc", g_app_dir);
		rei_z80_drc_enabled = sceIoGetstat(zp, &zst) < 0;
#ifdef REI_LOG_ENABLED
		if (sceIoGetstat("host0:/rei_noz80drc", &zst) >= 0)
			rei_z80_drc_enabled = 0;
		if (sceIoGetstat("host0:/rei_z80drc", &zst) >= 0)
			rei_z80_drc_enabled = 1;
#endif
	}
	{
		// Automatic idle-loop skipping is disabled along with dynarec: recording one loop iteration in the interpreter per slice cost more
		// than skipping saved, and with it off short backward jumps also chain blocks and run native. Same results (harness --no-idle-skip, 16 game hashes match).
		// In-game emu ms: dbreed 16.6->12.7, dsoccr94 18.8->17.2, airass 13.8->12.2, rtype 9.6->8.5, others unchanged (2026-10-14)
		extern int rei_drc_enabled;
		cfg.no_idle_skip = rei_drc_enabled ? 1 : 0;
#ifdef REI_LOG_ENABLED
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_idle", &st) >= 0) // (diag) re-enable
			cfg.no_idle_skip = 0;
		REI_LOG("reirem: other: idle skip %s\n", cfg.no_idle_skip ? "off" : "on");
#endif
		// V35 too: disabled only while dynarec runs (re-enabled on switch to interpreter — v25.cpp rei_v25_auto_with_drc)
		// V35 keeps auto idle on during dynarec, and games with nothing to skip turn it off themselves per 120-frame window (v25.cpp
		// rei_v25_auto_drc_policy — blocks also record writes, so skipping works without recording. In-game emu: kengo 16.4->13.2, ltswords 17.3->13.7)
		extern int rei_v25_auto_with_drc, rei_v25_auto_drc_policy;
		rei_v25_auto_with_drc = 1;
		// The policy (off by skipped amount per 120-frame window) is disabled: once off it only rechecked after 4 windows (~8s), so kengo/ltswords
		// in-game idle loop sections stayed at 16.5~17ms (always on: 11~12.6ms). dynablst/riskchal/hasamu/cosmccop with nothing to skip were
		// the same always on (12.4~12.9/12.9/15.0/13.2ms). Old policy: host0:/rei_v35autopolicy
		rei_v25_auto_drc_policy = 0;
#ifdef REI_LOG_ENABLED
		if (sceIoGetstat("host0:/rei_nov35auto", &st) >= 0) { // (diag) auto idle off during V35 dynarec (old behavior)
			rei_v25_auto_with_drc = cfg.no_idle_skip ? 0 : 1;
			rei_v25_auto_drc_policy = 0;
		}
		if (sceIoGetstat("host0:/rei_v35autopolicy", &st) >= 0) { // (test) per-window auto idle decision during V35 dynarec (old behavior)
			rei_v25_auto_drc_policy = 1;
			REI_LOG("reirem: other: V35 auto idle policy on\n");
		}
#endif
	}
#endif
#ifdef REI_PSP_ME
	me_sound_trace("core init", 0);
#endif
	if (core_init_threaded(&cfg) != 0) {
		REI_LOG("reirem: init failed: %s\n", rei_core_error());
		rei_message_screen("Init failed", rei_core_error());
		g_to_list = 1; // (the core cleaned up after itself: back to the list)
		return REI_RUN_DONE;
	}
	int w, h, flags;
	rei_core_screen(&w, &h, &flags);
	const struct mallinfo mi = mallinfo();
	REI_LOG("reirem: other board %s %s, screen %dx%d flags %d, init %u ms, heap arena %u KB used %u KB free %u KB\n",
		rei_core_board(), rei_core_game_name(), w, h, flags, (unsigned)((now_us() - t0) / 1000), (unsigned)(mi.arena / 1024),
		(unsigned)(mi.uordblks / 1024), (unsigned)(mi.fordblks / 1024));
	rei_loading_progress(940);
	{ // user cheat file (same as M92 path)
		char cd[112];
		snprintf(cd, sizeof(cd), "%scheats/", strncmp(g_app_dir, "host", 4) == 0 ? "host1:/" : g_app_dir);
		REI_LOG("reirem: cheats %d (%s)\n", rei_core_cheats_load(cd), cd);
	}
	if (g_settings.dip_count > 0)
		rei_core_dip_set_raw(g_settings.dip, g_settings.dip_count);
	const int stride = (w + 7) & ~7;
	// Two screen buffers: the GE reads the previous frame's buffer while the core draws into the other (with one, we waited on the GE: ~7ms/frame)
	uint16_t *fbs[2] = { (uint16_t *)memalign(64, (size_t)stride * h * 2), (uint16_t *)memalign(64, (size_t)stride * h * 2) };
	uint16_t *fb = fbs[0];
	const int alen = rei_core_audio_len();
	int16_t *audio = (int16_t *)memalign(64, ((size_t)alen * 2 + 16) * sizeof(int16_t));
	REI_LOG("reirem: other: fb %p audio %p alen %d\n", (void *)fb, (void *)audio, alen);
	if (!fbs[0] || !fbs[1] || !audio || alen > 1024) {
		rei_message_screen("Init failed", "Out of memory");
		free(fbs[0]);
		free(fbs[1]);
		free(audio);
		rei_core_exit();
		g_to_list = 1;
		return REI_RUN_DONE;
	}
	memset(fbs[0], 0, (size_t)stride * h * 2);
	memset(fbs[1], 0, (size_t)stride * h * 2);
	sceKernelDcacheWritebackRange(fbs[1], (unsigned)(stride * h * 2));
	video_set_game_buffer(fbs[1], w, h, stride, flags);
	int fb_cur = 0;
	g_frame_len = alen;
	g_or_w = g_or_r = 0;
	SceUID ath = sceKernelCreateThread("audio", audio_thread, 0x12, 0x4000, 0, NULL);
	if (ath >= 0)
		sceKernelStartThread(ath, 0, NULL);
	// Drawing on the ME: 3 screen buffers (the ME draws in turn, main submits the newest finished one to the GE)
	uint16_t *mfb[3] = { NULL, NULL, NULL };
	uint16_t *mclut[3] = { NULL, NULL, NULL }; // (index mode: palette per buffer)
	int idx_mode = 0, nclut = 0, gem = 0; // (gem: tile layer GE mode — 1 M107, 2 M72 family)
	uint16_t gm72_mask[GM72_MAX_MASKS]; // (M72 GE: pen masks to fill the CLUT)
	int gm72_nm = 0;
	uint32_t me_shown = 0;
	const int deferred = rei_core_render_threaded(); // (with deferral on, main does not draw: the ME or main draws separately)
	g_me_render_on = 0;
#ifdef REI_PSP_ME
	if (deferred) {
		// Index mode (2026-10-14): the ME draws color indices only (no palette transfer) and the GE colors via CLUT. Screen buffer has the same
		// shape as image 0 (width w, 16 overflow lines), 3 palette slots. Disabled if VRAM is short, shapes differ, or app folder nomeidx / host0:/rei_nomeidx
		nclut = rei_core_palette_entries();
		// (small boards M5x/M6x leave the ME idle: only GE passes grow, occasional spikes — kungfum/travrusa max 17 -> 23ms. Left as is)
		const char *bd = rei_core_board();
		const int small_board = strncmp(bd, "M5", 2) == 0 || strncmp(bd, "M6", 2) == 0;
		idx_mode = !small_board && stride == w && rei_core_trans_width() == w && nclut > 0 && nclut <= 0x800 && video_index_fits(w, h);
		{
			SceIoStat st;
			char mp[128];
			snprintf(mp, sizeof(mp), "%snomeidx", g_app_dir);
			if (sceIoGetstat(mp, &st) >= 0)
				idx_mode = 0;
#ifdef REI_LOG_ENABLED
			if (sceIoGetstat("host0:/rei_nomeidx", &st) >= 0)
				idx_mode = 0;
			{ extern int g_idx_nocopy; g_idx_nocopy = sceIoGetstat("host0:/rei_idxnocopy", &st) >= 0; }
			{ extern int g_present_wb; g_present_wb = sceIoGetstat("host0:/rei_presentwb", &st) >= 0; }
			{ // (diag) number in host0:/rei_geskip — video.c g_ge_skip
				extern int g_ge_skip;
				SceUID fd = sceIoOpen("host0:/rei_geskip", PSP_O_RDONLY, 0);
				if (fd >= 0) {
					char b[16] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					sceIoClose(fd);
					g_ge_skip = atoi(b);
				}
			}
#endif
		}
		// M107 tile layers on the GE (ge_m107.c, 2026-10-15): the ME builds GE lists instead of pixels and draws only sprites. New ME feature,
		// on by default (play-tested 2026-10-17) — disable: app folder nom107ge, dev host0:/rei_nom107ge
		rei_m107_tiles gem_tiles;
		if (idx_mode && w == 320 && h == 240 && strcmp(bd, "M107") == 0 && rei_m107_get_tiles(&gem_tiles) == 0) {
			SceIoStat st;
			char mp[128];
			snprintf(mp, sizeof(mp), "%snom107ge", g_app_dir);
			gem = sceIoGetstat(mp, &st) < 0;
#ifdef REI_LOG_ENABLED
			if (sceIoGetstat("host0:/rei_nom107ge", &st) >= 0)
				gem = 0;
#endif
			if (gem) {
				const rei_tile_info ti = { gem_tiles.gfx, gem_tiles.tile_mask, 0, 0 };
				if (video_gem_atlas_init(&ti) != 0) {
					REI_LOG("reirem: other: M107 GE atlas failed (tiles %d)\n", gem_tiles.tile_mask + 1);
					gem = 0;
				}
			}
		}
		// M72-family tile layers on the GE (ge_m72.c, 2026-10-17): the ME builds GE lists and draws only sprites (after the kengo boss, the big
		// explosion pushed ME drawing past one frame and main waited 6ms each). On by default (play-tested 2026-10-17 "kengo performance
		// is stable overall") — disable: app folder nom72ge, dev host0:/rei_nom72ge
		rei_m72_tiles gm72_tiles;
		int gm72 = 0;
		uint32_t gm72_atlas[2] = { 0, 0 };
		// (majtitle excluded: building per-line strips for the line-scroll layer made the ME slower — in-game emu 10.5 -> 15.9, ME wait 3.4ms)
		if (idx_mode && !gem && w <= 512 && h <= 256 && rei_m72_get_tiles(&gm72_tiles) == 0 && gm72_tiles.type != 3) {
			SceIoStat st;
			char mp[128];
			snprintf(mp, sizeof(mp), "%snom72ge", g_app_dir);
			gm72 = sceIoGetstat(mp, &st) < 0;
#ifdef REI_LOG_ENABLED
			if (sceIoGetstat("host0:/rei_nom72ge", &st) >= 0)
				gm72 = 0;
#endif
			if (gm72 && video_gm72_atlas_init(gm72_tiles.gfx, gm72_tiles.ntiles, gm72_atlas) != 0) {
				REI_LOG("reirem: other: M72 GE atlas failed (tiles %d %d)\n", gm72_tiles.ntiles[0], gm72_tiles.ntiles[1]);
				gm72 = 0;
			}
		}
		if (gm72)
			gem = 2; // (buffer layout matches M107 GE — B buffer unused)
		const size_t fbytes = idx_mode ? (size_t)w * (h + 16) * 2 : (size_t)stride * h * 2;
		// (GE mode: per buffer [A fbytes][B][result 128][commands][vertices])
		const size_t gb_off = (fbytes + 63) & ~(size_t)63, gr_off = gb_off + (size_t)w * h * 2, gc_off = gr_off + 128,
			     gv_off = gc_off + GEM_CMD_WORDS * 4;
		const size_t ftotal = gem ? gv_off + GEM_VTX * sizeof(gem_vtx) : fbytes;
		for (int i = 0; i < 3; i++) {
			mfb[i] = (uint16_t *)memalign(64, ftotal);
			if (mfb[i])
				memset(mfb[i], 0, ftotal);
			if (gem && mfb[i]) {
				uint8_t *base = (uint8_t *)mfb[i];
				gem_slot *s = &g_gslot[i];
				s->spr_a = mfb[i];
				s->spr_b = (uint16_t *)(base + gb_off);
				s->res = (gem_result *)(base + gr_off);
				s->cmd = (uint32_t *)(base + gc_off);
				s->cmd_ge = (uint32_t)(uintptr_t)s->cmd;
				s->cmd_max = GEM_CMD_WORDS;
				s->vtx = (gem_vtx *)(base + gv_off);
				s->vtx_ge = (uint32_t)(uintptr_t)s->vtx;
				s->vtx_max = GEM_VTX;
				s->cmd[0] = 0x0b000000u; // (CALLing a buffer the ME has not drawn yet: immediate RET)
				sceKernelDcacheWritebackInvalidateRange(base, ftotal); // (leave no lines of this memory in the main cache)
			}
			if (idx_mode) {
				mclut[i] = (uint16_t *)memalign(64, 0x800 * 2);
				if (mclut[i])
					memset(mclut[i], 0, 0x800 * 2);
				else
					idx_mode = 0;
			}
		}
		if (!idx_mode || !(mfb[0] && mfb[1] && mfb[2]))
			gem = 0;
#ifdef REI_LOG_ENABLED
		if (gem) { // (dev) do hand-built GE commands match sceGu — not enabled, a mismatch can hang the GE
			extern int video_gem_sgu_words(uint32_t *out, int max);
			uint32_t a[64], b[16];
			const int na = video_gem_sgu_words(a, 64), nb = gem_selftest_words(b);
			int miss = 0;
			for (int i = 0; i < nb; i++) {
				int f = 0;
				for (int j = 0; j < na && !f; j++)
					f = a[j] == b[i];
				if (!f) {
					REI_LOG("reirem: gem selftest: %08x not in sceGu output\n", (unsigned)b[i]);
					miss++;
				}
			}
			for (int j = 0; j < na; j++)
				REI_LOG("reirem: gem selftest: sceGu %08x\n", (unsigned)a[j]);
			REI_LOG("reirem: gem selftest: %d of %d missing\n", miss, nb);
			if (miss)
				gem = 0;
		}
#endif
		if (gem == 2) {
			gm72_config gc;
			memset(&gc, 0, sizeof(gc));
			gc.atlas_ge[0] = gm72_atlas[0];
			gc.atlas_ge[1] = gm72_atlas[1];
			gc.clut_ge = video_gm72_clut_addr();
			gc.spr_ge = video_index_copy_addr(w);
			gc.spr_tbw = video_index_copy_tbw(w);
			gc.get_slot = gem_get_slot;
			if (gm72_init(&gm72_tiles, &gc) == 0) {
				gm72_nm = gm72_masks(gm72_mask);
				rei_m72_set_ge(&gm72_ops, 0);
				sceKernelDcacheWritebackAll(); // (so the ME reads config and buffer table from memory)
			} else {
				gem = 0;
			}
			REI_LOG("reirem: other: M72 GE init %d, masks %d, tiles %d %d, sprite tex %08x tbw %d\n", gem == 2, gm72_nm,
				gm72_tiles.ntiles[0], gm72_tiles.ntiles[1], (unsigned)gc.spr_ge, gc.spr_tbw);
			if (!gem)
				video_layers_free();
		} else if (gem) {
			uint32_t cl[2];
			video_gem_cluts(cl);
			gem_init(&gem_tiles, (uint32_t)(uintptr_t)video_atlas_addr(), cl, gem_get_slot);
			rei_m107_set_ge(&gem_ops, 0);
			sceKernelDcacheWritebackAll(); // (so the ME reads config and buffer table from memory)
		} else if (video_atlas_addr() || gm72) {
			video_layers_free();
		}
		if (mfb[0] && mfb[1] && mfb[2]) {
			g_game_loop = 1; // (main handles cleanup on HOME exit: the ME must be returned to firmware)
			power_lock(1);   // (sleep only after main takes down the ME)
			// YM2151 synthesis on the ME too (sound 1 frame late, same result — harness --ym-me). On by default (2026-10-13: install built with meym
			// play/game-switch/exit tested, play_check MEYM=1 passed). Disabled by app folder nomeym or dev loop host0:/rei_nomeym
			int ym_me = 1;
			{
				SceIoStat st;
				char mp[128];
				snprintf(mp, sizeof(mp), "%snomeym", g_app_dir);
				if (sceIoGetstat(mp, &st) >= 0)
					ym_me = 0;
#ifdef REI_LOG_ENABLED
				if (sceIoGetstat("host0:/rei_nomeym", &st) >= 0)
					ym_me = 0;
#endif
			}
			if (g_mix)
				ym_me = 0; // (mode 3: YM2151 is synthesized by the ME's sound side as is)
			ym_me = ym_me && rei_core_ym_me_enable(1) == 0; // (before starting the ME: main fills the ME-side state)
			g_ym_me_on = ym_me;
			me_render_set_ym(ym_me);
			me_render_set_index(idx_mode, (void *const *)mclut);
			me_render_set_index_bytes((uint32_t)fbytes);
			extern void (*rei_sound_line_hook)(void);
			rei_sound_line_hook = g_mix ? me_mix_line : NULL; // (before starting the ME: the ME reads it from memory at start)
#ifdef REI_DRC
			if (g_mix) { // (test host0:/rei_mixdrc35) ME V35 dynarec too: code area reserved here (main), cache flush for the ME, no clock
				extern int rei_drc35_enabled, rei_drc35_on_me;
				extern int rei_drc35_preinit(void);
				extern void (*rei_drc35_cache_flush)(void *p, int len);
				extern unsigned (*rei_drc35_now_us)(void), (*rei_drc35_clock32)(void), (*rei_drc35_clock_fine)(void);
				int want = 0;
#ifdef REI_LOG_ENABLED
				SceIoStat dst;
				want = sceIoGetstat("host0:/rei_mixdrc35", &dst) >= 0;
#endif
				const int pre = want && rei_drc35_enabled ? rei_drc35_preinit() : 0;
				if (pre) {
					rei_drc35_cache_flush = me_drc_flush;
					rei_drc35_now_us = NULL;
					rei_drc35_clock32 = NULL;
					rei_drc35_clock_fine = NULL;
					rei_drc35_on_me = 1;
				}
				REI_LOG("reirem: other: V35 dynarec on ME %d (want %d)\n", pre, want);
			}
#endif
			const int r = g_mix ? me_mix_start((void *const *)mfb, stride * 2, alen) : me_render_start((void *const *)mfb, stride * 2);
#ifdef REI_YM_SPLIT // (YM split only in test builds — always including it grew the shared area, shifted layout, and caused a broken game list and M92 sprite
                    //  flicker on device, reported 2026-10-16)
			{
				extern int rei_ym_split_on;
				g_ym_split = g_mix && rei_ym_split_on;
			}
#endif
			REI_LOG("reirem: other: ME render start %d, YM2151 on ME %d, sound on ME %d, YM2151 on main %d\n", r, ym_me, g_mix, g_ym_split);
			g_me_render_on = r == 0;
			if (!g_me_render_on && ym_me)
				rei_core_ym_me_enable(0); // (nothing sent: main chip stays)
			if (!g_me_render_on && gem) {
				rei_m107_set_ge(NULL, 0);
				rei_m72_set_ge(NULL, 0);
				gem = 0;
			}
			REI_LOG("reirem: other: GE layers %d (1 M107, 2 M72)\n", gem);
			if (g_me_render_on) {
				if (gem == 2)
					video_set_game_gm72(mfb[2], mclut[2], g_gslot[2].cmd, gm72_mask, gm72_nm, w, h, flags);
				else if (gem)
					video_set_game_gem(mfb[2], g_gslot[2].spr_b, mclut[2], g_gslot[2].cmd, g_gslot[2].res, w, h, flags);
				else if (idx_mode)
					video_set_game_index(mfb[2], mclut[2], nclut, w, h, flags);
				else
					video_set_game_buffer(mfb[2], w, h, stride, flags);
			}
			REI_LOG("reirem: other: index mode %d (palette %d) idx %p %p %p clut %p %p %p w %d h %d\n", idx_mode, nclut, (void *)mfb[0],
				(void *)mfb[1], (void *)mfb[2], (void *)mclut[0], (void *)mclut[1], (void *)mclut[2], w, h);
#ifdef REI_LOG_ENABLED
			{ // (diag) µs in host0:/rei_meburn: keep the ME busy that long after drawing (ME headroom check — main times the same loop)
				SceUID fd = sceIoOpen("host0:/rei_meburn", PSP_O_RDONLY, 0);
				if (fd >= 0 && g_me_render_on) {
					char b[16] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					const int us = atoi(b);
					volatile uint32_t acc = 0;
					const uint64_t c0 = now_us();
					for (uint32_t i = 200000; i; i--)
						acc += i;
					const uint64_t dt = now_us() - c0;
					const uint32_t n = dt ? (uint32_t)((uint64_t)us * 200000 / dt) : 0;
					me_render_set_burn(n);
					REI_LOG("reirem: other: ME burn %d us = %u iterations (200000 in %u us)\n", us, (unsigned)n, (unsigned)dt);
				}
				if (fd >= 0)
					sceIoClose(fd);
			}
#endif
		}
	}
#endif
	REI_LOG("reirem: other: deferred %d, ME render %d\n", deferred, g_me_render_on);
#ifdef REI_PSP_ME
	if (g_mix && !g_me_render_on) { // (mode 3 failed to start: the threaded core stalls main waiting without a sound side — back to list)
		rei_message_screen("Sound on ME failed", "Remove m107mesnd from the app folder");
		g_to_list = 1;
		g_quit = 1;
	}
#ifdef REI_LOG_ENABLED
	if (g_mix && g_me_render_on) {
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_meclock", &st) >= 0) { // (diag) ME time breakdown
			g_mclk_quit = 0;
			g_mclk_th = sceKernelCreateThread("meclock", mix_clock_thread, 0x10, 0x800, 0, NULL);
			if (g_mclk_th >= 0)
				sceKernelStartThread(g_mclk_th, 0, NULL);
			REI_LOG("reirem: other: ME clock thread %d\n", (int)g_mclk_th);
		}
	}
#endif
#endif
	rei_loading_progress(1000);
	rei_loading_done();
#ifdef REI_PSP_ME
	me_sound_trace("loading done", 0);
#endif

	int dev_stop = 0, menu_open = 0;
	uint32_t prev = 0, vc_target = 0;
	int vc_started = 0, fps_n = 0;
	uint64_t fps_t0 = now_us(), acc_emu = 0, max_emu = 0;
	uint64_t acc_pres = 0, acc_vbw = 0, acc_iter = 0, max_iter = 0, acc_gesync = 0, t_iter = now_us();
#ifdef REI_PSP_ME
	int ym_over = 0, ym_main_over = 0, ym_cool = 0; // (YM2151-on-ME overflow decisions of this game)
#endif
#ifdef REI_LOG_ENABLED
	g_pad_n = 0;
	pad_script_load();
#endif
	g_playing = 1;
	g_loop_left = 0;
	for (int f = 0; !g_quit; f++) {
#ifdef REI_LOG_ENABLED
		g_wd_frame = f; // (dev-loop watchdog thread: progress check)
		g_wd_beat++;
		if (f % 10 == 0) { // stop request (host0:/rei_stop)
			SceIoStat st;
			if (sceIoGetstat("host0:/rei_stop", &st) >= 0) {
				dev_stop = 1;
				g_quit = 1;
			}
		}
#endif
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
#ifdef REI_LOG_ENABLED
		if (g_pad_n > 0) { // (dev loop host0:/rei_pad: menu/save tests, in-game measurement) script only — mixing device buttons/analog made
			                // held buttons swallow press edges (rtype coin/start ignored, 2026-10-14)
			pad.Buttons = pad_script(g_wd_beat);
			pad.Lx = pad.Ly = 128;
		}
		pad.Buttons |= rei_test_press();
#endif
		const uint32_t btn = pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_SQUARE |
			PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE | PSP_CTRL_START | PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER |
			PSP_CTRL_RTRIGGER);
		const int ax = (int)pad.Lx - 128, ay = (int)pad.Ly - 128;
		uint32_t mb = btn;
		if (ay < -STICK_DEAD) mb |= PSP_CTRL_UP;
		if (ay > STICK_DEAD) mb |= PSP_CTRL_DOWN;
		if (ax < -STICK_DEAD) mb |= PSP_CTRL_LEFT;
		if (ax > STICK_DEAD) mb |= PSP_CTRL_RIGHT;
		sleep_test_poll(f);
		if (g_sleep_req) { // (power switch: sleep. The ME does drawing/sound, so after restart resync via temp save)
			video_sync();
			char spath[160];
			sleep_state_path(spath, sizeof(spath));
			sleep_trace("other start", g_mix, 1);
			const int src = save_state_path(spath);
			sleep_trace("state save", src, 0);
#ifdef REI_PSP_ME
			if (g_me_render_on) {
				const int done = me_sound_stop();
				const int er = me_sound_prepare_exit();
				sleep_trace("ME stop", done, 0);
				sleep_trace("ME to firmware", er, 0);
			}
#endif
			sleep_wait();
			if (g_quit)
				break;
#ifdef REI_PSP_ME
			if (g_me_render_on) {
				const int r2 = g_mix ? me_mix_start((void *const *)mfb, stride * 2, alen) : me_render_start((void *const *)mfb, stride * 2);
				sleep_trace("ME restart", r2, 0);
				if (r2 != 0) { // (cannot continue without the ME: exit the game)
					g_me_render_on = 0;
					g_quit = 1;
					break;
				}
			}
#endif
			if (src == 0)
				sleep_trace("state load", load_state_path(spath), 0);
			sceIoRemove(spath);
		}
		if (!menu_open && rei_menu_should_open(mb)) {
			menu_open = 1;
			video_sync();
			rei_menu_set_open(1);
		}
		if (menu_open) {
			const rei_menu_result r = rei_menu_update(mb);
			if (r == REI_MENU_EXIT)
				g_quit = 1;
			if (r == REI_MENU_SELECT_GAME) { // back to the game list in this process (teardown below)
				g_to_list = 1;
				g_quit = 1;
			}
			if (r == REI_MENU_RESET)
				reset_game();
			if (r == REI_MENU_SAVE)
				save_state(rei_menu_state_slot());
			if (r == REI_MENU_LOAD)
				load_state(rei_menu_state_slot());
			if (r == REI_MENU_CLOSE || r == REI_MENU_RESET || r == REI_MENU_LOAD || r == REI_MENU_EXIT ||
				r == REI_MENU_SELECT_GAME) {
				menu_open = 0;
				rei_menu_set_open(0);
			}
			video_present();
			sceDisplayWaitVblankStart();
			vc_started = 0;
			continue;
		}
		{ // quick save SELECT+L, quick load SELECT+R
			const uint32_t qs = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER, ql = PSP_CTRL_SELECT | PSP_CTRL_RTRIGGER;
			if ((mb & qs) == qs && (prev & qs) != qs) {
				video_sync();
				save_state(REI_STATE_QUICK);
			}
			if ((mb & ql) == ql && (prev & ql) != ql) {
				video_sync();
				load_state(REI_STATE_QUICK);
			}
			prev = mb;
		}
		set_game_input(btn, ax, ay);
		memset(audio, 0, (size_t)alen * 2 * sizeof(int16_t));
		const uint64_t t = now_us();
		// (the GE list that last read this buffer was already waited on by the previous video_present)
		fb = fbs[fb_cur];
#ifdef REI_LOG_ENABLED
		// (diag) dev loop: if host0:/rei_nodraw / rei_nosound exist, measure without drawing / sound
		static int no_draw = -1, no_sound;
		if (no_draw < 0) {
			SceIoStat st;
			no_draw = sceIoGetstat("host0:/rei_nodraw", &st) >= 0;
			no_sound = sceIoGetstat("host0:/rei_nosound", &st) >= 0;
			extern int rei_m72_vlog; // (diag) M72 screen-off bit log
			rei_m72_vlog = sceIoGetstat("host0:/rei_vlog", &st) >= 0;
			extern int rei_defer_vlog;
			rei_defer_vlog = rei_m72_vlog;
			extern uint32_t rei_defer_skip[16]; // (diag) stages render (ME) skips (rei_defer.cpp)
			SceUID fd = sceIoOpen("host0:/rei_rskip", PSP_O_RDONLY, 0);
			if (fd >= 0) {
				char b[16] = { 0 };
				sceIoRead(fd, b, sizeof(b) - 1);
				sceIoClose(fd);
				REI_SH(rei_defer_skip[0]) = (uint32_t)strtoul(b, NULL, 0);
				REI_LOG("reirem: render skip %u\n", (unsigned)REI_SH(rei_defer_skip[0]));
			}
			REI_LOG("reirem: other: no_draw %d no_sound %d\n", no_draw, no_sound);
		}
		rei_core_run_frame(deferred || no_draw ? NULL : OTHER_FB_W(fb), stride * 2, no_sound ? NULL : audio);
#else
		rei_core_run_frame(deferred ? NULL : OTHER_FB_W(fb), stride * 2, audio);
#endif
		if (deferred && !g_me_render_on) // (ME failed to start: main does deferred drawing directly)
			rei_core_render_frame(OTHER_FB_W(fb), stride * 2);
		const uint64_t dt = now_us() - t;
		acc_emu += dt;
		if (dt > max_emu)
			max_emu = dt;
#ifdef REI_LOG_ENABLED
		{ // (diag) slow frame (emu over 20ms): V33/V30/V35 dynarec compile/flush/interpreted instruction counts in that frame — cause of occasional in-game spikes
			extern unsigned long long rei_drc_stats[13], rei_drc35_stats[13];
			static unsigned long long l[11];
			extern unsigned rei_drc_tph[4];
			static unsigned lt[3];
			const unsigned long long *s = rei_drc_stats, *d = rei_drc35_stats;
			extern unsigned long long rei_defer_wait_us; // (µs main waited on render (ME) in that frame)
			static unsigned long long lw;
			const unsigned wme = (unsigned)(rei_defer_wait_us - lw);
			lw = rei_defer_wait_us;
			static uint32_t lmw; // (mode 3: iterations main waited for sound/draw slots, that frame)
			const uint32_t mw = g_main_waits - lmw;
			lmw = g_main_waits;
			if (dt > (g_quiet_log ? 15500u : 20000u) && f > 300)
				REI_LOG("slow f%d: emu %u.%u ms (ME wait %u us, main waits %u), nec compiled +%llu (%llu insns, %llu us, sync %llu us, dec %u gen %u end %u) flushes +%llu interp +%llu, v35 compiled +%llu (%llu insns, %llu us) flushes +%llu interp +%llu\n", f,
					(unsigned)(dt / 1000), (unsigned)(dt / 100 % 10), wme, (unsigned)mw, s[3] - l[0], s[7] - l[6], s[10] - l[7], s[11] - l[10], rei_drc_tph[0] - lt[0], rei_drc_tph[1] - lt[1], rei_drc_tph[2] - lt[2], s[4] - l[1], s[2] - l[2], d[3] - l[3], d[7] - l[8], d[10] - l[9],
					d[4] - l[4], d[2] - l[5]);
			if (s[4] != l[1]) { // (code cache flushed in that frame: reason totals — nec_drc.inc DRC_WHY)
				extern unsigned rei_drc_flush_why[6];
				REI_LOG("  flush f%d why (total): code %u table %u overflow %u pages %u idle %u\n", f, rei_drc_flush_why[0], rei_drc_flush_why[1],
					rei_drc_flush_why[2], rei_drc_flush_why[3], rei_drc_flush_why[4]);
			}
			{ // (that frame's in-block V33 / M107 main line clock — host0:/rei_blkclk, event counts on REI_HWPROF builds — and block entry count)
				extern unsigned rei_m107_us[4], rei_drc_blk_acc;
				static unsigned lb, lm, lv;
				static unsigned long long le;
				if (dt > (g_quiet_log ? 15500u : 20000u) && f > 300 && rei_drc_blk_acc != lb)
					REI_LOG("  slow f%d blk %u main %u vezrun %u entries %llu\n", f, rei_drc_blk_acc - lb, rei_m107_us[0] - lm, rei_m107_us[3] - lv, s[0] - le);
				lb = rei_drc_blk_acc; lm = rei_m107_us[0]; lv = rei_m107_us[3]; le = s[0];
			}
			l[0] = s[3]; l[1] = s[4]; l[2] = s[2]; l[3] = d[3]; l[4] = d[4]; l[5] = d[2]; l[6] = s[7]; l[7] = s[10]; l[8] = d[7]; l[9] = d[10]; l[10] = s[11]; lt[0] = rei_drc_tph[0]; lt[1] = rei_drc_tph[1]; lt[2] = rei_drc_tph[2];
		}
#endif
#ifdef REI_PROFILE
		{ // (profile) collect spans per frame into the 60-frame sum; slow frames are logged per span on their own
			uint64_t a1[REI_PROF_COUNT];
			const int np = rei_core_prof_take(a1, REI_PROF_COUNT);
			for (int i = 0; i < np; i++)
				g_prof60[i] += a1[i];
			if (dt > (g_quiet_log ? 15500u : 20000u) && f > 300) {
				char line[256];
				int len = 0;
				for (int i = 0; i < np && len < (int)sizeof(line) - 24; i++)
					if (a1[i] && (i == REI_PROF_FRAME || i >= REI_PROF_Z80 || i == REI_PROF_XFER || i == REI_PROF_LAYERS || i == REI_PROF_SPRITES || i == REI_PROF_FAST || i == REI_PROF_V33 || i == REI_PROF_V35 || i == REI_PROF_YM || i == REI_PROF_SOUND))
						len += snprintf(line + len, sizeof(line) - len, " %s %.1f", rei_core_prof_name(i), a1[i] / 1000.0);
				REI_LOG("  slow f%d prof ms:%s\n", f, line);
			}
		}
#endif
#ifdef REI_PSP_ME
		if (!g_mix) // (mode 3: the ME puts sound in the ring — audio thread does me_sound_pop)
#endif
		other_push(audio);
		const uint64_t t_pres = now_us();
		video_sync(); // (log: time the wait for previous frame's GE composition separately — video_present does the same wait first)
		acc_gesync += now_us() - t_pres;
		if (g_me_render_on) {
			const uint32_t done = me_render_done();
			if (done != me_shown) { // next buffer the ME finished — do not skip ahead to the newest (sprites drawn on alternate frames
				                    // flickered or vanished when one buffer was skipped: vigilante/rtype device reports). The ME draws at most shown + 2 ahead
				if ((int32_t)(done - me_shown) < 0) // (reset/load restarts the render counter from 0)
					me_shown = done;
				else
					me_shown++;
				if (me_shown) {
					if (gem == 2) {
						const int k = (me_shown - 1) % 3;
						video_set_game_gm72(mfb[k], mclut[k], g_gslot[k].cmd, gm72_mask, gm72_nm, w, h, flags);
					} else if (gem) {
						const int k = (me_shown - 1) % 3;
						video_set_game_gem(mfb[k], g_gslot[k].spr_b, mclut[k], g_gslot[k].cmd, g_gslot[k].res, w, h, flags);
#ifdef REI_LOG_ENABLED
						{ // (diag) host0:/rei_gemdump "frame": dump that frame's result, GE commands and vertices to host0:/gem_<frame>.bin (read uncached)
							static int gd = -2;
							if (gd == -2) {
								gd = -1;
								SceUID fd = sceIoOpen("host0:/rei_gemdump", PSP_O_RDONLY, 0);
								if (fd >= 0) {
									char b[16] = { 0 };
									sceIoRead(fd, b, sizeof(b) - 1);
									sceIoClose(fd);
									gd = atoi(b);
								}
							}
							if (gd >= 0 && (int)me_shown == gd) {
								const gem_slot *s = &g_gslot[k];
								const gem_result *res = (const gem_result *)((uintptr_t)s->res | 0x40000000u);
								static uint32_t tmp[GEM_CMD_WORDS];
								SceUID o = sceIoOpen("host0:/gem_dump.bin", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
								if (o >= 0) {
									const uint32_t hdr[4] = { (uint32_t)(uintptr_t)s->cmd, (uint32_t)(uintptr_t)s->vtx, (uint32_t)GEM_CMD_WORDS,
										(uint32_t)GEM_VTX };
									sceIoWrite(o, hdr, sizeof(hdr));
									for (int i = 0; i < (int)(sizeof(gem_result) / 4); i++)
										tmp[i] = ((const volatile uint32_t *)res)[i];
									sceIoWrite(o, tmp, sizeof(gem_result));
									const volatile uint32_t *c = (const volatile uint32_t *)((uintptr_t)s->cmd | 0x40000000u);
									for (int i = 0; i < GEM_CMD_WORDS; i++)
										tmp[i] = c[i];
									sceIoWrite(o, tmp, GEM_CMD_WORDS * 4);
									const volatile uint32_t *v = (const volatile uint32_t *)((uintptr_t)s->vtx | 0x40000000u);
									for (int i = 0; i < GEM_CMD_WORDS && i < GEM_VTX * (int)sizeof(gem_vtx) / 4; i++)
										tmp[i] = v[i];
									sceIoWrite(o, tmp, GEM_CMD_WORDS * 4);
									sceIoClose(o);
									REI_LOG("reirem: gem dump frame %d slot %d\n", gd, k);
								}
							}
						}
#endif
					} else if (idx_mode)
						video_set_game_index(mfb[(me_shown - 1) % 3], mclut[(me_shown - 1) % 3], nclut, w, h, flags);
					else
						video_set_game_buffer(mfb[(me_shown - 1) % 3], w, h, stride, flags);
#ifdef REI_LOG_ENABLED
					{ // (diag) host0:/rei_fdump "start count": from that frame for count frames, read the submitted buffer (index+palette or screen) uncached
					  //  into host0:/fd_<frame>.bin (flicker investigation — compare against harness screens). 16-byte header: w h stride idx_mode
						static int fd_from = -2, fd_n;
						if (fd_from == -2) {
							fd_from = -1;
							SceUID fd = sceIoOpen("host0:/rei_fdump", PSP_O_RDONLY, 0);
							if (fd >= 0) {
								char b[32] = { 0 };
								sceIoRead(fd, b, sizeof(b) - 1);
								sceIoClose(fd);
								sscanf(b, "%d %d", &fd_from, &fd_n);
							}
						}
						if (fd_from >= 0 && (int)me_shown >= fd_from && (int)me_shown < fd_from + fd_n) {
							char p[64];
							snprintf(p, sizeof(p), "host0:/fd_%05u.bin", (unsigned)me_shown);
							static uint16_t *tmp; // (only when dumping — boards with tight memory)
							if (!tmp)
								tmp = (uint16_t *)malloc(512 * 288 * 2);
							SceUID o = tmp ? sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777) : -1;
							if (o >= 0) {
								const int hdr[4] = { w, h, stride, idx_mode };
								sceIoWrite(o, hdr, sizeof(hdr));
								const uint16_t *src = (const uint16_t *)((uintptr_t)mfb[(me_shown - 1) % 3] | 0x40000000u);
								const int n = idx_mode ? w * h : stride * h;
								for (int i = 0; i < n && i < 512 * 288; i++)
									tmp[i] = src[i];
								sceIoWrite(o, tmp, (size_t)n * 2);
								if (idx_mode) {
									const uint16_t *c = (const uint16_t *)((uintptr_t)mclut[(me_shown - 1) % 3] | 0x40000000u);
									for (int i = 0; i < 0x800; i++)
										tmp[i] = c[i];
									sceIoWrite(o, tmp, 0x800 * 2);
								}
								sceIoClose(o);
							}
						}
					}
#endif
				}
			}
#ifdef REI_LOG_ENABLED
			{
				extern uint32_t video_dcap_tag;
				video_dcap_tag = me_shown | (done << 16);
			}
#endif
			video_present();
			me_render_shown(me_shown); // (the GE list reading the last buffer was waited on by video_present)
		} else {
#ifndef OTHER_FB_UNCACHED
			sceKernelDcacheWritebackRange(fb, (unsigned)(stride * h * 2));
#endif
			video_set_game_buffer(fb, w, h, stride, flags);
			fb_cur ^= 1;
			video_present();
		}
#ifdef REI_LOG_ENABLED
		{ // (diag) host0:/rei_gesync: wait on the submitted composition list immediately to time GE composition alone (added to the GE wait column)
			static int ge_diag = -1;
			if (ge_diag < 0) {
				SceIoStat st;
				ge_diag = sceIoGetstat("host0:/rei_gesync", &st) >= 0;
			}
			if (ge_diag) {
				const uint64_t g0 = now_us();
				video_sync();
				acc_gesync += now_us() - g0;
			}
		}
#endif
		const uint64_t t_pace = now_us();
		acc_pres += t_pace - t_pres;
		{ // frame pacing: same vblank slot matching as the M92 path (no wait when late)
			const uint32_t vc = (uint32_t)sceDisplayGetVcount();
			if (!vc_started || (int32_t)(vc - vc_target) > 2) {
				vc_target = vc;
				vc_started = 1;
			}
#ifdef REI_DRC
			drc_idle_work((int32_t)(vc_target - vc) == 1 ? 16683 - (int)(now_us() - t_iter) : -1);
#endif
#ifdef REI_PSP_ME
			if (g_mix && g_ym_split) { // (mode 3 + YM split: main's share of YM2151 while waiting — the ME waits at frame end)
				while ((int32_t)(vc_target - (uint32_t)sceDisplayGetVcount()) > 0 && !g_quit)
					if (!rei_core_main_poll())
						sceKernelDelayThread(200);
			} else
#endif
			while ((int32_t)(vc_target - (uint32_t)sceDisplayGetVcount()) > 0 && !g_quit)
				sceDisplayWaitVblankStart();
			vc_target++;
		}
		{ // (log) loop iteration, output, vblank wait
			const uint64_t now = now_us();
			acc_vbw += now - t_pace;
			const uint64_t lp = now - t_iter;
			acc_iter += lp;
			if (lp > max_iter)
				max_iter = lp;
			if (lp > 17500) // (diag) iteration that missed vblank (previous frame shown once more)
				g_late_n++;
			t_iter = now;
		}
		if (++fps_n == 30) {
			const uint64_t now = now_us();
			const unsigned us = (unsigned)((now - fps_t0) / 30);
			rei_fps_set(us ? 1000000.0f / us : 0.0f);
			fps_t0 = now;
			fps_n = 0;
		}
#ifdef REI_PSP_GPROF
		{ // (diag) measure only from frame 900 to 1500 and write to host0 (gameplay section when the rei_pad emulation is used)
			extern void gprof_start(void);
			extern void gprof_stop(const char *filename, int should_dump);
			static int g_from = 900, g_to = 1500; // range from host0:/rei_gprof "start end" if present
			if (f == 0) {
				gprof_stop(NULL, 0);
				SceUID fd = sceIoOpen("host0:/rei_gprof", PSP_O_RDONLY, 0);
				if (fd >= 0) {
					char b[32] = { 0 };
					sceIoRead(fd, b, sizeof(b) - 1);
					sceIoClose(fd);
					sscanf(b, "%d %d", &g_from, &g_to);
				}
			}
			if (f == g_from) gprof_start();
			if (f == g_to) { gprof_stop("host0:/gmon.out", 1); REI_LOG("reirem: gprof written (%d..%d)\n", g_from, g_to); }
		}
#endif
		if ((f + 1) % 60 == 0) {
			scePowerTick(PSP_POWER_TICK_ALL);
			if (scePowerGetCpuClockFrequency() < 333 || scePowerGetBusClockFrequency() < 166)
				scePowerSetClockFrequency(333, 333, 166);
#ifdef REI_PSP_ME
			{ // ME overflows with YM2151 on the ME (main waits on drawing over 4ms avg per second, 3s in a row): move synthesis back to main
				// (riskchal: ME drawing alone is heavy; with YM too it is 24ms, 21ms with main synthesis)
				extern unsigned long long rei_defer_wait_us;
				static unsigned long long ym_lw;
				const unsigned long long w = rei_defer_wait_us - ym_lw;
				ym_lw = rei_defer_wait_us;
				static int ym_keep = -1; // (diag) host0:/rei_ymkeep: never move YM2151 back to main even if the ME is busy
				if (ym_keep < 0) {
					ym_keep = 0;
#ifdef REI_LOG_ENABLED
					SceIoStat st;
					ym_keep = sceIoGetstat("host0:/rei_ymkeep", &st) >= 0;
#endif
				}
				// Channel split (PERF PP35): on overflow first move 4 YM channels (0x55) -> 6 (0x77) to main, then all to main if still overflowing.
				// On by default (play-tested 2026-10-17: install with the test-enable file was "the most perfect") — disable: app folder nomeysplit,
				// dev host0:/rei_nomeysplit
				static int ym_split = -1;
				if (ym_split < 0) {
					SceIoStat st;
					char mp[128];
					snprintf(mp, sizeof(mp), "%snomeysplit", g_app_dir);
					ym_split = sceIoGetstat(mp, &st) < 0;
#ifdef REI_LOG_ENABLED
					if (sceIoGetstat("host0:/rei_nomeysplit", &st) >= 0)
						ym_split = 0;
#endif
				}
#if defined(REI_LOG_ENABLED) || defined(REI_PSP_TESTGAME)
				{ // (test) host0:/rei_ysplit: change the split at fixed frames — so play_check exercises the switch path even on the demo screen
					static int ys_test = -1;
					if (ys_test < 0) {
						SceIoStat st;
						ys_test = sceIoGetstat("host0:/rei_ysplit", &st) >= 0;
						if (ys_test)
							ym_keep = 1; // (disable the automatic decision below)
					}
					const int at = f + 1;
					const int m = !ys_test || !g_ym_me_on ? -1 : at == 900 ? 0x55 : at == 1200 ? 0x77 : at == 1500 ? 0x55 : at == 1800 ? 0 : -1;
					if (m >= 0) {
						const int r = rei_core_ym_me_split(m); // (outside the log macro argument — recurrence rule 5)
						REI_LOG("reirem: other: YM2151 split test %02x -> %d\n", m, r);
						(void)r;
					}
				}
#endif
				if (g_ym_me_on && f >= 1200 && !ym_keep) { // (the ME is briefly heavy right after start — dbreed moved back at frame 240, in-game 18.6->20.3ms)
					// (after splitting, main's wait at frame end for the ME's YM share also counts as overflow)
					extern unsigned long long rei_ymm_wait_us[2];
					static unsigned long long yw_l;
					const unsigned long long yw = rei_ymm_wait_us[0] + rei_ymm_wait_us[1] - yw_l;
					yw_l = rei_ymm_wait_us[0] + rei_ymm_wait_us[1];
					// (YM synthesis on main costs 4~5ms: when moving everything, keeping it on the ME is better if the wait is less. A split moves only part, so 1.5ms)
					// If main overflows while split (main work alone over 16ms without waiting — misses vblank, 3s in a row), reduce main's share one step — raised in heavy scenes, but
					// some games are main-bound in play (dsoccr94). For 20s after reducing, raising again needs 5s of continuous overflow (prevents flip-flopping)
					const int cur = rei_core_ym_me_split_mask();
					const unsigned long long own = acc_emu > w + yw ? acc_emu - w - yw : 0;
					ym_over = w + yw > 60 * (ym_split ? 1500 : 4000) ? ym_over + 1 : 0;
					ym_main_over = ym_split && cur > 0 && own > 60 * 16000 && w + yw < 60 * 300 ? ym_main_over + 1 : 0;
					if (ym_cool > 0)
						ym_cool--;
					if (ym_over >= (ym_cool ? 5 : 3)) {
						ym_over = 0;
						const int next = !ym_split ? -1 : cur == 0 ? 0x55 : cur == 0x55 ? 0x77 : -1;
						if (next > 0 && rei_core_ym_me_split(next) == 0) {
							REI_LOG("reirem: other: ME busy (wait %.1f ms/frame), YM2151 channels %02x on the main CPU\n", (w + yw) / 60000.0,
								next);
						} else {
							rei_core_ym_me_release();
							g_ym_me_on = 0;
							REI_LOG("reirem: other: ME busy (wait %.1f ms/frame), YM2151 back on the main CPU\n", (w + yw) / 60000.0);
						}
					} else if (ym_main_over >= 3) {
						ym_main_over = 0;
						const int next = cur == 0x77 ? 0x55 : 0;
						if (rei_core_ym_me_split(next) == 0) {
							ym_cool = 20;
							REI_LOG("reirem: other: main busy (%.1f ms/frame), YM2151 channels %02x on the main CPU\n", own / 60000.0, next);
						}
					}
				}
			}
#endif
			// (quiet measurement host0:/rei_quietlog: writing a dozen lines over USB every 60 frames itself delayed one iteration — dsoccr94 late
			//  iterations almost always 1 per window. Write only every 600 frames; late is the count accumulated meanwhile)
			if (!g_quiet_log || (f + 1) % 600 == 0) {
			REI_LOG("frame=%d\n  emu   avg %.1f max %.1f ms\n", f + 1, acc_emu / 60000.0, max_emu / 1000.0);
			{ // (diag) driver RAM hash — per-frame comparison with harness REI_RAMH
				extern uint32_t rei_core_ram_hash(void);
				REI_LOG("ramh %d %08x\n", f + 1, (unsigned)rei_core_ram_hash());
			}
			if (g_ym_me_on) { // (diag) time main waited on YM2151-on-ME (included in emu): ring is full, previous frame's share
				extern unsigned long long rei_ymm_wait_us[2];
				static unsigned long long l0, l1;
				REI_LOG("  ym me wait ring %.2f frame %.2f ms\n", (rei_ymm_wait_us[0] - l0) / 60000.0, (rei_ymm_wait_us[1] - l1) / 60000.0);
				l0 = rei_ymm_wait_us[0];
				l1 = rei_ymm_wait_us[1];
			}
			{
				extern unsigned long long rei_defer_wait_us; // (deferral: time main waited on render (ME), included in emu)
				static unsigned long long last_wait;
				REI_LOG("  loop  avg %.1f max %.1f ms, present %.1f vblank wait %.1f GE wait %.1f ME wait %.1f late %u\n",
					acc_iter / 60000.0, max_iter / 1000.0, acc_pres / 60000.0, acc_vbw / 60000.0, acc_gesync / 60000.0,
					(rei_defer_wait_us - last_wait) / 60000.0, g_late_n);
				g_late_n = 0;
				last_wait = rei_defer_wait_us;
				{ // (diag) audio underruns, peak level (/60f, all other boards)
					static int lu;
					REI_LOG("  audio /60f: underruns %d peak %d\n", g_audio_underruns - lu, g_audio_peak);
					lu = g_audio_underruns;
					g_audio_peak = 0;
				}
#ifdef REI_PSP_ME
				if (g_mix) { // (diag) mode 3: sound frames made / dropped by the ME, frames drawn in the wait hook (/60f)
					static uint32_t lsf, lsd, lmr;
					const uint32_t sf = me_sound_frames(), sd = me_sound_drops(), mr = me_mix_renders();
					static uint32_t lmm;
				const uint32_t mm = me_mix_renders_main();
				REI_LOG("  ME mix /60f: sound frames %u drops %u renders %u (main %u)\n", (unsigned)(sf - lsf), (unsigned)(sd - lsd), (unsigned)(mr - lmr),
					(unsigned)(mm - lmm));
				lmm = mm;
#ifdef REI_LOG_ENABLED
				if (g_mclk_th >= 0) { // (diag) ME ms/frame: drawing, main wait, whole sound frame (incl. both)
					static uint32_t lr, lw, lt;
					uint32_t r, wt, tt;
					me_mix_times(&r, &wt, &tt);
					REI_LOG("  ME ms/f: render %.2f wait %.2f frame %.2f -> sound %.2f\n", (r - lr) / 60000.0, (wt - lw) / 60000.0, (tt - lt) / 60000.0,
						((tt - lt) - (r - lr) - (wt - lw)) / 60000.0);
					lr = r; lw = wt; lt = tt;
				}
#endif
					lsf = sf; lsd = sd; lmr = mr;
				}
#endif
				{ // (diag) M107 single-thread pipeline: main lines (V33), sound lines (V35/timers), sound finish ms/frame
					extern unsigned rei_m107_us[4], rei_drc_blk_acc;
					static unsigned lm[4], lb;
					if (rei_m107_us[0] != lm[0])
						REI_LOG("  m107 ms/f: main %.2f sound %.2f end %.2f VezRun %.2f (V33 blocks %.2f)\n", (rei_m107_us[0] - lm[0]) / 60000.0,
							(rei_m107_us[1] - lm[1]) / 60000.0, (rei_m107_us[2] - lm[2]) / 60000.0, (rei_m107_us[3] - lm[3]) / 60000.0, (rei_drc_blk_acc - lb) / 60000.0);
					lm[0] = rei_m107_us[0]; lm[1] = rei_m107_us[1]; lm[2] = rei_m107_us[2]; lm[3] = rei_m107_us[3]; lb = rei_drc_blk_acc;
				}
#ifdef REI_DRC
				{ // (diag) deferred drawing: avg/max spans per frame over 60 frames (rei_defer.cpp)
					extern unsigned long long rei_defer_stats[3];
					static unsigned long long lf, ls;
					if (rei_defer_stats[0] > lf)
						REI_LOG("  defer segs /f: avg %.1f max %llu\n", (double)(rei_defer_stats[1] - ls) / (double)(rei_defer_stats[0] - lf),
							rei_defer_stats[2]);
					lf = rei_defer_stats[0];
					ls = rei_defer_stats[1];
					rei_defer_stats[2] = 0;
				}
				{ // (diag) V35 dynarec over 60 frames: block entries, block instrs, interpreted instrs, compiled blocks, cache flushes
					extern unsigned long long rei_drc35_stats[13];
					extern int rei_drc35_enabled;
					static unsigned long long l35[5];
					const unsigned long long *d = rei_drc35_stats;
					if (d[0] || d[2])
						REI_LOG("  drc35 /60f: on %d runs %llu binsns %llu interp %llu compiled %llu flushes %llu\n", rei_drc35_enabled,
							d[0] - l35[0], d[1] - l35[1], d[2] - l35[2], d[3] - l35[3], d[4] - l35[4]);
					l35[0] = d[0]; l35[1] = d[1]; l35[2] = d[2]; l35[3] = d[3]; l35[4] = d[4];
					{ // (diag) cycles skipped by V35 automatic idle-loop skipping /60f
						extern unsigned long long rei_v25_auto_skipped[4];
						static unsigned long long ls;
						const unsigned long long sk35 = rei_v25_auto_skipped[0] + rei_v25_auto_skipped[1]; // (M107 sound is chip 1)
						REI_LOG("  v35 auto skipped /60f: %llu\n", sk35 - ls);
						ls = sk35;
						extern unsigned long long rei_auto_skipped(int cpu); // (V33/V30 automatic idle-loop skip cycles /60f)
						static unsigned long long l33;
						REI_LOG("  nec auto skipped /60f: %llu\n", rei_auto_skipped(0) - l33);
						l33 = rei_auto_skipped(0);
						extern unsigned long long rei_drc_stats[13];
						static unsigned long long lr[3];
						REI_LOG("  nec drc /60f: dispatches %llu (insns %llu) interp %llu\n", rei_drc_stats[0] - lr[0], rei_drc_stats[1] - lr[1],
							rei_drc_stats[2] - lr[2]);
						lr[0] = rei_drc_stats[0]; lr[1] = rei_drc_stats[1]; lr[2] = rei_drc_stats[2];
					}
					extern unsigned long long rei_drc35_dbg[6];
					static unsigned long long lg[4];
					const unsigned long long *g = rei_drc35_dbg;
					if (d[0] || d[2])
						REI_LOG("  drc35 why: tables %llu rec %llu timer %llu noblock %llu\n", g[0] - lg[0], g[1] - lg[1],
							g[2] - lg[2], g[3] - lg[3]);
					lg[0] = g[0]; lg[1] = g[1]; lg[2] = g[2]; lg[3] = g[3];
				}
#endif
			}
			} // (quiet measurement)
			acc_pres = acc_vbw = acc_iter = max_iter = acc_gesync = 0;
			acc_emu = max_emu = 0;
#ifdef REI_PROFILE
			{ // per-span ms/frame (CPU is sampled)
				uint64_t acc[REI_PROF_COUNT];
				const int np = REI_PROF_COUNT; // (sums collected per frame — slow frame log above)
				memcpy(acc, g_prof60, sizeof(acc));
				memset(g_prof60, 0, sizeof(g_prof60));
				char line[256];
				int len = 0;
				for (int i = 0; i < np && len < (int)sizeof(line) - 24; i++)
					if (acc[i] && (i == REI_PROF_FRAME || i >= REI_PROF_Z80 || i == REI_PROF_XFER || i == REI_PROF_LAYERS || i == REI_PROF_SPRITES || i == REI_PROF_FAST || i == REI_PROF_V33 || i == REI_PROF_V35 || i == REI_PROF_YM || i == REI_PROF_SOUND))
						len += snprintf(line + len, sizeof(line) - len, " %s %.1f", rei_core_prof_name(i), acc[i] / 60000.0);
				REI_LOG("  prof ms/frame:%s\n", line);
				extern unsigned long long rei_z80_drc_stats[6];
				const unsigned long long *z = rei_z80_drc_stats;
				REI_LOG("  z80 drc: runs %llu blocks %llu insns %llu flushes %llu interp %llu ram %llu\n", z[0], z[1], z[2], z[3], z[4], z[5]);
				{
					extern unsigned long long rei_z80_n; // (Z80 instructions run by the interpreter /60f)
					static unsigned long long lz;
					REI_LOG("  z80 insns /60f: %llu\n", rei_z80_n - lz);
					lz = rei_z80_n;
				}
				{ // NEC dynarec over 60 frames (block runs, block instrs, interpreted instrs, slow-path handler calls, run slices)
					extern unsigned long long rei_drc_stats[13];
					static unsigned long long last[6];
					const unsigned long long *s = rei_drc_stats;
					REI_LOG("  nec drc /60f: runs %llu insns %llu interp %llu slow %llu slices %llu compiled %llu flushes %llu\n", s[0] - last[0],
						s[1] - last[1], s[2] - last[2], s[12] - last[3], acc[REI_PROF_HOOK] / 1000, s[3] - last[4], s[4] - last[5]);
					last[0] = s[0]; last[1] = s[1]; last[2] = s[2]; last[3] = s[12]; last[4] = s[3]; last[5] = s[4];
				}
#ifdef REI_SFCOUNT
				sf_report();
#endif
			}
#endif
		}
	}
	g_loop_left = 1;
	power_lock(0);
	const uint64_t t_left = now_us();
	REI_LOG("reirem: exit: other loop left\n");
#ifdef REI_LOG_ENABLED
	if (g_mclk_th >= 0) {
		g_mclk_quit = 1;
		sceKernelWaitThreadEnd(g_mclk_th, NULL);
		sceKernelDeleteThread(g_mclk_th);
		g_mclk_th = -1;
	}
#endif
#ifdef REI_LOG_ENABLED
	g_wd_done = 1;
#endif
	g_audio_quit = 1;
	if (ath >= 0) {
		SceUInt timeout = 500000;
		if (sceKernelWaitThreadEnd(ath, &timeout) < 0) {
			sceKernelTerminateDeleteThread(ath);
			sceAudioSRCChRelease();
		} else {
			sceKernelDeleteThread(ath);
		}
	}
	REI_LOG("reirem: exit: audio thread down\n");
	video_sync();
	REI_LOG("reirem: exit: GE idle\n");
#ifdef REI_PSP_ME
	if (g_me_render_on) {
		const int done = me_sound_stop();
		REI_LOG("reirem: exit: ME render stop %d (frames %u)\n", done, (unsigned)me_render_frames());
		(void)done;
	}
#endif
	video_set_game_buffer(NULL, 0, 0, 0, 0);
	g_playing = 0;
	REI_LOG("reirem: exit: video down\n");
	rei_core_exit();
	REI_LOG("reirem: exit: core down\n");
#ifdef REI_PSP_ME
	if (g_me_render_on) { // (halted in our loop: back to firmware only when leaving the app, as in the M92 path — me_release)
		g_me_ours = 1;
		g_me_render_on = 0;
	}
#endif
	if (gem) {
		rei_m107_set_ge(NULL, 0);
		rei_m72_set_ge(NULL, 0);
	}
	video_layers_free();
	for (int i = 0; i < 3; i++) {
		free(mfb[i]);
		free(mclut[i]);
	}
	free(fbs[0]);
	free(fbs[1]);
	free(audio);
	REI_LOG("reirem: exit: other board down, dev_stop %d (%u ms)\n", dev_stop, (unsigned)((now_us() - t_left) / 1000));
	return dev_stop ? REI_RUN_DEV_STOP : REI_RUN_DONE;
}

// M92 path: the game with sound on the ME (core init to teardown). REI_RUN_* (main loop: back to the list, exit, dev stop)
static int run_m92(const rei_game_found *game, const char *set_name)
{
	int dev_stop = 0;
	m92_state_reset();
	rei_core_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.zip_path = game->zip;
	cfg.sample_rate = AUDIO_RATE;
	cfg.bytes_per_pixel = 4;
	cfg.color = color_5551; // palette in GE CLUT (5551) format
	cfg.log = core_log;
#ifdef REI_HWPROF
	cfg.clock_us = hw_counter;
	REI_LOG("reirem: profiler clock = hw counter %d (log ms = 1000 events)\n", REI_HWPROF);
#else
	cfg.clock_us = now_us;
#endif
	cfg.pipelined = 1; // single-thread pipeline (same hash as harness --mode pipelined)
#if defined(REI_PSP_ME) && defined(REI_PSP_YM_MAIN)
	cfg.ym_main_channels = REI_PSP_YM_MAIN; // some YM2151 channels synthesized by the main CPU (same result, approved 2026-10-09)
#endif
	// Main line spans up to 64 lines: the sound side only sees lines after a span ends, so long spans make the ME and main wait on each other
	// (stage 6: no limit 16.0 -> 64 lines 13.7ms, better than 32/48/96/128. Same result)
	rei_span_max_lines = REI_PSP_SPAN_MAX;
	cfg.s2m_delay_lines = REI_PSP_S2M_DELAY; // sound->main 128 lines: the two CPUs are less coupled (PSP-ADR-005, same hash as harness --s2m-delay 128)
#ifdef REI_PSP_ME
	cfg.threaded = 1; // sound side on the Media Engine (same hash as harness --threads, deterministic protocol)
#endif
	cfg.index_out = 1; // color conversion on the GE
#ifndef REI_PSP_G1
	cfg.ge_layers = 1; // tile layers on the GE too (psp/docs/M92_VIDEO.md G2)
	cfg.layer_cb = ge_layers_segment;
#endif
#if REI_PSP_DEFER
	cfg.render_threaded = 1; // drawing done in pieces in wait_hook and at frame end (rei_core_render_step)
	cfg.wait_hook = wait_hook;
#ifndef REI_PSP_G2_SPRITES
	cfg.ge_sprites = 1; // G3: sprites on the GE too (psp/docs/M92_VIDEO.md)
	g_ge_sprites = 1;
#endif
#endif
	cfg.set_name = set_name;
	cfg.parent_zip_path = game->parent[0] ? game->parent : NULL;
	cfg.progress = load_progress;
#if defined(REI_DRC) && defined(REI_LOG_ENABLED)
	{ // (diag) dev loop: disable V33 dynarec if host0:/rei_nodrc exists (M92 path)
		extern int rei_drc_enabled;
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_nodrc", &st) >= 0) {
			rei_drc_enabled = 0;
			REI_LOG("reirem: dynarec off\n");
		}
	}
#endif

	uint64_t t0 = now_us();
#ifdef REI_PSP_ME
	me_sound_trace("core init", 0);
#endif
	if (core_init_threaded(&cfg) != 0) {
		REI_LOG("reirem: init failed: %s\n", rei_core_error());
		rei_message_screen("Init failed", rei_core_error());
		g_to_list = 1; // (the core cleaned up after itself: back to the list)
		return REI_RUN_DONE;
	}
	REI_LOG("reirem: init %s in %u ms, free %u KB, max block %u KB\n", rei_core_game_name(),
		(unsigned)((now_us() - t0) / 1000), (unsigned)(sceKernelTotalFreeMemSize() / 1024),
		(unsigned)(sceKernelMaxFreeMemSize() / 1024));
	rei_loading_progress(920);
	{ // user cheat file <app folder>cheats/<set>.ini (FBNeo format, dev loop: ROM folder host1:)
		char cd[112];
		snprintf(cd, sizeof(cd), "%scheats/", strncmp(g_app_dir, "host", 4) == 0 ? "host1:/" : g_app_dir);
		REI_LOG("reirem: cheats %d (%s)\n", rei_core_cheats_load(cd), cd);
	}
	if (g_settings.dip_count > 0)
		rei_core_dip_set_raw(g_settings.dip, g_settings.dip_count);
	rei_core_set_start_stage(g_settings.start_stage);
	rei_core_set_infinite_health(g_settings.cheat_health);
#ifdef REI_LOG_ENABLED
	{ // dev loop: if host0:/rei_stage holds a number (0 Seattle ~ 6 New York), start at that stage + infinite health (for measurement)
		SceUID fd = sceIoOpen("host0:/rei_stage", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[8] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			if (b[0] >= '0' && b[0] <= '6') {
				rei_core_set_start_stage(b[0] - '0');
				rei_core_set_infinite_health(1);
				REI_LOG("reirem: start stage %d, infinite health (host0:/rei_stage)\n", b[0] - '0');
			}
		}
		// host0:/rei_char: character 1~4 for autoplay to pick (default input = 4th if absent)
		fd = sceIoOpen("host0:/rei_char", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[8] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			rei_core_set_autoplay_char(b[0] - '0');
			REI_LOG("reirem: autoplay character %d (host0:/rei_char)\n", b[0] - '0');
		}
		// host0:/rei_spanmax: override main line span limit (for measurement, default REI_PSP_SPAN_MAX)
		fd = sceIoOpen("host0:/rei_spanmax", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[16] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			rei_span_max_lines = (int)strtol(b, NULL, 10);
			REI_LOG("reirem: span max %d lines\n", rei_span_max_lines);
		}
		// If host0:/rei_verify holds "A B", also compare the GE screen with the CPU reference every frame in [A, B) (accuracy check)
		fd = sceIoOpen("host0:/rei_verify", PSP_O_RDONLY, 0);
		if (fd >= 0) {
			char b[32] = { 0 };
			sceIoRead(fd, b, sizeof(b) - 1);
			sceIoClose(fd);
			char *e;
			g_xverify_from = (int)strtol(b, &e, 10);
			g_xverify_to = (int)strtol(e, NULL, 10);
			REI_LOG("reirem: GE verify frames %d..%d (host0:/rei_verify)\n", g_xverify_from, g_xverify_to);
		}
		pad_script_load();
		char path[128];
		int frame;
		if (read_name_frame("host0:/rei_load", path, sizeof(path), &frame) == 0) {
			const int r = rei_core_state_load(path);
			REI_LOG("reirem: state %s (frame %d): %s\n", path, frame, r ? "load failed" : "loaded");
			if (!r)
				g_af_offset = frame;
		}
		if (read_name_frame("host0:/rei_save", g_save_path, sizeof(g_save_path), &g_save_at) == 0)
			REI_LOG("reirem: will save %s at frame %d\n", g_save_path, g_save_at);
	}
#endif
	{
		// Hot V33 code pages (read-only) go to the main CPU scratchpad (16KB, no cache misses) — interpreter and fast-path
		// instruction fetches use less data cache. Same result (harness --page-cache)
		int n;
		const unsigned int *pages = rei_core_v33_hot_pages(&n);
		const int m = rei_core_v33_page_cache((unsigned char *)0x00010000, 16384, pages, n);
		REI_LOG("reirem: %d V33 code pages in the scratchpad\n", m);
	}

	int alen = rei_core_audio_len();
#ifdef REI_PSP_ME
	g_frame_len = alen;
#endif
	uint32_t *rgb = (uint32_t *)memalign(64, REI_SCREEN_W * REI_SCREEN_H * 4);
	int16_t *audio = (int16_t *)memalign(64, ((size_t)alen * 2 + 16) * sizeof(int16_t));
#ifndef REI_PSP_G1
	{
		rei_tile_info ti;
		rei_core_tile_info(&ti);
		t0 = now_us();
		if (ge_layers_init(&ti) != 0 || video_layers_init(&ti) != 0) {
			REI_LOG("reirem: G2 init failed (memory)\n");
			video_layers_free();
			free(rgb);
			free(audio);
			rei_core_exit();
			rei_message_screen("Init failed", "Out of memory");
			g_to_list = 1;
			return REI_RUN_DONE;
		}
		rei_loading_progress(960);
		REI_LOG("reirem: tile atlas %d tiles in %u ms, free %u KB, heap free %u KB\n", ti.tile_mask + 1,
			(unsigned)((now_us() - t0) / 1000), (unsigned)(sceKernelTotalFreeMemSize() / 1024),
			(unsigned)(mallinfo().fordblks / 1024));
		{ // diag: large buffer locations (memory layout comparison per launch method)
			void *probe = malloc(64);
			g_addr_tiles = (unsigned)(uintptr_t)ti.tiles;
			g_addr_atlas = (unsigned)(uintptr_t)video_atlas_addr();
			g_addr_heap = (unsigned)(uintptr_t)probe;
			g_addr_code = (unsigned)(uintptr_t)&run_m92;
			free(probe);
		}
	}
#endif
	REI_LOG("reirem: video_init done\n");
#ifdef REI_PSP_ME
	SceUID ath = -1;
	{
#ifdef REI_PROFILE
		g_clock_thread = sceKernelCreateThread("clock", clock_thread, 0x10, 0x1000, 0, NULL);
		if (g_clock_thread >= 0)
			sceKernelStartThread(g_clock_thread, 0, NULL);
#endif
#if defined(REI_PSP_OC_MHZ) && REI_PSP_OC_MHZ > 333
		REI_LOG("reirem: overclock to %d MHz...\n", REI_PSP_OC_MHZ);
		sceKernelDelayThread(200000); // let the log go out first
		overclock_set(REI_PSP_OC_MHZ); // before starting the ME (psp/src/overclock.c)
		sceKernelDelayThread(200000);
#endif
#ifdef REI_LOG_ENABLED
		{ // diag: main compute, memory and ME compute speed per launch method (PSPLink / home menu)
			uint64_t b0 = now_us();
			volatile uint32_t sink = me_bench_loop(20000000u);
			uint64_t b1 = now_us();
			uint32_t *mb = (uint32_t *)malloc(4 << 20);
			uint32_t acc = 0;
			uint64_t b2 = now_us(), b3 = b2;
			if (mb) {
				memset(mb, 1, 4 << 20);
				b2 = now_us();
				for (int pass = 0; pass < 4; pass++)
					for (int i = 0; i < (1 << 20); i += 16)
						acc += mb[i];
				b3 = now_us();
				sceKernelDcacheWritebackInvalidateAll();
				const volatile uint32_t *u = (const volatile uint32_t *)((uintptr_t)mb | 0x40000000u);
				const uint64_t b4 = now_us();
				for (int i = 0; i < (1 << 18); i += 16)
					acc += u[i];
				g_bench_mu = (unsigned)(now_us() - b4);
				free(mb);
			}
			(void)sink;
			g_bench_cpu = (unsigned)(b1 - b0);
			g_bench_mem = (unsigned)(b3 - b2);
			REI_LOG("reirem: bench cpu %u us, mem %u us (acc %u)\n", (unsigned)(b1 - b0), (unsigned)(b3 - b2), (unsigned)acc);
			me_sound_set_bench(1);
		}
		uint64_t mb0 = now_us();
#endif
		g_game_loop = 1; // from here, main handles cleanup on HOME exit (the ME must be returned to firmware)
		power_lock(1);   // (sleep only after main takes down the ME)
		{ // The ME's sound V35 does not use the auto-idle counter rule (record next iteration) — the recording cost made the sound side lag and main
		  // frames spike (nbbatman, after 1d35beb). Same result (before starting the ME, flushed to memory)
			extern int rei_v25_counter_rule;
			rei_v25_counter_rule = 0;
		}
#ifdef REI_DRC
		{ // The ME's sound V35 also uses dynarec (nbbatman main waited on the ME's V35 interpreter, keeping frames at the limit, PERF PP43).
		  // On by default (play-tested 2026-10-17) — disable: app folder nom92drc35, dev host0:/rei_nom92drc35
			drc35_main_hooks();
			SceIoStat dst;
			char dp[128];
			snprintf(dp, sizeof(dp), "%snom92drc35", g_app_dir);
			int want = sceIoGetstat(dp, &dst) < 0;
#ifdef REI_LOG_ENABLED
			if (sceIoGetstat("host0:/rei_nom92drc35", &dst) >= 0)
				want = 0;
#endif
			const int on = want && drc35_to_me();
			REI_LOG("reirem: V35 dynarec on ME %d (want %d)\n", on, want);
			(void)on;
		}
#endif
		int r = me_sound_start(alen);
#ifdef REI_LOG_ENABLED
		(void)mb0;
		{
			unsigned br[3];
			me_sound_bench_results(br);
			g_bench_me = br[0];
			g_bench_mec = br[1];
			g_bench_meu = br[2];
		}
		REI_LOG("reirem: bench me %u, me cached read %u, me uncached read %u, main uncached read %u us\n", g_bench_me,
			g_bench_mec, g_bench_meu, g_bench_mu);
		me_sound_set_bench(0);
#endif
		REI_LOG("reirem: media engine start %d\n", r);
		{
			int mode = -1;
			const int found = me_sound_boot_info(&mode);
			REI_LOG("reirem: ME firmware reboot %s, vector %s, MeRpc %s (mode %d)\n", (found & 1) ? "found" : "not found",
				(found & 2) ? "saved" : "not saved", (found & 4) ? "passed" : "not found", mode);
			(void)found;
		}
		if (r < 0) {
			static char body[48];
			snprintf(body, sizeof(body), "error %d", r);
			rei_message_screen("Media Engine start failed", body);
			sceKernelExitGame();
			return 0;
		}
		ath = sceKernelCreateThread("audio", audio_thread, 0x12, 0x4000, 0, NULL);
		if (ath >= 0)
			sceKernelStartThread(ath, 0, NULL);
	}
#endif

	rei_loading_progress(1000);
	rei_loading_done();
#ifdef REI_PSP_ME
	me_sound_trace("loading done", 0);
#endif
	uint64_t hv = FNV_OFFSET, ha = FNV_OFFSET;
	uint64_t acc_emu = 0, max_emu = 0, acc_loop = 0, max_loop = 0, acc_sub = 0, max_sub = 0, acc_wait = 0, max_wait = 0, acc_work = 0;
	int verify_bad = 0;
	uint64_t t_loop = now_us();
	g_game_loop = 1; // (ME builds already set this right before ME start)
	power_lock(1);
	// (input mode, menu and pacing state of this game: plain locals so the next game in the process starts fresh)
#if defined(REI_PSP_PLAYER) || !defined(REI_LOG_ENABLED)
	int g_manual = 1;
#else
	int g_manual = 0;
#endif
	int menu_open = 0, vc_started = 0, hud_n = 0;
	uint32_t prev = 0, vc_target = 0;
	uint64_t hud_acc = 0;
	g_playing = 1;
	g_loop_left = 0;
	for (int f = 0; !g_quit; f++) {
#ifdef REI_LOG_ENABLED
		g_wd_frame = f;
		if (f == g_save_at) { // frame boundary: when the sound side has finished up to this frame and is idle (max 2s, skipped otherwise)
			// (wait_hook can block long running draw pieces: only handle main's share of YM2151 and idle)
			const uint64_t t_end = now_us() + 2000000;
			while (!rei_core_pipe_synced() && now_us() < t_end) {
				rei_core_main_poll();
				sceKernelDelayThread(100);
			}
			if (rei_core_pipe_synced())
				REI_LOG("reirem: state save %s at frame %d: %d\n", g_save_path, f + g_af_offset,
					rei_core_state_save(g_save_path));
			else
				REI_LOG("reirem: state save skipped (sound side not at the frame boundary)\n");
		}
#endif
#ifdef REI_LOG_ENABLED
		g_wd_beat++;
#endif
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		// Input: log builds autoplay while no button is pressed (same deterministic input as harness --autoplay, for measurement/verification),
		// and switch to the real pad after any press. Play builds use the pad from the start. Exit via HOME or options screen Exit
#ifdef REI_LOG_ENABLED
		pad.Buttons |= pad_script(g_wd_beat) | rei_test_press();
#endif
		const uint32_t btn = pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_SQUARE |
			PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE | PSP_CTRL_START | PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER |
			PSP_CTRL_RTRIGGER);
		const int ax = (int)pad.Lx - 128, ay = (int)pad.Ly - 128;
		if (!g_manual && (btn || ax < -64 || ax > 64 || ay < -64 || ay > 64)) {
			g_manual = 1;
			REI_LOG("reirem: pad input from f%d\n", f);
		}
		{ // menu/system combos use the raw pad (stick too for directions)
			uint32_t mb = btn;
			if (ay < -STICK_DEAD) mb |= PSP_CTRL_UP;
			if (ay > STICK_DEAD) mb |= PSP_CTRL_DOWN;
			if (ax < -STICK_DEAD) mb |= PSP_CTRL_LEFT;
			if (ax > STICK_DEAD) mb |= PSP_CTRL_RIGHT;
			sleep_test_poll(f);
			if (g_sleep_req) { // (power switch: sleep)
				finish_frames(f);
				sleep_m92();
			}
			// Options screen (SELECT+START): the game pauses while open and the menu is drawn over the last frame
			if (!menu_open && rei_menu_should_open(mb)) {
				menu_open = 1;
				finish_frames(f);
				rei_menu_set_open(1);
			}
			if (menu_open) {
				const rei_menu_result r = rei_menu_update(mb);
				if (r == REI_MENU_EXIT)
					g_quit = 1;
				if (r == REI_MENU_SELECT_GAME) { // back to the game list in this process (teardown below)
					g_to_list = 1;
					g_quit = 1;
				}
				if (r == REI_MENU_RESET)
					reset_game();
				if (r == REI_MENU_SAVE)
					save_state(rei_menu_state_slot());
				if (r == REI_MENU_LOAD)
					load_state(rei_menu_state_slot());
				if (r == REI_MENU_CLOSE || r == REI_MENU_RESET || r == REI_MENU_LOAD || r == REI_MENU_EXIT ||
					r == REI_MENU_SELECT_GAME) {
					menu_open = 0;
					rei_menu_set_open(0);
				}
				video_present();
				sceDisplayWaitVblankStart();
				f--; // game frame unchanged
				continue;
			}
			// Quick save SELECT+L, quick load SELECT+R (once on press)
			const uint32_t qs = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER, ql = PSP_CTRL_SELECT | PSP_CTRL_RTRIGGER;
			if ((mb & qs) == qs && (prev & qs) != qs) {
				finish_frames(f);
				save_state(REI_STATE_QUICK);
			}
			if ((mb & ql) == ql && (prev & ql) != ql) {
				finish_frames(f);
				load_state(REI_STATE_QUICK);
			}
			prev = mb;
		}
		if (g_manual)
			set_game_input(btn, ax, ay);
		else
			rei_core_autoplay(f + g_af_offset);
		memset(audio, 0, (size_t)alen * 2 * sizeof(int16_t));
		const int rf = f - REI_PSP_DEFER; // frame drawn and submitted this iteration
		int check = rf >= 0 && rf < VERIFY_FRAMES;
		int xcheck = rf >= g_xverify_from && rf < g_xverify_to;
#ifndef REI_PSP_G1
		rei_core_set_ge_reference(rf < VERIFY_FRAMES || xcheck); // during verification also draw the CPU reference screen for hash/compare
#if !REI_PSP_DEFER
		ge_layers_begin();
#endif
#endif
		uint64_t t = now_us();
#if REI_PSP_DEFER
		g_cur_frame = f;
#endif
#ifdef REI_PSP_ME
		rei_core_run_frame(NULL, 0, NULL); // sound side is on the ME (audio unused)
#else
		rei_core_run_frame(NULL, 0, audio);
#endif
		const uint64_t t_run = now_us() - t; // (diag: split slow frames into emu / previous-frame draw pieces)
		(void)t_run;
#if REI_PSP_DEFER
		while (g_submitted < f) { // remaining pieces of the previous frame (this frame is drawn and sent during the next)
			rei_core_main_poll();
			defer_piece();
		}

#endif
		uint64_t dt = now_us() - t;
		acc_emu += dt;
		if (dt > max_emu)
			max_emu = dt;
#if defined(REI_LOG_ENABLED)
		{ // (diag) slow frame: dynarec activity in that frame
#ifndef REI_DRC
			static unsigned long long rei_drc_stats[13]; // (interpreter builds: always 0)
#endif
			static unsigned long long p[13];
#ifdef REI_PROFILE
			static uint64_t pp[REI_PROF_COUNT];
			uint64_t pn[REI_PROF_COUNT];
			for (int i = 0; i < REI_PROF_COUNT; i++)
				pn[i] = REI_PROF_ACC[i];
			if (dt > 30000)
				REI_LOG("slow f%d: emu %u ms (run %u), v33 %u mline %u swait %u ms, drc runs +%llu, interp +%llu, blocks +%llu (%llu insns, %llu us), flushes +%llu, hcall +%llu, sync %llu us\n",
					f, (unsigned)(dt / 1000), (unsigned)(t_run / 1000), (unsigned)((pn[REI_PROF_V33] - pp[REI_PROF_V33]) / 1000),
					(unsigned)((pn[REI_PROF_MLINE] - pp[REI_PROF_MLINE]) / 1000), (unsigned)((pn[REI_PROF_SWAIT] - pp[REI_PROF_SWAIT]) / 1000),
					rei_drc_stats[0] - p[0], rei_drc_stats[2] - p[2], rei_drc_stats[3] - p[3],
					rei_drc_stats[7] - p[7], rei_drc_stats[10] - p[10], rei_drc_stats[4] - p[4], rei_drc_stats[12] - p[12],
					rei_drc_stats[11] - p[11]);
			memcpy(pp, pn, sizeof(pp));
#endif
			memcpy(p, rei_drc_stats, sizeof(p));
		}
#endif

		const uint32_t *pal = NULL;
		int blank = 0;
		const uint16_t *index = rei_core_index_frame(&pal, &blank);
#if !REI_PSP_DEFER
		// Check the previous frame's drawing (done by the GE during emulation) is finished, show it, then send the next
		t = now_us();
		video_sync();
		uint64_t ws = now_us() - t;
		acc_wait += ws;
		if (ws > max_wait)
			max_wait = ws;
		if (g_submitted)
			video_flip();
		t = now_us();
		if (index) {
			if (!g_submitted)
				REI_LOG("reirem: first submit f%d index %p pal %p blank %d\n", f, (const void *)index,
					(const void *)pal, blank);
#ifdef REI_PSP_G1
			video_submit(index, pal, blank);
#else
			const ge_frame *fr = ge_layers_end();
			g_tiles_acc += fr->ntiles;
			if (fr->ntiles > g_tiles_max)
				g_tiles_max = fr->ntiles;
			if (video_submit_layers(fr, rei_core_sprite_frame(), pal, blank) && !g_overflow_logged++)
				REI_LOG("reirem: GE buffers overflowed at f%d (tiles %d)\n", f, fr->ntiles);
#endif
			if (!g_submitted) {
				video_sync();
				REI_LOG("reirem: first GE frame done\n");
			}
			g_submitted = 1;
		}
		dt = now_us() - t;
		acc_sub += dt;
		if (dt > max_sub)
			max_sub = dt;
#endif

		if (check) {
			if (index)
				to_rgb(rgb, index, pal, blank);
			if (rf == g_save_at) // save check: restart counting from the saved frame (compare with the hash of the loaded run)
				hv = FNV_OFFSET;
			hv = fnv1a(hv, rgb, REI_SCREEN_W * REI_SCREEN_H * 4);
#ifdef REI_PSP_ME
			ha = g_audio_hash; // computed by the audio thread from ME frames (may not align with frame boundaries — compared at 600)
#else
			ha = fnv1a(ha, audio, (size_t)alen * 2 * sizeof(int16_t));
#endif
			if (index && (rf + 1) % HASH_EVERY == 0) {
#if REI_PSP_DEFER
				REI_LOG("frame=%d video=%016llx audio=%016llx (drawn)\n", rf + 1, (unsigned long long)hv,
					(unsigned long long)ha);
#endif
				char why[192] = "";
				video_sync();
				int d = video_verify(index, pal, blank, why, sizeof(why));
				if (d) {
					verify_bad++;
					REI_LOG("  GE diff %d px (%s)\n", d, why);
#ifdef REI_LOG_ENABLED
					if (verify_bad == 1) // first mismatching frame: send GE screen, reference indices and palette to the PC (psp/tools/ge_diff.py)
						video_dump("host0:/ge_dump.bin", index, pal);
#endif
				}
			}
		}

		if (xcheck && index) {
			char why[192] = "";
			video_sync();
			int d = video_verify(index, pal, blank, why, sizeof(why));
			if (d) {
				if (++g_xverify_bad <= 20)
					REI_LOG("  f%d GE diff %d px (%s)\n", rf, d, why);
#ifdef REI_LOG_ENABLED
				if (g_xverify_bad == 1 && !verify_bad)
					video_dump("host0:/ge_dump.bin", index, pal);
#endif
			}
			if (rf + 1 == g_xverify_to)
				REI_LOG("reirem: GE verify %d..%d: %d frames differ\n", g_xverify_from, g_xverify_to, g_xverify_bad);
		}

		{ // Frame pacing (adaptive vsync): assign each frame a vblank slot to show on; if it finishes earlier,
		  // wait until that vblank (sceGuSwapBuffers flips on the next vblank). If late, don't wait and move to the next frame
		  // to catch up; if more than two frames behind, reset the slot. (Before: a 16.683ms timer — drifted slightly from the display period
		  // and occasionally showed a frame twice or skipped one)
			acc_work += now_us() - t_loop;
			const uint32_t vc = (uint32_t)sceDisplayGetVcount();
			if (!vc_started || (int32_t)(vc - vc_target) > 2) {
				vc_target = vc;
				vc_started = 1;
			}
			while ((int32_t)(vc_target - (uint32_t)sceDisplayGetVcount()) > 0 && !g_quit)
				sceDisplayWaitVblankStart();
			vc_target++;
		}
		uint64_t now = now_us();
		dt = now - t_loop;
		t_loop = now;
		acc_loop += dt;
		if (dt > max_loop)
			max_loop = dt;
		{ // screen HUD: 30-frame average frame time and FPS, frame number (left screen margin, to pin slow scenes to frames)
			hud_acc += dt;
			if (++hud_n == 30) {
				unsigned us = (unsigned)(hud_acc / 30);
				unsigned fps10 = us ? (unsigned)(10000000u / us) : 0;
				rei_fps_set(fps10 / 10.0f); // options screen Show FPS
#ifdef REI_LOG_ENABLED
				char s[48]; // dev: frame time and frame number
				snprintf(s, sizeof(s), "FPS %u.%u\nMS  %u.%u\nFR  %d", fps10 / 10, fps10 % 10, us / 1000, us / 100 % 10,
					 f + 1 + g_af_offset);
				video_set_hud(s);
#endif
				hud_acc = 0;
				hud_n = 0;
			}
		}

		if ((f + 1) % HASH_EVERY == 0) {
			scePowerTick(PSP_POWER_TICK_ALL); // don't go to power save (screen off) while running without input
			// When launched from the home menu, CWCheat (loaded alongside) drops the clock to its own setting a few seconds in (dev unit: 266/111), making the same scene
			// 1.5~1.8x slower (device log cpu 265 bus 110). Raise it again when lowered (MeRpc passes through even with our code on the ME, so no hang)
			if (scePowerGetCpuClockFrequency() < 333 || scePowerGetBusClockFrequency() < 166) {
				REI_LOG("reirem: clock lowered to %d/%d by someone, back to 333/166 (frame %d)\n", scePowerGetCpuClockFrequency(),
					scePowerGetBusClockFrequency(), f + 1);
				scePowerSetClockFrequency(333, 333, 166);
			}
			if ((f + 1) % (HASH_EVERY * 10) == 0) { // diag: log power state since the device has powered off mid-run
				REI_LOG("  bench: cpu %u mem %u me %u me-cached %u me-uncached %u main-uncached %u us\n", g_bench_cpu, g_bench_mem,
					g_bench_me, g_bench_mec, g_bench_meu, g_bench_mu);
				REI_LOG("  addr: code %08x tiles %08x atlas %08x heap %08x\n", g_addr_code, g_addr_tiles, g_addr_atlas, g_addr_heap);
				REI_LOG("  power: cpu %d bus %d MHz, battery %d%% %s, temp %d C, %d mV\n", scePowerGetCpuClockFrequency(),
					scePowerGetBusClockFrequency(), scePowerGetBatteryLifePercent(),
					scePowerIsPowerOnline() ? "AC" : "no AC", scePowerGetBatteryTemp(), scePowerGetBatteryVolt());
#if defined(REI_LOG_ENABLED) && defined(REI_PSP_ME)
				// diag: same compute/memory read timing in-game (tells whether home-menu-only slowness is external interference)
				if (g_addr_tiles) {
					const SceInt64 l0 = sceKernelGetSystemTimeWide();
					volatile uint32_t sink = me_bench_loop(200000u);
					const SceInt64 l1 = sceKernelGetSystemTimeWide();
					const volatile uint32_t *mp = (const volatile uint32_t *)g_addr_tiles;
					uint32_t acc = 0;
					for (int i = 0; i < (1 << 18); i += 16) // 1MB, every 64 bytes
						acc += mp[i];
					const SceInt64 l2 = sceKernelGetSystemTimeWide();
					const volatile uint32_t *up = (const volatile uint32_t *)(g_addr_tiles | 0x40000000u);
					for (int i = 0; i < (1 << 14); i += 16) // 64KB uncached
						acc += up[i];
					const SceInt64 l3 = sceKernelGetSystemTimeWide();
					sink += acc;
					(void)sink;
					REI_LOG("  live bench: cpu %u mem %u uncached %u us\n", (unsigned)(l1 - l0), (unsigned)(l2 - l1),
						(unsigned)(l3 - l2));
				}
#endif
			}
			if (check)
				REI_LOG("frame=%d video=%016llx audio=%016llx GE %s\n", f + 1, (unsigned long long)hv,
					(unsigned long long)ha, verify_bad ? "DIFF" : "ok");
			else
				REI_LOG("frame=%d\n", f + 1);
#ifdef REI_PSP_ME
			{ // audio output blocks that played silence on empty (only when it grew)
				static int last_u;
				const int u = g_audio_underruns;
				if (u != last_u)
					REI_LOG("  audio underruns +%d\n", u - last_u);
				last_u = u;
			}
#endif
			log_ms("emu  ", acc_emu, max_emu);
			log_ms("gewait", acc_wait, max_wait);
			log_ms("submit", acc_sub, max_sub);
			REI_LOG("  tiles avg %ld max %ld\n", g_tiles_acc / HASH_EVERY, g_tiles_max);
			g_tiles_acc = g_tiles_max = 0;
			log_ms("loop ", acc_loop, max_loop);
			log_ms("work ", acc_work, 0); // time excluding waits (pacing)
			{ // waits because the previous flip missed the display (only when it grew, video_flip)
				extern unsigned g_flip_waits;
				static unsigned last_fw;
				if (g_flip_waits != last_fw)
					REI_LOG("  flip waits +%u\n", g_flip_waits - last_fw);
				last_fw = g_flip_waits;
			}
			acc_emu = max_emu = acc_sub = max_sub = acc_loop = max_loop = acc_wait = max_wait = acc_work = 0;
#ifdef REI_LOG_ENABLED
			// Dev loop (run.sh): when host0:/rei_stop appears, exit cleanly so the module unloads itself (heap returned)
			SceIoStat st;
			if (sceIoGetstat("host0:/rei_stop", &st) >= 0) {
				REI_LOG("reirem: stop requested\n");
				dev_stop = 1;
				break;
			}
			uint64_t prof[16];
			int n = rei_core_prof_take(prof, 16);
			if (n > 0) {
				char line[256];
				int len = 0;
				for (int i = 0; i < n && len < (int)sizeof(line) - 32; i++)
					len += snprintf(line + len, sizeof(line) - len, " %s %u.%u", rei_core_prof_name(i),
							(unsigned)(prof[i] / HASH_EVERY / 1000), (unsigned)(prof[i] / HASH_EVERY / 100 % 10));
				REI_LOG("  prof ms/frame:%s\n", line);
			}
#ifdef REI_DRC
			REI_LOG("  drc: runs %llu, interp %llu, blocks %llu (%llu insns, %llu ms, sync %llu ms), flushes %llu, bank %llu, code %llu KB, links %llu\n",
				rei_drc_stats[0], rei_drc_stats[2], rei_drc_stats[3], rei_drc_stats[7], rei_drc_stats[10] / 1000, rei_drc_stats[11] / 1000,
				rei_drc_stats[4], rei_drc_stats[5], rei_drc_stats[6] / 1024, rei_drc_stats[8]);
			{ // (diag) ME V35 dynarec: compile count, window decision (v25.cpp rei_drc35_me_diag 2)
				extern volatile uint32_t rei_drc35_me_diag[4];
				const volatile uint32_t *d = (const volatile uint32_t *)((uintptr_t)rei_drc35_me_diag | 0x40000000u);
				if (d[1])
					REI_LOG("  V35 on ME: compiles %u, on %u short %u windows %u, insns/block %u.%u\n", (unsigned)d[1], (unsigned)(d[2] & 15),
						(unsigned)(d[2] >> 4 & 15), (unsigned)(d[2] >> 8 & 0xff), (unsigned)(d[2] >> 16) / 10, (unsigned)(d[2] >> 16) % 10);
			}
#endif
#ifdef REI_PROFILE
			{ // time main waited for sound-side (ME) progress
				static uint64_t last_ws;
				uint64_t ws, wr;
				rei_core_waits(&ws, &wr);
				REI_LOG("  wait sound %u.%u ms/frame\n", (unsigned)((ws - last_ws) / HASH_EVERY / 1000),
					(unsigned)((ws - last_ws) / HASH_EVERY / 100 % 10));
				last_ws = ws;
				{ // V33 fast path: native slices per frame, cumulative learning/mismatches (a mismatch disables that path)
					static unsigned last_n;
					unsigned n, l, b;
					rei_core_task_stats(&n, &l, &b);
					REI_LOG("  fast V33: native %u/frame, learned %u, bad %u\n", (n - last_n) / HASH_EVERY, l, b);
					last_n = n;
				}
#if REI_PSP_DEFER
				REI_LOG("  pieces ms/frame: render %u.%u clut %u.%u sprcopy %u.%u list %u.%u\n",
					(unsigned)(g_piece_us[0] / HASH_EVERY / 1000), (unsigned)(g_piece_us[0] / HASH_EVERY / 100 % 10),
					(unsigned)(g_piece_us[1] / HASH_EVERY / 1000), (unsigned)(g_piece_us[1] / HASH_EVERY / 100 % 10),
					(unsigned)(g_piece_us[2] / HASH_EVERY / 1000), (unsigned)(g_piece_us[2] / HASH_EVERY / 100 % 10),
					(unsigned)(g_piece_us[3] / HASH_EVERY / 1000), (unsigned)(g_piece_us[3] / HASH_EVERY / 100 % 10));
				memset(g_piece_us, 0, sizeof(g_piece_us));
				{
					void video_prof_take(uint32_t *out);
					uint32_t vp[3];
					video_prof_take(vp);
					REI_LOG("  list: sprites %u.%u layers %u.%u ms/frame, batches %u/frame\n", (unsigned)(vp[0] / HASH_EVERY / 1000),
						(unsigned)(vp[0] / HASH_EVERY / 100 % 10), (unsigned)(vp[1] / HASH_EVERY / 1000), (unsigned)(vp[1] / HASH_EVERY / 100 % 10),
						(unsigned)(vp[2] / HASH_EVERY));
				}
#endif
			}
#endif
#endif
		}
	}

	g_loop_left = 1;
	power_lock(0);
#ifdef REI_LOG_ENABLED
	g_wd_done = 1;
#endif
	REI_LOG("reirem: exit\n");
	video_sync();
	REI_LOG("reirem: exit: GE idle\n");
#ifdef REI_PSP_ME
	{
		long long md, sd, sw;
		rei_core_pipe_lines(&md, &sd, &sw);
		const int done = me_sound_stop();
		REI_LOG("reirem: exit: ME stop %d (main line %lld, sound line %lld, sound waits at %lld, ME frames %u)\n", done,
			md, sd, sw, (unsigned)me_sound_frames());
		(void)done;
	}
#ifdef REI_PROFILE
	g_clock_quit = 1;
	if (g_clock_thread >= 0) { // make sure it has finished before unloading the module
		SceUInt to = 100000;
		if (sceKernelWaitThreadEnd(g_clock_thread, &to) < 0)
			sceKernelTerminateThread(g_clock_thread);
		sceKernelDeleteThread(g_clock_thread);
	}
#endif
	g_audio_quit = 1;
	REI_LOG("reirem: ME frames %u (dropped %u), audio frames hashed %d, underruns %d, audio hash %016llx\n",
		(unsigned)me_sound_frames(), (unsigned)me_sound_drops(), g_audio_hashed, g_audio_underruns,
		(unsigned long long)g_audio_hash);
	if (ath >= 0) {
		SceUInt timeout = 500000;
		if (sceKernelWaitThreadEnd(ath, &timeout) < 0) {
			sceKernelTerminateDeleteThread(ath);
			sceAudioSRCChRelease();
		} else {
			sceKernelDeleteThread(ath);
		}
	}
#endif
	REI_LOG("reirem: exit: audio stopped\n");
	g_playing = 0;
#ifndef REI_PSP_G1
	video_layers_free(); // (the GE is idle: nothing reads the tile atlas any more)
#endif
	REI_LOG("reirem: exit: video down\n");
	rei_core_exit();
	REI_LOG("reirem: exit: core down, dev_stop %d\n", dev_stop);
#ifdef REI_PSP_ME
	g_me_ours = 1; // (halted in our loop: back to firmware only when leaving the app — me_release)
#endif
	free(rgb);
	free(audio);
	return dev_stop ? REI_RUN_DEV_STOP : REI_RUN_DONE;
}

#if defined(REI_PSP_TESTGAME) && !defined(REI_LOG_ENABLED)
static int exit_callback(int arg1, int arg2, void *common);
static int testhome_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		sceKernelDelayThread(1000000);
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_home", &st) >= 0) {
			sceIoRemove("host0:/rei_home");
			exit_callback(0, 0, NULL);
			return 0;
		}
		select_poll();
	}
}
#endif
int main(int argc, char *argv[])
{
	set_app_dir(argc, argv);
#ifdef REI_PSP_ME
	me_trace_set_dir(g_app_dir); // (play builds: startup stages to app folder me_trace.txt)
	me_sound_trace("app start", 0);
#endif
	g_main_thid = sceKernelGetThreadId();
#ifdef REI_DRC
	rei_drc_cache_flush = drc_cache_flush; // (all boards: flush generated code from D cache and invalidate I cache. M107 hung without it)
	{ // clock for spare-time compiling (drc_idle_work)
		extern unsigned (*rei_drc_now_us)(void);
		rei_drc_now_us = (unsigned (*)(void))sceKernelGetSystemTimeLow;
		drc35_main_hooks(); // (V35 instances use the same function — switched for the ME in mode 3)
	}
#ifdef REI_LOG_ENABLED
	{ // (diag) dynarec compile time µs (slow frame log)
		extern unsigned (*rei_drc_clock32)(void), (*rei_drc35_clock32)(void);
		rei_drc_clock32 = (unsigned (*)(void))sceKernelGetSystemTimeLow;
		rei_drc35_clock32 = rei_drc_clock32;
		// Per-instruction stage times only with host0:/rei_drcfine (clock reads inflated compile time by ~4µs per instruction)
		extern unsigned (*rei_drc_clock_fine)(void);
		SceIoStat st;
		if (sceIoGetstat("host0:/rei_drcfine", &st) >= 0)
			rei_drc_clock_fine = (unsigned (*)(void))sceKernelGetSystemTimeLow;
		// M107 main/sound line times (d_m107.cpp, ~34 clock reads per frame); in-block V33 time only with host0:/rei_blkclk
		// (twice per dispatch — ~800 per frame)
		extern unsigned (*rei_m107_clock)(void), (*rei_drc_blk_clock)(void);
		rei_m107_clock = (unsigned (*)(void))sceKernelGetSystemTimeLow;
		const int blk = sceIoGetstat("host0:/rei_blkclk", &st) >= 0;
		extern int rei_drc_interp_stat; // (diag) dispatcher interpretation reasons (nec_drc_ops.inc drc_interp_note)
		if (sceIoGetstat("host0:/rei_interpstat", &st) >= 0)
			rei_drc_interp_stat = 1;
		if (blk)
			rei_drc_blk_clock = (unsigned (*)(void))sceKernelGetSystemTimeLow;
#if defined(REI_HWPROF) && !defined(REI_PROFILE)
		// (REI_HWPROF builds) hardware counter value in place of compile "µs" (13 i_miss: is the compiler outside the I cache). M107 spans and in-block use the same event count
		rei_drc_clock32 = drc_hw_counter32;
		rei_m107_clock = drc_hw_counter32;
		if (blk)
			rei_drc_blk_clock = drc_hw_counter32;
#endif
	}
#endif
#endif
	SceUID th = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);
#if defined(REI_PSP_TESTGAME) && !defined(REI_LOG_ENABLED)
	{ // (test builds) HOME exit check for no-log builds: when host0:/rei_home appears, run the same callback as HOME > Quit game
		const SceUID th = sceKernelCreateThread("testhome", testhome_thread, 0x0c, 0x1000, 0, NULL);
		if (th >= 0)
			sceKernelStartThread(th, 0, NULL);
	}
#endif
#ifdef REI_LOG_ENABLED
	{ // dev-loop watchdog (from startup)
		g_wd_thread = sceKernelCreateThread("watchdog", watchdog_thread, 0x0c, 0x1000, 0, NULL);
		if (g_wd_thread >= 0)
			sceKernelStartThread(g_wd_thread, 0, NULL);
	}
#endif
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	// Max CPU/bus clock (333MHz per psp/docs/RULES.md ch.1). Left alone if CFW already set it higher (ARK overclock)
	// (if another plugin lowers it mid-game, the main loop below raises it again)
	if (scePowerGetCpuClockFrequency() < 333 || scePowerGetBusClockFrequency() < 166)
		scePowerSetClockFrequency(333, 333, 166);
	REI_LOG("reirem: start, cpu %d MHz bus %d MHz\n", scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());

	// Settings and save states go in the app folder (PSPLink dev loop: ROM folder host1:)
	rei_settings_set_dir(strncmp(g_app_dir, "host", 4) == 0 ? "host1:/" : g_app_dir);
#ifdef REI_PSP_ME
	// ME reset vector copy mevec.bin (plus MeRpc address merpc.bin, image type metab.bin): read from hardware (0xbfc00000) only the first time,
	// then from file. Play builds too (2026-10-16): the device powered off on the hardware read at game start (play_check kengo/vigilant,
	// last me_trace entry "table file" = vector read in me_kernel_start_table, recurrence log 8 "start side"). The dev loop has used the same
	// copy since 2026-10-12 for 1,408 start/exit cycles (contents identical every boot). The reason it was disabled for play builds on 2026-10-12 (XMB return hang) was
	// later found to be recurrence log 7 (call inside a trace macro vanished). Disable: app folder nomevec
	if (strncmp(g_app_dir, "host", 4) == 0)
		me_sound_set_dir("host1:/");
	else {
		SceIoStat vst;
		char vp[160];
		snprintf(vp, sizeof(vp), "%snomevec", g_app_dir);
		if (sceIoGetstat(vp, &vst) < 0)
			me_sound_set_dir(g_app_dir);
	}
	{ // (off by default) pause the CWCheat thread during play — app folder cwstop (me_sound.c; off because a kernel call once powered off the device)
		extern int me_sound_cw_stop;
		SceIoStat cst;
		char cp[160];
		snprintf(cp, sizeof(cp), "%scwstop", strncmp(g_app_dir, "host", 4) == 0 ? "host0:/rei_" : g_app_dir);
		me_sound_cw_stop = sceIoGetstat(cp, &cst) >= 0;
	}
#endif
	rei_settings_load();
	// Bring up the display first (game list, notice screen). Tile graphics bundle after the game is chosen (video_layers_init)
	REI_LOG("reirem: video_init\n");
	video_init();
	ui_init(); // (system font for the menus)
	rei_game_select_set_poll(list_poll);
	video_set_overlay(rei_menu_overlay);

	// Game list -> game -> back to the list in the same process (Select game in the options), until exit (HOME, Exit, dev stop)
	int run = REI_RUN_DONE;
	for (int games = 0;; games++) {
		g_quit = g_home_exit; // (a HOME exit that came during the switch stays)
		g_to_list = 0;
		g_loop_left = 1; // (not in a frame loop: the HOME callback must not release main's waits)
#ifdef REI_LOG_ENABLED
		g_wd_frame = -1;
		g_wd_done = 1; // (the watchdog leaves the list alone)
		{
			const struct mallinfo mi = mallinfo();
			REI_LOG("reirem: list %d: system free %u KB max block %u KB, heap used %u KB\n", games, (unsigned)(sceKernelTotalFreeMemSize() / 1024),
				(unsigned)(sceKernelMaxFreeMemSize() / 1024), (unsigned)(mi.uordblks / 1024));
		}
#endif
#if defined(REI_PSP_TESTGAME) && !defined(REI_LOG_ENABLED)
		{ // (test builds) free memory at each return to the list, appended to host0:/rei_mem.txt (play_check SWITCH: must stay the same)
			const struct mallinfo mi = mallinfo();
			char line[96];
			const int len = snprintf(line, sizeof(line), "list %d free %u KB max %u KB heap used %u KB\n", games,
				(unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024), (unsigned)(mi.uordblks / 1024));
			const SceUID fd = sceIoOpen("host0:/rei_mem.txt", PSP_O_WRONLY | PSP_O_CREAT | (games ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
			if (fd >= 0) {
				sceIoWrite(fd, line, len);
				sceIoClose(fd);
			}
		}
#endif
		const rei_game_found *game = choose_game();
#ifdef REI_PSP_ME
		me_sound_trace(game ? rei_core_game_set(game->game) : "no game", 0);
#endif
		if (!game && g_list_stop) { // (dev loop: stop request while in the list)
			run = REI_RUN_DEV_STOP;
			break;
		}
		if (!game && g_quit) // (HOME exit while the list is up)
			break;
		if (!game) {
			static char body[640];
			int len = snprintf(body, sizeof(body), "Put ROM zip files in\n  %sroms/\n\nSupported sets:\n  ", g_app_dir);
			for (int i = 0; i < rei_core_game_count() && len < (int)sizeof(body) - 24; i++)
				len += snprintf(body + len, sizeof(body) - len, "%s.zip%s", rei_core_game_set(i), (i % 4 == 3) ? "\n  " : "  ");
			snprintf(body + len, sizeof(body) - len, "\n\nClone sets (nbbatmanu, hooku, ...) may be split:\nmissing files are read from the parent zip.");
			REI_LOG("reirem: rom not found\n");
			rei_message_screen("ROM not found", body);
			sceKernelExitGame();
			return 0;
		}
		const char *set_name = rei_core_game_set(game->game);
		REI_LOG("reirem: rom %s (set %s, parent %s)\n", game->zip, set_name, game->parent);
		{ // diag: heap (newlib used size/free space, memory left in the system)
			const struct mallinfo mi = mallinfo();
			REI_LOG("reirem: heap arena %u KB used %u KB free %u KB, system free %u KB max block %u KB\n", (unsigned)(mi.arena / 1024),
				(unsigned)(mi.uordblks / 1024), (unsigned)(mi.fordblks / 1024), (unsigned)(sceKernelTotalFreeMemSize() / 1024),
				(unsigned)(sceKernelMaxFreeMemSize() / 1024));
		}
		rei_settings_load_game(set_name);
		rei_settings_save(); // remember last game
		rei_loading_title(rei_core_game_title(game->game));
		rei_loading_progress(0);
		// Off-screen artwork (user artwork/<set>.png, parent set if missing)
		video_load_art(g_app_dir, set_name, rei_core_game_parent(game->game));
#ifdef REI_LOG_ENABLED
		g_wd_done = 0;
#endif
		if (strcmp(rei_core_game_board(game->game), "M92") != 0) // other boards (run_other_board: ME draws, main runs the CPUs)
			run = run_other_board(game, set_name);
		else
			run = run_m92(game, set_name);
		// (the game's threads are gone. With the ME still in our code, HOME and the power switch stay main's job in the list too)
		g_game_loop = g_me_ours;
		power_lock(g_me_ours);
#ifdef REI_LOG_ENABLED
		g_wd_done = 1;
#endif
		rei_loading_done();
		video_set_game_buffer(NULL, 0, 0, 0, 0);
		if (run != REI_RUN_DONE || !g_to_list || g_home_exit)
			break;
		REI_LOG("reirem: back to the game list\n");
	}
	me_release(); // (the ME back to firmware before leaving: exit to XMB or module unload)
	if (run == REI_RUN_DEV_STOP) {
		// Unload only the module (the REIREM DEV game stays). Returning from main makes the C runtime _exit
		// exit the whole game via sceKernelExitGame (to XMB) since sce_newlib_nocreate_thread_in_start is missing,
		// so return the heap directly and unload ourselves. No malloc/printf after this
		video_shutdown();
		if (th >= 0)
			sceKernelTerminateDeleteThread(th);
#ifdef REI_LOG_ENABLED
		// End the watchdog thread before the module too (if left, it woke from its 1s wait, ran unloaded module code and powered off the device)
		if (g_wd_thread >= 0)
			sceKernelTerminateDeleteThread(g_wd_thread);
#endif
		__psp_free_heap();
		sceKernelSelfStopUnloadModule(1, 0, NULL);
		return 0;
	}
	// (on exit the system restores default clocks, but with our code on the ME the clock change never completed and the power thread
	// waited holding the ScePowerClock mutex — the ME is returned to firmware above)
	video_shutdown();
	g_cleanup_done = 1;
#ifdef REI_PSP_ME
	me_sound_trace("exit: cleanup done, home", (int)g_home_exit);
#endif
	if (g_home_exit) { // HOME exit: the callback calls sceKernelExitGame
		REI_LOG("reirem: exit: HOME, cleanup done\n");
		sceKernelSleepThread();
	}
	REI_LOG("reirem: exit: sceKernelExitGame\n");
	sceKernelExitGame();
	return 0;
}
