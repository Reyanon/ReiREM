#include "frontend/audio.h"

#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "frontend/resample.h"
#include "platform/log.h"

#ifdef REI_AUDIO_DUMP
#include <stdio.h>
#include <stdlib.h>
#define REI_AUDIO_DUMP_SAMPLES (48000 * 60) // 60 s (sample pairs)
static uint32_t *s_dump_buf[2];
static uint32_t s_dump_n[2];
static int s_dump_done[2];

void rei_audio_dump(const char *path, int slot, const int16_t *samples, int frames)
{
	if (s_dump_done[slot])
		return;
	if (!s_dump_buf[slot])
		s_dump_buf[slot] = (uint32_t *)malloc(REI_AUDIO_DUMP_SAMPLES * sizeof(uint32_t));
	const uint32_t *src = (const uint32_t *)samples;
	for (int i = 0; i < frames && s_dump_n[slot] < REI_AUDIO_DUMP_SAMPLES; i++)
		s_dump_buf[slot][s_dump_n[slot]++] = src[i];
	if (s_dump_n[slot] >= REI_AUDIO_DUMP_SAMPLES) {
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(s_dump_buf[slot], sizeof(uint32_t), s_dump_n[slot], f);
			fclose(f);
		}
		s_dump_done[slot] = 1;
		REI_LOG("audio: dump slot %d -> %s (%u samples)", slot, path, s_dump_n[slot]);
	}
}
#endif

#define GRAIN       256              // samples per sceAudioOutOutput call (multiple of 64)
#define RING_FRAMES 8192             // ring buffer size (sample pairs, power of 2, ~170ms)
#define RING_MASK   (RING_FRAMES - 1)
#define PREFILL     1600             // prefill before start (2 frames)
#define OUT_BUFS    3                // number of rotating output buffers

// single-producer (emu thread) / single-consumer (output thread) ring buffer. Sample pairs stored as uint32
static uint32_t s_ring[RING_FRAMES];
static volatile uint32_t s_head; // total written by producer
static volatile uint32_t s_tail; // total read by consumer

static int s_port = -1;
static SceUID s_thread = -1;
static volatile int s_running;
static volatile uint32_t s_underruns, s_overflows;

static int audio_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	// sceAudioOutOutput may return before the hardware has read the whole buffer. Overwriting a single buffer right away
	// corrupts the tail of each grain (256 samples), producing a 187.5Hz tone -> rotate 3 buffers
	static uint32_t bufs[OUT_BUFS][GRAIN] __attribute__((aligned(64)));
	int cur = 0;
	int started = 0;

	while (s_running) {
		uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
		uint32_t tail = s_tail;
		uint32_t avail = head - tail;

		if (!started) {
			if (avail < PREFILL) {
				sceKernelDelayThread(1000);
				continue;
			}
			started = 1;
		}

		uint32_t *buf = bufs[cur];
		cur = (cur + 1) % OUT_BUFS;
		uint32_t n = avail < GRAIN ? avail : GRAIN;
		for (uint32_t i = 0; i < n; i++)
			buf[i] = s_ring[(tail + i) & RING_MASK];
		if (n < GRAIN) {
			memset(buf + n, 0, (GRAIN - n) * sizeof(uint32_t));
			s_underruns++;
		}
		__atomic_store_n(&s_tail, tail + n, __ATOMIC_RELEASE);

#ifdef REI_AUDIO_DUMP
		if (started)
			rei_audio_dump("ux0:data/reirem/dump_out.raw", 1, (const int16_t *)buf, GRAIN);
#endif
		sceAudioOutOutput(s_port, buf); // blocks until the hardware consumes it
	}
	return sceKernelExitDeleteThread(0);
}

int rei_audio_init(void)
{
	s_head = s_tail = 0;
	rei_resample_init();
	s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, REI_AUDIO_RATE,
		SCE_AUDIO_OUT_MODE_STEREO);
	if (s_port < 0) {
		REI_LOG("audio: open port failed 0x%08x", s_port);
		return -1;
	}

	s_running = 1;
	// highest user priority. Core affinity decided in M3
	s_thread = sceKernelCreateThread("rei_audio", audio_thread, 64, 0x4000, 0, 0, NULL);
	if (s_thread < 0) {
		REI_LOG("audio: thread create failed 0x%08x", s_thread);
		sceAudioOutReleasePort(s_port);
		s_port = -1;
		return -1;
	}
	sceKernelStartThread(s_thread, 0, NULL);
	REI_LOG("audio: %d Hz, grain %d, ring %d", REI_AUDIO_RATE, GRAIN, RING_FRAMES);
	return 0;
}

void rei_audio_fini(void)
{
	if (s_thread >= 0) {
		s_running = 0;
		sceKernelWaitThreadEnd(s_thread, NULL, NULL);
		s_thread = -1;
	}
	if (s_port >= 0) {
		sceAudioOutReleasePort(s_port);
		s_port = -1;
	}
}

// --- dynamic rate control (DRC, docs/RULES.md section 3) ---
// The Vita display is 59.94Hz, so 800 samples per frame falls slightly short of 48kHz consumption. Input is stretched or
// shrunk within ±0.5% (16-tap resampling, resample.c) to keep the ring buffer fill at the target (2 frames).
#define DRC_TARGET   1600                  // target fill (sample pairs)
#define DRC_MAX_PPM  5000                  // max adjustment 0.5%
static int16_t s_drc_out[(REI_RESAMPLE_MAX_IN + REI_RESAMPLE_MAX_IN / 64 + 4) * 2];
static int32_t s_drc_err;                  // low-passed fill error (sample pairs * 256)
static volatile int32_t s_drc_ppm;         // current adjustment (stats)

static void ring_put(uint32_t *head, uint32_t tail, int16_t l, int16_t r)
{
	if (*head - tail >= RING_FRAMES) {
		s_overflows++;
		return;
	}
	s_ring[*head & RING_MASK] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
	(*head)++;
}

void rei_audio_push(const int16_t *samples, int frames)
{
	uint32_t head = s_head;
	uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
	int32_t fill = (int32_t)(head - tail);

	// track the error slowly so pitch doesn't wobble (1/16 low-pass)
	s_drc_err += (((fill - DRC_TARGET) << 8) - s_drc_err) >> 4;
	int32_t ppm = (int32_t)((int64_t)(s_drc_err >> 8) * DRC_MAX_PPM / DRC_TARGET);
	if (ppm > DRC_MAX_PPM) ppm = DRC_MAX_PPM;
	if (ppm < -DRC_MAX_PPM) ppm = -DRC_MAX_PPM;
	s_drc_ppm = ppm;
	// input advance per output sample. With a high fill (ppm>0) advance faster, producing less output
	uint32_t step = (uint32_t)(65536 + (int64_t)65536 * ppm / 1000000);

	int n = rei_resample(samples, frames, step, s_drc_out);
	for (int i = 0; i < n; i++)
		ring_put(&head, tail, s_drc_out[i * 2], s_drc_out[i * 2 + 1]);
	__atomic_store_n(&s_head, head, __ATOMIC_RELEASE);
}

void rei_audio_get_stats(rei_audio_stats *out)
{
	out->underruns = s_underruns;
	out->overflows = s_overflows;
	out->fill = (int)(s_head - s_tail);
	out->drc_ppm = s_drc_ppm;
}
