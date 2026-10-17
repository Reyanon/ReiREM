#include "frontend/video.h"

#include <math.h>
#include <stdio.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include "frontend/settings.h"
#include "rei_core.h"
#include "platform/log.h"
#include "platform/perf.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define NTEX     3

#define RENDER_PRIORITY   80
#define RENDER_STACK_SIZE (256 * 1024)

static vita2d_texture *s_tex[NTEX];
// Game draw buffer size and orientation (M92 320x240, other boards rei_core_screen). Vertical games show the buffer rotated
static int s_gw = REI_SCREEN_W, s_gh = REI_SCREEN_H, s_gflags;
#define DISP_W (s_gflags & REI_SCREEN_VERTICAL ? s_gh : s_gw) // visible game screen (after rotation)
#define DISP_H (s_gflags & REI_SCREEN_VERTICAL ? s_gw : s_gh)

void rei_video_set_screen(int w, int h, int flags)
{
	s_gw = w;
	s_gh = h;
	s_gflags = flags;
}
static void free_art(void);
static int s_cur; // texture to draw this time in single-thread mode

static SceUID s_render = -1;
static volatile int s_render_running;
static uint64_t s_rendered; // frames fully drawn by the render thread (updated atomically)

int rei_video_init(void)
{
	for (int i = 0; i < NTEX; i++) {
		s_tex[i] = vita2d_create_empty_texture_format(s_gw, s_gh,
			SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
		if (!s_tex[i])
			return -1;
	}
	rei_video_apply_settings();
	s_cur = 0;
	return 0;
}

// Scanlines: an 8px-wide translucent texture overlaid on the game screen.
// - CRT Soft : averages, over the span each screen line covers, a cos curve darkest at game line borders.
//              Even with one game line = 2.27 screen pixels, soft borders hide thickness differences
// - CRT Sharp: computes the position within the game line per screen line and darkens the lower half (dot filter)
// - 2/3/4px  : fixed spacing in screen pixels (independent of game lines, fully uniform)
static vita2d_texture *s_scan;
static int s_scan_h, s_scan_type = -1, s_scan_strength = -1, s_scan_dest_h;

// Integer 2x: if 2x does not fit (vertical games), the largest integer scale that fits
static int int_scale(void)
{
	int k = 2;
	while (k > 1 && (DISP_W * k > SCREEN_W || DISP_H * k > SCREEN_H))
		k--;
	return k;
}

static int dest_height(void)
{
	return g_settings.aspect == REI_ASPECT_INT2X ? DISP_H * int_scale() : SCREEN_H;
}

static void build_scanlines(void)
{
	const int type = g_settings.scan_type, strength = g_settings.scan_strength, dh = dest_height();
	if (type == s_scan_type && strength == s_scan_strength && dh == s_scan_dest_h)
		return;
	if (s_scan) {
		vita2d_wait_rendering_done(); // the GPU may still be reading
		vita2d_free_texture(s_scan);
		s_scan = NULL;
	}
	s_scan_type = type;
	s_scan_strength = strength;
	s_scan_dest_h = dh;
	if (type == REI_SCAN_OFF || (s_gflags & REI_SCREEN_VERTICAL)) // (vertical games have vertical game lines, so no overlay)
		return;

	static const float alpha[REI_SCAN_STRENGTH_COUNT] = { 70.0f, 120.0f, 175.0f };
	const float a = alpha[strength];
	const int h = dh;
	s_scan = vita2d_create_empty_texture(8, h);
	if (!s_scan)
		return;
	vita2d_texture_set_filters(s_scan, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
	s_scan_h = h;

	uint32_t *p = (uint32_t *)vita2d_texture_get_datap(s_scan);
	const unsigned stride = vita2d_texture_get_stride(s_scan) / 4;
	const float pi2 = 6.2831853f;
	for (int y = 0; y < h; y++) {
		float dark = 0.0f; // 0~1
		switch (type) {
		case REI_SCAN_CRT_SOFT: {
			// Darkness at game line position u: d(u) = (cos(2πu)+1)/2 (1 at line border, 0 at center).
			// Average over [u0,u1] covered by screen line y = (F(u1)-F(u0))/(u1-u0), F(u) = u/2 + sin(2πu)/(4π)
			const float u0 = (float)y * DISP_H / h, u1 = (float)(y + 1) * DISP_H / h;
			const float F0 = u0 * 0.5f + sinf(pi2 * u0) / (2.0f * pi2);
			const float F1 = u1 * 0.5f + sinf(pi2 * u1) / (2.0f * pi2);
			dark = (F1 - F0) / (u1 - u0);
			break;
		}
		case REI_SCAN_CRT_SHARP: {
			float pos = (y + 0.5f) * DISP_H / h;
			dark = (pos - (int)pos) >= 0.5f ? 1.0f : 0.0f;
			break;
		}
		case REI_SCAN_PX2: dark = (y % 2) == 1; break;
		case REI_SCAN_PX3: dark = (y % 3) == 2; break;
		case REI_SCAN_PX4: dark = (y % 4) == 3; break;
		}
		const uint32_t c = RGBA8(0, 0, 0, (uint8_t)(a * dark + 0.5f));
		for (int x = 0; x < 8; x++)
			p[y * stride + x] = c;
	}
}

void rei_video_apply_settings(void)
{
	SceGxmTextureFilter f = (g_settings.filter == REI_FILTER_POINT)
		? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	for (int i = 0; i < NTEX; i++)
		if (s_tex[i])
			vita2d_texture_set_filters(s_tex[i], f, f);
	build_scanlines();
}

void rei_video_fini(void)
{
	rei_video_stop_render_thread();
	vita2d_wait_rendering_done();
	for (int i = 0; i < NTEX; i++) {
		if (s_tex[i])
			vita2d_free_texture(s_tex[i]);
		s_tex[i] = NULL;
	}
	if (s_scan)
		vita2d_free_texture(s_scan);
	s_scan = NULL;
	free_art();
}

// --- Border outside the screen (Display > Border) ---
// Artwork: user-supplied full-screen background ux0:data/reirem/artwork/<game>.png (.jpg, .bmp), stretched to the screen.
// <game> = ROM set name, else the parent set name. No artwork ships with the app (copyright). Without it, solid Border Color
static vita2d_texture *s_art;

static vita2d_texture *load_art_file(const char *name)
{
	char path[160];
	snprintf(path, sizeof(path), REI_DATA_DIR "artwork/%s.png", name);
	vita2d_texture *t = vita2d_load_PNG_file(path);
	if (!t) {
		snprintf(path, sizeof(path), REI_DATA_DIR "artwork/%s.jpg", name);
		t = vita2d_load_JPEG_file(path);
	}
	if (!t) {
		snprintf(path, sizeof(path), REI_DATA_DIR "artwork/%s.bmp", name);
		t = vita2d_load_BMP_file(path);
	}
	return t;
}

void rei_video_set_art(const char *game, const char *parent)
{
	free_art();
	s_art = load_art_file(game);
	if (!s_art && parent)
		s_art = load_art_file(parent);
	if (!s_art)
		REI_LOG("video: %s artwork missing (Border Artwork falls back to solid color)", game);
}

static void free_art(void)
{
	if (s_art)
		vita2d_free_texture(s_art);
	s_art = NULL;
}

static unsigned int border_color(void)
{
	static const unsigned int c[REI_BCOL_COUNT] = {
		RGBA8(0x14, 0x18, 0x3C, 0xFF), RGBA8(0x28, 0x28, 0x2C, 0xFF), RGBA8(0x48, 0x10, 0x14, 0xFF),
		RGBA8(0x10, 0x34, 0x1C, 0xFF), RGBA8(0x2C, 0x14, 0x48, 0xFF), RGBA8(0x5A, 0x24, 0x18, 0xFF),
	};
	return c[g_settings.border_color];
}

// Fill outside the game area. Full screen has no outside
static void draw_border(void)
{
	if (g_settings.border == REI_BORDER_BLACK || g_settings.aspect == REI_ASPECT_FULL)
		return;
	vita2d_texture *art = s_art;
	if (g_settings.border == REI_BORDER_ARTWORK && art)
		vita2d_draw_texture_scale(art, 0, 0, (float)SCREEN_W / vita2d_texture_get_width(art),
			(float)SCREEN_H / vita2d_texture_get_height(art));
	else
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, border_color());
}

static void draw_scaled(vita2d_texture *tex)
{
	// Fit the visible game screen (DISP_W x DISP_H) to w x h per the aspect setting
	float w, h;
	switch (g_settings.aspect) {
	case REI_ASPECT_FULL: // stretch to full screen
		w = SCREEN_W;
		h = SCREEN_H;
		break;
	case REI_ASPECT_INT2X: // integer scale (usually 2x, 640x480)
		w = (float)DISP_W * int_scale();
		h = (float)DISP_H * int_scale();
		break;
	default: // fit height 544 and keep 4:3 (3:4 for vertical games) -> 725x544 / 408x544
		h = SCREEN_H;
		w = h * ((s_gflags & REI_SCREEN_VERTICAL) ? 3.0f / 4.0f : 4.0f / 3.0f);
		break;
	}
	const float x = (SCREEN_W - w) * 0.5f, y = (SCREEN_H - h) * 0.5f;
	draw_border();
	if (!(s_gflags & (REI_SCREEN_VERTICAL | REI_SCREEN_FLIPPED))) {
		vita2d_draw_texture_part_scale(tex, x, y, 0, 0, s_gw, s_gh, w / s_gw, h / s_gh);
	} else {
		// Rotate (same orientation as FBNeo libretro): vertical 90 deg CCW, flipped 180 deg, both 90 deg CW.
		// After rotation the texture width (buffer width) becomes screen height
		const int vert = (s_gflags & REI_SCREEN_VERTICAL) != 0;
		const float rad = vert ? ((s_gflags & REI_SCREEN_FLIPPED) ? 1.5707963f : -1.5707963f) : 3.1415927f;
		const float sx = vert ? h / s_gw : w / s_gw, sy = vert ? w / s_gh : h / s_gh;
		vita2d_draw_texture_part_scale_rotate(tex, x + w * 0.5f, y + h * 0.5f, 0, 0, s_gw, s_gh, sx, sy, rad);
	}
	if (s_scan)
		vita2d_draw_texture_scale(s_scan, x, y, w / 8.0f, h / (float)s_scan_h);
}

void *rei_video_target(int *pitch_bytes)
{
	*pitch_bytes = (int)vita2d_texture_get_stride(s_tex[s_cur]);
	return vita2d_texture_get_datap(s_tex[s_cur]);
}

void rei_video_draw(void)
{
	draw_scaled(s_tex[s_cur]);
	s_cur = (s_cur + 1) % 2;
}

void rei_video_redraw(void)
{
	draw_scaled(s_tex[(s_cur + 1) % 2]);
}

static int render_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	uint64_t k = 0;
	while (s_render_running) {
		vita2d_texture *tex = s_tex[k % NTEX];
		uint64_t t0 = rei_time_us();
		// Wait for frame k recorded by main, then draw it to the texture (including wait time)
		if (!rei_core_render_frame(vita2d_texture_get_datap(tex), (int)vita2d_texture_get_stride(tex)))
			break;
		rei_perf_record(REI_PERF_RENDER, (uint32_t)(rei_time_us() - t0));
		k++;
		__atomic_store_n(&s_rendered, k, __ATOMIC_RELEASE);
	}
	return sceKernelExitDeleteThread(0);
}

