/*
 * main.c — C-omposer application window: editor, header bar, open/save,
 * status bar.
 */
#include <gtk/gtk.h>
#include <string.h>

#include "editor.h"
#include "language.h"
#include "theme.h"

#include "runner.h"
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
    GtkWidget      *run_button;
    GtkWidget      *interp_toggle, *compile_toggle;
    GtkTextBuffer  *output;
    GtkWidget      *output_view;
    guint           live_timer;
    guint           live_gen;        /* newer live updates make older renders stale */
    GPtrArray      *viz_queue;       /* function names still to render */
} ComposerWindow;

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

static void act_template(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    ComposerWindow *w = data;
    const char *id = g_variant_get_string(p, NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(templates); i++) {
        if (strcmp(templates[i].id, id)) continue;
        /* never throw away unsaved work: open it in a new window instead */
        ComposerWindow *t = gtk_text_buffer_get_modified(composer_editor_buffer(w->editor)) || w->file
                          ? composer_window_new(w->app, NULL) : w;
        composer_editor_set_text(t->editor, templates[i].text);
        update_title(t);
    }
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
static double show_render(ComposerWindow *w, const char *function, const char *path)
{
    sound s = sound_load(path);
    double seconds = s.frames ? (double)s.frames / audio.rate : 0;
    const src_function *f = find_function(w, function);
    if (s.frames && f && f->viz)
        composer_editor_set_viz_audio(w->editor, function, s.l, s.r, s.frames, audio.rate);
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
    set_status(w, NULL);
}

static void play_rendered(gboolean ok, const char *out, const char *path, gpointer data)
{
    Job *j = data;
    ComposerWindow *w = j->w;
    output_append(w, out);
    composer_editor_set_busy(w->editor, NULL);
    if (ok) {
        show_render(w, j->function, path);
        char *err = NULL;
        if (runner_play(w->runner, path, playback_ended, w, &err)) {
            composer_editor_set_playing(w->editor, j->function);
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
        set_status(w, "Exported");
        g_free(msg); g_free(label);
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

static void start_export(ComposerWindow *w, GFile *file)
{
    char *path = g_file_get_path(file);
    if (!path) return;
    output_heading(w, "Compiling to %s", path);
    set_status(w, "Compiling…");
    char *text = composer_editor_get_text(w->editor);
    char *label = source_label(w);
    runner_build(w->runner, text, label, export_built, job_new(w, "main", path));
    g_free(text); g_free(label); g_free(path);
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

static void act_run(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    ComposerWindow *w = data;
    if (w->mode == MODE_COMPILER) {
        show_export_dialog(w);
        return;
    }
    if (!find_function(w, "main")) {
        set_status(w, "No main() to run");
        return;
    }
    play_function(w, "main");
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
            show_render(w, j->function, path);
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
    gtk_button_set_label(GTK_BUTTON(w->run_button), mode == MODE_INTERPRETER ? "▶  Run" : "⤓  Build");
    gtk_widget_set_tooltip_text(w->run_button, mode == MODE_INTERPRETER
        ? "Run main() and play it (F5)"
        : "Compile the song to an audio file: mp3, wav, flac, ogg… (F5)");
    if (mode == MODE_INTERPRETER) schedule_live_update(w, 50);
}

static void on_mode_toggled(GtkToggleButton *b, gpointer data)
{
    ComposerWindow *w = data;
    if (!gtk_toggle_button_get_active(b)) return;
    set_mode(w, GTK_WIDGET(b) == w->interp_toggle ? MODE_INTERPRETER : MODE_COMPILER);
}

/* ------------------------------------------------------------------ */
/*  Window                                                              */
/* ------------------------------------------------------------------ */

static void composer_window_free(gpointer data)
{
    ComposerWindow *w = data;
    if (w->live_timer) g_source_remove(w->live_timer);
    runner_free(w->runner);
    g_ptr_array_unref(w->viz_queue);
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
        { .name = "stop",     .activate = act_stop     },
        { .name = "clear-output", .activate = act_clear_output },
    };
    w->runner = runner_new();
    w->viz_queue = g_ptr_array_new_with_free_func(g_free);
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

    GMenu *tmenu = g_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(templates); i++) {
        char *action = g_strdup_printf("win.template::%s", templates[i].id);
        g_menu_append(tmenu, templates[i].label, action);
        g_free(action);
    }
    GtkWidget *tbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(tbtn), "Templates");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(tbtn), G_MENU_MODEL(tmenu));
    gtk_widget_set_tooltip_text(tbtn, "Start from a template (unsaved work opens in a new window)");
    g_object_unref(tmenu);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), tbtn);

    /* Interpreter | Compiler, Run, Stop */
    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(modes, "linked");
    w->interp_toggle = gtk_toggle_button_new_with_label("Interpreter");
    w->compile_toggle = gtk_toggle_button_new_with_label("Compiler");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(w->compile_toggle), GTK_TOGGLE_BUTTON(w->interp_toggle));
    gtk_widget_set_tooltip_text(w->interp_toggle, "Live: //viz panels update as you type, ▶ plays functions");
    gtk_widget_set_tooltip_text(w->compile_toggle, "Build the whole song into an audio file");
    gtk_box_append(GTK_BOX(modes), w->interp_toggle);
    gtk_box_append(GTK_BOX(modes), w->compile_toggle);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), modes);

    w->run_button = gtk_button_new_with_label("▶  Run");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->run_button), "win.run");
    gtk_widget_add_css_class(w->run_button, "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->run_button);
    GtkWidget *stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(stop), "win.stop");
    gtk_widget_set_tooltip_text(stop, "Stop (Shift+F5)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), stop);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New window", "win.new");
    g_menu_append(menu, "Save as…", "win.save-as");
    g_menu_append(menu, "Clear output", "win.clear-output");
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

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), paned);
    gtk_box_append(GTK_BOX(box), status);
    gtk_window_set_child(GTK_WINDOW(w->window), box);

    composer_editor_set_cursor_callback(w->editor, on_cursor, w);
    composer_editor_set_play_callback(w->editor, on_play_clicked, w);
    g_signal_connect(composer_editor_buffer(w->editor), "changed", G_CALLBACK(on_text_changed), w);
    g_signal_connect(w->interp_toggle, "toggled", G_CALLBACK(on_mode_toggled), w);
    g_signal_connect(w->compile_toggle, "toggled", G_CALLBACK(on_mode_toggled), w);
    g_signal_connect(composer_editor_buffer(w->editor), "modified-changed", G_CALLBACK(on_modified_changed), w);
    g_signal_connect(w->window, "close-request", G_CALLBACK(on_close_request), w);

    if (!file || !load_file(w, file))
        composer_editor_set_text(w->editor, templates[0].text);
    update_title(w);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->interp_toggle), TRUE);   /* -> set_mode */

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
        { "win.run",     "F5" },
        { "win.stop",    "<Shift>F5" },
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
