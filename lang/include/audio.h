/*
 * audio.h — the sound engine: what the language's functions turn into.
 *
 * Two kinds of value flow through a song:
 *
 *   part   notes over time (a melody, a chord progression, a drum pattern).
 *          Pure data: cheap to build, copy, transpose, concatenate.
 *   sound  rendered stereo audio. Synths turn a part into a sound, samples
 *          load as sounds, effects take a sound and give one back.
 *
 * So composition is plain function calls, f(g(x)):
 *
 *     sound chorus(void) {
 *         return fx_reverb(synth_render(&pad, &chords), 0.7f, 3.0f, 0.5f, 0.3f);
 *     }
 *
 * and main() just plays sections in order on the song's timeline:
 *
 *     song_play(&s, intro());  song_play(&s, chorus());  ...
 *
 * Ownership: every function that takes a `sound` by value CONSUMES it (it
 * may reuse or free its buffers) and returns a new one. Functions taking a
 * `const sound *` or `const part *` only read. This is what lets
 * reverb(drive(lead())) work without leaks.
 *
 * A sound has two lengths: `frames` (all the audio, including reverb and
 * release tails) and `length` (its musical length). Playing sounds one
 * after another uses `length`, so tails ring over the next section.
 *
 * Rendering is deterministic: the same code gives the same audio file.
 */
#pragma once

#include "music.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Context: sample rate and tempo                                     */
/* ------------------------------------------------------------------ */

#define TICKS_PER_BEAT 960

typedef struct {
    uint32_t rate;     /* samples per second, default 48000 */
    double   bpm;      /* default 120 */
} audio_ctx;

extern audio_ctx audio;

/* Musical time is in ticks (960 per beat); audio time is in frames. */
#define BEATS(b) ((uint32_t)((b) * TICKS_PER_BEAT + 0.5))
#define BARS(b)  BEATS((b) * 4)                    /* 4/4 */
uint32_t ticks_to_frames(uint32_t ticks);
double   ticks_to_seconds(uint32_t ticks);

/* ------------------------------------------------------------------ */
/*  sound                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    float   *l, *r;
    uint32_t frames;   /* allocated audio, including tails */
    uint32_t length;   /* musical length in frames         */
} sound;

sound sound_new(uint32_t frames);              /* silent, length = frames */
sound sound_silence(uint32_t ticks);
void  sound_free(sound *s);
bool  sound_resize(sound *s, uint32_t frames); /* grows with silence      */
float sound_peak(const sound *s);              /* linear, max of |l|,|r|  */

sound sound_mix(sound a, sound b);             /* layered; consumes both  */
sound sound_then(sound a, sound b);            /* b starts at a's length  */
sound sound_repeat(sound s, int times);
sound sound_gain(sound s, float db);
sound sound_pan(sound s, float pan);           /* -1 left .. +1 right     */
sound sound_reverse(sound s);
sound sound_pitch(sound s, float semitones);   /* resample: speed + pitch */
sound sound_trim(sound s, uint32_t start_frame, uint32_t frames);

/* Load a sample: WAV natively (8/16/24/32-bit, float, any rate); other
 * formats (mp3, flac, ogg, ...) through ffmpeg if it is installed.
 * Resampled to audio.rate. Returns frames == 0 on failure. */
sound sound_load(const char *path);

/* Save: format from the extension. .wav = 24-bit PCM (dithered);
 * .mp3 .flac .ogg .opus .m4a go through ffmpeg. Returns 0 on success. */
int sound_save(const sound *s, const char *path);

/* ------------------------------------------------------------------ */
/*  part                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t start, length;    /* ticks */
    note     pitch;
    uint8_t  velocity;         /* 1..127 */
    uint8_t  edo;              /* tuning of pitch, 0 = 12 */
} note_event;

typedef struct {
    note_event *events;
    uint32_t    count, cap;
    uint32_t    length;        /* ticks; grows to fit the notes */
} part;

part part_new(uint32_t length_ticks);
void part_free(part *p);
part part_copy(const part *p);
void part_note(part *p, uint32_t start, uint32_t len, note n, uint8_t velocity);
void part_note_edo(part *p, uint32_t start, uint32_t len, note n, uint8_t edo, uint8_t velocity);
void part_chord(part *p, uint32_t start, uint32_t len, const chord *c, uint8_t velocity);

/* Step pattern: one char per step. 'x' hit, 'X' accent, '.' or '-' rest,
 * anything else ignored (use spaces or '|' to group). */
void part_pattern(part *p, const char *steps, note n, uint32_t step_ticks, uint8_t velocity);

part part_then(part a, part b);            /* consumes both */
part part_repeat(part p, int times);       /* consumes      */
part part_transpose(part p, int steps);    /* consumes      */

/* ------------------------------------------------------------------ */
/*  Synth                                                              */
/* ------------------------------------------------------------------ */

typedef enum { WAVE_SAW, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SINE, WAVE_NOISE } waveform;
typedef enum { FILTER_OFF, FILTER_LADDER, FILTER_LOWPASS, FILTER_BANDPASS, FILTER_HIGHPASS } filter_kind;

typedef struct {
    float attack, decay, sustain, release;   /* seconds; sustain 0..1 */
} envelope;

typedef struct {
    uint8_t wave;            /* waveform */
    float   level;           /* 0..1 */
    float   semitones;       /* transpose */
    float   cents;           /* fine tune */
    float   pulse_width;     /* square only, 0.05..0.95 */
    uint8_t unison;          /* 1..7 voices */
    float   unison_detune;   /* cents between outermost voices */
    float   unison_width;    /* stereo spread 0..1 */
} osc_params;

