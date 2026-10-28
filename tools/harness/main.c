// ReiREM PC test harness (headless)
//
// Runs N frames and prints video/audio hashes, for accuracy comparison before/after optimizations
// (docs/RULES.md section 6 rule 3). ROM path from --rom or env REI_ROM.
//
//   harness --rom nbbatman.zip --frames 3600 [--autoplay] [--checkpoint 600]
//           [--dump N:out.ppm] [--rate 48000] [--wav out.wav]
#if defined(REI_RENDER_WRITES) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // (diag: ucontext REG_EFL)
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "rei_core.h"
#include "rei_prof.h"

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int g_w = REI_SCREEN_W, g_h = REI_SCREEN_H; // game screen size (non-M92 boards: rei_core_screen)

static int write_ppm(const char *path, const uint32_t *fb)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
	for (int i = 0; i < g_w * g_h; i++) {
		uint8_t rgb[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}

static void wr32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_header(FILE *f, int rate, uint32_t data_bytes)
{
	fwrite("RIFF", 1, 4, f); wr32(f, 36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
	wr32(f, 16); wr16(f, 1); wr16(f, 2); wr32(f, rate); wr32(f, rate * 4); wr16(f, 4); wr16(f, 16);
	fwrite("data", 1, 4, f); wr32(f, data_bytes);
}

// --threads: run the sound side on a separate thread. Audio hash and WAV are produced by it in frame order
typedef struct {
	int frames, alen;
	FILE *wav;
	uint32_t wav_bytes;
	uint64_t ha;
} sound_job;

static int g_snd_done; // 1 when the sound thread is done

static void *sound_thread(void *arg)
{
	sound_job *j = (sound_job *)arg;
	int16_t *buf = (int16_t *)calloc((size_t)j->alen * 2 + 16, sizeof(int16_t));
	for (int f = 0; f < j->frames; f++) {
		memset(buf, 0, (size_t)j->alen * 2 * sizeof(int16_t));
		rei_core_sound_frame(buf);
		j->ha = fnv1a(j->ha, buf, (size_t)j->alen * 2 * sizeof(int16_t));
		if (j->wav) {
			fwrite(buf, sizeof(int16_t), (size_t)j->alen * 2, j->wav);
			j->wav_bytes += j->alen * 4;
		}
	}
	free(buf);
	__atomic_store_n(&g_snd_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

// --render-thread: draw on a separate thread. Video hash is produced by it in frame order
typedef struct {
	int frames, dump_frame;
	const char *dump_path;
	uint64_t hv;
} render_job;

static int write_ppm(const char *path, const uint32_t *fb);

// --ge-steps: chunked rendering like PSP (ge_layers + render_threaded, rei_core_render_step while main waits).
// Hashes the reference screen (index + palette) in frame order; must match the pipelined reference hash
static pthread_t g_main_thread;
static uint64_t g_step_hv = FNV_OFFSET;
static int g_step_frames;
static uint32_t *g_step_fb;

static int g_step_spr_bad, g_step_spr_frames;
static uint16_t g_step_model[REI_SCREEN_W * REI_SCREEN_H];

// G3 model: whether drawing the recorded sprite tiles back-to-front like the GE matches the CPU sprite buffer
static void check_sprite_tiles(void)
{
	int n = 0, ovf = 0;
	const uint8_t *gfx = NULL;
	const rei_spr_tile *t = rei_core_sprite_tiles(&n, &ovf, &gfx);
	const uint16_t *ref = rei_core_sprite_frame();
	if (!ref)
		return;
	memset(g_step_model, 0, sizeof(g_step_model));
	for (int i = n - 1; i >= 0; i--) {
		const rei_spr_tile *s = &t[i];
		const uint8_t *src = gfx + (size_t)s->code * 256;
		for (int y = 0; y < 16; y++) {
			int py = s->y + y;
			if (py < 0 || py >= REI_SCREEN_H)
				continue;
			for (int x = 0; x < 16; x++) {
				int px = s->x + x;
				if (px < 0 || px >= REI_SCREEN_W)
					continue;
				int p = src[((s->flags & 2) ? 15 - y : y) * 16 + ((s->flags & 1) ? 15 - x : x)];
				if (!p)
					continue;
				g_step_model[py * REI_SCREEN_W + px] = (uint16_t)(0x800 | (s->color << 4) | p | ((s->flags & 4) ? 0x8000 : 0));
			}
		}
	}
	g_step_spr_frames++;
	if (ovf || memcmp(g_step_model, ref, sizeof(g_step_model))) {
		if (g_step_spr_bad++ < 5)
			fprintf(stderr, "ge-sprites: frame %d differs (tiles %d overflow %d)\n", g_step_frames, n, ovf);
	}
}

static int ge_step(void)
{
	int r = rei_core_render_step();
	if (r == 0) {
		check_sprite_tiles();
		const uint32_t *pal = NULL;
		int blank = 0;
		const uint16_t *index = rei_core_index_frame(&pal, &blank);
		for (int i = 0; i < REI_SCREEN_W * REI_SCREEN_H; i++)
			g_step_fb[i] = index ? pal[index[i]] : 0;
		g_step_hv = fnv1a(g_step_hv, g_step_fb, REI_SCREEN_W * REI_SCREEN_H * 4);
		g_step_frames++;
	}
	return r;
}

static void ge_step_hook(void)
{
	if (pthread_equal(pthread_self(), g_main_thread))
		ge_step();
	else
		rei_core_sound_idle();
}

// --threads (without chunked render): apply deferred YM2151 writes while the sound thread waits (same path as PSP ME)
static void sound_idle_hook(void)
{
	if (!pthread_equal(pthread_self(), g_main_thread))
		rei_core_sound_idle();
}

#ifdef REI_RENDER_WRITES
// (diag) --render-inline: global memory (.data/.bss) addresses written by one render, including same-value writes (variables that must
// not share cache lines with main in PSP ME rendering). Data section is made read-only; on each write fault, log the address, unprotect
// the page, single-step one insn (trap flag), then protect again. x86-64 Linux only
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
extern char __data_start[], _end[];
#define RW_MAX 65536
// (diag state lives on the heap so the handler can write while the data section is protected. Pointer is read-only)
struct rw_state { uintptr_t addr[RW_MAX]; volatile int n; uintptr_t page, lo, hi; };
static struct rw_state *g_rw;
static void rw_segv(int sig, siginfo_t *si, void *uc_)
{
	(void)sig;
	ucontext_t *uc = (ucontext_t *)uc_;
	const uintptr_t a = (uintptr_t)si->si_addr;
	if (a < g_rw->lo || a >= g_rw->hi) { signal(SIGSEGV, SIG_DFL); return; } // (other fault: die via default handling)
	if (g_rw->n < RW_MAX) g_rw->addr[g_rw->n++] = a;
	g_rw->page = a & ~(uintptr_t)4095;
	mprotect((void *)g_rw->page, 4096, PROT_READ | PROT_WRITE);
	uc->uc_mcontext.gregs[REG_EFL] |= 0x100; // SIGTRAP after one insn
}
static void rw_trap(int sig, siginfo_t *si, void *uc_)
{
	(void)sig; (void)si;
	ucontext_t *uc = (ucontext_t *)uc_;
	if (g_rw->page) mprotect((void *)g_rw->page, 4096, PROT_READ);
	g_rw->page = 0;
	uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
}
static void rw_before(void)
{
	static int init;
	if (!init) {
		init = 1;
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_flags = SA_SIGINFO;
		sa.sa_sigaction = rw_segv;
		sigaction(SIGSEGV, &sa, NULL);
		sa.sa_sigaction = rw_trap;
		sigaction(SIGTRAP, &sa, NULL);
		g_rw = (struct rw_state *)calloc(1, sizeof(struct rw_state));
		fprintf(stderr, "rw data %zu bytes\n", (size_t)(_end - __data_start));
	}
	g_rw->lo = (uintptr_t)__data_start & ~(uintptr_t)4095;
	g_rw->hi = ((uintptr_t)_end + 4095) & ~(uintptr_t)4095;
	mprotect((void *)g_rw->lo, g_rw->hi - g_rw->lo, PROT_READ);
}
static void rw_after(int f)
{
	mprotect((void *)g_rw->lo, g_rw->hi - g_rw->lo, PROT_READ | PROT_WRITE);
	for (int i = 0; i < g_rw->n; i++)
		fprintf(stderr, "rwrite f%d %p 1\n", f, (void *)g_rw->addr[i]);
	g_rw->n = 0;
}
#endif
static void *render_thread(void *arg)
{
	render_job *j = (render_job *)arg;
	uint32_t *fb = (uint32_t *)calloc(g_w * g_h, 4);
	for (int f = 0; f < j->frames; f++) {
		if (!rei_core_render_frame(fb, g_w * 4))
			break;
		j->hv = fnv1a(j->hv, fb, g_w * g_h * 4);
		if (f == j->dump_frame && j->dump_path)
			write_ppm(j->dump_path, fb);
	}
	free(fb);
	return NULL;
}

// REI_PRERUN="set:frames[,set:frames...]": before the measured game, run other games in the same process and shut each
// down the way the Vita frontend does between games (rei_core_exit). The measured hashes must equal a fresh process
// (in-app game switching, docs/DECISIONS.md). Zips are looked up next to --rom; clones find their parent there too
// REI_INIT_THREAD=1: rei_core_init runs on a thread of its own and the frames on the main thread, as on the Vita
// (the loading screen draws while the loader thread reads the ROMs)
typedef struct { const rei_core_config *cfg; int rc; } init_job;
static void *init_thread(void *arg)
{
	init_job *j = (init_job *)arg;
	j->rc = rei_core_init(j->cfg);
	return NULL;
}
static int core_init(const rei_core_config *cfg)
{
	if (!getenv("REI_INIT_THREAD"))
		return rei_core_init(cfg);
	init_job j = { cfg, -1 };
	pthread_t t;
	pthread_create(&t, NULL, init_thread, &j);
	pthread_join(t, NULL);
	return j.rc;
}

typedef struct { int frames, alen; } prerun_snd;
static void *prerun_sound(void *arg)
{
	prerun_snd *j = (prerun_snd *)arg;
	int16_t *buf = (int16_t *)calloc((size_t)j->alen * 2 + 16, sizeof(int16_t));
	for (int f = 0; f < j->frames; f++)
		rei_core_sound_frame(buf);
	free(buf);
	return NULL;
}
static void *ym_worker_thread(void *arg);
// REI_PRERUN_VITA=1: the earlier games run with the Vita frontend's threads (M92: pipelined, sound thread, render
// frame by frame; M107: sound thread; other boards: deferred drawing, YM2151 worker thread) and stop them as it does
static int prerun(const char *spec, const char *rom, int pipelined, int rate)
{
	const int vita = getenv("REI_PRERUN_VITA") != NULL;
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", spec);
	const char *sl = strrchr(rom, '/');
	const int dl = sl ? (int)(sl - rom) + 1 : 0;
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		char *colon = strchr(tok, ':');
		const int frames = colon ? atoi(colon + 1) : 600;
		if (colon)
			*colon = 0;
		static char zip[1024], par[1024];
		snprintf(zip, sizeof(zip), "%.*s%s.zip", dl, rom, tok);
		par[0] = 0;
		for (int i = 0; i < rei_core_game_count(); i++)
			if (!strcmp(rei_core_game_set(i), tok) && rei_core_game_parent(i))
				snprintf(par, sizeof(par), "%.*s%s.zip", dl, rom, rei_core_game_parent(i));
		rei_core_config c = { .zip_path = zip, .sample_rate = rate, .bytes_per_pixel = 4, .clock_us = now_us,
			.pipelined = vita || pipelined, .threaded = vita, .render_threaded = vita, .set_name = tok,
			.parent_zip_path = par[0] ? par : NULL };
		if (core_init(&c) != 0) {
			fprintf(stderr, "prerun %s: init failed: %s\n", tok, rei_core_error());
			return 1;
		}
		int w = REI_SCREEN_W, h = REI_SCREEN_H, fl = 0;
		if (!rei_core_is_m92())
			rei_core_screen(&w, &h, &fl);
		uint32_t *pfb = (uint32_t *)calloc((size_t)w * h, 4);
		const int alen = rei_core_audio_len();
		int16_t *pau = (int16_t *)calloc((size_t)alen * 2 + 16, sizeof(int16_t));
		const int sthr = vita && rei_core_pipelined() && rei_core_sound_threaded();
		const int rthr = vita && rei_core_render_threaded();
		const int ythr = vita && !sthr && rei_core_ym_worker();
		pthread_t st, yt;
		prerun_snd sj = { frames, alen };
		if (sthr)
			pthread_create(&st, NULL, prerun_sound, &sj);
		if (ythr) {
			rei_core_ym_worker_enable(1);
			pthread_create(&yt, NULL, ym_worker_thread, NULL);
		}
		for (int f = 0; f < frames; f++) {
			rei_core_autoplay(f);
			rei_core_run_frame(rthr ? NULL : pfb, w * 4, sthr ? NULL : pau);
			if (rthr)
				rei_core_render_frame(pfb, w * 4);
		}
		if (sthr)
			pthread_join(st, NULL);
		if (ythr) {
			rei_core_ym_worker_enable(0);
			rei_core_stop();
			pthread_join(yt, NULL);
		}
		for (int i = 0; i < REI_IN_DIGITAL_COUNT; i++) // (the frontend releases every input between games)
			rei_core_set_input(i, 0);
		rei_core_exit();
		free(pfb);
		free(pau);
		fprintf(stderr, "prerun %s: %d frames\n", tok, frames);
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: harness --rom nbbatman.zip [--frames N] [--autoplay] [--checkpoint K]\n"
		"               [--set nbbatmanu --parent nbbatman.zip] [--reset-at F]\n"
		"               [--dump N:out.ppm] [--rate HZ] [--wav out.wav]\n"
		"               [--pchist out.txt] [--cover dir] [--no-recomp] [--no-idle-skip] [--mul N] [--mode fbneo|pipelined] [--threads] [--render-thread]\n");
}

#ifdef REI_DRC
extern unsigned long long rei_drc_gstep_ops[256];
extern int rei_drc_enabled, rei_drc35_enabled;
extern unsigned long long rei_drc_stats[13];
#endif
static void *ym_worker_thread(void *arg) { (void)arg; rei_core_ym_worker_run(); return NULL; }
static void *z80_worker_thread(void *arg) { (void)arg; rei_core_z80_worker_run(); return NULL; }

int main(int argc, char **argv)
{
	const char *rom = getenv("REI_ROM");
	const char *wav_path = NULL;
	const char *pchist_path = NULL;
	int page_cache = 0; // --page-cache: hot V33 code pages in a separate 16KB buffer (verifies PSP scratchpad path)
	int pchist_from = 0; // --pchist-from F: clear the histogram before frame F (gameplay section only)
	const char *cover_dir = NULL;
	int frames = 3600, checkpoint = 0, rate = 48000, use_autoplay = 0, reset_at = -1;
	const char *pad_path = NULL;
	int pad_n = 0, pad_f[256];
	unsigned pad_b[256];
	const char *set_name = NULL, *parent_zip = NULL;
	const char *cheat_dir = NULL; // --cheats dir/ : user cheat files (<set>.ini)
	int cheat_set[16][2], ncheat_set = 0;
	unsigned peek_addr = 0;
	int peek_len = 0;
	int dump_every = 0, start_stage = 0, ramstep = 10, ramfrom = 0, dump_from = 0, dump_to = 0x7fffffff;
	// Save state check: --save-at F file (save before frame F starts, restart hashing from there),
	// --load file (load before the first frame), --af-offset N (added to autoplay frame number)
	int save_at = -1, af_offset = 0;
	const char *save_path = NULL, *load_path = NULL;
	const char *dump_dir = NULL;
	FILE *ramdump = NULL; // appends V33 RAM 0xe0000~0xeffff every 10 frames (for analysis)
	struct { int from; unsigned addr, val; } pokes[8];
	int npokes = 0;
	int ym_me = 0, ym_me_release = -1, ym_me_split_at[8], ym_me_split_mask[8], ym_me_nsplit = 0;
	int ym_thread = 0; // --ym-thread: YM2151 synthesis for other boards on a worker thread (results must match)
	int z80_thread = 0; // --z80-thread: M72-family Z80 on a worker thread right behind the V30 (results must match)
	int rinline = 0, gem = 0, gm72 = 0;
	int dump_frame = -1, no_idle = 0, mul = 0, pipelined = 0, threads = 0, rthread = 0, gesteps = 0, s2m_delay = 0, ym_main = 0;
	const char *dump_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--autoplay")) use_autoplay = 1;
		else if (!strcmp(argv[i], "--pad") && i + 1 < argc) pad_path = argv[++i]; // (diag) "frame bits" lines: bits = REI_IN_* 0~7 (P1 coin, start, up/down/left/right, B1, B2)
		else if (!strcmp(argv[i], "--set") && i + 1 < argc) set_name = argv[++i];
		else if (!strcmp(argv[i], "--parent") && i + 1 < argc) parent_zip = argv[++i];
		else if (!strcmp(argv[i], "--reset-at") && i + 1 < argc) reset_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ramdump") && i + 1 < argc) ramdump = fopen(argv[++i], "wb");
		else if (!strcmp(argv[i], "--ramstep") && i + 2 < argc) { ramfrom = atoi(argv[++i]); ramstep = atoi(argv[++i]); } // start frame, interval
		else if (!strcmp(argv[i], "--stage") && i + 1 < argc) start_stage = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--inf-health")) rei_core_set_infinite_health(1);
		else if (!strcmp(argv[i], "--char") && i + 1 < argc) rei_core_set_autoplay_char(atoi(argv[++i])); // autoplay character 1~4
		else if (!strcmp(argv[i], "--save-at") && i + 2 < argc) { save_at = atoi(argv[++i]); save_path = argv[++i]; }
		else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_path = argv[++i];
		else if (!strcmp(argv[i], "--af-offset") && i + 1 < argc) af_offset = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
			// --poke frame:addr:value (hex addr/value). Written every frame from that frame on (stage variable experiments)
			if (npokes < 8 && sscanf(argv[++i], "%d:%x:%x", &pokes[npokes].from, &pokes[npokes].addr, &pokes[npokes].val) == 3)
				npokes++;
		}
		else if (!strcmp(argv[i], "--threads")) threads = 1;
		else if (!strcmp(argv[i], "--ym-thread")) ym_thread = 1;
		else if (!strcmp(argv[i], "--z80-thread")) z80_thread = 1;
		else if (!strcmp(argv[i], "--ym-me-release") && i + 1 < argc) ym_me_release = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ym-me-split") && i + 2 < argc) { if (ym_me_nsplit < 8) { ym_me_split_at[ym_me_nsplit] = atoi(argv[i + 1]); ym_me_split_mask[ym_me_nsplit++] = (int)strtol(argv[i + 2], NULL, 16); } i += 2; } // F mask (hex), repeatable
		else if (!strcmp(argv[i], "--ym-me")) ym_me = 1; // (PSP path) YM2151 synthesis via the ME-side code, run by main each frame instead. Sound 1 frame late
		else if (!strcmp(argv[i], "--render-thread")) rthread = 1;
		else if (!strcmp(argv[i], "--render-inline")) rthread = rinline = 1; // (render on the main thread right away each frame)
		else if (!strcmp(argv[i], "--ge-steps")) gesteps = 1;
		else if (!strcmp(argv[i], "--ge-m72")) rthread = rinline = gm72 = 1; // (check PSP M72-family tile layer GE lists, gm72_check.c)
		else if (!strcmp(argv[i], "--ge-m107")) rthread = rinline = gem = 1; // (interpret PSP M107 tile layer GE lists and compare with CPU screen, gem_check.c)
		else if (!strcmp(argv[i], "--s2m-delay") && i + 1 < argc) s2m_delay = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ym-main") && i + 1 < argc) ym_main = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--page-cache")) page_cache = 1;
		else if (!strcmp(argv[i], "--no-span")) { extern int rei_span_enabled; rei_span_enabled = 0; }
		else if (!strcmp(argv[i], "--vlog")) { extern int rei_m72_vlog, rei_defer_vlog; rei_m72_vlog = rei_defer_vlog = 1; } // (diag) M72 screen-off bit log
		else if (!strcmp(argv[i], "--span-max") && i + 1 < argc) { extern int rei_span_max_lines; rei_span_max_lines = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--no-span35")) { extern int rei_span35_enabled; rei_span35_enabled = 0; }
		else if (!strcmp(argv[i], "--no-idle-skip")) no_idle = 1;
		else if (!strcmp(argv[i], "--no-v25-auto")) { extern int rei_v25_auto_enabled; rei_v25_auto_enabled = 0; } // (disable V35 idle-loop auto skip)
		else if (!strcmp(argv[i], "--psp-idle")) { // (match PSP non-M92 boards: idle skip off, V35 off only during dynarec)
			extern int rei_v25_auto_with_drc;
			no_idle = 1;
			rei_v25_auto_with_drc = 0;
		}
#ifdef REI_DRC
		else if (!strcmp(argv[i], "--no-drc")) rei_drc_enabled = 0;
		else if (!strcmp(argv[i], "--drc35-keep")) { extern int rei_drc35_short_off; rei_drc35_short_off = 0; } // (keep V35 dynarec even with short blocks)
		else if (!strcmp(argv[i], "--v25-auto-drc")) { extern int rei_v25_auto_with_drc, rei_v25_auto_drc_policy; rei_v25_auto_with_drc = rei_v25_auto_drc_policy = 1; } // (after --psp-idle: auto idle during V35 dynarec too, toggled per window; PSP default)
		else if (!strcmp(argv[i], "--no-drc35")) rei_drc35_enabled = 0;
#endif
		else if (!strcmp(argv[i], "--mul") && i + 1 < argc) mul = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "pipelined")) pipelined = 1;
			else if (strcmp(m, "fbneo")) { usage(); return 2; }
		}
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
		else if (!strcmp(argv[i], "--pchist") && i + 1 < argc) pchist_path = argv[++i];
		else if (!strcmp(argv[i], "--pchist-from") && i + 1 < argc) pchist_from = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cheats") && i + 1 < argc) cheat_dir = argv[++i];
		else if (!strcmp(argv[i], "--cheat") && i + 1 < argc) { // --cheat index=option (repeatable)
			if (ncheat_set < 16 && sscanf(argv[++i], "%d=%d", &cheat_set[ncheat_set][0], &cheat_set[ncheat_set][1]) == 2)
				ncheat_set++;
		}
		else if (!strcmp(argv[i], "--peek") && i + 2 < argc) { peek_addr = (unsigned)strtoul(argv[++i], NULL, 16); peek_len = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--cover") && i + 1 < argc) cover_dir = argv[++i];
		else if (!strcmp(argv[i], "--no-recomp")) { extern int rei_rc_enabled; rei_rc_enabled = 0; }
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			char *spec = argv[++i], *colon = strchr(spec, ':');
			if (!colon) { usage(); return 2; }
			*colon = '\0';
			dump_frame = atoi(spec);
			dump_path = colon + 1;
		} else if (!strcmp(argv[i], "--dump-range") && i + 2 < argc) { // (with --dump-every: only that frame range)
			dump_from = atoi(argv[++i]); dump_to = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) {
			// --dump-every N:dir -> dir/fNNNNN.ppm every N frames (single-thread path)
			char *spec = argv[++i], *colon = strchr(spec, ':');
			if (!colon) { usage(); return 2; }
			*colon = '\0';
			dump_every = atoi(spec);
			dump_dir = colon + 1;
		} else { usage(); return 2; }
	}
	if (!rom) { usage(); return 2; }
	(void)pchist_from; // used only in REI_PCHIST builds
	static char auto_parent[1024];
	if (!parent_zip && set_name) { // clone: <parent>.zip in the same folder as --rom
		for (int i = 0; i < rei_core_game_count(); i++) {
			const char *par = rei_core_game_parent(i);
			if (strcmp(rei_core_game_set(i), set_name) != 0 || !par)
				continue;
			const char *sl = strrchr(rom, '/');
			const int dl = sl ? (int)(sl - rom) + 1 : 0;
			snprintf(auto_parent, sizeof(auto_parent), "%.*s%s.zip", dl, rom, par);
			parent_zip = auto_parent;
		}
	}

	rei_core_config cfg = { .zip_path = rom, .sample_rate = rate, .bytes_per_pixel = 4, .clock_us = now_us,
		.no_idle_skip = no_idle, .interleave_mul = mul, .pipelined = pipelined, .threaded = threads,
		.render_threaded = rthread, .set_name = set_name, .parent_zip_path = parent_zip };
	if ((threads || gesteps) && !pipelined) { fprintf(stderr, "--threads requires --mode pipelined\n"); return 2; }
	cfg.s2m_delay_lines = s2m_delay;
	cfg.ym_main_channels = ym_main; // used only with REI_YM_SPLIT build + --threads
	if (threads && !gesteps) {
		g_main_thread = pthread_self();
		cfg.wait_hook = sound_idle_hook;
	}
	if (gesteps) {
		if (rthread) { fprintf(stderr, "--ge-steps and --render-thread cannot be combined\n"); return 2; }
		g_main_thread = pthread_self();
		g_step_fb = (uint32_t *)calloc(REI_SCREEN_W * REI_SCREEN_H, 4);
		cfg.render_threaded = 1;
		cfg.index_out = 1;
		cfg.ge_layers = 1;
		cfg.wait_hook = ge_step_hook;
		cfg.ge_sprites = 1;
	}
	if (getenv("REI_PRERUN") && prerun(getenv("REI_PRERUN"), rom, pipelined, rate) != 0)
		return 1;
	if (core_init(&cfg) != 0) {
		fprintf(stderr, "init failed: %s\n", rei_core_error());
		return 1;
	}
	if (cheat_dir) {
		const int nc = rei_core_cheats_load(cheat_dir);
		fprintf(stderr, "cheats: %d\n", nc);
		for (int k = 0; k < nc; k++) {
			fprintf(stderr, "  %d %s:", k, rei_core_cheat_name(k));
			for (int o = 0; o < rei_core_cheat_options(k); o++)
				fprintf(stderr, " [%d %s]", o, rei_core_cheat_option_name(k, o));
			fprintf(stderr, "\n");
		}
		for (int k = 0; k < ncheat_set; k++)
			rei_core_cheat_set(cheat_set[k][0], cheat_set[k][1]);
	}
	if (peek_len > 0) { // --peek ADDR LEN: V33 address space bytes (code inspection)
		for (int i = 0; i < peek_len; i++)
			printf("%02x%s", rei_core_peek(peek_addr + i), (i % 16 == 15) ? "\n" : " ");
		printf("\n");
		return 0;
	}

	if (page_cache) {
		static unsigned char scratch[16384];
		int n;
		const unsigned int *pages = rei_core_v33_hot_pages(&n);
		fprintf(stderr, "page cache: %d pages\n", rei_core_v33_page_cache(scratch, sizeof(scratch), pages, n));
	}
	rei_core_set_start_stage(start_stage);
	int alen = rei_core_audio_len();
	if (!rei_core_is_m92()) {
		int sflags;
		rei_core_screen(&g_w, &g_h, &sflags);
		fprintf(stderr, "board %s, screen %dx%d flags %d\n", rei_core_board(), g_w, g_h, sflags);
	}
	uint32_t *fb = (uint32_t *)calloc(g_w * g_h, 4);
	int16_t *audio = (int16_t *)calloc((size_t)alen * 2 + 16, sizeof(int16_t));
	FILE *wav = NULL;
	uint32_t wav_bytes = 0;
	if (wav_path) {
		wav = fopen(wav_path, "wb");
		if (wav) wav_header(wav, rate, 0);
	}

	uint64_t hv = FNV_OFFSET, ha = FNV_OFFSET;
	sound_job job = { frames, alen, wav, 0, FNV_OFFSET };
	pthread_t snd;
	if (threads)
		pthread_create(&snd, NULL, sound_thread, &job);
	if (rthread && !rei_core_render_threaded()) { // (M92: --mode pipelined; other boards: drivers supporting deferred drawing)
		fprintf(stderr, "--render-thread: this game cannot use the render thread\n");
		return 2;
	}
	pthread_t ymt, z8t;
	if (z80_thread) {
		if (!rei_core_z80_worker()) { fprintf(stderr, "--z80-thread: not supported for this game\n"); return 2; }
		rei_core_z80_worker_enable(1);
		pthread_create(&z8t, NULL, z80_worker_thread, NULL);
	}
	if (ym_thread) {
		if (!rei_core_ym_worker()) { fprintf(stderr, "--ym-thread: not supported for this game\n"); return 2; }
		rei_core_ym_worker_enable(1);
		pthread_create(&ymt, NULL, ym_worker_thread, NULL);
	}
	if (ym_me) {
		if (rei_core_ym_me_enable(1)) { fprintf(stderr, "--ym-me: not supported for this game\n"); return 2; }
		extern int rei_ym_me_inline;
		rei_ym_me_inline = 1;
	}
	render_job rjob = { frames, dump_frame, dump_path, FNV_OFFSET };
	pthread_t rnd;
	if (rthread && !rinline)
		pthread_create(&rnd, NULL, render_thread, &rjob);
	uint32_t *rfb = rinline ? (uint32_t *)calloc(g_w * g_h, 4) : NULL;
	double t0 = now_sec();
	int af0 = -af_offset; // after reset, autoplay restarts from the beginning
	if (load_path && rei_core_state_load(load_path) != 0) {
		fprintf(stderr, "load failed: %s\n", load_path);
		return 1;
	}
	if (gesteps)
		rei_core_set_ge_reference(1);
	extern int gem_check_setup(void);
	extern void gem_check_report(void);
	if (gem && gem_check_setup() != 0)
		return 2;
	extern int gm72_check_setup(void);
	extern void gm72_check_report(void);
	if (gm72 && gm72_check_setup() != 0)
		return 2;
	for (int f = 0; f < frames; f++) {
		if (f == reset_at && !threads && !rthread) {
			rei_core_reset();
			af0 = f;
			hv = ha = FNV_OFFSET; // hash only frames after reset (to compare with a run from scratch)
		}
		if (f == save_at && save_path) {
			int rc = rei_core_state_save(save_path);
			fprintf(stderr, "save f%d → %s rc %d\n", f, save_path, rc);
			hv = ha = FNV_OFFSET; // hash only frames after save (to compare with a loaded run)
		}
#ifdef REI_PCHIST
		if (pchist_from > 0 && f == pchist_from)
			rei_core_pchist_reset();
#endif
		if (use_autoplay)
			rei_core_autoplay(f - af0);
		if (pad_path) { // (diag) replay play input; same scene as PSP dev loop rei_pad, in the harness
			if (!pad_n) {
				FILE *pf = fopen(pad_path, "r");
				if (pf) { while (pad_n < 256 && fscanf(pf, "%d %x", &pad_f[pad_n], &pad_b[pad_n]) == 2) pad_n++; fclose(pf); }
				if (!pad_n) pad_n = -1;
			}
			unsigned b = 0;
			for (int k = 0; k < pad_n && pad_f[k] <= f; k++) b = pad_b[k];
			for (int k = 0; k < 8; k++) rei_core_set_input(k, (b >> k) & 1);
		}
		for (int k = 0; k < npokes; k++)
			if (pokes[k].from < 0 ? f == -pokes[k].from : f >= pokes[k].from) // negative frame: once at that frame only
				rei_core_poke(pokes[k].addr, (uint8_t)pokes[k].val);
		if (ramdump && f % ramstep == 0 && f >= ramfrom)
			for (uint32_t a = 0xe0000; a < 0xf0000; a++)
				fputc(rei_core_peek(a), ramdump);
		memset(audio, 0, (size_t)alen * 2 * sizeof(int16_t));
#ifdef REI_V33STAT
		double rei_st_t = now_sec();
#endif
		rei_core_run_frame((rthread || gesteps) ? NULL : fb, g_w * 4, threads ? NULL : audio);
		if (ym_me)
			while (rei_core_ym_me_poll()) { }
		if ((f + 1) % 60 == 0 && getenv("REI_RAMH")) { // (diag) compare with PSP dev log "ramh"
			extern uint32_t rei_core_ram_hash(void);
			fprintf(stderr, "ramh %d %08x\n", f + 1, (unsigned)rei_core_ram_hash());
		}
		if (ym_me && f + 1 == ym_me_release) // (--ym-me-release F: hand to main after frame F; PSP ME overflow handling)
			rei_core_ym_me_release();
		for (int k = 0; ym_me && k < ym_me_nsplit; k++) // (--ym-me-split F mask: main takes those channels after frame F; PSP ME overflow handling)
			if (f + 1 == ym_me_split_at[k] && rei_core_ym_me_split(ym_me_split_mask[k]) != 0) fprintf(stderr, "ym-me-split failed\n");
		while (gesteps && g_step_frames < f) // finish drawing up to the previous frame (this frame is drawn during the next)
			ge_step();
		if (rinline) {
#ifdef REI_RENDER_WRITES
			rw_before();
#endif
			rei_core_render_frame(rfb, g_w * 4);
#ifdef REI_RENDER_WRITES
			rw_after(f);
#endif
			rjob.hv = fnv1a(rjob.hv, rfb, g_w * g_h * 4);
		}
#ifdef REI_DRC
		if (getenv("REI_DRC_FRAMES")) { // (diag) per-frame dynarec activity: block entries, block insns, interpreted insns, compiled insns, handler calls
			static unsigned long long p[13], pf, pp;
			extern unsigned long long rei_drc_probes, rei_drc_finds;
			fprintf(stderr, "drcf %d runs %llu bins %llu interp %llu cins %llu hcall %llu flush %llu finds %llu probes %llu\n", f,
				rei_drc_stats[0] - p[0], rei_drc_stats[1] - p[1], rei_drc_stats[2] - p[2], rei_drc_stats[7] - p[7],
				rei_drc_stats[12] - p[12], rei_drc_stats[4] - p[4], rei_drc_finds - pf, rei_drc_probes - pp);
			pf = rei_drc_finds;
			pp = rei_drc_probes;
			memcpy(p, rei_drc_stats, sizeof(p));
		}
#endif
#ifdef REI_V33STAT
		{
			extern unsigned long long rei_v33st[4];
			extern unsigned long long rei_auto_skipped(int cpu);
			static unsigned long long pa, pi;
			unsigned long long iv, is;
			int id;
			rei_core_idle_stats(0, &iv, &is, &id);
			fprintf(stderr, "v33st %d ins %llu cyc %llu hrd %llu hwr %llu us %.0f auto %llu idle %llu\n", f, rei_v33st[0], rei_v33st[1],
				rei_v33st[2], rei_v33st[3], (now_sec() - rei_st_t) * 1e6, rei_auto_skipped(0) - pa, is - pi);
			pa = rei_auto_skipped(0);
			pi = is;
			memset(rei_v33st, 0, sizeof(rei_v33st));
		}
#endif

		if (!rthread && !gesteps)
			hv = fnv1a(hv, fb, g_w * g_h * 4);
		if (threads) {
			if (f == dump_frame && dump_path)
				write_ppm(dump_path, fb);
			if (checkpoint > 0 && (f + 1) % checkpoint == 0)
				printf("frame=%d video=%016llx audio=(thread)\n", f + 1, (unsigned long long)hv);
			continue;
		}
		ha = fnv1a(ha, audio, (size_t)alen * 2 * sizeof(int16_t));
		if (wav) {
			fwrite(audio, sizeof(int16_t), (size_t)alen * 2, wav);
			wav_bytes += alen * 4;
		}
		if (f == dump_frame && dump_path)
			write_ppm(dump_path, fb);
		if (dump_every > 0 && f % dump_every == 0 && f >= dump_from && f < dump_to) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/f%05d.ppm", dump_dir, f);
			write_ppm(path, fb);
		}
		if (checkpoint > 0 && (f + 1) % checkpoint == 0)
			printf("frame=%d video=%016llx audio=%016llx\n", f + 1,
				(unsigned long long)hv, (unsigned long long)ha);
	}
	if (rthread) {
		if (!rinline) pthread_join(rnd, NULL);
		hv = rjob.hv;
	}
	if (gem)
		gem_check_report();
	if (gm72)
		gm72_check_report();
	if (gesteps) {
		while (g_step_frames < frames && ge_step() >= 0)
			;
		hv = g_step_hv;
		fprintf(stderr, "ge-sprites: %d frames checked, %d differ\n", g_step_spr_frames, g_step_spr_bad);
	}
	if (threads) {
		// With YM2151 channel split, the sound side's last frame waits for main's share, so advance main's share until done
		while (!__atomic_load_n(&g_snd_done, __ATOMIC_ACQUIRE))
			rei_core_main_poll();
		pthread_join(snd, NULL);
		ha = job.ha;
		wav_bytes = job.wav_bytes;
	}
	if (ym_thread) {
		rei_core_ym_worker_enable(0); // (after all remaining writes are applied)
		rei_core_stop();
		pthread_join(ymt, NULL);
	}
	if (z80_thread) {
		rei_core_z80_worker_enable(0);
		rei_core_stop();
		pthread_join(z8t, NULL);
	}
	double dt = now_sec() - t0;
