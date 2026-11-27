/*
 * demo_song.c — a whole song, written the way the language is meant to
 * read, in plain C until the interpreter exists.
 *
 *   - main() takes no arguments and just plays sections in order
 *   - sections (intro, verse, chorus, bridge) are functions returning sound
 *   - instruments and effects compose: fx_reverb(fx_eq(synth_render(...)))
 *   - parts (notes) are built from the key with scale degrees and chords
 *   - the bridge is in 31-EDO, with harmonic-seventh chords
 *   - a drum break is rendered, saved, re-imported, chopped at its hits and
 *     re-sequenced, like a sampled break
 *
 * Build: cmake --build build && ./build/lang/demo_song
 * Writes demo_song.wav (and demo_song.mp3 if ffmpeg is installed).
 */
#include "audio.h"
#include "music.h"

#include <stdio.h>

static scale key;                    /* A minor, set in main */

/* ------------------------------------------------------------------ */
/*  Notes                                                              */
/* ------------------------------------------------------------------ */

/* i - VI - III - VII, one chord per bar. */
static const int progression_degrees[4] = { 0, 5, 2, 6 };

static chord bar_chord(int bar, int size, int octave)
{
    chord c = chord_from_scale(&key, progression_degrees[bar % 4], size);
    for (int i = 0; i < c.count; i++) c.notes[i] = (note)(c.notes[i] + 12 * octave);
    return c;
}

static part chords(int size, int octave, uint8_t velocity)
{
    part p = part_new(BARS(4));
    for (int bar = 0; bar < 4; bar++) {
        chord c = bar_chord(bar, size, octave);
        part_chord(&p, BARS(bar), BARS(1), &c, velocity);
    }
    return p;
}

/* 16th-note acid line on each chord's root: 0 = root, 12 = octave, -1 = rest. */
static part acid_line(void)
{
    static const int steps[16] = { 0, -1, 0, 12, -1, 0, -1, 0, 0, -1, 12, -1, 0, 7, 0, 12 };
    static const uint8_t vel[16] = { 120, 0, 80, 110, 0, 85, 0, 115, 80, 0, 120, 0, 85, 90, 80, 127 };
    part p = part_new(BARS(4));
    for (int bar = 0; bar < 4; bar++) {
        note root = (note)(bar_chord(bar, 1, -2).notes[0]);
        for (int s = 0; s < 16; s++)
            if (steps[s] >= 0)
                part_note(&p, BARS(bar) + BEATS(s * 0.25), BEATS(0.22), (note)(root + steps[s]), vel[s]);
    }
    return p;
}

static part sub_line(void)
{
    part p = part_new(BARS(4));
    for (int bar = 0; bar < 4; bar++) {
        note root = bar_chord(bar, 1, -2).notes[0];
        part_note(&p, BARS(bar), BEATS(2.5), root, 110);
        part_note(&p, BARS(bar) + BEATS(2.75), BEATS(1.0), root, 95);
    }
    return p;
}

/* 16th arpeggio over each chord's tones, an octave up. */
static part arpeggio(void)
{
    static const int order[8] = { 0, 1, 2, 3, 2, 1, 2, 3 };
    part p = part_new(BARS(4));
    for (int bar = 0; bar < 4; bar++) {
        chord c = bar_chord(bar, 4, 1);
        for (int s = 0; s < 16; s++)
            part_note(&p, BARS(bar) + BEATS(s * 0.25), BEATS(0.2), c.notes[order[s % 8]],
                      (uint8_t)(s % 4 == 0 ? 110 : 80));
    }
    return p;
}

