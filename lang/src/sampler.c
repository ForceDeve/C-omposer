/*
 * sampler.c — band-limited sample playback, resampling and chopping.
 */
#include "audio_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Windowed-sinc interpolation                                        */
/* ------------------------------------------------------------------ */

#define SINC_ZEROS 16          /* zero crossings each side             */
#define SINC_RES   512         /* table points per zero crossing       */

static float sinc_table[SINC_ZEROS * SINC_RES + 2];
static int   sinc_ready;

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
    return sum;
}

static void sinc_init(void)
{
    if (sinc_ready) return;
    const double beta = 9.0;                           /* Kaiser window */
    for (int i = 0; i <= SINC_ZEROS * SINC_RES + 1; i++) {
        double x = (double)i / SINC_RES;
        double s = i == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = x >= SINC_ZEROS ? 0.0
                 : bessel_i0(beta * sqrt(1.0 - (x / SINC_ZEROS) * (x / SINC_ZEROS))) / bessel_i0(beta);
        sinc_table[i] = (float)(s * w);
    }
    sinc_ready = 1;
}

static inline float kernel(double x)
{
    x = fabs(x) * SINC_RES;
    int i = (int)x;
    if (i >= SINC_ZEROS * SINC_RES) return 0.0f;
    float f = (float)(x - i);
    return sinc_table[i] + f * (sinc_table[i + 1] - sinc_table[i]);
}

float sinc_read(const float *src, uint32_t n, double pos, double speed)
{
    sinc_init();
    double scale = speed > 1.0 ? speed : 1.0;          /* widen = lower cutoff */
    if (scale > 8.0) scale = 8.0;
    double half = SINC_ZEROS * scale;
    long lo = (long)ceil(pos - half), hi = (long)floor(pos + half);
    if (lo < 0) lo = 0;
    if (hi >= (long)n) hi = (long)n - 1;
    float sum = 0;
    for (long i = lo; i <= hi; i++)
        sum += src[i] * kernel((pos - i) / scale);
    return (float)(sum / scale);
}

