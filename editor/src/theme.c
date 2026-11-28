/*
 * theme.c — ★ COLOURS & FONTS ★  (see theme.h for the theme file format)
 *
 * The built-in defaults below are what a theme falls back to for anything it
 * doesn't set. Theme files live in editor/themes/ and ~/.config/c-omposer/themes/.
 */
#include "theme.h"
#include "settings.h"

#include <stdlib.h>
#include <string.h>

#ifndef COMPOSER_THEME_DIR
#define COMPOSER_THEME_DIR "themes"
#endif

/* ------------------------------------------------------------------ */
/*  Built-in defaults                                                  */
/* ------------------------------------------------------------------ */

typedef struct { const char *fg; gboolean bold, italic, underline; } DefStyle;

/*                                    dark          light        bold   italic underline */
static const struct { DefStyle dark, light; } default_styles[TOK_COUNT] = {
    [TOK_KEYWORD]       = { { "#c678dd", TRUE },          { "#a626a4", TRUE } },
    [TOK_MUSIC_KEYWORD] = { { "#ff79c6", TRUE },          { "#d6336c", TRUE } },
    [TOK_TYPE]          = { { "#e5c07b" },                { "#c18401" } },
    [TOK_MUSIC_TYPE]    = { { "#56d4c4", TRUE },          { "#0f8a7e", TRUE } },
    [TOK_USER_TYPE]     = { { "#f0a45d" },                { "#b76b01" } },
    [TOK_BUILTIN]       = { { "#4fc1ff" },                { "#0184bc" } },
    [TOK_FUNCTION]      = { { "#61afef" },                { "#4078f2" } },
    [TOK_CONSTANT]      = { { "#f78c6c" },                { "#d75f00" } },
    [TOK_NUMBER]        = { { "#d19a66" },                { "#986801" } },
    [TOK_UNIT]          = { { "#e6b673", FALSE, TRUE },   { "#9a6e00", FALSE, TRUE } },
    [TOK_NOTE]          = { { "#ff6e9c", TRUE },          { "#c0185e", TRUE } },
    [TOK_STRING]        = { { "#98c379" },                { "#50a14f" } },
    [TOK_CHAR]          = { { "#98c379" },                { "#50a14f" } },
    [TOK_COMMENT]       = { { "#5c6370", FALSE, TRUE },   { "#a0a1a7", FALSE, TRUE } },
    [TOK_PREPROCESSOR]  = { { "#e06c75" },                { "#e45649" } },
    [TOK_OPERATOR]      = { { "#89ddff" },                { "#0997b3" } },
    [TOK_PUNCTUATION]   = { { "#abb2bf" },                { "#696c77" } },
    [TOK_ERROR]         = { { "#ff5370", FALSE, FALSE, TRUE }, { "#e45649", FALSE, FALSE, TRUE } },
};

static const struct { const char *bg, *fg, *window_bg, *panel_bg, *accent, *caret; } default_chrome[2] = {
    /* dark  */ { "#1b1d23", "#d4d7de", "#21252b", "#1b1d23", "#528bff", "#528bff" },
    /* light */ { "#fafafa", "#383a42", "#eaeaeb", "#f3f3f3", "#4078f2", "#526fff" },
};

#define DEFAULT_FONT "JetBrains Mono, Fira Code, Source Code Pro, DejaVu Sans Mono, monospace"

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static EditorTheme  cur;
static TokenStyle   styles[TOK_COUNT];
static char        *cur_id;
static gboolean     loaded;
static GtkCssProvider *provider;

typedef struct { GtkTextBuffer *buffer; GtkTextTag **tags; GtkTextTag *bracket; } TagSet;
static GPtrArray *tag_sets;                 /* TagSet*; removed when their buffer dies */

typedef struct { ThemeListener fn; gpointer data; } Listener;
static GArray *listeners;

/* ------------------------------------------------------------------ */
/*  Colour helpers                                                     */
/* ------------------------------------------------------------------ */