#ifdef REI_RC_STATS
	{
		extern unsigned long long rei_rc_st[4];
		extern unsigned rei_rc_miss[1 << 20];
		fprintf(stderr, "rc: calls %llu cycles %llu interp %llu (non-idle %llu)\n", rei_rc_st[0], rei_rc_st[1], rei_rc_st[2], rei_rc_st[3]);
		for (int k = 0; k < 8; k++) {
			int b = 0;
			for (int i = 0; i < (1 << 20); i++) if (rei_rc_miss[i] > rei_rc_miss[b]) b = i;
			fprintf(stderr, "rc miss %05x %u\n", b, rei_rc_miss[b]);
			rei_rc_miss[b] = 0;
		}
	}
#endif

#ifdef REI_TIMER_COUNT
	{ extern unsigned long long rei_timer_conv[2]; fprintf(stderr, "timer conv: ticks %llu cycles %llu\n", rei_timer_conv[0], rei_timer_conv[1]); }
#endif
	if (wav_path && wav_bytes == 0)
		fprintf(stderr, "warn: WAV is empty\n");
	if (rthread) { // (diag) deferred drawing: segments per frame (state is captured per segment)
		extern unsigned long long rei_defer_stats[3];
		if (rei_defer_stats[0])
			fprintf(stderr, "defer: frames %llu segments/frame %.2f\n", rei_defer_stats[0],
				(double)rei_defer_stats[1] / (double)rei_defer_stats[0]);
	}
	if (getenv("REI_V25SKIP")) { // (diag) cycles skipped by V35 idle-loop auto skip (compare with PSP dev log "v35 auto skipped")
		extern unsigned long long rei_v25_auto_skipped[4];
		fprintf(stderr, "v35 auto skipped %llu (cpu1 %llu)\n", rei_v25_auto_skipped[0], rei_v25_auto_skipped[1]);
		extern unsigned long long rei_vez_runs[4];
		fprintf(stderr, "VezRun calls/frame: cpu0 %.1f cpu1 %.1f\n", (double)rei_vez_runs[0] / frames, (double)rei_vez_runs[1] / frames);
#ifdef REI_DRC
		extern unsigned long long rei_drc35_stats[13]; // (V35 dynarec: block entries, block insns, interpreted insns)
		fprintf(stderr, "drc35 runs %llu binsns %llu interp %llu\n", rei_drc35_stats[0], rei_drc35_stats[1], rei_drc35_stats[2]);
		extern unsigned long long rei_v25_visits[2];
		fprintf(stderr, "v35 visits/frame %.1f skipping %.1f\n", (double)rei_v25_visits[0] / frames, (double)rei_v25_visits[1] / frames);
		extern unsigned long long rei_drc35_cycles;
		fprintf(stderr, "drc35 cycles/frame %.0f\n", (double)rei_drc35_cycles / frames);
		extern unsigned long long rei_z80_drc_stats[6];
		fprintf(stderr, "z80 drc runs %llu blocks %llu insns %llu flushes %llu interp %llu\n", rei_z80_drc_stats[0], rei_z80_drc_stats[1],
			rei_z80_drc_stats[2], rei_z80_drc_stats[3], rei_z80_drc_stats[4]);
		fprintf(stderr, "drc (V30/V33) code max %llu bytes, compiled insns %llu (%.1f bytes/insn), compiled blocks %llu\n", rei_drc_stats[6],
			rei_drc_stats[7], rei_drc_stats[7] ? (double)rei_drc_stats[6] / rei_drc_stats[7] : 0.0, rei_drc_stats[3]);
#endif
	}
	printf("frames=%d video=%016llx audio=%016llx\n", frames,
		(unsigned long long)hv, (unsigned long long)ha);
	fprintf(stderr, "%.2f s, %.1f fps (%.3f ms/frame), audio %d samples/frame @ %d Hz\n",
		dt, frames / dt, dt * 1000.0 / frames, alen, rate);

	// REI_PROFILE builds print average ms/frame per section
	uint64_t acc[REI_PROF_COUNT];
	int np = rei_core_prof_take(acc, REI_PROF_COUNT);
	for (int i = 0; i < np; i++)
		fprintf(stderr, "prof %-10s %7.3f ms/frame\n", rei_core_prof_name(i),
			acc[i] / (double)frames / 1000.0);

