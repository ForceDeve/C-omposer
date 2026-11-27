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

static const LangMember sound_members[] = {
    { "frames", "uint32_t", "Frames of audio, including reverb/release tails" },
    { "length", "uint32_t", "Musical length in frames (what play() advances by)" },
    { NULL }
};

static const LangMember part_members[] = {
    { "count",  "uint32_t", "Number of notes" },
    { "length", "uint32_t", "Length in ticks (960 per beat)" },
    { NULL }
};

static const LangMember synth_members[] = {
    { "cutoff",            "float",    "Filter cutoff in Hz" },
    { "resonance",         "float",    "0..1 (1 = self-oscillation)" },
    { "drive",             "float",    "Saturation into the filter, 0..1" },
    { "filter",            "uint8_t",  "FILTER_LADDER, FILTER_LOWPASS, ..." },
    { "filter_env_amount", "float",    "Envelope to cutoff, in octaves" },
    { "amp_env",           "envelope", "Loudness envelope" },
    { "filter_env",        "envelope", "Filter envelope" },
    { "glide",             "float",    "Seconds to slide between notes" },
    { "drift",             "float",    "Cents of analog pitch wander" },
    { "gain",              "float",    "Output level in dB" },
    { "pan",               "float",    "-1 left .. 1 right" },
    { "fm",                "float",    "Osc 2 phase-modulates osc 1" },
    { "sub",               "float",    "Sine one octave down" },
    { "noise",             "float",    "White noise level" },
    { "lfo_rate",          "float",    "LFO speed in Hz" },
    { NULL }
};

