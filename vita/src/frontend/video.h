#ifndef REI_FRONTEND_VIDEO_H
#define REI_FRONTEND_VIDEO_H

#include <stdint.h>

// Game screen texture (RGB565, 320x240). The renderer draws directly into texture memory (docs/RULES.md ch.3)
//
// Render thread mode (ADR-007): the CPU2 thread draws frame k into texture k%3, and main
// emulates frame N then shows the finished frame N-1 (display latency +1 frame).
// Single-thread mode: alternates two textures, and the core draws directly inside run_frame.

// Game draw buffer size and orientation (rei_core_screen values, REI_SCREEN_VERTICAL/FLIPPED). Before rei_video_init. Default 320x240
void rei_video_set_screen(int w, int h, int flags);
int rei_video_init(void);
void rei_video_fini(void);

// --- single-thread mode ---
// Texture memory and stride (bytes) the core draws into this frame
void *rei_video_target(int *pitch_bytes);
// Draw the just-rendered texture and advance to the next (between start_drawing and end_drawing)
void rei_video_draw(void);
// Redraw the last rendered texture (menu background while paused)
void rei_video_redraw(void);

// --- render thread mode ---
int rei_video_start_render_thread(void);
void rei_video_stop_render_thread(void);
// Call after main finishes frame N: waits until frame N-1 is drawn, then draws that texture
// (between start_drawing and end_drawing). Draws nothing if N == 0.
void rei_video_draw_rendered(uint64_t frame_n);
// Call after a game reset before restarting the render thread (frame numbers restart at 0)
void rei_video_reset_rendered(void);

// Apply option changes (aspect ratio, filter) to the textures
void rei_video_apply_settings(void);
// Outside-screen image (Border = Artwork): user artwork/<game>.png/.jpg/.bmp (else parent set name). Solid color if none
void rei_video_set_art(const char *game, const char *parent);

// Core color conversion callback (r,g,b 0-255 -> RGB565)
uint32_t rei_video_color(int r, int g, int b);

#endif
