/*
 * language.c — ★ THE SYNTAX DEFINITION ★
 *
 * Everything that makes the language look like C-with-music lives here:
 *   - comment / string / preprocessor style
 *   - operators and punctuation
 *   - note literals (C4, F#3, Bb2) and number units (120bpm, 500ms, 2b)
 *   - keywords, base types, music types (with their fields), built-in
 *     functions and constants, each with the docs/snippets shown by the
 *     autocompletion popup
 *   - editor behaviour (indent string, auto-closed pairs, ...)
 *
 * Want `fn` instead of `void`, `#` comments, or `loop` instead of `repeat`?
 * Change the tables below and rebuild; nothing else needs to change.
 *
 * NOTE: note/scale/chord and their fields mirror lang/include/music.h.
 * The other music types and most built-ins are placeholders until the
 * interpreter exists.
 */
#include "language.h"

#include <glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Fields of the built-in music types (shown after '.' / '->')        */
/* ------------------------------------------------------------------ */

/* note is a bare byte (uint8_t), so it has no fields. */

static const LangMember scale_members[] = {
    { "root", "note",    "Root note of the scale" },
    { "mode", "uint8_t", "Mode (IONIAN, DORIAN, ...); mode++ wraps around" },
    { "kind", "uint8_t", "Scale family (SCALE_MAJOR, SCALE_PENTATONIC, ...)" },
    { "edo",  "uint8_t", "Tuning: steps per octave (0 = 12; try 31)" },
    { NULL }
};

static const LangMember chord_members[] = {
    { "notes", "note",    "Notes of the chord (up to 8), lowest first" },
    { "count", "uint8_t", "Number of notes in the chord" },
    { "edo",   "uint8_t", "Tuning of the notes (0 = 12)" },
    { "root",  "uint8_t", "Root pitch class, computed from the notes (read-only)" },
    { "name",  "char*",   "Chord symbol, computed from the notes: \"Am7\", \"G7(b9)\" (read-only)" },
    { NULL }
};

static const LangMember duration_members[] = {
    { "beats", "float", "Length in beats (1.0 = one quarter note in 4/4)" },
    { NULL }
};

static const LangMember tempo_members[] = {
    { "bpm", "float", "Beats per minute" },
    { NULL }
};

static const LangMember time_sig_members[] = {
    { "beats", "char", "Beats per bar (top number)" },
    { "unit",  "char", "Note value of one beat (bottom number)" },
    { NULL }
};

static const LangMember instrument_members[] = {
    { "program", "char", "General MIDI program number (0-127)" },
    { "channel", "char", "MIDI channel (0-15)" },
    { NULL }
};

static const LangMember track_members[] = {
    { "name",       "char*",      "Display name of the track" },
    { "instrument", "instrument", "Instrument the track plays on" },
    { "volume",     "char",       "Track volume (0-127)" },
    { NULL }
};

static const LangMember melody_members[] = {
    { "notes",  "note*", "Notes of the melody, in order" },
    { "length", "int",   "Number of notes" },
    { NULL }
};

/* ------------------------------------------------------------------ */
/*  Reserved words                                                      */
/*  { word, category, detail, doc, snippet, type, members }             */
/* ------------------------------------------------------------------ */

#define KW(w)            { w, TOK_KEYWORD, "keyword", NULL, NULL, NULL, NULL }
#define KW_S(w, snip)    { w, TOK_KEYWORD, "snippet", NULL, snip, NULL, NULL }
#define TYPE(w)          { w, TOK_TYPE,    "type",    NULL, NULL, NULL, NULL }
#define CONST(w, t, doc) { w, TOK_CONSTANT, t, doc, NULL, t, NULL }

