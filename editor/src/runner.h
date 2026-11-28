/*
 * runner.h — builds the song in the background and runs single functions
 * from it, in a separate process (a crashing song can't take the editor
 * down). Also plays the rendered audio.
 *
 * Builds are cached: asking again for the same text reuses the last build.
 */
#pragma once

#include <glib.h>

typedef struct Runner Runner;

typedef void (*RunnerBuildDone)(gboolean ok, const char *log, gpointer data);
typedef void (*RunnerRenderDone)(gboolean ok, const char *output, const char *path, gpointer data);
typedef void (*RunnerPlayDone)(gpointer data);

Runner     *runner_new(void);
void        runner_free(Runner *r);          /* pending callbacks are dropped */
const char *runner_dir(Runner *r);           /* scratch directory for renders */
/* Songs run in this folder (the project), so load_sample("samples/x.wav") works. */
void        runner_set_cwd(Runner *r, const char *dir);

void runner_build(Runner *r, const char *text, const char *label, RunnerBuildDone done, gpointer data);

/* Render one function of the last successful build to `path`. */
void runner_render(Runner *r, const char *function, const char *path, RunnerRenderDone done, gpointer data);

/* Play an audio file (stops anything already playing). */
gboolean runner_play(Runner *r, const char *path, RunnerPlayDone done, gpointer data, char **error);
void     runner_stop(Runner *r);
