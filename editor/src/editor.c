/*
 * editor.c — see editor.h.
 *
 * Every change schedules one high-priority idle "update" that runs before
 * the next frame is drawn:
 *     re-lex -> scan symbols -> re-apply tags -> brackets -> gutter -> completion
 * Re-lexing the whole buffer keeps things simple (block comments, types used
 * before their typedef, ...) and is fast enough for composition-sized files.
 */
#include "editor.h"
#include "completion.h"
#include "lexer.h"
#include "symbols.h"
#include "theme.h"

#include <string.h>

struct ComposerEditor {
    const Language *lang;
    GtkWidget      *scroller;
    GtkTextView    *view;
    GtkTextBuffer  *buffer;
    GtkWidget      *gutter;
    int             gutter_digits;

    GtkTextTag     *tags[TOK_COUNT];
    GtkTextTag     *bracket_tag;

    char           *text;      /* snapshot the tokens point into */
    GArray         *tokens;    /* Token */
    SymbolTable    *symbols;

    Completion     *completion;
    guint           update_id;
    gboolean        text_dirty;
    gboolean        want_completion;
    gboolean        force_completion;

    EditorCursorCallback cursor_cb;
    gpointer             cursor_data;
};

/* ------------------------------------------------------------------ */
/*  Highlighting                                                        */
/* ------------------------------------------------------------------ */

static void relex(ComposerEditor *ed)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ed->buffer, &start, &end);

    g_free(ed->text);
    if (ed->tokens)
        g_array_unref(ed->tokens);
    symbols_free(ed->symbols);

    ed->text    = gtk_text_buffer_get_text(ed->buffer, &start, &end, TRUE);
    ed->tokens  = lexer_tokenize(ed->lang, ed->text, -1);
    ed->symbols = symbols_scan(ed->lang, ed->text, ed->tokens);

    for (int c = 0; c < TOK_COUNT; c++)
        gtk_text_buffer_remove_tag(ed->buffer, ed->tags[c], &start, &end);

    const Token *t = (const Token *)ed->tokens->data;
    GtkTextIter a, b;
    for (guint i = 0; i < ed->tokens->len; i++) {
        if (t[i].category == TOK_PLAIN)
            continue;
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &a, t[i].char_start);
        b = a;
        gtk_text_iter_forward_chars(&b, t[i].char_length);
        gtk_text_buffer_apply_tag(ed->buffer, ed->tags[t[i].category], &a, &b);
    }
}

/* ------------------------------------------------------------------ */
/*  Bracket matching                                                    */
/* ------------------------------------------------------------------ */

static int find_token_at(ComposerEditor *ed, int char_offset)
{
    const Token *t = (const Token *)ed->tokens->data;
    int lo = 0, hi = (int)ed->tokens->len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid].char_start + t[mid].char_length <= char_offset) lo = mid + 1;
        else if (t[mid].char_start > char_offset)                 hi = mid - 1;
        else return mid;
    }
    return -1;
}

static char bracket_char(ComposerEditor *ed, int index)
{
    const Token *t = &g_array_index(ed->tokens, Token, index);
    if (t->category != TOK_PUNCTUATION || t->length != 1)
        return 0;
    char c = ed->text[t->start];
    return strchr("()[]{}", c) ? c : 0;
}

static void update_brackets(ComposerEditor *ed)
{
    GtkTextIter start, end, cursor;
    gtk_text_buffer_get_bounds(ed->buffer, &start, &end);
    gtk_text_buffer_remove_tag(ed->buffer, ed->bracket_tag, &start, &end);
    if (!ed->tokens)
        return;

    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor, gtk_text_buffer_get_insert(ed->buffer));
    int off = gtk_text_iter_get_offset(&cursor);

    /* bracket right after the cursor, else right before it */
    int idx = find_token_at(ed, off);
    if (idx < 0 || !bracket_char(ed, idx))
        idx = off > 0 ? find_token_at(ed, off - 1) : -1;
    if (idx < 0 || !bracket_char(ed, idx))
        return;

    static const char *pairs = "()[]{}";
    char c = bracket_char(ed, idx);
    int p = (int)(strchr(pairs, c) - pairs);
    char open = pairs[p & ~1], close = pairs[p | 1];
    int dir = (c == open) ? 1 : -1, depth = 0, match = -1;

    for (int i = idx; i >= 0 && i < (int)ed->tokens->len; i += dir) {
        char b = bracket_char(ed, i);
        if (b == open)  depth += dir;
        if (b == close) depth -= dir;
        if (depth == 0) { match = i; break; }
    }
    if (match < 0)
        return;

    int both[2] = { idx, match };
    for (int k = 0; k < 2; k++) {
        const Token *t = &g_array_index(ed->tokens, Token, both[k]);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &a, t->char_start);
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &b, t->char_start + 1);
        gtk_text_buffer_apply_tag(ed->buffer, ed->bracket_tag, &a, &b);
    }
}

