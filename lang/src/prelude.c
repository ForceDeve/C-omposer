/*
 * prelude.c — the language's built-in functions (see composer.h).
 */
#include "composer.h"

#include <stdarg.h>

static song the_song;

void tempo(double bpm)
{
    if (bpm > 1 && bpm < 2000) audio.bpm = bpm;
}

uint32_t composer_ms(double ms)
{
    return (uint32_t)(ms / 1000.0 * audio.bpm / 60.0 * TICKS_PER_BEAT + 0.5);
}

uint32_t composer_seconds(double s) { return composer_ms(s * 1000.0); }

void play(sound s)  { song_play(&the_song, s); }
void layer(sound s) { song_layer(&the_song, s); }
void rest(uint32_t ticks) { song_rest(&the_song, ticks); }

sound composer_take_song(void)
{
    sound s = the_song.mix;
    if (the_song.cursor > s.length) {           /* trailing rest() counts */
        sound_resize(&s, the_song.cursor);
        s.length = the_song.cursor;
    }
    the_song = song_new();
    return s;
}

/* ---- sound makers ------------------------------------------------ */

static synth_patch beep_patch(void)
{
    synth_patch p = { 0 };
    p.osc[0] = (osc_params){ .wave = WAVE_SQUARE, .level = 0.6f, .pulse_width = 0.5f, .unison = 1 };
    p.osc[1] = (osc_params){ .wave = WAVE_SINE, .level = 0.5f, .pulse_width = 0.5f, .unison = 1 };
    p.filter = FILTER_LOWPASS;
    p.cutoff = 3500;
    p.amp_env = (envelope){ 0.002f, 0.15f, 0.7f, 0.05f };
    p.velocity_to_amp = 0.3f;
    p.gain = -14;
    return p;
}

sound beep_part(part p)
{
    synth_patch patch = beep_patch();
    sound s = synth_render(&patch, &p);
    part_free(&p);
    return s;
}

sound beep(note n, uint32_t length) { return beep_part(one_note(n, length)); }
sound beep_chord(chord c, uint32_t length) { return beep_part(one_chord(c, length)); }

sound synth(patch_preset preset, part p)
{
    synth_patch patch = synth_preset(preset);
    sound s = synth_render(&patch, &p);
    part_free(&p);
    return s;
}

sound drum_machine(kit_preset kit, part p)
{
    drum_kit k = drums_preset(kit);
    sound s = drums_render(&k, &p);
    part_free(&p);
    return s;
}

sound load_sample(const char *path)
{
    sound s = sound_load(path);
    if (!s.frames) fprintf(stderr, "load_sample: could not load \"%s\"\n", path);
    return s;
}

part pattern(const char *steps, note n)
{
    part p = part_new(0);
    part_pattern(&p, steps, n, BEATS(0.25), 100);
    return p;
}

part one_note(note n, uint32_t length)
{
    part p = part_new(length);
    part_note(&p, 0, length, n, 100);
    return p;
}

part one_chord(chord c, uint32_t length)
{
    part p = part_new(length);
    part_chord(&p, 0, length, &c, 90);
    return p;
}

chord chord_of(int count, ...)
{
    chord c = { 0 };
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count && i < CHORD_MAX; i++)
        chord_add(&c, (note)va_arg(ap, int));
    va_end(ap);
    return c;
}

note  composer_default_note(void)  { return 60; }
chord composer_default_chord(void) { return chord_of(3, 60, 64, 67); }

/* ---- the runner -------------------------------------------------- */

static int usage(const char *exe)
{
    fprintf(stderr, "usage: %s list | %s call <function> <out.wav|.mp3|...>\n", exe, exe);
    return 2;
}

int composer_run(int argc, char **argv, const composer_entry *entries, int n)
{
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        for (int i = 0; i < n; i++) printf("%s\n", entries[i].name);
        return 0;
    }
    if (argc < 4 || strcmp(argv[1], "call")) return usage(argv[0]);
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, argv[2])) continue;
        audio.bpm = 120;
        the_song = song_new();
        sound s = entries[i].call();
        if (!s.frames) {
            fprintf(stderr, "%s() made no sound\n", argv[2]);
            return 3;
        }
        s = fx_limit(s, -1.0f);
        uint32_t frames = s.frames;
        int rc = sound_save(&s, argv[3]);
        sound_free(&s);
        if (rc != 0) { fprintf(stderr, "could not write %s\n", argv[3]); return 4; }
        fprintf(stderr, "%s(): %.2f s rendered\n", argv[2], (double)frames / audio.rate);
        return 0;
    }
    fprintf(stderr, "no playable function named %s()\n", argv[2]);
    return 5;
}
