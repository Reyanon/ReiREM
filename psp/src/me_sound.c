// Runs the sound side on the Media Engine (ME) (psp/docs/DECISIONS.md PSP-ADR-003).
//
// ME code is launched with mcidclan psp-media-engine-custom-core (MIT, https://github.com/mcidclan/psp-media-engine-custom-core):
// copy that library's handler to the ME reset vector, reset the ME, and meLibOnProcess() runs on the ME.
// We call the same steps directly instead of meLibDefaultInit(): kinit swaps the system event (sleep) handler, which
// would point at freed code when the dev loop unloads only the module (sleep during ME run is unsupported for now).
//
// The ME and the main CPU do not share caches:
// - core data used by the ME is gathered into one block (.rei_me) by linker script psp/me.ld (src/core/shim/rei_me.h)
// - everything exchanged here (start/stop flags, audio ring) is accessed only via uncached addresses (REI_ME_SHARED + REI_SH)
// - ME code makes no system calls (including the core sound path: wait hook NULL, profile clock 0)
#include "me_sound.h"

#include <me-core-mapper/me-core.h>
#include <kubridge.h>
#include <psploadcore.h>
#include <pspsysevent.h>
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rei_core.h"
#include "log.h"
#include "rei_me.h"
#include "rei_defer.h" // (other boards: drawing on the ME)

#define RING 8          // audio frame ring (~133ms)
#define MAX_LEN 1024    // max stereo samples per frame

// main <-> ME shared (uncached only)
static volatile uint32_t g_go REI_ME_SHARED, g_quit REI_ME_SHARED, g_done REI_ME_SHARED;
static volatile uint32_t g_w REI_ME_SHARED, g_r REI_ME_SHARED; // audio ring write (ME) / read (main) positions
static int16_t g_ring[RING][MAX_LEN * 2] REI_ME_SHARED;
static volatile int32_t g_len REI_ME_SHARED;
static volatile uint32_t g_drops REI_ME_SHARED; // frames dropped on a full ring (emulation never stalls even if main doesn't drain audio)
// diag: ME compute speed at start (main compares against the same loop to see ME speed per execution mode)
static volatile uint32_t g_bench REI_ME_SHARED, g_bench_done REI_ME_SHARED, g_bench_sink REI_ME_SHARED;
static volatile uint32_t g_breq REI_ME_SHARED, g_back REI_ME_SHARED, g_bkind REI_ME_SHARED, g_bbuf REI_ME_SHARED;
static unsigned g_bres[3]; // (main) ME compute / cached read / uncached read us
#define BENCH_N 20000000u
uint32_t me_bench_loop(uint32_t n)
{
	uint32_t x = 1;
	for (uint32_t i = 0; i < n; i++)
		x = x * 1664525u + 1013904223u;
	return x;
}

// ME only (ME cache)
static int16_t g_local[MAX_LEN * 2] __attribute__((section(".rei_me_local"), aligned(64)));

// ---- other boards: deferred drawing (rei_defer) rendered on the ME (PSP-ADR-013) ----
// Once main snapshots the draw state each frame (rei_defer_frame: writes back dcache, then signals), the ME draws into
// buffer (frames drawn % 3) of 3 screen buffers. Render-only globals live in .rei_rdr so they share no cache lines with main.
// The ME never draws more than 2 frames ahead of what main has shown (so it never overwrites the buffer the GE reads)
#define RFB 3
static inline void me_delay(void);
static volatile uint32_t g_mode REI_ME_SHARED;   // 1 sound (M92), 2 drawing (other boards)
static volatile uint32_t g_rfb[RFB] REI_ME_SHARED, g_rpitch REI_ME_SHARED;
static volatile uint32_t g_rshown REI_ME_SHARED; // frames shown by main
static volatile uint32_t g_rpause REI_ME_SHARED, g_rbusy REI_ME_SHARED, g_rframes REI_ME_SHARED;
static volatile uint32_t g_rburn REI_ME_SHARED; // (diag) busy-loop iterations for the ME per frame
void me_render_set_burn(uint32_t n) { REI_SH(g_rburn) = n; }
static volatile uint32_t g_ym_me REI_ME_SHARED; // YM2151 synthesis on the ME too (set before me_render_start)
void me_render_set_ym(int on) { REI_SH(g_ym_me) = on ? 1 : 0; }
// index mode (before me_render_start): ME draws color indices into the screen buffer, palette in clut[k]; the GE applies color
static volatile uint32_t g_rindex REI_ME_SHARED, g_rclut[RFB] REI_ME_SHARED;
static volatile uint32_t g_ridx_bytes REI_ME_SHARED;
void me_render_set_index_bytes(uint32_t bytes) { REI_SH(g_ridx_bytes) = bytes; }
void me_render_set_index(int on, void *const clut[3])
{
	REI_SH(g_rindex) = on ? 1 : 0;
	for (int i = 0; i < RFB; i++)
		REI_SH(g_rclut[i]) = on ? (uint32_t)clut[i] : 0;
}

// (ME) just before the renderer finishes a frame (rei_render_done_hook): push drawn screen/buffers to memory. In index mode,
// after reliably writing back drawn indices/palette. (Main sets the hook once in me_render_start: if the ME writes a main global,
// stale values in that cache line overwrite main vars: rewriting the hook per frame reverted s_open on the same line, giving blank frames, majtitle flicker)
static uint32_t g_flush_buf __attribute__((section(".rei_me_local"))), g_flush_clut __attribute__((section(".rei_me_local")));
static void me_render_flush(void);
static inline int me_on_me(void)
{
	uintptr_t sp;
	__asm__ volatile("move %0, $sp" : "=r"(sp));
	return (sp >> 31) != 0;
}
// draw-done hook (mode 3: main draws too): write back the ME cache on the ME, main cache on main
static void me_render_done_any(void)
{
	if (me_on_me())
		me_render_flush();
	else
		sceKernelDcacheWritebackAll();
}
static void me_render_flush(void)
{
	if (REI_SH(g_rindex)) {
		meLibDcacheWritebackRange(g_flush_buf, REI_SH(g_ridx_bytes));
		meLibDcacheWritebackRange(g_flush_clut, 0x800 * 2);
	}
	meLibDcacheWritebackInvalidateAll();
}

// (ME) draw one pending frame (0 if nothing to draw or paused). Used by me_render_loop and the mode 3 wait hook
// (mode 3) main can also draw while waiting (me_mix_main_render): one side per frame. ME sets g_rbusy, main g_rwant_main first,
// syncs, then checks the other flag (uncached; both may back off, but both never enter)
static volatile uint32_t g_rwant_main REI_ME_SHARED, g_mix_renders_main REI_ME_SHARED;
static volatile uint32_t g_mix_lagmax REI_ME_SHARED; // (mode 3) if sound lags more than this, the ME doesn't draw from the line hook (0 = always draw)
extern int rei_m107_sound_lag(void);
void me_mix_set_lagmax(uint32_t n) { REI_SH(g_mix_lagmax) = n; }
static int me_render_one(void)
{
	if (REI_SH(g_rpause) || !rei_defer_pending() || rei_defer_rendered() - REI_SH(g_rshown) >= 2)
		return 0;
	REI_SH(g_rbusy) = 1;
	REI_ME_SYNC();
	// (recheck after claiming: main may have drawn one meanwhile; otherwise we'd draw 3 ahead of shown and overwrite the displayed one,
	//  or deadlock waiting on a full draw slot)
	if (REI_SH(g_rpause) || REI_SH(g_rwant_main) || !rei_defer_pending() || rei_defer_rendered() - REI_SH(g_rshown) >= 2) {
		REI_SH(g_rbusy) = 0;
		REI_ME_SYNC();
		return 0;
	}
	meLibDcacheWritebackInvalidateAll(); // read main's frame state from memory (discard stale cache)
	const uint32_t k = rei_defer_rendered();
	if (REI_SH(g_rindex)) {
		rei_render_index = (unsigned short *)REI_SH(g_rfb[k % RFB]);
		rei_render_clut = (unsigned short *)REI_SH(g_rclut[k % RFB]);
		g_flush_buf = REI_SH(g_rfb[k % RFB]); // (index mode: write back drawn indices/palette, then drawn count +1 in me_render_flush)
		g_flush_clut = REI_SH(g_rclut[k % RFB]);
	} else {
		rei_render_index = NULL;
	}
	rei_defer_render((unsigned char *)REI_SH(g_rfb[k % RFB]), (int)REI_SH(g_rpitch)); // (me_render_flush at the end, drawn count +1)
	REI_SH(g_rframes) = REI_SH(g_rframes) + 1;
	REI_ME_SYNC();
	REI_SH(g_rbusy) = 0;
	REI_ME_SYNC();
	return 1;
}