static const LangWord words[] = {
    /* --- C keywords --------------------------------------------------- */
    KW_S("if",     "if ($0) {\n\t\n}"),
    KW_S("else",   "else {\n\t$0\n}"),
    KW_S("for",    "for (int i = 0; i < $0; i++) {\n\t\n}"),
    KW_S("while",  "while ($0) {\n\t\n}"),
    KW_S("switch", "switch ($0) {\n\tcase :\n\t\tbreak;\n}"),
    KW("do"), KW("case"), KW("default"), KW("break"), KW("continue"),
    KW("return"), KW("goto"), KW("sizeof"), KW("typedef"), KW("struct"),
    KW("union"), KW("enum"), KW("const"), KW("static"), KW("extern"),
    KW("volatile"), KW("register"), KW("inline"), KW("restrict"),

    /* --- C base types ------------------------------------------------- */
    TYPE("void"), TYPE("char"), TYPE("short"), TYPE("int"), TYPE("long"),
    TYPE("float"), TYPE("double"), TYPE("signed"), TYPE("unsigned"),
    TYPE("bool"),
    TYPE("uint8_t"), TYPE("int8_t"), TYPE("uint16_t"), TYPE("int16_t"),

    /* --- music keywords (semantics are up to the future runtime) ------ */
    { "repeat",   TOK_MUSIC_KEYWORD, "repeat (n) { }",
      "Run the block n times.", "repeat ($0) {\n\t\n}", NULL, NULL },
    { "parallel", TOK_MUSIC_KEYWORD, "parallel { }",
      "Statements in the block start at the same time (voices/tracks).",
      "parallel {\n\t$0\n}", NULL, NULL },
    { "section",  TOK_MUSIC_KEYWORD, "section name { }",
      "A named, reusable part of the piece (verse, chorus, ...).",
      "section $0 {\n\t\n}", NULL, NULL },

    /* --- music types -------------------------------------------------- */
    { "note",       TOK_MUSIC_TYPE, "music type", "A pitch in one byte (MIDI number, C4 = 60). n++ is a semitone up.",
      NULL, NULL, NULL },
    { "scale",      TOK_MUSIC_TYPE, "music type", "Root + mode + kind (3 bytes); its notes are computed.",
      NULL, NULL, scale_members },
    { "chord",      TOK_MUSIC_TYPE, "music type", "Up to 8 notes; root and name are computed from them.",
      NULL, NULL, chord_members },
    { "duration",   TOK_MUSIC_TYPE, "music type", "A length of time measured in beats.",
      NULL, NULL, duration_members },
    { "tempo",      TOK_MUSIC_TYPE, "music type", "Speed of the music in BPM.",
      NULL, NULL, tempo_members },
    { "time_sig",   TOK_MUSIC_TYPE, "music type", "Time signature, e.g. 4/4 or 6/8.",
      NULL, NULL, time_sig_members },
    { "instrument", TOK_MUSIC_TYPE, "music type", "A MIDI instrument on a channel.",
      NULL, NULL, instrument_members },
    { "track",      TOK_MUSIC_TYPE, "music type", "One part / voice of the piece.",
      NULL, NULL, track_members },
    { "melody",     TOK_MUSIC_TYPE, "music type", "An ordered sequence of notes.",
      NULL, NULL, melody_members },

    /* --- built-in functions ------------------------------------------- */
    { "play",           TOK_BUILTIN, "void play(note n, duration d)",
      "Play a note for the given duration.", "play($0)", "void", NULL },
    { "play_chord",     TOK_BUILTIN, "void play_chord(chord c, duration d)",
      "Play every note of a chord at once.", "play_chord($0)", "void", NULL },
    { "wait",           TOK_BUILTIN, "void wait(duration d)",
      "Rest for the given duration.", "wait($0)", "void", NULL },
    { "set_tempo",      TOK_BUILTIN, "void set_tempo(float bpm)",
      "Change the tempo from this point on.", "set_tempo($0)", "void", NULL },
    { "set_instrument", TOK_BUILTIN, "void set_instrument(track t, int program)",
      "Assign a General MIDI program to a track.", "set_instrument($0)", "void", NULL },
    { "transpose",      TOK_BUILTIN, "note transpose(note n, int semitones)",
      "Shift a note up or down by semitones.", "transpose($0)", "note", NULL },
    { "make_scale",     TOK_BUILTIN, "scale make_scale(note root, char mode)",
      "Build a scale from a root note and a mode.", "make_scale($0)", "scale", NULL },
    { "make_chord",     TOK_BUILTIN, "chord make_chord(note root, char quality)",
      "Build a chord from a root note and a quality.", "make_chord($0)", "chord", NULL },
    { "invert",         TOK_BUILTIN, "chord invert(chord c, int times)",
      "Invert a chord n times.", "invert($0)", "chord", NULL },
    { "arpeggiate",     TOK_BUILTIN, "melody arpeggiate(chord c, duration step)",
      "Turn a chord into a melody of its notes.", "arpeggiate($0)", "melody", NULL },
    { "interval",       TOK_BUILTIN, "int interval(note a, note b)",
      "Distance between two notes in semitones.", "interval($0)", "int", NULL },
    { "midi",           TOK_BUILTIN, "note midi(int pitch)",
      "Make a note from a MIDI pitch number.", "midi($0)", "note", NULL },
    { "scale_degree",   TOK_BUILTIN, "note scale_degree(scale s, int degree)",
      "Note at a scale degree; degrees past the top continue an octave up.", "scale_degree($0)", "note", NULL },
    { "mode_next",      TOK_BUILTIN, "void mode_next(scale *s)",
      "Next mode, wrapping (LOCRIAN -> IONIAN).", "mode_next($0)", "void", NULL },
    { "chord_from_scale", TOK_BUILTIN, "chord chord_from_scale(scale s, int degree, int size)",
      "Stack thirds in a scale: size 3 = triad, 4 = seventh chord.", "chord_from_scale($0)", "chord", NULL },
    { "chord_name",     TOK_BUILTIN, "char *chord_name(chord c)",
      "Chord symbol computed from the notes.", "chord_name($0)", "char*", NULL },
    { "edo_note",       TOK_BUILTIN, "note edo_note(uint8_t edo, int fifths, int octave)",
      "A spelled note in a tuning: fifths from C (G=1, F=-1, F#=6, Db=-5).", "edo_note($0)", "note", NULL },
    { "edo_c4",         TOK_BUILTIN, "note edo_c4(uint8_t edo)",
      "Middle C in a tuning (60 in 12-EDO, 124 in 31-EDO).", "edo_c4($0)", "note", NULL },
    { "note_freq_edo",  TOK_BUILTIN, "double note_freq_edo(note n, uint8_t edo)",
      "Frequency in Hz of a note in a tuning.", "note_freq_edo($0)", "double", NULL },
    { "printf",         TOK_BUILTIN, "int printf(const char *fmt, ...)",
      "Print to the console (debugging).", "printf(\"$0\")", "int", NULL },

    /* --- constants ---------------------------------------------------- */
    CONST("true",  "bool", NULL), CONST("false", "bool", NULL),
    CONST("NULL",  "void*", NULL),

    CONST("MAJOR", "mode", "Major mode (Ionian)"),
    CONST("MINOR", "mode", "Natural minor mode (Aeolian)"),
    CONST("IONIAN", "mode", NULL),     CONST("DORIAN", "mode", NULL),
    CONST("PHRYGIAN", "mode", NULL),   CONST("LYDIAN", "mode", NULL),
    CONST("MIXOLYDIAN", "mode", NULL), CONST("AEOLIAN", "mode", NULL),
    CONST("LOCRIAN", "mode", NULL),

    CONST("SCALE_MAJOR", "scale_kind", "Diatonic: the 7 church modes"),
    CONST("SCALE_HARMONIC_MINOR", "scale_kind", NULL),
    CONST("SCALE_MELODIC_MINOR", "scale_kind", NULL),
    CONST("SCALE_PENTATONIC", "scale_kind", "Major pentatonic; mode 4 = minor pentatonic"),
    CONST("SCALE_BLUES", "scale_kind", NULL),
    CONST("SCALE_WHOLE_TONE", "scale_kind", NULL),
    CONST("SCALE_CHROMATIC", "scale_kind", NULL),
    CONST("SCALE_EDO_ALL", "scale_kind", "Every step of the tuning (31 notes in 31-EDO)"),

    CONST("MAJ",  "quality", "Major triad"),
    CONST("MIN",  "quality", "Minor triad"),
    CONST("DIM",  "quality", "Diminished triad"),
    CONST("AUG",  "quality", "Augmented triad"),
    CONST("SUS2", "quality", NULL), CONST("SUS4", "quality", NULL),
    CONST("DOM7", "quality", "Dominant seventh"),
    CONST("MAJ7", "quality", "Major seventh"),
    CONST("MIN7", "quality", "Minor seventh"),

    CONST("WHOLE",     "duration", "4 beats"),
    CONST("HALF",      "duration", "2 beats"),
    CONST("QUARTER",   "duration", "1 beat"),
    CONST("EIGHTH",    "duration", "1/2 beat"),
    CONST("SIXTEENTH", "duration", "1/4 beat"),
};

