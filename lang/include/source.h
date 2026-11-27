/*
 * source.h — reading a song's source text: translating the language's
 * extras into C, finding its functions, and building it into a runner.
 *
 * Shared by the command-line compiler (composerc) and the editor, so both
 * run code exactly the same way.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What a function gives back, which decides how it is played on its own. */
typedef enum {
    RET_SOUND,   /* played as is                         */
    RET_PART,    /* notes: played with a plain beep      */
    RET_NOTE,    /* a beep at that note                  */
    RET_CHORD,   /* a beep chord                         */
    RET_VOID,    /* whatever it play()ed                 */
    RET_OTHER,   /* int, float, ...: whatever it play()ed */
} src_ret;

typedef enum { PARAM_NOTE, PARAM_CHORD, PARAM_OTHER } src_param;

#define SRC_MAX_PARAMS 8

typedef struct {
    char      name[64];
    char      ret_type[32];
    src_ret   ret;
    int       n_params;
    src_param params[SRC_MAX_PARAMS];
    bool      playable;      /* no params, or only notes/chords (played with C4 / C major) */
    bool      viz;           /* "//viz" after its closing brace */
    int       header_line;   /* 0-based line of the name */
    int       end_line;      /* 0-based line of the closing brace */
} src_function;

/* Top-level function definitions, in order. Returns how many (<= max). */
int src_scan_functions(const char *text, src_function *out, int max);

/* The language's extras, rewritten as C on the same lines (so compiler
 * errors point at the right line):
 *   note literals  C4, F#3, Bb2, Eb-1   -> MIDI numbers (C4 = 60)
 *   units          2b -> BEATS(2), 3bar(s) -> BARS(3), 250ms -> MS(250),
 *                  2s -> SECONDS(2), 120bpm -> 120
 * Comments, strings and #include lines are left alone. Returns malloc'd text. */
char *src_translate(const char *text);

/* Translate, add a runner for every playable function, and compile with
 * gcc into `exe`. `label` is the file name errors are reported against.
 * Returns 0 on success; *log gets the compiler output (malloc'd, may be
 * empty). */
int src_build(const char *text, const char *label, const char *exe, char **log);

/* The generated C for the runner (for debugging). malloc'd. */
char *src_harness(const char *text, const char *label);
