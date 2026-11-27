/*
 * editor.h — the code editor widget: GtkTextView + syntax highlighting,
 * line-number gutter, autocompletion, auto-indent, auto-closed brackets and
 * bracket matching.
 */
#pragma once

#include <gtk/gtk.h>
#include "language.h"
#include "source.h"

typedef struct ComposerEditor ComposerEditor;

typedef void (*EditorCursorCallback)(ComposerEditor *editor, int line, int column, gpointer user_data);
/* A ▶ in the margin was clicked. */
typedef void (*EditorPlayCallback)(ComposerEditor *editor, const char *function, gpointer user_data);

/* The editor is freed automatically when its widget is destroyed. */
ComposerEditor *composer_editor_new(const Language *lang);
GtkWidget      *composer_editor_widget(ComposerEditor *editor);
GtkTextBuffer  *composer_editor_buffer(ComposerEditor *editor);
GtkTextView    *composer_editor_view(ComposerEditor *editor);

/* Replaces the whole text (not undoable, clears the modified flag). */
void            composer_editor_set_text(ComposerEditor *editor, const char *text);
char           *composer_editor_get_text(ComposerEditor *editor);

void            composer_editor_set_cursor_callback(ComposerEditor *editor, EditorCursorCallback cb, gpointer user_data);

/* --- running code ------------------------------------------------ */

void composer_editor_set_play_callback(ComposerEditor *editor, EditorPlayCallback cb, gpointer user_data);
/* The functions in the document (up to date). */
int  composer_editor_functions(ComposerEditor *editor, const src_function **out);
/* Interpreter mode: show //viz panels under functions marked with it. */
void composer_editor_set_viz_mode(ComposerEditor *editor, gboolean on);
/* Gutter state: which function is being built/rendered or is playing. */
void composer_editor_set_busy(ComposerEditor *editor, const char *function);
void composer_editor_set_playing(ComposerEditor *editor, const char *function);
const char *composer_editor_playing(ComposerEditor *editor);   /* NULL if nothing */
/* Panel contents for a //viz function. */
void composer_editor_set_viz_state(ComposerEditor *editor, const char *function, const char *state);
void composer_editor_set_viz_audio(ComposerEditor *editor, const char *function,
                                   const float *l, const float *r, guint frames, guint rate);
