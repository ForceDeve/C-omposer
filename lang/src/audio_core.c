/*
 * audio_core.c — sound and part values, the song timeline, file I/O.
 */
#define _POSIX_C_SOURCE 200809L
#include "audio_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

audio_ctx audio = { .rate = 48000, .bpm = 120.0 };

uint32_t ticks_to_frames(uint32_t ticks)
{
    return (uint32_t)llround((double)ticks * 60.0 * audio.rate / (audio.bpm * TICKS_PER_BEAT));
}

double ticks_to_seconds(uint32_t ticks)
{
    return (double)ticks * 60.0 / (audio.bpm * TICKS_PER_BEAT);
}

/* ------------------------------------------------------------------ */
/*  sound                                                              */
/* ------------------------------------------------------------------ */

sound sound_new(uint32_t frames)
{
    sound s = { 0 };
    if (frames == 0) return s;
    s.l = calloc(frames, sizeof(float));
    s.r = calloc(frames, sizeof(float));
    if (!s.l || !s.r) { free(s.l); free(s.r); return (sound){ 0 }; }
    s.frames = s.length = frames;
    return s;
}

sound sound_silence(uint32_t ticks)
{
    uint32_t n = ticks_to_frames(ticks);
    sound s = sound_new(n ? n : 1);
    s.length = n;
    return s;
}

void sound_free(sound *s)
{
    free(s->l); free(s->r);
    *s = (sound){ 0 };
}

bool sound_resize(sound *s, uint32_t frames)
{
    if (frames <= s->frames) { s->frames = frames ? frames : s->frames; return true; }
    float *l = realloc(s->l, frames * sizeof(float));
    if (!l) return false;
    s->l = l;
    float *r = realloc(s->r, frames * sizeof(float));
    if (!r) return false;
    s->r = r;
    memset(s->l + s->frames, 0, (frames - s->frames) * sizeof(float));
    memset(s->r + s->frames, 0, (frames - s->frames) * sizeof(float));
    s->frames = frames;
    return true;
}

float sound_peak(const sound *s)
{
    float p = 0;
    for (uint32_t i = 0; i < s->frames; i++) {
        float a = fabsf(s->l[i]), b = fabsf(s->r[i]);
        if (a > p) p = a;
        if (b > p) p = b;
    }
    return p;
}

/* Add b into a starting at frame `at` (a grows as needed). */
static void mix_into(sound *a, const sound *b, uint32_t at)
{
    if (!b->frames) return;
    if (at + b->frames > a->frames && !sound_resize(a, at + b->frames)) return;
    for (uint32_t i = 0; i < b->frames; i++) {
        a->l[at + i] += b->l[i];
        a->r[at + i] += b->r[i];
    }
}

sound sound_mix(sound a, sound b)
{
    if (!a.frames) return b;
    uint32_t len = a.length > b.length ? a.length : b.length;
    mix_into(&a, &b, 0);
    a.length = len;
    sound_free(&b);
    return a;
}

sound sound_then(sound a, sound b)
{
    if (!a.frames && !a.length) return b;
    uint32_t at = a.length;
    mix_into(&a, &b, at);
    a.length = at + b.length;
    sound_free(&b);
    return a;
}

sound sound_repeat(sound s, int times)
{
    if (times <= 1) return s;
    uint32_t tail = s.frames > s.length ? s.frames - s.length : 0;
    sound out = sound_new(s.length * (uint32_t)times + tail);
    if (!out.frames) return s;
    out.length = 0;
    for (int i = 0; i < times; i++) {
        mix_into(&out, &s, out.length);
        out.length += s.length;
    }
    sound_free(&s);
    return out;
}

sound sound_gain(sound s, float db)
{
    float g = db_to_gain(db);
    for (uint32_t i = 0; i < s.frames; i++) { s.l[i] *= g; s.r[i] *= g; }
    return s;
}

sound sound_pan(sound s, float pan)
{
    /* balance law: the far side is attenuated, the near side kept */
    pan = clampf(pan, -1, 1);
    float gl = pan > 0 ? cosf(pan * (float)M_PI / 2) : 1.0f;
    float gr = pan < 0 ? cosf(-pan * (float)M_PI / 2) : 1.0f;
    for (uint32_t i = 0; i < s.frames; i++) { s.l[i] *= gl; s.r[i] *= gr; }
    return s;
}

sound sound_reverse(sound s)
{
    for (uint32_t i = 0, j = s.frames ? s.frames - 1 : 0; i < j; i++, j--) {
        float t = s.l[i]; s.l[i] = s.l[j]; s.l[j] = t;
        t = s.r[i]; s.r[i] = s.r[j]; s.r[j] = t;
    }
    s.length = s.frames;
    return s;
}

