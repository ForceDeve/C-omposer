/*
 * runner.c — background builds, function renders and playback.
 */
#include "runner.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "source.h"

struct Runner {
    char         *dir;
    char         *exe;
    char         *built_hash;    /* hash of the text the exe was built from */
    char         *built_log;
    GCancellable *cancel;        /* cancelled when the editor goes away */

    gboolean      building;
    /* the latest build asked for while one was running, and everyone
     * waiting for it (they all get that newest build's result) */
    char         *pending_text, *pending_label;
    GArray       *pending_waiters;   /* Waiter */

    GSubprocess  *player;
    guint         player_gen;
};

typedef struct { RunnerBuildDone done; gpointer data; } Waiter;

/* ------------------------------------------------------------------ */

Runner *runner_new(void)
{
    Runner *r = g_atomic_rc_box_new0(Runner);
    r->dir = g_dir_make_tmp("c-omposer-XXXXXX", NULL);
    if (!r->dir) r->dir = g_strdup(g_get_tmp_dir());
    r->exe = g_build_filename(r->dir, "song", NULL);
    r->cancel = g_cancellable_new();
    r->pending_waiters = g_array_new(FALSE, FALSE, sizeof(Waiter));
    return r;
}

static void runner_clear(gpointer data)
{
    Runner *r = data;
    /* remove our scratch files */
    GDir *d = g_dir_open(r->dir, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            char *p = g_build_filename(r->dir, name, NULL);
            g_unlink(p);
            g_free(p);
        }
        g_dir_close(d);
        g_rmdir(r->dir);
    }
    g_free(r->dir); g_free(r->exe);
    g_free(r->built_hash); g_free(r->built_log);
    g_free(r->pending_text); g_free(r->pending_label);
    g_array_unref(r->pending_waiters);
    g_clear_object(&r->cancel);
}

static Runner *runner_ref(Runner *r) { return g_atomic_rc_box_acquire(r); }
static void runner_unref(Runner *r) { g_atomic_rc_box_release_full(r, runner_clear); }

void runner_free(Runner *r)
{
    if (!r) return;
    g_cancellable_cancel(r->cancel);
    runner_stop(r);
    runner_unref(r);
}

const char *runner_dir(Runner *r) { return r->dir; }

/* ------------------------------------------------------------------ */
/*  Build (in a worker thread)                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    Runner   *r;
    char     *text, *label, *hash, *log;
    gboolean  ok;
    GArray   *waiters;     /* Waiter */
} BuildJob;

static void build_thread(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    BuildJob *j = task_data;
    char *tmp = g_strconcat(j->r->exe, ".new", NULL);
    j->ok = src_build(j->text, j->label, tmp, &j->log) == 0;
    if (j->ok && g_rename(tmp, j->r->exe) != 0) j->ok = FALSE;   /* atomic swap */
    g_free(tmp);
    g_task_return_boolean(task, j->ok);
}

static void start_build(Runner *r, const char *text, const char *label, GArray *waiters);

static void build_finished(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)src; (void)user;
    BuildJob *j = g_task_get_task_data(G_TASK(res));
    Runner *r = j->r;
    r->building = FALSE;
    if (j->ok) {
        g_free(r->built_hash); r->built_hash = g_strdup(j->hash);
        g_free(r->built_log);  r->built_log = g_strdup(j->log ? j->log : "");
    }
    if (!g_cancellable_is_cancelled(r->cancel)) {
        for (guint i = 0; i < j->waiters->len; i++) {
            Waiter *w = &g_array_index(j->waiters, Waiter, i);
            w->done(j->ok, j->log ? j->log : "", w->data);
        }
        if (r->pending_text) {
            char *t = r->pending_text, *l = r->pending_label;
            GArray *waiters = r->pending_waiters;
            r->pending_text = r->pending_label = NULL;
            r->pending_waiters = g_array_new(FALSE, FALSE, sizeof(Waiter));
            start_build(r, t, l, waiters);
            g_array_unref(waiters);
            g_free(t); g_free(l);
        }
    }
}

static void build_job_free(gpointer p)
{
    BuildJob *j = p;
    runner_unref(j->r);
    g_free(j->text); g_free(j->label); g_free(j->hash); free(j->log);
    g_array_unref(j->waiters);
    g_free(j);
}

typedef struct { RunnerBuildDone done; gpointer data; char *log; Runner *r; } Cached;