/* ------------------------------------------------------------------ */
/*  Line-number gutter                                                  */
/* ------------------------------------------------------------------ */

static void update_gutter_width(ComposerEditor *ed)
{
    int lines = gtk_text_buffer_get_line_count(ed->buffer), digits = 1;
    while (lines >= 10) { lines /= 10; digits++; }
    digits = MAX(digits, 3);
    if (digits == ed->gutter_digits)
        return;
    ed->gutter_digits = digits;

    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(ed->view), "0");
    int cw, ch;
    pango_layout_get_pixel_size(layout, &cw, &ch);
    g_object_unref(layout);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ed->gutter), (digits + 2) * cw);
}

static void draw_gutter(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)height;
    ComposerEditor *ed = data;
    const EditorTheme *th = theme_get();
    GdkRGBA normal, current;
    gdk_rgba_parse(&normal, th->gutter_foreground);
    gdk_rgba_parse(&current, th->gutter_current);

    GtkTextIter cursor, it;
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor, gtk_text_buffer_get_insert(ed->buffer));
    int cursor_line = gtk_text_iter_get_line(&cursor);

    GdkRectangle vis;
    gtk_text_view_get_visible_rect(ed->view, &vis);
    gtk_text_view_get_line_at_y(ed->view, &it, vis.y, NULL);

    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(ed->view), NULL);
    for (;;) {
        int y, h, wy, tw, tht;
        gtk_text_view_get_line_yrange(ed->view, &it, &y, &h);
        if (y > vis.y + vis.height)
            break;
        gtk_text_view_buffer_to_window_coords(ed->view, GTK_TEXT_WINDOW_LEFT, 0, y, NULL, &wy);

        int line = gtk_text_iter_get_line(&it);
        char num[16];
        g_snprintf(num, sizeof num, "%d", line + 1);
        pango_layout_set_text(layout, num, -1);
        pango_layout_get_pixel_size(layout, &tw, &tht);

        gdk_cairo_set_source_rgba(cr, line == cursor_line ? &current : &normal);
        cairo_move_to(cr, width - tw - 8, wy);
        pango_cairo_show_layout(cr, layout);

        if (!gtk_text_iter_forward_line(&it))
            break;
    }
    g_object_unref(layout);
    (void)area;
}

/* ------------------------------------------------------------------ */
/*  The update pipeline                                                 */
/* ------------------------------------------------------------------ */

static gboolean update_idle(gpointer data)
{
    ComposerEditor *ed = data;
    ed->update_id = 0;

    if (ed->text_dirty) {
        relex(ed);
        ed->text_dirty = FALSE;
    }
    update_brackets(ed);
    update_gutter_width(ed);
    gtk_widget_queue_draw(ed->gutter);

    if (ed->want_completion || completion_is_visible(ed->completion))
        completion_refresh(ed->completion, ed->lang, ed->symbols, ed->force_completion);
    ed->want_completion = ed->force_completion = FALSE;

    if (ed->cursor_cb) {
        GtkTextIter c;
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &c, gtk_text_buffer_get_insert(ed->buffer));
        ed->cursor_cb(ed, gtk_text_iter_get_line(&c) + 1, gtk_text_iter_get_line_offset(&c) + 1, ed->cursor_data);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_update(ComposerEditor *ed)
{
    if (!ed->update_id)
        ed->update_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, update_idle, ed, NULL);
}