sound sound_pitch(sound s, float semitones)
{
    double speed = pow(2.0, semitones / 12.0);
    uint32_t nl, nr;
    float *l = resample_channel(s.l, s.frames, speed, &nl);
    float *r = resample_channel(s.r, s.frames, speed, &nr);
    if (!l || !r) { free(l); free(r); return s; }
    sound out = { l, r, nl < nr ? nl : nr, (uint32_t)(s.length / speed) };
    sound_free(&s);
    return out;
}

sound sound_trim(sound s, uint32_t start, uint32_t frames)
{
    if (start >= s.frames) { sound_free(&s); return sound_new(1); }
    if (frames == 0 || start + frames > s.frames) frames = s.frames - start;
    sound out = sound_new(frames);
    memcpy(out.l, s.l + start, frames * sizeof(float));
    memcpy(out.r, s.r + start, frames * sizeof(float));
    sound_free(&s);
    return out;
}

/* ------------------------------------------------------------------ */
/*  part                                                               */
/* ------------------------------------------------------------------ */

part part_new(uint32_t length_ticks) { return (part){ .length = length_ticks }; }

void part_free(part *p) { free(p->events); *p = (part){ 0 }; }

part part_copy(const part *p)
{
    part c = *p;
    c.events = NULL; c.cap = 0;
    if (p->count) {
        c.events = malloc(p->count * sizeof *c.events);
        if (!c.events) { c.count = 0; return c; }
        memcpy(c.events, p->events, p->count * sizeof *c.events);
        c.cap = p->count;
    }
    return c;
}

static void push(part *p, note_event e)
{
    if (p->count == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 16;
        note_event *ev = realloc(p->events, cap * sizeof *ev);
        if (!ev) return;
        p->events = ev; p->cap = cap;
    }
    p->events[p->count++] = e;
    if (e.start + e.length > p->length) p->length = e.start + e.length;
}

void part_note_edo(part *p, uint32_t start, uint32_t len, note n, uint8_t edo, uint8_t velocity)
{
    if (!velocity) return;
    push(p, (note_event){ start, len, n, velocity > 127 ? 127 : velocity, edo });
}

void part_note(part *p, uint32_t start, uint32_t len, note n, uint8_t velocity)
{
    part_note_edo(p, start, len, n, 0, velocity);
}

void part_chord(part *p, uint32_t start, uint32_t len, const chord *c, uint8_t velocity)
{
    for (int i = 0; i < c->count && i < CHORD_MAX; i++)
        part_note_edo(p, start, len, c->notes[i], c->edo, velocity);
}

void part_pattern(part *p, const char *steps, note n, uint32_t step_ticks, uint8_t velocity)
{
    uint32_t t = 0;
    for (const char *c = steps; *c; c++) {
        if (*c == 'x' || *c == 'X') {
            unsigned v = *c == 'X' ? velocity + 25u : velocity;
            part_note(p, t, step_ticks, n, (uint8_t)(v > 127 ? 127 : v));
        } else if (*c != '.' && *c != '-') {
            continue;                                /* grouping chars */
        }
        t += step_ticks;
    }
    if (t > p->length) p->length = t;
}

part part_then(part a, part b)
{
    uint32_t off = a.length;
    for (uint32_t i = 0; i < b.count; i++) {
        note_event e = b.events[i];
        e.start += off;
        push(&a, e);
    }
    a.length = off + b.length;
    part_free(&b);
    return a;
}

part part_repeat(part p, int times)
{
    part out = part_new(0);
    for (int i = 0; i < times; i++)
        out = part_then(out, part_copy(&p));
    part_free(&p);
    return out;
}

part part_transpose(part p, int steps)
{
    for (uint32_t i = 0; i < p.count; i++)
        p.events[i].pitch = (note)(p.events[i].pitch + steps);
    return p;
}

/* ------------------------------------------------------------------ */
/*  song                                                               */
/* ------------------------------------------------------------------ */

song song_new(void) { return (song){ 0 }; }

void song_layer(song *s, sound section)
{
    mix_into(&s->mix, &section, s->cursor);
    if (s->cursor + section.length > s->mix.length)
        s->mix.length = s->cursor + section.length;
    sound_free(&section);
}

void song_play(song *s, sound section)
{
    uint32_t len = section.length;
    song_layer(s, section);
    s->cursor += len;
}

void song_rest(song *s, uint32_t ticks) { s->cursor += ticks_to_frames(ticks); }

