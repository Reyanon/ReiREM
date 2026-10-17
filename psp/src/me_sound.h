// Runs the sound side (V35, YM2151, GA20) on the Media Engine (psp/docs/DECISIONS.md PSP-ADR-003).
// Core in threaded pipeline mode: main CPU loops rei_core_run_frame(), ME loops rei_core_sound_frame().
#ifndef REI_PSP_ME_SOUND_H
#define REI_PSP_ME_SOUND_H

#include <stdint.h>

// Once after core init. Starts the ME and begins running sound frames. frame_len = rei_core_audio_len(). 0 = success
int me_sound_start(int frame_len);
// Folder for the ME reset vector copy (mevec.bin) (before me_sound_start): once read, hardware is not read again
void me_sound_set_dir(const char *dir);
void me_sound_set_bench(int on);   // diag: ME compute loop at start (me_sound_start waits for it)
int me_sound_bench_done(void);
void me_sound_bench_results(unsigned out[3]); // ME compute, cached read, uncached read us
uint32_t me_bench_loop(uint32_t n);
// Stops the ME and waits for it (also calls rei_core_stop()). The ME stays halted afterwards
int me_sound_stop(void); // 1 = ME left its loop and halted, 0 = not done within 2 s, -1 = not started
// Right before sceKernelExitGame (after me_sound_stop). The system ME event handler (MeRpc) waits for an ME reply
// on game exit, but the ME is halted in our code and never answers (exit hung). Replace it with a handler that returns at once
void me_trace_set_dir(const char *dir); // (play build) send me_sound_trace to me_trace.txt in this folder
void me_sound_trace(const char *what, int v); // (log build) write one line straight to host0:/me_trace.txt
int me_sound_prepare_exit(void); // reboot the ME into system firmware (sceMeCore 0x5DFF5C50). >= 0 = success
int me_sound_boot_info(int *mode); // 1 if the firmware reboot function was found, mode = firmware mode at start
// For save states: make the ME write back its data cache and wait (max 1 s, 0 = success). Call while the sound side
// has caught up with main and is idle (rei_core_pipe_synced). The ME calls me_sound_me_poll from the core wait hook
int me_sound_flush(void);
void me_sound_me_poll(void);
// Main CPU: pops one audio frame made by the ME (stereo, frame_len samples). 0 if none
int me_sound_pop(int16_t *dst);
// diag: frames produced by the ME
uint32_t me_sound_frames(void);
// diag: frames dropped because the ring was full
uint32_t me_sound_drops(void);

// Other boards (PSP-ADR-013): deferred-draw rendering on the ME. Once after core init (render_threaded, deferral on); draws into
// 3 fbs in turn (frames drawn % 3). Stop/restore via me_sound_stop / me_sound_prepare_exit as before
int me_render_start(void *const fb[3], int pitch);
void me_render_set_burn(uint32_t n); // (diag) keep the ME busy n loops after drawing (headroom check)
void me_render_set_ym(int on);
// Before me_render_start: index mode (ME draws color indices into the frame buffer and the palette into clut[k] as 16-bit; GE applies colors)
void me_render_set_index(int on, void *const clut[3]);
void me_render_set_index_bytes(uint32_t bytes); // bytes per index buffer // before me_render_start: YM2151 synthesis on the ME too (when rei_core_ym_me_enable succeeded)
void me_render_shown(uint32_t n);  // main: frames shown (the ME never draws more than 2 ahead)
uint32_t me_render_done(void);     // frames finished (latest = fb[(n - 1) % 3])
uint32_t me_render_frames(void);   // diag: frames drawn by the ME
int me_render_pause(int on);       // before load/reset (1, waits if drawing), after (0)

// Mode 3 (M107, experimental): sound side (V35, YM2151, GA20) and drawing together on the ME. Core in threaded pipeline (sound->main 192 lines),
// the core wait hook calls me_mix_poll on the ME (one pending draw, save cache, stop). Audio via me_sound_pop
int me_mix_start(void *const fb[3], int pitch, int frame_len);
void me_mix_poll(void);            // (ME) from the wait hook
void me_mix_line(void);            // (ME) per sound line (core rei_sound_line_hook): pending draw first
int me_mix_pause(int on);          // (main) before load/reset/save (1: park the ME in the wait hook and write back cache), after (0). 0 = success
uint32_t me_mix_renders(void);     // diag: frames drawn in the wait hook
void me_mix_times(uint32_t *render, uint32_t *wait, uint32_t *total); // diag (accumulated us, only while me_mix_clock runs)
void me_mix_clock(uint32_t us);    // diag: main clock thread feeds the time to the ME
void me_drc_flush(void *p, int len); // (ME) flush V35 dynarec code cache (mode 3)
int me_mix_main_render(void);      // (main, mode 3) one pending draw while waiting (1 = drew)
void me_mix_set_lagmax(uint32_t n);  // (mode 3) ME skips drawing in the line hook when sound lags > n lines (0 = off)
uint32_t me_mix_renders_main(void); // diag: frames drawn by main
void me_mix_alive(uint32_t *polls, uint32_t *lines); // diag: ME wait hook / line hook call counts

#endif