static void hex(const GdkRGBA *c, char out[THEME_COLOR])
{
    g_snprintf(out, THEME_COLOR, "#%02x%02x%02x",
               (int)(CLAMP(c->red, 0, 1) * 255 + 0.5), (int)(CLAMP(c->green, 0, 1) * 255 + 0.5),
               (int)(CLAMP(c->blue, 0, 1) * 255 + 0.5));
}

/* a + (b - a) * t, written to out */
static void mix(const char *a, const char *b, double t, char out[THEME_COLOR])
{
    GdkRGBA x, y;
    if (!gdk_rgba_parse(&x, a) || !gdk_rgba_parse(&y, b)) { g_strlcpy(out, a, THEME_COLOR); return; }
    GdkRGBA m = { x.red + (y.red - x.red) * t, x.green + (y.green - x.green) * t,
                  x.blue + (y.blue - x.blue) * t, 1 };
    hex(&m, out);
}

static double luminance(const char *c)
{
    GdkRGBA x;
    if (!gdk_rgba_parse(&x, c)) return 0;
    return 0.2126 * x.red + 0.7152 * x.green + 0.0722 * x.blue;
}

static const char *contrast(const char *bg) { return luminance(bg) > 0.55 ? "#1b1b1b" : "#ffffff"; }

/* ------------------------------------------------------------------ */
/*  Loading                                                            */
/* ------------------------------------------------------------------ */

/* A value without its trailing "# comment" (FHI theme files have those). */
static char *read_value(GKeyFile *kf, const char *key)
{
    char *v = g_key_file_get_string(kf, "theme", key, NULL);
    if (!v) return NULL;
    for (char *p = v; *p; p++)
        if ((*p == ' ' || *p == '\t') && strchr(p, '#') && *(p + strspn(p, " \t")) == '#') { *p = '\0'; break; }
    g_strstrip(v);
    return v;
}

static gboolean read_color(GKeyFile *kf, const char *key, char out[THEME_COLOR])
{
    char *v = read_value(kf, key);
    if (!v) return FALSE;
    GdkRGBA c;
    gboolean ok = gdk_rgba_parse(&c, v);
    if (ok) hex(&c, out);
    g_free(v);
    return ok;
}

