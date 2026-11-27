/*
 * symbols.c — see symbols.h.
 *
 * Two passes over the "significant" tokens (comments dropped):
 *   1. type definitions: typedef, struct/union, enum, #define
 *   2. declarations:     <type> [*...] name  ->  variable, or function if '(' follows
 */
#include "symbols.h"
#include "lexer.h"

#include <string.h>

typedef struct {
    const Language *lang;
    const char     *text;
    Token          *toks;
    guint          *sig;     /* indices of non-comment tokens */
    guint           nsig;
    SymbolTable    *table;
    gboolean       *in_body; /* per sig index: inside a struct/union body */
} Scan;

#define TOK(sc, i) (&(sc)->toks[(sc)->sig[(i)]])

static void symbol_free(gpointer p)
{
    Symbol *s = p;
    if (!s)
        return;
    g_free(s->name);
    g_free(s->type_name);
    if (s->members)
        g_ptr_array_unref(s->members);
    g_free(s);
}

static Symbol *symbol_new(char *name, SymbolKind kind, char *type_name, GPtrArray *members)
{
    Symbol *s = g_new0(Symbol, 1);
    s->name = name;
    s->kind = kind;
    s->type_name = type_name;
    s->members = members;
    return s;
}

/* Takes ownership of name/type_name and one reference of members. */
static void add_symbol(Scan *sc, char *name, SymbolKind kind, char *type_name, GPtrArray *members)
{
    Symbol *old = g_hash_table_lookup(sc->table->by_name, name);
    if (old) {
        /* "struct foo;" forward declaration followed by the real definition */
        if (old->kind == SYM_TYPE && kind == SYM_TYPE && !old->members && members) {
            old->members = members;
            members = NULL;
        }
        g_free(name);
        g_free(type_name);
        if (members)
            g_ptr_array_unref(members);
        return;
    }
    g_hash_table_insert(sc->table->by_name, name, symbol_new(name, kind, type_name, members));
}

static gboolean is_text(Scan *sc, guint i, const char *s)
{
    return i < sc->nsig && token_text_is(sc->text, TOK(sc, i), s);
}

static gboolean is_in_list(Scan *sc, guint i, const char *const *list)
{
    for (; list && *list; list++)
        if (is_text(sc, i, *list))
            return TRUE;
    return FALSE;
}

static char *tok_dup(Scan *sc, guint i)
{
    Token *t = TOK(sc, i);
    return g_strndup(sc->text + t->start, t->length);
}

static gboolean has_cat(Scan *sc, guint i, TokenCategory c)
{
    return i < sc->nsig && TOK(sc, i)->category == c;
}

/* A plain name the user could be declaring. */
static gboolean is_name(Scan *sc, guint i)
{
    return has_cat(sc, i, TOK_IDENTIFIER) || has_cat(sc, i, TOK_FUNCTION);
}

/* A name that may follow `typedef ... ` / `struct` (users may (re)define music types). */
static gboolean is_type_name(Scan *sc, guint i)
{
    return is_name(sc, i) || has_cat(sc, i, TOK_USER_TYPE) || has_cat(sc, i, TOK_MUSIC_TYPE);
}

static gboolean is_opener(Scan *sc, guint i) { return is_text(sc, i, "(") || is_text(sc, i, "[") || is_text(sc, i, "{"); }
static gboolean is_closer(Scan *sc, guint i) { return is_text(sc, i, ")") || is_text(sc, i, "]") || is_text(sc, i, "}"); }