typedef struct {
    osc_params osc[2];
    float sub;               /* sine one octave below osc 1 */
    float noise;
    float fm;                /* osc 2 phase-modulates osc 1 (radians) */
    float fm_env;            /* 0..1: how much fm follows the filter envelope */

    uint8_t filter;          /* filter_kind */
    float cutoff;            /* Hz */
    float resonance;         /* 0..1 (1 = self-oscillation) */
    float drive;             /* 0..1, saturation into the filter */
    float key_track;         /* 0..1: cutoff follows pitch */
    float filter_env_amount; /* octaves */
    float velocity_to_filter;/* octaves at full velocity */
    envelope amp_env, filter_env;

    float lfo_rate;          /* Hz */
    float lfo_pitch;         /* cents (vibrato) */
    float lfo_filter;        /* octaves */
    float lfo_pulse_width;

    float glide;             /* seconds; > 0 makes it slide between notes */
    float drift;             /* cents of slow analog pitch wander */
    float velocity_to_amp;   /* 0..1 */
    float gain;              /* dB */
    float pan;               /* -1..1 */
} synth_patch;

typedef enum {
    PATCH_SUPERSAW,          /* wide trance/EDM lead or chords */
    PATCH_WARM_PAD,          /* slow detuned pad */
    PATCH_ACID_BASS,         /* resonant ladder squelch */
    PATCH_SUB_BASS,          /* clean deep bass */
    PATCH_REESE,             /* detuned saw bass */
    PATCH_PLUCK,             /* short filtered pluck */
    PATCH_LEAD,              /* mono glide lead */
    PATCH_FM_KEYS,           /* electric-piano-like FM */
    PATCH_BRASS,             /* filter-swell brass */
    PATCH_COUNT
} patch_preset;

synth_patch synth_preset(patch_preset preset);
sound synth_render(const synth_patch *patch, const part *notes);

/* ------------------------------------------------------------------ */
/*  Drum synth                                                         */
/* ------------------------------------------------------------------ */

/* Notes that trigger drums (General MIDI numbers). */
enum {
    KICK = 36, RIM = 37, SNARE = 38, CLAP = 39, HAT = 42,
    TOM_LOW = 45, OPEN_HAT = 46, TOM_HIGH = 50, CRASH = 49
};

typedef struct {
    float kick_tune;    /* Hz of the body, ~40-70 */
    float kick_punch;   /* pitch sweep amount, 0..1 */
    float kick_decay;   /* seconds */
    float kick_drive;   /* 0..1 */
    float snare_tune;   /* Hz of the body, ~150-250 */
    float snare_snap;   /* noise amount, 0..1 */
    float snare_decay;
    float hat_decay;    /* closed hat, seconds */
    float tone;         /* overall brightness 0..1 */
    float gain;         /* dB */
} drum_kit;

typedef enum { KIT_808, KIT_PUNCHY, KIT_LOFI, KIT_COUNT } kit_preset;

drum_kit drums_preset(kit_preset preset);
sound    drums_render(const drum_kit *kit, const part *hits);

/* ------------------------------------------------------------------ */
/*  Sampler and chopper                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const sound *sample;
    note     root;           /* the pitch the sample was recorded at */
    uint8_t  root_edo;
    envelope env;
    float    gain, pan;
    bool     loop;
    uint32_t loop_start, loop_end;   /* frames; end 0 = sample end */
    bool     pitched;        /* false: every note plays at original speed */

    /* Slice mode (from sound_chop): note `slice_base` plays slice 0,
     * the next note slice 1, ... MPC-style. */
    const sound *slices;
    int      n_slices;
    note     slice_base;
} sampler;

sampler sampler_new(const sound *sample, note root);
sampler sampler_slices(const sound *slices, int n, note base);
sound   sampler_render(const sampler *smp, const part *notes);

/* Chop into n equal slices, or at detected hits (sensitivity 0..1).
 * Writes up to max slices to out (each a new sound with click-free
 * edges) and returns how many. */
int sound_chop(const sound *s, int n, sound *out, int max);
int sound_chop_onsets(const sound *s, float sensitivity, sound *out, int max);

/* ------------------------------------------------------------------ */
/*  Effects (each consumes its input and returns the processed sound)  */
/* ------------------------------------------------------------------ */

typedef enum {
    EQ_LOWPASS, EQ_HIGHPASS, EQ_BANDPASS, EQ_NOTCH,
    EQ_PEAK, EQ_LOWSHELF, EQ_HIGHSHELF
} eq_kind;

sound fx_reverb(sound s, float size, float decay_seconds, float damping, float mix);
sound fx_delay(sound s, uint32_t time_ticks, float feedback, float tone, float mix, bool ping_pong);
sound fx_chorus(sound s, float rate_hz, float depth_ms, float mix);
sound fx_drive(sound s, float drive_db, float tone, float mix);
sound fx_eq(sound s, eq_kind kind, float freq, float gain_db, float q);
sound fx_compress(sound s, float threshold_db, float ratio, float attack_ms,
                  float release_ms, float makeup_db);
sound fx_pump(sound s, uint32_t every_ticks, float depth, float release);  /* sidechain feel */
sound fx_limit(sound s, float ceiling_db);                                 /* true-peak, 5 ms look-ahead */

/* ------------------------------------------------------------------ */
/*  Song: the timeline main() plays onto                               */
/* ------------------------------------------------------------------ */

typedef struct {
    sound    mix;
    uint32_t cursor;   /* frames */
} song;

song song_new(void);
void song_play(song *s, sound section);    /* at the cursor, then advance  */
void song_layer(song *s, sound section);   /* at the cursor, don't advance */
void song_rest(song *s, uint32_t ticks);
/* Masters (DC filter + true-peak limiter at -1 dBTP) and saves. 0 on success. */
int  song_export(song *s, const char *path);
void song_free(song *s);
