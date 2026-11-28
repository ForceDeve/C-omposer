/*
 * sidebar.h — the project sidebar: a file tree of the project folder, where
 * songs, snippets of code and samples live.
 *
 *   double-click   open code / preview a sample
 *   right-click    open, play, insert into the code, show in the file manager
 *   drag           drop a sample into the code as load_sample("...")
 */
#pragma once

#include <gtk/gtk.h>

typedef struct Sidebar Sidebar;

typedef struct {
    void (*open)(GFile *file, gpointer data);          /* a code/text file   */
    void (*play)(GFile *file, gpointer data);          /* an audio file      */
    void (*insert)(const char *text, gpointer data);   /* at the cursor      */
    void (*project_changed)(const char *dir, gpointer data);
    void (*new_project)(gpointer data);                /* "New project…"     */
    void (*open_project)(gpointer data);               /* "Open folder…"     */
    gpointer data;
} SidebarCallbacks;

Sidebar    *sidebar_new(const SidebarCallbacks *cb);
GtkWidget  *sidebar_widget(Sidebar *s);
void        sidebar_set_project(Sidebar *s, const char *dir);   /* NULL = none */
const char *sidebar_project(Sidebar *s);

/* What load_sample("...") should say for a file: relative to the project. */
char *sidebar_relative_path(Sidebar *s, GFile *file);

gboolean sidebar_is_audio(const char *name);
gboolean sidebar_is_code(const char *name);