/* The chorus hook, as scale degrees (7 = the A an octave above the root). */
static part hook(void)
{
    static const struct { int degree; float beat, len; } notes[] = {
        { 7, 0, 1.5f }, { 9, 1.5f, 0.5f }, { 8, 2, 1 }, { 7, 3, 0.5f }, { 6, 3.5f, 0.5f },
        { 5, 4, 1.5f }, { 7, 5.5f, 0.5f }, { 9, 6, 1 }, { 11, 7, 1 },
        { 11, 8, 1.5f }, { 10, 9.5f, 0.5f }, { 9, 10, 1 }, { 7, 11, 1 },
        { 8, 12, 1.5f }, { 10, 13.5f, 0.5f }, { 8, 14, 1 }, { 6, 15, 1 },
    };
    part p = part_new(BARS(4));
    for (size_t i = 0; i < sizeof notes / sizeof notes[0]; i++)
        part_note(&p, BEATS(notes[i].beat), BEATS(notes[i].len * 0.95f),
                  (note)(scale_degree(&key, notes[i].degree) + 12), 105);
    return p;
}

static part beat(bool full)
{
    part p = part_new(BARS(1));
    part_pattern(&p, "x...x...x...x...", KICK, BEATS(0.25), 115);
    part_pattern(&p, "....x.......x...", CLAP, BEATS(0.25), 100);
    part_pattern(&p, "..x...x...x...x.", full ? OPEN_HAT : HAT, BEATS(0.25), full ? 70 : 90);
    if (full) part_pattern(&p, "xx.xxx.xxx.xxx.x", HAT, BEATS(0.25), 55);
    return part_repeat(p, 4);
}

/* ------------------------------------------------------------------ */
/*  Sections                                                           */
/* ------------------------------------------------------------------ */

static sound intro(void)
{
    synth_patch pad = synth_preset(PATCH_WARM_PAD);
    pad.gain += 10;                               /* alone in the mix: bring it up */
    part notes = chords(4, 0, 90);
    sound s = fx_reverb(fx_eq(synth_render(&pad, &notes), EQ_HIGHPASS, 140, 0, 0.7f),
                        0.85f, 4.5f, 0.45f, 0.4f);
    part_free(&notes);
    return s;
}

static sound drums(bool full)
{
    drum_kit kit = drums_preset(KIT_PUNCHY);
    part hits = beat(full);
    if (full) part_note(&hits, 0, BEATS(1), CRASH, 100);
    sound s = fx_compress(drums_render(&kit, &hits), -14, 3, 8, 90, 3);
    part_free(&hits);
    return s;
}

static sound verse(void)
{
    synth_patch acid = synth_preset(PATCH_ACID_BASS);
    synth_patch pluck = synth_preset(PATCH_PLUCK);
    synth_patch pad = synth_preset(PATCH_WARM_PAD);
    pad.gain -= 8;

    part bass = acid_line(), arp = arpeggio(), keys = chords(4, 0, 70);
    sound s = sound_mix(drums(false),
              sound_mix(fx_drive(synth_render(&acid, &bass), 6, 0.6f, 0.5f),
              sound_mix(fx_delay(synth_render(&pluck, &arp), BEATS(0.75), 0.4f, 0.5f, 0.35f, true),
                        fx_reverb(synth_render(&pad, &keys), 0.7f, 3.0f, 0.5f, 0.3f))));
    part_free(&bass); part_free(&arp); part_free(&keys);
    return sound_repeat(s, 2);                    /* 8 bars */
}

static sound chorus(void)
{
    synth_patch saw = synth_preset(PATCH_SUPERSAW);
    synth_patch sub = synth_preset(PATCH_SUB_BASS);
    synth_patch lead = synth_preset(PATCH_LEAD);

    part harmony = chords(4, 0, 100), low = sub_line(), tune = hook();
    sound s = sound_mix(drums(true),
              sound_mix(fx_pump(fx_reverb(synth_render(&saw, &harmony), 0.6f, 2.2f, 0.5f, 0.25f),
                                BEATS(1), 0.65f, 0.28f),
              sound_mix(fx_pump(synth_render(&sub, &low), BEATS(1), 0.5f, 0.25f),
                        fx_reverb(fx_delay(synth_render(&lead, &tune), BEATS(0.75), 0.35f, 0.4f, 0.3f, true),
                                  0.7f, 2.5f, 0.4f, 0.25f))));
    part_free(&harmony); part_free(&low); part_free(&tune);
    return sound_repeat(s, 2);
}