/* ------------------------------------------------------------------ */
/*  Operators, units, the language itself                              */
/* ------------------------------------------------------------------ */

static const char *const operators[] = {
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==",
    "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "+", "-", "*", "/", "%", "<", ">", "=", "!", "~", "&", "|", "^",
    "?", ":", ".",
    NULL
};

static const char *const number_units[] = {
    "bpm",  /* tempo        : 120bpm */
    "ms",   /* milliseconds : 250ms  */
    "s",    /* seconds      : 2s     */
    "b",    /* beats        : 4b     */
    "hz",   /* frequency    : 440hz  */
    "st",   /* semitones    : 7st    */
    "ct",   /* cents        : 25ct   */
    NULL
};

static const char *const aggregate_keywords[] = { "struct", "union", NULL };
static const char *const member_access[]      = { ".", "->", NULL };

static const Language composer_language = {
    .name                  = "C-omposer",
    .file_extension        = "cmp",

    .line_comment          = "//",
    .block_comment_start   = "/*",
    .block_comment_end     = "*/",
    .string_delimiter      = '"',
    .char_delimiter        = '\'',
    .escape_char           = '\\',
    .preprocessor_char     = '#',
    .define_directive      = "define",

    .operators             = operators,
    .punctuation           = "(){}[];,",

    .note_literals         = true,
    .note_letters          = "ABCDEFG",
    .note_accidentals      = "#b",
    .max_accidentals       = 2,
    .max_octave_digits     = 1,
    .note_requires_octave  = false,

    .number_units          = number_units,
    .number_c_suffixes     = "uUlLfF",

    .typedef_keyword       = "typedef",
    .aggregate_keywords    = aggregate_keywords,
    .enum_keyword          = "enum",
    .member_access         = member_access,

    .words                 = words,
    .n_words               = G_N_ELEMENTS(words),

    .indent                = "    ",
    .block_open            = '{',
    .block_close           = '}',
    .auto_pairs            = "()[]{}\"\"''",
    .completion_min_prefix = 2,
};

const Language *language_get(void)
{
    return &composer_language;
}

/* ------------------------------------------------------------------ */

const LangWord *language_lookup(const Language *lang, const char *s, size_t len)
{
    static GHashTable     *index;
    static const Language *indexed;

    if (indexed != lang) {
        if (index)
            g_hash_table_destroy(index);
        index = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < lang->n_words; i++)
            g_hash_table_insert(index, (gpointer)lang->words[i].word, (gpointer)&lang->words[i]);
        indexed = lang;
    }

    char buf[128];
    if (len == 0 || len >= sizeof buf)
        return NULL;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return g_hash_table_lookup(index, buf);
}
