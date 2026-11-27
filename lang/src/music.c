/*
 * music.c — behaviour of the runtime music types (see music.h).
 */
#include "music.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  note                                                               */
/* ------------------------------------------------------------------ */

static const char *const pc_names[12] = {
    "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
};

const char *pitch_class_name(uint8_t pc) { return pc_names[pc % 12]; }

double note_freq(note n)
{
    return 440.0 * pow(2.0, ((int)n - 69) / 12.0);
}

char *note_name(note n, char *buf, size_t size)
{
    snprintf(buf, size, "%s%d", pc_names[note_pitch_class(n)], note_octave(n));
    return buf;
}

/* ------------------------------------------------------------------ */
/*  scale                                                              */
/* ------------------------------------------------------------------ */

/* Semitone steps between consecutive degrees; each row sums to 12. */
typedef struct {
    const char   *name;
    uint8_t       size;
    const uint8_t steps[12];
} scale_def;

static const scale_def scale_defs[SCALE_KIND_COUNT] = {
    [SCALE_MAJOR]          = { "major",          7, {2, 2, 1, 2, 2, 2, 1} },
    [SCALE_HARMONIC_MINOR] = { "harmonic minor", 7, {2, 1, 2, 2, 1, 3, 1} },
    [SCALE_MELODIC_MINOR]  = { "melodic minor",  7, {2, 1, 2, 2, 2, 2, 1} },
    [SCALE_PENTATONIC]     = { "pentatonic",     5, {2, 2, 3, 2, 3} },
    [SCALE_BLUES]          = { "blues",          6, {3, 2, 1, 1, 3, 2} },
    [SCALE_WHOLE_TONE]     = { "whole tone",     6, {2, 2, 2, 2, 2, 2} },
    [SCALE_CHROMATIC]      = { "chromatic",     12, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1} },
};

static const scale_def *def_of(const scale *s)
{
    return &scale_defs[s->kind < SCALE_KIND_COUNT ? s->kind : SCALE_CHROMATIC];
}

uint8_t scale_size(const scale *s) { return def_of(s)->size; }

void mode_next(scale *s) { s->mode = (uint8_t)((s->mode + 1) % scale_size(s)); }

void mode_prev(scale *s)
{
    uint8_t n = scale_size(s);
    s->mode = (uint8_t)((s->mode % n + n - 1) % n);
}

note scale_degree(const scale *s, int degree)
{
    const scale_def *d = def_of(s);
    int n = d->size;
    int octave = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
    int index = degree - octave * n;            /* 0 .. n-1 */
    int offset = 0;
    for (int k = 0; k < index; k++)
        offset += d->steps[(s->mode + k) % n];
    return (note)(s->root + 12 * octave + offset);
}