int rei_video_start_render_thread(void)
{
	s_render_running = 1;
	s_render = sceKernelCreateThread("rei_render", render_thread, RENDER_PRIORITY, RENDER_STACK_SIZE,
		0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (s_render < 0) {
		REI_LOG("video: render thread creation failed 0x%08x", s_render);
		s_render_running = 0;
		return -1;
	}
	sceKernelStartThread(s_render, 0, NULL);
	REI_LOG("video: render thread started on CPU2");
	return 0;
}

void rei_video_stop_render_thread(void)
{
	if (s_render < 0)
		return;
	s_render_running = 0;
	rei_core_stop();
	sceKernelWaitThreadEnd(s_render, NULL, NULL);
	s_render = -1;
}

void rei_video_reset_rendered(void)
{
	__atomic_store_n(&s_rendered, 0, __ATOMIC_RELEASE);
}

void rei_video_draw_rendered(uint64_t frame_n)
{
	if (frame_n == 0)
		return;
	// Wait until frame N-1 (= render sequence N-1) finishes. Usually already done
	while (__atomic_load_n(&s_rendered, __ATOMIC_ACQUIRE) < frame_n && s_render_running)
		;
	draw_scaled(s_tex[(frame_n - 1) % NTEX]);
}

uint32_t rei_video_color(int r, int g, int b)
{
	return ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (uint32_t)(b >> 3);
}
