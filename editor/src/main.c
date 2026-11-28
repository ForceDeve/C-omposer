/*
 * main.c — C-omposer application window: editor, header bar, open/save,
 * status bar.
 */
#include <gtk/gtk.h>
#include <string.h>

#include "editor.h"
#include "language.h"
#include "theme.h"
#include "settings.h"

#include "runner.h"
#include "sidebar.h"
#include "prefs.h"
#include "audio.h"

typedef enum { MODE_INTERPRETER, MODE_COMPILER } RunMode;

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *title;
    GtkWidget      *status_pos;
    GtkWidget      *status_msg;
    ComposerEditor *editor;
    GFile          *file;
    gboolean        close_after_save;

    /* running code */
    Runner         *runner;
    RunMode         mode;
    GtkWidget      *play_button;     /* ▶ run in the interpreter          */
    GtkWidget      *build_button;    /* 🔨 compile to an audio file and play it */
    char           *export_path;     /* where 🔨 writes; asked for once     */
    GMenu          *template_menu;   /* rebuilt when templates change       */
    GtkTextBuffer  *output;
    GtkWidget      *output_view;
    guint           live_timer;
    guint           live_gen;        /* newer live updates make older renders stale */
    GPtrArray      *viz_queue;       /* function names still to render */

    Sidebar        *sidebar;
    GtkWidget      *sidebar_toggle;
} ComposerWindow;

static GList *all_windows;          /* ComposerWindow*: settings apply to every window */
static GList *all_windows_for_templates(void) { return all_windows; }

/* Templates, generated at build time from the .cmp files in editor/templates. */
extern const char TEMPLATE_basic[];
extern const char TEMPLATE_test_functions[];

static const struct { const char *id, *label; const char *text; } templates[] = {
    { "basic",          "Basic C program", TEMPLATE_basic },
    { "test-functions", "Test functions",  TEMPLATE_test_functions },
};

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
    composer_window_new(w->app, NULL);         /* starts from the basic template */
}

/* Your own templates: ~/.config/c-omposer/templates/<name>.cmp */
static char *user_template_dir(void)
{
    char *dir = settings_dir();
    char *t = g_build_filename(dir, "templates", NULL);
    g_free(dir);
    return t;
}

static void use_template_text(ComposerWindow *w, const char *text)
{
    /* never throw away unsaved work: open it in a new window instead */
    ComposerWindow *t = gtk_text_buffer_get_modified(composer_editor_buffer(w->editor)) || w->file
                      ? composer_window_new(w->app, NULL) : w;
    composer_editor_set_text(t->editor, text);
    update_title(t);
}

/* id: a built-in's id, or "user:<file name>" */
static void act_template(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    ComposerWindow *w = data;
    const char *id = g_variant_get_string(p, NULL);
    if (g_str_has_prefix(id, "user:")) {
        char *dir = user_template_dir();
        char *path = g_build_filename(dir, id + 5, NULL);
        char *text = NULL;
        if (g_file_get_contents(path, &text, NULL, NULL) && g_utf8_validate(text, -1, NULL))
            use_template_text(w, text);
        else
            set_status(w, "Couldn't read that template");
        g_free(text); g_free(path); g_free(dir);
        return;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(templates); i++)
        if (!strcmp(templates[i].id, id)) use_template_text(w, templates[i].text);
}

static int by_string(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(*(char *const *)a, *(char *const *)b);
}

static void fill_template_menu(GMenu *m)
{
    g_menu_remove_all(m);
    GMenu *builtin = g_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(templates); i++) {
        char *action = g_strdup_printf("win.template::%s", templates[i].id);
        g_menu_append(builtin, templates[i].label, action);
        g_free(action);
    }
    g_menu_append_section(m, NULL, G_MENU_MODEL(builtin));
    g_object_unref(builtin);

    GMenu *mine = g_menu_new();
    char *dir = user_template_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *f;
    while (d && (f = g_dir_read_name(d)))
        if (g_str_has_suffix(f, ".cmp")) g_ptr_array_add(names, g_strdup(f));
    if (d) g_dir_close(d);
    g_ptr_array_sort(names, by_string);
    for (guint i = 0; i < names->len; i++) {
        const char *file = names->pdata[i];
        char *label = g_strndup(file, strlen(file) - 4);
        for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
        GMenuItem *it = g_menu_item_new(NULL, NULL);
        g_menu_item_set_label(it, label);                 /* plain label: no mnemonic games */
        g_menu_item_set_action_and_target_value(it, "win.template", g_variant_new_printf("user:%s", file));
        g_menu_append_item(mine, it);
        g_object_unref(it);
        g_free(label);
    }
    g_ptr_array_unref(names);
    g_free(dir);
    g_menu_append_section(m, NULL, G_MENU_MODEL(mine));
    g_object_unref(mine);

    GMenu *manage = g_menu_new();
    g_menu_append(manage, "Save this file as a template…", "win.save-template");
    g_menu_append(manage, "Open my templates folder", "win.open-templates");
    g_menu_append_section(m, NULL, G_MENU_MODEL(manage));
    g_object_unref(manage);
}

