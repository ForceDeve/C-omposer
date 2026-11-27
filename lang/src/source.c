/*
 * source.c — scanning, translating and building song source (see source.h).
 */
#define _POSIX_C_SOURCE 200809L
#include "source.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef COMPOSER_INCLUDE_DIR
#define COMPOSER_INCLUDE_DIR "."
#endif
#ifndef COMPOSER_LIB_FILE
#define COMPOSER_LIB_FILE "liblang_music.a"
#endif

/* ------------------------------------------------------------------ */
/*  Growable string                                                    */
/* ------------------------------------------------------------------ */

typedef struct { char *s; size_t len, cap; } str;

static void str_putn(str *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) return;
        b->s = s; b->cap = cap;
    }
    memcpy(b->s + b->len, p, n);
    b->len += n;
    b->s[b->len] = '\0';
}
static void str_put(str *b, const char *p) { str_putn(b, p, strlen(p)); }
static void str_printf(str *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) str_putn(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static char *str_take(str *b) { if (!b->s) { char *e = malloc(1); if (e) *e = 0; return e; } return b->s; }

static bool ident_start(int c) { return isalpha(c) || c == '_'; }
static bool ident_char(int c)  { return isalnum(c) || c == '_'; }

/* Skip a comment or string/char literal starting at p; returns the end, or
 * p itself if there is none. Counts newlines into *line. */
static const char *skip_inert(const char *p, int *line)
{
    if (p[0] == '/' && p[1] == '/') {
        while (*p && *p != '\n') p++;
        return p;
    }
    if (p[0] == '/' && p[1] == '*') {
        p += 2;
        while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') (*line)++; p++; }
        return *p ? p + 2 : p;
    }
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q && *p != '\n') { if (*p == '\\' && p[1]) p++; p++; }
        return *p == q ? p + 1 : p;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/*  Translation                                                        */
/* ------------------------------------------------------------------ */

/* A note literal at p (letter, up to 2 accidentals, optional '-', one
 * octave digit, then a non-identifier char). Returns its length or 0. */
static int note_literal(const char *p, int *midi)
{
    static const int pc_of[7] = { 9, 11, 0, 2, 4, 5, 7 };    /* A..G */
    if (*p < 'A' || *p > 'G') return 0;
    int pc = pc_of[*p - 'A'], i = 1;
    for (int a = 0; a < 2 && (p[i] == '#' || p[i] == 'b'); a++, i++)
        pc += p[i] == '#' ? 1 : -1;
    bool neg = false;
    if (p[i] == '-') { neg = true; i++; }
    if (!isdigit((unsigned char)p[i])) return 0;
    int octave = p[i] - '0';
    i++;
    if (ident_char((unsigned char)p[i])) return 0;
    if (neg) octave = -octave;
    *midi = (octave + 1) * 12 + pc;
    return i;
}

char *src_translate(const char *text)
{
    str out = { 0 };
    const char *p = text;
    int line = 0;
    bool line_start = true;
    char prev = 0;                         /* last non-space char written */

    while (*p) {
        /* #include lines stay as they are */
        if (line_start) {
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') {
                const char *d = q + 1;
                while (*d == ' ' || *d == '\t') d++;
                if (!strncmp(d, "include", 7)) {
                    const char *e = strchr(p, '\n');
                    size_t n = e ? (size_t)(e - p) : strlen(p);
                    str_putn(&out, p, n);
                    p += n;
                    continue;
                }
            }
        }
        line_start = false;

        const char *e = skip_inert(p, &line);
        if (e != p) { str_putn(&out, p, (size_t)(e - p)); p = e; prev = 'x'; continue; }

        if (*p == '\n') { str_putn(&out, p, 1); p++; line++; line_start = true; continue; }

        /* identifiers (and note literals) */
        if (ident_start((unsigned char)*p)) {
            int midi, n = 0;
            bool member = prev == '.' || prev == '>';
            if (!member && (n = note_literal(p, &midi)) > 0) {
                str_printf(&out, "%d", midi);
                p += n;
            } else {
                const char *s = p;
                while (ident_char((unsigned char)*p)) p++;
                /* a note literal containing '#' isn't an identifier: F#3 */
                str_putn(&out, s, (size_t)(p - s));
            }
            prev = 'x';
            continue;
        }

        /* numbers, with an optional unit */
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            const char *s = p;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (isxdigit((unsigned char)*p)) p++;
            } else {
                while (isdigit((unsigned char)*p) || *p == '.') p++;
                if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
                    ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
                    p += 2;
                    while (isdigit((unsigned char)*p)) p++;
                }
            }
            const char *u = p;
            while (isalpha((unsigned char)*u)) u++;
            size_t ulen = (size_t)(u - p), nlen = (size_t)(p - s);
            const char *wrap = NULL;
            if      (ulen == 3 && !strncmp(p, "bpm", 3))  wrap = "";
            else if (ulen == 1 && *p == 'b')              wrap = "BEATS";
            else if ((ulen == 3 && !strncmp(p, "bar", 3)) || (ulen == 4 && !strncmp(p, "bars", 4))) wrap = "BARS";
            else if (ulen == 2 && !strncmp(p, "ms", 2))   wrap = "MS";
            else if (ulen == 1 && *p == 's')              wrap = "SECONDS";
            if (wrap) {
                if (*wrap) { str_put(&out, wrap); str_put(&out, "("); }
                str_putn(&out, s, nlen);
                if (*wrap) str_put(&out, ")");
                p = u;
            } else {
                while (ident_char((unsigned char)*p)) p++;   /* C suffixes: 1.0f, 10u */
                str_putn(&out, s, (size_t)(p - s));
            }
            prev = '0';
            continue;
        }

        str_putn(&out, p, 1);
        if (*p != ' ' && *p != '\t' && *p != '\r') prev = *p;
        p++;
    }
    return str_take(&out);
}

