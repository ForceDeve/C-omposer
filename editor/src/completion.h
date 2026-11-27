/*
 * completion.h — the autocompletion popup.
 *
 * Candidates come from the language's reserved words (language.c) plus the
 * symbols found in the document (symbols.c).  After '.' or '->' it offers
 * the fields of the variable's type, including chains like a.b[2].c and
 * call results like make_chord(...).
 */
#pragma once

#include <gtk/gtk.h>
#include "language.h"
#include "symbols.h"

typedef struct Completion Completion;

Completion *completion_new(GtkTextView *view);
void        completion_free(Completion *c);

/* Recompute the popup for the cursor position: shows, filters or hides it.
 * `force` (Ctrl+Space) shows it even when the prefix is shorter than
 * lang->completion_min_prefix. */
void        completion_refresh(Completion *c, const Language *lang, const SymbolTable *symbols, gboolean force);
void        completion_hide(Completion *c);
gboolean    completion_is_visible(Completion *c);

/* True while the popup itself is inserting text (so the editor doesn't
 * re-trigger completion for it). */
gboolean    completion_is_inserting(Completion *c);

/* Up/Down/PageUp/PageDown/Enter/Tab/Escape while visible.  Returns TRUE if handled. */
gboolean    completion_handle_key(Completion *c, guint keyval, GdkModifierType state);

/* Is `widget` part of the popup? (used to ignore clicks inside it) */
gboolean    completion_contains(Completion *c, GtkWidget *widget);
