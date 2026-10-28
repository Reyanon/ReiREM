#include "frontend/video.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include "frontend/menu.h"
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
// quarter turns clockwise the buffer is shown with: the game's monitor (vertical, flipped) and Display > Rotate
static int s_rot;
int rei_input_rot;
#define DISP_W (s_rot & 1 ? s_gh : s_gw) // visible game screen (after rotation)
#define DISP_H (s_rot & 1 ? s_gw : s_gh)
static void update_rotation(void)
{
	const int vert = (s_gflags & REI_SCREEN_VERTICAL) != 0, flip = (s_gflags & REI_SCREEN_FLIPPED) != 0;
	// same orientation as FBNeo libretro: vertical 90 degrees counter-clockwise, flipped 180, both 90 clockwise
	const int base = vert ? (flip ? 1 : 3) : (flip ? 2 : 0);
	rei_input_rot = 0;
	switch (g_settings.rotate) {
	case REI_ROT_OFF: s_rot = flip ? 2 : 0; break; // (the monitor's quarter turn left out, its flip kept)
	case REI_ROT_VL: s_rot = (base + 1) & 3; rei_input_rot = 90; break;
	case REI_ROT_VR: s_rot = (base + 3) & 3; rei_input_rot = 270; break;
	default: s_rot = base; break;
	}
}

void rei_video_set_screen(int w, int h, int flags)
{
	s_gw = w;
	s_gh = h;
	s_gflags = flags;
}
static void free_art(void);
static int s_pic_ok; // a game picture was drawn (rei_video_picture_rect)
static float s_pic_x, s_pic_y, s_pic_w, s_pic_h;
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
	const int type = (s_rot & 1) ? REI_SCAN_OFF : g_settings.scan_type, strength = g_settings.scan_strength, dh = dest_height();
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
	if (type == REI_SCAN_OFF) // (a picture turned a quarter has vertical game lines: no overlay, see type above)
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
	update_rotation();
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
	s_scan_type = s_scan_strength = s_scan_dest_h = -1;
	free_art();
	s_pic_ok = 0;
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

// the app's blank artwork (games without their own), loaded once and kept. It is drawn tinted with the theme's accent
// (its brightness times the colour), made again when the theme changes into the other of two textures (the GPU may
// still be reading the one shown)
static uint8_t *s_blank_src; // RGBA
static int s_blank_w, s_blank_h, s_blank_tried, s_blank_cur;
static vita2d_texture *s_blank_tex[2], *s_blank;
static uint32_t s_blank_accent;

static uint8_t *load_png_rgba(const char *path, int *w, int *h)
{
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path))
		return NULL;
	img.format = PNG_FORMAT_RGBA;
	uint8_t *buf = (uint8_t *)malloc(PNG_IMAGE_SIZE(img));
	if (!buf || !png_image_finish_read(&img, NULL, buf, 0, NULL)) {
		free(buf);
		png_image_free(&img);
		return NULL;
	}
	*w = (int)img.width;
	*h = (int)img.height;
	return buf;
}