static GList *all_windows_for_templates(void);

static void refresh_template_menus(void)
{
    for (GList *l = all_windows_for_templates(); l; l = l->next)
        fill_template_menu(((ComposerWindow *)l->data)->template_menu);
}

/* "Save as template": a small name prompt */
typedef struct { ComposerWindow *w; GtkWidget *dialog, *entry; } TemplatePrompt;

static void save_template_now(GtkWidget *button, gpointer data)
{
    (void)button;
    TemplatePrompt *tp = data;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(tp->entry));
    char *clean = g_strstrip(g_strdup(name));
    if (*clean) {
        for (char *c = clean; *c; c++)
            if (*c == '/' || *c == '\\' || *c == ' ') *c = '_';
        char *dir = user_template_dir();
        g_mkdir_with_parents(dir, 0755);
        char *file = g_strconcat(clean, ".cmp", NULL);
        char *path = g_build_filename(dir, file, NULL);
        char *text = composer_editor_get_text(tp->w->editor);
        GError *err = NULL;
        if (g_file_set_contents(path, text, -1, &err)) {
            char *msg = g_strdup_printf("Saved template \"%s\"", name);
            set_status(tp->w, msg);
            g_free(msg);
            refresh_template_menus();
        } else {
            set_status(tp->w, err->message);
            g_error_free(err);
        }
        g_free(text); g_free(path); g_free(file); g_free(dir);
    }
    g_free(clean);
    gtk_window_destroy(GTK_WINDOW(tp->dialog));
}

static void template_prompt_free(gpointer data) { g_free(data); }

static void act_save_template(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    TemplatePrompt *tp = g_new0(TemplatePrompt, 1);
    tp->w = w;
    tp->dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(tp->dialog), "Save as template");
    gtk_window_set_transient_for(GTK_WINDOW(tp->dialog), GTK_WINDOW(w->window));
    gtk_window_set_modal(GTK_WINDOW(tp->dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(tp->dialog), 360, -1);
    g_object_set_data_full(G_OBJECT(tp->dialog), "prompt", tp, template_prompt_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "composer-prefs");
    GtkWidget *label = gtk_label_new("Template name (it shows up under ☰ → New from template):");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    tp->entry = gtk_entry_new();
    char *base = w->file ? g_file_get_basename(w->file) : g_strdup("my template.cmp");
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    gtk_editable_set_text(GTK_EDITABLE(tp->entry), base);
    g_free(base);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), tp->dialog);
    g_signal_connect(save, "clicked", G_CALLBACK(save_template_now), tp);
    g_signal_connect(tp->entry, "activate", G_CALLBACK(save_template_now), tp);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), tp->entry);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_child(GTK_WINDOW(tp->dialog), box);
    gtk_window_present(GTK_WINDOW(tp->dialog));
}

