/*
 * synth.c — the subtractive/FM synth and the drum synth.
 *
 * Quality notes:
 *   - oscillators are band-limited (PolyBLEP / PolyBLAMP), so high notes
 *     don't alias into inharmonic fizz
 *   - filters are zero-delay-feedback (topology-preserving transform), so
 *     they stay stable and in tune under fast modulation; the ladder has
 *     tanh saturation in its feedback loop and self-oscillates at
 *     resonance 1 without blowing up
 *   - envelopes are exponential with an analog-style attack overshoot
 *   - unison voices start at random phases and slowly drift
 */
#include "audio_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Envelope                                                           */
/* ------------------------------------------------------------------ */

enum { ENV_ATTACK, ENV_DECAY, ENV_RELEASE, ENV_DONE };

typedef struct {
    int   stage;
    float level, ca, cd, cr, sustain;
} env_state;

static void env_init(env_state *e, const envelope *p, float rate)
{
    e->stage = ENV_ATTACK;
    e->level = 0;
    /* attack aims at 1.3 and stops at 1.0, like an analog envelope:
     * reaching 1.0 takes ln(1.3/0.3) = 1.466 time constants */
    float a = p->attack < 0.0005f ? 0.0005f : p->attack;
    e->ca = expf(-1.4663371f / (a * rate));
    e->cd = env_coef(p->decay, rate);
    e->cr = env_coef(p->release, rate);
    e->sustain = clampf(p->sustain, 0, 1);
}

static inline float env_tick(env_state *e)
{
    switch (e->stage) {
    case ENV_ATTACK:
        e->level = 1.3f + (e->level - 1.3f) * e->ca;
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ENV_DECAY; }
        break;
    case ENV_DECAY:
        e->level = e->sustain + (e->level - e->sustain) * e->cd;
        break;
    case ENV_RELEASE:
        e->level *= e->cr;
        if (e->level < 1e-5f) { e->level = 0; e->stage = ENV_DONE; }
        break;
    default:
        e->level = 0;
    }
    return e->level;
}

/* ------------------------------------------------------------------ */
/*  Oscillators                                                        */
/* ------------------------------------------------------------------ */

/* PolyBLEP: removes the aliasing of a step discontinuity. */
static inline float blep(double t, double dt)
{
    if (t < dt)       { double x = t / dt;          return (float)(x + x - x * x - 1.0); }
    if (t > 1.0 - dt) { double x = (t - 1.0) / dt;  return (float)(x * x + x + x + 1.0); }
    return 0.0f;
}

/* PolyBLAMP: the same for a corner (slope discontinuity), for triangles. */
static inline float blamp(double t, double dt)
{
    if (t < dt)       { double x = t / dt - 1.0;       return (float)(-x * x * x / 3.0); }
    if (t > 1.0 - dt) { double x = (t - 1.0) / dt + 1.0; return (float)(x * x * x / 3.0); }
    return 0.0f;
}

static inline double frac(double x) { return x - floor(x); }

static inline float osc_wave(int wave, double t, double dt, float pw, rng *r)
{
    switch (wave) {
    case WAVE_SAW:
        return (float)(2.0 * t - 1.0) - blep(t, dt);
    case WAVE_SQUARE:
        return (t < pw ? 1.0f : -1.0f) + blep(t, dt) - blep(frac(t + 1.0 - pw), dt);
    case WAVE_TRIANGLE: {
        float y = (float)(4.0 * fabs(t - 0.5) - 1.0);
        /* corners at the peak (t=0) and trough (t=.5): the slope flips by
         * 8*dt per sample; blep() is scaled for a step of 2, so blamp()
         * is for a slope change of 2 per sample, hence 4*dt */
        return y + 4.0f * (float)dt * (blamp(frac(t + 0.5), dt) - blamp(t, dt));
    }
    case WAVE_SINE:
        return sinf((float)(2.0 * M_PI * t));
    case WAVE_NOISE:
        return rng_bipolar(r);
    }
    return 0.0f;
}

/* ------------------------------------------------------------------ */
/*  Filters (zero-delay feedback)                                      */
/* ------------------------------------------------------------------ */

