/*
 * main.c — C-omposer application window: editor, header bar, open/save,
 * status bar.
 */
#include <gtk/gtk.h>
#include <string.h>

#include "editor.h"
#include "language.h"
#include "theme.h"

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *title;
    GtkWidget      *status_pos;
    GtkWidget      *status_msg;
    ComposerEditor *editor;
    GFile          *file;
    gboolean        close_after_save;
} ComposerWindow;

static const char *SAMPLE =
    "// C-omposer: a C-like language for writing music\n"
    "// Ctrl+Space = autocomplete, try typing \"verse.\" or \"progression[0].\"\n"
    "\n"
    "#define BARS 4\n"
    "\n"
    "typedef enum { INTRO, VERSE, CHORUS } part;\n"
    "\n"
    "typedef struct {\n"
    "    note  lead;\n"
    "    chord harmony;\n"
    "    tempo speed;\n"
    "} phrase;\n"
    "\n"
    "/* a I-vi-IV-V progression in C major */\n"
    "chord progression[BARS];\n"
    "track piano;\n"
    "\n"
    "void setup(void) {\n"
    "    set_tempo(96bpm);\n"
    "    piano.name = \"Grand piano\";\n"
    "    scale key = make_scale(C4, MAJOR);\n"
    "\n"
    "    progression[0] = make_chord(C4, MAJ);\n"
    "    progression[1] = make_chord(A3, MIN);\n"
    "    progression[2] = make_chord(F3, MAJ);\n"
    "    progression[3] = make_chord(G3, DOM7);\n"
    "}\n"
    "\n"
    "void play_verse(phrase verse) {\n"
    "    repeat (2) {\n"
    "        for (int i = 0; i < BARS; i++) {\n"
    "            parallel {\n"
    "                play_chord(progression[i], 4b);\n"
    "                play(transpose(verse.lead, i * 2), 500ms);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    play(Bb4, QUARTER);\n"
    "    wait(HALF);\n"
    "}\n";

/* ------------------------------------------------------------------ */

static void update_title(ComposerWindow *w)
{
    char *name = w->file ? g_file_get_basename(w->file)
                         : g_strdup_printf("untitled.%s", language_get()->file_extension);
    gboolean modified = gtk_text_buffer_get_modified(composer_editor_buffer(w->editor));
    char *title = g_strdup_printf("%s%s — C-omposer", modified ? "• " : "", name);
    gtk_label_set_text(GTK_LABEL(w->title), title);
    gtk_window_set_title(GTK_WINDOW(w->window), title);
    g_free(title);
    g_free(name);
}

static void set_status(ComposerWindow *w, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(w->status_msg), msg ? msg : "");
}

static void on_modified_changed(GtkTextBuffer *buf, gpointer data)
{
    (void)buf;
    update_title(data);
}

static void on_cursor(ComposerEditor *ed, int line, int col, gpointer data)
{
    (void)ed;
    ComposerWindow *w = data;
    char text[64];
    g_snprintf(text, sizeof text, "Ln %d, Col %d", line, col);
    gtk_label_set_text(GTK_LABEL(w->status_pos), text);
}

static gboolean load_file(ComposerWindow *w, GFile *file)
{
    char *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_load_contents(file, NULL, &contents, &len, NULL, &err)) {
        set_status(w, err->message);
        g_error_free(err);
        return FALSE;
    }
    if (!g_utf8_validate(contents, len, NULL)) {
        set_status(w, "Cannot open: file is not valid UTF-8 text");
        g_free(contents);
        return FALSE;
    }
    composer_editor_set_text(w->editor, contents);
    g_free(contents);
    g_set_object(&w->file, file);
    update_title(w);
    set_status(w, NULL);
    return TRUE;
}

static gboolean save_file(ComposerWindow *w, GFile *file)
{
    char *text = composer_editor_get_text(w->editor);
    GError *err = NULL;
    gboolean ok = g_file_replace_contents(file, text, strlen(text), NULL, FALSE,
                                          G_FILE_CREATE_NONE, NULL, NULL, &err);
    g_free(text);
    if (!ok) {
        set_status(w, err->message);
        g_error_free(err);
        return FALSE;
    }
    g_set_object(&w->file, file);
    gtk_text_buffer_set_modified(composer_editor_buffer(w->editor), FALSE);
    update_title(w);
    set_status(w, "Saved");
    return TRUE;
}

static ComposerWindow *composer_window_new(GtkApplication *app, GFile *file);

/* ------------------------------------------------------------------ */
/*  File dialogs (GtkFileDialog on 4.10+, GtkFileChooserNative before)  */
/* ------------------------------------------------------------------ */

