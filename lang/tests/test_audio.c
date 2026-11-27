/*
 * test_audio.c — measurable properties of the sound engine.
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;              \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool all_finite(const sound *s)
{
    for (uint32_t i = 0; i < s->frames; i++)
        if (!isfinite(s->l[i]) || !isfinite(s->r[i])) return false;
    return true;
}

/* Fraction of energy (dB) below 18 kHz NOT within +-bw Hz of a harmonic of f0, measured
 * with a Hann-windowed DFT. Low = clean, high = aliasing. */
static double inharmonic_db(const float *x, uint32_t n, double f0, double bw)
{
    double total = 0, bad = 0;
    double res = (double)audio.rate / n;
    for (uint32_t k = 1; k < n / 2; k++) {
        double re = 0, im = 0, w = 2 * M_PI * k / n;
        for (uint32_t i = 0; i < n; i++) {
            double h = 0.5 - 0.5 * cos(2 * M_PI * i / n);
            re += x[i] * h * cos(w * i);
            im -= x[i] * h * sin(w * i);
        }
        double p = re * re + im * im, f = k * res;
        double nearest = round(f / f0) * f0;
        total += p;
        if (f > 18000) continue;                  /* only what folds into the audible band */
        if (fabs(f - nearest) > bw || nearest == 0) bad += p;
    }
    return 10 * log10(bad / total + 1e-30);
}

static synth_patch raw_osc(int wave)
{
    synth_patch p = synth_preset(PATCH_LEAD);
    memset(&p, 0, sizeof p);
    p.osc[0] = (osc_params){ .wave = (uint8_t)wave, .level = 1, .pulse_width = 0.5f, .unison = 1 };
    p.amp_env = (envelope){ 0, 0, 1, 0.01f };
    p.filter = FILTER_OFF;
    return p;
}

static void test_oscillators(void)
{
    /* A7 (3520 Hz) is the worst case: harmonics fold back hard if the
     * oscillator is naive. A5 (880 Hz) is a normal lead register. */
    const struct { note n; double limit; } pitches[] = { { 105, -50.0 }, { 81, -60.0 } };
    const int waves[] = { WAVE_SAW, WAVE_SQUARE, WAVE_TRIANGLE };
    const char *names[] = { "saw", "square", "triangle" };
    for (int pi = 0; pi < 2; pi++) {
        double f0 = 440.0 * pow(2.0, (pitches[pi].n - 69) / 12.0);
        for (int w = 0; w < 3; w++) {
            synth_patch p = raw_osc(waves[w]);
            part notes = part_new(0);
            part_note(&notes, 0, BEATS(1), pitches[pi].n, 127);
            sound s = synth_render(&p, &notes);
            double db = inharmonic_db(s.l + 4800, 4800, f0, 25.0);
            CHECK(db < pitches[pi].limit, "%s at %.0f Hz: %.1f dB inharmonic (aliasing)", names[w], f0, db);
            printf("  %-8s at %4.0f Hz: aliasing %.1f dB\n", names[w], f0, db);
            sound_free(&s);
            part_free(&notes);
        }
    }
}

static void test_filters_stable(void)
{
    /* max resonance, sweeping cutoff, loud input: must stay finite and bounded */
    const int kinds[] = { FILTER_LADDER, FILTER_LOWPASS, FILTER_BANDPASS, FILTER_HIGHPASS };
    for (int k = 0; k < 4; k++) {
        synth_patch p = synth_preset(PATCH_ACID_BASS);
        p.filter = (uint8_t)kinds[k];
        p.resonance = 1.0f; p.drive = 1.0f; p.filter_env_amount = 7; p.cutoff = 40;
        p.lfo_rate = 13; p.lfo_filter = 3;
        part notes = part_new(0);
        for (int i = 0; i < 16; i++) part_note(&notes, BEATS(i * 0.25), BEATS(0.2), (note)(24 + i * 5), 127);
        sound s = synth_render(&p, &notes);
        float peak = sound_peak(&s);
        CHECK(all_finite(&s) && peak < 8.0f, "filter %d unstable (peak %f)", kinds[k], peak);
        sound_free(&s);
        part_free(&notes);
    }
}

static void test_presets_render(void)
{
    part chords = part_new(0);
    chord c = { .notes = { 48, 55, 60, 64 }, .count = 4 };
    part_chord(&chords, 0, BEATS(2), &c, 100);
    for (int i = 0; i < PATCH_COUNT; i++) {
        synth_patch p = synth_preset((patch_preset)i);
        sound s = synth_render(&p, &chords);
        float peak = sound_peak(&s);
        CHECK(all_finite(&s) && peak > 0.02f && peak < 2.0f, "preset %d peak %f", i, peak);
        CHECK(s.length == ticks_to_frames(BEATS(2)), "preset %d length", i);
        sound_free(&s);
    }
    part_free(&chords);

    part hits = part_new(0);
    const int drums[] = { KICK, SNARE, CLAP, HAT, OPEN_HAT, TOM_LOW, TOM_HIGH, RIM, CRASH };
    for (int i = 0; i < 9; i++) part_note(&hits, BEATS(i * 0.5), BEATS(0.25), (note)drums[i], 110);
    for (int k = 0; k < KIT_COUNT; k++) {
        drum_kit kit = drums_preset((kit_preset)k);
        sound s = drums_render(&kit, &hits);
        float peak = sound_peak(&s);
        CHECK(all_finite(&s) && peak > 0.05f && peak < 2.0f, "kit %d peak %f", k, peak);
        sound_free(&s);
    }
    part_free(&hits);
}