typedef struct { float s[4]; } ladder_state;
typedef struct { float ic1, ic2; } svf_state;

/* 4-pole ladder (24 dB/oct). g = tan(pi fc / fs), k = 0..4. */
static inline float ladder_tick(ladder_state *f, float x, float g, float k)
{
    float G = g / (1.0f + g);
    float b = 1.0f - G;
    float S = b * (G * G * G * f->s[0] + G * G * f->s[1] + G * f->s[2] + f->s[3]);
    float u = (x * (1.0f + 0.45f * k) - k * S) / (1.0f + k * G * G * G * G);
    u = tanhf(u);                                  /* saturating feedback */
    for (int i = 0; i < 4; i++) {
        float v = (u - f->s[i]) * G;
        float y = v + f->s[i];
        f->s[i] = y + v;
        u = y;
    }
    return u;
}

/* State-variable filter (12 dB/oct). Returns the chosen output. */
static inline float svf_tick(svf_state *f, float x, float g, float k, int kind)
{
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1, a3 = g * a2;
    float v3 = x - f->ic2;
    float v1 = a1 * f->ic1 + a2 * v3;
    float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    switch (kind) {
    case FILTER_BANDPASS: return v1 * k;           /* unity gain at the peak */
    case FILTER_HIGHPASS: return x - k * v1 - v2;
    default:              return v2;
    }
}

/* ------------------------------------------------------------------ */
/*  2x oversampling: voices run at twice the sample rate and come back */
/*  down through a half-band low-pass, so oscillator and saturation    */
/*  harmonics above the audible band don't fold back into it.          */
/* ------------------------------------------------------------------ */

#define HB_TAPS  63
#define HB_DELAY 15                 /* output frames of latency to compensate */

static float hb_coef[HB_TAPS];
static int   hb_ready;

static void hb_init(void)
{
    if (hb_ready) return;
    const double beta = 8.0;
    double sum = 0;
    for (int n = 0; n < HB_TAPS; n++) {
        int m = n - HB_TAPS / 2;
        double x = m / 2.0;
        double sinc = m == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double r = (double)m / (HB_TAPS / 2);
        double i0a = 1, t = 1, i0b = 1, u = 1, a = beta * sqrt(1 - r * r);
        for (int k = 1; k < 25; k++) {
            t *= (a / (2 * k)) * (a / (2 * k)); i0a += t;
            u *= (beta / (2 * k)) * (beta / (2 * k)); i0b += u;
        }
        hb_coef[n] = (float)(0.5 * sinc * i0a / i0b);
        sum += hb_coef[n];
    }
    for (int n = 0; n < HB_TAPS; n++) hb_coef[n] = (float)(hb_coef[n] / sum);
    hb_ready = 1;
}

/* Each sample is stored twice so the last HB_TAPS samples are always one
 * contiguous run (no wrap-around in the inner loop). */
typedef struct { float x[2][2 * HB_TAPS]; int pos; } halfband;

static inline void hb_push(halfband *h, float l, float r)
{
    h->x[0][h->pos] = h->x[0][h->pos + HB_TAPS] = l;
    h->x[1][h->pos] = h->x[1][h->pos + HB_TAPS] = r;
    h->pos = (h->pos + 1) % HB_TAPS;
}

static inline void hb_out(const halfband *h, float *l, float *r)
{
    const float *a = h->x[0] + h->pos, *b = h->x[1] + h->pos;    /* oldest first */
    const int c = HB_TAPS / 2;
    float sl = hb_coef[c] * a[c], sr = hb_coef[c] * b[c];
    /* half-band: every other tap is zero, and the rest are symmetric */
    for (int k = 1; k <= c; k += 2) {
        sl += hb_coef[c + k] * (a[c + k] + a[c - k]);
        sr += hb_coef[c + k] * (b[c + k] + b[c - k]);
    }
    *l = sl; *r = sr;
}

/* ------------------------------------------------------------------ */
/*  One synth voice                                                    */
/* ------------------------------------------------------------------ */

#define MAX_UNISON 7

