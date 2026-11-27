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
/*  tuning: equal divisions of the octave (EDO)                        */
/* ------------------------------------------------------------------ */

/* A tuning is one byte: the number of equal steps per octave. 12 is
 * standard; 31 gives near-pure thirds and a real harmonic seventh; 19, 22,
 * 24, 53 ... all work. 0 means 12, so zero-initialised structs are normal
 * 12-tone.
 *
 * Inside a tuning the note byte counts *steps of that tuning*: note++ is
 * one step (a semitone in 12-EDO, a diesis of ~39 cents in 31-EDO).
 * Middle C sits at edo_c4(): 60 in 12-EDO, so MIDI numbers still work,
 * and near the middle of the byte (124 in 31-EDO) otherwise, which gives
 * 256/edo octaves of range (about 8 in 31-EDO).
 *
 * Note names and scales are spelled on the chain of fifths, so in 31-EDO
 * C# and Db are different notes, as they should be. */

static inline uint8_t edo_of(uint8_t edo) { return edo ? edo : 12; }

note   edo_c4(uint8_t edo);          /* step number of middle C            */
double edo_c4_freq(void);            /* Hz of middle C in every tuning (A4=440 in 12-EDO) */
int    edo_fifth(uint8_t edo);       /* steps in the tuning's fifth: 12->7, 31->18, 53->31 */
int    edo_chromatic(uint8_t edo);   /* steps from C to C#: 12->1, 31->2, 19->1 */

/* A note spelled as a position on the chain of fifths (0 = C, 1 = G,
 * 2 = D, -1 = F, 6 = F#, -5 = Db, 7 = C#, ...) in a given octave.
 * edo_note(31, 6, 4) is F#4 in 31-EDO. */
note edo_note(uint8_t edo, int fifths, int octave);

/* Frequency and name of a note in a tuning. Steps that don't fall on the
 * chain of fifths (the in-between steps of 24-EDO, 36-EDO, ...) are named
 * with ups: "C^4" is one step above C4. */
double note_freq_edo(note n, uint8_t edo);
char  *note_name_edo(note n, uint8_t edo, char *buf, size_t size);

/* Cents between two notes in a tuning (1200 = an octave). */
double edo_cents(int steps, uint8_t edo);

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
    SCALE_CHROMATIC,        /* the 12 named notes (also in other EDOs) */
    SCALE_EDO_ALL,          /* every step of the tuning: 31 notes in 31-EDO */
    SCALE_KIND_COUNT
} scale_kind;

/* Mode names for SCALE_MAJOR. Other kinds use the same numbers (0 = as
 * written, 1 = starting on the 2nd degree, ...). */
typedef enum {
    IONIAN, DORIAN, PHRYGIAN, LYDIAN, MIXOLYDIAN, AEOLIAN, LOCRIAN,
    MAJOR = IONIAN,
    MINOR = AEOLIAN,
} mode;

/* 4 bytes. The notes are computed with scale_degree(), not stored.
 * Set edo to play the same scale in another tuning: C major in 31-EDO
 * is { .root = edo_c4(31), .kind = SCALE_MAJOR, .edo = 31 }. */
typedef struct {
    note    root;
    uint8_t mode;   /* 0 .. scale_size()-1 */
    uint8_t kind;   /* scale_kind          */
    uint8_t edo;    /* tuning; 0 = 12      */
} scale;

/* Notes per octave (7 for major, 5 for pentatonic, edo for SCALE_EDO_ALL). */
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

/* 10 bytes, no pointers. Only the notes (and their tuning) are stored,
 * lowest first by convention (order doesn't affect the name, only which
 * note is the bass). */
typedef struct {
    note    notes[CHORD_MAX];
    uint8_t count;
    uint8_t edo;    /* tuning of the notes; 0 = 12 */
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

/* Chord names and analysis are 12-tone: in other tunings each note is
 * read as the nearest 12-tone pitch (so 31-EDO's harmonic seventh C-E-G-A#
 * reads as C7). Proper microtonal chord names are a later feature. */

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

/* Build a chord by stacking thirds inside a scale (it takes the scale's
 * tuning): size 3 = triad,
 * 4 = seventh chord, ... from the given degree. So the I-vi-IV-V of a
 * scale is just degrees 0, 5, 3, 4. */
chord chord_from_scale(const scale *s, int degree, int size);