/* Fill cur/styles from a key file (NULL = just the defaults). */
static void build(GKeyFile *kf)
{
    char *scheme = kf ? read_value(kf, "scheme") : NULL;
    cur.scheme = !scheme ? 2 : !strcmp(scheme, "light") ? 1 : !strcmp(scheme, "system") ? 0 : 2;
    g_free(scheme);

    char wbg[THEME_COLOR] = "", wfg[THEME_COLOR] = "", pbg[THEME_COLOR] = "", acc[THEME_COLOR] = "";
    char ebg[THEME_COLOR] = "", efg[THEME_COLOR] = "";
    if (kf) {
        read_color(kf, "window_bg", wbg);
        read_color(kf, "window_fg", wfg);
        read_color(kf, "panel_bg", pbg);
        read_color(kf, "accent", acc);
        read_color(kf, "editor_bg", ebg);
        read_color(kf, "editor_fg", efg);
    }
    /* "system" with a window colour: the colour decides light or dark */
    gboolean light = cur.scheme == 1 || (cur.scheme == 0 && *wbg && luminance(wbg) > 0.5);
    const typeof(default_chrome[0]) *d = &default_chrome[light ? 1 : 0];

    g_strlcpy(cur.window_bg, *wbg ? wbg : d->window_bg, THEME_COLOR);
    g_strlcpy(cur.window_fg, *wfg ? wfg : *efg ? efg : d->fg, THEME_COLOR);
    g_strlcpy(cur.accent, *acc ? acc : d->accent, THEME_COLOR);
    g_strlcpy(cur.background, *ebg ? ebg : *pbg ? pbg : *wbg ? wbg : d->bg, THEME_COLOR);
    g_strlcpy(cur.foreground, *efg ? efg : cur.window_fg, THEME_COLOR);
    if (*pbg) g_strlcpy(cur.panel_bg, pbg, THEME_COLOR);
    else mix(cur.window_bg, cur.foreground, 0.04, cur.panel_bg);

    const char *bg = cur.background, *fg = cur.foreground;
    if (!kf || !read_color(kf, "caret", cur.caret))
        if (!kf || !read_color(kf, "cursor", cur.caret)) g_strlcpy(cur.caret, *acc ? acc : d->caret, THEME_COLOR);
    if (!kf || !read_color(kf, "selection", cur.selection)) mix(bg, cur.accent, 0.30, cur.selection);
    g_strlcpy(cur.gutter_background, bg, THEME_COLOR);
    if (!kf || !read_color(kf, "gutter_fg", cur.gutter_foreground)) mix(bg, fg, 0.35, cur.gutter_foreground);
    if (!kf || !read_color(kf, "gutter_current", cur.gutter_current)) mix(bg, fg, 0.85, cur.gutter_current);
    if (!kf || !read_color(kf, "bracket", cur.bracket_match)) mix(bg, cur.accent, 0.25, cur.bracket_match);
    mix(bg, fg, 0.04, cur.popup_background);
    mix(bg, fg, 0.18, cur.popup_border);
    mix(bg, cur.accent, 0.22, cur.popup_selected);
    mix(bg, fg, 0.55, cur.popup_detail);

    char *font = kf ? read_value(kf, "font") : NULL;
    g_snprintf(cur.font_family, sizeof cur.font_family, "%s%s%s",
               font && *font ? font : "", font && *font ? ", " : "", DEFAULT_FONT);
    g_free(font);
    char *size = kf ? read_value(kf, "font_size") : NULL;
    int sz = size ? atoi(size) : 0;
    cur.font_size_pt = sz >= 6 && sz <= 40 ? sz : 11;
    char *radius = kf ? read_value(kf, "radius") : NULL;
    cur.radius = radius ? CLAMP(atoi(radius), 0, 24) : 6;
    g_free(size); g_free(radius);

    for (int c = 0; c < TOK_COUNT; c++) {
        const DefStyle *ds = light ? &default_styles[c].light : &default_styles[c].dark;
        TokenStyle *st = &styles[c];
        memset(st, 0, sizeof *st);
        st->bold = ds->bold; st->italic = ds->italic; st->underline = ds->underline;
        char key[40];
        g_strlcpy(key, token_category_name(c), sizeof key);
        for (char *k = key; *k; k++) if (*k == '-') *k = '_';
        if (!kf || !read_color(kf, key, st->foreground))
            if (ds->fg) g_strlcpy(st->foreground, ds->fg, THEME_COLOR);
    }
}

static void ensure_loaded(void)
{
    if (loaded) return;
    loaded = TRUE;
    build(NULL);
    if (!theme_apply(settings_get()->theme))
        theme_apply("c-omposer-dark");
}

const EditorTheme *theme_get(void) { ensure_loaded(); return &cur; }

const TokenStyle *theme_token_style(TokenCategory category)
{
    ensure_loaded();
    return &styles[(category >= 0 && category < TOK_COUNT) ? category : TOK_PLAIN];
}

const char *theme_current_id(void) { ensure_loaded(); return cur_id ? cur_id : "c-omposer-dark"; }

/* ------------------------------------------------------------------ */
/*  Finding theme files                                                */
/* ------------------------------------------------------------------ */

char *theme_user_dir(void)
{
    char *dir = settings_dir();
    char *t = g_build_filename(dir, "themes", NULL);
    g_free(dir);
    return t;
}

static void info_free(gpointer p)
{
    ThemeInfo *i = p;
    g_free(i->id); g_free(i->name); g_free(i->path);
    g_free(i);
}

