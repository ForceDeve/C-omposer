/*
 * sighelp.c — see sighelp.h.
 */
#include "sighelp.h"
#include "theme.h"

#include <string.h>

struct SigHelp {
    GtkTextView *view;
    GtkWidget   *popover;
    GtkWidget   *sig, *doc;
    int          paren_char;     /* where the '(' is, to point at it */
};

SigHelp *sighelp_new(GtkTextView *view)
{
    SigHelp *s = g_new0(SigHelp, 1);
    s->view = view;
    s->popover = gtk_popover_new();
    gtk_popover_set_autohide(GTK_POPOVER(s->popover), FALSE);   /* never takes the keyboard */
    gtk_popover_set_has_arrow(GTK_POPOVER(s->popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(s->popover), GTK_POS_TOP);
    gtk_widget_set_can_focus(s->popover, FALSE);
    gtk_widget_set_can_target(s->popover, FALSE);
    gtk_widget_add_css_class(s->popover, "composer-sighelp");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    s->sig = gtk_label_new(NULL);
    s->doc = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(s->sig), 0);
    gtk_label_set_xalign(GTK_LABEL(s->doc), 0);
    gtk_label_set_wrap(GTK_LABEL(s->doc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->doc), 70);
    gtk_widget_add_css_class(s->doc, "dim-label");
    gtk_box_append(GTK_BOX(box), s->sig);
    gtk_box_append(GTK_BOX(box), s->doc);
    gtk_popover_set_child(GTK_POPOVER(s->popover), box);
    gtk_widget_set_parent(s->popover, GTK_WIDGET(view));
    return s;
}

void sighelp_free(SigHelp *s)
{
    if (!s) return;
    gtk_widget_unparent(s->popover);
    g_free(s);
}

gboolean sighelp_visible(SigHelp *s) { return gtk_widget_get_visible(s->popover); }
void sighelp_hide(SigHelp *s) { if (gtk_widget_get_visible(s->popover)) gtk_popover_popdown(GTK_POPOVER(s->popover)); }
void sighelp_present(SigHelp *s) { if (gtk_widget_get_visible(s->popover)) gtk_popover_present(GTK_POPOVER(s->popover)); }

void sighelp_reposition(SigHelp *s)
{
    if (!gtk_widget_get_visible(s->popover)) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(gtk_text_view_get_buffer(s->view), &it, s->paren_char);
    GdkRectangle r;
    gtk_text_view_get_iter_location(s->view, &it, &r);
    int x, y;
    gtk_text_view_buffer_to_window_coords(s->view, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &x, &y);
    /* popovers centre on what they point at: point at a box as wide as the
     * popup, starting at the '(', so it opens rightwards and stays on screen */
    int nat = 0;
    gtk_widget_measure(s->popover, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &nat, NULL, NULL);
    int view_w = gtk_widget_get_width(GTK_WIDGET(s->view));
    if (x + nat > view_w - 8) x = MAX(view_w - 8 - nat, 0);
    GdkRectangle at = { x, y, MAX(nat, 1), r.height };
    gtk_popover_set_pointing_to(GTK_POPOVER(s->popover), &at);
}

static gboolean tok_is(const char *text, const Token *t, const char *s)
{
    return t->length == (int)strlen(s) && !strncmp(text + t->start, s, t->length);
}

/* Split "a, b(c, d), e" at top-level commas. */
static GPtrArray *split_params(const char *params)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    int depth = 0;
    const char *start = params;
    for (const char *p = params; ; p++) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        if (*p == ')' || *p == ']' || *p == '}') depth--;
        if (!*p || (*p == ',' && depth == 0)) {
            char *one = g_strstrip(g_strndup(start, p - start));
            if (*one || out->len) g_ptr_array_add(out, one); else g_free(one);
            if (!*p) break;
            start = p + 1;
        }
    }
    return out;
}