#ifdef REI_DRC
	fprintf(stderr, "drc: block runs %llu, block insns %llu, interpreted %llu, compiled %llu, flushes %llu, bank misses %llu, code %llu KB, insns %llu, links %llu, volatile pages %llu%c",
		rei_drc_stats[0], rei_drc_stats[1], rei_drc_stats[2], rei_drc_stats[3], rei_drc_stats[4], rei_drc_stats[5],
		rei_drc_stats[6] / 1024, rei_drc_stats[7], rei_drc_stats[8], rei_drc_stats[9], 10);
	{
		extern unsigned rei_drc_flush_why[6];
		extern unsigned long long rei_drc_variants;
		extern unsigned long long rei_drc_probes, rei_drc_finds;
		fprintf(stderr, "drc finds %llu, probes %llu%c", rei_drc_finds, rei_drc_probes, 10);
#ifdef __mips__
		extern unsigned long long rei_drc35_stats[13];
		fprintf(stderr, "drc35: block runs %llu, block insns %llu, interpreted %llu, compiled %llu, flushes %llu, bank misses %llu, code %llu KB, links %llu%c",
			rei_drc35_stats[0], rei_drc35_stats[1], rei_drc35_stats[2], rei_drc35_stats[3], rei_drc35_stats[4], rei_drc35_stats[5],
			rei_drc35_stats[6] / 1024, rei_drc35_stats[8], 10);
		extern unsigned rei_drc35_flush_why[6];
		fprintf(stderr, "drc35 flush why: code %u, table %u, overflow %u, pages %u, idle %u, opcode table %u%c", rei_drc35_flush_why[0],
			rei_drc35_flush_why[1], rei_drc35_flush_why[2], rei_drc35_flush_why[3], rei_drc35_flush_why[4], rei_drc35_flush_why[5], 10);
		extern unsigned long long rei_z80_drc_stats[6];
		fprintf(stderr, "z80 drc: block runs %llu, compiled %llu blocks (%llu insns), flushes %llu, interpreted %llu (ram/enc pages %llu)%c",
			rei_z80_drc_stats[0], rei_z80_drc_stats[1], rei_z80_drc_stats[2], rei_z80_drc_stats[3], rei_z80_drc_stats[4],
			rei_z80_drc_stats[5], 10);
#endif
		fprintf(stderr, "drc flush why: code %u, table %u, overflow %u, pages %u, idle %u, same-place variants %llu%c", rei_drc_flush_why[0],
			rei_drc_flush_why[1], rei_drc_flush_why[2], rei_drc_flush_why[3], rei_drc_flush_why[4], rei_drc_variants, 10);
	}
	if (getenv("REI_DRC_GSTEP")) { // opcodes with many in-block interpreter steps
		for (int k = 0; k < 12; k++) {
			int best = -1;
			for (int o = 0; o < 256; o++)
				if (rei_drc_gstep_ops[o] && (best < 0 || rei_drc_gstep_ops[o] > rei_drc_gstep_ops[best])) best = o;
			if (best < 0) break;
			fprintf(stderr, "gstep %02x %llu%c", best, rei_drc_gstep_ops[best], 10);
			rei_drc_gstep_ops[best] = 0;
		}
	}
