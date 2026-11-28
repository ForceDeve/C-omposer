/*
 * settings.h — the user's preferences, kept in ~/.config/c-omposer/settings.ini.
 */
#pragma once

#include <glib.h>

typedef enum { VIS_OFF, VIS_BARS, VIS_SCOPE, VIS_RADIAL, VIS_PARTICLES, VIS_COUNT } VisualizerKind;

typedef struct {
    char          *theme;          /* theme id ("c-omposer-dark", "dracula", ...) */
    char          *project_dir;    /* last project folder, or NULL */
    gboolean       sidebar;        /* projects sidebar shown */
    /* interpreter-mode background */
    char          *bg_image;       /* picture behind the code, or NULL */
    int            bg_dim;         /* 0..95: how much the editor colour covers it */
    VisualizerKind visualizer;     /* audio visualizer behind the code (off by default) */
    int            vis_intensity;  /* 10..100 */
} Settings;

Settings   *settings_get(void);    /* loaded on first use */
void        settings_save(void);
char       *settings_dir(void);    /* ~/.config/c-omposer (new string) */
const char *visualizer_name(VisualizerKind k);
