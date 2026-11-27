/*
 * audio_internal.h — helpers shared by the engine's source files.
 */
#pragma once

#include "audio.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
/* Flush denormals to zero: decaying filters and reverbs otherwise slow to
 * a crawl when their state gets tiny. */
static inline void audio_fp_setup(void) { _mm_setcsr(_mm_getcsr() | 0x8040); }
#else
static inline void audio_fp_setup(void) {}
#endif

/* Small deterministic RNG (xorshift32): renders are reproducible. */
typedef struct { uint32_t s; } rng;
static inline uint32_t rng_next(rng *r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}
static inline float rng_float(rng *r)  { return (rng_next(r) >> 8) * (1.0f / 16777216.0f); } /* 0..1 */
static inline float rng_bipolar(rng *r) { return rng_float(r) * 2.0f - 1.0f; }             /* -1..1 */

static inline float db_to_gain(float db) { return powf(10.0f, db / 20.0f); }
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Band-limited interpolation (windowed sinc), shared by the sampler,
 * sound_pitch and sample-rate conversion on load.
 * Reads src[] (n frames, zero outside) at fractional position pos, with a
 * low-pass for playback speed `speed` (> 1 = faster = needs more filtering). */
float sinc_read(const float *src, uint32_t n, double pos, double speed);

/* Resample one channel by `speed` (output frames = n / speed). */
float *resample_channel(const float *src, uint32_t n, double speed, uint32_t *out_n);

/* Exponential envelope coefficient: reaches -60 dB of the distance to
 * its target after `seconds`. */
static inline float env_coef(float seconds, float rate)
{
    if (seconds < 0.0005f) seconds = 0.0005f;
    return expf(-6.9077553f / (seconds * rate));
}
