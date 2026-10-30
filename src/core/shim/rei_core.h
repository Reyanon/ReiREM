#ifndef REI_CORE_H
#define REI_CORE_H

// Frontend C API of the ReiREM emulation core.
// Wraps the FBNeo burn library (Irem board drivers) (burn_shim.cpp).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REI_SCREEN_W 320
#define REI_SCREEN_H 240

// Logical inputs. Matched to driver inputs by name ("P1 Coin", "P1 Button 3" ...) (list order differs per game)
enum {
	REI_IN_P1_COIN, REI_IN_P1_START, REI_IN_P1_UP, REI_IN_P1_DOWN,
	REI_IN_P1_LEFT, REI_IN_P1_RIGHT, REI_IN_P1_B1, REI_IN_P1_B2,
	REI_IN_P2_COIN, REI_IN_P2_START, REI_IN_P2_UP, REI_IN_P2_DOWN,
	REI_IN_P2_LEFT, REI_IN_P2_RIGHT, REI_IN_P2_B1, REI_IN_P2_B2,
	REI_IN_P3_COIN, REI_IN_P3_START, REI_IN_P3_UP, REI_IN_P3_DOWN,
	REI_IN_P3_LEFT, REI_IN_P3_RIGHT, REI_IN_P3_B1, REI_IN_P3_B2,
	REI_IN_P4_COIN, REI_IN_P4_START, REI_IN_P4_UP, REI_IN_P4_DOWN,
	REI_IN_P4_LEFT, REI_IN_P4_RIGHT, REI_IN_P4_B1, REI_IN_P4_B2,
	REI_IN_RESET, REI_IN_SERVICE,
	REI_IN_P1_B3, REI_IN_P1_B4, REI_IN_P1_B5, REI_IN_P1_B6, // games with 3+ buttons (Perfect Soldiers has 6)
	REI_IN_P2_B3, REI_IN_P2_B4, REI_IN_P2_B5, REI_IN_P2_B6,
	REI_IN_DIGITAL_COUNT
};
// 1P button count of the current game (largest N of "P1 Button N" in the driver list, max 6)
int rei_core_button_count(void);

// Color conversion callback: r,g,b 0-255 -> frontend pixel value
typedef uint32_t (*rei_color_fn)(int r, int g, int b);

// ge_layers mode (PSP GE, psp/docs/M92_VIDEO.md G2): tile layer state for one raster segment.
// Registers as used by FBNeo draw_layer_byline; vram/rowscroll point to the VRAM copy at that segment
typedef struct {
	int enable, wide, rowscroll, scrollx, scrolly;
	const uint16_t *vram;      // 2 words per cell (code, attr), wide x 64 cells
	const uint16_t *scroll;    // row scroll table, 512 entries
} rei_layer_regs;
typedef struct {
	int start, finish;         // screen lines [start, finish)
	int pf2_off;               // 1 = fill with palette color 0 instead of pf2
	int layer_mask;            // FBNeo nBurnLayer (usually 0xf)
	int vram_changed;          // 1 if VRAM was written since the previous segment (1 for the first)
	rei_layer_regs l[3];
} rei_layer_seg;
typedef void (*rei_layer_fn)(const rei_layer_seg *seg, void *user);

typedef struct {
	const char *zip_path;   // ROM zip (set of set_name)
	int sample_rate;        // 0 = no sound
	int bytes_per_pixel;    // 2 or 4
	rei_color_fn color;     // NULL = 0x00RRGGBB
	void (*log)(const char *msg); // NULL = stderr
	uint64_t (*clock_us)(void);   // section profiler time source (REI_PROFILE builds only, may be NULL)
	int no_idle_skip;             // 1 = disable idle loop skipping (for comparison/verification)
	int interleave_mul;           // CPU slices per scanline. 0 = FBNeo default 8 (experimental)
	int pipelined;                // 1 = ADR-003 pipelined mode (hash differs from original)
	int threaded;                 // with pipelined, 1 = run the sound side on its own thread
	void (*wait_hook)(void);      // called while a thread spins waiting for the other (NULL = pure spin)
	int render_threaded;          // with pipelined, 1 = drawing done by the render thread (ADR-007)
	const char *set_name;         // "nbbatman" (World, default/NULL) or "nbbatmanu" (US, clone)
	const char *parent_zip_path;  // for clone sets, parent nbbatman.zip for ROMs missing from the zip (may be NULL)
	int index_out;                // pipelined single render: 1 = skip palette conversion -> rei_core_index_frame()
	int ge_layers;                // with index_out, 1 = frontend also draws tile layers (layer_cb, sprite_frame)
	rei_layer_fn layer_cb;        // ge_layers: called per segment (right after applying VRAM)
	void *layer_cb_user;
	int ge_sprites;               // with ge_layers + render_threaded, 1 = frontend also draws sprites (sprite_tiles)
	int s2m_delay_lines;          // pipelined: sound->main delay in lines (0 = default 32, 16/64/128). Changes results (hash)
	int ym_main_channels;         // threaded + REI_YM_SPLIT build: YM2151 channel bits synthesized on main (0 = all on sound side). Same results
	void (*progress)(int permille, void *user); // load progress 0-900 (ROM reading, same thread inside rei_core_init). May be NULL
	void *progress_user;
} rei_core_config;