static void act_open_templates(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    char *dir = user_template_dir();
    g_mkdir_with_parents(dir, 0755);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_show_uri(GTK_WINDOW(w->window), uri, GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_free(uri); g_free(dir);
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
/*  Output panel                                                        */
/* ------------------------------------------------------------------ */

static void output_append(ComposerWindow *w, const char *text)
{
    if (!text || !*text) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(w->output, &end);
    gtk_text_buffer_insert(w->output, &end, text, -1);
    if (text[strlen(text) - 1] != '\n') {
        gtk_text_buffer_get_end_iter(w->output, &end);
        gtk_text_buffer_insert(w->output, &end, "\n", 1);
    }
    gtk_text_buffer_get_end_iter(w->output, &end);
    GtkTextMark *m = gtk_text_buffer_create_mark(w->output, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(w->output_view), m);
    gtk_text_buffer_delete_mark(w->output, m);
}

static void output_heading(ComposerWindow *w, const char *fmt, const char *arg)
{
    GDateTime *now = g_date_time_new_now_local();
    char *time = g_date_time_format(now, "%H:%M:%S");
    char *what = g_strdup_printf(fmt, arg);
    char *line = g_strdup_printf("\n[%s] %s\n", time, what);
    output_append(w, line);
    g_free(line); g_free(what); g_free(time);
    g_date_time_unref(now);
}

static void act_clear_output(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    gtk_text_buffer_set_text(w->output, "", 0);
}

/* ------------------------------------------------------------------ */
/*  Running code: ▶ buttons, Run, Stop                                  */
/* ------------------------------------------------------------------ */

static char *source_label(ComposerWindow *w)
{
    return w->file ? g_file_get_basename(w->file)
                   : g_strdup_printf("untitled.%s", language_get()->file_extension);
}

static const src_function *find_function(ComposerWindow *w, const char *name)
{
    const src_function *fn;
    int n = composer_editor_functions(w->editor, &fn);
    for (int i = 0; i < n; i++)
        if (!strcmp(fn[i].name, name)) return &fn[i];
    return NULL;
}

/* Load a render and show it in the function's //viz panel (if it has one). */
static double show_render(ComposerWindow *w, const char *function, const char *path, gboolean for_visualizer)
{
    sound s = sound_load(path);
    double seconds = s.frames ? (double)s.frames / audio.rate : 0;
    const src_function *f = function ? find_function(w, function) : NULL;
    if (s.frames && f && f->viz)
        composer_editor_set_viz_audio(w->editor, function, s.l, s.r, s.frames, audio.rate);
    Visuals *vis = composer_editor_visuals(w->editor);
    if (for_visualizer && s.frames && visuals_wants_audio(vis))
        visuals_set_audio(vis, s.l, s.r, s.frames, audio.rate);
    sound_free(&s);
    return seconds;
}

typedef struct { ComposerWindow *w; char *function; char *path; } Job;

static Job *job_new(ComposerWindow *w, const char *function, const char *path)
{
    Job *j = g_new0(Job, 1);
    j->w = w; j->function = g_strdup(function); j->path = g_strdup(path);
    return j;
}
static void job_free(Job *j) { g_free(j->function); g_free(j->path); g_free(j); }

static void playback_ended(gpointer data)
{
    ComposerWindow *w = data;
    composer_editor_set_playing(w->editor, NULL);
    visuals_play_stopped(composer_editor_visuals(w->editor));
    set_status(w, NULL);
}

static void play_rendered(gboolean ok, const char *out, const char *path, gpointer data)
{
    Job *j = data;
    ComposerWindow *w = j->w;
    output_append(w, out);
    composer_editor_set_busy(w->editor, NULL);
    if (ok) {
        show_render(w, j->function, path, TRUE);
        char *err = NULL;
        if (runner_play(w->runner, path, playback_ended, w, &err)) {
            composer_editor_set_playing(w->editor, j->function);
            visuals_play_started(composer_editor_visuals(w->editor));
            char *msg = g_strdup_printf("Playing %s()", j->function);
            set_status(w, msg);
            g_free(msg);
        } else {
            output_append(w, err);
            set_status(w, err);
            g_free(err);
        }
    } else {
        set_status(w, "Run failed — see Output");
    }
    job_free(j);
}

static void play_built(gboolean ok, const char *log, gpointer data)
{
    Job *j = data;
    output_append(j->w, log);
    if (!ok) {
        composer_editor_set_busy(j->w->editor, NULL);
        set_status(j->w, "Build failed — see Output");
        job_free(j);
        return;
    }
    char *file = g_strdup_printf("play_%s.wav", j->function);
    char *path = g_build_filename(runner_dir(j->w->runner), file, NULL);
    runner_render(j->w->runner, j->function, path, play_rendered, j);
    g_free(file); g_free(path);
}

/* ▶ next to a function (or Run in interpreter mode: main). */
static void play_function(ComposerWindow *w, const char *name)
{
    runner_stop(w->runner);
    composer_editor_set_playing(w->editor, NULL);
    composer_editor_set_busy(w->editor, name);
    output_heading(w, "Running %s()", name);
    set_status(w, "Building…");
    char *text = composer_editor_get_text(w->editor);
    char *label = source_label(w);
    runner_build(w->runner, text, label, play_built, job_new(w, name, NULL));
    g_free(text); g_free(label);
}

static void on_play_clicked(ComposerEditor *ed, const char *function, gpointer data)
{
    (void)ed;
    ComposerWindow *w = data;
    const char *playing = composer_editor_playing(w->editor);
    if (playing && !strcmp(playing, function)) {        /* ■ : stop */
        runner_stop(w->runner);
        composer_editor_set_playing(w->editor, NULL);
        visuals_play_stopped(composer_editor_visuals(w->editor));
        set_status(w, "Stopped");
        return;
    }
    const src_function *f = find_function(w, function);
    if (f && f->n_params > 0)
        output_heading(w, "%s() takes a note/chord: playing it with C4 / C major", function);
    play_function(w, function);
}

static void act_stop(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    runner_stop(w->runner);
    composer_editor_set_playing(w->editor, NULL);
    visuals_play_stopped(composer_editor_visuals(w->editor));
    set_status(w, "Stopped");
}

/* ---- compiler mode: export the whole song --------------------------- */

static void export_rendered(gboolean ok, const char *out, const char *path, gpointer data)
{
    Job *j = data;
    ComposerWindow *w = j->w;
    output_append(w, out);
    if (ok) {
        char *label = source_label(w);
        char *msg = g_strdup_printf("Wrote %s\n(from a terminal: composerc %s -o %s)", path, label, path);
        output_append(w, msg);
        set_status(w, "Compiled");
        g_free(msg); g_free(label);
        /* 🔨 = compile and run: play what was just written */
        char *err = NULL;
        if (runner_play(w->runner, path, playback_ended, w, &err)) {
            composer_editor_set_playing(w->editor, "main");
            set_status(w, "Compiled · playing");
        } else {
            output_append(w, err);
            g_free(err);
        }
    } else {
        set_status(w, "Export failed — see Output");
    }
    job_free(j);
}

static void export_built(gboolean ok, const char *log, gpointer data)
{
    Job *j = data;
    output_append(j->w, log);
    if (!ok) {
        set_status(j->w, "Build failed — see Output");
        job_free(j);
        return;
    }
    set_status(j->w, "Rendering…");
    runner_render(j->w->runner, "main", j->path, export_rendered, j);
}

static void compile_to(ComposerWindow *w, const char *path);

static void start_export(ComposerWindow *w, GFile *file)
{
    char *path = g_file_get_path(file);
    if (!path) return;
    g_free(w->export_path);
    w->export_path = g_strdup(path);           /* 🔨 reuses it from now on */
    compile_to(w, path);
    g_free(path);
}

static void compile_to(ComposerWindow *w, const char *path)
{
    runner_stop(w->runner);
    composer_editor_set_playing(w->editor, NULL);
    output_heading(w, "Compiling to %s", path);
    set_status(w, "Compiling…");
    char *text = composer_editor_get_text(w->editor);
    char *label = source_label(w);
    runner_build(w->runner, text, label, export_built, job_new(w, "main", path));
    g_free(text); g_free(label);
}

static char *export_name(ComposerWindow *w)
{
    char *label = source_label(w);
    char *dot = strrchr(label, '.');
    if (dot) *dot = '\0';
    char *name = g_strconcat(label, ".mp3", NULL);
    g_free(label);
    return name;
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void export_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (file) { start_export(data, file); g_object_unref(file); }
}

static void show_export_dialog(ComposerWindow *w)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Export audio (.mp3 .wav .flac .ogg .opus .m4a)");
    char *name = export_name(w);
    gtk_file_dialog_set_initial_name(d, name);
    g_free(name);
    gtk_file_dialog_save(d, GTK_WINDOW(w->window), NULL, export_done, w);
    g_object_unref(d);
}
#else
static void export_response(GtkNativeDialog *d, int response, gpointer data)
{
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
        if (file) { start_export(data, file); g_object_unref(file); }
    }
    g_object_unref(d);
}

