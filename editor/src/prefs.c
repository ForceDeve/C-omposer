/*
 * prefs.c — see prefs.h.
 */
#include "prefs.h"
#include "settings.h"
#include "theme.h"

#include <glib/gstdio.h>

typedef struct {
    GtkWidget   *window;
    GPtrArray   *themes;        /* ThemeInfo* in dropdown order */
    GtkWidget   *picture_label;
    PrefsChanged changed;
    gpointer     data;
} Prefs;

static Prefs *open_prefs;

static void notify(Prefs *p)
{
    settings_save();
    if (p->changed) p->changed(p->data);
}

static void on_theme(GObject *dd, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Prefs *p = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= p->themes->len) return;
    ThemeInfo *info = p->themes->pdata[i];
    if (theme_apply(info->id)) {
        Settings *s = settings_get();
        g_free(s->theme);
        s->theme = g_strdup(info->id);
        settings_save();
    }
}

static void on_open_themes(GtkButton *b, gpointer data)
{
    (void)b;
    Prefs *p = data;
    char *dir = theme_user_dir();
    g_mkdir_with_parents(dir, 0755);
    char *readme = g_build_filename(dir, "README.txt", NULL);
    if (!g_file_test(readme, G_FILE_TEST_EXISTS))
        g_file_set_contents(readme,
            "Put your own themes here: copy one of the .ini files from C-omposer's\n"
            "editor/themes folder, rename it and change any colour. Every key is\n"
            "optional. FHI theme files work too. Reopen Preferences to see new ones.\n", -1, NULL);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_show_uri(GTK_WINDOW(p->window), uri, GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_free(uri); g_free(readme); g_free(dir);
}

static void show_picture_name(Prefs *p)
{
    const char *img = settings_get()->bg_image;
    char *base = img ? g_path_get_basename(img) : NULL;
    gtk_label_set_text(GTK_LABEL(p->picture_label), base ? base : "none");
    g_free(base);
}

static void set_picture(Prefs *p, const char *path)
{
    Settings *s = settings_get();
    g_free(s->bg_image);
    s->bg_image = path ? g_strdup(path) : NULL;
    show_picture_name(p);
    notify(p);
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void picture_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    char *path = g_file_get_path(f);
    set_picture(data, path);
    g_free(path);
    g_object_unref(f);
}
static void on_choose_picture(GtkButton *b, gpointer data)
{
    (void)b;
    Prefs *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Background picture");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Pictures");
    gtk_file_filter_add_mime_type(f, "image/*");
    GListStore *fl = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(fl, f);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(fl));
    g_object_unref(fl); g_object_unref(f);
    gtk_file_dialog_open(d, GTK_WINDOW(p->window), NULL, picture_done, p);
    g_object_unref(d);
}
#else
static void picture_response(GtkNativeDialog *d, int response, gpointer data)
{
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
        if (f) {
            char *path = g_file_get_path(f);
            set_picture(data, path);
            g_free(path);
            g_object_unref(f);
        }
    }
    g_object_unref(d);
}
static void on_choose_picture(GtkButton *b, gpointer data)
{
    (void)b;
    Prefs *p = data;
    GtkFileChooserNative *d = gtk_file_chooser_native_new("Background picture", GTK_WINDOW(p->window),
                                                          GTK_FILE_CHOOSER_ACTION_OPEN, "_Choose", "_Cancel");
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Pictures");
    gtk_file_filter_add_mime_type(f, "image/*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    g_object_unref(f);
    g_signal_connect(d, "response", G_CALLBACK(picture_response), p);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(d));
}
#endif

static void on_clear_picture(GtkButton *b, gpointer data) { (void)b; set_picture(data, NULL); }

static void on_dim(GtkRange *r, gpointer data)
{
    settings_get()->bg_dim = (int)gtk_range_get_value(r);
    notify(data);
}

static void on_visualizer(GObject *dd, GParamSpec *ps, gpointer data)
{
    (void)ps;
    settings_get()->visualizer = (VisualizerKind)gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    notify(data);
}

static void on_intensity(GtkRange *r, gpointer data)
{
    settings_get()->vis_intensity = (int)gtk_range_get_value(r);
    notify(data);
}

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "heading");
    return l;
}

static GtkWidget *row(const char *label, GtkWidget *w)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_size_request(l, 110, -1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_box_append(GTK_BOX(b), l);
    gtk_box_append(GTK_BOX(b), w);
    return b;
}