// 0 = success. On failure see rei_core_error() (nothing to release then: do not call rei_core_exit). After
// rei_core_exit another game can be started with rei_core_init in the same process (in-app game switching)
int rei_core_init(const rei_core_config *cfg);
void rei_core_exit(void);
const char *rei_core_error(void);

// game name of the running set (e.g. "Ninja Baseball Bat Man (US)")
const char *rei_core_game_name(void);
// ROM set name ("nbbatman" / "nbbatmanu")
const char *rei_core_set_name(void);

// supported game list (usable before rei_core_init). set = ROM set name (zip name), parent = parent set name for clones (else NULL)
int rei_core_game_count(void);
const char *rei_core_game_set(int i);
const char *rei_core_game_parent(int i);
const char *rei_core_game_title(int i);
// board name ("M92", "M72", "M62" ...): game list tab
const char *rei_core_game_board(int i);
// current game (after rei_core_init). Non-M92 boards ignore pipeline/thread/GE settings and run the stock FBNeo frame:
// rei_core_run_frame writes the frame to fb (rei_core_screen size, bytes_per_pixel) and sound to audio
const char *rei_core_board(void);
int rei_core_is_m92(void);
// draw buffer size and orientation (vertical games draw into an unrotated landscape buffer, rotated 90 degrees for display)
#define REI_SCREEN_VERTICAL 1   // display rotated 90 degrees clockwise (FBNeo BDF_ORIENTATION_VERTICAL)
#define REI_SCREEN_FLIPPED  2   // 180 degrees (BDF_ORIENTATION_FLIPPED)
void rei_core_screen(int *w, int *h, int *flags);
// does the current game have cheats (start stage, infinite health) (only games with known RAM addresses: nbbatman family)
int rei_core_has_cheats(void);
// user cheat file (FBNeo cheat .ini format, docs/CHEATS.md): <set>.ini in dir (ending in '/'), else the parent set .ini. Call after init.
// returns cheats read. Enabled cheats are written to memory at the end of each rei_core_run_frame
int rei_core_cheats_load(const char *dir);
int rei_core_cheat_count(void);
const char *rei_core_cheat_name(int i);
int rei_core_cheat_options(int i);               // option count (0 = usually "Disabled")
const char *rei_core_cheat_option_name(int i, int o);
int rei_core_cheat_get(int i);
void rei_core_cheat_set(int i, int o);

void rei_core_set_input(int index, int pressed);

// DIP switches (groups in driver definition order, emulator HACK entries excluded). Main thread only
int rei_core_dip_count(void);
const char *rei_core_dip_name(int group);
int rei_core_dip_options(int group);
const char *rei_core_dip_option_name(int group, int option);
int rei_core_dip_get(int group);              // current option index, -1 if unknown
void rei_core_dip_set(int group, int option);
void rei_core_dip_defaults(void);
// DIP bytes for the settings file. Returns byte count
int rei_core_dip_raw(uint8_t *out, int max);
void rei_core_dip_set_raw(const uint8_t *in, int count);

// V33 address space read/write (main thread, between frames). Used for stage select etc.
uint8_t rei_core_peek(uint32_t addr);
void rei_core_poke(uint32_t addr, uint8_t value);