static void show_export_dialog(ComposerWindow *w)
{
    GtkFileChooserNative *d = gtk_file_chooser_native_new(
        "Export audio (.mp3 .wav .flac .ogg .opus .m4a)", GTK_WINDOW(w->window),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
    char *name = export_name(w);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), name);
    g_free(name);
    g_signal_connect(d, "response", G_CALLBACK(export_response), w);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(d));
}
#endif

static void set_mode(ComposerWindow *w, RunMode mode);

/* ▶ : interpreter mode, run main() */
static void act_run(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    set_mode(w, MODE_INTERPRETER);
    if (!find_function(w, "main")) {
        set_status(w, "No main() to run");
        return;
    }
    play_function(w, "main");
}

/* 🔨 : compiler mode, build the song to an audio file and play it */
static void act_build(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    set_mode(w, MODE_COMPILER);
    if (!find_function(w, "main")) {
        set_status(w, "No main() to compile");
        return;
    }
    if (w->export_path) compile_to(w, w->export_path);
    else                show_export_dialog(w);       /* first time: where to? */
}

static void act_export_as(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    set_mode(w, MODE_COMPILER);
    show_export_dialog(w);
}

/* ---- interpreter mode: keep //viz panels live ----------------------- */

typedef struct { ComposerWindow *w; guint gen; char *function; } VizJob;

static void render_next_viz(ComposerWindow *w, guint gen);