// ---- mode 3 (M107): sound side (V35/YM2151/GA20) and drawing together on the ME ----
// The ME runs sound frames (same ring as mode 1); while sound has caught up with main and waits (core wait hook -> me_mix_poll)
// it draws pending frames one at a time. Sound stalls while drawing, so the sound->main delay is raised to 192 lines to let main run ahead
// (core s2m_delay_lines). Load/reset/save don't reboot the ME; it is parked in the wait hook (g_spause -> g_sack):
// while parked the ME writes back its cache and polls only uncached flags. When main restarts the pipeline (load/reset) the core
// drops the pending sound frame and restarts from line 0 (d_m107.cpp m107_epoch)
static volatile uint32_t g_spause REI_ME_SHARED, g_sack REI_ME_SHARED;
static volatile uint32_t g_mix_renders REI_ME_SHARED; // (diag) frames drawn in the wait hook
static volatile uint32_t g_mix_polls REI_ME_SHARED, g_mix_lines REI_ME_SHARED; // (diag) is the ME alive: wait hook / line hook call counts
// (diag, dev host0:/rei_meclock) the ME has no readable clock: a main-side thread writes µs to g_mclk and the ME reads it to sum draw/wait/
// whole-frame time (coarse resolution, averages only)
static volatile uint32_t g_mclk REI_ME_SHARED, g_mclk_on REI_ME_SHARED;
static volatile uint32_t g_mt_render REI_ME_SHARED, g_mt_wait REI_ME_SHARED, g_mt_total REI_ME_SHARED;
static uint32_t g_mt_last __attribute__((section(".rei_me_local"))); // time the previous wait hook ended (ME only)
// (me_mix_poll is below, after me_sound_me_poll)

static void me_render_loop(void)
{
	while (!REI_SH(g_quit)) {
		// YM2151 synthesis (if enabled, rei_core_ym_me_enable): flush the backlog first; main only waits at the end of the next frame.
		// Done even while drawing is paused (save/reset exchange chip state through this path)
		if (REI_SH(g_ym_me))
			while (rei_core_ym_me_poll()) { }
		if (REI_SH(g_rpause) || !rei_defer_pending() || rei_defer_rendered() - REI_SH(g_rshown) >= 2) {
			me_delay();
			continue;
		}
		REI_SH(g_rbusy) = 1;
		REI_ME_SYNC();
		if (REI_SH(g_rpause)) { // (main just asked us to pause)
			REI_SH(g_rbusy) = 0;
			REI_ME_SYNC();
			continue;
		}
		meLibDcacheWritebackInvalidateAll(); // read main's frame state from memory (discard stale cache)
		const uint32_t k = rei_defer_rendered();
		if (REI_SH(g_rindex)) {
			rei_render_index = (unsigned short *)REI_SH(g_rfb[k % RFB]);
			rei_render_clut = (unsigned short *)REI_SH(g_rclut[k % RFB]);
		} else {
			rei_render_index = NULL;
		}
		if (REI_SH(g_rindex)) { // (index mode: write back drawn indices/palette, then drawn count +1 in me_render_flush)
			g_flush_buf = REI_SH(g_rfb[k % RFB]);
			g_flush_clut = REI_SH(g_rclut[k % RFB]);
		}
		rei_defer_render((unsigned char *)REI_SH(g_rfb[k % RFB]), (int)REI_SH(g_rpitch)); // (me_render_flush at the end, drawn count +1)
		REI_SH(g_rframes) = REI_SH(g_rframes) + 1;
		{ // (diag) keep the ME busy after drawing to check ME headroom (g_rburn iterations, 0 = off)
			volatile uint32_t acc = 0;
			for (uint32_t i = REI_SH(g_rburn); i; i--)
				acc += i;
		}
		REI_ME_SYNC();
		REI_SH(g_rbusy) = 0;
		REI_ME_SYNC();
	}
}

static inline void me_delay(void)
{
	__asm__ volatile("nop; nop; nop; nop; nop; nop; nop; sync" ::: "memory");
}

// Original contents of the ME reset vector (0xbfc00000, 4KB). meLibReset overwrites it with the library handler, so on exit we restore it
// so the system boots the ME with its own boot code. The exiting ME does the write itself (g_vec_ok -> g_vec_back):
// main occasionally died with an exception (Breakpoint) writing this area, even with the ME held in reset (trial rtype Select game, 2026-10-14)
static uint32_t g_me_vector[1024];
static volatile uint32_t g_vec_ok REI_ME_SHARED, g_vec_back REI_ME_SHARED;
static volatile uint32_t g_vec_sum REI_ME_SHARED; // (diag) vector checksum re-read after the ME restored it; main compares with the copy's sum in me_trace

// entry point running on the ME (library meLibHandler -> meLibOnPreProcess -> here)
__attribute__((noinline, aligned(4))) void meLibOnProcess(void)
{
	meLibDcacheWritebackInvalidateAll();
	while (!REI_SH(g_go)) {
		const uint32_t q = REI_SH(g_breq);
		if (q != REI_SH(g_back)) { // (diag) measurement requested by main: 1 compute, 2 cached read 4MB x4, 3 uncached read 1MB
			const uint32_t kind = REI_SH(g_bkind), buf = REI_SH(g_bbuf);
			uint32_t r = 0;
			if (kind == 1) {
				r = me_bench_loop(BENCH_N);
			} else if (kind == 2) {
				meLibDcacheWritebackInvalidateAll();
				const volatile uint32_t *p = (const volatile uint32_t *)buf;
				for (int pass = 0; pass < 4; pass++)
					for (int i = 0; i < (1 << 20); i += 16)
						r += p[i];
			} else if (kind == 3) {
				const volatile uint32_t *p = (const volatile uint32_t *)(buf | 0x40000000u);
				for (int i = 0; i < (1 << 18); i += 16)
					r += p[i];
			}
			REI_SH(g_bench_sink) = r;
			REI_ME_SYNC();
			REI_SH(g_back) = q;
			REI_ME_SYNC();
		}
		me_delay();
	}
	// The main CPU has finished core init and written back to memory. Discard old cache contents and start
	meLibDcacheWritebackInvalidateAll();
	// icache too (lines from the previous run survive an ME reset: when V35 dynarec shared routines prebuilt by main land at the same heap
	// address with different contents in a new build, stale lines ran and hung; dsoccr94/firebarr hung on first run after a build, mode 3 + ME dynarec)
	meLibIcacheInvalidateAll();
	__asm__ volatile("sync" ::: "memory");
	const int len = REI_SH(g_len);
	if (REI_SH(g_mode) == 2)
		me_render_loop();
	while (REI_SH(g_mode) != 2 && !REI_SH(g_quit)) {
		const uint32_t ft0 = REI_SH(g_mclk_on) ? REI_SH(g_mclk) : 0;
		rei_core_sound_frame(g_local); // V35 256 lines (waiting on main progress) + YM2151/GA20 render
		if (ft0)
			REI_SH(g_mt_total) = REI_SH(g_mt_total) + (REI_SH(g_mclk) - ft0);
		if (REI_SH(g_quit))
			break;
		uint32_t w = REI_SH(g_w);
		if (w - REI_SH(g_r) >= RING) {
			// ring full (audio not draining): drop this frame. Waiting here would stall main, which waits on sound progress
			REI_SH(g_drops) = REI_SH(g_drops) + 1;
			continue;
		}
		int16_t *dst = REI_UNCACHED(&g_ring[w % RING][0]);
		for (int i = 0; i < len * 2; i++)
			dst[i] = g_local[i];
		REI_ME_SYNC();
		REI_SH(g_w) = w + 1;
		REI_ME_SYNC();
	}
	meLibDcacheWritebackInvalidateAll();
	// Disable interrupts and idle only here (until firmware boot resets us). Returning after halt goes to the library handler (reset vector area),
	// and every timer interrupt (~1ms) it wrote that area's handler/context (0xbfc00110), racing main's vector restore:
	// main exception/hang on exit after long xmultipl runs (regression log 8)
	__asm__ volatile("mtc0 $0, $12\n sync" ::: "memory");
	if (REI_SH(g_vec_ok)) { // the ME restores the original vector (we never return to that area's handler now)
		volatile uint32_t *v = (volatile uint32_t *)0xbfc00000;
		const volatile uint32_t *src = (const volatile uint32_t *)REI_UNCACHED(&g_me_vector[0]);
		for (int i = 0; i < 1024; i++)
			v[i] = src[i];
		REI_ME_SYNC();
		uint32_t sum = 0;
		for (int i = 0; i < 1024; i++)
			sum = sum * 31 + v[i];
		REI_SH(g_vec_sum) = sum;
		REI_SH(g_vec_back) = 1;
	}
	REI_SH(g_done) = 1;
	REI_ME_SYNC();
	for (;;)
		meLibHalt();
}