// start stage (0 = Stage 1 Seattle .. 6 = Final Stage New York). Applied at every new game start.
// 0 = leave the game untouched
#define REI_STAGE_COUNT 7
void rei_core_set_start_stage(int stage);
// 1P infinite health (keeps health at max during play; hits knock you down but cost no life)
void rei_core_set_infinite_health(int on);

// save states (between frames, main thread). 0 = success.
// Save: no need to stop threads once the sound side has caught up with main (while the options screen is open).
// Load: like reset, stop sound/render threads, call, then restart them. States from other ROM sets are rejected
int rei_core_state_save(const char *path);
int rei_core_state_load(const char *path);
int rei_core_state_exists(const char *path);
// is it safe to save (sound side has caught up with main and is idle)
int rei_core_pipe_synced(void);
// diag: lines emitted by main/sound, line the sound side waits for (-1 = none)
void rei_core_pipe_lines(long long *main_done, long long *sound_done, long long *sound_waiting);

// game reset. In threaded mode stop sound/render threads, call on the main thread, then restart them
void rei_core_reset(void);

// deterministic auto input (for measurement/regression). frame = emulated frame number from 0
void rei_core_autoplay(int frame);
// character autoplay picks (1 JOSE, 2 RYNO, 3 ROGER, 4 STRAW). 0 = original input (right held, so the last one, STRAW;
// baseline hashes use this). 1-4: on the select screen (1200+) tap right (n-1) times and pick with attack
void rei_core_set_autoplay_char(int ch);

// run one frame. fb NULL = no drawing. audio is interleaved stereo INT16,
// rei_core_audio_len() samples per frame. NULL = skip sound rendering.
void rei_core_run_frame(void *fb, int pitch_bytes, int16_t *audio);
int rei_core_audio_len(void);

// index_out mode: 320x240 palette index buffer of the last drawn frame (0-0x800, 0x800 = display off) and
// palette (0x801 entries, color callback format). *blank = 1 for a fully blanked frame. NULL if none yet
const uint16_t *rei_core_index_frame(const uint32_t **pal, int *blank);

// ge_layers mode
// 320x240 sprite-only buffer: 0 = empty, else 0x800 | palette index | (0x8000 if hidden behind high tiles).
// "first drawn sprite wins" is already applied
const uint16_t *rei_core_sprite_frame(void);
// ge_sprites mode: 16x16 sprite tiles of the last drawn frame in core draw order (off-screen omitted).
// Drawing back to front leaves earlier sprites on top, matching sprite_frame.
// Pixels at gfx + code x 256 (1 byte/pixel 0-15, 0 transparent). Value = 0x800 | color x 16 | pixel | (hidden ? 0x8000 : 0)
typedef struct {
	int16_t x, y;      // top-left (game screen coords)
	uint32_t code;     // tile number
	uint8_t color;     // 0~127
	uint8_t flags;     // bit0 flipx, bit1 flipy, bit2 hidden behind high tiles (0x8000 mark)
	uint16_t pad;
} rei_spr_tile;
const rei_spr_tile *rei_core_sprite_tiles(int *count, int *overflow, const uint8_t **gfx);
// tile graphics (8x8, 1 byte/pixel 0-15, 64 bytes/tile) and screen offset
typedef struct { const uint8_t *tiles; int tile_mask; int ofs_x, ofs_y; } rei_tile_info;
void rei_core_tile_info(rei_tile_info *out);
// 1 = CPU also draws the same frame the original way into rei_core_index_frame() (verification/hash, slow)
void rei_core_set_ge_reference(int on);

// threaded mode: called repeatedly on the sound thread. Runs one frame of V35/YM2151/GA20 and
// mixes rei_core_audio_len() samples into audio. Waits by itself so it never runs ahead of main.
// The audio argument of rei_core_run_frame() is then unused.
void rei_core_sound_frame(int16_t *audio);