static void test_edo_synth(void)
{
    /* a 31-EDO note renders at its 31-EDO frequency */
    synth_patch p = raw_osc(WAVE_SINE);
    part notes = part_new(0);
    note g = (note)(edo_c4(31) + 18);                 /* G4 in 31-EDO */
    part_note_edo(&notes, 0, BEATS(2), g, 31, 127);
    sound s = synth_render(&p, &notes);
    int crossings = 0;
    uint32_t a = 4800, b = 4800 + 24000;              /* 0.5 s */
    for (uint32_t i = a + 1; i < b; i++)
        if (s.l[i - 1] < 0 && s.l[i] >= 0) crossings++;
    double hz = crossings / 0.5, want = note_freq_edo(g, 31);
    CHECK(fabs(hz - want) < 3.0, "31-EDO G4: measured %.1f Hz, want %.1f", hz, want);
    sound_free(&s);
    part_free(&notes);
}

static void test_wav_roundtrip(void)
{
    sound s = sound_new(4800);
    for (uint32_t i = 0; i < s.frames; i++) {
        s.l[i] = 0.5f * sinf(i * 0.05f);
        s.r[i] = -0.25f * sinf(i * 0.031f);
    }
    char path[] = "/tmp/composer_test_XXXXXX.wav";
    int fd = mkstemps(path, 4);
    if (fd >= 0) close(fd);
    CHECK(sound_save(&s, path) == 0, "save wav");
    sound back = sound_load(path);
    CHECK(back.frames == s.frames, "frames %u vs %u", back.frames, s.frames);
    float err = 0;
    for (uint32_t i = 0; i < s.frames && i < back.frames; i++)
        err = fmaxf(err, fmaxf(fabsf(back.l[i] - s.l[i]), fabsf(back.r[i] - s.r[i])));
    CHECK(err < 1e-5f, "24-bit roundtrip error %g", err);
    unlink(path);
    sound_free(&s);
    sound_free(&back);
}

static void test_sampler(void)
{
    /* a 440 Hz sine sample, rooted at A4, played an octave up -> 880 Hz */
    sound smp = sound_new(48000);
    for (uint32_t i = 0; i < smp.frames; i++)
        smp.l[i] = smp.r[i] = 0.8f * sinf(2 * (float)M_PI * 440.0f * i / 48000.0f);
    sampler s = sampler_new(&smp, 69);
    part notes = part_new(0);
    part_note(&notes, 0, BEATS(1), 81, 127);
    sound out = sampler_render(&s, &notes);
    int crossings = 0;
    for (uint32_t i = 1001; i < 1000 + 9600; i++)
        if (out.l[i - 1] < 0 && out.l[i] >= 0) crossings++;
    double hz = crossings / 0.2;
    CHECK(fabs(hz - 880) < 10, "sampler +12: %.1f Hz", hz);
    CHECK(out.frames <= ticks_to_frames(BEATS(1)) + 24064, "pitched-up sample is shorter");
    sound_free(&out);
    part_free(&notes);

    /* equal chop */
    sound slices[8];
    int n = sound_chop(&smp, 4, slices, 8);
    uint32_t total = 0;
    for (int i = 0; i < n; i++) { total += slices[i].frames; }
    CHECK(n == 4 && total == smp.frames, "chop 4: %d slices, %u frames", n, total);
    for (int i = 0; i < n; i++) sound_free(&slices[i]);

    /* onset chop: 4 clicks at known places in silence */
    sound hits = sound_new(48000 * 2);
    const uint32_t at[4] = { 0, 24000, 48000, 72000 };
    for (int h = 0; h < 4; h++)
        for (uint32_t i = 0; i < 2400; i++) {
            float v = 0.9f * expf(-(float)i / 300.0f) * ((i * 7919u) % 200 / 100.0f - 1.0f);
            hits.l[at[h] + i] = hits.r[at[h] + i] = v;
        }
    sound found[16];
    n = sound_chop_onsets(&hits, 0.5f, found, 16);
    CHECK(n == 4, "onset chop found %d slices, expected 4", n);
    uint32_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (i < 4) CHECK(abs((int)pos - (int)at[i]) < 480, "slice %d starts at %u, hit at %u", i, pos, at[i]);
        pos += found[i].frames;
        sound_free(&found[i]);
    }

    /* slice mode: note base+1 plays slice 1 */
    sound sl[2] = { sound_new(100), sound_new(100) };
    sl[1].l[0] = sl[1].r[0] = 1.0f;
    sampler chopper = sampler_slices(sl, 2, 60);
    part p2 = part_new(0);
    part_note(&p2, 0, BEATS(1), 61, 127);
    sound o2 = sampler_render(&chopper, &p2);
    CHECK(sound_peak(&o2) > 0.1f, "slice mode plays slice 1");
    sound_free(&o2); sound_free(&sl[0]); sound_free(&sl[1]);
    part_free(&p2);
    sound_free(&hits);
    sound_free(&smp);
}