static void on_changed(GtkTextBuffer *buf, gpointer data)
{
    (void)buf;
    ComposerEditor *ed = data;
    ed->text_dirty = TRUE;
    schedule_update(ed);
}

static void on_insert_text_after(GtkTextBuffer *buf, GtkTextIter *loc, const char *text, int len, gpointer data)
{
    (void)buf; (void)loc;
    ComposerEditor *ed = data;
    if (completion_is_inserting(ed->completion))
        return;
    /* one typed character that may start/continue a word or a member access */
    if (len == 1 && (g_ascii_isalnum(text[0]) || text[0] == '_' || text[0] == '.' || text[0] == '>'))
        ed->want_completion = TRUE;
}

static void on_mark_set(GtkTextBuffer *buf, GtkTextIter *loc, GtkTextMark *mark, gpointer data)
{
    (void)loc;
    if (mark == gtk_text_buffer_get_insert(buf))
        schedule_update(data);
}

/* ------------------------------------------------------------------ */
/*  Typing helpers: indent, pairs, backspace                            */
/* ------------------------------------------------------------------ */

static int indent_width(ComposerEditor *ed) { return MAX(1, (int)strlen(ed->lang->indent)); }

/* Leading whitespace of the cursor's line (up to the cursor). */
static char *line_indent(const GtkTextIter *at)
{
    GtkTextIter i = *at;
    gtk_text_iter_set_line_offset(&i, 0);
    GString *s = g_string_new(NULL);
    while (gtk_text_iter_compare(&i, at) < 0) {
        gunichar c = gtk_text_iter_get_char(&i);
        if (c != ' ' && c != '\t')
            break;
        g_string_append_unichar(s, c);
        gtk_text_iter_forward_char(&i);
    }
    return g_string_free(s, FALSE);
}

static gboolean only_space_before(const GtkTextIter *at)
{
    GtkTextIter i = *at;
    gtk_text_iter_set_line_offset(&i, 0);
    for (; gtk_text_iter_compare(&i, at) < 0; gtk_text_iter_forward_char(&i)) {
        gunichar c = gtk_text_iter_get_char(&i);
        if (c != ' ' && c != '\t')
            return FALSE;
    }
    return TRUE;
}

static gunichar char_before(const GtkTextIter *at)
{
    GtkTextIter i = *at;
    return gtk_text_iter_backward_char(&i) ? gtk_text_iter_get_char(&i) : 0;
}

static void get_cursor(ComposerEditor *ed, GtkTextIter *it)
{
    gtk_text_buffer_get_iter_at_mark(ed->buffer, it, gtk_text_buffer_get_insert(ed->buffer));
}

static gboolean handle_newline(ComposerEditor *ed)
{
    GtkTextBuffer *buf = ed->buffer;
    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_delete_selection(buf, TRUE, TRUE);

    GtkTextIter cur;
    get_cursor(ed, &cur);
    char *indent = line_indent(&cur);

    /* last non-blank character before the cursor on this line */
    GtkTextIter back = cur;
    gunichar prev = 0;
    while (!gtk_text_iter_starts_line(&back) && gtk_text_iter_backward_char(&back)) {
        gunichar c = gtk_text_iter_get_char(&back);
        if (c != ' ' && c != '\t') { prev = c; break; }
    }
    gboolean open  = ed->lang->block_open && prev == (gunichar)ed->lang->block_open;
    gboolean close = ed->lang->block_close && gtk_text_iter_get_char(&cur) == (gunichar)ed->lang->block_close;

    char *first = g_strconcat("\n", indent, open ? ed->lang->indent : "", NULL);
    gtk_text_buffer_insert(buf, &cur, first, -1);
    if (open && close) {   /* {|}  ->  {\n    |\n} */
        GtkTextMark *m = gtk_text_buffer_create_mark(buf, NULL, &cur, TRUE);
        char *second = g_strconcat("\n", indent, NULL);
        gtk_text_buffer_insert(buf, &cur, second, -1);
        gtk_text_buffer_get_iter_at_mark(buf, &cur, m);
        gtk_text_buffer_place_cursor(buf, &cur);
        gtk_text_buffer_delete_mark(buf, m);
        g_free(second);
    }
    gtk_text_buffer_end_user_action(buf);
    gtk_text_view_scroll_mark_onscreen(ed->view, gtk_text_buffer_get_insert(buf));
    g_free(first);
    g_free(indent);
    return TRUE;
}