// in kernel mode (kcall): same steps as library meLibInit
static int g_me_vector_saved;

// Block system ME communication (MeRpc) while the ME runs our code. On a clock-change system event the MeRpc handler sends a request
// to ME firmware and waits for a reply; with no reply it hangs holding the comm mutex (SceMediaEngineRpc). CWCheat, launched
// alongside from the home menu, hung that way in-game, and our exit (clock restore, firmware reboot) hung waiting on that mutex (device thlist).
// Swap the handler to a pass-through on start, restore the original on exit (including dev loop stop)
static int me_rpc_pass(int id, char *name, void *param, int *result)
{
	(void)id;
	(void)name;
	(void)param;
	(void)result;
	return 0;
}

// find a function in a system module's export table (obtained user-side via kuKernelFindModuleByName) (kernel mode)
typedef struct {
	const void *top;
	unsigned size;
} ent_info;
static ent_info g_ent_mewrap, g_ent_sysmem;

static void *me_k_find(const ent_info *ei, const char *lib, uint32_t nid)
{
	const unsigned char *p = (const unsigned char *)ei->top, *end = p + ei->size;
	while (p && p < end) {
		const SceLibraryEntryTable *e = (const SceLibraryEntryTable *)p;
		if (e->len == 0)
			break;
		if (e->libname && strcmp(e->libname, lib) == 0) {
			const unsigned *t = (const unsigned *)e->entrytable;
			const int n = e->stubcount + e->vstubcount;
			for (int i = 0; i < e->stubcount; i++)
				if (t[i] == nid)
					return (void *)t[n + i];
		}
		p += e->len * 4;
	}
	return NULL;
}

static void me_find_ent(const char *mod, ent_info *ei)
{
	SceModule m;
	memset(&m, 0, sizeof(m));
	ei->top = NULL;
	ei->size = 0;
	if (kuKernelFindModuleByName((char *)mod, &m) >= 0) {
		ei->top = m.ent_top;
		ei->size = m.ent_size;
	}
}

// Reload ME firmware (for game exit). sceMeCore_driver 0x5DFF5C50 in the system ME wrapper (sceMeCodecWrapper)
// reads flash0:/kd/resource/meimg.img etc. and boots the ME (arg 1-4 = image type, confirmed by disassembly on 6.61 hardware).
// It returns immediately if the mode matches the current one, so at start we record the mode variable's location (found via lui/lw
// in the function) and value, and on exit clear the mode to 0 and call with the original mode
#define ME_BOOT_NID 0x5DFF5C50u
#define SYSEV_REFER_NID 0x68D55505u // sceSysEventForKernel sceKernelReferSysEventHandler
static int (*g_me_boot)(int);
static volatile int *g_me_mode_var;
static int g_me_mode = -1;
static PspSysEventHandler *g_me_rpc;      // system MeRpc handler
static PspSysEventHandlerFunc g_me_rpc_orig; // its original function

// (kernel mode) find the firmware boot function, its mode variable and the MeRpc handler; each step is a separate call so me_trace shows where it hung
// (hardware: device powered off at this step when relaunching after returning the ME to firmware and switching games, 2026-10-13)
// (diag) kernel function step marker (uncached): did kcall enter the function; hang at find boot (2026-10-15 23:42, 1 in 1,173)
static volatile uint32_t g_kstage REI_ME_SHARED;
// mode variable of the boot function: "lui s7, hi" and "lw a3, lo(s7)" near the function start. NULL if the shape differs
static volatile int *me_k_mode_var(const void *fn)
{
	const unsigned *c = (const unsigned *)fn;
	unsigned hi = 0;
	for (int i = 0; i < 48; i++) {
		if ((c[i] & 0xffff0000u) == 0x3c170000u)
			hi = c[i] & 0xffff;
		else if (hi && (c[i] & 0xffff0000u) == 0x8ee70000u)
			return (volatile int *)((hi << 16) + (int)(short)(c[i] & 0xffff));
	}
	return NULL;
}

// NIDs differ between firmware versions (0x5DFF5C50 is 6.61; PSP go 6.20 has another one). If the NID is missing, take the
// sceMeCore_driver export whose code has the boot function's mode-variable shape and a mode of 1..4
static void *me_k_find_boot_by_shape(const ent_info *ei)
{
	const unsigned char *p = (const unsigned char *)ei->top, *end = p + ei->size;
	while (p && p < end) {
		const SceLibraryEntryTable *e = (const SceLibraryEntryTable *)p;
		if (e->len == 0)
			break;
		if (e->libname && strcmp(e->libname, "sceMeCore_driver") == 0) {
			const unsigned *t = (const unsigned *)e->entrytable;
			const int n = e->stubcount + e->vstubcount;
			for (int i = 0; i < e->stubcount; i++) {
				const void *fn = (const void *)t[n + i];
				if ((uint32_t)fn < 0x88000000u || (uint32_t)fn >= 0x88400000u)
					continue;
				volatile int *mv = me_k_mode_var(fn);
				if (mv && (uint32_t)mv >= 0x88000000u && (uint32_t)mv < 0x88400000u && *mv >= 1 && *mv <= 4)
					return (void *)fn;
			}
		}
		p += e->len * 4;
	}
	return NULL;
}

