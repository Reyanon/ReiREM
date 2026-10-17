// Deferred drawing (ADR-018): hands screen drawing of non-M92 boards to the render thread.
//
// Drivers register with rei_defer_register in init. When deferral is on (rei_defer_on), main does not draw;
// on each draw it captures draw state via rei_defer_segment, and calls rei_defer_frame at frame end.
// The render thread (rei_core_render_frame) makes the draw functions read the captured state (apply) and redraws with the original functions.
// Results match direct drawing on main (verified by harness hash). When off, drivers draw directly as before.
#ifndef REI_DEFER_H
#define REI_DEFER_H

#include <stddef.h>

// Globals written only by the render side (main does not write them while deferral is on). PSP runs the render on the ME, so these
// are grouped into one section (.rei_rdr, gathered on a 64-byte boundary by psp/me.ld) to avoid sharing cache lines with main's variables.
// The list is found with harness --render-inline + REI_RENDER_WRITES (global addresses written by one render)
#ifdef REI_ME_SPLIT
#define REI_RDR __attribute__((section(".rei_rdr_data")))
#else
#define REI_RDR
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	size_t seg_size;                     // size of one state
	void (*capture)(void *seg);          // main: copy state needed for drawing (VRAM, registers, palette ...) into seg
	void (*apply)(const void *seg);      // render: make the draw functions read seg
	void (*draw)(int start, int finish); // render: draw screen lines [start, finish) (original function)
	void (*frame_begin)(void);           // render: frame start (may be NULL, usually BurnTransferClear)
	void (*frame_end)(void);             // render: frame end (usually BurnTransferCopy). After apply has set the frame-end state
	void (*prepare)(void);               // main: when enabling deferral (may be NULL). Builds tables the render would build on first use (PSP ME render cannot allocate)
	void (*capture_end)(void *seg);      // main: frame-end state (NULL = capture). Only what frame_end reads; avoids copying all VRAM every frame
} rei_defer_ops;

// Large copies of segment state (VRAM copy). The platform may use a fast path (PSP: DMA) when dst is 64-byte aligned and n a multiple of 64.
// Default is memcpy. State buffers are 64-byte aligned, so the first field of a state struct satisfies this
extern void (*rei_big_copy)(void *dst, const void *src, size_t n);

// 1 = the render side must not allocate memory (PSP: render runs on the ME). Without the tables the render draws without them
extern int rei_render_noalloc;
// (PSP) When main has captured the frame state: write back the data cache so the render (ME) reads from memory. Not called if NULL
extern void (*rei_defer_flush_hook)(void);
// (PSP ME) Right before the render signals a finished frame: write back the ME data cache to memory. Not called if NULL
extern void (*rei_render_done_hook)(void);

// (PSP render side) If not NULL, the next rei_defer_render draws bitmap 0 (pTransDraw) into this index buffer (width nTransWidth, incl. 16 overflow lines)
// and instead of the screen transfer copies the palette (up to 0x800) to rei_render_clut as 16-bit (the GE colors it via CLUT)
extern unsigned short *rei_render_index, *rei_render_clut;
extern int rei_xfer_clut_n; // (palette count for that frame)

// In driver init
void rei_defer_register(const rei_defer_ops *ops);

// 1 = deferring: main does not draw (pBurnDraw also belongs to the render thread, so main does not read it)
extern int rei_defer_on;
// Main: when screen lines [start, finish) must be drawn with the current state
void rei_defer_segment(int start, int finish);
// Main: frame end (where DrvDraw used to call BurnTransferCopy)
void rei_defer_frame(void);

// --- Inside the core (burn_shim.cpp) ---
int rei_defer_registered(void);
void rei_defer_unregister(void);
int rei_defer_enable(void);               // 0 on success (allocates buffers)
void rei_defer_disable(void);
void rei_defer_reset(void);               // after reset/load (with the render thread stopped)
int rei_defer_render(unsigned char *fb, int pitch); // render thread: next frame. 0 = stop requested
int rei_defer_pending(void);              // a frame is ready to draw (PSP ME render: only when calling without waiting)
unsigned rei_defer_rendered(void);        // frames fully drawn by the render

#ifdef __cplusplus
}
#endif

#endif