typedef struct {
    double ratio[MAX_UNISON];   /* frequency multiplier of each unison voice */
    double phase[MAX_UNISON];
    float  gl[MAX_UNISON], gr[MAX_UNISON];
    int    n;
} osc_voice;

static void osc_voice_init(osc_voice *v, const osc_params *p, rng *r)
{
    v->n = p->unison < 1 ? 1 : p->unison > MAX_UNISON ? MAX_UNISON : p->unison;
    float norm = 1.0f / sqrtf((float)v->n);
    for (int i = 0; i < v->n; i++) {
        float pos = v->n > 1 ? (float)i / (v->n - 1) * 2.0f - 1.0f : 0.0f;   /* -1..1 */
        float cents = p->semitones * 100.0f + p->cents + pos * p->unison_detune * 0.5f;
        v->ratio[i] = pow(2.0, cents / 1200.0);
        v->phase[i] = v->n > 1 ? rng_float(r) : 0.0;
        float pan = pos * clampf(p->unison_width, 0, 1);
        float ang = (pan + 1.0f) * (float)M_PI / 4.0f;               /* equal power */
        v->gl[i] = cosf(ang) * norm * (float)M_SQRT2;
        v->gr[i] = sinf(ang) * norm * (float)M_SQRT2;
    }
}

static float note_hz(note n, uint8_t edo) { return (float)note_freq_edo(n, edo); }