static int g_me_boot_by_shape; // (diag) boot function found by code shape, not NID
static int me_kernel_find_boot(void)
{
	REI_SH(g_kstage) = 1;
	g_me_boot = (int (*)(int))me_k_find(&g_ent_mewrap, "sceMeCore_driver", ME_BOOT_NID);
	if (!g_me_boot) {
		g_me_boot = (int (*)(int))me_k_find_boot_by_shape(&g_ent_mewrap);
		g_me_boot_by_shape = g_me_boot != NULL;
	}
	REI_SH(g_kstage) = 2;
	return g_me_boot ? 1 + g_me_boot_by_shape : 0;
}
static int me_kernel_find_mode(void)
{
	g_me_mode_var = NULL;
	if (g_me_boot) {
		g_me_mode_var = me_k_mode_var(g_me_boot);
		if (g_me_mode_var)
			g_me_mode = *g_me_mode_var;
		else
			g_me_boot = NULL;
	}
	return g_me_mode;
}
static inline int me_k_ptr_ok(const void *p) // pointer inside kernel memory (cached)?
{
	const uint32_t a = (uint32_t)p;
	return a >= 0x88000000u && a < 0x88400000u && (a & 3) == 0;
}
// (dev loop) MeRpc handler address found by the previous run; if its name checks out, skip walking the list. Walking the list
// (sceKernelReferSysEventHandler and traversal) hung 4 times in consecutive dev loop runs (HANDOFF open, 2026-10-13..15); avoid that path
static uint32_t g_rpc_cached, g_rpc_from_cache;
static int me_kernel_find_rpc(void)
{
	g_me_rpc = NULL;
	g_rpc_from_cache = 0;
	if (g_rpc_cached && me_k_ptr_ok((const void *)g_rpc_cached)) {
		PspSysEventHandler *h = (PspSysEventHandler *)g_rpc_cached;
		if (me_k_ptr_ok(h->name) && strcmp(h->name, "SceMeRpc") == 0) {
			g_me_rpc = h;
			g_rpc_from_cache = 1;
			return 100;
		}
	}
	PspSysEventHandler *(*refer)(void) = (PspSysEventHandler * (*)(void)) me_k_find(&g_ent_sysmem, "sceSysEventForKernel", SYSEV_REFER_NID);
	int n = 0;
	for (PspSysEventHandler *h = refer ? refer() : NULL; h && n < 64; h = h->next, n++) {
		if (!me_k_ptr_ok(h) || !h->name || ((uint32_t)h->name < 0x88000000u || (uint32_t)h->name >= 0x88400000u))
			return -1 - n; // (list looks corrupt: don't follow it)
		if (strcmp(h->name, "SceMeRpc") == 0) {
			g_me_rpc = h;
			break;
		}
	}
	return n;
}