/* A busy lo-fi break, rendered then treated like a sample. */
static sound make_break(void)
{
    drum_kit kit = drums_preset(KIT_LOFI);
    part p = part_new(BARS(1));
    part_pattern(&p, "x.....x...x.....", KICK, BEATS(0.25), 115);
    part_pattern(&p, "....x.......x..x", SNARE, BEATS(0.25), 105);
    part_pattern(&p, "x.x.x.x.x.x.x.x.", HAT, BEATS(0.25), 70);
    sound s = drums_render(&kit, &p);
    part_free(&p);
    return s;
}

static sound bridge(void)
{
    /* harmonic-seventh chords in 31-EDO: root, +10 (pure-ish major third),
     * +18 (fifth), +25 (the 7:4 seventh 12-tone can't play) */
    const int roots_fifths[4] = { 3, -1, 0, 1 };        /* A F C G on the chain of fifths */
    part harm = part_new(BARS(4));
    for (int bar = 0; bar < 4; bar++) {
        note r = edo_note(31, roots_fifths[bar], 3);
        chord c = { .notes = { r, (note)(r + 10), (note)(r + 18), (note)(r + 25) }, .count = 4, .edo = 31 };
        part_chord(&harm, BARS(bar), BARS(1), &c, 95);
    }
    synth_patch keys = synth_preset(PATCH_FM_KEYS);
    synth_patch pad = synth_preset(PATCH_WARM_PAD);
    keys.gain += 4; pad.gain += 4;
    pad.amp_env.attack = 0.3f;

    /* the break: save, re-import, chop at the hits, play the slices back
     * in a new order (note 60 = slice 0, 61 = slice 1, ...) */
    sound brk = make_break();
    sound_save(&brk, "demo_break.wav");
    sound_free(&brk);
    sound loaded = sound_load("demo_break.wav");
    sound slices[32];
    int n = sound_chop_onsets(&loaded, 0.6f, slices, 32);
    sampler chopper = sampler_slices(slices, n, 60);
    part chops = part_new(BARS(4));
    static const int order[16] = { 0, 2, 1, 3, 0, 0, 4, 2, 5, 1, 3, 3, 6, 4, 2, 7 };
    for (int bar = 0; bar < 4; bar++)
        for (int s = 0; s < 16; s++)
            if (n > 0 && (s % 2 == 0 || bar >= 2))
                part_note(&chops, BARS(bar) + BEATS(s * 0.25), BEATS(0.25),
                          (note)(60 + order[(s + bar * 3) % 16] % n), 100);

    sound s = sound_mix(fx_reverb(synth_render(&keys, &harm), 0.7f, 3.0f, 0.4f, 0.3f),
              sound_mix(fx_reverb(sound_gain(synth_render(&pad, &harm), -6), 0.9f, 5.0f, 0.5f, 0.45f),
                        fx_eq(sampler_render(&chopper, &chops), EQ_LOWPASS, 5000, 0, 0.7f)));
    for (int i = 0; i < n; i++) sound_free(&slices[i]);
    sound_free(&loaded);
    part_free(&harm); part_free(&chops);
    printf("bridge: chopped the break into %d slices\n", n);
    return s;
}

static sound outro(void)
{
    synth_patch pad = synth_preset(PATCH_WARM_PAD);
    pad.gain += 10;
    part notes = chords(4, 0, 80);
    sound s = fx_reverb(synth_render(&pad, &notes), 0.9f, 6.0f, 0.5f, 0.5f);
    part_free(&notes);
    return s;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    audio.bpm = 124;
    key = (scale){ .root = 57, .mode = MINOR, .kind = SCALE_MAJOR };   /* A minor, A3 */

    song s = song_new();
    song_play(&s, intro());
    song_play(&s, verse());
    song_play(&s, chorus());
    song_play(&s, bridge());
    song_play(&s, chorus());
    song_play(&s, outro());

    int rc = song_export(&s, "demo_song.wav");
    printf("demo_song.wav: %s (%.1f s)\n", rc == 0 ? "written" : "FAILED",
           (double)s.mix.frames / audio.rate);
    if (rc == 0 && sound_save(&s.mix, "demo_song.mp3") == 0)
        printf("demo_song.mp3: written\n");
    song_free(&s);
    return rc;
}
