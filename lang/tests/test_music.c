/*
 * test_music.c — checks for the runtime music types.  Run: ctest, or
 * ./build/lang/test_music directly.
 */
#include "music.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do {                                    \
        checks++;                                                \
        if (!(cond)) {                                           \
            failures++;                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                 \
            printf("\n");                                        \
        }                                                        \
    } while (0)

/* MIDI numbers for octave 4 (and the next one up). */
enum { C4 = 60, Db4, D4, Eb4, E4, F4, Gb4, G4, Ab4, A4, Bb4, B4,
       C5, Db5, D5, Eb5, E5, F5, Gb5, G5, Ab5, A5, Bb5, B5 };

#define CHORD(...) make_chord((const note[]){__VA_ARGS__}, \
                              sizeof((const note[]){__VA_ARGS__}))
static chord make_chord(const note *ns, size_t n)
{
    chord c = {0};
    for (size_t i = 0; i < n; i++) chord_add(&c, ns[i]);
    return c;
}

static void expect_name(chord c, const char *want, int line)
{
    char got[32];
    chord_name(&c, got, sizeof got);
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("FAIL line %d: chord named \"%s\", expected \"%s\"\n", line, got, want);
    }
}
#define NAME(c, want) expect_name((c), (want), __LINE__)

static void test_note(void)
{
    char buf[8];
    CHECK(sizeof(note) == 1, "note is %zu bytes", sizeof(note));
    CHECK(strcmp(note_name(C4, buf, sizeof buf), "C4") == 0, "C4 -> %s", buf);
    CHECK(strcmp(note_name(Bb4 - 12, buf, sizeof buf), "Bb3") == 0, "Bb3 -> %s", buf);
    CHECK(strcmp(note_name(0, buf, sizeof buf), "C-1") == 0, "0 -> %s", buf);
    CHECK(fabs(note_freq(A4) - 440.0) < 1e-9, "A4 = %f", note_freq(A4));
    CHECK(fabs(note_freq(C4) - 261.6256) < 1e-3, "C4 = %f", note_freq(C4));

    note n = B4;
    n++;
    CHECK(n == C5, "B4++ should be C5, got %d", n);
    n += 7;
    CHECK(n == G5, "C5 + 7 should be G5, got %d", n);
}

static void test_scale(void)
{
    char buf[32];
    scale s = { .root = C4, .mode = MAJOR, .kind = SCALE_MAJOR };
    CHECK(sizeof(scale) == 3, "scale is %zu bytes", sizeof(scale));

    const note c_major[] = { C4, D4, E4, F4, G4, A4, B4, C5, D5 };
    for (int d = 0; d < 9; d++)
        CHECK(scale_degree(&s, d) == c_major[d], "C major degree %d = %d", d, scale_degree(&s, d));
    CHECK(scale_degree(&s, -1) == B4 - 12, "degree -1 = %d", scale_degree(&s, -1));
    CHECK(scale_degree(&s, -7) == C4 - 12, "degree -7 = %d", scale_degree(&s, -7));

    /* mode++ wraps and keeps the root */
    s.mode = LOCRIAN;
    mode_next(&s);
    CHECK(s.mode == IONIAN, "LOCRIAN -> next should be IONIAN, got %d", s.mode);
    mode_prev(&s);
    CHECK(s.mode == LOCRIAN, "IONIAN -> prev should be LOCRIAN, got %d", s.mode);

    /* C dorian: C D Eb F G A Bb */
    s.mode = DORIAN;
    CHECK(scale_degree(&s, 2) == Eb4 && scale_degree(&s, 6) == Bb4, "C dorian is wrong");
    CHECK(strcmp(scale_mode_name(&s, buf, sizeof buf), "dorian") == 0, "%s", buf);

    /* A minor pentatonic = pentatonic mode 5 on A: A C D E G */
    scale p = { .root = A4 - 12, .mode = 4, .kind = SCALE_PENTATONIC };
    CHECK(scale_degree(&p, 1) == C4 && scale_degree(&p, 4) == G4, "A minor pentatonic is wrong");
    CHECK(strcmp(scale_mode_name(&p, buf, sizeof buf), "minor pentatonic") == 0, "%s", buf);
    mode_next(&p);
    CHECK(p.mode == 0, "pentatonic mode wraps after 5, got %d", p.mode);
}

