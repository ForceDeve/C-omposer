/*
 * theme.h — colours and fonts.  Edit theme.c to restyle the editor.
 */
#pragma once

#include <gtk/gtk.h>
#include "tokens.h"

typedef struct {
    const char *foreground;   /* "#rrggbb" or NULL for the default text colour */
    const char *background;   /* NULL = none */
    gboolean    bold;
    gboolean    italic;
    gboolean    underline;    /* squiggly error underline */
} TokenStyle;

typedef struct {
    const char *font_family;
    int         font_size_pt;
    const char *background;
    const char *foreground;
    const char *caret;
    const char *selection;
    const char *gutter_background;
    const char *gutter_foreground;
    const char *gutter_current;     /* line number of the cursor line */
    const char *bracket_match;      /* background of matching brackets */
    const char *popup_background;
    const char *popup_border;
    const char *popup_selected;
    const char *popup_detail;
} EditorTheme;

const EditorTheme *theme_get(void);
const TokenStyle  *theme_token_style(TokenCategory category);

/* Installs the application-wide CSS (editor, gutter, completion popup). */
void theme_install_css(void);

/* Creates one GtkTextTag per token category (named token_category_name()). */
void theme_create_tags(GtkTextBuffer *buffer, GtkTextTag *tags[TOK_COUNT]);

/* Tag used to highlight matching brackets. */
GtkTextTag *theme_create_bracket_tag(GtkTextBuffer *buffer);
