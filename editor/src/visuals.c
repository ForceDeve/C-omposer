/*
 * visuals.c — see visuals.h.
 */
#include "visuals.h"
#include "theme.h"

#include <math.h>
#include <string.h>

#define FFT_N      2048
#define BANDS      64
#define MAX_PARTS  320

typedef struct { float x, y, vx, vy, life, size, hue; } Particle;

struct Visuals {
    GtkWidget     *view;
    gboolean       active;           /* Interpreter mode */
    /* picture */
    char          *image_path;
    GdkTexture    *texture;
    int            dim;
    /* visualizer */
    VisualizerKind kind;
    float          intensity;        /* 0.1 .. 1 */
    float         *mono;             /* only while a visualizer is on */
    guint          frames, rate;
    gint64         play_start;       /* 0 = not playing */
    guint          tick;
    float          bands[BANDS];     /* smoothed 0..1 */
    float          scope[512];
    float          bass;
    Particle       parts[MAX_PARTS];
    int            n_parts;
    guint32        seed;
    gint64         last_frame;
};

Visuals *visuals_new(GtkWidget *view)
{
    Visuals *v = g_new0(Visuals, 1);
    v->view = view;
    v->seed = 0x1234567;
    return v;
}

static void stop_tick(Visuals *v)
{
    if (v->tick) { gtk_widget_remove_tick_callback(v->view, v->tick); v->tick = 0; }
}

void visuals_free(Visuals *v)
{
    if (!v) return;
    stop_tick(v);
    g_clear_object(&v->texture);
    g_free(v->image_path);
    g_free(v->mono);
    g_free(v);
}

void visuals_configure(Visuals *v, const char *image, int dim, VisualizerKind kind, int intensity)
{
    if (g_strcmp0(image, v->image_path)) {
        g_free(v->image_path);
        v->image_path = image && *image ? g_strdup(image) : NULL;
        g_clear_object(&v->texture);
        if (v->image_path) {
            GError *e = NULL;
            v->texture = gdk_texture_new_from_filename(v->image_path, &e);
            if (!v->texture) {
                g_warning("background picture: %s", e->message);
                g_error_free(e);
            }
        }
    }
    v->dim = CLAMP(dim, 0, 95);
    v->kind = kind < VIS_COUNT ? kind : VIS_OFF;
    v->intensity = CLAMP(intensity, 10, 100) / 100.0f;
    if (v->kind == VIS_OFF) {                 /* give everything back */
        g_clear_pointer(&v->mono, g_free);
        v->frames = 0;
        v->n_parts = 0;
        memset(v->bands, 0, sizeof v->bands);
        stop_tick(v);
    }
    gtk_widget_queue_draw(v->view);
}

void visuals_set_active(Visuals *v, gboolean active)
{
    v->active = active;
    if (!active) stop_tick(v);
    gtk_widget_queue_draw(v->view);
}

gboolean visuals_wants_audio(Visuals *v) { return v->active && v->kind != VIS_OFF; }

void visuals_set_audio(Visuals *v, const float *l, const float *r, guint frames, guint rate)
{
    g_clear_pointer(&v->mono, g_free);
    v->frames = 0;
    if (!visuals_wants_audio(v) || !frames) return;
    v->mono = g_new(float, frames);
    for (guint i = 0; i < frames; i++) v->mono[i] = (l[i] + r[i]) * 0.5f;
    v->frames = frames;
    v->rate = rate;
}