static void test_fx(void)
{
    sound s = sound_new(48000);
    s.l[100] = s.r[100] = 1.0f;                       /* impulse */
    uint32_t len = s.frames;
    s = fx_reverb(s, 0.6f, 2.0f, 0.4f, 1.0f);
    CHECK(all_finite(&s) && s.frames > len && s.length == len, "reverb extends frames only");
    double early = 0, late = 0;
    for (uint32_t i = 4800; i < 14400; i++) early += s.l[i] * s.l[i];
    for (uint32_t i = 48000 + 4800 * 6; i < 48000 + 4800 * 8; i++) late += s.l[i] * s.l[i];
    CHECK(late < early * 1e-3, "reverb decays (early %g late %g)", early, late);
    sound_free(&s);

    s = sound_new(48000);
    s.l[0] = s.r[0] = 1.0f;
    s = fx_delay(s, BEATS(0.5), 0.5f, 1.0f, 1.0f, false);
    uint32_t d = ticks_to_frames(BEATS(0.5));
    CHECK(fabsf(s.l[d]) > 0.3f, "delay echo at %u (got %f)", d, s.l[d]);
    sound_free(&s);

    /* limiter: loud sine never exceeds the ceiling */
    s = sound_new(48000);
    for (uint32_t i = 0; i < s.frames; i++) s.l[i] = s.r[i] = 3.0f * sinf(i * 0.01f) * (i > 20000 ? 1.0f : 0.2f);
    s = fx_limit(s, -1.0f);
    CHECK(sound_peak(&s) <= powf(10, -1.0f / 20) + 1e-6f, "limiter peak %f", sound_peak(&s));
    sound_free(&s);

    s = sound_new(48000);
    for (uint32_t i = 0; i < s.frames; i++) s.l[i] = s.r[i] = 0.9f * sinf(i * 0.2f);
    s = fx_drive(s, 18, 0.7f, 1.0f);
    s = fx_eq(s, EQ_PEAK, 1000, 6, 1);
    s = fx_compress(s, -18, 4, 5, 100, 3);
    s = fx_chorus(s, 0.8f, 3, 0.5f);
    s = fx_pump(s, BEATS(1), 0.7f, 0.25f);
    CHECK(all_finite(&s) && sound_peak(&s) < 4.0f, "fx chain finite");
    sound_free(&s);
}

static void test_song_timing(void)
{
    song sg = song_new();
    sound a = sound_silence(BEATS(1)), b = sound_silence(BEATS(1));
    b.l[0] = 1.0f;
    song_play(&sg, a);
    song_play(&sg, b);
    uint32_t beat = ticks_to_frames(BEATS(1));
    CHECK(sg.mix.l[beat] == 1.0f, "second section starts on beat 2");
    CHECK(sg.cursor == 2 * beat, "cursor after two beats: %u", sg.cursor);
    song_free(&sg);

    part p = part_new(0);
    part_pattern(&p, "x...|x...|x...|x...", KICK, BEATS(0.25), 100);
    CHECK(p.count == 4 && p.length == BEATS(4), "pattern: %u hits, length %u", p.count, p.length);
    p = part_repeat(p, 2);
    CHECK(p.count == 8 && p.events[4].start == BEATS(4), "repeat");
    part_free(&p);
}

static void test_export_formats(void)
{
    if (system("command -v ffmpeg >/dev/null 2>&1") != 0) { printf("  (ffmpeg not found, skipping mp3)\n"); return; }
    sound s = sound_new(9600);
    for (uint32_t i = 0; i < s.frames; i++) s.l[i] = s.r[i] = 0.5f * sinf(i * 0.06f);
    char path[] = "/tmp/composer_test_XXXXXX.mp3";
    int fd = mkstemps(path, 4);
    if (fd >= 0) close(fd);
    CHECK(sound_save(&s, path) == 0, "mp3 export");
    sound back = sound_load(path);
    CHECK(back.frames > 9000, "mp3 import (%u frames)", back.frames);
    unlink(path);
    sound_free(&s);
    sound_free(&back);
}

int main(void)
{
    test_oscillators();
    test_filters_stable();
    test_presets_render();
    test_edo_synth();
    test_wav_roundtrip();
    test_sampler();
    test_fx();
    test_song_timing();
    test_export_formats();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