int song_export(song *s, const char *path)
{
    if (!s->mix.frames) return -1;
    sound m = s->mix;
    s->mix = (sound){ 0 };
    /* DC filter (one-pole high-pass at ~5 Hz), then the limiter */
    float a = expf(-2.0f * (float)M_PI * 5.0f / audio.rate);
    float xl = 0, yl = 0, xr = 0, yr = 0;
    for (uint32_t i = 0; i < m.frames; i++) {
        float l = m.l[i], r = m.r[i];
        yl = a * (yl + l - xl); xl = l; m.l[i] = yl;
        yr = a * (yr + r - xr); xr = r; m.r[i] = yr;
    }
    m = fx_limit(m, -1.0f);                 /* -1 dBTP: safe for mp3/ogg/aac */
    int rc = sound_save(&m, path);
    s->mix = m;
    return rc;
}

void song_free(song *s) { sound_free(&s->mix); s->cursor = 0; }

/* ------------------------------------------------------------------ */
/*  WAV files                                                          */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static sound to_engine_rate(sound s, uint32_t file_rate)
{
    if (file_rate == audio.rate || !s.frames) return s;
    double speed = (double)file_rate / audio.rate;
    uint32_t nl, nr;
    float *l = resample_channel(s.l, s.frames, speed, &nl);
    float *r = resample_channel(s.r, s.frames, speed, &nr);
    if (!l || !r) { free(l); free(r); return s; }
    sound out = { l, r, nl < nr ? nl : nr, nl < nr ? nl : nr };
    sound_free(&s);
    return out;
}

