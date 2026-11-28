/*
 * visuals.h — what's drawn behind the code in Interpreter mode: a picture of
 * your choice and/or an audio visualizer that reacts to what's playing.
 *
 * Everything here is off by default and costs nothing while off: no picture
 * is loaded, no audio is kept, no frame clock runs.
 */
#pragma once

#include <gtk/gtk.h>
#include "settings.h"

typedef struct Visuals Visuals;

Visuals *visuals_new(GtkWidget *view);
void     visuals_free(Visuals *v);

/* From the preferences: picture (NULL = none), how much the editor colour
 * covers it (0..95), which visualizer, how strong (10..100). */
void visuals_configure(Visuals *v, const char *image, int dim, VisualizerKind kind, int intensity);
/* Only shown in Interpreter mode. */
void visuals_set_active(Visuals *v, gboolean active);
gboolean visuals_wants_audio(Visuals *v);

/* The audio about to play (copied as mono only if a visualizer is on). */
void visuals_set_audio(Visuals *v, const float *l, const float *r, guint frames, guint rate);
void visuals_play_started(Visuals *v);
void visuals_play_stopped(Visuals *v);

/* Called from the text view's below-text layer (buffer coordinates). */
void visuals_snapshot(Visuals *v, GtkSnapshot *snapshot, const graphene_rect_t *visible);