static void viz_rendered(gboolean ok, const char *out, const char *path, gpointer data)
{
    VizJob *j = data;
    ComposerWindow *w = j->w;
    if (j->gen == w->live_gen) {
        if (ok) {
            show_render(w, j->function, path, FALSE);
        } else {
            char *first = g_strdup(out && *out ? out : "render failed");
            char *nl = strchr(first, '\n');
            if (nl) *nl = '\0';
            composer_editor_set_viz_state(w->editor, j->function, first);
            output_heading(w, "%s() failed:", j->function);
            output_append(w, out);
            g_free(first);
        }
        render_next_viz(w, j->gen);
    }
    g_free(j->function);
    g_free(j);
}

static void render_next_viz(ComposerWindow *w, guint gen)
{
    if (gen != w->live_gen || !w->viz_queue->len) return;
    char *name = g_ptr_array_steal_index(w->viz_queue, 0);
    composer_editor_set_viz_state(w->editor, name, "rendering…");
    char *file = g_strdup_printf("viz_%s.wav", name);
    char *path = g_build_filename(runner_dir(w->runner), file, NULL);
    VizJob *j = g_new0(VizJob, 1);
    *j = (VizJob){ w, gen, name };
    runner_render(w->runner, name, path, viz_rendered, j);
    g_free(file); g_free(path);
}

static void live_built(gboolean ok, const char *log, gpointer data)
{
    VizJob *j = data;
    ComposerWindow *w = j->w;
    if (j->gen == w->live_gen) {
        if (ok) {
            render_next_viz(w, j->gen);
        } else {
            output_heading(w, "Build failed (%s)", "live");
            output_append(w, log);
            for (guint i = 0; i < w->viz_queue->len; i++)
                composer_editor_set_viz_state(w->editor, g_ptr_array_index(w->viz_queue, i),
                                              "build failed — see Output");
            g_ptr_array_set_size(w->viz_queue, 0);
        }
    }
    g_free(j);
}

static gboolean live_update(gpointer data)
{
    ComposerWindow *w = data;
    w->live_timer = 0;
    if (w->mode != MODE_INTERPRETER) return G_SOURCE_REMOVE;
    w->live_gen++;
    g_ptr_array_set_size(w->viz_queue, 0);
    const src_function *fn;
    int n = composer_editor_functions(w->editor, &fn);
    for (int i = 0; i < n; i++) {
        if (!fn[i].viz) continue;
        if (!fn[i].playable) {
            composer_editor_set_viz_state(w->editor, fn[i].name, "can't play: takes arguments other than a note/chord");
            continue;
        }
        g_ptr_array_add(w->viz_queue, g_strdup(fn[i].name));
        composer_editor_set_viz_state(w->editor, fn[i].name, "building…");
    }
    if (!w->viz_queue->len) return G_SOURCE_REMOVE;
    char *text = composer_editor_get_text(w->editor);
    char *label = source_label(w);
    VizJob *j = g_new0(VizJob, 1);
    j->w = w; j->gen = w->live_gen;
    runner_build(w->runner, text, label, live_built, j);
    g_free(text); g_free(label);
    return G_SOURCE_REMOVE;
}

static void schedule_live_update(ComposerWindow *w, guint delay_ms)
{
    if (w->mode != MODE_INTERPRETER) return;
    if (w->live_timer) g_source_remove(w->live_timer);
    w->live_timer = g_timeout_add(delay_ms, live_update, w);
}

static void on_text_changed(GtkTextBuffer *b, gpointer data)
{
    (void)b;
    schedule_live_update(data, 900);
}

static void set_mode(ComposerWindow *w, RunMode mode)
{
    w->mode = mode;
    composer_editor_set_viz_mode(w->editor, mode == MODE_INTERPRETER);
    visuals_set_active(composer_editor_visuals(w->editor), mode == MODE_INTERPRETER);
    /* the button of the current mode is lit */
    if (mode == MODE_INTERPRETER) {
        gtk_widget_add_css_class(w->play_button, "composer-mode-on");
        gtk_widget_remove_css_class(w->build_button, "composer-mode-on");
        schedule_live_update(w, 50);
    } else {
        gtk_widget_add_css_class(w->build_button, "composer-mode-on");
        gtk_widget_remove_css_class(w->play_button, "composer-mode-on");
    }
}

/* ------------------------------------------------------------------ */
/*  Preferences, background visuals                                     */
/* ------------------------------------------------------------------ */

static void apply_visual_settings(ComposerWindow *w)
{
    Settings *s = settings_get();
    Visuals *v = composer_editor_visuals(w->editor);
    visuals_configure(v, s->bg_image, s->bg_dim, s->visualizer, s->vis_intensity);
    visuals_set_active(v, w->mode == MODE_INTERPRETER);
}