/* ------------------------------------------------------------------ */
/*  Finding functions                                                  */
/* ------------------------------------------------------------------ */

typedef struct { const char *s; int len, line; } tok;

/* Tokenise into identifiers/numbers and single punctuation characters,
 * skipping comments, strings and preprocessor lines. */
static tok *tokenize(const char *text, int *count)
{
    int cap = 1024, n = 0, line = 0;
    tok *t = malloc(cap * sizeof *t);
    if (!t) { *count = 0; return NULL; }
    const char *p = text;
    bool line_start = true;
    while (*p) {
        if (*p == '\n') { line++; p++; line_start = true; continue; }
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (line_start && *p == '#') {                 /* preprocessor: skip line (with \ continuations) */
            while (*p && *p != '\n') { if (*p == '\\' && p[1] == '\n') { p++; line++; } p++; }
            continue;
        }
        line_start = false;
        const char *e = skip_inert(p, &line);
        if (e != p) { p = e; continue; }
        if (n == cap) {
            tok *bigger = realloc(t, (cap *= 2) * sizeof *t);
            if (!bigger) break;
            t = bigger;
        }
        if (ident_char((unsigned char)*p)) {
            const char *s = p;
            while (ident_char((unsigned char)*p)) p++;
            t[n++] = (tok){ s, (int)(p - s), line };
        } else {
            t[n++] = (tok){ p, 1, line };
            p++;
        }
    }
    *count = n;
    return t;
}

static bool tok_is(const tok *t, const char *s) { return t->len == (int)strlen(s) && !strncmp(t->s, s, t->len); }

static src_param param_kind(const tok *t, int a, int b)
{
    /* last identifier is the name; the rest is the type (no pointers) */
    int words = 0;
    bool ptr = false;
    const tok *type = NULL;
    for (int i = a; i < b; i++) {
        if (t[i].len == 1 && (t[i].s[0] == '*' || t[i].s[0] == '[')) ptr = true;
        if (ident_char((unsigned char)t[i].s[0]) && !tok_is(&t[i], "const")) {
            if (words == 0) type = &t[i];
            words++;
        }
    }
    if (ptr || !type) return PARAM_OTHER;
    if (tok_is(type, "note")) return PARAM_NOTE;
    if (tok_is(type, "chord")) return PARAM_CHORD;
    return PARAM_OTHER;
}