static GtkWidget *note(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "dim-label");
    return l;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Prefs *p = data;
    g_ptr_array_unref(p->themes);
    if (open_prefs == p) open_prefs = NULL;
    g_free(p);
}

void prefs_show(GtkWindow *parent, PrefsChanged changed, gpointer data)
{
    if (open_prefs) { gtk_window_present(GTK_WINDOW(open_prefs->window)); return; }
    Prefs *p = g_new0(Prefs, 1);
    open_prefs = p;
    p->changed = changed;
    p->data = data;
    Settings *s = settings_get();

    p->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p->window), "Preferences — C-omposer");
    gtk_window_set_transient_for(GTK_WINDOW(p->window), parent);
    gtk_window_set_default_size(GTK_WINDOW(p->window), 460, -1);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_destroy), p);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "composer-prefs");

    /* appearance */
    gtk_box_append(GTK_BOX(box), heading("Appearance"));
    p->themes = theme_list();
    GtkStringList *names = gtk_string_list_new(NULL);
    guint current = 0;
    for (guint i = 0; i < p->themes->len; i++) {
        ThemeInfo *info = p->themes->pdata[i];
        char *label = info->user ? g_strdup_printf("%s (yours)", info->name) : g_strdup(info->name);
        gtk_string_list_append(names, label);
        g_free(label);
        if (!g_strcmp0(info->id, theme_current_id())) current = i;
    }
    GtkWidget *themes = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(themes), current);
    g_signal_connect(themes, "notify::selected", G_CALLBACK(on_theme), p);
    gtk_box_append(GTK_BOX(box), row("Theme", themes));
    GtkWidget *open_themes = gtk_button_new_with_label("Open my themes folder");
    g_signal_connect(open_themes, "clicked", G_CALLBACK(on_open_themes), p);
    gtk_box_append(GTK_BOX(box), row("", open_themes));
    gtk_box_append(GTK_BOX(box), note("Themes are .ini files, the same format as FHI's. Yours go in "
                                      "~/.config/c-omposer/themes and win over the built-in ones."));

    /* interpreter background */
    gtk_box_append(GTK_BOX(box), heading("Interpreter mode background"));
    GtkWidget *pic = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->picture_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->picture_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->picture_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(p->picture_label, TRUE);
    GtkWidget *choose = gtk_button_new_with_label("Choose…");
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, "No picture");
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_picture), p);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_picture), p);
    gtk_box_append(GTK_BOX(pic), p->picture_label);
    gtk_box_append(GTK_BOX(pic), choose);
    gtk_box_append(GTK_BOX(pic), clear);
    show_picture_name(p);
    gtk_box_append(GTK_BOX(box), row("Picture", pic));
    GtkWidget *dim = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 95, 1);
    gtk_range_set_value(GTK_RANGE(dim), s->bg_dim);
    g_signal_connect(dim, "value-changed", G_CALLBACK(on_dim), p);
    gtk_box_append(GTK_BOX(box), row("Dim", dim));

    /* visualizer */
    gtk_box_append(GTK_BOX(box), heading("Audio visualizer"));
    GtkStringList *kinds = gtk_string_list_new(NULL);
    for (int k = 0; k < VIS_COUNT; k++) gtk_string_list_append(kinds, visualizer_name(k));
    GtkWidget *vis = gtk_drop_down_new(G_LIST_MODEL(kinds), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(vis), s->visualizer);
    g_signal_connect(vis, "notify::selected", G_CALLBACK(on_visualizer), p);
    gtk_box_append(GTK_BOX(box), row("Visualizer", vis));
    GtkWidget *inten = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100, 1);
    gtk_range_set_value(GTK_RANGE(inten), s->vis_intensity);
    g_signal_connect(inten, "value-changed", G_CALLBACK(on_intensity), p);
    gtk_box_append(GTK_BOX(box), row("Intensity", inten));
    gtk_box_append(GTK_BOX(box), note("Plays behind the code in Interpreter mode while something is "
                                      "playing. Off by default: when off, no audio is kept and nothing "
                                      "is animated, so it costs no memory or CPU."));

    gtk_window_set_child(GTK_WINDOW(p->window), box);
    gtk_window_present(GTK_WINDOW(p->window));
}
