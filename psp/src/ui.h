// Drawing layer for the menus, game list, loading screen, notices and the FPS box (PSP port of the Vita vita/src/frontend/ui.h,
// same calls and colours at half the size). The Vita draws to a CPU canvas; here every call goes straight into the GE list that
// video_present builds (only inside the overlay callback, video_set_overlay), so a screen is drawn again in full every frame.
// Text is the system font (flash0:/font/ltn0.pgf via intraFont), like the Vita's system PGF font.
#ifndef REI_PSP_UI_H
#define REI_PSP_UI_H

#include <stdint.h>

// colour 0xAABBGGRR (the GE's own order)
#define UI_RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

void ui_init(void); // the font (once, before the first draw)
// around the drawing of one screen, in the overlay callback (GE state for these calls, then back to what the game drawing expects)
void ui_begin(void);
void ui_end(void);

void ui_rect(int x, int y, int w, int h, uint32_t col);
void ui_grad(int x, int y, int w, int h, uint32_t top, uint32_t bot); // vertical gradient
void ui_rrect(int x, int y, int w, int h, int r, uint32_t col);       // rounded rectangle (row spans, no overlap)
void ui_rrect_outline(int x, int y, int w, int h, int r, int t, uint32_t col);
void ui_circle(float cx, float cy, float r, uint32_t col);            // edges softened
void ui_line(float x0, float y0, float x1, float y1, float width, uint32_t col);
void ui_star(float cx, float cy, float r, uint32_t col);              // five points (favourites)
int ui_toggle(int x, float cy, int on, uint32_t track, uint32_t knob); // on/off switch from x, centred on cy; returns its width
// window with slightly rounded corners, an outline and a soft drop shadow (Vita window_box)
void ui_window(int x, int y, int w, int h, uint32_t edge, uint32_t fill);
// a 16-bit 5650 picture (main memory, stride in pixels) stretched over the rectangle, colours times tint
void ui_image565(const uint16_t *px, int tw, int th, int stride, int x, int y, int w, int h, uint32_t tint);

// text: y = baseline. scale 1.0 = the Vita's menu text size (drawn at half size). Returns the width
int ui_text(int x, int y, uint32_t col, float scale, const char *s);
int ui_text_width(float scale, const char *s);
// only within the columns [cx0, cx1) (scrolling and clipped titles)
int ui_text_clip(int x, int y, uint32_t col, float scale, const char *s, int cx0, int cx1);
// drawing limited to rows [y0, y1) (ui_clip_y(0, 272) = whole screen)
void ui_clip_y(int y0, int y1);

#endif