// (log build) trace ME start/stop steps: write each line immediately to host0:/me_trace.txt (sceIoWrite waits until it completes
// over USB, so the last step reaches the PC even if the device dies. printf was lost on power-off, 2026-10-11/12)
#ifdef REI_LOG_ENABLED
static void me_trace(const char *what, int v)
{
	char line[96];
	const int n = snprintf(line, sizeof(line), "%u %s %d\n", (unsigned)sceKernelGetSystemTimeLow(), what, v);
	const SceUID fd = sceIoOpen("host0:/me_trace.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0644);
	if (fd >= 0) {
		sceIoWrite(fd, line, n);
		sceIoClose(fd);
	}
}
void me_trace_set_dir(const char *dir) { (void)dir; }
#else
// (play build) append the same steps to me_trace.txt in the app folder, so the last step survives a hang without USB (2026-10-13,
// hang at loading after repeated game selection). Runs are separated by "=== start"; cleared at run start once over 64KB
static char g_trace_path[160];
static int g_trace_opened;
void me_trace_set_dir(const char *dir) { snprintf(g_trace_path, sizeof(g_trace_path), "%sme_trace.txt", dir); }
static void me_trace(const char *what, int v)
{
	if (!g_trace_path[0])
		return;
	int flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND;
	if (!g_trace_opened) {
		SceIoStat st;
		if (sceIoGetstat(g_trace_path, &st) >= 0 && st.st_size > 65536)
			flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
	}
	const SceUID fd = sceIoOpen(g_trace_path, flags, 0644);
	if (fd < 0)
		return;
	char line[112];
	int n = 0;
	if (!g_trace_opened) {
		g_trace_opened = 1;
		n = snprintf(line, sizeof(line), "=== start\n");
		sceIoWrite(fd, line, n);
	}
	n = snprintf(line, sizeof(line), "%u %s %d\n", (unsigned)sceKernelGetSystemTimeLow(), what, v);
	sceIoWrite(fd, line, n);
	sceIoClose(fd);
}
#endif
void me_sound_trace(const char *what, int v) { me_trace(what, v); (void)what; (void)v; }

volatile int me_sound_starting; // in ME start step (if the dev watchdog sees a hang now, it leaves the game running for thlist)
static uint32_t g_me_witness; // (diag) last marker read (0x88300018)
static uint32_t g_tab_cached, g_tab_from_file; // ME image type stored in file (metab.bin), and whether it was used this time
static int g_me_table;        // ME image type (supported if >= 2). Keep using the first value read correctly
static int me_kernel_restore(void);

// (kernel mode) read the ME image marker. After running our code (relaunch on load/reset) the marker changes and reads as
// an unsupported image (-6), so keep the first valid value. Cache lines are invalidated, not written back (see what the ME wrote;
// writing back could let stale main cache contents overwrite ME-side memory)
static int me_kernel_witness(void)
{
	if (g_me_table >= 2)
		return g_me_table;
	sceKernelDcacheInvalidateRange((void *)0x88300000, 64);
	g_me_witness = *(const volatile uint32_t *)0x88300018;
	g_me_table = meCoreGetTableIdFromWitnessWord();
	// The library only knows some ME images (PSP go 6.20 reads 0x279c80fc). We never call the image's functions through the table
	// (only boot, cache and reset code of our own), so any firmware ME image marker (0x279cxxxx) is accepted with the slim table
	if (g_me_table < 2 && (g_me_witness >> 16) == 0x279cu)
		g_me_table = ME_CORE_T2_IMG_TABLE;
	// After a long idle (~40s to 7min) since the previous run, the firmware ME marker reads 0x00010000. The image type is fixed
	// per device (firmware), so use the value saved to file earlier; in that case the firmware ME relaunch path (g_me_boot) never returned and the game
	// hung at the end of loading (1 in 8, rtype, 2026-10-16 me_trace last "witness reboot begin", regression log 11)
	if (g_me_table < 2 && g_tab_cached >= 2) {
		g_me_table = (int)g_tab_cached;
		g_tab_from_file = 1;
	}
	return g_me_table;
}

// ME start is split into kcalls (for tracing): 1 image type + save reset vector, 2 MeRpc pass-through, 3 ME reset
static int me_kernel_start_table(void)
{
	// read the ME image type only once: after running our code (relaunch on load/reset) the marker changes and
	// reads as an unsupported image (-6)
	if (g_me_table < 2)
		return -6; // unsupported ME image (me_kernel_witness reads it first)
	const int table = g_me_table;
	meCoreSelectSystemTable(table);
	if (!g_me_vector_saved) {
		// read while holding the firmware ME in reset (meLibReset soon releases it and starts our handler). Reading this area next to a running ME
		// powered off the device (trial airass start, me_trace last "witness", 2026-10-14; same issue as the exit-time write, regression log 8)
		*(volatile uint32_t *)0xbc10004c |= 0x04;
		__asm__ volatile("sync" ::: "memory");
		const volatile uint32_t *v = (const volatile uint32_t *)0xbfc00000;
		for (int i = 0; i < 1024; i++)
			g_me_vector[i] = v[i];
		// if a previous run exited without restoring firmware, the vector still holds the library handler: not the original, so don't save it
		const uint32_t *lib = (const uint32_t *)&__start__me_section;
		int same = 1;
		for (int i = 0; i < 16; i++)
			same &= g_me_vector[i] == lib[i];
		g_me_vector_saved = same ? 0 : 1;
	}
	return table;
}

static int me_kernel_start_rpc(void)
{
	if (g_me_rpc && g_me_rpc->handler != (PspSysEventHandlerFunc)me_rpc_pass) {
		g_me_rpc_orig = g_me_rpc->handler;
		g_me_rpc->handler = (PspSysEventHandlerFunc)me_rpc_pass;
	}
	return 0;
}

static int me_kernel_start_reset(void)
{
	meLibReset();
	return 0;
}

// (kernel mode) restore the MeRpc handler and the reset vector, then reboot ME firmware in the original mode
static void me_rpc_restore(void)
{
	if (g_me_rpc && g_me_rpc_orig) {
		g_me_rpc->handler = g_me_rpc_orig;
		g_me_rpc_orig = NULL;
	}
}
static int me_kernel_restore(void)
{
	// Restore the MeRpc handler only after the firmware ME is back up: restoring first let a CWCheat thread's clock change call MeRpc via
	// a power event and wait, holding the mutex, for a reply from the ME stuck in our loop (SceMediaEngineRpcWait); the firmware relaunch
	// then waited on that mutex (SceMediaEngineRpc) and exit hung (play_check airass after HOME, 2026-10-16 thinfo, regression log 12)
	if (!g_me_boot || !g_me_mode_var || g_me_mode < 1 || g_me_mode > 4) {
		me_rpc_restore();
		return -1;
	}
	// The system boot code in the vector is written only once at power-on; if our handler remains, the boot function waits forever
	// for an ME reply (hardware)
	if (!g_me_vector_saved) {
		me_rpc_restore();
		return -2;
	}
	// Hold the ME in reset and write the vector (0xbc10004c bit2 = ME reset, firmware boot releases it). A halted ME also
	// wakes on interrupts and writes this area (library handler, 0x110 context), so writing while it ran killed main with an exception (xmultipl, hardware)
	// (if the exiting ME already restored it, main doesn't touch this area; see g_me_vector)
	if (!REI_SH(g_vec_back)) {
		*(volatile uint32_t *)0xbc10004c |= 0x04;
		__asm__ volatile("sync" ::: "memory");
		volatile uint32_t *v = (volatile uint32_t *)0xbfc00000;
		for (int i = 0; i < 1024; i++)
			v[i] = g_me_vector[i];
	}
	REI_SH(g_vec_back) = 0;
	sceKernelDcacheWritebackInvalidateAll();
	*g_me_mode_var = 0;
	__asm__ volatile("sync" ::: "memory");
	// k1 = 0 (looking like a user call returned permission error 0x800200D1)
	unsigned k1;
	__asm__ volatile("move %0, $k1\n move $k1, $zero" : "=r"(k1));
	const int r = g_me_boot(g_me_mode);
	__asm__ volatile("move $k1, %0" ::"r"(k1));
	me_rpc_restore();
	return r;
}

// Suspend the CWCheat plugin (decided 2026-10-16: CWCheat must not run in this app). The CWCheat thread changes clocks and waits on MeRpc
// for ME replies, which deadlocked with the firmware ME relaunch at exit (regression log 12). Suspend that thread before starting the game ME
// (sceKernelSuspendThread, kernel mode) and resume it after the ME is back on firmware (on exit to XMB or when the dev loop unloads
// only the module). Suspend only when it holds nothing (no wait target = idle); suspending while waiting on an MeRpc reply leaves the mutex held
int me_sound_cw_stop; // (off by default, app folder cwstop) suspend CWCheat threads in-game
#define CW_MAX 4
static SceUID g_cw_thid[CW_MAX];
static int g_cw_n;
static ent_info g_ent_thman; // sceThreadManager exports (looked up user-side)
// Thread functions are called through sceThreadManager's ThreadManForUser exports directly with k1 = 0, not via syscalls
// (which look like user calls and fail on kernel threads); same reasoning as me_kernel_restore
#define CW_NID_IDLIST 0x94416130u // sceKernelGetThreadmanIdList
#define CW_NID_REFER 0x17C1684Eu  // sceKernelReferThreadStatus
#define CW_NID_SUSPEND 0x9944F31Fu // sceKernelSuspendThread
#define CW_NID_RESUME 0x75156E8Fu  // sceKernelResumeThread
#define CW_NID_EVF 0xA66B0120u     // sceKernelReferEventFlagStatus
static int me_kernel_cw_suspend(void)
{
	int (*idlist)(int, SceUID *, int, int *) = (int (*)(int, SceUID *, int, int *))me_k_find(&g_ent_thman, "ThreadManForUser", CW_NID_IDLIST);
	int (*refer)(SceUID, SceKernelThreadInfo *) = (int (*)(SceUID, SceKernelThreadInfo *))me_k_find(&g_ent_thman, "ThreadManForUser", CW_NID_REFER);
	int (*suspend)(SceUID) = (int (*)(SceUID))me_k_find(&g_ent_thman, "ThreadManForUser", CW_NID_SUSPEND);
	int (*evf)(SceUID, SceKernelEventFlagInfo *) = (int (*)(SceUID, SceKernelEventFlagInfo *))me_k_find(&g_ent_thman, "ThreadManForUser", CW_NID_EVF);
	if (!idlist || !refer || !suspend || !evf)
		return -2;
	unsigned k1;
	__asm__ volatile("move %0, $k1\n move $k1, $zero" : "=r"(k1));
	static SceUID ids[96];
	int n = 0, found = 0;
	if (idlist(SCE_KERNEL_TMID_Thread, ids, 96, &n) < 0)
		n = -1;
	for (int i = 0; i < n && g_cw_n < CW_MAX; i++) {
		static SceKernelThreadInfo ti;
		memset(&ti, 0, sizeof(ti));
		ti.size = sizeof(ti);
		if (refer(ids[i], &ti) < 0 || strncasecmp(ti.name, "cwcheat", 7) != 0)
			continue;
		found++;
		int already = 0;
		for (int k = 0; k < g_cw_n; k++)
			already |= g_cw_thid[k] == ids[i];
		if (already)
			continue;
		// only when idle (no wait target) or waiting for vblank (hardware: cwcheat_main normally waits on the SceDisplayVblank event flag)
		if (!(ti.status & PSP_THREAD_WAITING))
			continue;
		if (ti.waitId != 0) {
			static SceKernelEventFlagInfo ei;
			memset(&ei, 0, sizeof(ei));
			ei.size = sizeof(ei);
			if (evf(ti.waitId, &ei) < 0 || strcmp(ei.name, "SceDisplayVblank") != 0)
				continue; // (waiting on something else, possibly an MeRpc reply: next time)
		}
		if (suspend(ids[i]) >= 0)
			g_cw_thid[g_cw_n++] = ids[i];
	}
	__asm__ volatile("move $k1, %0" ::"r"(k1));
	if (n < 0)
		return -1;
	return found - g_cw_n; // 0 if all found threads were suspended (0 if none)
}
static int me_kernel_cw_resume(void)
{
	int (*resume)(SceUID) = (int (*)(SceUID))me_k_find(&g_ent_thman, "ThreadManForUser", CW_NID_RESUME);
	if (!resume)
		return -2;
	unsigned k1;
	__asm__ volatile("move %0, $k1\n move $k1, $zero" : "=r"(k1));
	for (int k = 0; k < g_cw_n; k++)
		resume(g_cw_thid[k]);
	__asm__ volatile("move $k1, %0" ::"r"(k1));
	const int n = g_cw_n;
	g_cw_n = 0;
	return n;
}

// 1: reboot function found, 2: original reset vector saved, 4: MeRpc handler switched to pass-through
int me_sound_boot_info(int *mode)
{
	if (mode)
		*mode = g_me_mode;
	return (g_me_boot != NULL) | (g_me_vector_saved ? 2 : 0) | (g_me_rpc_orig ? 4 : 0);
}

void me_sound_set_bench(int on) { REI_SH(g_bench) = on ? 1 : 0; }

// File copy of the original ME reset vector (0xbfc00000) contents. The main CPU hung several times reading this area during consecutive runs
// (right before the ME start "vector saved" step, sometimes powering off; me_trace). It's fixed content written once at power-on, so
// the first good read is saved to file and read from the file afterwards
static char g_vec_path[128];
static int g_vec_from_file;
static char g_rpc_path[128];
static char g_tab_path[128];
void me_sound_set_dir(const char *dir)
{
	snprintf(g_vec_path, sizeof(g_vec_path), "%smevec.bin", dir ? dir : "");
	snprintf(g_rpc_path, sizeof(g_rpc_path), "%smerpc.bin", dir ? dir : "");
	snprintf(g_tab_path, sizeof(g_tab_path), "%smetab.bin", dir ? dir : "");
}
static void me_tab_load(void) // (user mode: file -> g_tab_cached)
{
	g_tab_cached = 0;
	g_tab_from_file = 0;
	if (!g_tab_path[0])
		return;
	const SceUID fd = sceIoOpen(g_tab_path, PSP_O_RDONLY, 0);
	if (fd < 0)
		return;
	static uint32_t v __attribute__((aligned(64)));
	if (sceIoRead(fd, &v, 4) == 4 && v >= 2 && v < 16)
		g_tab_cached = v;
	sceIoClose(fd);
}
static void me_tab_store(void) // (only when read correctly from the marker and different from the file value)
{
	if (!g_tab_path[0] || g_tab_from_file || g_me_table < 2 || (uint32_t)g_me_table == g_tab_cached)
		return;
	static uint32_t v __attribute__((aligned(64)));
	v = (uint32_t)g_me_table;
	const SceUID fd = sceIoOpen(g_tab_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0644);
	if (fd >= 0) {
		sceIoWrite(fd, &v, 4);
		sceIoClose(fd);
	}
}
static void me_rpc_load(void) // (user mode: file -> g_rpc_cached)
{
	g_rpc_cached = 0;
	if (!g_rpc_path[0])
		return;
	const SceUID fd = sceIoOpen(g_rpc_path, PSP_O_RDONLY, 0);
	if (fd < 0)
		return;
	static uint32_t v __attribute__((aligned(64)));
	if (sceIoRead(fd, &v, 4) == 4)
		g_rpc_cached = v;
	sceIoClose(fd);
}
static void me_rpc_store(void)
{
	if (!g_rpc_path[0] || !g_me_rpc || g_rpc_from_cache)
		return;
	static uint32_t v __attribute__((aligned(64)));
	v = (uint32_t)g_me_rpc;
	const SceUID fd = sceIoOpen(g_rpc_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0644);
	if (fd >= 0) {
		sceIoWrite(fd, &v, 4);
		sceIoClose(fd);
	}
}
static void me_vec_load(void)
{
	if (!g_vec_path[0] || g_me_vector_saved)
		return;
	const SceUID fd = sceIoOpen(g_vec_path, PSP_O_RDONLY, 0);
	if (fd < 0)
		return;
	const int n = sceIoRead(fd, g_me_vector, sizeof(g_me_vector));
	sceIoClose(fd);
	if (n != (int)sizeof(g_me_vector))
		return;
	const uint32_t *lib = (const uint32_t *)&__start__me_section; // (don't save a copy that still holds our handler)
	int same = 1;
	for (int i = 0; i < 16; i++)
		same &= g_me_vector[i] == lib[i];
	if (!same) {
		g_me_vector_saved = 1;
		g_vec_from_file = 1;
	}
}
static void me_vec_store(void)
{
	if (!g_vec_path[0] || !g_me_vector_saved || g_vec_from_file)
		return;
	static uint32_t buf[1024] __attribute__((aligned(64))); // (write from the RAM buffer)
	memcpy(buf, g_me_vector, sizeof(buf));
	const SceUID fd = sceIoOpen(g_vec_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0644);
	if (fd >= 0) {
		sceIoWrite(fd, buf, sizeof(buf));
		sceIoClose(fd);
		g_vec_from_file = 1;
	}
}
int me_sound_bench_done(void) { return (int)REI_SH(g_bench_done); }
void me_sound_bench_results(unsigned out[3]) { out[0] = g_bres[0]; out[1] = g_bres[1]; out[2] = g_bres[2]; }

static int me_boot(int frame_len)
{
	REI_SH(g_go) = 0;
	REI_SH(g_quit) = 0;
	REI_SH(g_done) = 0;
	REI_SH(g_w) = 0;
	REI_SH(g_r) = 0;
	REI_SH(g_len) = frame_len;
	REI_SH(g_drops) = 0;
	REI_SH(g_bench_done) = 0;
	REI_SH(g_breq) = 0;
	REI_SH(g_back) = 0;
	REI_SH(g_vec_ok) = 0;
	REI_SH(g_vec_back) = 0;
	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();
	REI_SH(g_kstage) = 0;
	me_sound_starting = 1;
	me_trace("start", frame_len);
	const int lp = meLibLoadPrx(); // kernel bridge kcall.prx. From the second dev loop run it's already loaded, so failure is fine
	REI_LOG("me: kcall.prx %d\n", lp);
	me_trace("kcall.prx", lp);
	(void)lp;
	// (off by default) 2026-10-16 play check: this kernel call never returned at rtype start and the device powered off (app folder me_trace last "kcall.prx").
	// The exit hang itself was fixed by the MeRpc handler restore order (regression log 12). Enable: app folder cwstop
	if (me_sound_cw_stop) {
		me_find_ent("sceThreadManager", &g_ent_thman);
		int cw = kcall(me_kernel_cw_suspend, 0);
		for (int i = 0; i < 40 && cw > 0; i++) { // (wait for idle, up to 0.2s)
			sceKernelDelayThread(5000);
			cw = kcall(me_kernel_cw_suspend, 0);
		}
		me_trace("cwcheat left", cw);
		me_trace("cwcheat held", g_cw_n);
	}
	// (before changing the ME) find the firmware reboot function, current mode and MeRpc handler; once per run: system modules don't move
	// so addresses don't change. Each quick load / HOME return relaunches the ME, and redoing the kernel lookup each time crashed main with an
	// exception inside find rpc (2026-10-17 nbbatman quick load, exprint user_main KILLED, strcmp; regression log 18)
	if (g_me_boot && g_me_mode_var && g_me_rpc) {
		me_trace("finds reused", g_me_mode);
		goto found;
	}
	me_find_ent("sceMeCodecWrapper", &g_ent_mewrap);
	me_trace("ent mewrap", g_ent_mewrap.top != NULL);
	me_find_ent("sceSystemMemoryManager", &g_ent_sysmem);
	me_trace("ent sysmem", g_ent_sysmem.top != NULL);
	// (call outside me_trace: in no-log builds the me_trace args vanished, dropping the lookup entirely; play builds then had
	//  no MeRpc pass-through or firmware reboot, hanging clock restore and HOME exit, 2026-10-13 regression log 7)
	const int fb = kcall(me_kernel_find_boot, 0);
	me_trace("find boot", fb);
	const int fm = kcall(me_kernel_find_mode, 0);
	me_trace("find mode", fm);
	me_rpc_load();
	me_trace("rpc cache", g_rpc_cached != 0);
	const int fr = kcall(me_kernel_find_rpc, 0);
	me_trace("find rpc", fr);
	me_rpc_store();
	(void)fb;
	(void)fm;
	(void)fr;
found:
	REI_LOG("me: found boot %d mode %d MeRpc %d\n", g_me_boot != NULL, g_me_mode, g_me_rpc != NULL);
	me_trace("found mode", g_me_mode);
	me_vec_load();
	me_trace("vector file", g_vec_from_file);
	me_tab_load();
	int w = kcall(me_kernel_witness, 0);
	me_trace("witness", (int)g_me_witness);
	me_trace("table file", g_tab_from_file ? (int)g_tab_cached : -(int)g_tab_cached);
	me_tab_store();
	if (w < 2 && g_me_vector_saved) {
		// (2026-10-13 hardware: even after earlier runs restored firmware properly, the marker read 0x00010000 and -6 persisted. Relaunch
		//  firmware once in the original mode and read again)
		REI_LOG("me: witness %08x -> firmware reboot\n", (unsigned)g_me_witness);
		me_trace("witness reboot begin", 0);
		const int rr = kcall(me_kernel_restore, 0);
		me_trace("witness reboot", rr);
		if (rr >= 0)
			w = kcall(me_kernel_witness, 0);
		me_trace("witness again", (int)g_me_witness);
	}
	int r = kcall(me_kernel_start_table, 0);
	me_trace("table", r);
	if (r >= 0) {
		me_trace("vector saved", g_me_vector_saved);
		me_vec_store();
		kcall(me_kernel_start_rpc, 0);
		me_trace("rpc pass", g_me_rpc_orig != NULL);
		kcall(me_kernel_start_reset, 0);
		me_trace("reset done", 0);
	}
	REI_LOG("me: reset %d\n", r);
	if (r < 0)
		return r;
	if (REI_SH(g_bench)) { // (diag) request measurements one by one while the ME is in its wait loop and time them
		uint32_t *buf = (uint32_t *)memalign(64, 4 << 20);
		if (buf) {
			memset(buf, 1, 4 << 20);
			sceKernelDcacheWritebackInvalidateAll();
			for (uint32_t kind = 1; kind <= 3; kind++) {
				REI_SH(g_bkind) = kind;
				REI_SH(g_bbuf) = (uint32_t)buf;
				REI_ME_SYNC();
				const uint32_t q = REI_SH(g_breq) + 1;
				const SceInt64 t0 = sceKernelGetSystemTimeWide();
				REI_SH(g_breq) = q;
				REI_ME_SYNC();
				while (REI_SH(g_back) != q && sceKernelGetSystemTimeWide() - t0 < 3000000)
					;
				g_bres[kind - 1] = REI_SH(g_back) == q ? (unsigned)(sceKernelGetSystemTimeWide() - t0) : 0;
			}
			free(buf);
		}
		REI_SH(g_bench_done) = 1;
	}
	// write back core data (including ME side, vector copy) to memory and start the ME
	REI_SH(g_vec_ok) = g_me_vector_saved ? 1 : 0;
	sceKernelDcacheWritebackInvalidateAll();
	REI_ME_SYNC();
	REI_SH(g_go) = 1;
	REI_ME_SYNC();
	me_trace("go", 0);
	me_sound_starting = 0;
	return 0;
}

int me_sound_start(int frame_len)
{
	if (frame_len <= 0 || frame_len > MAX_LEN)
		return -1;
	REI_SH(g_mode) = 1;
	return me_boot(frame_len);
}

// other boards: once after core init with deferred drawing enabled. The ME draws into 3 fbs in turn (cached addresses, 64-byte aligned)
static void main_defer_flush(void) { sceKernelDcacheWritebackAll(); }
int me_render_start(void *const fb[3], int pitch)
{
	REI_SH(g_mode) = 2;
	for (int i = 0; i < RFB; i++)
		REI_SH(g_rfb[i]) = (uint32_t)fb[i];
	REI_SH(g_rpitch) = (uint32_t)pitch;
	REI_SH(g_rshown) = 0;
	REI_SH(g_rpause) = 0;
	REI_SH(g_rbusy) = 0;
	REI_SH(g_rframes) = 0;
	rei_render_noalloc = 1;               // (the ME can't allocate: tables used by the renderer were built by main when deferral was enabled)
	rei_render_done_hook = me_render_flush; // (called on the ME)
	rei_defer_flush_hook = main_defer_flush;
	return me_boot(1);
}

// mode 3 (M107): sound side + drawing (me_mix_poll above). Core uses the threaded pipeline; the wait hook calls me_mix_poll on the ME.
// frame_len = rei_core_audio_len() (audio ring, me_sound_pop)
int me_mix_start(void *const fb[3], int pitch, int frame_len)
{
	if (frame_len <= 0 || frame_len > MAX_LEN)
		return -1;
	REI_SH(g_mode) = 3;
	for (int i = 0; i < RFB; i++)
		REI_SH(g_rfb[i]) = (uint32_t)fb[i];
	REI_SH(g_rpitch) = (uint32_t)pitch;
	REI_SH(g_rshown) = 0;
	REI_SH(g_rpause) = 0;
	REI_SH(g_rbusy) = 0;
	REI_SH(g_rframes) = 0;
	REI_SH(g_spause) = 0;
	REI_SH(g_sack) = 0;
	REI_SH(g_mix_renders) = 0;
	REI_SH(g_ym_me) = 0;
	rei_render_noalloc = 1;
	rei_render_done_hook = me_render_done_any;
	rei_defer_flush_hook = main_defer_flush;
	REI_SH(g_rwant_main) = 0;
	REI_SH(g_mix_renders_main) = 0;
	return me_boot(frame_len);
}

void me_render_shown(uint32_t n)
{
	REI_SH(g_rshown) = n;
	REI_ME_SYNC();
}

uint32_t me_render_done(void)
{
	return rei_defer_rendered();
}

uint32_t me_render_frames(void)
{
	return REI_SH(g_rframes);
}

// around load/reset: wait for the ME to finish any drawing and stop it starting more (up to 0.5s)
int me_render_pause(int on)
{
	if (!on) {
		// (main may have written renderer buffers while paused, e.g. drivers that redraw the screen on load. Write back and invalidate so
		//  lines left in main cache don't later overwrite what the ME wrote)
		sceKernelDcacheWritebackInvalidateAll();
		REI_SH(g_rshown) = rei_defer_rendered(); // (counters went back to 0 after reset)
		REI_ME_SYNC();
		REI_SH(g_rpause) = 0;
		REI_ME_SYNC();
		return 0;
	}
	REI_SH(g_rpause) = 1;
	REI_ME_SYNC();
	for (int i = 0; i < 500 && REI_SH(g_rbusy); i++)
		sceKernelDelayThread(1000);
	return REI_SH(g_rbusy) ? -1 : 0;
}

int me_sound_stop(void)
{
	if (!REI_SH(g_go))
		return -1;
	REI_SH(g_quit) = 1;
	REI_ME_SYNC();
	rei_core_stop(); // release the sound side's wait on main
	for (int i = 0; i < 2000 && !REI_SH(g_done); i++)
		sceKernelDelayThread(1000);
	REI_SH(g_go) = 0;
	return REI_SH(g_done) != 0;
}

// prepare for game exit / dev loop stop: restore the MeRpc handler and reboot the ME into system firmware (me_kernel_restore).
// >= 0 on success
int me_sound_prepare_exit(void)
{
	me_trace("restore begin, ME done", (int)REI_SH(g_done));
	me_trace("kstage", (int)REI_SH(g_kstage));
	me_trace("vector by ME", (int)REI_SH(g_vec_back));
	if (REI_SH(g_vec_back)) { // (diag) does the vector restored by the ME match the copy (investigating firmware reboot wait hang, HANDOFF 2026-10-15)
		uint32_t sum = 0;
		const volatile uint32_t *c = (const volatile uint32_t *)REI_UNCACHED(&g_me_vector[0]);
		for (int i = 0; i < 1024; i++)
			sum = sum * 31 + c[i];
		me_trace("vector sum match", sum == REI_SH(g_vec_sum));
	}
	const int r = kcall(me_kernel_restore, 0);
	me_trace("restore", r);
	if (g_cw_n) { // (resume CWCheat after the ME is back on firmware)
		const int cw = kcall(me_kernel_cw_resume, 0);
		me_trace("cwcheat resumed", cw);
	}
	return r;
}

// save state: make the ME write back its dcache before main reads sound-side data. The ME calls me_sound_me_poll via the
// wait hook during core waits (sound side caught up with main and idle)
static volatile uint32_t g_freq REI_ME_SHARED, g_fack REI_ME_SHARED;

void me_sound_me_poll(void)
{
	const uint32_t q = REI_SH(g_freq);
	if (q != REI_SH(g_fack)) {
		meLibDcacheWritebackInvalidateAll();
		REI_ME_SYNC();
		REI_SH(g_fack) = q;
		REI_ME_SYNC();
	}
}

// (ME, mode 3) from the core wait hook while sound waits on main: cache writeback for saves, pause requests, one pending draw
void me_mix_poll(void)
{
	REI_SH(g_mix_polls) = REI_SH(g_mix_polls) + 1;
	const uint32_t mt0 = REI_SH(g_mclk_on) ? REI_SH(g_mclk) : 0;
	if (mt0 && g_mt_last && mt0 - g_mt_last < 1000) // (idle time since the previous wait hook)
		REI_SH(g_mt_wait) = REI_SH(g_mt_wait) + (mt0 - g_mt_last);
	me_sound_me_poll();
	if (REI_SH(g_spause)) {
		meLibDcacheWritebackInvalidateAll(); // sound-side data to memory (main reads or modifies it)
		REI_ME_SYNC();
		REI_SH(g_sack) = 1;
		REI_ME_SYNC();
		while (REI_SH(g_spause) && !REI_SH(g_quit))
			me_delay();
		meLibDcacheWritebackInvalidateAll(); // re-read what main changed from memory
		REI_SH(g_sack) = 0;
		REI_ME_SYNC();
		return;
	}
	if (me_render_one()) {
		REI_SH(g_mix_renders) = REI_SH(g_mix_renders) + 1;
		if (mt0)
			REI_SH(g_mt_render) = REI_SH(g_mt_render) + (REI_SH(g_mclk) - mt0);
	}
	g_mt_last = mt0 ? REI_SH(g_mclk) : 0;
}

// (main, mode 3) before (1) / after (0) load/reset/save. Before: wait until the ME parks in the wait hook and writes back its cache (up to 1s);
// main may then read and modify sound-side data. After: write back main cache, then release. 0 = success
int me_mix_pause(int on)
{
	if (!on) {
		sceKernelDcacheWritebackInvalidateAll();
		REI_SH(g_rshown) = rei_defer_rendered(); // (counters went back to 0 after reset)
		REI_ME_SYNC();
		REI_SH(g_spause) = 0;
		REI_ME_SYNC();
		return 0;
	}
	if (!REI_SH(g_go) || REI_SH(g_done))
		return -1;
	REI_SH(g_sack) = 0;
	REI_ME_SYNC();
	REI_SH(g_spause) = 1;
	REI_ME_SYNC();
	// (YM split: the ME may be waiting at frame end for main's share of YM2151; process main's share while waiting so it reaches the wait hook)
	for (int i = 0; i < 4000 && !REI_SH(g_sack); i++)
		if (!rei_core_main_poll())
			sceKernelDelayThread(250);
	if (!REI_SH(g_sack))
		return -1;
	sceKernelDcacheWritebackInvalidateAll(); // (discard stale sound-side lines in main cache)
	return 0;
}

uint32_t me_mix_renders(void) { return REI_SH(g_mix_renders); }
uint32_t me_mix_renders_main(void) { return REI_SH(g_mix_renders_main); }

// (main, mode 3) while main waits for a sound/draw slot, main draws one pending frame (when the ME overflows; firebarr). 1 = drew
int me_mix_main_render(void)
{
	if (REI_SH(g_mode) != 3 || !REI_SH(g_go) || REI_SH(g_done))
		return 0;
	if (REI_SH(g_rpause) || !rei_defer_pending() || rei_defer_rendered() - REI_SH(g_rshown) >= 2)
		return 0;
	REI_SH(g_rwant_main) = 1;
	REI_ME_SYNC();
	if (REI_SH(g_rbusy) || REI_SH(g_rpause) || !rei_defer_pending() || rei_defer_rendered() - REI_SH(g_rshown) >= 2) { // (ME is drawing / ME drew meanwhile)
		REI_SH(g_rwant_main) = 0;
		REI_ME_SYNC();
		return 0;
	}
	sceKernelDcacheWritebackInvalidateAll(); // (read renderer state/screen written by the ME from memory)
	const uint32_t k = rei_defer_rendered();
	if (REI_SH(g_rindex)) {
		rei_render_index = (unsigned short *)REI_SH(g_rfb[k % RFB]);
		rei_render_clut = (unsigned short *)REI_SH(g_rclut[k % RFB]);
	} else {
		rei_render_index = NULL;
	}
	rei_defer_render((unsigned char *)REI_SH(g_rfb[k % RFB]), (int)REI_SH(g_rpitch)); // (ends with me_render_done_any -> main cache writeback)
	REI_SH(g_mix_renders_main) = REI_SH(g_mix_renders_main) + 1;
	REI_ME_SYNC();
	REI_SH(g_rwant_main) = 0;
	REI_ME_SYNC();
	return 1;
}
void me_mix_alive(uint32_t *polls, uint32_t *lines) { *polls = REI_SH(g_mix_polls); *lines = REI_SH(g_mix_lines); }

// (ME, mode 3) machine code from the V35 dynarec: write back ME dcache and invalidate ME icache (v25.cpp rei_drc35_cache_flush)
void me_drc_flush(void *p, int len)
{
	// (whole cache, not range: ranged flush occasionally ran stale code and desynced sound; compiles are rare so full is cheap)
	(void)p;
	(void)len;
	meLibDcacheWritebackInvalidateAll();
	__asm__ volatile("sync" ::: "memory");
	meLibIcacheInvalidateAll();
	__asm__ volatile("sync" ::: "memory");
}

// (ME, mode 3) per sound line (core rei_sound_line_hook): draw pending frames before sound. Sound may lag up to 192 lines,
// but main stalls waiting for a free draw slot (drawing only in the wait hook drew after sound fully caught up, stalling main ~6ms)
void me_mix_line(void)
{
	REI_SH(g_mix_lines) = REI_SH(g_mix_lines) + 1;
	g_mt_last = 0; // (sound is running: not a wait)
	if (REI_SH(g_mix_lagmax) && rei_m107_sound_lag() > (int)REI_SH(g_mix_lagmax)) // (sound lags a lot: leave drawing to the waiting main)
		return;
	const uint32_t mt0 = REI_SH(g_mclk_on) ? REI_SH(g_mclk) : 0;
	if (me_render_one()) {
		REI_SH(g_mix_renders) = REI_SH(g_mix_renders) + 1;
		if (mt0)
			REI_SH(g_mt_render) = REI_SH(g_mt_render) + (REI_SH(g_mclk) - mt0);
	}
}

// (diag) ME time totals (µs accumulated): drawing, idle waiting on main, whole sound frame (incl. drawing/idle)
void me_mix_times(uint32_t *render, uint32_t *wait, uint32_t *total)
{
	*render = REI_SH(g_mt_render);
	*wait = REI_SH(g_mt_wait);
	*total = REI_SH(g_mt_total);
}
void me_mix_clock(uint32_t us) { REI_SH(g_mclk) = us; REI_SH(g_mclk_on) = 1; } // (main clock thread)

int me_sound_flush(void)
{
	if (!REI_SH(g_go))
		return 0; // ME not running (already written back)
	const uint32_t q = REI_SH(g_freq) + 1;
	REI_SH(g_freq) = q;
	REI_ME_SYNC();
	for (int i = 0; i < 1000 && REI_SH(g_fack) != q; i++)
		sceKernelDelayThread(1000);
	return REI_SH(g_fack) == q ? 0 : -1;
}

int me_sound_pop(int16_t *dst)
{
	uint32_t r = REI_SH(g_r);
	if (r == REI_SH(g_w))
		return 0;
	const int16_t *src = REI_UNCACHED(&g_ring[r % RING][0]);
	const int n = REI_SH(g_len) * 2;
	for (int i = 0; i < n; i++)
		dst[i] = src[i];
	REI_ME_SYNC();
	REI_SH(g_r) = r + 1;
	REI_ME_SYNC();
	return 1;
}

uint32_t me_sound_frames(void)
{
	return REI_SH(g_w);
}

uint32_t me_sound_drops(void)
{
	return REI_SH(g_drops);
}