void sighelp_update(SigHelp *s, const Language *lang, const char *text, GArray *tokens,
                    int cursor_char, const src_function *funcs, int n_funcs)
{
    if (!tokens || !text) { sighelp_hide(s); return; }

    /* the last token that ends at or before the cursor */
    int i = -1;
    for (guint k = 0; k < tokens->len; k++) {
        const Token *t = &g_array_index(tokens, Token, k);
        if (t->char_start + t->char_length <= cursor_char) i = (int)k;
        else break;
    }

    /* walk back to the unclosed '(' of the call we're in, counting commas */
    int depth = 0, arg = 0, open = -1;
    for (int k = i, steps = 0; k >= 0 && steps < 4000; k--, steps++) {
        const Token *t = &g_array_index(tokens, Token, k);
        if (t->category != TOK_PUNCTUATION && t->category != TOK_OPERATOR) continue;
        if (tok_is(text, t, ")") || tok_is(text, t, "]")) depth++;
        else if (tok_is(text, t, "(") || tok_is(text, t, "[")) {
            if (depth == 0) {
                if (tok_is(text, t, "(")) open = k;
                break;
            }
            depth--;
        } else if (depth == 0 && tok_is(text, t, ",")) arg++;
        else if (depth == 0 && (tok_is(text, t, ";") || tok_is(text, t, "{") || tok_is(text, t, "}"))) break;
    }
    if (open < 1) { sighelp_hide(s); return; }

    const Token *paren = &g_array_index(tokens, Token, open);
    const Token *name = &g_array_index(tokens, Token, open - 1);
    char *fname = g_strndup(text + name->start, name->length);
    if (!g_ascii_isalpha(fname[0]) && fname[0] != '_') { g_free(fname); sighelp_hide(s); return; }

    /* user functions first, then the language's built-ins */
    char *ret = NULL, *params = NULL, *doc = NULL;
    for (int k = 0; k < n_funcs && !params; k++)
        if (!strcmp(funcs[k].name, fname)) {
            ret = g_strdup(funcs[k].ret_type);
            params = g_strdup(funcs[k].param_text);
        }
    if (!params) {
        const LangWord *w = language_lookup(lang, fname, strlen(fname));
        const char *detail = w ? w->detail : NULL;
        const char *lp = detail ? strchr(detail, '(') : NULL, *rp = detail ? strrchr(detail, ')') : NULL;
        if (w && w->category == TOK_BUILTIN && lp && rp > lp) {
            const char *sp = lp;                        /* return type: words before the name */
            while (sp > detail && sp[-1] != ' ' && sp[-1] != '*') sp--;
            ret = g_strstrip(g_strndup(detail, sp - detail));
            params = g_strndup(lp + 1, rp - lp - 1);
            doc = w->doc ? g_strdup(w->doc) : NULL;
        }
    }
    if (!params) { g_free(fname); sighelp_hide(s); return; }

    GPtrArray *ps = split_params(params);
    if (ps->len == 1 && !strcmp(ps->pdata[0], "void")) g_ptr_array_set_size(ps, 0);
    GString *m = g_string_new(NULL);
    char *e = g_markup_escape_text(ret && *ret ? ret : "", -1);
    if (*e) g_string_append_printf(m, "<span alpha=\"60%%\">%s</span> ", e);
    g_free(e);
    e = g_markup_escape_text(fname, -1);
    g_string_append_printf(m, "<b>%s</b>(", e);
    g_free(e);
    const char *accent = theme_get()->accent;
    for (guint k = 0; k < ps->len; k++) {
        const char *p = ps->pdata[k];
        gboolean variadic = !strcmp(p, "...");
        gboolean current = (int)k == arg || (variadic && arg >= (int)k);
        e = g_markup_escape_text(p, -1);
        if (k) g_string_append(m, ", ");
        if (current) g_string_append_printf(m, "<span foreground=\"%s\" weight=\"bold\" underline=\"single\">%s</span>", accent, e);
        else         g_string_append(m, e);
        g_free(e);
    }
    if (!ps->len) g_string_append(m, "<span alpha=\"60%\">no arguments</span>");
    g_string_append(m, ")");
    gtk_label_set_markup(GTK_LABEL(s->sig), m->str);
    gtk_label_set_text(GTK_LABEL(s->doc), doc ? doc : "");
    gtk_widget_set_visible(s->doc, doc && *doc);
    g_string_free(m, TRUE);
    g_ptr_array_unref(ps);
    g_free(fname); g_free(ret); g_free(params); g_free(doc);

    s->paren_char = paren->char_start;
    gboolean was = gtk_widget_get_visible(s->popover);
    if (!was) gtk_popover_popup(GTK_POPOVER(s->popover));
    sighelp_reposition(s);
}