static void after_open(ComposerWindow *w, GFile *file)
{
    if (!gtk_text_buffer_get_modified(composer_editor_buffer(w->editor)))
        load_file(w, file);
    else
        composer_window_new(w->app, file);   /* never throw away unsaved work */
}

static void after_save_as(ComposerWindow *w, GFile *file)
{
    if (save_file(w, file) && w->close_after_save)
        gtk_window_destroy(GTK_WINDOW(w->window));
    w->close_after_save = FALSE;
}

#if GTK_CHECK_VERSION(4, 10, 0)

static GtkFileDialog *make_dialog(ComposerWindow *w, const char *title)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title);
    GtkFileFilter *f = gtk_file_filter_new();
    char *pattern = g_strdup_printf("*.%s", language_get()->file_extension);
    gtk_file_filter_set_name(f, "C-omposer files");
    gtk_file_filter_add_pattern(f, pattern);
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(f);
    g_object_unref(all);
    g_free(pattern);
    if (w->file)
        gtk_file_dialog_set_initial_file(d, w->file);
    return d;
}

static void open_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (file) {
        after_open(data, file);
        g_object_unref(file);
    }
}

static void save_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ComposerWindow *w = data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (file) {
        after_save_as(w, file);
        g_object_unref(file);
    } else {
        w->close_after_save = FALSE;
    }
}

static void show_open_dialog(ComposerWindow *w)
{
    GtkFileDialog *d = make_dialog(w, "Open composition");
    gtk_file_dialog_open(d, GTK_WINDOW(w->window), NULL, open_done, w);
    g_object_unref(d);
}

static void show_save_dialog(ComposerWindow *w)
{
    GtkFileDialog *d = make_dialog(w, "Save composition");
    if (!w->file) {
        char *name = g_strdup_printf("untitled.%s", language_get()->file_extension);
        gtk_file_dialog_set_initial_name(d, name);
        g_free(name);
    }
    gtk_file_dialog_save(d, GTK_WINDOW(w->window), NULL, save_done, w);
    g_object_unref(d);
}

#else /* GTK < 4.10 */

static void chooser_response(GtkNativeDialog *d, int response, gpointer data)
{
    ComposerWindow *w = data;
    gboolean saving = gtk_file_chooser_get_action(GTK_FILE_CHOOSER(d)) == GTK_FILE_CHOOSER_ACTION_SAVE;
    GFile *file = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d)) : NULL;
    if (file) {
        if (saving) after_save_as(w, file);
        else        after_open(w, file);
        g_object_unref(file);
    } else if (saving) {
        w->close_after_save = FALSE;
    }
    g_object_unref(d);
}

static void show_chooser(ComposerWindow *w, gboolean save)
{
    GtkFileChooserNative *d = gtk_file_chooser_native_new(
        save ? "Save composition" : "Open composition", GTK_WINDOW(w->window),
        save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
        save ? "_Save" : "_Open", "_Cancel");
    GtkFileFilter *f = gtk_file_filter_new();
    char *pattern = g_strdup_printf("*.%s", language_get()->file_extension);
    gtk_file_filter_set_name(f, "C-omposer files");
    gtk_file_filter_add_pattern(f, pattern);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
    g_object_unref(f);
    g_object_unref(all);
    if (save) {
        if (w->file) {
            gtk_file_chooser_set_file(GTK_FILE_CHOOSER(d), w->file, NULL);
        } else {
            char *name = g_strdup_printf("untitled.%s", language_get()->file_extension);
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), name);
            g_free(name);
        }
    }
    g_free(pattern);
    g_signal_connect(d, "response", G_CALLBACK(chooser_response), w);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(d));
}

static void show_open_dialog(ComposerWindow *w) { show_chooser(w, FALSE); }
static void show_save_dialog(ComposerWindow *w) { show_chooser(w, TRUE); }

#endif

/* ------------------------------------------------------------------ */
/*  Actions                                                             */
/* ------------------------------------------------------------------ */

static void act_open(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    show_open_dialog(data);
}

static void act_save(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    if (w->file) save_file(w, w->file);
    else         show_save_dialog(w);
}

static void act_save_as(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    show_save_dialog(data);
}

static void act_new(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    ComposerWindow *nw = composer_window_new(w->app, NULL);
    composer_editor_set_text(nw->editor, "");
}

/* ------------------------------------------------------------------ */
/*  Closing with unsaved changes                                        */
/* ------------------------------------------------------------------ */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS   /* GtkMessageDialog: still fine, deprecated in 4.10 */