static bool rest_of_line_has_viz(const tok *close)
{
    const char *p = close->s + 1;
    while (*p && *p != '\n') {
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            return !strncmp(p, "viz", 3) && !ident_char((unsigned char)p[3]);
        }
        p++;
    }
    return false;
}

int src_scan_functions(const char *text, src_function *out, int max)
{
    int n_tok, found = 0;
    tok *t = tokenize(text, &n_tok);
    if (!t) return 0;
    int stmt_start = 0;                    /* first token of the current top-level statement */
    for (int i = 0; i < n_tok && found < max; i++) {
        char c = t[i].len == 1 ? t[i].s[0] : 0;
        if (c == ';' || c == '}') { stmt_start = i + 1; continue; }
        if (c != '{') continue;

        /* top-level '{': a function body if it follows ')' */
        int body = i;
        int close = i - 1;
        bool is_function = close >= 0 && t[close].len == 1 && t[close].s[0] == ')';
        int d = 0, open = close;
        if (is_function) {
            for (; open >= stmt_start; open--) {
                if (t[open].len == 1 && t[open].s[0] == ')') d++;
                if (t[open].len == 1 && t[open].s[0] == '(' && --d == 0) break;
            }
            is_function = open > stmt_start && ident_start((unsigned char)t[open - 1].s[0]);
        }
        if (!is_function) {
            /* struct/enum/initialiser: skip to its matching brace */
            d = 0;
            for (; i < n_tok; i++) {
                if (t[i].len == 1 && t[i].s[0] == '{') d++;
                if (t[i].len == 1 && t[i].s[0] == '}' && --d == 0) break;
            }
            continue;
        }
        int name_i = open - 1;

        src_function f = { 0 };
        snprintf(f.name, sizeof f.name, "%.*s", t[name_i].len, t[name_i].s);
        f.header_line = t[name_i].line;

        /* return type: words before the name, minus storage qualifiers */
        bool ptr = false;
        for (int k = stmt_start; k < name_i; k++) {
            if (t[k].len == 1 && t[k].s[0] == '*') { ptr = true; continue; }
            if (tok_is(&t[k], "static") || tok_is(&t[k], "inline") || tok_is(&t[k], "extern")
                || tok_is(&t[k], "const")) continue;
            snprintf(f.ret_type, sizeof f.ret_type, "%.*s", t[k].len, t[k].s);
        }
        if (ptr)                               f.ret = RET_OTHER;
        else if (!strcmp(f.ret_type, "sound")) f.ret = RET_SOUND;
        else if (!strcmp(f.ret_type, "part"))  f.ret = RET_PART;
        else if (!strcmp(f.ret_type, "note"))  f.ret = RET_NOTE;
        else if (!strcmp(f.ret_type, "chord")) f.ret = RET_CHORD;
        else if (!strcmp(f.ret_type, "void"))  f.ret = RET_VOID;
        else                                   f.ret = RET_OTHER;

        /* parameters */
        bool all_known = true;
        int a = open + 1;
        if (!(close == a || (close == a + 1 && tok_is(&t[a], "void")))) {
            int pd = 0;
            for (int k = open + 1; k <= close; k++) {
                char ch = t[k].len == 1 ? t[k].s[0] : 0;
                if (ch == '(') pd++;
                if ((ch == ',' && pd == 0) || k == close) {
                    src_param pk = param_kind(t, a, k);
                    if (f.n_params < SRC_MAX_PARAMS) f.params[f.n_params] = pk;
                    f.n_params++;
                    if (pk == PARAM_OTHER) all_known = false;
                    a = k + 1;
                }
                if (ch == ')') pd--;
            }
        }
        f.playable = all_known && f.n_params <= SRC_MAX_PARAMS;

        /* body: find the closing brace */
        d = 0;
        for (i = body; i < n_tok; i++) {
            if (t[i].len == 1 && t[i].s[0] == '{') d++;
            if (t[i].len == 1 && t[i].s[0] == '}' && --d == 0) break;
        }
        if (i >= n_tok) { f.end_line = t[n_tok - 1].line; i = n_tok - 1; }
        else { f.end_line = t[i].line; f.viz = rest_of_line_has_viz(&t[i]); }
        out[found++] = f;
        stmt_start = i + 1;
    }
    free(t);
    return found;
}