// render_threaded mode: called repeatedly on the render thread. Waits for the next frame main recorded and
// draws it to fb (fb argument of rei_core_run_frame unused). Returns 0 when stopped.
int rei_core_render_frame(void *fb, int pitch_bytes);
// after init: should a render thread be started (M92 follows the render_threaded request; other boards 1 only if the driver
// supports deferred drawing, else run_frame draws straight to fb)
int rei_core_render_threaded(void);
// after init: is the main/sound pipeline used, should the sound side run threaded (M92 follows the request; other boards only
// where supported, currently M107, ADR-019)
int rei_core_pipelined(void);
int rei_core_sound_threaded(void);
// YM2151 synthesis worker thread (Vita/harness, other boards): 1 if available. After enabling, call run on a thread (until rei_core_stop)
int rei_core_ym_worker(void);
void rei_core_ym_worker_enable(int on);
void rei_core_ym_worker_run(void);
// M72-family boards without MCU: Z80 worker thread instead (same results; asks, enables, runs until rei_core_stop)
int rei_core_z80_worker(void);
void rei_core_z80_worker_enable(int on);
void rei_core_z80_worker_run(void);
// (PSP other boards) YM2151 synthesis on the ME: enable before starting the ME, disable after it stops. The ME loops rei_core_ym_me_poll.
// When enabled, audio from rei_core_run_frame is one frame late (same results). 0 = success
int rei_core_ym_me_supported(void);
int rei_core_ym_me_enable(int on);
int rei_core_ym_me_poll(void);
int rei_core_palette_entries(void); // driver palette entries
int rei_core_trans_width(void);     // width of bitmap 0 (pTransDraw)
void rei_core_ym_me_release(void); // main synthesizes from now on (when the ME overloads, between frames)
int rei_core_ym_me_split(int mask); // split synthesis: mask channels on main, rest on ME (when the ME overloads, between frames). 0 = success
int rei_core_ym_me_split_mask(void); // channels handled by main (0 = all ME, -1 = not YM-on-ME)

// diag: total time main waited on sound/render threads (REI_PROFILE builds only)
void rei_core_waits(uint64_t *sound_us, uint64_t *render_us);
// render_threaded + ge_layers: draw one piece of the oldest recorded frame (called while main waits on sound).
// -1 no pending frame, 1 drew a piece (more left), 0 this call finished the frame (index_frame/sprite_frame now show it)
int rei_core_render_step(void);
// threaded mode: called while the sound side waits on main (sound thread / ME). Applies one deferred YM2151 register
// write (REI_YM_LAZY build, same results). 1 if work was done
int rei_core_sound_idle(void);
// with ym_main_channels: advance the main share of YM2151 (the core already calls it per main line and while waiting; if the frontend
// works long outside emulation, calling it in between makes the sound side wait less). 1 if work was done
int rei_core_main_poll(void);

// release all waiting threads (on shutdown)
void rei_core_stop(void);

// fetch and reset section profile totals (us). Order follows REI_PROF_* in rei_prof.h.
// Without REI_PROFILE returns 0 and leaves out untouched; otherwise returns the section count.
int rei_core_prof_take(uint64_t *out, int max);
const char *rei_core_prof_name(int id);

// REI_PCHIST build: write the top cycle-weighted PCs per CPU to a file (analysis). Else -1
int rei_core_pchist_dump(const char *path, int top);
void rei_core_pchist_reset(void); // REI_PCHIST builds only
// M6 recompiler input (REI_PCHIST build): V33 coverage text and 0x00000-0x9ffff image
int rei_core_cover_dump(const char *cover_path, const char *image_path);

// idle loop skip stats (cpu 0 = V33, 1 = V35)
void rei_core_idle_stats(int cpu, unsigned long long *visits, unsigned long long *skipped, int *last_d);
// V33 sprite chunk loop fast path: native iterations, learned (path, prefetch) pairs, mismatched measurements
void rei_core_piece_stats(unsigned *native, unsigned *learned, unsigned *bad);
// copy V33 read-only (code) pages to buf and repoint the page table there (PSP: scratchpad). Returns pages moved
int rei_core_v33_page_cache(unsigned char *buf, int size, const unsigned int *pages, int n);
// default list for the above (hot code pages of this game, most used first)
const unsigned int *rei_core_v33_hot_pages(int *n);
// V33 empty-slot loop per-instruction handling: native insns, learned costs, mismatches
void rei_core_scan_stats(unsigned *native, unsigned *learned, unsigned *bad);
// V33 player check function (0x3ef04) fast path: native calls, learned costs, mismatches
void rei_core_pcheck_stats(unsigned *native, unsigned *learned, unsigned *bad);
// V33 task dispatch loop (0x2974) fast path: native slices, learned costs, mismatches
void rei_core_task_stats(unsigned *native, unsigned *learned, unsigned *bad);

#ifdef __cplusplus
}
#endif

#endif