static char *with_stars(const char *base, int stars)
{
    GString *s = g_string_new(base);
    while (stars-- > 0)
        g_string_append_c(s, '*');
    return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Pass 1: type definitions                                            */
/* ------------------------------------------------------------------ */

/* Fields of `{ ... }` starting at `open`.  *close receives the index of '}'. */
static GPtrArray *parse_struct_body(Scan *sc, guint open, guint *close)
{
    GPtrArray *members = g_ptr_array_new_with_free_func(symbol_free);
    GString *type = g_string_new(NULL);
    int depth = 0, stars = 0;
    guint j;

    for (j = open + 1; j < sc->nsig; j++) {
        if (is_opener(sc, j)) { depth++; continue; }
        if (is_closer(sc, j)) {
            if (depth == 0)
                break;
            depth--;
            continue;
        }
        if (depth > 0)
            continue;
        if (is_text(sc, j, ";")) { g_string_truncate(type, 0); stars = 0; continue; }
        if (is_text(sc, j, ",")) { stars = 0; continue; }
        if (is_text(sc, j, "*")) { stars++; continue; }

        Token *t = TOK(sc, j);
        gboolean typeish = t->category == TOK_TYPE || t->category == TOK_MUSIC_TYPE ||
                           t->category == TOK_USER_TYPE || t->category == TOK_IDENTIFIER;
        if (type->len == 0 && typeish) {
            g_string_append_len(type, sc->text + t->start, t->length);
        } else if (type->len > 0 && t->category == TOK_TYPE) {
            g_string_append_c(type, ' ');               /* unsigned int, long long */
            g_string_append_len(type, sc->text + t->start, t->length);
        } else if (type->len > 0 && is_name(sc, j) &&
                   (is_text(sc, j + 1, ";") || is_text(sc, j + 1, ",") || is_text(sc, j + 1, "[") ||
                    is_text(sc, j + 1, ":") || is_text(sc, j + 1, "="))) {
            g_ptr_array_add(members, symbol_new(tok_dup(sc, j), SYM_VARIABLE,
                                                with_stars(type->str, stars), NULL));
        }
    }
    g_string_free(type, TRUE);
    for (guint k = open; k <= j && k < sc->nsig; k++)
        sc->in_body[k] = TRUE;   /* fields are not global variables */
    *close = j;
    return members;
}

/* struct/union [tag] [{ fields }] — returns the last index consumed. */
static guint parse_aggregate(Scan *sc, guint i, GPtrArray **out_members)
{
    guint j = i + 1;
    char *tag = NULL;
    GPtrArray *members = NULL;

    if (is_type_name(sc, j))
        tag = tok_dup(sc, j++);
    if (is_text(sc, j, "{"))
        members = parse_struct_body(sc, j, &j);
    else
        j--;

    if (out_members)
        *out_members = members ? g_ptr_array_ref(members) : NULL;
    if (tag)
        add_symbol(sc, tag, SYM_TYPE, NULL, members);
    else if (members)
        g_ptr_array_unref(members);
    return j;
}

/* enum [tag] [{ A, B = 2, ... }] — returns the last index consumed. */
static guint parse_enum(Scan *sc, guint i)
{
    guint j = i + 1;
    char *tag = NULL;

    if (is_type_name(sc, j))
        tag = tok_dup(sc, j++);

    if (is_text(sc, j, "{")) {
        gboolean expect_item = TRUE;
        int depth = 0;
        for (j++; j < sc->nsig; j++) {
            if (is_opener(sc, j)) { depth++; continue; }
            if (is_closer(sc, j)) {
                if (depth == 0)
                    break;
                depth--;
                continue;
            }
            if (depth > 0)
                continue;
            if (is_text(sc, j, ",")) { expect_item = TRUE; continue; }
            if (expect_item && is_name(sc, j))
                add_symbol(sc, tok_dup(sc, j), SYM_CONSTANT, g_strdup(tag ? tag : "enum"), NULL);
            expect_item = FALSE;
        }
    } else {
        j--;
    }
    if (tag)
        add_symbol(sc, tag, SYM_TYPE, NULL, NULL);
    return j;
}

/* typedef <anything> name;   typedef struct {...} name;   typedef void (*name)(...); */
static guint parse_typedef(Scan *sc, guint i)
{
    /* find the terminating ';' */
    guint end = i + 1;
    int depth = 0;
    for (; end < sc->nsig; end++) {
        if (is_opener(sc, end)) depth++;
        else if (is_closer(sc, end)) depth--;
        else if (depth <= 0 && is_text(sc, end, ";")) break;
    }
    if (end >= sc->nsig)
        end = sc->nsig;   /* still being typed */

    GPtrArray *members = NULL;
    guint k = i + 1;
    while (k < end && has_cat(sc, k, TOK_KEYWORD) && is_text(sc, k, "const"))
        k++;
    if (is_in_list(sc, k, sc->lang->aggregate_keywords))
        k = parse_aggregate(sc, k, &members) + 1;
    else if (is_text(sc, k, sc->lang->enum_keyword))
        k = parse_enum(sc, k) + 1;

    /* function pointer: ( * name ) */
    char *name = NULL;
    for (guint j = k; j + 3 < end; j++) {
        if (is_text(sc, j, "(") && is_text(sc, j + 1, "*") && is_type_name(sc, j + 2) && is_text(sc, j + 3, ")")) {
            name = tok_dup(sc, j + 2);
            break;
        }
    }
    /* otherwise: last name at depth 0 */
    if (!name) {
        guint last = G_MAXUINT;
        depth = 0;
        for (guint j = k; j < end; j++) {
            if (is_opener(sc, j)) depth++;
            else if (is_closer(sc, j)) depth--;
            else if (depth == 0 && is_type_name(sc, j)) last = j;
        }
        if (last != G_MAXUINT)
            name = tok_dup(sc, last);
    }

    if (name)
        add_symbol(sc, name, SYM_TYPE, NULL, members);
    else if (members)
        g_ptr_array_unref(members);
    return end;
}

/* #define NAME value   /   #define NAME(args) body */
static void parse_define(Scan *sc, const Token *t)
{
    const char *p = sc->text + t->start, *e = p + t->length;
    const char *directive = sc->lang->define_directive;
    if (!directive)
        return;
    p++;                                           /* the '#' */
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    size_t dl = strlen(directive);
    if ((size_t)(e - p) <= dl || strncmp(p, directive, dl) != 0 || !g_ascii_isspace(p[dl]))
        return;
    p += dl;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    const char *ns = p;
    while (p < e && (g_ascii_isalnum(*p) || *p == '_')) p++;
    if (p == ns)
        return;
    char *name = g_strndup(ns, p - ns);
    gboolean fn_like = p < e && *p == '(';
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    char *value = g_strstrip(g_strndup(p, MIN(e - p, 40)));
    if (fn_like) {
        g_free(value);
        value = g_strdup("macro()");
    }
    add_symbol(sc, name, SYM_MACRO, value, NULL);
}

/* ------------------------------------------------------------------ */
/*  Pass 2: declarations                                                */
/* ------------------------------------------------------------------ */

static gboolean is_decl_type(Scan *sc, guint i)
{
    return has_cat(sc, i, TOK_TYPE) || has_cat(sc, i, TOK_MUSIC_TYPE) || has_cat(sc, i, TOK_USER_TYPE);
}

static void parse_declarations(Scan *sc)
{
    for (guint i = 0; i < sc->nsig; i++) {
        if (!is_decl_type(sc, i) || sc->in_body[i])
            continue;

        guint k = i + 1;
        int stars = 0;
        while (is_text(sc, k, "*")) { stars++; k++; }
        if (!is_name(sc, k))
            continue;

        char *base = tok_dup(sc, i);
        if (is_text(sc, k + 1, "(")) {
            add_symbol(sc, tok_dup(sc, k), SYM_FUNCTION, with_stars(base, stars), NULL);
            g_free(base);
            continue;   /* parameters are found by later iterations */
        }
        add_symbol(sc, tok_dup(sc, k), SYM_VARIABLE, with_stars(base, stars), NULL);

        /* more declarators: note a, *b, c = x; */
        int depth = 0;
        for (guint m = k + 1; m < sc->nsig; m++) {
            if (is_opener(sc, m)) { depth++; continue; }
            if (is_closer(sc, m)) {
                if (depth == 0)
                    break;
                depth--;
                continue;
            }
            if (depth > 0)
                continue;
            if (is_text(sc, m, ";"))
                break;
            if (is_text(sc, m, ",")) {
                guint n = m + 1;
                int s2 = 0;
                while (is_text(sc, n, "*")) { s2++; n++; }
                if (!is_name(sc, n) || is_text(sc, n + 1, "("))
                    break;
                add_symbol(sc, tok_dup(sc, n), SYM_VARIABLE, with_stars(base, s2), NULL);
                m = n;
            }
        }
        g_free(base);
    }
}

/* ------------------------------------------------------------------ */

static const Symbol *lookup_n(const SymbolTable *table, const char *s, int len)
{
    char buf[128];
    if (len <= 0 || len >= (int)sizeof buf)
        return NULL;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return g_hash_table_lookup(table->by_name, buf);
}

SymbolTable *symbols_scan(const Language *lang, const char *text, GArray *tokens)
{
    SymbolTable *table = g_new0(SymbolTable, 1);
    table->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, symbol_free);

    Token *toks = (Token *)tokens->data;
    guint *sig = g_new(guint, tokens->len + 1);
    guint nsig = 0;
    for (guint i = 0; i < tokens->len; i++)
        if (toks[i].category != TOK_COMMENT)
            sig[nsig++] = i;

    Scan sc = { lang, text, toks, sig, nsig, table, g_new0(gboolean, nsig + 1) };

    /* pass 1 */
    for (guint i = 0; i < nsig; i++) {
        Token *t = TOK(&sc, i);
        if (t->category == TOK_PREPROCESSOR) {
            parse_define(&sc, t);
        } else if (t->category == TOK_KEYWORD) {
            if (lang->typedef_keyword && is_text(&sc, i, lang->typedef_keyword))
                i = MAX(i, parse_typedef(&sc, i));
            else if (is_in_list(&sc, i, lang->aggregate_keywords))
                i = MAX(i, parse_aggregate(&sc, i, NULL));
            else if (lang->enum_keyword && is_text(&sc, i, lang->enum_keyword))
                i = MAX(i, parse_enum(&sc, i));
        }
    }

    /* re-categorise identifiers that name user types / constants */
    for (guint i = 0; i < tokens->len; i++) {
        Token *t = &toks[i];
        if (t->category != TOK_IDENTIFIER && t->category != TOK_FUNCTION)
            continue;
        const Symbol *s = lookup_n(table, text + t->start, t->length);
        if (!s)
            continue;
        if (s->kind == SYM_TYPE)
            t->category = TOK_USER_TYPE;
        else if (s->kind == SYM_CONSTANT || (s->kind == SYM_MACRO && t->category == TOK_IDENTIFIER))
            t->category = TOK_CONSTANT;
    }

    /* pass 2 */
    parse_declarations(&sc);

    g_free(sc.in_body);
    g_free(sig);
    return table;
}

void symbols_free(SymbolTable *table)
{
    if (!table)
        return;
    g_hash_table_destroy(table->by_name);
    g_free(table);
}

const Symbol *symbols_lookup(const SymbolTable *table, const char *name)
{
    return table ? g_hash_table_lookup(table->by_name, name) : NULL;
}