static sound load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (sound){ 0 };
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size > 12 ? malloc((size_t)size) : NULL;
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); fclose(f); return (sound){ 0 }; }
    fclose(f);

    sound out = { 0 };
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) goto done;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    for (long pos = 12; pos + 8 <= size;) {
        uint32_t len = rd32(buf + pos + 4);
        const uint8_t *body = buf + pos + 8;
        if (len > (uint32_t)(size - pos - 8)) len = (uint32_t)(size - pos - 8);
        if (!memcmp(buf + pos, "fmt ", 4) && len >= 16) {
            format = rd16(body); channels = rd16(body + 2);
            rate = rd32(body + 4); bits = rd16(body + 14);
            if (format == 0xFFFE && len >= 26) format = rd16(body + 24);  /* extensible */
        } else if (!memcmp(buf + pos, "data", 4)) {
            data = body; data_len = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || !channels || !rate || !(format == 1 || format == 3)) goto done;

    uint32_t bps = bits / 8, frame_bytes = bps * channels;
    if (!bps || !frame_bytes) goto done;
    uint32_t frames = data_len / frame_bytes;
    out = sound_new(frames);
    if (!out.frames) goto done;

    for (uint32_t i = 0; i < frames; i++) {
        float ch[2] = { 0, 0 };
        for (int c = 0; c < 2 && c < channels; c++) {
            const uint8_t *p = data + i * frame_bytes + c * bps;
            float v = 0;
            if (format == 3 && bits == 32)      { float x; memcpy(&x, p, 4); v = x; }
            else if (format == 3 && bits == 64) { double x; memcpy(&x, p, 8); v = (float)x; }
            else if (bits == 8)  v = (p[0] - 128) / 128.0f;
            else if (bits == 16) v = (int16_t)rd16(p) / 32768.0f;
            else if (bits == 24) v = ((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
            else if (bits == 32) v = (int32_t)rd32(p) / 2147483648.0f;
            ch[c] = v;
        }
        out.l[i] = ch[0];
        out.r[i] = channels == 1 ? ch[0] : ch[1];
    }
    out = to_engine_rate(out, rate);
done:
    free(buf);
    return out;
}

static void wr32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void wr16(FILE *f, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static int save_wav24(const sound *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data_len = s->frames * 6;
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + data_len); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, 2);
    wr32(f, audio.rate); wr32(f, audio.rate * 6); wr16(f, 6); wr16(f, 24);
    fwrite("data", 1, 4, f); wr32(f, data_len);

    rng r = { 0x5EED1234u };
    uint8_t *buf = malloc(6 * 4096);
    if (!buf) { fclose(f); return -1; }
    for (uint32_t i = 0; i < s->frames; i += 4096) {
        uint32_t n = s->frames - i < 4096 ? s->frames - i : 4096;
        for (uint32_t j = 0; j < n; j++) {
            float ch[2] = { s->l[i + j], s->r[i + j] };
            for (int c = 0; c < 2; c++) {
                /* TPDF dither at 1 LSB */
                double v = ch[c] * 8388607.0 + (rng_float(&r) - rng_float(&r));
                long q = lround(v);
                if (q > 8388607) q = 8388607;
                if (q < -8388608) q = -8388608;
                uint8_t *o = buf + j * 6 + c * 3;
                o[0] = (uint8_t)q; o[1] = (uint8_t)(q >> 8); o[2] = (uint8_t)(q >> 16);
            }
        }
        fwrite(buf, 1, n * 6, f);
    }
    free(buf);
    return fclose(f) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  ffmpeg bridge (no shell: arguments go straight to execvp)          */
/* ------------------------------------------------------------------ */

static pid_t spawn(char *const argv[], int *fd, bool child_reads)
{
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(child_reads ? p[0] : p[1], child_reads ? 0 : 1);
        close(p[0]); close(p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(child_reads ? p[0] : p[1]);
    *fd = child_reads ? p[1] : p[0];
    return pid;
}

static int wait_ok(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* "file:" so a path starting with '-' can't be read as an option */
static char *file_url(const char *path)
{
    size_t n = strlen(path) + 6;
    char *u = malloc(n);
    if (u) snprintf(u, n, "file:%s", path);
    return u;
}

static sound load_ffmpeg(const char *path)
{
    char rate[16];
    snprintf(rate, sizeof rate, "%u", audio.rate);
    char *url = file_url(path);
    if (!url) return (sound){ 0 };
    char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-i", url,
                     "-f", "f32le", "-ac", "2", "-ar", rate, "-", NULL };
    int fd;
    pid_t pid = spawn(argv, &fd, false);
    free(url);
    if (pid < 0) return (sound){ 0 };

    size_t cap = 1 << 20, len = 0;
    float *raw = malloc(cap);
    ssize_t got;
    while (raw && (got = read(fd, (char *)raw + len, cap - len)) > 0) {
        len += (size_t)got;
        if (len == cap) {
            float *bigger = realloc(raw, cap *= 2);
            if (!bigger) { free(raw); raw = NULL; }
            else raw = bigger;
        }
    }
    close(fd);
    if (wait_ok(pid) != 0 || !raw) { free(raw); return (sound){ 0 }; }

    uint32_t frames = (uint32_t)(len / 8);
    sound s = sound_new(frames);
    for (uint32_t i = 0; i < s.frames; i++) { s.l[i] = raw[2 * i]; s.r[i] = raw[2 * i + 1]; }
    free(raw);
    return s;
}

static int save_ffmpeg(const sound *s, const char *path, const char *ext)
{
    char rate[16];
    snprintf(rate, sizeof rate, "%u", audio.rate);
    const char *codec[6] = { NULL };
    if      (!strcasecmp(ext, "mp3"))  { codec[0] = "-c:a"; codec[1] = "libmp3lame"; codec[2] = "-b:a"; codec[3] = "320k"; }
    else if (!strcasecmp(ext, "ogg"))  { codec[0] = "-c:a"; codec[1] = "libvorbis";  codec[2] = "-q:a"; codec[3] = "8"; }
    else if (!strcasecmp(ext, "opus")) { codec[0] = "-c:a"; codec[1] = "libopus";    codec[2] = "-b:a"; codec[3] = "256k"; }
    else if (!strcasecmp(ext, "flac")) { codec[0] = "-c:a"; codec[1] = "flac"; codec[2] = "-sample_fmt"; codec[3] = "s32"; }
    else if (!strcasecmp(ext, "m4a"))  { codec[0] = "-c:a"; codec[1] = "aac"; codec[2] = "-b:a"; codec[3] = "256k"; }

    char *argv[24];
    int a = 0;
    const char *head[] = { "ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", rate, "-ac", "2", "-i", "-" };
    for (size_t i = 0; i < sizeof head / sizeof *head; i++) argv[a++] = (char *)head[i];
    for (int i = 0; codec[i]; i++) argv[a++] = (char *)codec[i];
    char *url = file_url(path);
    if (!url) return -1;
    argv[a++] = url;
    argv[a] = NULL;

    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    int fd;
    pid_t pid = spawn(argv, &fd, true);
    free(url);
    if (pid < 0) { signal(SIGPIPE, old); return -1; }
    float buf[2 * 4096];
    int rc = 0;
    for (uint32_t i = 0; i < s->frames && rc == 0; i += 4096) {
        uint32_t n = s->frames - i < 4096 ? s->frames - i : 4096;
        for (uint32_t j = 0; j < n; j++) {
            buf[2 * j] = clampf(s->l[i + j], -1, 1);
            buf[2 * j + 1] = clampf(s->r[i + j], -1, 1);
        }
        size_t want = n * 2 * sizeof(float), off = 0;
        while (off < want) {
            ssize_t w = write(fd, (char *)buf + off, want - off);
            if (w <= 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += (size_t)w;
        }
    }
    close(fd);
    if (wait_ok(pid) != 0) rc = -1;
    signal(SIGPIPE, old);
    return rc;
}

static const char *extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

sound sound_load(const char *path)
{
    sound s = load_wav(path);
    if (!s.frames && strcasecmp(extension(path), "wav") != 0)
        s = load_ffmpeg(path);
    return s;
}

int sound_save(const sound *s, const char *path)
{
    const char *ext = extension(path);
    if (!strcasecmp(ext, "wav")) return save_wav24(s, path);
    return save_ffmpeg(s, path, ext);
}
