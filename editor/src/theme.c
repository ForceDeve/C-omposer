/*
 * theme.c — ★ COLOURS & FONTS ★
 *
 * token_styles[] gives every token category its look; editor_theme holds the
 * editor chrome.  The completion popup's kind badges reuse the token colours,
 * so a music type is the same teal in the code and in the popup.
 */
#include "theme.h"

static const EditorTheme editor_theme = {
    .font_family       = "JetBrains Mono, Fira Code, Source Code Pro, DejaVu Sans Mono, monospace",
    .font_size_pt      = 11,
    .background        = "#1b1d23",
    .foreground        = "#d4d7de",
    .caret             = "#528bff",
    .selection         = "#3e4451",
    .gutter_background = "#1b1d23",
    .gutter_foreground = "#4b5263",
    .gutter_current    = "#abb2bf",
    .bracket_match     = "#3b4252",
    .popup_background  = "#21252b",
    .popup_border      = "#3e4451",
    .popup_selected    = "#2c3340",
    .popup_detail      = "#7f848e",
};

/*                                 foreground  background  bold   italic  underline */
static const TokenStyle token_styles[TOK_COUNT] = {
    [TOK_PLAIN]         = { NULL },
    [TOK_IDENTIFIER]    = { NULL },
    [TOK_KEYWORD]       = { "#c678dd", NULL, TRUE,  FALSE, FALSE },
    [TOK_MUSIC_KEYWORD] = { "#ff79c6", NULL, TRUE,  FALSE, FALSE },
    [TOK_TYPE]          = { "#e5c07b", NULL, FALSE, FALSE, FALSE },
    [TOK_MUSIC_TYPE]    = { "#56d4c4", NULL, TRUE,  FALSE, FALSE },
    [TOK_USER_TYPE]     = { "#f0a45d", NULL, FALSE, FALSE, FALSE },
    [TOK_BUILTIN]       = { "#4fc1ff", NULL, FALSE, FALSE, FALSE },
    [TOK_FUNCTION]      = { "#61afef", NULL, FALSE, FALSE, FALSE },
    [TOK_CONSTANT]      = { "#f78c6c", NULL, FALSE, FALSE, FALSE },
    [TOK_NUMBER]        = { "#d19a66", NULL, FALSE, FALSE, FALSE },
    [TOK_UNIT]          = { "#e6b673", NULL, FALSE, TRUE,  FALSE },
    [TOK_NOTE]          = { "#ff6e9c", NULL, TRUE,  FALSE, FALSE },
    [TOK_STRING]        = { "#98c379", NULL, FALSE, FALSE, FALSE },
    [TOK_CHAR]          = { "#98c379", NULL, FALSE, FALSE, FALSE },
    [TOK_COMMENT]       = { "#5c6370", NULL, FALSE, TRUE,  FALSE },
    [TOK_PREPROCESSOR]  = { "#e06c75", NULL, FALSE, FALSE, FALSE },
    [TOK_OPERATOR]      = { "#89ddff", NULL, FALSE, FALSE, FALSE },
    [TOK_PUNCTUATION]   = { "#abb2bf", NULL, FALSE, FALSE, FALSE },
    [TOK_ERROR]         = { "#ff5370", NULL, FALSE, FALSE, TRUE  },
};

/* Badge colour for struct fields in the completion popup. */
static const char *member_kind_color = "#c3e88d";

const EditorTheme *theme_get(void) { return &editor_theme; }

const TokenStyle *theme_token_style(TokenCategory category)
{
    return &token_styles[(category >= 0 && category < TOK_COUNT) ? category : TOK_PLAIN];
}

void theme_create_tags(GtkTextBuffer *buffer, GtkTextTag *tags[TOK_COUNT])
{
    for (int c = 0; c < TOK_COUNT; c++) {
        const TokenStyle *st = &token_styles[c];
        GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, token_category_name(c), NULL);
        if (st->foreground) g_object_set(tag, "foreground", st->foreground, NULL);
        if (st->background) g_object_set(tag, "background", st->background, NULL);
        if (st->bold)       g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
        if (st->italic)     g_object_set(tag, "style", PANGO_STYLE_ITALIC, NULL);
        if (st->underline)  g_object_set(tag, "underline", PANGO_UNDERLINE_ERROR, NULL);
        tags[c] = tag;
    }
}

GtkTextTag *theme_create_bracket_tag(GtkTextBuffer *buffer)
{
    return gtk_text_buffer_create_tag(buffer, "bracket-match",
                                      "background", editor_theme.bracket_match,
                                      "weight", PANGO_WEIGHT_BOLD, NULL);
}

void theme_install_css(void)
{
    const EditorTheme *t = &editor_theme;
    GString *css = g_string_new(NULL);

    g_string_append_printf(css,
        "textview.composer-editor { font-family: %s; font-size: %dpt; background-color: %s; }\n"
        "textview.composer-editor text { background-color: %s; color: %s; caret-color: %s; }\n"
        "textview.composer-editor text selection { background-color: %s; }\n"
        ".composer-gutter { background-color: %s; }\n"
        ".completion { background-color: %s; border: 1px solid %s; border-radius: 6px; padding: 2px; }\n"
        ".completion list { background-color: transparent; }\n"
        ".completion row { padding: 1px 6px; border-radius: 4px; }\n"
        ".completion row:selected { background-color: %s; }\n"
        ".completion .kind { font-weight: bold; min-width: 22px; }\n"
        ".completion .name { color: %s; }\n"
        ".completion .detail { color: %s; font-size: 0.9em; }\n"
        ".completion .doc { color: %s; padding: 6px 8px 4px 8px; border-top: 1px solid %s; }\n"
        ".composer-status { padding: 2px 10px; font-size: 0.9em; color: %s; background-color: %s; }\n",
        t->font_family, t->font_size_pt, t->background,
        t->background, t->foreground, t->caret,
        t->selection,
        t->gutter_background,
        t->popup_background, t->popup_border,
        t->popup_selected,
        t->foreground,
        t->popup_detail,
        t->popup_detail, t->popup_border,
        t->popup_detail, t->background);

    for (int c = 0; c < TOK_COUNT; c++)
        g_string_append_printf(css, ".completion .kind-%s { color: %s; }\n",
                               token_category_name(c),
                               token_styles[c].foreground ? token_styles[c].foreground : t->foreground);
    g_string_append_printf(css, ".completion .kind-member { color: %s; }\n", member_kind_color);

    GtkCssProvider *provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css->str);
#else
    gtk_css_provider_load_from_data(provider, css->str, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_string_free(css, TRUE);
}