static void prefs_changed(gpointer data)
{
    (void)data;
    for (GList *l = all_windows; l; l = l->next) apply_visual_settings(l->data);
}

static void act_prefs(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    prefs_show(GTK_WINDOW(w->window), prefs_changed, NULL);
}

/* ------------------------------------------------------------------ */
/*  Projects sidebar                                                    */
/* ------------------------------------------------------------------ */

static void side_open(GFile *file, gpointer data) { after_open(data, file); }

static void side_play(GFile *file, gpointer data)
{
    ComposerWindow *w = data;
    char *path = g_file_get_path(file);
    if (!path) return;
    char *err = NULL;
    composer_editor_set_playing(w->editor, NULL);
    show_render(w, NULL, path, TRUE);
    if (runner_play(w->runner, path, playback_ended, w, &err)) {
        visuals_play_started(composer_editor_visuals(w->editor));
        char *base = g_path_get_basename(path);
        char *msg = g_strdup_printf("Previewing %s", base);
        set_status(w, msg);
        g_free(msg); g_free(base);
    } else {
        set_status(w, err);
        g_free(err);
    }
    g_free(path);
}

static void side_insert(const char *text, gpointer data)
{
    ComposerWindow *w = data;
    gtk_text_buffer_insert_at_cursor(composer_editor_buffer(w->editor), text, -1);
    gtk_widget_grab_focus(GTK_WIDGET(composer_editor_view(w->editor)));
}

static void side_project_changed(const char *dir, gpointer data)
{
    ComposerWindow *w = data;
    runner_set_cwd(w->runner, dir);           /* load_sample("samples/x.wav") works */
    Settings *s = settings_get();
    if (g_strcmp0(s->project_dir, dir)) {
        g_free(s->project_dir);
        s->project_dir = g_strdup(dir);
        settings_save();
    }
}

/* A new project: samples/, snippets/ and a main.cmp to start from. */
static void create_project(ComposerWindow *w, const char *dir)
{
    const char *sub[] = { "samples", "snippets" };
    for (size_t i = 0; i < G_N_ELEMENTS(sub); i++) {
        char *p = g_build_filename(dir, sub[i], NULL);
        g_mkdir_with_parents(p, 0755);
        g_free(p);
    }
    char *main_file = g_build_filename(dir, "main.cmp", NULL);
    if (!g_file_test(main_file, G_FILE_TEST_EXISTS))
        g_file_set_contents(main_file, templates[0].text, -1, NULL);
    char *snippet = g_build_filename(dir, "snippets", "four_on_the_floor.cmp", NULL);
    if (!g_file_test(snippet, G_FILE_TEST_EXISTS))
        g_file_set_contents(snippet,
            "sound four_on_the_floor(void) {\n"
            "    part p = pattern(\"x...x...x...x...\", KICK);\n"
            "    part_pattern(&p, \"....x.......x...\", CLAP, 0.25b, 100);\n"
            "    part_pattern(&p, \"..x...x...x...x.\", HAT, 0.25b, 90);\n"
            "    return drum_machine(KIT_808, p);\n"
            "}\n", -1, NULL);
    sidebar_set_project(w->sidebar, dir);
    GFile *f = g_file_new_for_path(main_file);
    after_open(w, f);
    g_object_unref(f);
    g_free(main_file); g_free(snippet);
}

typedef struct { ComposerWindow *w; gboolean create; } FolderJob;

static void folder_chosen(FolderJob *j, GFile *folder)
{
    char *dir = folder ? g_file_get_path(folder) : NULL;
    if (dir) {
        if (j->create) create_project(j->w, dir);
        else           sidebar_set_project(j->w->sidebar, dir);
        if (!gtk_widget_get_visible(sidebar_widget(j->w->sidebar)))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(j->w->sidebar_toggle), TRUE);
    }
    g_free(dir);
    g_free(j);
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void folder_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    folder_chosen(data, f);
    if (f) g_object_unref(f);
}
static void choose_folder(ComposerWindow *w, gboolean create)
{
    FolderJob *j = g_new0(FolderJob, 1);
    *j = (FolderJob){ w, create };
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, create ? "New project: choose or create an empty folder" : "Open a project folder");
    gtk_file_dialog_select_folder(d, GTK_WINDOW(w->window), NULL, folder_done, j);
    g_object_unref(d);
}
#else
static void folder_response(GtkNativeDialog *d, int response, gpointer data)
{
    GFile *f = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d)) : NULL;
    folder_chosen(data, f);
    if (f) g_object_unref(f);
    g_object_unref(d);
}
static void choose_folder(ComposerWindow *w, gboolean create)
{
    FolderJob *j = g_new0(FolderJob, 1);
    *j = (FolderJob){ w, create };
    GtkFileChooserNative *d = gtk_file_chooser_native_new(
        create ? "New project: choose or create an empty folder" : "Open a project folder",
        GTK_WINDOW(w->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, create ? "_Create here" : "_Open", "_Cancel");
    g_signal_connect(d, "response", G_CALLBACK(folder_response), j);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(d));
}
#endif

