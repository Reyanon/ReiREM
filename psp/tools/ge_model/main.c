// PC model of the PSP GE layer renderer (G2): composites the ge_layers run list and sprite buffer in software using GE rules
// and compares per pixel (palette index) with the core CPU reference frame (rei_core_set_ge_reference).
// GE rules = psp/src/video.c: tile pixels only for colors in the pattern, high-pattern pixels mark stencil "high", sprites
// overwrite unless (0x8000 mark && high). Also prints the reference frame hash to check against the harness (--mode pipelined) baseline.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_layers.h"
#include "rei_core.h"

#define W REI_SCREEN_W
#define H REI_SCREEN_H
#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint16_t g_idx[W * H];
static uint8_t g_high[W * H];
static rei_tile_info g_ti;
static int g_variant; // diag: 1 = ignore texture window (tile & 4095), 2 = last window instead of first
static int g_last_win;

static void draw_tile(const ge_tile *t, int layer, int y0, int y1)
{
	int group = (t->flags >> 2) & 3;
	int tile = t->tile;
	if (g_variant == 1)
		tile &= 4095;
	else if (g_variant == 2)
		tile = (g_last_win << 12) | (tile & 4095);
	const uint8_t *gfx = g_ti.tiles + (size_t)tile * 64;
	for (int pass = 0; pass < 2; pass++) {
		int pat = ge_pat_of[layer == 2][group][pass];
		if (pat < 0)
			continue;
		uint16_t bits = ge_pat_bits[pat];
		int strip = (t->flags & GE_TILE_STRIP) != 0;
		int ty0 = strip ? GE_STRIP_ROW(t->flags) : 0, ty1 = strip ? ty0 + t->h : 8;
		for (int ty = ty0; ty < ty1; ty++) {
			int y = t->y + ty - ty0;
			if (y < y0 || y >= y1 || y < 0 || y >= H)
				continue;
			int sy = (t->flags & 2) ? 7 - ty : ty;
			for (int tx = 0; tx < 8; tx++) {
				int x = t->x + tx;
				if (x < 0 || x >= W)
					continue;
				int sx = (t->flags & 1) ? 7 - tx : tx;
				int v = gfx[sy * 8 + sx] & 15;
				if (!(bits & (1u << v)))
					continue;
				g_idx[y * W + x] = (uint16_t)(t->pal * 16 + v);
				if (pass == 1)
					g_high[y * W + x] = 1;
			}
		}
	}
}

