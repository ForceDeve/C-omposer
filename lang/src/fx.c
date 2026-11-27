/*
 * fx.c — effects. Each takes a sound, consumes it, and returns the result.
 * Effects with tails (reverb, delay) extend `frames` but not `length`.
 */
#include "audio_internal.h"

#include <stdlib.h>
#include <string.h>

/* Fractional delay line. */
typedef struct { float *buf; uint32_t size, pos; } delay_line;

static bool dl_init(delay_line *d, uint32_t size)
{
    d->buf = calloc(size, sizeof(float));
    d->size = size; d->pos = 0;
    return d->buf != NULL;
}
static void dl_free(delay_line *d) { free(d->buf); d->buf = NULL; }
static inline void dl_push(delay_line *d, float x) { d->buf[d->pos] = x; d->pos = (d->pos + 1) % d->size; }
/* Read `delay` samples back (fractional, linear). */
static inline float dl_read(const delay_line *d, float delay)
{
    float rp = (float)d->pos - 1.0f - delay;
    while (rp < 0) rp += d->size;
    uint32_t i = (uint32_t)rp;
    float f = rp - i;
    float a = d->buf[i % d->size], b = d->buf[(i + 1) % d->size];
    return a + f * (b - a);
}

static sound extend(sound s, float seconds)
{
    uint32_t extra = (uint32_t)(seconds * audio.rate);
    sound_resize(&s, s.frames + extra);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Reverb: 8-line feedback delay network                              */
/* ------------------------------------------------------------------ */

sound fx_reverb(sound s, float size, float decay, float damping, float mix)
{
    audio_fp_setup();
    if (!s.frames) return s;
    size = clampf(size, 0, 1);
    decay = clampf(decay, 0.1f, 30.0f);
    mix = clampf(mix, 0, 1);
    s = extend(s, decay * 1.1f);
    const float rate = (float)audio.rate;

    static const float base_ms[8] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.0f, 59.9f, 67.1f, 73.3f };
    static const float diff_ms[4] = { 4.7f, 3.6f, 12.7f, 9.3f };
    delay_line line[8], diff[4], pre;
    float len[8], fb[8], lp[8] = { 0 };
    bool ok = dl_init(&pre, (uint32_t)(0.03f * rate) + 2);
    for (int i = 0; i < 8; i++) {
        len[i] = base_ms[i] * (0.45f + 1.1f * size) * 0.001f * rate;
        ok &= dl_init(&line[i], (uint32_t)len[i] + 16);
        fb[i] = powf(10.0f, -3.0f * len[i] / (decay * rate));    /* -60 dB after `decay` */
    }
    for (int i = 0; i < 4; i++) ok &= dl_init(&diff[i], (uint32_t)(diff_ms[i] * 0.001f * rate * (0.5f + size)) + 2);
    if (!ok) goto cleanup;

    float damp = clampf(damping, 0, 0.95f);
    float lfo = 0, lfo_inc = 2.0f * (float)M_PI * 0.7f / rate;
    for (uint32_t n = 0; n < s.frames; n++) {
        float in = (s.l[n] + s.r[n]) * 0.5f;
        dl_push(&pre, in);
        float x = dl_read(&pre, pre.size - 2.0f);

        /* input diffusion: 4 series all-passes */
        for (int i = 0; i < 4; i++) {
            float d = dl_read(&diff[i], diff[i].size - 2.0f);
            float v = x + 0.62f * d;
            dl_push(&diff[i], v);
            x = d - 0.62f * v;
        }

        /* read lines (two are gently modulated to avoid metallic ringing) */
        float o[8];
        lfo += lfo_inc;
        if (lfo > 2 * (float)M_PI) lfo -= 2 * (float)M_PI;
        for (int i = 0; i < 8; i++) {
            float mod = i == 1 ? sinf(lfo) * 6.0f : i == 6 ? cosf(lfo * 0.83f) * 5.0f : 0.0f;
            float y = dl_read(&line[i], len[i] + mod);
            lp[i] = y + damp * (lp[i] - y);                     /* damping */
            o[i] = lp[i];
        }
        /* Hadamard mix (fast Walsh-Hadamard, normalised) */
        for (int h = 1; h < 8; h <<= 1)
            for (int i = 0; i < 8; i += h << 1)
                for (int j = i; j < i + h; j++) {
                    float a = o[j], b = o[j + h];
                    o[j] = a + b; o[j + h] = a - b;
                }
        for (int i = 0; i < 8; i++)
            dl_push(&line[i], o[i] * 0.35355339f * fb[i] + (i & 1 ? -x : x) * 0.5f);

        float wl = lp[0] + lp[2] + lp[4] + lp[6];
        float wr = lp[1] + lp[3] + lp[5] + lp[7];
        s.l[n] = s.l[n] * (1.0f - mix) + wl * 0.5f * mix;
        s.r[n] = s.r[n] * (1.0f - mix) + wr * 0.5f * mix;
    }
cleanup:
    dl_free(&pre);
    for (int i = 0; i < 8; i++) dl_free(&line[i]);
    for (int i = 0; i < 4; i++) dl_free(&diff[i]);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Delay                                                              */
/* ------------------------------------------------------------------ */

sound fx_delay(sound s, uint32_t time_ticks, float feedback, float tone, float mix, bool ping_pong)
{
    audio_fp_setup();
    if (!s.frames) return s;
    feedback = clampf(feedback, 0, 0.95f);
    mix = clampf(mix, 0, 1);
    float t = (float)ticks_to_seconds(time_ticks);
    if (t < 0.001f) return s;
    float repeats = feedback > 0.001f ? logf(0.001f) / logf(feedback) : 1.0f;
    s = extend(s, fminf(t * (repeats + 1), 20.0f));

    uint32_t d = (uint32_t)(t * audio.rate);
    delay_line dl, dr;
    if (!dl_init(&dl, d + 2) || !dl_init(&dr, d + 2)) { dl_free(&dl); dl_free(&dr); return s; }
    float fc = 800.0f + 14000.0f * clampf(tone, 0, 1);
    float a = expf(-2.0f * (float)M_PI * fc / audio.rate);
    float zl = 0, zr = 0;
    for (uint32_t n = 0; n < s.frames; n++) {
        float yl = dl_read(&dl, (float)d - 1.0f), yr = dl_read(&dr, (float)d - 1.0f);
        zl = yl + a * (zl - yl);
        zr = yr + a * (zr - yr);
        float inl = s.l[n], inr = s.r[n];
        if (ping_pong) {
            dl_push(&dl, (inl + inr) * 0.5f + zr * feedback);
            dl_push(&dr, zl * feedback);
        } else {
            dl_push(&dl, inl + zl * feedback);
            dl_push(&dr, inr + zr * feedback);
        }
        s.l[n] = inl + zl * mix;
        s.r[n] = inr + zr * mix;
    }
    dl_free(&dl); dl_free(&dr);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Chorus                                                             */
/* ------------------------------------------------------------------ */

sound fx_chorus(sound s, float rate_hz, float depth_ms, float mix)
{
    if (!s.frames) return s;
    const float rate = (float)audio.rate;
    float base = 0.012f * rate, depth = clampf(depth_ms, 0, 10) * 0.001f * rate;
    delay_line dl, dr;
    if (!dl_init(&dl, (uint32_t)(base + depth) + 4) || !dl_init(&dr, (uint32_t)(base + depth) + 4)) {
        dl_free(&dl); dl_free(&dr); return s;
    }
    float ph = 0, inc = 2.0f * (float)M_PI * rate_hz / rate;
    mix = clampf(mix, 0, 1);
    for (uint32_t n = 0; n < s.frames; n++) {
        dl_push(&dl, s.l[n]);
        dl_push(&dr, s.r[n]);
        float wl = 0, wr = 0;
        for (int v = 0; v < 2; v++) {   /* two voices per side, quadrature across sides */
            float o = v * (float)M_PI;
            wl += dl_read(&dl, base + depth * 0.5f * (1 + sinf(ph + o)));
            wr += dl_read(&dr, base + depth * 0.5f * (1 + sinf(ph + o + (float)M_PI / 2)));
        }
        ph += inc;
        if (ph > 2 * (float)M_PI) ph -= 2 * (float)M_PI;
        s.l[n] = s.l[n] * (1 - mix * 0.5f) + wl * 0.5f * mix;
        s.r[n] = s.r[n] * (1 - mix * 0.5f) + wr * 0.5f * mix;
    }
    dl_free(&dl); dl_free(&dr);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Drive: tanh with first-order antiderivative anti-aliasing (ADAA)  */
/* ------------------------------------------------------------------ */

static inline double log_cosh(double x)
{
    double a = fabs(x);
    return a > 20 ? a - 0.69314718055994531 : log(cosh(x));
}

sound fx_drive(sound s, float drive_db, float tone, float mix)
{
    if (!s.frames) return s;
    float g = db_to_gain(drive_db);
    float comp = 1.0f / sqrtf(g);
    float fc = 1500.0f + 16000.0f * clampf(tone, 0, 1);
    float a = expf(-2.0f * (float)M_PI * fc / audio.rate);
    mix = clampf(mix, 0, 1);
    float *ch[2] = { s.l, s.r };
    for (int c = 0; c < 2; c++) {
        double x1 = 0, F1 = 0;
        float z = 0;
        for (uint32_t n = 0; n < s.frames; n++) {
            double x = ch[c][n] * g;
            double F = log_cosh(x);
            double y = fabs(x - x1) > 1e-5 ? (F - F1) / (x - x1) : tanh(0.5 * (x + x1));
            x1 = x; F1 = F;
            z = (float)y + a * (z - (float)y);
            ch[c][n] = ch[c][n] * (1 - mix) + z * comp * mix;
        }
    }
    return s;
}

/* ------------------------------------------------------------------ */
/*  EQ: RBJ biquads                                                    */
/* ------------------------------------------------------------------ */

sound fx_eq(sound s, eq_kind kind, float freq, float gain_db, float q)
{
    if (!s.frames) return s;
    double w = 2 * M_PI * clampf(freq, 10, audio.rate * 0.49f) / audio.rate;
    double cw = cos(w), sw = sin(w);
    if (q <= 0) q = 0.707f;
    double alpha = sw / (2 * q), A = pow(10, gain_db / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (kind) {
    case EQ_HIGHPASS: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case EQ_BANDPASS: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case EQ_NOTCH:    b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case EQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
    case EQ_LOWSHELF: {
        double sa = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa); a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa; break;
    }
    case EQ_HIGHSHELF: {
        double sa = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa); a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa; break;
    }
    case EQ_LOWPASS:
    default:          b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    }
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    float *ch[2] = { s.l, s.r };
    for (int c = 0; c < 2; c++) {
        double z1 = 0, z2 = 0;                      /* transposed direct form II */
        for (uint32_t n = 0; n < s.frames; n++) {
            double x = ch[c][n];
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            ch[c][n] = (float)y;
        }
    }
    return s;
}

/* ------------------------------------------------------------------ */
/*  Dynamics                                                           */
/* ------------------------------------------------------------------ */

sound fx_compress(sound s, float threshold_db, float ratio, float attack_ms,
                  float release_ms, float makeup_db)
{
    if (!s.frames) return s;
    if (ratio < 1) ratio = 1;
    float ca = env_coef(fmaxf(attack_ms, 0.05f) * 0.001f, (float)audio.rate);
    float cr = env_coef(fmaxf(release_ms, 1.0f) * 0.001f, (float)audio.rate);
    float makeup = db_to_gain(makeup_db), knee = 6.0f;
    float gr = 0;                                   /* gain reduction in dB (>= 0) */
    for (uint32_t n = 0; n < s.frames; n++) {
        float peak = fmaxf(fabsf(s.l[n]), fabsf(s.r[n]));
        float lvl = 20.0f * log10f(peak + 1e-9f);
        float over = lvl - threshold_db, want;
        if (over <= -knee / 2)     want = 0;
        else if (over >= knee / 2) want = over * (1 - 1 / ratio);
        else { float x = over + knee / 2; want = (1 - 1 / ratio) * x * x / (2 * knee); }
        gr = want > gr ? want + (gr - want) * ca : want + (gr - want) * cr;
        float g = db_to_gain(-gr) * makeup;
        s.l[n] *= g; s.r[n] *= g;
    }
    return s;
}

sound fx_pump(sound s, uint32_t every_ticks, float depth, float release)
{
    if (!s.frames || !every_ticks) return s;
    uint32_t period = ticks_to_frames(every_ticks);
    depth = clampf(depth, 0, 1);
    float rel = fmaxf(release, 0.01f) * audio.rate;
    float dip = 0.004f * audio.rate;                /* 4 ms fade down */
    for (uint32_t n = 0; n < s.frames; n++) {
        float t = (float)(n % period);
        float g = t < dip ? 1.0f - depth * (t / dip)
                          : 1.0f - depth * expf(-4.0f * (t - dip) / rel);
        s.l[n] *= g; s.r[n] *= g;
    }
    return s;
}

/* Inter-sample ("true") peak between x[i] and x[i+1]: the waveform a DAC or
 * mp3 decoder reconstructs can overshoot the samples. Checked at 1/4, 1/2,
 * 3/4 with an 8-tap windowed sinc, like a 4x oversampled meter. */
static float tp_coef[3][8];
static int   tp_ready;

static void tp_init(void)
{
    if (tp_ready) return;
    for (int ph = 0; ph < 3; ph++) {
        double fr = (ph + 1) / 4.0, sum = 0;
        for (int k = 0; k < 8; k++) {
            double x = (k - 3) - fr;                    /* taps at i-3 .. i+4 */
            double sinc = sin(M_PI * x) / (M_PI * x);
            double w = 0.5 + 0.5 * cos(M_PI * x / 4.5);  /* Hann over the span */
            tp_coef[ph][k] = (float)(sinc * w);
            sum += tp_coef[ph][k];
        }
        for (int k = 0; k < 8; k++) tp_coef[ph][k] = (float)(tp_coef[ph][k] / sum);
    }
    tp_ready = 1;
}

static float true_peak_at(const float *x, uint32_t n, uint32_t i)
{
    float p = fabsf(x[i]);
    for (int ph = 0; ph < 3; ph++) {
        float acc = 0;
        for (int k = 0; k < 8; k++) {
            long j = (long)i + k - 3;
            if (j >= 0 && j < (long)n) acc += tp_coef[ph][k] * x[j];
        }
        if (fabsf(acc) > p) p = fabsf(acc);
    }
    return p;
}

sound fx_limit(sound s, float ceiling_db)
{
    tp_init();
    if (!s.frames) return s;
    float ceil_g = db_to_gain(ceiling_db);
    uint32_t la = audio.rate / 200;                 /* 5 ms look-ahead */
    if (la < 1) la = 1;
    uint32_t n = s.frames;
    float *need = malloc(n * sizeof(float)), *gmin = malloc(n * sizeof(float));
    if (!need || !gmin) { free(need); free(gmin); return s; }

    for (uint32_t i = 0; i < n; i++) {
        float p = fmaxf(true_peak_at(s.l, n, i), true_peak_at(s.r, n, i));
        need[i] = p > ceil_g ? ceil_g / p : 1.0f;
    }
    /* a peak between i and i+1 also constrains i+1 */
    for (uint32_t i = n - 1; i > 0; i--)
        need[i] = fminf(need[i], need[i - 1]);   /* backwards: uses the unmodified left neighbour */
    /* smallest gain needed anywhere in the next `la` samples (monotonic deque) */
    uint32_t *dq = malloc((n + 1) * sizeof(uint32_t));
    if (!dq) { free(need); free(gmin); return s; }
    uint32_t head = 0, tail = 0;
    for (uint32_t i = n; i-- > 0;) {
        while (tail > head && need[dq[tail - 1]] >= need[i]) tail--;
        dq[tail++] = i;
        while (dq[head] > i + la) head++;
        gmin[i] = need[dq[head]];
    }
    free(dq);
    /* release smoothing (gain recovers slowly, falls instantly) */
    float cr = env_coef(0.08f, (float)audio.rate), g = 1.0f;
    for (uint32_t i = 0; i < n; i++) {
        g = gmin[i] < g ? gmin[i] : gmin[i] + (g - gmin[i]) * cr;
        gmin[i] = g;
    }
    /* box-average over the look-ahead: smooth, and never above what's needed
     * at a peak because every sample in the window already saw that peak */
    double acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += gmin[i];
        if (i >= la) acc -= gmin[i - la];
        need[i] = (float)(acc / (i + 1 < la ? i + 1 : la));
    }
    for (uint32_t i = 0; i < n; i++) {
        float gg = fminf(need[i], 1.0f);
        s.l[i] *= gg; s.r[i] *= gg;
        /* safety: true ceiling even with rounding */
        s.l[i] = clampf(s.l[i], -ceil_g, ceil_g);
        s.r[i] = clampf(s.r[i], -ceil_g, ceil_g);
    }
    free(need); free(gmin);
    return s;
}