static void side_new_project(gpointer data)  { choose_folder(data, TRUE); }
static void side_open_project(gpointer data) { choose_folder(data, FALSE); }
static void act_new_project(GSimpleAction *a, GVariant *p, gpointer data)  { (void)a; (void)p; choose_folder(data, TRUE); }
static void act_open_project(GSimpleAction *a, GVariant *p, gpointer data) { (void)a; (void)p; choose_folder(data, FALSE); }

static void on_sidebar_toggled(GtkToggleButton *b, gpointer data)
{
    ComposerWindow *w = data;
    gboolean on = gtk_toggle_button_get_active(b);
    gtk_widget_set_visible(sidebar_widget(w->sidebar), on);
    Settings *s = settings_get();
    if (s->sidebar != on) { s->sidebar = on; settings_save(); }
}

static void act_toggle_sidebar(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->sidebar_toggle),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->sidebar_toggle)));
}

/* ------------------------------------------------------------------ */
/*  Window                                                              */
/* ------------------------------------------------------------------ */

static void composer_window_free(gpointer data)
{
    ComposerWindow *w = data;
    all_windows = g_list_remove(all_windows, w);
    if (w->live_timer) g_source_remove(w->live_timer);
    runner_free(w->runner);
    g_ptr_array_unref(w->viz_queue);
    g_free(w->export_path);
    g_clear_object(&w->template_menu);
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
        { .name = "template", .activate = act_template, .parameter_type = "s" },
        { .name = "run",      .activate = act_run      },
        { .name = "build",    .activate = act_build    },
        { .name = "export-as", .activate = act_export_as },
        { .name = "save-template", .activate = act_save_template },
        { .name = "open-templates", .activate = act_open_templates },
        { .name = "stop",     .activate = act_stop     },
        { .name = "clear-output", .activate = act_clear_output },
        { .name = "preferences",  .activate = act_prefs },
        { .name = "new-project",  .activate = act_new_project },
        { .name = "open-project", .activate = act_open_project },
        { .name = "toggle-sidebar", .activate = act_toggle_sidebar },
    };
    all_windows = g_list_prepend(all_windows, w);
    w->runner = runner_new();
    w->viz_queue = g_ptr_array_new_with_free_func(g_free);
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), actions, G_N_ELEMENTS(actions), w);

    /* header bar */
    GtkWidget *header = gtk_header_bar_new();
    w->title = gtk_label_new(NULL);
    gtk_widget_add_css_class(w->title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), w->title);

    w->sidebar_toggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(w->sidebar_toggle), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(w->sidebar_toggle, "Project sidebar (F9)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->sidebar_toggle);

    GtkWidget *open = gtk_button_new_with_label("Open");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
    gtk_widget_set_tooltip_text(open, "Open (Ctrl+O)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

    /* ▶ run (interpreter) · 🔨 compile & run · ■ stop */
    GtkWidget *runbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(runbox, "linked");
    w->play_button = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->play_button), "win.run");
    gtk_widget_set_tooltip_text(w->play_button, "Run main() live in the interpreter (F5)\n//viz panels and ▶ buttons stay live as you type");
    w->build_button = gtk_button_new_from_icon_name("composer-hammer-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->build_button), "win.build");
    gtk_widget_set_tooltip_text(w->build_button, "Compile the song to an audio file and play it (Ctrl+B)\nAsks where the first time; ☰ → Export as… to change");
    gtk_box_append(GTK_BOX(runbox), w->play_button);
    gtk_box_append(GTK_BOX(runbox), w->build_button);
    GtkWidget *stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(stop), "win.stop");
    gtk_widget_set_tooltip_text(stop, "Stop (Shift+F5)");
    gtk_box_append(GTK_BOX(runbox), stop);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), runbox);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New window", "win.new");
    g_menu_append(menu, "Save as…", "win.save-as");
    g_menu_append(menu, "Export audio as…", "win.export-as");
    g_menu_append(menu, "Clear output", "win.clear-output");
    w->template_menu = g_menu_new();
    fill_template_menu(w->template_menu);
    GMenu *tsec = g_menu_new();
    g_menu_append_submenu(tsec, "New from template", G_MENU_MODEL(w->template_menu));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tsec));
    g_object_unref(tsec);
    GMenu *proj = g_menu_new();
    g_menu_append(proj, "New project…", "win.new-project");
    g_menu_append(proj, "Open project folder…", "win.open-project");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(proj));
    g_object_unref(proj);
    GMenu *prefs = g_menu_new();
    g_menu_append(prefs, "Preferences…", "win.preferences");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(prefs));
    g_object_unref(prefs);
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_btn);

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(save), "win.save");
    gtk_widget_set_tooltip_text(save, "Save (Ctrl+S)");
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
    GtkWidget *hint = gtk_label_new("F5 run   ·   Ctrl+Space autocomplete");
    gtk_box_append(GTK_BOX(status), w->status_pos);
    gtk_box_append(GTK_BOX(status), w->status_msg);
    gtk_box_append(GTK_BOX(status), hint);

    /* output panel, like an IDE's build/run console */
    w->output_view = gtk_text_view_new();
    w->output = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->output_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(w->output_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->output_view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->output_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->output_view), 4);
    gtk_widget_add_css_class(w->output_view, "composer-output");
    GtkWidget *out_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(out_scroll), w->output_view);
    gtk_widget_set_vexpand(out_scroll, TRUE);
    GtkWidget *out_title = gtk_label_new("Output");
    gtk_label_set_xalign(GTK_LABEL(out_title), 0);
    gtk_widget_add_css_class(out_title, "composer-output-title");
    GtkWidget *out_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(out_box), out_title);
    gtk_box_append(GTK_BOX(out_box), out_scroll);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(paned), composer_editor_widget(w->editor));
    gtk_paned_set_end_child(GTK_PANED(paned), out_box);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 540);
    gtk_widget_set_vexpand(paned, TRUE);

    /* projects sidebar | (editor / output) */
    SidebarCallbacks cb = { side_open, side_play, side_insert, side_project_changed,
                            side_new_project, side_open_project, w };
    w->sidebar = sidebar_new(&cb);
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(hpaned), sidebar_widget(w->sidebar));
    gtk_paned_set_end_child(GTK_PANED(hpaned), paned);
    gtk_paned_set_resize_start_child(GTK_PANED(hpaned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), FALSE);
    gtk_paned_set_position(GTK_PANED(hpaned), 230);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), hpaned);
    gtk_box_append(GTK_BOX(box), status);
    gtk_window_set_child(GTK_WINDOW(w->window), box);

    composer_editor_set_cursor_callback(w->editor, on_cursor, w);
    composer_editor_set_play_callback(w->editor, on_play_clicked, w);
    g_signal_connect(composer_editor_buffer(w->editor), "changed", G_CALLBACK(on_text_changed), w);
    g_signal_connect(w->sidebar_toggle, "toggled", G_CALLBACK(on_sidebar_toggled), w);
    Settings *st = settings_get();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->sidebar_toggle), st->sidebar);
    gtk_widget_set_visible(sidebar_widget(w->sidebar), st->sidebar);
    sidebar_set_project(w->sidebar, st->project_dir);
    g_signal_connect(composer_editor_buffer(w->editor), "modified-changed", G_CALLBACK(on_modified_changed), w);
    g_signal_connect(w->window, "close-request", G_CALLBACK(on_close_request), w);

    if (!file || !load_file(w, file))
        composer_editor_set_text(w->editor, templates[0].text);
    update_title(w);
    set_mode(w, MODE_INTERPRETER);
    apply_visual_settings(w);

    gtk_window_present(GTK_WINDOW(w->window));
    gtk_widget_grab_focus(GTK_WIDGET(composer_editor_view(w->editor)));
    return w;
}

/* ------------------------------------------------------------------ */

static void on_startup(GApplication *app, gpointer data)
{
    (void)data;
    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                   COMPOSER_ICON_DIR);        /* the hammer */
    if (!theme_apply(settings_get()->theme))       /* CSS, colours, light/dark */
        theme_apply("c-omposer-dark");

    struct { const char *action; const char *accel; } accels[] = {
        { "win.new",     "<Ctrl>n" },
        { "win.open",    "<Ctrl>o" },
        { "win.save",    "<Ctrl>s" },
        { "win.save-as", "<Ctrl><Shift>s" },
        { "win.run",     "F5" },
        { "win.build",   "<Ctrl>b" },
        { "win.stop",    "<Shift>F5" },
        { "win.toggle-sidebar", "F9" },
        { "win.preferences", "<Ctrl>comma" },
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