static void scan(GPtrArray *out, const char *dir, gboolean user)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *f;
    while ((f = g_dir_read_name(d))) {
        if (!g_str_has_suffix(f, ".ini")) continue;
        char *id = g_strndup(f, strlen(f) - 4);
        gboolean dup = FALSE;
        for (guint i = 0; i < out->len; i++)
            if (!strcmp(((ThemeInfo *)out->pdata[i])->id, id)) dup = TRUE;   /* user file already listed */
        if (dup) { g_free(id); continue; }
        ThemeInfo *info = g_new0(ThemeInfo, 1);
        info->id = id;
        info->path = g_build_filename(dir, f, NULL);
        info->user = user;
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, info->path, G_KEY_FILE_NONE, NULL))
            info->name = g_key_file_get_string(kf, "theme", "name", NULL);
        g_key_file_unref(kf);
        if (info->name) g_strstrip(info->name);
        if (!info->name || !*info->name) { g_free(info->name); info->name = g_strdup(id); }
        g_ptr_array_add(out, info);
    }
    g_dir_close(d);
}

static int by_name(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate((*(ThemeInfo *const *)a)->name, (*(ThemeInfo *const *)b)->name);
}

GPtrArray *theme_list(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(info_free);
    char *user = theme_user_dir();
    scan(out, user, TRUE);          /* first, so a user file hides a bundled one */
    scan(out, COMPOSER_THEME_DIR, FALSE);
    g_free(user);
    g_ptr_array_sort(out, by_name);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Applying                                                           */
/* ------------------------------------------------------------------ */

static void style_tag(GtkTextTag *tag, const TokenStyle *st)
{
    if (*st->foreground) g_object_set(tag, "foreground", st->foreground, NULL);
    else                 g_object_set(tag, "foreground-set", FALSE, NULL);
    if (*st->background) g_object_set(tag, "background", st->background, NULL);
    else                 g_object_set(tag, "background-set", FALSE, NULL);
    g_object_set(tag, "weight", st->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                 "style", st->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
                 "underline", st->underline ? PANGO_UNDERLINE_ERROR : PANGO_UNDERLINE_NONE, NULL);
}

static void restyle_tags(void)
{
    if (!tag_sets) return;
    for (guint i = 0; i < tag_sets->len; i++) {
        TagSet *ts = tag_sets->pdata[i];
        if (ts->tags)
            for (int c = 0; c < TOK_COUNT; c++) if (ts->tags[c]) style_tag(ts->tags[c], &styles[c]);
        if (ts->bracket) g_object_set(ts->bracket, "background", cur.bracket_match, NULL);
    }
}

gboolean theme_apply(const char *id)
{
    if (!id) return FALSE;
    loaded = TRUE;
    GKeyFile *kf = NULL;
    GPtrArray *list = theme_list();
    for (guint i = 0; i < list->len && !kf; i++) {
        ThemeInfo *info = list->pdata[i];
        if (strcmp(info->id, id)) continue;
        kf = g_key_file_new();
        if (!g_key_file_load_from_file(kf, info->path, G_KEY_FILE_NONE, NULL)) {
            g_key_file_unref(kf);
            kf = NULL;
        }
    }
    g_ptr_array_unref(list);
    if (!kf && strcmp(id, "c-omposer-dark")) return FALSE;   /* the built-in one needs no file */
    build(kf);
    if (kf) g_key_file_unref(kf);
    g_free(cur_id);
    cur_id = g_strdup(id);

    if (gdk_display_get_default()) {
        if (cur.scheme)
            g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", cur.scheme == 2, NULL);
        theme_install_css();
    }
    restyle_tags();
    if (listeners)
        for (guint i = 0; i < listeners->len; i++) {
            Listener *l = &g_array_index(listeners, Listener, i);
            l->fn(l->data);
        }
    return TRUE;
}

void theme_add_listener(ThemeListener fn, gpointer data)
{
    if (!listeners) listeners = g_array_new(FALSE, FALSE, sizeof(Listener));
    Listener l = { fn, data };
    g_array_append_val(listeners, l);
}

void theme_remove_listener(ThemeListener fn, gpointer data)
{
    if (!listeners) return;
    for (guint i = 0; i < listeners->len; i++) {
        Listener *l = &g_array_index(listeners, Listener, i);
        if (l->fn == fn && l->data == data) { g_array_remove_index(listeners, i); return; }
    }
}

/* ------------------------------------------------------------------ */
/*  Tags                                                               */
/* ------------------------------------------------------------------ */

static void buffer_gone(gpointer data, GObject *dead)
{
    (void)data;
    for (guint i = 0; tag_sets && i < tag_sets->len; i++)
        if ((GObject *)((TagSet *)tag_sets->pdata[i])->buffer == dead)
            g_ptr_array_remove_index(tag_sets, i--);
}

static TagSet *tag_set_for(GtkTextBuffer *buffer)
{
    if (!tag_sets) tag_sets = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < tag_sets->len; i++)
        if (((TagSet *)tag_sets->pdata[i])->buffer == buffer) return tag_sets->pdata[i];
    TagSet *ts = g_new0(TagSet, 1);
    ts->buffer = buffer;
    g_object_weak_ref(G_OBJECT(buffer), buffer_gone, NULL);
    g_ptr_array_add(tag_sets, ts);
    return ts;
}