static void close_response(GtkDialog *dialog, int response, gpointer data)
{
    ComposerWindow *w = data;
    gtk_window_destroy(GTK_WINDOW(dialog));
    if (response == GTK_RESPONSE_REJECT) {            /* discard */
        gtk_text_buffer_set_modified(composer_editor_buffer(w->editor), FALSE);
        gtk_window_destroy(GTK_WINDOW(w->window));
    } else if (response == GTK_RESPONSE_ACCEPT) {     /* save */
        if (w->file) {
            if (save_file(w, w->file))
                gtk_window_destroy(GTK_WINDOW(w->window));
        } else {
            w->close_after_save = TRUE;
            show_save_dialog(w);
        }
    }
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    ComposerWindow *w = data;
    if (!gtk_text_buffer_get_modified(composer_editor_buffer(w->editor)))
        return FALSE;

    GtkWidget *d = gtk_message_dialog_new(win, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "Save changes before closing?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Unsaved changes will be lost.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Discard", GTK_RESPONSE_REJECT,
                           "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    g_signal_connect(d, "response", G_CALLBACK(close_response), w);
    gtk_window_present(GTK_WINDOW(d));
    return TRUE;
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ------------------------------------------------------------------ */
/*  Window                                                              */
/* ------------------------------------------------------------------ */

static void composer_window_free(gpointer data)
{
    ComposerWindow *w = data;
    g_clear_object(&w->file);
    g_free(w);
}

static ComposerWindow *composer_window_new(GtkApplication *app, GFile *file)
{
    ComposerWindow *w = g_new0(ComposerWindow, 1);
    w->app = app;
    w->window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(w->window), 1000, 720);
    g_object_set_data_full(G_OBJECT(w->window), "composer-window", w, composer_window_free);

    static const GActionEntry actions[] = {
        { .name = "new",     .activate = act_new     },
        { .name = "open",    .activate = act_open    },
        { .name = "save",    .activate = act_save    },
        { .name = "save-as", .activate = act_save_as },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), actions, G_N_ELEMENTS(actions), w);

    /* header bar */
    GtkWidget *header = gtk_header_bar_new();
    w->title = gtk_label_new(NULL);
    gtk_widget_add_css_class(w->title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), w->title);

    GtkWidget *open = gtk_button_new_with_label("Open");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
    gtk_widget_set_tooltip_text(open, "Open (Ctrl+O)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New window", "win.new");
    g_menu_append(menu, "Save as…", "win.save-as");
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_btn);

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(save), "win.save");
    gtk_widget_set_tooltip_text(save, "Save (Ctrl+S)");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), save);

    gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

    /* editor + status bar */
    w->editor = composer_editor_new(language_get());

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(status, "composer-status");
    w->status_pos = gtk_label_new("Ln 1, Col 1");
    w->status_msg = gtk_label_new(NULL);
    gtk_widget_set_hexpand(w->status_msg, TRUE);
    gtk_label_set_xalign(GTK_LABEL(w->status_msg), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->status_msg), PANGO_ELLIPSIZE_END);
    GtkWidget *hint = gtk_label_new("Ctrl+Space  autocomplete");
    gtk_box_append(GTK_BOX(status), w->status_pos);
    gtk_box_append(GTK_BOX(status), w->status_msg);
    gtk_box_append(GTK_BOX(status), hint);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), composer_editor_widget(w->editor));
    gtk_box_append(GTK_BOX(box), status);
    gtk_window_set_child(GTK_WINDOW(w->window), box);

    composer_editor_set_cursor_callback(w->editor, on_cursor, w);
    g_signal_connect(composer_editor_buffer(w->editor), "modified-changed", G_CALLBACK(on_modified_changed), w);
    g_signal_connect(w->window, "close-request", G_CALLBACK(on_close_request), w);

    if (!file || !load_file(w, file))
        composer_editor_set_text(w->editor, SAMPLE);
    update_title(w);

    gtk_window_present(GTK_WINDOW(w->window));
    gtk_widget_grab_focus(GTK_WIDGET(composer_editor_view(w->editor)));
    return w;
}

/* ------------------------------------------------------------------ */

static void on_startup(GApplication *app, gpointer data)
{
    (void)data;
    theme_install_css();
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);

    struct { const char *action; const char *accel; } accels[] = {
        { "win.new",     "<Ctrl>n" },
        { "win.open",    "<Ctrl>o" },
        { "win.save",    "<Ctrl>s" },
        { "win.save-as", "<Ctrl><Shift>s" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(accels); i++)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), accels[i].action,
                                              (const char *[]){ accels[i].accel, NULL });
}

static void on_activate(GApplication *app, gpointer data)
{
    (void)data;
    composer_window_new(GTK_APPLICATION(app), NULL);
}

static void on_open(GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
    (void)hint; (void)data;
    for (int i = 0; i < n; i++)
        composer_window_new(GTK_APPLICATION(app), files[i]);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("dev.composer.CComposer",
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