static void model(const ge_frame *fr, const uint16_t *spr, int blank)
{
	for (int i = 0; i < W * H; i++) {
		g_idx[i] = 0xffff; // mark pixels nothing drew
		g_high[i] = 0;
	}
	if (blank) {
		for (int i = 0; i < W * H; i++)
			g_idx[i] = 0x800;
		return;
	}
	for (int r = 0; r < fr->nruns; r++) {
		const ge_run *run = &fr->runs[r];
		if (run->fill) {
			for (int y = run->y0; y < run->y1 && y < H; y++)
				for (int x = 0; x < W; x++)
					g_idx[y * W + x] = 0;
			continue;
		}
		// diag REI_MODEL_REVERSE: reverse band runs (hardware draws them sorted by state); a changed result means tiles overlap
		static int rev = -1;
		if (rev < 0)
			rev = getenv("REI_MODEL_REVERSE") != NULL;
		for (int i = 0; i < run->count; i++) {
			int k = (rev && run->strips) ? run->count - 1 - i : i;
			draw_tile(&fr->tiles[run->first + k], run->layer, run->y0, run->y1);
		}
	}
	if (spr)
		for (int i = 0; i < W * H; i++) {
			uint16_t s = spr[i];
			if (!s)
				continue;
			if ((s & 0x8000) && g_high[i])
				continue;
			g_idx[i] = s & 0x7ff;
		}
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

int main(int argc, char **argv)
{
	const char *rom = "nbbatman.zip", *set = NULL, *parent = NULL;
	int frames = 3600, autoplay = 0, verbose = 3, dump_frame = -1, start_stage = 0;
	int runs_frame = -1;
	const char *dump_path = NULL, *got_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rom") && i + 1 < argc)
			rom = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--set") && i + 1 < argc)
			set = argv[++i];
		else if (!strcmp(argv[i], "--parent") && i + 1 < argc)
			parent = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 2 < argc) {
			dump_frame = atoi(argv[++i]);
			dump_path = argv[++i];
		} else if (!strcmp(argv[i], "--variant") && i + 1 < argc)
			g_variant = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--got") && i + 1 < argc)
			got_path = argv[++i];
		else if (!strcmp(argv[i], "--runs") && i + 1 < argc)
			runs_frame = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--autoplay"))
			autoplay = 1;
		else if (!strcmp(argv[i], "--stage") && i + 1 < argc)
			start_stage = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--inf-health"))
			rei_core_set_infinite_health(1);
		else if (!strcmp(argv[i], "--verbose") && i + 1 < argc)
			verbose = atoi(argv[++i]);
	}
	rei_core_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.zip_path = rom;
	cfg.sample_rate = 48000;
	cfg.bytes_per_pixel = 4;
	cfg.pipelined = 1;
	cfg.index_out = 1;
	cfg.ge_layers = 1;
	cfg.layer_cb = ge_layers_segment;
	cfg.set_name = set;
	cfg.parent_zip_path = parent;
	if (rei_core_init(&cfg) != 0) {
		fprintf(stderr, "init failed: %s\n", rei_core_error());
		return 1;
	}
	rei_core_tile_info(&g_ti);
	if (ge_layers_init(&g_ti) != 0)
		return 1;
	rei_core_set_ge_reference(1);
	rei_core_set_start_stage(start_stage);

	int alen = rei_core_audio_len();
	int16_t *audio = calloc((size_t)alen * 2 + 16, sizeof(int16_t));
	static uint32_t rgb[W * H];
	uint64_t hv = FNV_OFFSET, ha = FNV_OFFSET;
	long undrawn_odd = 0, bad_frames = 0, bad_px = 0, undrawn = 0, max_tiles = 0, max_runs = 0, sum_tiles = 0, sum_runs = 0, overflow = 0;
	for (int f = 0; f < frames; f++) {
		if (autoplay)
			rei_core_autoplay(f);
		memset(audio, 0, (size_t)alen * 4);
		ge_layers_begin();
		rei_core_run_frame(NULL, 0, audio);
		const ge_frame *fr = ge_layers_end();
		const uint32_t *pal = NULL;
		int blank = 0;
		const uint16_t *ref = rei_core_index_frame(&pal, &blank);
		if (!ref) {
			hv = fnv1a(hv, rgb, sizeof(rgb));
			ha = fnv1a(ha, audio, (size_t)alen * 4);
			continue;
		}
		for (int i = 0; i < W * H; i++)
			rgb[i] = pal[ref[i]];
		hv = fnv1a(hv, rgb, sizeof(rgb));
		ha = fnv1a(ha, audio, (size_t)alen * 4);

		model(fr, rei_core_sprite_frame(), blank);
		if (f == dump_frame && got_path) {
			// compare the PSP GE dump (video_dump) frame with this model (incl. variants)
			FILE *g = fopen(got_path, "rb");
			static uint32_t gimg[W * H];
			if (g && fread(gimg, 4, W * H, g) == W * H) {
				static uint16_t gidx[W * H];
				static uint32_t gpal[0x801];
				int nr = 0, nt = 0;
				if (fread(gidx, 2, W * H, g) == W * H && fread(gpal, 4, 0x801, g) == 0x801 && fread(&nr, 4, 1, g) == 1 &&
				    fread(&nt, 4, 1, g) == 1) {
					ge_run *rr = malloc(sizeof(ge_run) * (nr + 1));
					ge_tile *tt = malloc(sizeof(ge_tile) * (nt + 1));
					size_t a = fread(rr, sizeof(ge_run), nr, g), b = fread(tt, sizeof(ge_tile), nt, g);
					printf("PSP list: runs %d tiles %d (read %zu %zu) / model: runs %d tiles %d\n", nr, nt, a, b, fr->nruns, fr->ntiles);
					int idiff = 0;
					for (int i = 0; i < W * H; i++)
						idiff += gidx[i] != ref[i];
					printf("reference index diff %d px\n", idiff);
					int shown = 0;
					for (int i = 0; i < nr && i < fr->nruns && shown < 5; i++)
						if (memcmp(&rr[i], &fr->runs[i], sizeof(ge_run))) {
							printf("run %d differs: PSP y%d-%d L%d fill%d n%d / model y%d-%d L%d fill%d n%d\n", i, rr[i].y0, rr[i].y1,
							       rr[i].layer, rr[i].fill, rr[i].count, fr->runs[i].y0, fr->runs[i].y1, fr->runs[i].layer,
							       fr->runs[i].fill, fr->runs[i].count);
							shown++;
						}
					for (int i = 0; i < nt && i < fr->ntiles && shown < 10; i++)
						if (memcmp(&tt[i], &fr->tiles[i], sizeof(ge_tile))) {
							printf("tile %d differs: PSP x%d y%d t%04x p%02x f%x / model x%d y%d t%04x p%02x f%x\n", i, tt[i].x, tt[i].y,
							       tt[i].tile, tt[i].pal, tt[i].flags, fr->tiles[i].x, fr->tiles[i].y, fr->tiles[i].tile,
							       fr->tiles[i].pal, fr->tiles[i].flags);
							shown++;
						}
					if (!shown)
						printf("run/tile lists match\n");
					free(rr);
					free(tt);
				}
				fseek(g, 0, SEEK_SET);
				if (fread(gimg, 4, W * H, g) != W * H)
					printf("reread failed\n");
				int same = 0;
				for (int i = 0; i < W * H; i++) {
					uint16_t ix = g_idx[i] == 0xffff ? 0 : g_idx[i];
					uint32_t c = pal[ix];
					uint32_t want = ((c >> 16) & 255) | (c & 0xff00) | ((c & 255) << 16); // 0x00RRGGBB → 0x00BBGGRR
					if ((gimg[i] & 0xffffff) == want)
						same++;
				}
				printf("variant %d: pixels matching GE dump %d / %d\n", g_variant, same, W * H);
				// on-screen tiles of the last run (top layer) where GE is wrong: find which tile the actual 8x8 came from
				const ge_run *top = &fr->runs[fr->nruns - 1];
				int found = 0;
				for (int k = 0; k < top->count && found < 6; k++) {
					const ge_tile *t = &fr->tiles[top->first + k];
					if (t->x < 0 || t->y < top->y0 || t->x + 8 > W || t->y + 8 > top->y1 || t->y + 8 > H)
						continue;
					int wrong = 0;
					uint32_t blk[64];
					for (int yy = 0; yy < 8; yy++)
						for (int xx = 0; xx < 8; xx++) {
							int i = (t->y + yy) * W + t->x + xx;
							blk[yy * 8 + xx] = gimg[i] & 0xffffff;
							uint16_t ix = g_idx[i] == 0xffff ? 0 : g_idx[i];
							uint32_t c = pal[ix];
							if (blk[yy * 8 + xx] != (((c >> 16) & 255) | (c & 0xff00) | ((c & 255) << 16)))
								wrong++;
						}
					if (!wrong)
						continue;
					// match against all tiles with the same palette/flip (assumes opaque)
					int hit = -1, hitv = -1, hitp = -1;
					for (int P = 0; P < 128 && hit < 0; P++)
					for (int var = 0; var < 8 && hit < 0; var++)
					for (int cand = 0; cand <= g_ti.tile_mask && hit < 0; cand++) {
						const uint8_t *gfx = g_ti.tiles + (size_t)cand * 64;
						int ok = 1;
						for (int yy = 0; yy < 8 && ok; yy++)
							for (int xx = 0; xx < 8 && ok; xx++) {
								int sy = (var & 2) ? 7 - yy : yy, sx = (var & 1) ? 7 - xx : xx;
								if (var & 4) { int k = sx; sx = sy; sy = k; }
								uint32_t c = pal[P * 16 + (gfx[sy * 8 + sx] & 15)];
								if (blk[yy * 8 + xx] != (((c >> 16) & 255) | (c & 0xff00) | ((c & 255) << 16)))
									ok = 0;
							}
						if (ok) {
							hit = cand;
							hitv = var;
							hitp = P;
						}
					}
					printf("tile x%d y%d t%04x p%02x f%x wrong pixels %d -> GE drew tile %04x variant %d palette %02x\n", t->x, t->y, t->tile, t->pal,
					       t->flags, wrong, hit, hitv, hitp);
					found++;
				}
			}
			if (g)
				fclose(g);
		}
		if (f == runs_frame) { // diag --runs F: run list of that frame
			printf("f%d: runs %d tiles %d\n", f, fr->nruns, fr->ntiles);
			for (int i = 0; i < fr->nruns; i++)
				printf("  run %d: y %d..%d layer %d fill %d strips %d tiles %d\n", i, fr->runs[i].y0, fr->runs[i].y1,
				       fr->runs[i].layer, fr->runs[i].fill, fr->runs[i].strips, fr->runs[i].count);
		}
		if (f == dump_frame && dump_path) {
			// reference (left) and model (right) side by side as PPM
			FILE *o = fopen(dump_path, "wb");
			if (o) {
				fprintf(o, "P6\n%d %d\n255\n", W * 2, H);
				for (int y = 0; y < H; y++)
					for (int k = 0; k < 2; k++)
						for (int x = 0; x < W; x++) {
							uint16_t ix = k ? g_idx[y * W + x] : ref[y * W + x];
							uint32_t c = ix < 0x801 ? pal[ix] : 0;
							fputc((c >> 16) & 255, o);
							fputc((c >> 8) & 255, o);
							fputc(c & 255, o);
						}
				fclose(o);
			}
		}
		int d = 0, u = 0, fx = -1, fy = -1;
		for (int i = 0; i < W * H; i++) {
			if (g_idx[i] == 0xffff) {
				u++;
				if (ref[i] != 0 && ref[i] != 0x800 && !undrawn_odd++)
					printf("f%d: undrawn pixel x%d y%d reference value %03x\n", f, i % W, i / W, ref[i]);
				continue;
			}
			if (g_idx[i] != ref[i]) {
				if (!d) {
					fx = i % W;
					fy = i / W;
				}
				d++;
			}
		}
		undrawn += u;
		if (d) {
			bad_frames++;
			bad_px += d;
			if (bad_frames <= verbose)
				printf("f%d: %d px differ (first x%d y%d model %03x ref %03x), runs %d tiles %d\n", f, d, fx, fy,
				       g_idx[fy * W + fx], ref[fy * W + fx], fr->nruns, fr->ntiles);
		}
		if (fr->ntiles > max_tiles)
			max_tiles = fr->ntiles;
		if (fr->nruns > max_runs)
			max_runs = fr->nruns;
		sum_tiles += fr->ntiles;
		sum_runs += fr->nruns;
		overflow += fr->overflow;
	}
	printf("frames=%d video=%016llx audio=%016llx\n", frames, (unsigned long long)hv, (unsigned long long)ha);
	printf("bad frames %ld (pixels %ld), undrawn pixels %ld (not 0/0x800: %ld), tiles avg %ld max %ld, runs avg %ld max %ld, overflow %ld\n", bad_frames,
	       bad_px, undrawn, undrawn_odd, sum_tiles / (frames ? frames : 1), max_tiles, sum_runs / (frames ? frames : 1), max_runs, overflow);
	rei_core_exit();
	return bad_frames ? 2 : 0;
}