void theme_create_tags(GtkTextBuffer *buffer, GtkTextTag *tags[TOK_COUNT])
{
    ensure_loaded();
    for (int c = 0; c < TOK_COUNT; c++) {
        tags[c] = gtk_text_buffer_create_tag(buffer, token_category_name(c), NULL);
        style_tag(tags[c], &styles[c]);
    }
    tag_set_for(buffer)->tags = tags;
}

GtkTextTag *theme_create_bracket_tag(GtkTextBuffer *buffer)
{
    ensure_loaded();
    GtkTextTag *t = gtk_text_buffer_create_tag(buffer, "bracket-match", "background", cur.bracket_match,
                                               "weight", PANGO_WEIGHT_BOLD, NULL);
    tag_set_for(buffer)->bracket = t;
    return t;
}

/* ------------------------------------------------------------------ */
/*  CSS                                                                */
/* ------------------------------------------------------------------ */

/* GTK 4 themes use either the old (@theme_*) or the newer (@window_bg_color)
 * names for the same colours; defining both covers any theme (as in FHI). */
static void define(GString *s, const char *names, const char *value)
{
    char **n = g_strsplit(names, " ", -1);
    for (char **x = n; *x; x++) g_string_append_printf(s, "@define-color %s %s;\n", *x, value);
    g_strfreev(n);
}