/* Indent / dedent every line touched by the selection (or the cursor line). */
static void shift_lines(ComposerEditor *ed, gboolean dedent)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e);
    int first = gtk_text_iter_get_line(&s), last = gtk_text_iter_get_line(&e);
    if (last > first && gtk_text_iter_get_line_offset(&e) == 0)
        last--;

    gtk_text_buffer_begin_user_action(ed->buffer);
    for (int line = first; line <= last; line++) {
        GtkTextIter a;
        gtk_text_buffer_get_iter_at_line(ed->buffer, &a, line);
        if (!dedent) {
            gtk_text_buffer_insert(ed->buffer, &a, ed->lang->indent, -1);
            continue;
        }
        GtkTextIter b = a;
        int n = 0;
        while (n < indent_width(ed) && gtk_text_iter_get_char(&b) == ' ') { gtk_text_iter_forward_char(&b); n++; }
        if (n == 0 && gtk_text_iter_get_char(&b) == '\t')
            gtk_text_iter_forward_char(&b);
        gtk_text_buffer_delete(ed->buffer, &a, &b);
    }
    gtk_text_buffer_end_user_action(ed->buffer);
}

static gboolean handle_tab(ComposerEditor *ed, gboolean shift)
{
    GtkTextIter s, e;
    gboolean sel = gtk_text_buffer_get_selection_bounds(ed->buffer, &s, &e);
    if (shift || (sel && gtk_text_iter_get_line(&s) != gtk_text_iter_get_line(&e))) {
        shift_lines(ed, shift);
        return TRUE;
    }
    gtk_text_buffer_begin_user_action(ed->buffer);
    gtk_text_buffer_delete_selection(ed->buffer, TRUE, TRUE);
    GtkTextIter cur;
    get_cursor(ed, &cur);
    /* pad to the next indent stop */
    int w = indent_width(ed), col = gtk_text_iter_get_line_offset(&cur);
    int n = (ed->lang->indent[0] == ' ') ? w - (col % w) : 1;
    char *pad = (ed->lang->indent[0] == ' ') ? g_strnfill(n, ' ') : g_strdup(ed->lang->indent);
    gtk_text_buffer_insert(ed->buffer, &cur, pad, -1);
    g_free(pad);
    gtk_text_buffer_end_user_action(ed->buffer);
    return TRUE;
}

static const char *pair_closer(ComposerEditor *ed, gunichar open)
{
    const char *p = ed->lang->auto_pairs;
    for (; p && p[0] && p[1]; p += 2)
        if ((gunichar)p[0] == open)
            return p + 1;
    return NULL;
}

static gboolean is_closer(ComposerEditor *ed, gunichar c)
{
    const char *p = ed->lang->auto_pairs;
    for (; p && p[0] && p[1]; p += 2)
        if ((gunichar)p[1] == c)
            return TRUE;
    return FALSE;
}

static gboolean handle_backspace(ComposerEditor *ed)
{
    if (gtk_text_buffer_get_has_selection(ed->buffer))
        return FALSE;
    GtkTextIter cur;
    get_cursor(ed, &cur);
    if (gtk_text_iter_starts_line(&cur))
        return FALSE;

    /* empty pair: (|)  ->  | */
    gunichar prev = char_before(&cur), next = gtk_text_iter_get_char(&cur);
    const char *closer = pair_closer(ed, prev);
    if (closer && (gunichar)*closer == next) {
        GtkTextIter a = cur, b = cur;
        gtk_text_iter_backward_char(&a);
        gtk_text_iter_forward_char(&b);
        gtk_text_buffer_delete(ed->buffer, &a, &b);
        return TRUE;
    }

    /* in leading spaces: delete back to the previous indent stop */
    if (ed->lang->indent[0] == ' ' && prev == ' ' && only_space_before(&cur)) {
        int col = gtk_text_iter_get_line_offset(&cur), w = indent_width(ed);
        int n = col % w ? col % w : w;
        GtkTextIter a = cur;
        gtk_text_iter_backward_chars(&a, n);
        gtk_text_buffer_delete(ed->buffer, &a, &cur);
        return TRUE;
    }
    return FALSE;
}

