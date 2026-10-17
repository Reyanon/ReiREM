#ifndef REI_FRONTEND_RESAMPLE_H
#define REI_FRONTEND_RESAMPLE_H

#include <stdint.h>

// Resampler for audio DRC (stereo INT16, 16-tap Lanczos, 1024 phases)
// Linear interpolation reached only 31dB in-band SNR: high-frequency attenuation varied periodically as the fractional
// position drifted (crackling). 16 taps give 68dB under the same conditions. Latency is 8 samples.

#define REI_RESAMPLE_MAX_IN 4096 // max sample pairs per call

void rei_resample_init(void);

// Resample frames sample pairs from in at step (16.16, input advance per output sample) into out.
// Returns the number of output sample pairs. out must hold at least frames * 65536 / step + 2 pairs.
int rei_resample(const int16_t *in, int frames, uint32_t step, int16_t *out);

#endif
