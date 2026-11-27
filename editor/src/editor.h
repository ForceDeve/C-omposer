/*
 * editor.h — the code editor widget: GtkTextView + syntax highlighting,
 * line-number gutter, autocompletion, auto-indent, auto-closed brackets and
 * bracket matching.
 */
#pragma once

#include <gtk/gtk.h>
#include "language.h"

typedef struct ComposerEditor ComposerEditor;

typedef void (*EditorCursorCallback)(ComposerEditor *editor, int line, int column, gpointer user_data);

/* The editor is freed automatically when its widget is destroyed. */
ComposerEditor *composer_editor_new(const Language *lang);
GtkWidget      *composer_editor_widget(ComposerEditor *editor);
GtkTextBuffer  *composer_editor_buffer(ComposerEditor *editor);
GtkTextView    *composer_editor_view(ComposerEditor *editor);

/* Replaces the whole text (not undoable, clears the modified flag). */
void            composer_editor_set_text(ComposerEditor *editor, const char *text);
char           *composer_editor_get_text(ComposerEditor *editor);

void            composer_editor_set_cursor_callback(ComposerEditor *editor, EditorCursorCallback cb, gpointer user_data);