/* ------------------------------------------------------------------ */
/*  The runner                                                         */
/* ------------------------------------------------------------------ */

char *src_harness(const char *text, const char *label)
{
    src_function fn[256];
    int n = src_scan_functions(text, fn, 256);
    char *body = src_translate(text);
    str h = { 0 };

    str_put(&h, "/* generated by C-omposer: your code plus a runner */\n");
    str_put(&h, "#include <composer.h>\n");
    str_put(&h, "#define main composer_user_main\n");
    str_printf(&h, "#line 1 \"%s\"\n", label);
    str_put(&h, body ? body : "");
    free(body);
    str_put(&h, "\n#undef main\n\n");

    /* one wrapper per playable function: call it, turn the result into sound */
    int entries = 0;
    for (int i = 0; i < n; i++) {
        if (!fn[i].playable) continue;
        bool is_main = !strcmp(fn[i].name, "main");
        const char *callee = is_main ? "composer_user_main" : fn[i].name;
        str_printf(&h, "static sound composer_call_%d(void) {\n    ", i);
        str str_args = { 0 };
        for (int k = 0; k < fn[i].n_params; k++) {
            if (k) str_put(&str_args, ", ");
            str_put(&str_args, fn[i].params[k] == PARAM_NOTE ? "composer_default_note()" : "composer_default_chord()");
        }
        char *args = str_take(&str_args);
        switch (fn[i].ret) {
        case RET_SOUND: str_printf(&h, "return %s(%s);\n", callee, args); break;
        case RET_PART:  str_printf(&h, "return beep_part(%s(%s));\n", callee, args); break;
        case RET_NOTE:  str_printf(&h, "return beep(%s(%s), BEATS(1));\n", callee, args); break;
        case RET_CHORD: str_printf(&h, "return beep_chord(%s(%s), BEATS(2));\n", callee, args); break;
        default:        str_printf(&h, "%s(%s);\n    return composer_take_song();\n", callee, args); break;
        }
        free(args);
        str_put(&h, "}\n");
        entries++;
    }
    str_put(&h, "\nstatic const composer_entry composer_entries[] = {\n");
    for (int i = 0; i < n; i++)
        if (fn[i].playable)
            str_printf(&h, "    { \"%s\", composer_call_%d },\n", fn[i].name, i);
    str_put(&h, "    { 0, 0 }\n};\n\n");
    str_printf(&h, "int main(int argc, char **argv)\n{\n    return composer_run(argc, argv, composer_entries, %d);\n}\n", entries);
    return str_take(&h);
}

int src_build(const char *text, const char *label, const char *exe, char **log)
{
    *log = NULL;
    char *harness = src_harness(text, label);
    if (!harness) return -1;

    char cpath[4096];
    snprintf(cpath, sizeof cpath, "%s.c", exe);
    FILE *f = fopen(cpath, "w");
    if (!f) { free(harness); *log = strdup("cannot write the generated source\n"); return -1; }
    fputs(harness, f);
    fclose(f);
    free(harness);

    char *argv[] = {
        "gcc", "-std=gnu2x", "-O1", "-fdiagnostics-color=never", "-Wno-main",
        "-I", COMPOSER_INCLUDE_DIR, "-o", (char *)exe, cpath, COMPOSER_LIB_FILE, "-lm", NULL
    };
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        dup2(pipefd[1], 1); dup2(pipefd[1], 2);
        close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "could not run gcc: %s\n", strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);
    str out = { 0 };
    char buf[4096];
    ssize_t got;
    while ((got = read(pipefd[0], buf, sizeof buf)) > 0) str_putn(&out, buf, (size_t)got);
    close(pipefd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    *log = str_take(&out);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