static gboolean cached_done(gpointer p)
{
    Cached *c = p;
    if (!g_cancellable_is_cancelled(c->r->cancel)) c->done(TRUE, c->log, c->data);
    runner_unref(c->r);
    g_free(c->log);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void start_build(Runner *r, const char *text, const char *label, GArray *waiters)
{
    char *key = g_strconcat(label, "\n", text, NULL);
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
    g_free(key);

    if (!r->building && r->built_hash && !strcmp(hash, r->built_hash)
        && g_file_test(r->exe, G_FILE_TEST_IS_EXECUTABLE)) {
        for (guint i = 0; i < waiters->len; i++) {
            Waiter *w = &g_array_index(waiters, Waiter, i);
            Cached *c = g_new0(Cached, 1);
            *c = (Cached){ w->done, w->data, g_strdup(r->built_log), runner_ref(r) };
            g_idle_add(cached_done, c);
        }
        g_free(hash);
        return;
    }
    if (r->building) {                      /* build only the newest text next */
        g_free(r->pending_text); g_free(r->pending_label);
        r->pending_text = g_strdup(text);
        r->pending_label = g_strdup(label);
        g_array_append_vals(r->pending_waiters, waiters->data, waiters->len);
        g_free(hash);
        return;
    }
    r->building = TRUE;
    BuildJob *j = g_new0(BuildJob, 1);
    j->r = runner_ref(r);
    j->text = g_strdup(text);
    j->label = g_strdup(label);
    j->hash = hash;
    j->waiters = g_array_copy(waiters);
    GTask *task = g_task_new(NULL, NULL, build_finished, NULL);
    g_task_set_task_data(task, j, build_job_free);
    g_task_run_in_thread(task, build_thread);
    g_object_unref(task);
}

void runner_build(Runner *r, const char *text, const char *label, RunnerBuildDone done, gpointer data)
{
    GArray *one = g_array_new(FALSE, FALSE, sizeof(Waiter));
    Waiter w = { done, data };
    g_array_append_val(one, w);
    start_build(r, text, label, one);
    g_array_unref(one);
}

/* ------------------------------------------------------------------ */
/*  Render one function (separate process)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    Runner *r;
    char   *path;
    RunnerRenderDone done;
    gpointer data;
} RenderJob;

static void render_finished(GObject *src, GAsyncResult *res, gpointer user)
{
    RenderJob *j = user;
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
    if (ok) ok = g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
    if (!g_cancellable_is_cancelled(j->r->cancel)) {
        if (err && !out) out = g_strdup(err->message);
        j->done(ok, out ? out : "", j->path, j->data);
    }
    g_clear_error(&err);
    g_free(out);
    runner_unref(j->r);
    g_free(j->path);
    g_free(j);
}

void runner_render(Runner *r, const char *function, const char *path, RunnerRenderDone done, gpointer data)
{
    GError *err = NULL;
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
                                      r->exe, "call", function, path, NULL);
    if (!p) {
        done(FALSE, err->message, path, data);
        g_error_free(err);
        return;
    }
    RenderJob *j = g_new0(RenderJob, 1);
    *j = (RenderJob){ runner_ref(r), g_strdup(path), done, data };
    g_subprocess_communicate_utf8_async(p, NULL, r->cancel, render_finished, j);
    g_object_unref(p);
}

/* ------------------------------------------------------------------ */
/*  Playback through the system's player                               */
/* ------------------------------------------------------------------ */

typedef struct { Runner *r; guint gen; RunnerPlayDone done; gpointer data; } PlayJob;

static void play_finished(GObject *src, GAsyncResult *res, gpointer user)
{
    PlayJob *j = user;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    /* only report the end of the current playback, not one we replaced */
    if (!g_cancellable_is_cancelled(j->r->cancel) && j->gen == j->r->player_gen) {
        g_clear_object(&j->r->player);
        if (j->done) j->done(j->data);
    }
    runner_unref(j->r);
    g_free(j);
}

gboolean runner_play(Runner *r, const char *path, RunnerPlayDone done, gpointer data, char **error)
{
    runner_stop(r);
    static const char *players[] = { "pw-play", "paplay", "aplay" };
    char *prog = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(players) && !prog; i++)
        prog = g_find_program_in_path(players[i]);
    if (!prog) {
        *error = g_strdup("no audio player found (install pipewire, pulseaudio-utils or alsa-utils)");
        return FALSE;
    }
    GError *err = NULL;
    r->player = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err,
                                 prog, path, NULL);
    g_free(prog);
    if (!r->player) {
        *error = g_strdup(err->message);
        g_error_free(err);
        return FALSE;
    }
    PlayJob *j = g_new0(PlayJob, 1);
    *j = (PlayJob){ runner_ref(r), ++r->player_gen, done, data };
    g_subprocess_wait_async(r->player, NULL, play_finished, j);
    return TRUE;
}

void runner_stop(Runner *r)
{
    if (r->player) {
        g_subprocess_force_exit(r->player);
        g_clear_object(&r->player);
    }
}