/* Render one note into out at frame `at`. Returns the frames written. */
static uint32_t render_voice(const synth_patch *p, const note_event *e, float from_hz,
                             sound *out, uint32_t at, uint32_t seed)
{
    hb_init();
    const float rate = 2.0f * (float)audio.rate;   /* internal, oversampled */
    rng r = { seed * 2654435761u + 1 };

    float hz = note_hz(e->pitch, e->edo);
    float vel = e->velocity / 127.0f;
    float amp_vel = 1.0f - p->velocity_to_amp + p->velocity_to_amp * vel * vel;
    float base_gain = db_to_gain(p->gain) * amp_vel * 0.5f;
    float pan = clampf(p->pan, -1, 1);
    float pan_l = cosf((pan + 1) * (float)M_PI / 4) * (float)M_SQRT2;
    float pan_r = sinf((pan + 1) * (float)M_PI / 4) * (float)M_SQRT2;

    osc_voice ov[2];
    for (int o = 0; o < 2; o++) osc_voice_init(&ov[o], &p->osc[o], &r);
    double sub_phase = 0;

    env_state amp, fenv;
    env_init(&amp, &p->amp_env, rate);
    env_init(&fenv, &p->filter_env, rate);

    ladder_state lad[2] = { 0 };
    svf_state svf[2] = { 0 };
    float k_ladder = 4.0f * clampf(p->resonance, 0, 1);
    float k_svf = 2.0f - 1.98f * clampf(p->resonance, 0, 1);
    float drive = 1.0f + 4.0f * clampf(p->drive, 0, 1);
    float key_oct = p->key_track * log2f(hz / 261.6256f);
    float vel_oct = p->velocity_to_filter * vel;

    /* glide: start at the previous note's pitch and slide in */
    float glide_oct = (p->glide > 0 && from_hz > 0) ? log2f(from_hz / hz) : 0.0f;
    float glide_coef = p->glide > 0 ? env_coef(p->glide, rate) : 0.0f;

    /* analog drift: a static offset plus a slow wander */
    float drift_static = rng_bipolar(&r) * p->drift * 0.5f;
    float drift = 0, drift_target = 0;

    double lfo_phase = frac((double)at / audio.rate * p->lfo_rate);
    double lfo_inc = p->lfo_rate / rate;

    uint32_t gate = 2 * ticks_to_frames(e->length);              /* internal samples */
    if (gate < 2) gate = 2;
    uint32_t max_frames = gate / 2 + (uint32_t)(p->amp_env.release * 1.5f * audio.rate) + 64;
    if (at + max_frames > out->frames && !sound_resize(out, at + max_frames)) return 0;

    halfband hb = { 0 };
    uint32_t i, flush = 0;
    for (i = 0; i < 2 * max_frames; i++) {
        if (i == gate) { amp.stage = ENV_RELEASE; fenv.stage = ENV_RELEASE; }
        float a = env_tick(&amp);
        float fe = env_tick(&fenv);
        if (amp.stage == ENV_DONE && ++flush > HB_TAPS + 2) break;

        float lfo = sinf((float)(2.0 * M_PI * lfo_phase));
        lfo_phase = frac(lfo_phase + lfo_inc);

        if ((i & 4095) == 0) drift_target = rng_bipolar(&r);
        drift += (drift_target - drift) * 0.0002f;
        glide_oct *= glide_coef;

        float cents = drift_static + drift * p->drift + lfo * p->lfo_pitch;
        double f = hz * exp2((double)glide_oct + cents / 1200.0);

        float pw = clampf(p->osc[0].pulse_width + lfo * p->lfo_pulse_width, 0.05f, 0.95f);
        float pw2 = clampf(p->osc[1].pulse_width + lfo * p->lfo_pulse_width, 0.05f, 0.95f);

        /* osc 2 first: it can modulate osc 1 */
        float l = 0, rr = 0, mod = 0;
        for (int u = 0; u < ov[1].n; u++) {
            double dt = f * ov[1].ratio[u] / rate;
            float s = osc_wave(p->osc[1].wave, ov[1].phase[u], dt, pw2, &r);
            if (u == 0) mod = s;
            l += s * ov[1].gl[u] * p->osc[1].level;
            rr += s * ov[1].gr[u] * p->osc[1].level;
            ov[1].phase[u] = frac(ov[1].phase[u] + dt);
        }
        float fm = p->fm * (1.0f - p->fm_env + p->fm_env * fe);
        for (int u = 0; u < ov[0].n; u++) {
            double dt = f * ov[0].ratio[u] / rate;
            double t = fm != 0 ? frac(ov[0].phase[u] + fm * mod / (2.0 * M_PI)) : ov[0].phase[u];
            float s = osc_wave(p->osc[0].wave, t, dt, pw, &r);
            l += s * ov[0].gl[u] * p->osc[0].level;
            rr += s * ov[0].gr[u] * p->osc[0].level;
            ov[0].phase[u] = frac(ov[0].phase[u] + dt);
        }
        if (p->sub > 0) {
            float s = sinf((float)(2.0 * M_PI * sub_phase)) * p->sub;
            sub_phase = frac(sub_phase + f * ov[0].ratio[0] * 0.5 / rate);
            l += s; rr += s;
        }
        if (p->noise > 0) {
            float s = rng_bipolar(&r) * p->noise;
            l += s; rr += s;
        }

        if (p->filter != FILTER_OFF) {
            float oct = key_oct + vel_oct + fe * p->filter_env_amount + lfo * p->lfo_filter;
            float fc = clampf(p->cutoff * exp2f(oct), 16.0f, rate * 0.47f);
            float g = tanf((float)M_PI * fc / rate);
            if (p->filter == FILTER_LADDER) {
                l = ladder_tick(&lad[0], l * drive * 0.5f, g, k_ladder) * 2.0f / sqrtf(drive);
                rr = ladder_tick(&lad[1], rr * drive * 0.5f, g, k_ladder) * 2.0f / sqrtf(drive);
            } else {
                if (p->drive > 0) { l = tanhf(l * drive) / drive * 1.5f; rr = tanhf(rr * drive) / drive * 1.5f; }
                l = svf_tick(&svf[0], l, g, k_svf, p->filter);
                rr = svf_tick(&svf[1], rr, g, k_svf, p->filter);
            }
        } else if (p->drive > 0) {
            l = tanhf(l * drive) / sqrtf(drive);
            rr = tanhf(rr * drive) / sqrtf(drive);
        }

        float g = a * base_gain;
        hb_push(&hb, l * g * pan_l, rr * g * pan_r);
        if (i & 1) {                                /* every 2nd: one output frame */
            long o = (long)at + (long)(i >> 1) - HB_DELAY;
            if (o >= 0 && (uint32_t)o < out->frames) {
                float yl, yr;
                hb_out(&hb, &yl, &yr);
                out->l[o] += yl;
                out->r[o] += yr;
            }
        }
    }
    return i / 2;
}