static void blank_tint(void)
{
	const uint32_t acc = rei_menu_accent();
	if (!s_blank_src || (s_blank && acc == s_blank_accent))
		return;
	const int k = s_blank ? s_blank_cur ^ 1 : 0;
	if (!s_blank_tex[k]) {
		s_blank_tex[k] = vita2d_create_empty_texture(s_blank_w, s_blank_h);
		if (!s_blank_tex[k])
			return;
		vita2d_texture_set_filters(s_blank_tex[k], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
	const unsigned tr = acc & 0xFF, tg = (acc >> 8) & 0xFF, tb = (acc >> 16) & 0xFF;
	uint32_t *dst = (uint32_t *)vita2d_texture_get_datap(s_blank_tex[k]);
	const int stride = (int)vita2d_texture_get_stride(s_blank_tex[k]) / 4;
	static uint32_t row[1024];
	for (int y = 0; y < s_blank_h; y++) {
		const uint8_t *p = s_blank_src + (size_t)y * s_blank_w * 4;
		for (int x = 0; x < s_blank_w && x < 1024; x++, p += 4) {
			const unsigned l = (p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8; // brightness, x2 so mid grey is the colour
			unsigned r = l * tr / 128, g = l * tg / 128, b = l * tb / 128;
			row[x] = (r > 255 ? 255 : r) | ((g > 255 ? 255 : g) << 8) | ((b > 255 ? 255 : b) << 16) | 0xFF000000u;
		}
		memcpy(dst + (size_t)y * stride, row, (size_t)(s_blank_w < 1024 ? s_blank_w : 1024) * 4); // (one copy a row:
		// word stores to texture memory are slow)
	}
	s_blank_cur = k;
	s_blank = s_blank_tex[k];
	s_blank_accent = acc;
}

void rei_video_set_art(const char *game, const char *parent)
{
	free_art();
	s_art = load_art_file(game);
	if (!s_art && parent)
		s_art = load_art_file(parent);
	if (s_art)
		vita2d_texture_set_filters(s_art, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	if (!s_art && !s_blank_tried) {
		s_blank_tried = 1;
		s_blank_src = load_png_rgba("app0:assets/blankArtwork.png", &s_blank_w, &s_blank_h);
	}
	if (!s_art)
		blank_tint();
	if (!s_art)
		REI_LOG("video: %s artwork missing (%s)", game, s_blank ? "blank artwork" : "border color");
}

static void free_art(void)
{
	if (s_art) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(s_art);
	}
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
	if (!s_art && g_settings.border == REI_BORDER_ARTWORK)
		blank_tint(); // (the theme may have changed in the menu)
	vita2d_texture *art = s_art ? s_art : s_blank;
	if (g_settings.border == REI_BORDER_ARTWORK && art)
		vita2d_draw_texture_scale(art, 0, 0, (float)SCREEN_W / vita2d_texture_get_width(art),
			(float)SCREEN_H / vita2d_texture_get_height(art));
	else
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, border_color());
}

static void draw_scaled(vita2d_texture *tex)
{
	// Fit the visible game screen (DISP_W x DISP_H, after rotation) to w x h per the aspect setting
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
	default: // fit height 544 and keep 4:3 (3:4 when turned a quarter) -> 725x544 / 408x544
		h = SCREEN_H;
		w = h * ((s_rot & 1) ? 3.0f / 4.0f : 4.0f / 3.0f);
		break;
	}
	const float x = (SCREEN_W - w) * 0.5f, y = (SCREEN_H - h) * 0.5f;
	s_pic_x = x;
	s_pic_y = y;
	s_pic_w = w;
	s_pic_h = h;
	s_pic_ok = 1;
	draw_border();
	if (!s_rot) {
		vita2d_draw_texture_part_scale(tex, x, y, 0, 0, s_gw, s_gh, w / s_gw, h / s_gh);
	} else {
		// turned clockwise by s_rot quarters around the picture centre; turned a quarter, the buffer width is the
		// screen height
		const float rad = 1.5707963f * s_rot;
		const float sx = (s_rot & 1) ? h / s_gw : w / s_gw, sy = (s_rot & 1) ? w / s_gh : h / s_gh;
		vita2d_draw_texture_part_scale_rotate(tex, x + w * 0.5f, y + h * 0.5f, 0, 0, s_gw, s_gh, sx, sy, rad);
	}
	if (s_scan)
		vita2d_draw_texture_scale(s_scan, x, y, w / 8.0f, h / (float)s_scan_h);
}

int rei_video_picture_rect(int *x, int *y, int *w, int *h)
{
	if (!s_pic_ok)
		return 0;
	*x = (int)s_pic_x;
	*y = (int)s_pic_y;
	*w = (int)s_pic_w;
	*h = (int)s_pic_h;
	return 1;
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

// --- overlay layers ---
ReiTex *rei_video_tex(int w, int h)
{
	vita2d_texture *t = vita2d_create_empty_texture(w, h);
	if (!t)
		return NULL;
	vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
	ReiTex *r = (ReiTex *)calloc(1, sizeof(ReiTex));
	if (!r) {
		vita2d_free_texture(t);
		return NULL;
	}
	r->tex = t;
	r->data = (uint32_t *)vita2d_texture_get_datap(t);
	r->stride = (int)vita2d_texture_get_stride(t) / 4;
	r->w = w;
	r->h = h;
	memset(r->data, 0, (size_t)r->stride * h * 4);
	return r;
}

static struct { ReiTex *t; int x, y, w, h, rot; float a; } s_layer[REI_NLAYER] = { { .a = 1 }, { .a = 1 }, { .a = 1 }, { .a = 1 },
	{ .a = 1 } };

void rei_video_layer(int slot, ReiTex *t, int x, int y, int w, int h)
{
	if (slot < 0 || slot >= REI_NLAYER)
		return;
	s_layer[slot].t = t;
	s_layer[slot].x = x;
	s_layer[slot].y = y;
	s_layer[slot].w = w;
	s_layer[slot].h = h;
}

void rei_video_layer_alpha(int slot, float a)
{
	if (slot >= 0 && slot < REI_NLAYER)
		s_layer[slot].a = a;
}

void rei_video_layer_rot(int slot, int rot)
{
	if (slot >= 0 && slot < REI_NLAYER)
		s_layer[slot].rot = rot & 3;
}

static ReiStrip s_strip[REI_NSTRIP];
static int s_nstrip, s_strip_y0, s_strip_y1;
void rei_video_strips(const ReiStrip *s, int n, int y0, int y1)
{
	if (n > REI_NSTRIP)
		n = REI_NSTRIP;
	for (int i = 0; i < n; i++)
		s_strip[i] = s[i];
	s_nstrip = n;
	s_strip_y0 = y0;
	s_strip_y1 = y1;
}

static void draw_strips(void)
{
	for (int i = 0; i < s_nstrip; i++) {
		const ReiStrip *s = &s_strip[i];
		const int y0 = s->y > s_strip_y0 ? s->y : s_strip_y0, y1 = s->y + s->t->h < s_strip_y1 ? s->y + s->t->h : s_strip_y1;
		if (y1 > y0)
			vita2d_draw_texture_part((vita2d_texture *)s->t->tex, s->x, y0, 0, y0 - s->y, s->t->w, y1 - y0);
	}
}

void rei_video_draw_layers(void)
{
	for (int o = 0; o < REI_NLAYER; o++) {
		const int k = o ? o - 1 : REI_NLAYER - 1; // (slot 4, the touch buttons, under the others)
		if (o == 2)
			draw_strips(); // (over slot 0, under the focus outline)
		const ReiTex *t = s_layer[k].t;
		if (!t)
			continue;
		vita2d_texture *tex = (vita2d_texture *)t->tex;
		const float a = s_layer[k].a;
		const unsigned int col = RGBA8(0xFF, 0xFF, 0xFF, (int)(a * 255.0f + 0.5f));
		const int rot = s_layer[k].rot, w = s_layer[k].w, h = s_layer[k].h;
		if (rot) { // turned: on screen h x w after a quarter turn, from x, y
			const float ow = (float)((rot & 1) ? h : w), oh = (float)((rot & 1) ? w : h);
			vita2d_draw_texture_part_tint_scale_rotate(tex, s_layer[k].x + ow * 0.5f, s_layer[k].y + oh * 0.5f, 0, 0, w, h,
				1.0f, 1.0f, 1.5707963f * rot, col);
		} else if (a < 1.0f)
			vita2d_draw_texture_tint_part(tex, s_layer[k].x, s_layer[k].y, 0, 0, w, h, col);
		else
			vita2d_draw_texture_part(tex, s_layer[k].x, s_layer[k].y, 0, 0, w, h);
	}
}

void rei_video_present(void)
{
	vita2d_start_drawing();
	vita2d_clear_screen();
	rei_video_draw_layers();
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

uint8_t *rei_video_load_bg(int *w, int *h)
{
	return load_png_rgba("app0:sce_sys/livearea/contents/bg.png", w, h);
}
