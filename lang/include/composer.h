/*
 * composer.h — the language prelude: everything a song can use without
 * declaring it. Every song is built with this included.
 *
 *     int main(void) {
 *         tempo(120bpm);
 *         play(beep(C4, 1b));
 *         return 0;
 *     }
 *
 * main() takes no arguments: it play()s sections onto the song, one after
 * another, and the song is what gets rendered.
 */
#pragma once

#include "audio.h"
#include "music.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- time ------------------------------------------------------- */

void     tempo(double bpm);               /* from here on               */
uint32_t composer_ms(double ms);          /* ticks at the current tempo */
uint32_t composer_seconds(double s);
#define MS(x)      composer_ms(x)         /* 250ms -> MS(250)           */
#define SECONDS(x) composer_seconds(x)    /* 2s    -> SECONDS(2)        */

/* ---- the song ---------------------------------------------------- */

void play(sound s);                       /* at the cursor, then move on */
void layer(sound s);                      /* at the cursor, stay put     */
void rest(uint32_t ticks);

/* ---- making sound ------------------------------------------------ */

sound beep(note n, uint32_t length);              /* a plain C-style beep */
sound beep_chord(chord c, uint32_t length);
sound beep_part(part p);                          /* consumes p */
sound synth(patch_preset preset, part p);         /* consumes p */
sound drum_machine(kit_preset kit, part p);       /* consumes p */
sound load_sample(const char *path);              /* wav, mp3, flac, ... */

/* 16th-note step pattern: pattern("x...x...x...x...", KICK) */
part  pattern(const char *steps, note n);
/* A part with one note / one chord at the start. */
part  one_note(note n, uint32_t length);
part  one_chord(chord c, uint32_t length);

/* chord_of(3, C4, E4, G4) */
chord chord_of(int count, ...);

static inline sound mix(sound a, sound b) { return sound_mix(a, b); }
static inline sound then(sound a, sound b) { return sound_then(a, b); }
static inline sound repeat(sound s, int times) { return sound_repeat(s, times); }

/* ---- used by the generated runner -------------------------------- */

typedef struct {
    const char *name;
    sound (*call)(void);
} composer_entry;

note  composer_default_note(void);        /* C4, for functions taking a note   */
chord composer_default_chord(void);       /* C major, for functions taking one */
sound composer_take_song(void);           /* what play() collected            */
int   composer_run(int argc, char **argv, const composer_entry *entries, int n);
