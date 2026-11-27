/*
 * language.h — the shape of a language definition.
 *
 * The actual definition (keywords, music types, operators, note literals,
 * comment style, ...) lives in language.c.  The lexer, the symbol scanner,
 * the autocompletion and the editor's auto-indent all read from it, so
 * changing the syntax means editing that one file.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "tokens.h"

/* A field reachable with '.' / '->' on a built-in type. */
typedef struct {
    const char *name;
    const char *type;   /* type of the field, e.g. "note*" (used to resolve a.b.c chains) */
    const char *doc;
} LangMember;

/* One reserved word: keyword, type, built-in function, constant, ... */
typedef struct {
    const char       *word;
    TokenCategory     category;  /* TOK_KEYWORD, TOK_MUSIC_TYPE, TOK_BUILTIN, ... */
    const char       *detail;    /* short text on the right of the completion list */
    const char       *doc;       /* description shown under the completion list    */
    const char       *snippet;   /* inserted instead of `word` (NULL = word itself).
                                    "$0" = cursor, "\n" = newline (keeps indent),
                                    "\t" = one indent level                         */
    const char       *type;      /* builtins: return type, constants: their type    */
    const LangMember *members;   /* types: fields, terminated by {NULL}            */
} LangWord;

typedef struct {
    const char *name;
    const char *file_extension;           /* without the dot */

    /* --- comments, strings, preprocessor ------------------------------ */
    const char *line_comment;             /* "//"  (NULL to disable)       */
    const char *block_comment_start;      /* block comment opener, e.g. slash-star (NULL = none) */
    const char *block_comment_end;
    char        string_delimiter;         /* '"'                           */
    char        char_delimiter;           /* '\''  (0 to disable)          */
    char        escape_char;              /* '\\'                          */
    char        preprocessor_char;        /* '#' at line start (0 = none)  */
    const char *define_directive;         /* "define" -> #define NAME      */

    /* --- operators & punctuation -------------------------------------- */
    const char *const *operators;         /* NULL-terminated, longest match wins */
    const char *punctuation;              /* single characters             */

    /* --- note literals: C4, F#3, Bb, Ebb2 ----------------------------- */
    bool        note_literals;
    const char *note_letters;             /* "ABCDEFG"                     */
    const char *note_accidentals;         /* "#b"                          */
    int         max_accidentals;          /* 2 -> allows C## / Dbb         */
    int         max_octave_digits;        /* 1 -> C0..C9                   */
    bool        note_requires_octave;     /* true -> plain "C" is not a note */

    /* --- numbers: 120bpm, 500ms, 2b ----------------------------------- */
    const char *const *number_units;      /* NULL-terminated               */
    const char *number_c_suffixes;        /* "uUlLfF"                      */

    /* --- declarations (used to find user-defined types/functions) ----- */
    const char *typedef_keyword;          /* "typedef"                     */
    const char *const *aggregate_keywords;/* {"struct","union",NULL}      */
    const char *enum_keyword;             /* "enum"                        */
    const char *const *member_access;     /* {".", "->", NULL}            */

    /* --- reserved words ----------------------------------------------- */
    const LangWord *words;
    size_t          n_words;

    /* --- editor behaviour --------------------------------------------- */
    const char *indent;                   /* inserted by Tab / auto-indent */
    char        block_open;               /* '{' -> indent after it        */
    char        block_close;              /* '}' -> dedent when typed      */
    const char *auto_pairs;               /* pairs: "()[]{}\"\"''"        */
    int         completion_min_prefix;    /* chars typed before popup      */
} Language;

const Language *language_get(void);

/* Look up a reserved word (s need not be NUL-terminated). */
const LangWord *language_lookup(const Language *lang, const char *s, size_t len);