/* Auto-closing pairs, typing over closers, dedenting '}'. */
static gboolean handle_char(ComposerEditor *ed, gunichar ch)
{
    if (ch == 0 || ch > 127)
        return FALSE;
    GtkTextBuffer *buf = ed->buffer;
    GtkTextIter cur, s, e;
    get_cursor(ed, &cur);
    gunichar next = gtk_text_iter_get_char(&cur), prev = char_before(&cur);

    /* '}' on an otherwise empty line: remove one indent level first */
    if (ed->lang->block_close && ch == (gunichar)ed->lang->block_close &&
        only_space_before(&cur) && !gtk_text_iter_starts_line(&cur) && next != ch) {
        GtkTextIter a = cur;
        int col = gtk_text_iter_get_line_offset(&cur);
        gtk_text_iter_backward_chars(&a, MIN(col, indent_width(ed)));
        gtk_text_buffer_begin_user_action(buf);
        gtk_text_buffer_delete(buf, &a, &cur);
        gtk_text_buffer_insert(buf, &cur, (char[]){ (char)ch, 0 }, 1);
        gtk_text_buffer_end_user_action(buf);
        return TRUE;
    }

    /* type over an auto-inserted closer */
    if (is_closer(ed, ch) && next == ch && !gtk_text_buffer_get_has_selection(buf)) {
        gtk_text_iter_forward_char(&cur);
        gtk_text_buffer_place_cursor(buf, &cur);
        return TRUE;
    }

    const char *closer = pair_closer(ed, ch);
    if (!closer)
        return FALSE;

    /* wrap a selection: (selected) */
    if (gtk_text_buffer_get_selection_bounds(buf, &s, &e)) {
        char *inner = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
        char *wrapped = g_strdup_printf("%c%s%c", (char)ch, inner, *closer);
        gtk_text_buffer_begin_user_action(buf);
        gtk_text_buffer_delete(buf, &s, &e);
        gtk_text_buffer_insert(buf, &s, wrapped, -1);
        gtk_text_buffer_end_user_action(buf);
        g_free(inner);
        g_free(wrapped);
        return TRUE;
    }

    /* only pair when the next character is "empty-ish" */
    gboolean quote = (gunichar)*closer == ch;
    gboolean room = next == 0 || g_unichar_isspace(next) || is_closer(ed, next) || next == ';' || next == ',';
    if (!room || (quote && (g_unichar_isalnum(prev) || prev == '_')))
        return FALSE;

    char pair[3] = { (char)ch, *closer, 0 };
    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_insert(buf, &cur, pair, 2);
    gtk_text_iter_backward_char(&cur);
    gtk_text_buffer_place_cursor(buf, &cur);
    gtk_text_buffer_end_user_action(buf);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Input                                                               */
/* ------------------------------------------------------------------ */

static gboolean on_key_pressed(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer data)
{
    (void)ctl; (void)keycode;
    ComposerEditor *ed = data;

    if (completion_handle_key(ed->completion, keyval, state))
        return TRUE;

    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_space) {
        ed->want_completion = ed->force_completion = TRUE;
        schedule_update(ed);
        return TRUE;
    }
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;

    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:    return handle_newline(ed);
    case GDK_KEY_Tab:          return handle_tab(ed, FALSE);
    case GDK_KEY_ISO_Left_Tab: return handle_tab(ed, TRUE);
    case GDK_KEY_BackSpace:    return handle_backspace(ed);
    case GDK_KEY_Escape:       return FALSE;
    default:                   return handle_char(ed, gdk_keyval_to_unicode(keyval));
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void)n;
    ComposerEditor *ed = data;
    GtkWidget *picked = gtk_widget_pick(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), x, y, GTK_PICK_DEFAULT);
    if (!completion_contains(ed->completion, picked))
        completion_hide(ed->completion);
}

static void on_focus_leave(GtkEventControllerFocus *f, gpointer data)
{
    (void)f;
    completion_hide(((ComposerEditor *)data)->completion);
}

