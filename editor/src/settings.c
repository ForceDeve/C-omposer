/*
 * settings.c — see settings.h.
 */
#include "settings.h"

#include <glib/gstdio.h>

static Settings *the_settings;

char *settings_dir(void) { return g_build_filename(g_get_user_config_dir(), "c-omposer", NULL); }

static char *settings_path(void)
{
    char *dir = settings_dir();
    char *p = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return p;
}

const char *visualizer_name(VisualizerKind k)
{
    static const char *names[VIS_COUNT] = { "Off", "Spectrum bars", "Oscilloscope", "Radial spectrum", "Particles" };
    return k < VIS_COUNT ? names[k] : names[0];
}

static char *get_str(GKeyFile *kf, const char *key, const char *def)
{
    char *v = g_key_file_get_string(kf, "c-omposer", key, NULL);
    if (v && !*v) { g_free(v); v = NULL; }
    return v ? v : g_strdup(def);
}

static int get_int(GKeyFile *kf, const char *key, int def, int lo, int hi)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, "c-omposer", key, &e);
    if (e) { g_error_free(e); return def; }
    return CLAMP(v, lo, hi);
}

Settings *settings_get(void)
{
    if (the_settings) return the_settings;
    the_settings = g_new0(Settings, 1);
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);   /* missing file: defaults */
    g_free(path);
    the_settings->theme = get_str(kf, "theme", "c-omposer-dark");
    the_settings->project_dir = get_str(kf, "project_dir", NULL);
    the_settings->sidebar = get_int(kf, "sidebar", 1, 0, 1);
    the_settings->bg_image = get_str(kf, "bg_image", NULL);
    the_settings->bg_dim = get_int(kf, "bg_dim", 80, 0, 95);
    the_settings->visualizer = get_int(kf, "visualizer", VIS_OFF, 0, VIS_COUNT - 1);
    the_settings->vis_intensity = get_int(kf, "vis_intensity", 60, 10, 100);
    g_key_file_unref(kf);
    return the_settings;
}

void settings_save(void)
{
    Settings *s = settings_get();
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "c-omposer", "theme", s->theme ? s->theme : "");
    g_key_file_set_string(kf, "c-omposer", "project_dir", s->project_dir ? s->project_dir : "");
    g_key_file_set_integer(kf, "c-omposer", "sidebar", s->sidebar);
    g_key_file_set_string(kf, "c-omposer", "bg_image", s->bg_image ? s->bg_image : "");
    g_key_file_set_integer(kf, "c-omposer", "bg_dim", s->bg_dim);
    g_key_file_set_integer(kf, "c-omposer", "visualizer", s->visualizer);
    g_key_file_set_integer(kf, "c-omposer", "vis_intensity", s->vis_intensity);
    char *dir = settings_dir();
    g_mkdir_with_parents(dir, 0755);
    char *path = settings_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path); g_free(dir);
    g_key_file_unref(kf);
}
