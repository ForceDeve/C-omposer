/*
 * test_source.c — the translator (language extras -> C) and the function
 * scanner that the editor's ▶ buttons and //viz panels rely on.
 */
#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

static void expect_translation(const char *in, const char *want, int line)
{
    char *got = src_translate(in);
    checks++;
    if (strcmp(got, want)) {
        failures++;
        printf("FAIL line %d:\n  in:   %s\n  got:  %s\n  want: %s\n", line, in, got, want);
    }
    free(got);
}
#define T(in, want) expect_translation(in, want, __LINE__)
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* note literals */
    T("beep(C4, x);", "beep(60, x);");
    T("F#3 Bb2 A4 C-1 Ebb4", "54 46 69 0 62");
    T("C4x CC4 c4 H4", "C4x CC4 c4 H4");                 /* not notes */
    T("s.E4 p->A4", "s.E4 p->A4");                       /* members stay */
    /* units */
    T("beep(C4, 1b); rest(0.5b);", "beep(60, BEATS(1)); rest(BEATS(0.5));");
    T("tempo(124bpm); x = 2bars + 1bar;", "tempo(124); x = BARS(2) + BARS(1);");
    T("250ms 2s", "MS(250) SECONDS(2)");
    T("1.5f 10u 0x1F 1e3 2E-1", "1.5f 10u 0x1F 1e3 2E-1");  /* plain C numbers */
    /* left alone */
    T("printf(\"C4 1b\"); // C4 2b\n/* A4 */ 'b'", "printf(\"C4 1b\"); // C4 2b\n/* A4 */ 'b'");
    T("#include <C4.h>\nC4", "#include <C4.h>\n60");
    T("#define ROOT C4\n", "#define ROOT 60\n");
    /* line structure is kept */
    char *multi = src_translate("a\nC4\n\n1b\n");
    int nl = 0;
    for (char *p = multi; *p; p++) nl += *p == '\n';
    CHECK(nl == 4, "translation changed the line count (%d)", nl);
    free(multi);

    /* scanning functions */
    const char *src =
        "#include <composer.h>\n"                        /* 0 */
        "typedef struct { int a; } thing;\n"             /* 1 */
        "static sound beat(void) {\n"                    /* 2 */
        "    if (1) { }\n"                               /* 3 */
        "    return x;\n"                                /* 4 */
        "} //viz\n"                                      /* 5 */
        "sound lead(note n) { return y; }\n"             /* 6 */
        "sound pad(chord c, note n)\n{\n}\n"             /* 7-9 */
        "sound fx(sound s, float f) { return s; } // viz\n"  /* 10 */
        "chord am(void) { return c; }\n"                 /* 11 */
        "part tune() { \"}\"; }   //vizzz\n"             /* 12 */
        "int main(void) {\n  return 0;\n}\n";             /* 13-15 */
    src_function f[16];
    int n = src_scan_functions(src, f, 16);
    CHECK(n == 7, "found %d functions, expected 7", n);
    if (n == 7) {
        CHECK(!strcmp(f[0].name, "beat") && f[0].ret == RET_SOUND && f[0].playable && f[0].viz
              && f[0].header_line == 2 && f[0].end_line == 5, "beat");
        CHECK(!strcmp(f[1].name, "lead") && f[1].n_params == 1 && f[1].params[0] == PARAM_NOTE
              && f[1].playable && !f[1].viz, "lead");
        CHECK(!strcmp(f[2].name, "pad") && f[2].n_params == 2 && f[2].playable && f[2].end_line == 9, "pad");
        CHECK(!strcmp(f[3].name, "fx") && !f[3].playable && f[3].viz, "fx: not playable, '// viz' counts");
        CHECK(!strcmp(f[4].name, "am") && f[4].ret == RET_CHORD && f[4].playable, "am");
        CHECK(!strcmp(f[5].name, "tune") && f[5].ret == RET_PART && f[5].n_params == 0 && !f[5].viz,
              "tune: '}' in a string, and //vizzz isn't //viz");
        CHECK(!strcmp(f[6].name, "main") && f[6].ret == RET_OTHER && f[6].playable, "main");
    }
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