static void on_scrolled(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    gtk_widget_queue_draw(((ComposerEditor *)data)->gutter);
}

/* ------------------------------------------------------------------ */
/*  Lifetime                                                            */
/* ------------------------------------------------------------------ */

static void editor_free(gpointer data)
{
    ComposerEditor *ed = data;
    if (ed->update_id)
        g_source_remove(ed->update_id);
    completion_free(ed->completion);
    symbols_free(ed->symbols);
    if (ed->tokens)
        g_array_unref(ed->tokens);
    g_free(ed->text);
    g_free(ed);
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    ComposerEditor *ed = data;
    g_signal_handlers_disconnect_by_data(ed->buffer, ed);
    if (ed->update_id) {
        g_source_remove(ed->update_id);
        ed->update_id = 0;
    }
}

ComposerEditor *composer_editor_new(const Language *lang)
{
    ComposerEditor *ed = g_new0(ComposerEditor, 1);
    ed->lang = lang;

    ed->buffer = gtk_text_buffer_new(NULL);
    ed->view   = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(ed->buffer));
    g_object_unref(ed->buffer);   /* owned by the view */

    gtk_text_view_set_monospace(ed->view, TRUE);
    gtk_text_view_set_wrap_mode(ed->view, GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(ed->view, 10);
    gtk_text_view_set_top_margin(ed->view, 6);
    gtk_text_view_set_bottom_margin(ed->view, 200);   /* let the last line scroll up */
    gtk_widget_add_css_class(GTK_WIDGET(ed->view), "composer-editor");

    theme_create_tags(ed->buffer, ed->tags);
    ed->bracket_tag = theme_create_bracket_tag(ed->buffer);

    ed->gutter = gtk_drawing_area_new();
    gtk_widget_add_css_class(ed->gutter, "composer-gutter");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ed->gutter), draw_gutter, ed, NULL);
    gtk_text_view_set_gutter(ed->view, GTK_TEXT_WINDOW_LEFT, ed->gutter);

    ed->scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ed->scroller), GTK_WIDGET(ed->view));
    gtk_widget_set_vexpand(ed->scroller, TRUE);
    gtk_widget_set_hexpand(ed->scroller, TRUE);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ed->scroller)),
                     "value-changed", G_CALLBACK(on_scrolled), ed);

    ed->completion = completion_new(ed->view);

    g_signal_connect(ed->buffer, "changed", G_CALLBACK(on_changed), ed);
    g_signal_connect_after(ed->buffer, "insert-text", G_CALLBACK(on_insert_text_after), ed);
    g_signal_connect(ed->buffer, "mark-set", G_CALLBACK(on_mark_set), ed);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view), keys);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view), GTK_EVENT_CONTROLLER(click));

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view), focus);

    g_signal_connect(ed->scroller, "destroy", G_CALLBACK(on_destroy), ed);
    g_object_set_data_full(G_OBJECT(ed->scroller), "composer-editor", ed, editor_free);

    ed->text_dirty = TRUE;
    schedule_update(ed);
    return ed;
}

GtkWidget     *composer_editor_widget(ComposerEditor *ed) { return ed->scroller; }
GtkTextBuffer *composer_editor_buffer(ComposerEditor *ed) { return ed->buffer; }
GtkTextView   *composer_editor_view(ComposerEditor *ed)   { return ed->view; }

void composer_editor_set_text(ComposerEditor *ed, const char *text)
{
    completion_hide(ed->completion);
    gtk_text_buffer_begin_irreversible_action(ed->buffer);
    gtk_text_buffer_set_text(ed->buffer, text, -1);
    gtk_text_buffer_end_irreversible_action(ed->buffer);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(ed->buffer, &start);
    gtk_text_buffer_place_cursor(ed->buffer, &start);
    gtk_text_buffer_set_modified(ed->buffer, FALSE);
}

char *composer_editor_get_text(ComposerEditor *ed)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(ed->buffer, &s, &e);
    return gtk_text_buffer_get_text(ed->buffer, &s, &e, TRUE);
}

void composer_editor_set_cursor_callback(ComposerEditor *ed, EditorCursorCallback cb, gpointer user_data)
{
    ed->cursor_cb = cb;
    ed->cursor_data = user_data;
}