static int by_start(const void *a, const void *b)
{
    const note_event *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

sound synth_render(const synth_patch *p, const part *notes)
{
    audio_fp_setup();
    uint32_t len = ticks_to_frames(notes->length);
    sound out = sound_new(len ? len : 1);
    out.length = len;
    if (!notes->count) return out;

    note_event *ev = malloc(notes->count * sizeof *ev);
    if (!ev) return out;
    memcpy(ev, notes->events, notes->count * sizeof *ev);
    qsort(ev, notes->count, sizeof *ev, by_start);

    float prev_hz = 0;
    uint32_t end = 0;
    for (uint32_t i = 0; i < notes->count; i++) {
        uint32_t at = ticks_to_frames(ev[i].start);
        uint32_t n = render_voice(p, &ev[i], prev_hz, &out, at, i + ev[i].pitch * 977u + ev[i].start);
        if (at + n > end) end = at + n;
        prev_hz = note_hz(ev[i].pitch, ev[i].edo);
    }
    free(ev);
    if (end > out.length) out.frames = end;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Presets                                                            */
/* ------------------------------------------------------------------ */

/* OSC() sets defaults that the extra arguments may override. */
#pragma GCC diagnostic ignored "-Woverride-init"
#define OSC(w, lvl, ...) { .wave = (w), .level = (lvl), .pulse_width = 0.5f, .unison = 1, __VA_ARGS__ }

synth_patch synth_preset(patch_preset preset)
{
    synth_patch p = {
        .osc = { OSC(WAVE_SAW, 1.0f), OSC(WAVE_SAW, 0.0f) },
        .filter = FILTER_LADDER, .cutoff = 2000, .resonance = 0.2f,
        .amp_env = { 0.005f, 0.3f, 0.8f, 0.3f },
        .filter_env = { 0.005f, 0.3f, 0.5f, 0.3f },
        .velocity_to_amp = 0.5f, .drift = 2.0f, .gain = -10,
    };
    switch (preset) {
    case PATCH_SUPERSAW:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 1.0f, .unison = 7, .unison_detune = 38, .unison_width = 1.0f);
        p.osc[1] = (osc_params)OSC(WAVE_SAW, 0.35f, .semitones = -12, .unison = 5, .unison_detune = 22, .unison_width = 0.7f);
        p.filter = FILTER_LOWPASS; p.cutoff = 5200; p.resonance = 0.12f; p.key_track = 0.3f;
        p.filter_env_amount = 0.8f; p.filter_env = (envelope){ 0.002f, 0.35f, 0.3f, 0.4f };
        p.amp_env = (envelope){ 0.006f, 0.4f, 0.85f, 0.35f };
        p.drift = 3; p.gain = -13;
        break;
    case PATCH_WARM_PAD:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 0.9f, .unison = 5, .unison_detune = 18, .unison_width = 0.8f);
        p.osc[1] = (osc_params)OSC(WAVE_SQUARE, 0.5f, .semitones = -12, .pulse_width = 0.4f, .unison = 3, .unison_detune = 12, .unison_width = 0.6f);
        p.filter = FILTER_LADDER; p.cutoff = 900; p.resonance = 0.25f; p.key_track = 0.4f;
        p.filter_env_amount = 1.3f; p.filter_env = (envelope){ 1.2f, 2.0f, 0.4f, 2.0f };
        p.amp_env = (envelope){ 0.9f, 1.0f, 0.85f, 2.2f };
        p.lfo_rate = 0.3f; p.lfo_filter = 0.3f; p.lfo_pulse_width = 0.15f;
        p.drift = 4; p.velocity_to_amp = 0.2f; p.gain = -14;
        break;
    case PATCH_ACID_BASS:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 1.0f);
        p.filter = FILTER_LADDER; p.cutoff = 280; p.resonance = 0.82f; p.drive = 0.55f;
        p.filter_env_amount = 3.4f; p.filter_env = (envelope){ 0.001f, 0.22f, 0.0f, 0.1f };
        p.velocity_to_filter = 1.6f;
        p.amp_env = (envelope){ 0.001f, 0.3f, 0.9f, 0.06f };
        p.glide = 0.06f; p.drift = 1; p.gain = -9;
        break;
    case PATCH_SUB_BASS:
        p.osc[0] = (osc_params)OSC(WAVE_SINE, 1.0f);
        p.osc[1] = (osc_params)OSC(WAVE_TRIANGLE, 0.2f, .semitones = 12);
        p.filter = FILTER_LADDER; p.cutoff = 700; p.resonance = 0.0f; p.drive = 0.2f;
        p.amp_env = (envelope){ 0.004f, 0.2f, 1.0f, 0.12f };
        p.drift = 0.5f; p.velocity_to_amp = 0.3f; p.gain = -5;
        break;
    case PATCH_REESE:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 0.8f, .unison = 3, .unison_detune = 24, .unison_width = 0.5f);
        p.osc[1] = (osc_params)OSC(WAVE_SAW, 0.8f, .cents = -9, .unison = 3, .unison_detune = 31, .unison_width = 0.5f);
        p.sub = 0.45f;
        p.filter = FILTER_LADDER; p.cutoff = 650; p.resonance = 0.35f; p.drive = 0.6f;
        p.lfo_rate = 0.25f; p.lfo_filter = 0.6f;
        p.amp_env = (envelope){ 0.005f, 0.2f, 1.0f, 0.15f };
        p.velocity_to_amp = 0.2f; p.gain = -13;
        break;
    case PATCH_PLUCK:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 1.0f, .unison = 3, .unison_detune = 12, .unison_width = 0.6f);
        p.osc[1] = (osc_params)OSC(WAVE_SQUARE, 0.3f, .semitones = 12, .pulse_width = 0.3f);
        p.filter = FILTER_LADDER; p.cutoff = 380; p.resonance = 0.35f; p.key_track = 0.5f;
        p.filter_env_amount = 4.5f; p.filter_env = (envelope){ 0.001f, 0.18f, 0.0f, 0.15f };
        p.velocity_to_filter = 1.0f;
        p.amp_env = (envelope){ 0.001f, 0.5f, 0.0f, 0.3f };
        p.gain = -10;
        break;
    case PATCH_LEAD:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 0.9f, .unison = 3, .unison_detune = 10, .unison_width = 0.4f);
        p.osc[1] = (osc_params)OSC(WAVE_SQUARE, 0.6f, .cents = 6, .pulse_width = 0.45f);
        p.filter = FILTER_LADDER; p.cutoff = 2000; p.resonance = 0.3f; p.key_track = 0.5f;
        p.filter_env_amount = 1.5f; p.filter_env = (envelope){ 0.01f, 0.4f, 0.3f, 0.3f };
        p.amp_env = (envelope){ 0.004f, 0.2f, 0.85f, 0.25f };
        p.lfo_rate = 5.5f; p.lfo_pitch = 12;
        p.glide = 0.08f; p.gain = -12;
        break;
    case PATCH_FM_KEYS:
        p.osc[0] = (osc_params)OSC(WAVE_SINE, 1.0f);
        p.osc[1] = (osc_params)OSC(WAVE_SINE, 0.0f);         /* modulator only */
        p.fm = 2.4f; p.fm_env = 0.85f;
        p.filter = FILTER_OFF;
        p.filter_env = (envelope){ 0.001f, 1.1f, 0.12f, 0.5f };
        p.amp_env = (envelope){ 0.002f, 2.8f, 0.2f, 0.5f };
        p.velocity_to_amp = 0.7f; p.drift = 1; p.gain = -8;
        break;
    case PATCH_BRASS:
        p.osc[0] = (osc_params)OSC(WAVE_SAW, 0.9f, .unison = 3, .unison_detune = 8, .unison_width = 0.5f);
        p.osc[1] = (osc_params)OSC(WAVE_SAW, 0.7f, .cents = 7);
        p.filter = FILTER_LADDER; p.cutoff = 480; p.resonance = 0.15f; p.key_track = 0.5f;
        p.filter_env_amount = 2.6f; p.filter_env = (envelope){ 0.08f, 0.5f, 0.55f, 0.2f };
        p.velocity_to_filter = 1.2f;
        p.amp_env = (envelope){ 0.04f, 0.3f, 0.85f, 0.2f };
        p.lfo_rate = 5.0f; p.lfo_pitch = 6; p.gain = -12;
        break;
    default:
        break;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/*  Drum synth                                                         */
