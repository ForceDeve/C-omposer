/*
 * lexer.c — table-driven tokenizer.  See lexer.h.
 */
#include "lexer.h"

#include <string.h>

const char *token_category_name(TokenCategory category)
{
    static const char *const names[] = {
#define X(id, name) name,
        TOKEN_CATEGORIES(X)
#undef X
    };
    return (category >= 0 && category < TOK_COUNT) ? names[category] : "plain";
}

gboolean token_text_is(const char *text, const Token *tok, const char *s)
{
    size_t n = strlen(s);
    return (size_t)tok->length == n && memcmp(text + tok->start, s, n) == 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    const Language *lang;
    const char     *s;
    int             len;
    int             pos;         /* byte offset      */
    int             cpos;        /* character offset */
    gboolean        line_start;  /* only whitespace since the last newline */
    GArray         *out;
} Lexer;

static inline char peek(const Lexer *lx, int ahead)
{
    int p = lx->pos + ahead;
    return p < lx->len ? lx->s[p] : '\0';
}

static inline gboolean in_set(const char *set, char c)
{
    return c != '\0' && set != NULL && strchr(set, c) != NULL;
}

static inline gboolean is_ident_start(char c) { return g_ascii_isalpha(c) || c == '_'; }
static inline gboolean is_ident_char(char c)  { return g_ascii_isalnum(c) || c == '_'; }

static gboolean starts_with(const Lexer *lx, const char *str)
{
    if (str == NULL || *str == '\0')
        return FALSE;
    size_t n = strlen(str);
    return lx->pos + (int)n <= lx->len && memcmp(lx->s + lx->pos, str, n) == 0;
}

/* Advance one (possibly multi-byte) UTF-8 character. */
static void advance(Lexer *lx)
{
    if (lx->pos >= lx->len)
        return;
    int step = 1;
    if ((unsigned char)lx->s[lx->pos] >= 0x80) {
        const char *next = g_utf8_find_next_char(lx->s + lx->pos, lx->s + lx->len);
        step = next ? (int)(next - (lx->s + lx->pos)) : lx->len - lx->pos;
    }
    lx->pos  += step;
    lx->cpos += 1;
}

static void advance_n(Lexer *lx, int n)
{
    while (n-- > 0)
        advance(lx);
}

static void emit(Lexer *lx, TokenCategory cat, int start, int cstart)
{
    Token t = { cat, start, lx->pos - start, cstart, lx->cpos - cstart };
    g_array_append_val(lx->out, t);
    lx->line_start = FALSE;
}

/* Length in bytes of the identifier-like run at the cursor. */
static int ident_run(const Lexer *lx)
{
    int n = 0;
    while (is_ident_char(peek(lx, n)))
        n++;
    return n;
}

/* Byte length of a note literal (C4, F#3, Bb, Ebb2) at the cursor, or 0. */
static int match_note(const Lexer *lx)
{
    const Language *L = lx->lang;
    if (!L->note_literals || !in_set(L->note_letters, peek(lx, 0)))
        return 0;

    int n = 1;
    for (int acc = 0; acc < L->max_accidentals && in_set(L->note_accidentals, peek(lx, n)); acc++)
        n++;

    int digits = 0;
    while (digits < L->max_octave_digits && g_ascii_isdigit(peek(lx, n))) {
        n++;
        digits++;
    }
    if (L->note_requires_octave && digits == 0)
        return 0;
    if (is_ident_char(peek(lx, n)))   /* "Bbx", "C4a" -> plain identifiers */
        return 0;
    return n;
}

/* Longest operator at the cursor, or 0. */
static int match_operator(const Lexer *lx)
{
    int best = 0;
    for (const char *const *op = lx->lang->operators; op && *op; op++) {
        int n = (int)strlen(*op);
        if (n > best && starts_with(lx, *op))
            best = n;
    }
    return best;
}