static void test_chord_names(void)
{
    CHECK(sizeof(chord) == 9, "chord is %zu bytes", sizeof(chord));

    /* triads */
    NAME(CHORD(C4, E4, G4), "C");
    NAME(CHORD(C4, Eb4, G4), "Cm");
    NAME(CHORD(C4, Eb4, Gb4), "Cdim");
    NAME(CHORD(C4, E4, Ab4), "Caug");
    NAME(CHORD(C4, F4, G4), "Csus4");
    NAME(CHORD(C4, D4, G4), "Csus2");
    NAME(CHORD(C4, G4), "C5");
    NAME(CHORD(C4, E4, Gb4), "C(b5)");
    NAME(CHORD(C4), "C");
    NAME(CHORD(C4, C5), "C");

    /* sevenths and sixths */
    NAME(CHORD(C4, E4, G4, B4), "Cmaj7");
    NAME(CHORD(G4, B4, D5, F5), "G7");
    NAME(CHORD(A4, C5, E5, G5), "Am7");
    NAME(CHORD(C4, Eb4, G4, B4), "Cm(maj7)");
    NAME(CHORD(B4, D5, F5, A5), "Bm7b5");
    NAME(CHORD(B4, D5, F5, Ab5), "Bdim7");
    NAME(CHORD(C4, E4, G4, A4), "C6");
    NAME(CHORD(C4, Eb4, G4, A4), "Cm6");
    NAME(CHORD(G4, C5, D5, F5), "G7sus4");

    /* extensions and alterations */
    NAME(CHORD(C4, E4, G4, Bb4, D5), "C9");
    NAME(CHORD(C4, E4, G4, B4, D5), "Cmaj9");
    NAME(CHORD(A4 - 12, C4, E4, G4, B4), "Am9");
    NAME(CHORD(C4, E4, G4, Bb4, D5, A5), "C13");
    NAME(CHORD(C4, E4, G4, Bb4, Db5), "C7(b9)");
    NAME(CHORD(C4, E4, G4, Bb4, Eb5), "C7(#9)");
    NAME(CHORD(C4, E4, G4, B4, Gb5), "Cmaj7(#11)");
    NAME(CHORD(C4, E4, G4, D5), "Cadd9");
    NAME(CHORD(C4, E4, G4, A4, D5), "C6/9");
    NAME(CHORD(C4, F4, Gb4, B4), "Cmaj7sus4(b5)");

    /* inversions: the bass decides ties, and shows after a slash */
    NAME(CHORD(E4, G4, C5), "C/E");
    NAME(CHORD(G4, C5, E5), "C/G");
    NAME(CHORD(C4, E4, G4, A4 + 12), "C6");     /* bass C: C6, not Am7/C */
    NAME(CHORD(A4 - 12, C4, E4, G4), "Am7");    /* bass A: Am7, not C6/A */
}

static void test_chord_follows_notes(void)
{
    /* Change one note and the name follows. Nothing is stored. */
    chord c = CHORD(C4, E4, G4);
    NAME(c, "C");
    c.notes[1]--;                 /* E -> Eb */
    NAME(c, "Cm");
    c.notes[2]--;                 /* G -> Gb */
    NAME(c, "Cdim");
    chord_add(&c, A4);            /* add bb7 */
    NAME(c, "Cdim7");
    c.notes[3]++;                 /* A -> Bb */
    NAME(c, "Cm7b5");
    c.notes[1]++; c.notes[2]++;   /* back to E and G */
    NAME(c, "C7");

    /* transposing every note moves the root */
    for (int i = 0; i < c.count; i++) c.notes[i] += 2;
    NAME(c, "D7");
    CHECK(chord_root(&c) == 2, "root of D7 should be pitch class 2, got %d", chord_root(&c));
}

static void test_progressions(void)
{
    /* I - vi - IV - V in C as scale degrees */
    scale s = { .root = C4, .mode = MAJOR, .kind = SCALE_MAJOR };
    const int degrees[] = { 0, 5, 3, 4 };
    const char *triads[] = { "C", "Am", "F", "G" };
    const char *sevenths[] = { "Cmaj7", "Am7", "Fmaj7", "G7" };
    for (int i = 0; i < 4; i++) {
        NAME(chord_from_scale(&s, degrees[i], 3), triads[i]);
        NAME(chord_from_scale(&s, degrees[i], 4), sevenths[i]);
    }

    /* same degrees, mode++ to dorian: the progression changes colour */
    mode_next(&s);
    NAME(chord_from_scale(&s, 0, 3), "Cm");
    NAME(chord_from_scale(&s, 3, 3), "F");      /* the dorian major IV */
    NAME(chord_from_scale(&s, 6, 4), "Bbmaj7");

    /* diatonic sevenths in harmonic minor include dim7 and m(maj7) */
    scale h = { .root = A4 - 12, .mode = 0, .kind = SCALE_HARMONIC_MINOR };
    NAME(chord_from_scale(&h, 0, 4), "Am(maj7)");
    NAME(chord_from_scale(&h, 4, 4), "E7");
    NAME(chord_from_scale(&h, 6, 4), "Abdim7");  /* G#dim7; names use flats for now */
}

int main(void)
{
    test_note();
    test_scale();
    test_chord_names();
    test_chord_follows_notes();
    test_progressions();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