/* ------------------------------------------------------------------ */

drum_kit drums_preset(kit_preset preset)
{
    switch (preset) {
    case KIT_PUNCHY:
        return (drum_kit){ 57, 0.85f, 0.38f, 0.5f, 205, 0.75f, 0.17f, 0.045f, 0.8f, -4 };
    case KIT_LOFI:
        return (drum_kit){ 52, 0.5f, 0.45f, 0.35f, 170, 0.5f, 0.2f, 0.07f, 0.3f, -4 };
    case KIT_808:
    default:
        return (drum_kit){ 47, 0.6f, 0.9f, 0.2f, 185, 0.65f, 0.22f, 0.06f, 0.6f, -4 };
    }
}

/* 808-style metallic partials for hats and cymbals. */
static const float metal_hz[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

static float metal(double t, const float *hz, float scale)
{
    float s = 0;
    for (int i = 0; i < 6; i++) {
        double ph = frac(t * hz[i] * scale);
        double dt = hz[i] * scale / audio.rate;
        s += (ph < 0.5 ? 1.0f : -1.0f) + blep(ph, dt) - blep(frac(ph + 0.5), dt);
    }
    return s / 6.0f;
}

/* Render one drum hit. Returns frames written. */
static uint32_t render_hit(const drum_kit *k, int which, float vel, sound *out, uint32_t at, uint32_t seed)
{
    const float rate = (float)audio.rate;
    rng r = { seed * 2246822519u + 7 };
    float len_s;
    switch (which) {
    case KICK: case TOM_LOW: case TOM_HIGH: len_s = (which == KICK ? k->kick_decay : 0.45f) * 1.2f + 0.05f; break;
    case SNARE: len_s = k->snare_decay * 1.3f + 0.05f; break;
    case CLAP:  len_s = 0.45f; break;
    case HAT:   len_s = k->hat_decay * 1.4f + 0.02f; break;
    case OPEN_HAT: len_s = k->hat_decay * 7.0f + 0.05f; break;
    case CRASH: len_s = 2.2f; break;
    case RIM:   len_s = 0.06f; break;
    default: return 0;
    }
    uint32_t n = (uint32_t)(len_s * rate);
    if (at + n > out->frames && !sound_resize(out, at + n)) return 0;

    float gain = db_to_gain(k->gain) * powf(vel, 1.3f);
    svf_state f1 = { 0 }, f2 = { 0 };
    double phase = 0;
    float g_hp = tanf((float)M_PI * clampf(1500.0f + 5000.0f * k->tone, 20, rate * 0.45f) / rate);
    float g_hat = tanf((float)M_PI * 7500.0f / rate);
    float g_hat_bp = tanf((float)M_PI * clampf(9000.0f + 3000.0f * k->tone, 20, rate * 0.45f) / rate);
    float g_clap = tanf((float)M_PI * 1150.0f / rate);
    float g_rim = tanf((float)M_PI * 1700.0f / rate);
    float g_crash = tanf((float)M_PI * 4500.0f / rate);
    float kick_drive = 1.0f + 5.0f * k->kick_drive;

    for (uint32_t i = 0; i < n; i++) {
        float t = i / rate, s = 0;
        switch (which) {
        case KICK: case TOM_LOW: case TOM_HIGH: {
            float base = which == KICK ? k->kick_tune : which == TOM_LOW ? 105.0f : 165.0f;
            float punch = which == KICK ? k->kick_punch : 0.35f;
            float decay = which == KICK ? k->kick_decay : 0.4f;
            float f = base * (1.0f + punch * 5.0f * expf(-t / 0.028f));
            phase = frac(phase + f / rate);
            float body = sinf((float)(2.0 * M_PI * phase)) * expf(-6.9f * t / decay);
            float click = rng_bipolar(&r) * expf(-t / 0.0018f) * 0.35f * (0.4f + k->tone);
            s = body + click;
            if (which == KICK) s = tanhf(s * kick_drive) / tanhf(kick_drive);
            else s *= 0.8f;
            break;
        }
        case SNARE: {
            float f = k->snare_tune * (1.0f + 0.4f * expf(-t / 0.012f));
            phase = frac(phase + f / rate);
            float body = (sinf((float)(2.0 * M_PI * phase)) + 0.45f * sinf((float)(2.0 * M_PI * phase * 1.62)))
                         * expf(-t / 0.045f) * (1.0f - 0.5f * k->snare_snap);
            float nz = svf_tick(&f1, rng_bipolar(&r), g_hp, 1.2f, FILTER_HIGHPASS);
            nz *= expf(-6.9f * t / k->snare_decay) * k->snare_snap * 1.3f;
            s = body * 0.8f + nz;
            break;
        }
        case CLAP: {
            float env = 0;
            for (int b = 0; b < 3; b++) {
                float tb = t - b * 0.011f;
                if (tb >= 0) env += expf(-tb / 0.0035f);
            }
            if (t > 0.03f) env += 0.6f * expf(-6.9f * (t - 0.03f) / 0.28f);
            s = svf_tick(&f1, rng_bipolar(&r), g_clap, 0.7f, FILTER_BANDPASS) * env * 1.4f;
            break;
        }
        case HAT: case OPEN_HAT: case CRASH: {
            float m = metal(t, metal_hz, which == CRASH ? 1.4f : 1.0f) + rng_bipolar(&r) * 0.35f;
            float decay = which == HAT ? k->hat_decay : which == OPEN_HAT ? k->hat_decay * 6.0f : 1.8f;
            float x = svf_tick(&f1, m, which == CRASH ? g_crash : g_hat_bp, 1.0f, FILTER_BANDPASS);
            x = svf_tick(&f2, x, g_hat, 1.4f, FILTER_HIGHPASS);
            s = x * expf(-6.9f * t / decay) * (which == CRASH ? 0.9f : 1.2f);
            break;
        }
        case RIM: {
            phase = frac(phase + 1700.0 / rate);
            s = (sinf((float)(2.0 * M_PI * phase)) * 0.6f
                 + svf_tick(&f1, rng_bipolar(&r), g_rim, 0.8f, FILTER_BANDPASS)) * expf(-t / 0.009f);
            break;
        }
        }
        s *= gain;
        /* tiny stereo placement so the kit isn't a mono point */
        float pan = which == HAT || which == OPEN_HAT ? 0.25f : which == TOM_HIGH ? -0.3f
                  : which == TOM_LOW ? 0.3f : which == CRASH ? -0.2f : 0.0f;
        out->l[at + i] += s * (pan > 0 ? 1.0f - pan : 1.0f);
        out->r[at + i] += s * (pan < 0 ? 1.0f + pan : 1.0f);
    }
    return n;
}

sound drums_render(const drum_kit *k, const part *hits)
{
    audio_fp_setup();
    uint32_t len = ticks_to_frames(hits->length);
    sound out = sound_new(len ? len : 1);
    out.length = len;
    uint32_t end = 0;
    for (uint32_t i = 0; i < hits->count; i++) {
        const note_event *e = &hits->events[i];
        uint32_t at = ticks_to_frames(e->start);
        uint32_t n = render_hit(k, e->pitch, e->velocity / 127.0f, &out, at, i * 31u + e->start);
        if (at + n > end) end = at + n;
    }
    if (end > out.length) out.frames = end;

    /* overall tone: gentle one-pole low-pass */
    float fc = 3000.0f + 17000.0f * k->tone;
    float a = expf(-2.0f * (float)M_PI * fc / audio.rate);
    float yl = 0, yr = 0;
    for (uint32_t i = 0; i < out.frames; i++) {
        yl = out.l[i] + a * (yl - out.l[i]);
        yr = out.r[i] + a * (yr - out.r[i]);
        out.l[i] = yl; out.r[i] = yr;
    }
    return out;
}
