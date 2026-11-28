/*
 * theme.h — colours and fonts, loaded from theme files.
 *
 * A theme is a small INI file, in the same format as FHI's: every key is
 * optional, and anything a theme leaves out is worked out from what it does
 * set (or taken from the built-in dark/light defaults).
 *
 *   [theme]
 *   name       = Dracula
 *   scheme     = dark             # dark | light | system
 *   window_bg  = #282a36          # window, header bar, menus
 *   window_fg  = #f8f8f2          # text
 *   panel_bg   = #21222c          # sidebar, output panel
 *   accent     = #bd93f9          # selection, buttons, focus
 *   editor_bg  = #282a36          # the code (default: panel_bg / window_bg)
 *   editor_fg, caret, selection, gutter_fg, gutter_current, bracket
 *   radius     = 6                # rounded corners in px
 *   font       = JetBrains Mono
 *   font_size  = 11
 *   # syntax: one key per token kind
 *   keyword, music_keyword, type, music_type, user_type, builtin, function,
 *   constant, number, unit, note, string, char, comment, preprocessor,
 *   operator, punctuation, error
 *
 * Themes are looked up in ~/.config/c-omposer/themes/ first (yours win), then
 * in editor/themes/ of the source tree.
 */
#pragma once

#include <gtk/gtk.h>
#include "tokens.h"

#define THEME_COLOR 32

typedef struct {
    char     foreground[THEME_COLOR];   /* "" = the default text colour */
    char     background[THEME_COLOR];   /* "" = none */
    gboolean bold;
    gboolean italic;
    gboolean underline;                 /* squiggly error underline */
} TokenStyle;

typedef struct {
    char font_family[128];
    int  font_size_pt;
    int  scheme;                        /* 0 system, 1 light, 2 dark */
    int  radius;
    char window_bg[THEME_COLOR], window_fg[THEME_COLOR], panel_bg[THEME_COLOR], accent[THEME_COLOR];
    char background[THEME_COLOR];       /* editor */
    char foreground[THEME_COLOR];
    char caret[THEME_COLOR];
    char selection[THEME_COLOR];
    char gutter_background[THEME_COLOR];
    char gutter_foreground[THEME_COLOR];
    char gutter_current[THEME_COLOR];   /* line number of the cursor line */
    char bracket_match[THEME_COLOR];    /* background of matching brackets */
    char popup_background[THEME_COLOR];
    char popup_border[THEME_COLOR];
    char popup_selected[THEME_COLOR];
    char popup_detail[THEME_COLOR];
} EditorTheme;

const EditorTheme *theme_get(void);
const TokenStyle  *theme_token_style(TokenCategory category);

/* ---- the theme files ---- */

typedef struct {
    char    *id;      /* file name without .ini; what settings.ini stores */
    char    *name;    /* what the file calls itself */
    char    *path;
    gboolean user;    /* from ~/.config/c-omposer/themes */
} ThemeInfo;

GPtrArray  *theme_list(void);           /* ThemeInfo*, sorted by name (free with g_ptr_array_unref) */
const char *theme_current_id(void);
char       *theme_user_dir(void);       /* new string */

/* Load and apply a theme everywhere, live. FALSE (and nothing changes) if
 * it can't be read. */
gboolean theme_apply(const char *id);

/* Called after every theme change (to redraw custom-drawn parts). */
typedef void (*ThemeListener)(gpointer data);
void theme_add_listener(ThemeListener fn, gpointer data);
void theme_remove_listener(ThemeListener fn, gpointer data);

/* Installs the application-wide CSS for the current theme. */
void theme_install_css(void);

/* One GtkTextTag per token category (named token_category_name()); they
 * follow theme changes. */
void theme_create_tags(GtkTextBuffer *buffer, GtkTextTag *tags[TOK_COUNT]);

/* Tag used to highlight matching brackets (follows theme changes too). */
GtkTextTag *theme_create_bracket_tag(GtkTextBuffer *buffer);