const char *scale_mode_name(const scale *s, char *buf, size_t size)
{
    static const char *const church[7] = {
        "ionian (major)", "dorian", "phrygian", "lydian",
        "mixolydian", "aeolian (minor)", "locrian"
    };
    const scale_def *d = def_of(s);
    uint8_t m = s->mode % d->size;
    const char *special = NULL;

    if (d == &scale_defs[SCALE_MAJOR])                     special = church[m];
    else if (d == &scale_defs[SCALE_PENTATONIC] && m == 4) special = "minor pentatonic";
    else if (d == &scale_defs[SCALE_HARMONIC_MINOR] && m == 4) special = "phrygian dominant";
    else if (d == &scale_defs[SCALE_MELODIC_MINOR] && m == 3)  special = "lydian dominant";
    else if (d == &scale_defs[SCALE_MELODIC_MINOR] && m == 6)  special = "altered";

    if (special)       snprintf(buf, size, "%s", special);
    else if (m == 0)   snprintf(buf, size, "%s", d->name);
    else               snprintf(buf, size, "%s mode %d", d->name, m + 1);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  chord analysis                                                     */
/* ------------------------------------------------------------------ */

/* Extra bit beyond the public EXT_* ones: a b7 next to a maj7. */
#define EXT_ADD_FLAT7 (1 << 7)

static uint16_t pc_mask(const chord *c)
{
    uint16_t m = 0;
    for (int i = 0; i < c->count && i < CHORD_MAX; i++)
        m |= (uint16_t)(1u << note_pitch_class(c->notes[i]));
    return m;
}

/* Rotate a 12-bit pitch-class mask so `root` becomes bit 0. */
static uint16_t rotate_to(uint16_t m, int root)
{
    return (uint16_t)(((m >> root) | (m << (12 - root))) & 0xFFF);
}

#define HAS(rel, i) (((rel) >> (i)) & 1)

/* Read the intervals above one candidate root. Returns how "strange" the
 * reading is: lower = more conventional. Every interval present ends up
 * as the third, fifth, seventh or an extension, so nothing is dropped. */
static int read_intervals(uint16_t rel, chord_info *ci, uint16_t *triad, uint16_t *seventh)
{
    uint16_t used = 1;  /* the root */
    int cost = 0;

    /* third */
    if      (HAS(rel, 4)) { ci->third = THIRD_MAJOR; used |= 1 << 4; }
    else if (HAS(rel, 3)) { ci->third = THIRD_MINOR; used |= 1 << 3; }
    else if (HAS(rel, 5)) { ci->third = THIRD_SUS4;  used |= 1 << 5; cost += 2; }
    else if (HAS(rel, 2)) { ci->third = THIRD_SUS2;  used |= 1 << 2; cost += 2; }
    else                  { ci->third = THIRD_NONE; }

    /* fifth */
    if (HAS(rel, 7)) {
        ci->fifth = FIFTH_PERFECT; used |= 1 << 7;
    } else if (HAS(rel, 6)) {
        ci->fifth = FIFTH_FLAT; used |= 1 << 6;
        cost += ci->third == THIRD_MINOR ? 0 : 2;          /* dim is normal */
    } else if (HAS(rel, 8) && ci->third != THIRD_MINOR) {
        ci->fifth = FIFTH_SHARP; used |= 1 << 8;
        cost += ci->third == THIRD_MAJOR ? 1 : 2;          /* aug */
    } else {
        ci->fifth = FIFTH_NONE; cost += 1;
    }

    *triad = used;

    /* seventh (or sixth) */
    if      (HAS(rel, 11)) { ci->seventh = SEVENTH_MAJOR; used |= 1 << 11; }
    else if (HAS(rel, 10)) { ci->seventh = SEVENTH_MINOR; used |= 1 << 10; }
    else if (HAS(rel, 9)) {
        used |= 1 << 9;
        if (ci->third == THIRD_MINOR && ci->fifth == FIFTH_FLAT)
            ci->seventh = SEVENTH_DIM;
        else { ci->seventh = SEVENTH_SIXTH; cost += 1; }
    } else {
        ci->seventh = SEVENTH_NONE;
    }

    *seventh = used & (uint16_t)~*triad;

    /* whatever is left is an extension */
    static const struct { uint8_t interval, bit, cost; } exts[] = {
        { 1, EXT_FLAT9, 3 }, { 2, EXT_9, 1 }, { 3, EXT_SHARP9, 3 },
        { 5, EXT_11, 2 }, { 6, EXT_SHARP11, 2 },
        { 8, EXT_FLAT13, 3 }, { 9, EXT_13, 2 }, { 10, EXT_ADD_FLAT7, 4 },
    };
    ci->extensions = 0;
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        if (HAS(rel, exts[i].interval) && !HAS(used, exts[i].interval)) {
            ci->extensions |= exts[i].bit;
            cost += exts[i].cost;
        }
    }

    /* A bare third-less fifth is a power chord, not a missing third. */
    if (ci->third == THIRD_NONE)
        cost += (ci->fifth == FIFTH_PERFECT && ci->seventh == SEVENTH_NONE
                 && ci->extensions == 0) ? 0 : 3;
    return cost;
}

int chord_analyze(const chord *c, chord_info *out)
{
    uint16_t mask = pc_mask(c);
    if (!mask)
        return -1;

    note low = 255;
    for (int i = 0; i < c->count && i < CHORD_MAX; i++)
        if (c->notes[i] < low) low = c->notes[i];
    uint8_t bass = note_pitch_class(low);

    int best_cost = 1 << 30;
    for (int r = 0; r < 12; r++) {
        if (!(mask >> r & 1))
            continue;
        chord_info ci = { .root = (uint8_t)r, .bass = bass };
        uint16_t triad, seventh;
        int cost = read_intervals(rotate_to(mask, r), &ci, &triad, &seventh);
        /* Inversions are less likely, and a bass on the 7th or on a
         * tension (C7b9 read as Bbdim7/C) is less likely still. */
        int bass_iv = (bass - r + 12) % 12;
        if (bass_iv != 0)
            cost += (triad >> bass_iv & 1) ? 1 : (seventh >> bass_iv & 1) ? 2 : 4;
        /* strictly better wins; on a tie the bass as root wins */
        if (cost < best_cost || (cost == best_cost && r == bass)) {
            best_cost = cost;
            *out = ci;
        }
    }
    return 0;
}

uint8_t chord_root(const chord *c)
{
    chord_info ci;
    return chord_analyze(c, &ci) == 0 ? ci.root : 0;
}

/* ------------------------------------------------------------------ */
/*  chord naming                                                       */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t size, len; } sbuf;

