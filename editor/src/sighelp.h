/*
 * sighelp.h — "what goes in here": while the cursor is inside a call's
 * parentheses, a small popup above it shows the function's parameters
 * (types and names) with the one being typed highlighted.
 */
#pragma once

#include <gtk/gtk.h>
#include "language.h"
#include "source.h"
#include "tokens.h"

typedef struct SigHelp SigHelp;

SigHelp *sighelp_new(GtkTextView *view);
void     sighelp_free(SigHelp *s);

/* Look at the cursor and show, update or hide the popup. */
void sighelp_update(SigHelp *s, const Language *lang, const char *text, GArray *tokens,
                    int cursor_char, const src_function *funcs, int n_funcs);
void sighelp_hide(SigHelp *s);
gboolean sighelp_visible(SigHelp *s);

/* For the parent's size_allocate (GTK needs popovers placed there). */
void sighelp_present(SigHelp *s);
/* Re-point after scrolling. */
void sighelp_reposition(SigHelp *s);