/* ------------------------------------------------------------------ */
/*  Analysis                                                           */
/* ------------------------------------------------------------------ */

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {                  /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * G_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static float frand(Visuals *v)
{
    v->seed = v->seed * 1664525u + 1013904223u;
    return (v->seed >> 8) / 16777216.0f;
}

/* Advance the analysis to "now"; returns FALSE when there's nothing left to animate. */
static gboolean analyse(Visuals *v)
{
    gint64 now = g_get_monotonic_time();
    float dt = v->last_frame ? (now - v->last_frame) / 1e6f : 1 / 60.0f;
    v->last_frame = now;
    dt = CLAMP(dt, 0.001f, 0.1f);

    float target[BANDS] = { 0 };
    float bass = 0;
    gboolean playing = FALSE;
    if (v->play_start && v->mono) {
        double t = (now - v->play_start) / 1e6;
        long pos = (long)(t * v->rate);
        if (pos < (long)v->frames) {
            playing = TRUE;
            static float re[FFT_N], im[FFT_N];
            for (int i = 0; i < FFT_N; i++) {
                long p = pos - FFT_N / 2 + i;
                float x = p >= 0 && p < (long)v->frames ? v->mono[p] : 0;
                float w = 0.5f - 0.5f * cosf(2 * (float)G_PI * i / (FFT_N - 1));
                re[i] = x * w; im[i] = 0;
                if (i >= FFT_N / 2 - 256 && i < FFT_N / 2 + 256) v->scope[i - (FFT_N / 2 - 256)] = x;
            }
            fft(re, im, FFT_N);
            /* log-spaced bands, 30 Hz .. 16 kHz */
            for (int b = 0; b < BANDS; b++) {
                double f0 = 30 * pow(16000.0 / 30, (double)b / BANDS);
                double f1 = 30 * pow(16000.0 / 30, (double)(b + 1) / BANDS);
                int k0 = MAX(1, (int)(f0 * FFT_N / v->rate)), k1 = MAX(k0 + 1, (int)(f1 * FFT_N / v->rate));
                float m = 0;
                for (int k = k0; k < k1 && k < FFT_N / 2; k++) m = fmaxf(m, sqrtf(re[k] * re[k] + im[k] * im[k]));
                float db = 20 * log10f(m / (FFT_N / 4.0f) + 1e-9f);
                target[b] = CLAMP((db + 60) / 60, 0, 1);
            }
            for (int b = 0; b < 6; b++) bass += target[b] / 6;
        } else {
            v->play_start = 0;
        }
    }
    if (!playing) memset(v->scope, 0, sizeof v->scope);

    /* fast attack, slow release */
    float alive = 0;
    float fall = expf(-dt * 4.0f);
    for (int b = 0; b < BANDS; b++) {
        v->bands[b] = target[b] > v->bands[b] ? target[b] * 0.6f + v->bands[b] * 0.4f : v->bands[b] * fall;
        alive += v->bands[b];
    }
    float prev_bass = v->bass;
    v->bass = bass > v->bass ? bass : v->bass * fall;

    /* particles: bursts on bass hits */
    if (v->kind == VIS_PARTICLES) {
        if (playing && bass > 0.35f && bass > prev_bass * 1.08f) {
            int burst = (int)(12 + 40 * bass * v->intensity);
            for (int i = 0; i < burst && v->n_parts < MAX_PARTS; i++) {
                float a = frand(v) * 2 * (float)G_PI, sp = 0.15f + frand(v) * 0.5f * (0.5f + bass);
                v->parts[v->n_parts++] = (Particle){ 0.5f, 0.5f, cosf(a) * sp, sinf(a) * sp,
                                                     1.0f, 1.5f + frand(v) * 3.5f, frand(v) };
            }
        }
        for (int i = 0; i < v->n_parts; i++) {
            Particle *p = &v->parts[i];
            p->x += p->vx * dt; p->y += p->vy * dt;
            p->vx *= 1 - dt * 0.6f; p->vy *= 1 - dt * 0.6f;
            p->life -= dt * 0.55f;
            if (p->life <= 0 || p->x < -0.1f || p->x > 1.1f || p->y < -0.1f || p->y > 1.1f)
                v->parts[i--] = v->parts[--v->n_parts];
        }
        alive += v->n_parts;
    }
    return playing || alive > 0.01f;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    Visuals *v = data;
    gboolean more = analyse(v);
    gtk_widget_queue_draw(w);
    if (!more) { v->tick = 0; v->last_frame = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

void visuals_play_started(Visuals *v)
{
    if (!visuals_wants_audio(v) || !v->mono) return;
    v->play_start = g_get_monotonic_time();
    v->last_frame = 0;
    if (!v->tick) v->tick = gtk_widget_add_tick_callback(v->view, on_tick, v, NULL);
}

void visuals_play_stopped(Visuals *v)
{
    v->play_start = 0;            /* the tick keeps running until things settle */
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

static void color(const char *spec, double a, cairo_t *cr)
{
    GdkRGBA c;
    if (!gdk_rgba_parse(&c, spec)) gdk_rgba_parse(&c, "#528bff");
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, a);
}

void visuals_snapshot(Visuals *v, GtkSnapshot *snapshot, const graphene_rect_t *vis)
{
    if (!v->active) return;
    const EditorTheme *th = theme_get();

    if (v->texture) {
        /* cover the visible area, keep the aspect */
        float tw = gdk_texture_get_width(v->texture), tht = gdk_texture_get_height(v->texture);
        float scale = fmaxf(vis->size.width / tw, vis->size.height / tht);
        float w = tw * scale, h = tht * scale;
        graphene_rect_t r = GRAPHENE_RECT_INIT(vis->origin.x + (vis->size.width - w) / 2,
                                               vis->origin.y + (vis->size.height - h) / 2, w, h);
        gtk_snapshot_push_clip(snapshot, vis);
        gtk_snapshot_append_texture(snapshot, v->texture, &r);
        gtk_snapshot_pop(snapshot);
        GdkRGBA bg;
        gdk_rgba_parse(&bg, th->background);
        bg.alpha = v->dim / 100.0f;
        gtk_snapshot_append_color(snapshot, &bg, vis);
    }

    if (v->kind == VIS_OFF || !v->tick) return;
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, vis);
    cairo_translate(cr, vis->origin.x, vis->origin.y);
    double W = vis->size.width, H = vis->size.height, k = v->intensity;
    const char *c1 = th->accent, *c2 = theme_token_style(TOK_MUSIC_TYPE)->foreground;
    if (!*c2) c2 = th->accent;

    switch (v->kind) {
    case VIS_BARS: {
        double bw = W / BANDS;
        for (int b = 0; b < BANDS; b++) {
            double h = v->bands[b] * H * 0.55 * k;
            cairo_pattern_t *g = cairo_pattern_create_linear(0, H, 0, H - h);
            GdkRGBA a, bcol;
            gdk_rgba_parse(&a, c1); gdk_rgba_parse(&bcol, c2);
            cairo_pattern_add_color_stop_rgba(g, 0, a.red, a.green, a.blue, 0.30 * k + 0.05);
            cairo_pattern_add_color_stop_rgba(g, 1, bcol.red, bcol.green, bcol.blue, 0.05);
            cairo_set_source(cr, g);
            cairo_rectangle(cr, b * bw + 1, H - h, bw - 2, h);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
        }
        break;
    }
    case VIS_SCOPE: {
        for (int pass = 0; pass < 2; pass++) {
            cairo_set_line_width(cr, pass ? 1.5 : 7);
            color(pass ? c2 : c1, pass ? 0.45 * k + 0.1 : 0.10 * k, cr);
            for (int i = 0; i < 512; i++) {
                double x = W * i / 511.0, y = H / 2 - v->scope[i] * H * 0.35 * (0.4 + k);
                if (i) cairo_line_to(cr, x, y); else cairo_move_to(cr, x, y);
            }
            cairo_stroke(cr);
        }
        break;
    }
    case VIS_RADIAL: {
        double cx = W / 2, cy = H / 2, r0 = fmin(W, H) * (0.14 + 0.05 * v->bass);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, fmax(2, r0 * 0.05));
        for (int i = 0; i < BANDS * 2; i++) {
            int b = i < BANDS ? i : 2 * BANDS - 1 - i;          /* mirrored */
            double a = 2 * G_PI * i / (BANDS * 2) - G_PI / 2;
            double len = v->bands[b] * fmin(W, H) * 0.30 * k;
            color(i % 2 ? c1 : c2, 0.22 * k + 0.06, cr);
            cairo_move_to(cr, cx + cos(a) * r0, cy + sin(a) * r0);
            cairo_line_to(cr, cx + cos(a) * (r0 + len), cy + sin(a) * (r0 + len));
            cairo_stroke(cr);
        }
        color(c1, 0.10 * k + 0.05 * v->bass, cr);
        cairo_arc(cr, cx, cy, r0 * 0.92, 0, 2 * G_PI);
        cairo_fill(cr);
        break;
    }
    case VIS_PARTICLES: {
        for (int i = 0; i < v->n_parts; i++) {
            Particle *p = &v->parts[i];
            color(p->hue > 0.5f ? c1 : c2, p->life * (0.35 * k + 0.1), cr);
            cairo_arc(cr, p->x * W, p->y * H, p->size * (0.6 + v->bass), 0, 2 * G_PI);
            cairo_fill(cr);
        }
        color(c2, 0.08 * k * v->bass, cr);
        cairo_arc(cr, W / 2, H / 2, fmin(W, H) * 0.25 * v->bass, 0, 2 * G_PI);
        cairo_fill(cr);
        break;
    }
    default:
        break;
    }
    cairo_destroy(cr);
}