void theme_install_css(void)
{
    ensure_loaded();
    const EditorTheme *t = &cur;
    GString *css = g_string_new(NULL);
    char btn[THEME_COLOR], btn_hover[THEME_COLOR], border[THEME_COLOR], muted[THEME_COLOR];
    mix(t->window_bg, t->window_fg, 0.08, btn);
    mix(t->window_bg, t->window_fg, 0.14, btn_hover);
    mix(t->window_bg, t->window_fg, 0.16, border);
    mix(t->window_bg, t->window_fg, 0.55, muted);
    const char *acc_fg = contrast(t->accent);

    define(css, "theme_bg_color window_bg_color headerbar_bg_color popover_bg_color dialog_bg_color sidebar_bg_color", t->window_bg);
    define(css, "theme_fg_color window_fg_color headerbar_fg_color popover_fg_color dialog_fg_color sidebar_fg_color", t->window_fg);
    define(css, "theme_base_color view_bg_color card_bg_color", t->panel_bg);
    define(css, "theme_text_color view_fg_color card_fg_color", t->window_fg);
    define(css, "accent_bg_color accent_color theme_selected_bg_color", t->accent);
    define(css, "accent_fg_color theme_selected_fg_color", acc_fg);

    g_string_append_printf(css,
        "window.background, dialog { background-color: %s; color: %s; }\n"
        "headerbar { background-image: none; background-color: %s; color: %s; border-bottom: 1px solid %s; box-shadow: none; }\n"
        "popover > contents, popover > arrow { background-color: %s; color: %s; border-color: %s; }\n"
        "button { background-image: none; background-color: %s; color: %s; border-color: %s; box-shadow: none; text-shadow: none; }\n"
        "button:hover { background-color: %s; }\n"
        "button:checked, button.suggested-action { background-color: %s; color: %s; }\n"
        "button.flat { background-color: transparent; }\n"
        "button.flat:hover { background-color: %s; }\n"
        "entry, spinbutton, dropdown > button { background-color: %s; color: %s; border-color: %s; }\n"
        "scale trough { background-color: %s; } scale highlight { background-color: %s; }\n"
        "switch:checked { background-color: %s; }\n"
        "selection, *:selected, row:selected { background-color: alpha(%s, 0.35); color: %s; }\n"
        "paned > separator { background-color: %s; }\n"
        "button, entry, dropdown > button, popover > contents, .composer-viz-card { border-radius: %dpx; }\n",
        t->window_bg, t->window_fg,
        t->window_bg, t->window_fg, border,
        t->window_bg, t->window_fg, border,
        btn, t->window_fg, border,
        btn_hover,
        t->accent, acc_fg,
        btn,
        t->panel_bg, t->window_fg, border,
        btn, t->accent,
        t->accent,
        t->accent, t->window_fg,
        border,
        t->radius);

    g_string_append_printf(css,
        "textview.composer-editor { font-family: %s; font-size: %dpt; background-color: %s; }\n"
        "textview.composer-editor text { background-color: %s; color: %s; caret-color: %s; }\n"
        "textview.composer-editor text selection { background-color: %s; }\n"
        ".composer-gutter { background-color: %s; }\n"
        ".completion, .composer-sighelp { background-color: %s; border: 1px solid %s; border-radius: %dpx; padding: 2px; }\n"
        ".completion list { background-color: transparent; }\n"
        ".completion row { padding: 1px 6px; border-radius: 4px; }\n"
        ".completion row:selected { background-color: %s; }\n"
        ".completion .kind { font-weight: bold; min-width: 22px; }\n"
        ".completion .name { color: %s; }\n"
        ".completion .detail { color: %s; font-size: 0.9em; }\n"
        ".completion .doc { color: %s; padding: 6px 8px 4px 8px; border-top: 1px solid %s; }\n"
        ".composer-sighelp label { color: %s; padding: 3px 8px; font-family: %s; font-size: %dpt; }\n"
        ".composer-status { padding: 2px 10px; font-size: 0.9em; color: %s; background-color: %s; }\n"
        ".composer-output, .composer-output text { background-color: %s; color: %s; }\n"
        ".composer-output-title, .composer-sidebar-title { padding: 3px 10px; font-size: 0.85em; font-weight: bold; color: %s; background-color: %s; }\n"
        ".composer-sidebar, .composer-sidebar listview { background-color: %s; color: %s; }\n"
        ".composer-sidebar row { padding: 2px 4px; }\n"
        ".composer-sidebar .dim-label { color: %s; }\n"
        ".composer-prefs { padding: 16px; }\n"
        "button.composer-mode-on { background-color: alpha(%s, 0.35); }\n"
        ".composer-prefs .heading { font-weight: bold; margin-top: 8px; }\n",
        t->font_family, t->font_size_pt, t->background,
        t->background, t->foreground, t->caret,
        t->selection,
        t->gutter_background,
        t->popup_background, t->popup_border, t->radius,
        t->popup_selected,
        t->foreground,
        t->popup_detail,
        t->popup_detail, t->popup_border,
        t->foreground, t->font_family, MAX(t->font_size_pt - 1, 8),
        t->popup_detail, t->window_bg,
        t->panel_bg, t->window_fg,
        muted, t->window_bg,
        t->panel_bg, t->window_fg,
        muted,
        t->accent);

    for (int c = 0; c < TOK_COUNT; c++)
        g_string_append_printf(css, ".completion .kind-%s { color: %s; }\n",
                               token_category_name(c), *styles[c].foreground ? styles[c].foreground : t->foreground);
    g_string_append_printf(css, ".completion .kind-member { color: %s; }\n", styles[TOK_STRING].foreground);

    GdkDisplay *display = gdk_display_get_default();
    if (provider) gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(provider));
    else provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css->str);
#else
    gtk_css_provider_load_from_data(provider, css->str, -1);
#endif
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_string_free(css, TRUE);
}