static gboolean in_list(const char *const *list, const char *s, int n)
{
    for (; list && *list; list++)
        if ((int)strlen(*list) == n && memcmp(*list, s, n) == 0)
            return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */

static void lex_preprocessor(Lexer *lx)
{
    const Language *L = lx->lang;
    int start = lx->pos, cstart = lx->cpos;
    while (lx->pos < lx->len) {
        char c = peek(lx, 0);
        if (c == '\n')
            break;
        if (starts_with(lx, L->line_comment) || starts_with(lx, L->block_comment_start))
            break;
        if (c == '\\' && peek(lx, 1) == '\n') {   /* line continuation */
            advance_n(lx, 2);
            continue;
        }
        advance(lx);
    }
    emit(lx, TOK_PREPROCESSOR, start, cstart);
}

static void lex_line_comment(Lexer *lx)
{
    int start = lx->pos, cstart = lx->cpos;
    while (lx->pos < lx->len && peek(lx, 0) != '\n')
        advance(lx);
    emit(lx, TOK_COMMENT, start, cstart);
}

static void lex_block_comment(Lexer *lx)
{
    const Language *L = lx->lang;
    int start = lx->pos, cstart = lx->cpos;
    advance_n(lx, (int)strlen(L->block_comment_start));
    while (lx->pos < lx->len) {
        if (starts_with(lx, L->block_comment_end)) {
            advance_n(lx, (int)strlen(L->block_comment_end));
            break;
        }
        advance(lx);
    }
    emit(lx, TOK_COMMENT, start, cstart);
}

/* Strings and char literals.  Unterminated ones stop at the end of the line. */
static void lex_quoted(Lexer *lx, char delim, TokenCategory cat)
{
    int start = lx->pos, cstart = lx->cpos;
    advance(lx);
    while (lx->pos < lx->len) {
        char c = peek(lx, 0);
        if (c == '\n')
            break;
        if (c == lx->lang->escape_char && peek(lx, 1) != '\0' && peek(lx, 1) != '\n') {
            advance_n(lx, 2);
            continue;
        }
        advance(lx);
        if (c == delim)
            break;
    }
    emit(lx, cat, start, cstart);
}

static void lex_number(Lexer *lx)
{
    const Language *L = lx->lang;
    int start = lx->pos, cstart = lx->cpos;

    if (peek(lx, 0) == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'X') && g_ascii_isxdigit(peek(lx, 2))) {
        advance_n(lx, 2);
        while (g_ascii_isxdigit(peek(lx, 0)))
            advance(lx);
    } else {
        while (g_ascii_isdigit(peek(lx, 0)))
            advance(lx);
        if (peek(lx, 0) == '.' && peek(lx, 1) != '.') {
            advance(lx);
            while (g_ascii_isdigit(peek(lx, 0)))
                advance(lx);
        }
        char e = peek(lx, 0);
        if ((e == 'e' || e == 'E') &&
            (g_ascii_isdigit(peek(lx, 1)) ||
             ((peek(lx, 1) == '+' || peek(lx, 1) == '-') && g_ascii_isdigit(peek(lx, 2))))) {
            advance_n(lx, 2);
            while (g_ascii_isdigit(peek(lx, 0)))
                advance(lx);
        }
    }

    /* Suffix: a unit (120bpm), a C suffix (1.5f, 10UL) or garbage (12xyz). */
    int n = ident_run(lx);
    if (n == 0) {
        emit(lx, TOK_NUMBER, start, cstart);
        return;
    }
    const char *suffix = lx->s + lx->pos;
    if (in_list(L->number_units, suffix, n)) {
        emit(lx, TOK_NUMBER, start, cstart);
        int us = lx->pos, ucs = lx->cpos;
        advance_n(lx, n);
        emit(lx, TOK_UNIT, us, ucs);
        return;
    }
    gboolean c_suffix = TRUE;
    for (int i = 0; i < n; i++)
        if (!in_set(L->number_c_suffixes, suffix[i]))
            c_suffix = FALSE;
    if (c_suffix) {
        advance_n(lx, n);
        emit(lx, TOK_NUMBER, start, cstart);
        return;
    }
    emit(lx, TOK_NUMBER, start, cstart);
    int es = lx->pos, ecs = lx->cpos;
    advance_n(lx, n);
    emit(lx, TOK_ERROR, es, ecs);
}

static void lex_word(Lexer *lx)
{
    int start = lx->pos, cstart = lx->cpos;
    int n = ident_run(lx);

    /* Reserved words win over note literals ("DIM" is not D + ...). */
    const LangWord *w = language_lookup(lx->lang, lx->s + lx->pos, n);
    if (w) {
        advance_n(lx, n);
        emit(lx, w->category, start, cstart);
        return;
    }
    int note = match_note(lx);
    if (note) {
        advance_n(lx, note);
        emit(lx, TOK_NOTE, start, cstart);
        return;
    }
    advance_n(lx, n);
    emit(lx, TOK_IDENTIFIER, start, cstart);
}

/* ------------------------------------------------------------------ */

GArray *lexer_tokenize(const Language *lang, const char *text, int length)
{
    Lexer lx = {
        .lang = lang, .s = text, .len = length < 0 ? (int)strlen(text) : length,
        .line_start = TRUE,
        .out = g_array_sized_new(FALSE, FALSE, sizeof(Token), 1024),
    };

    while (lx.pos < lx.len) {
        char c = peek(&lx, 0);

        if (c == '\n') {
            advance(&lx);
            lx.line_start = TRUE;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            advance(&lx);
        } else if (lx.line_start && lang->preprocessor_char && c == lang->preprocessor_char) {
            lex_preprocessor(&lx);
        } else if (starts_with(&lx, lang->line_comment)) {
            lex_line_comment(&lx);
        } else if (starts_with(&lx, lang->block_comment_start)) {
            lex_block_comment(&lx);
        } else if (lang->string_delimiter && c == lang->string_delimiter) {
            lex_quoted(&lx, c, TOK_STRING);
        } else if (lang->char_delimiter && c == lang->char_delimiter) {
            lex_quoted(&lx, c, TOK_CHAR);
        } else if (g_ascii_isdigit(c) || (c == '.' && g_ascii_isdigit(peek(&lx, 1)))) {
            lex_number(&lx);
        } else if (is_ident_start(c)) {
            lex_word(&lx);
        } else {
            int start = lx.pos, cstart = lx.cpos;
            int op = match_operator(&lx);
            if (op) {
                advance_n(&lx, op);
                emit(&lx, TOK_OPERATOR, start, cstart);
            } else if (in_set(lang->punctuation, c)) {
                advance(&lx);
                emit(&lx, TOK_PUNCTUATION, start, cstart);
            } else {
                advance(&lx);   /* unknown character: leave unstyled */
                lx.line_start = FALSE;
            }
        }
    }

    /* identifier followed by '(' -> function name */
    Token *t = (Token *)lx.out->data;
    for (guint i = 0; i + 1 < lx.out->len; i++)
        if (t[i].category == TOK_IDENTIFIER && t[i + 1].category == TOK_PUNCTUATION &&
            token_text_is(text, &t[i + 1], "("))
            t[i].category = TOK_FUNCTION;

    return lx.out;
}
