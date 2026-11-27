/*
 * composerc — the command-line compiler.
 *
 *   composerc song.csus                 render main() to song.mp3
 *   composerc song.csus -o song.wav     any format: wav mp3 flac ogg opus m4a
 *   composerc song.csus --call chorus   render one function on its own
 *   composerc song.csus --list          show the playable functions
 *   composerc song.csus --emit-c        print the generated C
 *
 * Write songs in any editor; this turns them into audio files.
 */
#define _POSIX_C_SOURCE 200809L
#include "source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = '\0';
    fclose(f);
    return s;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: composerc <song> [-o out.mp3] [--call function] [--list] [--emit-c]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *call = "main";
    int list = 0, emit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) call = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--emit-c")) emit = 1;
        else if (argv[i][0] == '-') return usage();
        else in = argv[i];
    }
    if (!in) return usage();
    char *text = read_file(in);
    if (!text) { fprintf(stderr, "composerc: cannot read %s: %s\n", in, strerror(errno)); return 1; }

    if (emit) {
        char *c = src_harness(text, in);
        fputs(c, stdout);
        free(c); free(text);
        return 0;
    }
    if (list) {
        src_function fn[256];
        int n = src_scan_functions(text, fn, 256);
        for (int i = 0; i < n; i++)
            printf("%-24s line %-4d %-6s %s%s\n", fn[i].name, fn[i].header_line + 1, fn[i].ret_type,
                   fn[i].playable ? "playable" : "-", fn[i].viz ? "  //viz" : "");
        free(text);
        return 0;
    }

    char dir[] = "/tmp/composerc_XXXXXX";
    if (!mkdtemp(dir)) { perror("composerc"); free(text); return 1; }
    char exe[sizeof dir + 16];
    snprintf(exe, sizeof exe, "%s/song", dir);

    char *log = NULL;
    int rc = src_build(text, in, exe, &log);
    if (log && *log) fputs(log, stderr);
    free(log);
    free(text);

    char default_out[4096];
    if (!out) {                                 /* song.csus -> song.mp3 */
        snprintf(default_out, sizeof default_out, "%s", in);
        char *dot = strrchr(default_out, '.');
        char *slash = strrchr(default_out, '/');
        if (dot && (!slash || dot > slash)) *dot = '\0';
        strncat(default_out, strcmp(call, "main") ? "_" : "", sizeof default_out - strlen(default_out) - 1);
        if (strcmp(call, "main")) strncat(default_out, call, sizeof default_out - strlen(default_out) - 1);
        strncat(default_out, ".mp3", sizeof default_out - strlen(default_out) - 1);
        out = default_out;
    }

    if (rc == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(exe, exe, "call", call, out, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        if (rc == 0) printf("wrote %s\n", out);
    } else {
        fprintf(stderr, "composerc: build failed\n");
        rc = 1;
    }

    char c_file[sizeof exe + 4];
    snprintf(c_file, sizeof c_file, "%s.c", exe);
    unlink(exe); unlink(c_file); rmdir(dir);
    return rc;
}