float *resample_channel(const float *src, uint32_t n, double speed, uint32_t *out_n)
{
    uint32_t m = (uint32_t)(n / speed);
    if (m == 0) m = 1;
    float *out = malloc(m * sizeof(float));
    if (!out) return NULL;
    for (uint32_t i = 0; i < m; i++)
        out[i] = sinc_read(src, n, i * speed, speed);
    *out_n = m;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Sampler instrument                                                 */
/* ------------------------------------------------------------------ */

sampler sampler_new(const sound *sample, note root)
{
    return (sampler){
        .sample = sample, .root = root, .pitched = true, .gain = 1.0f,
        .env = { 0.001f, 0.0f, 1.0f, 0.05f },
    };
}

sampler sampler_slices(const sound *slices, int n, note base)
{
    sampler s = sampler_new(NULL, base);
    s.slices = slices; s.n_slices = n; s.slice_base = base;
    s.pitched = false;
    s.env.attack = 0;          /* slices already have click-free edges: keep the transient */
    return s;
}

sound sampler_render(const sampler *smp, const part *notes)
{
    audio_fp_setup();
    const float rate = (float)audio.rate;
    uint32_t len = ticks_to_frames(notes->length);
    sound out = sound_new(len ? len : 1);
    out.length = len;
    uint32_t end = 0;

    float pan = clampf(smp->pan, -1, 1);
    float pl = cosf((pan + 1) * (float)M_PI / 4) * (float)M_SQRT2;
    float pr = sinf((pan + 1) * (float)M_PI / 4) * (float)M_SQRT2;
    float ca = expf(-1.4663371f / (fmaxf(smp->env.attack, 0.0005f) * rate));
    float cd = env_coef(smp->env.decay, rate);
    float cr = env_coef(smp->env.release, rate);
    float sus = smp->env.decay > 0 ? clampf(smp->env.sustain, 0, 1) : 1.0f;

    for (uint32_t k = 0; k < notes->count; k++) {
        const note_event *e = &notes->events[k];
        const sound *src = smp->sample;
        double speed = 1.0;
        if (smp->slices) {
            int idx = (int)e->pitch - smp->slice_base;
            if (idx < 0 || idx >= smp->n_slices) continue;
            src = &smp->slices[idx];
        } else if (smp->pitched) {
            speed = note_freq_edo(e->pitch, e->edo) / note_freq_edo(smp->root, smp->root_edo);
        }
        if (!src || !src->frames) continue;

        uint32_t loop_end = smp->loop_end && smp->loop_end <= src->frames ? smp->loop_end : src->frames;
        bool loop = smp->loop && loop_end > smp->loop_start + 1;
        uint32_t at = ticks_to_frames(e->start);
        uint32_t gate = ticks_to_frames(e->length);
        uint32_t natural = (uint32_t)(src->frames / speed);
        uint32_t max_n = loop ? gate + (uint32_t)(smp->env.release * 1.5f * rate) + 64
                              : natural + 1;
        if (at + max_n > out.frames && !sound_resize(&out, at + max_n)) break;

        float g = smp->gain * (0.3f + 0.7f * (e->velocity / 127.0f));
        bool instant = smp->env.attack <= 0;
        float level = instant ? 1.0f : 0.0f;
        int stage = instant ? 1 : 0;                    /* 0 att, 1 dec, 2 rel */
        double pos = 0;
        uint32_t i;
        for (i = 0; i < max_n; i++) {
            if (i == gate && stage < 2) stage = 2;
            if (stage == 0) { level = 1.3f + (level - 1.3f) * ca; if (level >= 1) { level = 1; stage = 1; } }
            else if (stage == 1) level = sus + (level - sus) * cd;
            else { level *= cr; if (level < 1e-5f) break; }

            if (loop) {
                while (pos >= loop_end) pos -= (loop_end - smp->loop_start);
            } else if (pos >= src->frames) {
                break;
            }
            float l, r;
            if (speed == 1.0 && pos == floor(pos)) {
                l = src->l[(uint32_t)pos]; r = src->r[(uint32_t)pos];
            } else {
                l = sinc_read(src->l, src->frames, pos, speed);
                r = sinc_read(src->r, src->frames, pos, speed);
            }
            out.l[at + i] += l * level * g * pl;
            out.r[at + i] += r * level * g * pr;
            pos += speed;
        }
        if (at + i > end) end = at + i;
    }
    if (end > out.length) out.frames = end;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Chopping                                                           */
/* ------------------------------------------------------------------ */

/* Copy [start, end) into a new sound with short fades so cuts don't click. */
static sound slice_of(const sound *s, uint32_t start, uint32_t end)
{
    uint32_t n = end > start ? end - start : 1;
    sound out = sound_new(n);
    if (!out.frames) return out;
    memcpy(out.l, s->l + start, n * sizeof(float));
    memcpy(out.r, s->r + start, n * sizeof(float));
    uint32_t fin = audio.rate / 1000;              /* 1 ms in  */
    uint32_t fout = audio.rate / 200;              /* 5 ms out */
    for (uint32_t i = 0; i < fin && i < n; i++) {
        float g = (float)i / fin;
        out.l[i] *= g; out.r[i] *= g;
    }
    for (uint32_t i = 0; i < fout && i < n; i++) {
        float g = (float)i / fout;
        out.l[n - 1 - i] *= g; out.r[n - 1 - i] *= g;
    }
    return out;
}

int sound_chop(const sound *s, int n, sound *out, int max)
{
    if (n < 1 || !s->frames) return 0;
    if (n > max) n = max;
    uint32_t total = s->length && s->length <= s->frames ? s->length : s->frames;
    for (int i = 0; i < n; i++) {
        uint32_t a = (uint32_t)((uint64_t)total * i / n);
        uint32_t b = (uint32_t)((uint64_t)total * (i + 1) / n);
        out[i] = slice_of(s, a, b);
    }
    return n;
}

int sound_chop_onsets(const sound *s, float sensitivity, sound *out, int max)
{
    if (!s->frames || max < 1) return 0;
    const uint32_t hop = 256;
    uint32_t hops = s->frames / hop;
    if (hops < 2) { out[0] = slice_of(s, 0, s->frames); return 1; }

    /* onset strength: rise in log energy of the high-passed signal */
    float *energy = calloc(hops, sizeof(float));
    uint32_t *cuts = malloc((size_t)(max + 1) * sizeof(uint32_t));
    if (!energy || !cuts) { free(energy); free(cuts); return 0; }
    float prev = 0;
    for (uint32_t h = 0; h < hops; h++) {
        double e = 0;
        for (uint32_t i = h * hop; i < (h + 1) * hop; i++) {
            float x = (s->l[i] + s->r[i]) * 0.5f;
            float d = x - prev;                        /* first difference = HF emphasis */
            prev = x;
            e += d * d;
        }
        energy[h] = logf((float)(e / hop) + 1e-10f);
    }
    float threshold = 3.0f - 2.6f * clampf(sensitivity, 0, 1);    /* nats */
    uint32_t min_gap = (uint32_t)(0.05f * audio.rate / hop) + 1;   /* 50 ms */
    int n = 0;
    cuts[n++] = 0;
    uint32_t last = 0;
    for (uint32_t h = 1; h + 1 < hops && n < max; h++) {
        float rise = energy[h] - energy[h - 1];
        float next = energy[h + 1] - energy[h];
        if (rise > threshold && rise >= next && h - last >= min_gap && energy[h] > -18.0f) {
            /* start the slice slightly before the hit */
            uint32_t at = h * hop > hop ? h * hop - hop / 2 : 0;
            cuts[n++] = at;
            last = h;
        }
    }
    cuts[n] = s->frames;
    for (int i = 0; i < n; i++)
        out[i] = slice_of(s, cuts[i], cuts[i + 1]);
    free(energy);
    free(cuts);
    return n;
}
