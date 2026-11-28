/*
 * prefs.h — the Preferences window: theme, Interpreter-mode background
 * picture and audio visualizer. Changes apply immediately and are saved.
 */
#pragma once

#include <gtk/gtk.h>

typedef void (*PrefsChanged)(gpointer data);    /* background/visualizer settings changed */

void prefs_show(GtkWindow *parent, PrefsChanged changed, gpointer data);
