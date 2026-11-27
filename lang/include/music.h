/*
 * music.h — the runtime music types of the language, and the C helpers
 * that give them their behaviour.
 *
 * (Enums are plain C enums, so they're int-sized; struct fields that hold
 * them are uint8_t to keep the structs byte-sized.)
 *
 * Design rules:
 *   - Everything that fits in a byte is a byte.
 *   - Nothing derived is stored. A chord stores its notes; its root and
 *     quality are computed from them, so editing one note renames the chord.
 *   - Stepping wraps where music wraps: mode_next(LOCRIAN) is IONIAN, and
 *     scale_degree() past the last degree continues in the next octave.
 *
 * The editor's autocomplete tables (editor/src/language.c) mirror these.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  note                                                               */
/* ------------------------------------------------------------------ */

/* A pitch as a MIDI number: C4 = 60, A4 = 69. One byte, so n++ is a
 * semitone up, n += 7 a fifth up, and arrays of notes are plain bytes.
 * 0..127 is the MIDI range; 128..255 are valid (very high) pitches that
 * just can't be sent as MIDI. Arithmetic wraps like any uint8_t.
 * Loudness is not part of a note: it belongs to the event that plays it. */
typedef uint8_t note;

#define NOTE_C4 ((note)60)
#define NOTE_A4 ((note)69)

static inline uint8_t note_pitch_class(note n) { return n % 12; }  /* C=0 .. B=11 */
static inline int     note_octave(note n)      { return n / 12 - 1; } /* MIDI 0 is C-1 */

/* Frequency in Hz, 12-tone equal temperament, A4 = 440 Hz. */
double note_freq(note n);

/* "C4", "F#3", "Bb-1". buf needs 6 bytes. Returns buf. */
char *note_name(note n, char *buf, size_t size);

/* Name of a pitch class (0..11) without octave: "C", "C#", "Eb", ... */
const char *pitch_class_name(uint8_t pc);

/* ------------------------------------------------------------------ */
/*  scale                                                              */
/* ------------------------------------------------------------------ */

/* The family of a scale. Its mode picks which degree it starts on, so
 * mode = DORIAN on SCALE_MAJOR gives the dorian mode; on SCALE_MELODIC_MINOR
 * mode 3 gives lydian dominant, and so on. */
typedef enum {
    SCALE_MAJOR,            /* diatonic: the 7 church modes             */
    SCALE_HARMONIC_MINOR,
    SCALE_MELODIC_MINOR,
    SCALE_PENTATONIC,       /* major pentatonic; mode 4 = minor pent.   */
    SCALE_BLUES,
    SCALE_WHOLE_TONE,
    SCALE_CHROMATIC,
    SCALE_KIND_COUNT
} scale_kind;

/* Mode names for SCALE_MAJOR. Other kinds use the same numbers (0 = as
 * written, 1 = starting on the 2nd degree, ...). */
typedef enum {
    IONIAN, DORIAN, PHRYGIAN, LYDIAN, MIXOLYDIAN, AEOLIAN, LOCRIAN,
    MAJOR = IONIAN,
    MINOR = AEOLIAN,
} mode;

/* 3 bytes. The notes are computed with scale_degree(), not stored. */
typedef struct {
    note    root;
    uint8_t mode;   /* 0 .. scale_size()-1 */
    uint8_t kind;   /* scale_kind          */
} scale;

/* Notes per octave for this scale's kind (7 for major, 5 for pentatonic...). */
uint8_t scale_size(const scale *s);

/* Next / previous mode, wrapping: LOCRIAN -> IONIAN. The root stays put. */
void mode_next(scale *s);
void mode_prev(scale *s);

/* The note at a scale degree. 0 = root, 1 = second, ...; degrees past the
 * top continue into the next octave and negative ones go down, so
 * scale_degree(s, d++) walks the scale forever. */
note scale_degree(const scale *s, int degree);

/* Name of the mode for display: "dorian", "harmonic minor mode 5", ... */
const char *scale_mode_name(const scale *s, char *buf, size_t size);

/* ------------------------------------------------------------------ */
/*  chord                                                              */
/* ------------------------------------------------------------------ */

#define CHORD_MAX 8

/* 9 bytes, no pointers. Only the notes are stored, lowest first by
 * convention (order doesn't affect the name, only which note is the bass). */
typedef struct {
    note    notes[CHORD_MAX];
    uint8_t count;
} chord;

/* How a chord was read. Everything here is derived from the notes. */
typedef enum { THIRD_NONE, THIRD_MINOR, THIRD_MAJOR, THIRD_SUS2, THIRD_SUS4 } chord_third;
typedef enum { FIFTH_NONE, FIFTH_PERFECT, FIFTH_FLAT, FIFTH_SHARP } chord_fifth;
typedef enum { SEVENTH_NONE, SEVENTH_MAJOR, SEVENTH_MINOR, SEVENTH_DIM, SEVENTH_SIXTH } chord_seventh;

/* Bits for chord_info.extensions. */
enum {
    EXT_FLAT9  = 1 << 0, EXT_9  = 1 << 1, EXT_SHARP9  = 1 << 2,
    EXT_11     = 1 << 3, EXT_SHARP11 = 1 << 4,
    EXT_FLAT13 = 1 << 5, EXT_13 = 1 << 6,
};

typedef struct {
    uint8_t       root;        /* pitch class 0..11 */
    uint8_t       bass;        /* pitch class of the lowest note */
    uint8_t       third;       /* chord_third   */
    uint8_t       fifth;       /* chord_fifth   */
    uint8_t       seventh;     /* chord_seventh */
    uint8_t       extensions;  /* EXT_* bits */
} chord_info;

/* Read a chord: picks the root that explains the notes with the fewest
 * alterations (the bass wins ties). Returns 0, or -1 for an empty chord. */
int chord_analyze(const chord *c, chord_info *out);

/* Pitch class of the root (0..11), computed from the notes. 0 if empty. */
uint8_t chord_root(const chord *c);

/* Chord symbol: "C", "Am7", "G7(b9)", "Bm7b5", "Cmaj7sus4(b5)", "C/E".
 * Any set of notes gets a name. Returns buf ("" for an empty chord). */
char *chord_name(const chord *c, char *buf, size_t size);

/* Add a note (ignored when the chord is full). */
void chord_add(chord *c, note n);

/* Build a chord by stacking thirds inside a scale: size 3 = triad,
 * 4 = seventh chord, ... from the given degree. So the I-vi-IV-V of a
 * scale is just degrees 0, 5, 3, 4. */
chord chord_from_scale(const scale *s, int degree, int size);