static void put(sbuf *b, const char *s)
{
    int n = snprintf(b->buf + b->len, b->len < b->size ? b->size - b->len : 0, "%s", s);
    if (n > 0) b->len += (size_t)n;
    if (b->len >= b->size) b->len = b->size ? b->size - 1 : 0;
}

char *chord_name(const chord *c, char *buf, size_t size)
{
    if (!size) return buf;
    buf[0] = '\0';
    chord_info ci;
    if (chord_analyze(c, &ci) != 0)
        return buf;

    sbuf b = { buf, size, 0 };
    put(&b, pitch_class_name(ci.root));

    uint16_t mask = pc_mask(c);
    if ((mask & (mask - 1)) == 0)            /* a single pitch class */
        return buf;

    const char *alts[8];
    int n_alts = 0;
    uint8_t ext = ci.extensions;
    bool has7 = ci.seventh == SEVENTH_MAJOR || ci.seventh == SEVENTH_MINOR;

    /* Power chord: root + fifth only. */
    if (ci.third == THIRD_NONE && ci.fifth == FIFTH_PERFECT
        && ci.seventh == SEVENTH_NONE && ext == 0) {
        put(&b, "5");
        return buf;
    }

    /* In a 7th chord the highest natural extension replaces the 7:
     * C7 + 9 = C9, + 13 = C13. Naturals below it are implied. */
    char num[4] = "7";
    if (has7) {
        if      (ext & EXT_13) strcpy(num, "13");
        else if (ext & EXT_11) strcpy(num, "11");
        else if (ext & EXT_9)  strcpy(num, "9");
        ext &= (uint8_t)~(EXT_9 | EXT_11 | EXT_13);
    }

    bool diminished = ci.third == THIRD_MINOR && ci.fifth == FIFTH_FLAT;

    if (diminished) {
        switch (ci.seventh) {
        case SEVENTH_DIM:   put(&b, "dim7"); break;
        case SEVENTH_MINOR: put(&b, "m"); put(&b, num); put(&b, "b5"); break;
        case SEVENTH_MAJOR: put(&b, "dim(maj"); put(&b, num); put(&b, ")"); break;
        default:            put(&b, "dim"); break;
        }
    } else {
        if (ci.third == THIRD_MINOR)
            put(&b, "m");
        bool aug = ci.third == THIRD_MAJOR && ci.fifth == FIFTH_SHARP
                   && ci.seventh == SEVENTH_NONE;
        if (aug)
            put(&b, "aug");

        switch (ci.seventh) {
        case SEVENTH_MAJOR:
            if (ci.third == THIRD_MINOR) { put(&b, "(maj"); put(&b, num); put(&b, ")"); }
            else                         { put(&b, "maj"); put(&b, num); }
            break;
        case SEVENTH_MINOR: put(&b, num); break;
        case SEVENTH_SIXTH:
            put(&b, (ext & EXT_9) ? "6/9" : "6");
            ext &= (uint8_t)~EXT_9;
            break;
        default: break;
        }

        if (ci.third == THIRD_SUS4) put(&b, "sus4");
        if (ci.third == THIRD_SUS2) put(&b, "sus2");

        if (ci.fifth == FIFTH_FLAT)             alts[n_alts++] = "b5";
        if (ci.fifth == FIFTH_SHARP && !aug)    alts[n_alts++] = "#5";
        if (ci.third == THIRD_NONE)             alts[n_alts++] = "no3";
    }

    /* Natural extensions without a 7th are "add" tones. */
    if (ext & EXT_9)  put(&b, "add9");
    if (ext & EXT_11) put(&b, "add11");
    if (ext & EXT_13) put(&b, "add13");

    if (ext & EXT_FLAT9)     alts[n_alts++] = "b9";
    if (ext & EXT_SHARP9)    alts[n_alts++] = "#9";
    if (ext & EXT_SHARP11)   alts[n_alts++] = "#11";
    if (ext & EXT_FLAT13)    alts[n_alts++] = "b13";
    if (ext & EXT_ADD_FLAT7) alts[n_alts++] = "add b7";

    if (n_alts) {
        put(&b, "(");
        for (int i = 0; i < n_alts; i++) {
            if (i) put(&b, ",");
            put(&b, alts[i]);
        }
        put(&b, ")");
    }

    if (ci.bass != ci.root) {
        put(&b, "/");
        put(&b, pitch_class_name(ci.bass));
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  chord building                                                     */
/* ------------------------------------------------------------------ */

void chord_add(chord *c, note n)
{
    if (c->count < CHORD_MAX)
        c->notes[c->count++] = n;
}

chord chord_from_scale(const scale *s, int degree, int size)
{
    chord c = {0};
    if (size < 1) size = 1;
    if (size > CHORD_MAX) size = CHORD_MAX;
    for (int i = 0; i < size; i++)
        chord_add(&c, scale_degree(s, degree + 2 * i));
    return c;
}
