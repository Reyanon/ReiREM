#ifndef REI_FRONTEND_AUDIO_H
#define REI_FRONTEND_AUDIO_H

#include <stdint.h>

// SceAudio output (docs/RULES.md ch. 3)
// The emu thread pushes samples into the ring buffer each frame; the top-priority output thread
// feeds sceAudioOutOutput in 256-sample blocks. On push, resample by up to ±0.5% based on fill (DRC).

#define REI_AUDIO_RATE 48000

int rei_audio_init(void);
void rei_audio_fini(void);

// Interleaved stereo INT16, frames = sample pairs. Excess is dropped when full
void rei_audio_push(const int16_t *samples, int frames);

typedef struct {
	uint32_t underruns;  // times silence was output for lack of samples
	uint32_t overflows;  // times samples were dropped on a full ring
	int fill;            // current ring fill (sample pairs)
	int drc_ppm;         // current rate adjustment (ppm, + = fewer output samples)
} rei_audio_stats;

void rei_audio_get_stats(rei_audio_stats *out);

#ifdef REI_AUDIO_DUMP
// Diag: buffer the first REI_AUDIO_DUMP_FRAMES in memory, then write them to a file at once.
// slot 0 = sound thread output (pre-DRC), 1 = what went to the device (post-DRC/ring)
void rei_audio_dump(const char *path, int slot, const int16_t *samples, int frames);
#endif

#endif