static const LangMember envelope_members[] = {
    { "attack",  "float", "Seconds" }, { "decay", "float", "Seconds" },
    { "sustain", "float", "Level 0..1" }, { "release", "float", "Seconds" },
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
    { "sound",      TOK_MUSIC_TYPE, "music type", "Rendered stereo audio. Effects take one and return one.",
      NULL, NULL, sound_members },
    { "part",       TOK_MUSIC_TYPE, "music type", "Notes over time: a melody, chords or a drum pattern.",
      NULL, NULL, part_members },
    { "synth_patch", TOK_MUSIC_TYPE, "music type", "A synth sound: oscillators, filter, envelopes.",
      NULL, NULL, synth_members },
    { "envelope",   TOK_MUSIC_TYPE, "music type", "Attack, decay, sustain, release.",
      NULL, NULL, envelope_members },
    { "drum_kit",   TOK_MUSIC_TYPE, "music type", "Drum synth settings.", NULL, NULL, NULL },
    { "sampler",    TOK_MUSIC_TYPE, "music type", "Plays a sample (or its slices) from notes.", NULL, NULL, NULL },
    { "song",       TOK_MUSIC_TYPE, "music type", "The timeline main() plays sections onto.", NULL, NULL, NULL },

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

    /* --- sound engine (lang/include/audio.h) -------------------------- */
    { "synth_render",   TOK_BUILTIN, "sound synth_render(synth_patch *patch, part *notes)",
      "Play a part on a synth.", "synth_render($0)", "sound", NULL },
    { "synth_preset",   TOK_BUILTIN, "synth_patch synth_preset(patch_preset p)",
      "Start from a preset (PATCH_SUPERSAW, PATCH_ACID_BASS, ...).", "synth_preset($0)", "synth_patch", NULL },
    { "drums_render",   TOK_BUILTIN, "sound drums_render(drum_kit *kit, part *hits)",
      "Play a part on the drum synth (KICK, SNARE, HAT, ...).", "drums_render($0)", "sound", NULL },
    { "drums_preset",   TOK_BUILTIN, "drum_kit drums_preset(kit_preset k)",
      "KIT_808, KIT_PUNCHY, KIT_LOFI.", "drums_preset($0)", "drum_kit", NULL },
    { "sound_load",     TOK_BUILTIN, "sound sound_load(char *path)",
      "Load a sample (wav natively; mp3/flac/ogg via ffmpeg).", "sound_load(\"$0\")", "sound", NULL },
    { "sampler_new",    TOK_BUILTIN, "sampler sampler_new(sound *sample, note root)",
      "Play a sample at pitch, rooted at a note.", "sampler_new($0)", "sampler", NULL },
    { "sampler_render", TOK_BUILTIN, "sound sampler_render(sampler *s, part *notes)",
      "Play a part on a sampler.", "sampler_render($0)", "sound", NULL },
    { "sound_chop",     TOK_BUILTIN, "int sound_chop(sound *s, int n, sound *out, int max)",
      "Chop into n equal slices.", "sound_chop($0)", "int", NULL },
    { "sound_chop_onsets", TOK_BUILTIN, "int sound_chop_onsets(sound *s, float sensitivity, sound *out, int max)",
      "Chop at the hits.", "sound_chop_onsets($0)", "int", NULL },
    { "sampler_slices", TOK_BUILTIN, "sampler sampler_slices(sound *slices, int n, note base)",
      "Play slices by note: base = slice 0, base+1 = slice 1 ...", "sampler_slices($0)", "sampler", NULL },
    { "sound_mix",      TOK_BUILTIN, "sound sound_mix(sound a, sound b)",
      "Layer two sounds.", "sound_mix($0)", "sound", NULL },
    { "sound_then",     TOK_BUILTIN, "sound sound_then(sound a, sound b)",
      "b starts where a ends.", "sound_then($0)", "sound", NULL },
    { "sound_repeat",   TOK_BUILTIN, "sound sound_repeat(sound s, int times)",
      "Loop a sound.", "sound_repeat($0)", "sound", NULL },
    { "sound_gain",     TOK_BUILTIN, "sound sound_gain(sound s, float db)", NULL, "sound_gain($0)", "sound", NULL },
    { "sound_pitch",    TOK_BUILTIN, "sound sound_pitch(sound s, float semitones)", NULL, "sound_pitch($0)", "sound", NULL },
    { "sound_reverse",  TOK_BUILTIN, "sound sound_reverse(sound s)", NULL, "sound_reverse($0)", "sound", NULL },
    { "fx_reverb",      TOK_BUILTIN, "sound fx_reverb(sound s, float size, float decay, float damping, float mix)",
      "Lush 8-line FDN reverb.", "fx_reverb($0)", "sound", NULL },
    { "fx_delay",       TOK_BUILTIN, "sound fx_delay(sound s, uint32_t ticks, float feedback, float tone, float mix, bool ping_pong)",
      "Tempo-synced delay.", "fx_delay($0)", "sound", NULL },
    { "fx_chorus",      TOK_BUILTIN, "sound fx_chorus(sound s, float rate, float depth_ms, float mix)", NULL, "fx_chorus($0)", "sound", NULL },
    { "fx_drive",       TOK_BUILTIN, "sound fx_drive(sound s, float drive_db, float tone, float mix)",
      "Anti-aliased tanh saturation.", "fx_drive($0)", "sound", NULL },
    { "fx_eq",          TOK_BUILTIN, "sound fx_eq(sound s, eq_kind kind, float freq, float gain_db, float q)", NULL, "fx_eq($0)", "sound", NULL },
    { "fx_compress",    TOK_BUILTIN, "sound fx_compress(sound s, float threshold_db, float ratio, float attack_ms, float release_ms, float makeup_db)",
      NULL, "fx_compress($0)", "sound", NULL },
    { "fx_pump",        TOK_BUILTIN, "sound fx_pump(sound s, uint32_t every, float depth, float release)",
      "Sidechain-style ducking on every beat.", "fx_pump($0)", "sound", NULL },
    { "fx_limit",       TOK_BUILTIN, "sound fx_limit(sound s, float ceiling_db)", NULL, "fx_limit($0)", "sound", NULL },
    { "part_new",       TOK_BUILTIN, "part part_new(uint32_t length)", "An empty part of notes.", "part_new($0)", "part", NULL },
    { "part_note",      TOK_BUILTIN, "void part_note(part *p, uint32_t start, uint32_t len, note n, uint8_t velocity)",
      NULL, "part_note($0)", "void", NULL },
    { "part_chord",     TOK_BUILTIN, "void part_chord(part *p, uint32_t start, uint32_t len, chord *c, uint8_t velocity)",
      NULL, "part_chord($0)", "void", NULL },
    { "part_pattern",   TOK_BUILTIN, "void part_pattern(part *p, char *steps, note n, uint32_t step, uint8_t velocity)",
      "Step pattern: \"x...x...\" (X = accent).", "part_pattern($0)", "void", NULL },
    { "part_repeat",    TOK_BUILTIN, "part part_repeat(part p, int times)", NULL, "part_repeat($0)", "part", NULL },
    { "BEATS",          TOK_BUILTIN, "uint32_t BEATS(float beats)", "Beats to ticks.", "BEATS($0)", "uint32_t", NULL },
    { "BARS",           TOK_BUILTIN, "uint32_t BARS(float bars)", "Bars (4/4) to ticks.", "BARS($0)", "uint32_t", NULL },
    { "song_play",      TOK_BUILTIN, "void song_play(song *s, sound section)",
      "Play a section at the cursor, then move past it.", "song_play($0)", "void", NULL },
    { "song_layer",     TOK_BUILTIN, "void song_layer(song *s, sound section)", NULL, "song_layer($0)", "void", NULL },
    { "song_export",    TOK_BUILTIN, "int song_export(song *s, char *path)",
      "Master and save (.wav, .mp3, .flac, .ogg).", "song_export($0)", "int", NULL },

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

    CONST("PATCH_SUPERSAW", "patch_preset", NULL), CONST("PATCH_WARM_PAD", "patch_preset", NULL),
    CONST("PATCH_ACID_BASS", "patch_preset", NULL), CONST("PATCH_SUB_BASS", "patch_preset", NULL),
    CONST("PATCH_REESE", "patch_preset", NULL), CONST("PATCH_PLUCK", "patch_preset", NULL),
    CONST("PATCH_LEAD", "patch_preset", NULL), CONST("PATCH_FM_KEYS", "patch_preset", NULL),
    CONST("PATCH_BRASS", "patch_preset", NULL),
    CONST("KIT_808", "kit_preset", NULL), CONST("KIT_PUNCHY", "kit_preset", NULL), CONST("KIT_LOFI", "kit_preset", NULL),
    CONST("KICK", "note", "Drum note 36"), CONST("SNARE", "note", "Drum note 38"),
    CONST("CLAP", "note", "Drum note 39"), CONST("HAT", "note", "Drum note 42"),
    CONST("OPEN_HAT", "note", "Drum note 46"), CONST("CRASH", "note", "Drum note 49"),
    CONST("RIM", "note", NULL), CONST("TOM_LOW", "note", NULL), CONST("TOM_HIGH", "note", NULL),
    CONST("FILTER_LADDER", "filter_kind", "24 dB Moog-style ladder"), CONST("FILTER_LOWPASS", "filter_kind", "12 dB SVF"),
    CONST("FILTER_BANDPASS", "filter_kind", NULL), CONST("FILTER_HIGHPASS", "filter_kind", NULL),
    CONST("FILTER_OFF", "filter_kind", NULL),
    CONST("WAVE_SAW", "waveform", NULL), CONST("WAVE_SQUARE", "waveform", NULL),
    CONST("WAVE_TRIANGLE", "waveform", NULL), CONST("WAVE_SINE", "waveform", NULL), CONST("WAVE_NOISE", "waveform", NULL),
    CONST("EQ_LOWPASS", "eq_kind", NULL), CONST("EQ_HIGHPASS", "eq_kind", NULL), CONST("EQ_PEAK", "eq_kind", NULL),
    CONST("EQ_LOWSHELF", "eq_kind", NULL), CONST("EQ_HIGHSHELF", "eq_kind", NULL),
    CONST("TICKS_PER_BEAT", "int", "960"),

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