#endif
	for (int c = 0; c < 2; c++) {
		unsigned long long v, sk;
		int d;
		rei_core_idle_stats(c, &v, &sk, &d);
		fprintf(stderr, "idle %s: visits %llu, skipped %llu cycles, last d %d\n", c ? "V35" : "V33", v, sk, d);
	}

	{
		unsigned pn, pl, pb;
		rei_core_scan_stats(&pn, &pl, &pb);
		fprintf(stderr, "scan V33: native steps %u, learned %u, bad %u", pn, pl, pb);
		fputc(10, stderr);
		rei_core_task_stats(&pn, &pl, &pb);
		fprintf(stderr, "task V33: native %u, learned %u, bad %u", pn, pl, pb);
		fputc(10, stderr);
		rei_core_pcheck_stats(&pn, &pl, &pb);
		fprintf(stderr, "pcheck V33: native %u, learned %u, bad %u", pn, pl, pb);
		fputc(10, stderr);
		rei_core_piece_stats(&pn, &pl, &pb);
		fprintf(stderr, "piece V33: native %u, learned %u, bad %u\n", pn, pl, pb);
	}

#ifdef REI_PAGEHIST
	{ // 512-byte pages with the most V33 accesses (read + code + write)
		extern unsigned rei_pagehist[3][2048];
		static unsigned tot[2048];
		unsigned long long all = 0;
		for (int i = 0; i < 2048; i++) { tot[i] = rei_pagehist[0][i] + rei_pagehist[1][i] + rei_pagehist[2][i]; all += tot[i]; }
		unsigned long long cum = 0;
		for (int k = 0; k < 48; k++) {
			int best = 0;
			for (int i = 1; i < 2048; i++) if (tot[i] > tot[best]) best = i;
			cum += tot[best];
			fprintf(stderr, "page %05x rd %u code %u wr %u  cum %.1f%%", best << 9, rei_pagehist[0][best], rei_pagehist[1][best],
				rei_pagehist[2][best], 100.0 * cum / all);
			fputc(10, stderr);
			tot[best] = 0;
		}
	}
#endif
	if (pchist_path && rei_core_pchist_dump(pchist_path, getenv("REI_PCHIST_TOP") ? atoi(getenv("REI_PCHIST_TOP")) : 40) != 0)
		fprintf(stderr, "pchist: not a REI_PCHIST build or write failed\n");
	if (cover_dir) {
		char cp[1024], ip[1024];
		snprintf(cp, sizeof(cp), "%s/v33_cover.txt", cover_dir);
		snprintf(ip, sizeof(ip), "%s/v33_image.bin", cover_dir);
		if (rei_core_cover_dump(cp, ip) != 0)
			fprintf(stderr, "cover: not a REI_PCHIST build or write failed\n");
	}

	if (wav) {
		fseek(wav, 0, SEEK_SET);
		wav_header(wav, rate, wav_bytes);
		fclose(wav);
	}
	rei_core_exit();
	free(fb);
	free(audio);
	return 0;
}
