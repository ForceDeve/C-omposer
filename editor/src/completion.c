/*
 * completion.c — see completion.h.
 *
 * The popup is an overlay child of the GtkTextView (gtk_text_view_add_overlay),
 * so it scrolls with the text and never steals keyboard focus.
 */
#include "completion.h"

#include <string.h>

#define MAX_ITEMS      60
#define POPUP_MAX_H    240
#define POPUP_MIN_W    300
#define POPUP_MAX_W    560

typedef struct {
    char       *label;
    char       *insert;     /* text or snippet ($0 / \n / \t) */
    char       *detail;
    char       *doc;
    const char *glyph;      /* badge text, e.g. "♪" */
    const char *kind_css;   /* badge colour class suffix */
    int         score;
    int         rank;       /* tie-breaker: category priority */
} Item;

struct Completion {
    GtkTextView    *view;
    GtkWidget      *frame;      /* overlay root */
    GtkWidget      *scroller;
    GtkWidget      *list;
    GtkWidget      *doc;
    GPtrArray      *items;      /* Item* */
    GtkTextMark    *start_mark; /* start of the word being completed */
    const Language *lang;
    gboolean        forced;
    gboolean        inserting;
};

static void item_free(gpointer p)
{
    Item *it = p;
    g_free(it->label);
    g_free(it->insert);
    g_free(it->detail);
    g_free(it->doc);
    g_free(it);
}

static Item *item_new(const char *label, const char *insert, const char *detail, const char *doc,
                      const char *glyph, const char *kind_css, int rank)
{
    Item *it = g_new0(Item, 1);
    it->label    = g_strdup(label);
    it->insert   = g_strdup(insert ? insert : label);
    it->detail   = g_strdup(detail ? detail : "");
    it->doc      = (doc && *doc) ? g_strdup(doc) : NULL;
    it->glyph    = glyph;
    it->kind_css = kind_css;
    it->rank     = rank;
    return it;
}

/* Badge + sort rank per category. */
static void category_badge(TokenCategory cat, const char **glyph, int *rank)
{
    switch (cat) {
    case TOK_MUSIC_TYPE:    *glyph = "♪";  *rank = 0; break;
    case TOK_USER_TYPE:     *glyph = "S";  *rank = 1; break;
    case TOK_IDENTIFIER:    *glyph = "v";  *rank = 1; break;
    case TOK_FUNCTION:      *glyph = "ƒ";  *rank = 2; break;
    case TOK_BUILTIN:       *glyph = "ƒ";  *rank = 2; break;
    case TOK_MUSIC_KEYWORD: *glyph = "♫";  *rank = 3; break;
    case TOK_CONSTANT:      *glyph = "c";  *rank = 3; break;
    case TOK_TYPE:          *glyph = "T";  *rank = 4; break;
    case TOK_KEYWORD:       *glyph = "k";  *rank = 4; break;
    default:                *glyph = "·";  *rank = 5; break;
    }
}

/* ------------------------------------------------------------------ */
/*  Matching                                                            */
/* ------------------------------------------------------------------ */

/* 0 = exact-case prefix, 1 = any-case prefix, 2 = substring, -1 = no match */
static int match_score(const char *label, const char *prefix)
{
    size_t n = strlen(prefix);
    if (n == 0)
        return 0;
    if (strncmp(label, prefix, n) == 0)
        return 0;
    if (g_ascii_strncasecmp(label, prefix, n) == 0)
        return 1;
    if (n >= 2) {
        char *l = g_ascii_strdown(label, -1), *p = g_ascii_strdown(prefix, -1);
        gboolean hit = strstr(l, p) != NULL;
        g_free(l);
        g_free(p);
        if (hit)
            return 2;
    }
    return -1;
}

static int item_compare(gconstpointer a, gconstpointer b)
{
    const Item *x = *(Item *const *)a, *y = *(Item *const *)b;
    if (x->score != y->score) return x->score - y->score;
    if (x->rank != y->rank)   return x->rank - y->rank;
    size_t lx = strlen(x->label), ly = strlen(y->label);
    if (lx != ly)             return (int)lx - (int)ly;
    return strcmp(x->label, y->label);
}

static void add_if_match(GPtrArray *items, GHashTable *seen, Item *it, const char *prefix)
{
    int score = match_score(it->label, prefix);
    if (score < 0 || g_hash_table_contains(seen, it->label)) {
        item_free(it);
        return;
    }
    it->score = score;
    g_hash_table_add(seen, it->label);
    g_ptr_array_add(items, it);
}

/* ------------------------------------------------------------------ */
/*  Member access: resolve the type left of '.' / '->'                  */
/* ------------------------------------------------------------------ */

/* "const struct foo *" -> "foo" */
static char *base_type(const char *type)
{
    if (!type)
        return NULL;
    char *result = NULL;
    const char *p = type;
    while (*p) {
        if (g_ascii_isalpha(*p) || *p == '_') {
            const char *s = p;
            while (g_ascii_isalnum(*p) || *p == '_') p++;
            char *w = g_strndup(s, p - s);
            if (strcmp(w, "const") && strcmp(w, "struct") && strcmp(w, "union") &&
                strcmp(w, "enum") && strcmp(w, "volatile")) {
                g_free(result);
                result = w;
            } else {
                g_free(w);
            }
        } else {
            p++;
        }
    }
    return result;
}

typedef struct {
    char    *name;
    gboolean call;   /* name(...) */
} Segment;

/* Does `s[0..*end)` end with one of the member-access operators?  Strips it. */
static gboolean strip_accessor(const Language *lang, const char *s, int *end)
{
    for (const char *const *op = lang->member_access; op && *op; op++) {
        int n = (int)strlen(*op);
        if (*end >= n && strncmp(s + *end - n, *op, n) == 0) {
            *end -= n;
            return TRUE;
        }
    }
    return FALSE;
}

static int skip_back_balanced(const char *s, int end, char open, char close)
{
    int depth = 0;
    while (end > 0) {
        char c = s[--end];
        if (c == close) depth++;
        else if (c == open && --depth == 0) return end;
    }
    return -1;
}

/* Parse "a.b[1]->c(x)." backwards from `end` into segments (outermost first). */
static GArray *parse_chain(const Language *lang, const char *s, int end)
{
    GArray *chain = g_array_new(FALSE, TRUE, sizeof(Segment));
    while (strip_accessor(lang, s, &end)) {
        while (end > 0 && s[end - 1] == ' ') end--;
        gboolean call = FALSE;
        while (end > 0 && (s[end - 1] == ']' || s[end - 1] == ')')) {
            if (s[end - 1] == ')') call = TRUE;
            end = s[end - 1] == ']' ? skip_back_balanced(s, end, '[', ']')
                                    : skip_back_balanced(s, end, '(', ')');
            if (end < 0) goto fail;
        }
        int name_end = end;
        while (end > 0 && (g_ascii_isalnum(s[end - 1]) || s[end - 1] == '_')) end--;
        if (end == name_end) goto fail;
        Segment seg = { g_strndup(s + end, name_end - end), call };
        g_array_prepend_val(chain, seg);
    }
    if (chain->len > 0)
        return chain;
fail:
    for (guint i = 0; i < chain->len; i++)
        g_free(g_array_index(chain, Segment, i).name);
    g_array_free(chain, TRUE);
    return NULL;
}

/* Fields of `type`: user definitions win over the built-in ones. */
static gboolean type_members(const Language *lang, const SymbolTable *syms, const char *type,
                             const GPtrArray **user, const LangMember **builtin)
{
    *user = NULL;
    *builtin = NULL;
    const Symbol *s = symbols_lookup(syms, type);
    if (s && s->kind == SYM_TYPE && s->members) {
        *user = s->members;
        return TRUE;
    }
    const LangWord *w = language_lookup(lang, type, strlen(type));
    if (w && w->members) {
        *builtin = w->members;
        return TRUE;
    }
    return FALSE;
}

static char *member_type(const Language *lang, const SymbolTable *syms, const char *type, const char *field)
{
    const GPtrArray *user;
    const LangMember *builtin;
    if (!type_members(lang, syms, type, &user, &builtin))
        return NULL;
    if (user) {
        for (guint i = 0; i < user->len; i++) {
            const Symbol *m = g_ptr_array_index((GPtrArray *)user, i);
            if (strcmp(m->name, field) == 0)
                return base_type(m->type_name);
        }
    } else {
        for (const LangMember *m = builtin; m->name; m++)
            if (strcmp(m->name, field) == 0)
                return base_type(m->type);
    }
    return NULL;
}

/* Returns the type name the chain evaluates to, or NULL. */
static char *resolve_chain(const Language *lang, const SymbolTable *syms, GArray *chain)
{
    Segment *first = &g_array_index(chain, Segment, 0);
    char *type = NULL;

    const Symbol *s = symbols_lookup(syms, first->name);
    if (s && (s->kind == SYM_VARIABLE || s->kind == SYM_FUNCTION)) {
        type = base_type(s->type_name);
    } else {
        const LangWord *w = language_lookup(lang, first->name, strlen(first->name));
        if (w && w->type)
            type = base_type(w->type);
    }

    for (guint i = 1; type && i < chain->len; i++) {
        char *next = member_type(lang, syms, type, g_array_index(chain, Segment, i).name);
        g_free(type);
        type = next;
    }
    return type;
}

/* ------------------------------------------------------------------ */
/*  Collecting candidates                                               */
/* ------------------------------------------------------------------ */

static void collect_members(Completion *c, const SymbolTable *syms, const char *type, const char *prefix)
{
    const GPtrArray *user;
    const LangMember *builtin;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (type_members(c->lang, syms, type, &user, &builtin)) {
        if (user) {
            for (guint i = 0; i < user->len; i++) {
                const Symbol *m = g_ptr_array_index((GPtrArray *)user, i);
                add_if_match(c->items, seen, item_new(m->name, NULL, m->type_name, NULL, "m", "member", 0), prefix);
            }
        } else {
            for (const LangMember *m = builtin; m->name; m++)
                add_if_match(c->items, seen, item_new(m->name, NULL, m->type, m->doc, "m", "member", 0), prefix);
        }
    }
    g_hash_table_destroy(seen);
}

static void collect_words(Completion *c, const SymbolTable *syms, const char *prefix)
{
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

    /* document symbols first, so user redefinitions shadow built-ins */
    if (syms) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, syms->by_name);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            const Symbol *s = value;
            const char *glyph;
            int rank;
            Item *it = NULL;
            switch (s->kind) {
            case SYM_TYPE:
                category_badge(TOK_USER_TYPE, &glyph, &rank);
                it = item_new(s->name, NULL, "user type", NULL, glyph, "user-type", rank);
                break;
            case SYM_FUNCTION: {
                category_badge(TOK_FUNCTION, &glyph, &rank);
                char *ins = g_strdup_printf("%s($0)", s->name);
                char *det = g_strdup_printf("%s %s()", s->type_name ? s->type_name : "", s->name);
                it = item_new(s->name, ins, det, NULL, glyph, "function", rank);
                g_free(ins);
                g_free(det);
                break;
            }
            case SYM_VARIABLE:
                category_badge(TOK_IDENTIFIER, &glyph, &rank);
                it = item_new(s->name, NULL, s->type_name, NULL, glyph, "identifier", rank);
                break;
            case SYM_CONSTANT:
            case SYM_MACRO:
                category_badge(TOK_CONSTANT, &glyph, &rank);
                it = item_new(s->name, NULL, s->type_name, NULL, glyph, "constant", rank);
                break;
            }
            add_if_match(c->items, seen, it, prefix);
        }
    }

    for (size_t i = 0; i < c->lang->n_words; i++) {
        const LangWord *w = &c->lang->words[i];
        const char *glyph;
        int rank;
        category_badge(w->category, &glyph, &rank);
        add_if_match(c->items, seen,
                     item_new(w->word, w->snippet, w->detail, w->doc, glyph, token_category_name(w->category), rank),
                     prefix);
    }
    g_hash_table_destroy(seen);
}

/* ------------------------------------------------------------------ */
/*  UI                                                                  */
/* ------------------------------------------------------------------ */

static void clear_rows(Completion *c)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(c->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(c->list), child);
}

static GtkWidget *make_row(const Item *it, size_t prefix_len)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *kind = gtk_label_new(it->glyph);
    gtk_widget_add_css_class(kind, "kind");
    char *cls = g_strdup_printf("kind-%s", it->kind_css);
    gtk_widget_add_css_class(kind, cls);
    g_free(cls);
    gtk_box_append(GTK_BOX(box), kind);

    GtkWidget *name = gtk_label_new(NULL);
    if (it->score <= 1 && prefix_len > 0 && prefix_len <= strlen(it->label)) {
        char *head = g_strndup(it->label, prefix_len);
        char *markup = g_markup_printf_escaped("<b>%s</b>%s", head, it->label + prefix_len);
        gtk_label_set_markup(GTK_LABEL(name), markup);
        g_free(markup);
        g_free(head);
    } else {
        gtk_label_set_text(GTK_LABEL(name), it->label);
    }
    gtk_widget_add_css_class(name, "name");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(box), name);

    GtkWidget *detail = gtk_label_new(it->detail);
    gtk_widget_add_css_class(detail, "detail");
    gtk_label_set_xalign(GTK_LABEL(detail), 1);
    gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(detail), 36);
    gtk_box_append(GTK_BOX(box), detail);
    return box;
}

static int selected_index(Completion *c)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(c->list));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

static void ensure_visible(Completion *c, GtkListBoxRow *row)
{
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), c->list, &b))
        return;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->scroller));
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (b.origin.y < v)
        gtk_adjustment_set_value(adj, b.origin.y);
    else if (b.origin.y + b.size.height > v + page)
        gtk_adjustment_set_value(adj, b.origin.y + b.size.height - page);
}

static void select_index(Completion *c, int index)
{
    int n = (int)c->items->len;
    if (n == 0)
        return;
    index = CLAMP(index, 0, n - 1);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(c->list), index);
    gtk_list_box_select_row(GTK_LIST_BOX(c->list), row);
    ensure_visible(c, row);
}

static void on_row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    Completion *c = data;
    int i = row ? gtk_list_box_row_get_index(row) : -1;
    const Item *it = (i >= 0 && i < (int)c->items->len) ? g_ptr_array_index(c->items, i) : NULL;
    if (it && it->doc) {
        gtk_label_set_text(GTK_LABEL(c->doc), it->doc);
        gtk_widget_set_visible(c->doc, TRUE);
    } else {
        gtk_widget_set_visible(c->doc, FALSE);
    }
}

static void position_popup(Completion *c)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(c->view);
    GtkTextIter start;
    GdkRectangle loc, vis;
    gtk_text_buffer_get_iter_at_mark(buf, &start, c->start_mark);
    gtk_text_view_get_iter_location(c->view, &start, &loc);
    gtk_text_view_get_visible_rect(c->view, &vis);

    /* GtkTextView gives overlay children only their *minimum* size, so pin
     * the popup to its natural size explicitly. */
    int w, h, list_h;
    gtk_widget_set_size_request(c->frame, -1, -1);

    /* a short list needs no scrollbar (whose minimum height would leave a gap) */
    gtk_widget_measure(c->list, GTK_ORIENTATION_VERTICAL, -1, NULL, &list_h, NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroller), GTK_POLICY_NEVER,
                                   list_h > POPUP_MAX_H ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER);

    gtk_widget_measure(c->frame, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
    w = CLAMP(w, POPUP_MIN_W, POPUP_MAX_W);
    gtk_widget_measure(c->frame, GTK_ORIENTATION_VERTICAL, w, NULL, &h, NULL, NULL);
    gtk_widget_set_size_request(c->frame, w, h);

    int x = loc.x - 30;   /* line the label up with the word, past the badge */
    x = MIN(x, vis.x + vis.width - w - 4);
    x = MAX(x, vis.x);

    int y = loc.y + loc.height + 2;
    if (y + h > vis.y + vis.height && loc.y - h - 2 >= vis.y)
        y = loc.y - h - 2;   /* no room below: open upwards */

    gtk_text_view_move_overlay(c->view, c->frame, x, y);
}

/* ------------------------------------------------------------------ */
/*  Accepting                                                           */
/* ------------------------------------------------------------------ */

static void accept(Completion *c, int index)
{
    if (index < 0 || index >= (int)c->items->len)
        return;
    const Item *it = g_ptr_array_index(c->items, index);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(c->view);
    GtkTextIter start, end, line;

    gtk_text_buffer_get_iter_at_mark(buf, &start, c->start_mark);
    gtk_text_buffer_get_iter_at_mark(buf, &end, gtk_text_buffer_get_insert(buf));

    /* indentation of the current line, so multi-line snippets line up */
    line = start;
    gtk_text_iter_set_line_offset(&line, 0);
    GString *indent = g_string_new(NULL);
    for (GtkTextIter i = line; gtk_text_iter_compare(&i, &start) < 0; gtk_text_iter_forward_char(&i)) {
        gunichar ch = gtk_text_iter_get_char(&i);
        if (ch != ' ' && ch != '\t')
            break;
        g_string_append_unichar(indent, ch);
    }

    GString *text = g_string_new(NULL);
    long cursor = -1;
    for (const char *p = it->insert; *p; p++) {
        if (p[0] == '$' && p[1] == '0') {
            cursor = g_utf8_strlen(text->str, -1);
            p++;
        } else if (*p == '\n') {
            g_string_append_c(text, '\n');
            g_string_append(text, indent->str);
        } else if (*p == '\t') {
            g_string_append(text, c->lang ? c->lang->indent : "    ");
        } else {
            g_string_append_c(text, *p);
        }
    }

    c->inserting = TRUE;
    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_delete(buf, &start, &end);
    int offset = gtk_text_iter_get_offset(&start);
    gtk_text_buffer_insert(buf, &start, text->str, -1);
    if (cursor >= 0) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buf, &at, offset + (int)cursor);
        gtk_text_buffer_place_cursor(buf, &at);
    }
    gtk_text_buffer_end_user_action(buf);
    c->inserting = FALSE;

    g_string_free(indent, TRUE);
    g_string_free(text, TRUE);
    completion_hide(c);
    gtk_text_view_scroll_mark_onscreen(c->view, gtk_text_buffer_get_insert(buf));
    gtk_widget_grab_focus(GTK_WIDGET(c->view));
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    accept(data, gtk_list_box_row_get_index(row));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

Completion *completion_new(GtkTextView *view)
{
    Completion *c = g_new0(Completion, 1);
    c->view  = view;
    c->items = g_ptr_array_new_with_free_func(item_free);

    c->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(c->list), GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(c->list), TRUE);
    gtk_widget_set_focusable(c->list, FALSE);
    g_signal_connect(c->list, "row-selected", G_CALLBACK(on_row_selected), c);
    g_signal_connect(c->list, "row-activated", G_CALLBACK(on_row_activated), c);

    c->scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(c->scroller), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(c->scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(c->scroller), POPUP_MAX_H);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(c->scroller), POPUP_MIN_W);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(c->scroller), POPUP_MAX_W);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(c->scroller), c->list);

    c->doc = gtk_label_new(NULL);
    gtk_widget_add_css_class(c->doc, "doc");
    gtk_label_set_xalign(GTK_LABEL(c->doc), 0);
    gtk_label_set_wrap(GTK_LABEL(c->doc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(c->doc), 50);
    gtk_widget_set_visible(c->doc, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), c->scroller);
    gtk_box_append(GTK_BOX(box), c->doc);

    c->frame = box;
    gtk_widget_add_css_class(c->frame, "completion");
    gtk_widget_set_cursor_from_name(c->frame, "default");
    gtk_widget_set_visible(c->frame, FALSE);
    gtk_text_view_add_overlay(view, c->frame, 0, 0);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    c->start_mark = gtk_text_buffer_create_mark(buf, NULL, &start, TRUE);
    return c;
}

void completion_free(Completion *c)
{
    if (!c)
        return;
    g_ptr_array_unref(c->items);
    g_free(c);
}

gboolean completion_is_visible(Completion *c)   { return gtk_widget_get_visible(c->frame); }
gboolean completion_is_inserting(Completion *c) { return c->inserting; }

gboolean completion_contains(Completion *c, GtkWidget *widget)
{
    return widget && (widget == c->frame || gtk_widget_is_ancestor(widget, c->frame));
}

void completion_hide(Completion *c)
{
    gtk_widget_set_visible(c->frame, FALSE);
    c->forced = FALSE;
}

static gboolean is_word_char(gunichar ch) { return g_unichar_isalnum(ch) || ch == '_'; }

void completion_refresh(Completion *c, const Language *lang, const SymbolTable *symbols, gboolean force)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(c->view);
    c->lang = lang;
    force = force || (c->forced && completion_is_visible(c));

    if (gtk_text_buffer_get_has_selection(buf)) {
        completion_hide(c);
        return;
    }

    GtkTextIter cursor, start;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));

    /* never complete inside comments, strings or preprocessor lines */
    GtkTextIter before = cursor;
    if (gtk_text_iter_backward_char(&before)) {
        GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(buf);
        static const TokenCategory quiet[] = { TOK_COMMENT, TOK_STRING, TOK_CHAR, TOK_PREPROCESSOR };
        for (size_t i = 0; i < G_N_ELEMENTS(quiet); i++) {
            GtkTextTag *tag = gtk_text_tag_table_lookup(tt, token_category_name(quiet[i]));
            if (tag && gtk_text_iter_has_tag(&before, tag)) {
                completion_hide(c);
                return;
            }
        }
    }

    start = cursor;
    while (!gtk_text_iter_starts_line(&start)) {
        GtkTextIter prev = start;
        gtk_text_iter_backward_char(&prev);
        if (!is_word_char(gtk_text_iter_get_char(&prev)))
            break;
        start = prev;
    }
    char *prefix = gtk_text_iter_get_text(&start, &cursor);
    gtk_text_buffer_move_mark(buf, c->start_mark, &start);

    /* the text of the line up to the word, to detect "expr." / "expr->" */
    GtkTextIter line = start;
    gtk_text_iter_set_line_offset(&line, 0);
    char *head = gtk_text_iter_get_text(&line, &start);
    GArray *chain = parse_chain(lang, head, (int)strlen(head));

    g_ptr_array_set_size(c->items, 0);

    if (chain) {
        char *type = resolve_chain(lang, symbols, chain);
        if (type)
            collect_members(c, symbols, type, prefix);
        g_free(type);
        for (guint i = 0; i < chain->len; i++)
            g_free(g_array_index(chain, Segment, i).name);
        g_array_free(chain, TRUE);
    } else if ((force || (int)g_utf8_strlen(prefix, -1) >= lang->completion_min_prefix) &&
               !g_ascii_isdigit(prefix[0])) {
        collect_words(c, symbols, prefix);
    }
    g_free(head);

    g_ptr_array_sort(c->items, item_compare);
    if (c->items->len > MAX_ITEMS)
        g_ptr_array_set_size(c->items, MAX_ITEMS);

    /* nothing to offer, or the word is already complete */
    gboolean done = c->items->len == 0;
    if (!done && !force) {
        for (guint i = 0; i < c->items->len; i++) {
            const Item *it = g_ptr_array_index(c->items, i);
            if (strcmp(it->label, prefix) == 0 && strcmp(it->insert, it->label) == 0 &&
                c->items->len == 1)
                done = TRUE;
        }
    }
    if (done) {
        g_free(prefix);
        completion_hide(c);
        return;
    }

    clear_rows(c);
    size_t plen = strlen(prefix);
    for (guint i = 0; i < c->items->len; i++)
        gtk_list_box_append(GTK_LIST_BOX(c->list), make_row(g_ptr_array_index(c->items, i), plen));
    g_free(prefix);

    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->scroller)), 0);
    select_index(c, 0);
    if (force)
        c->forced = TRUE;
    gtk_widget_set_visible(c->frame, TRUE);
    position_popup(c);
}

gboolean completion_handle_key(Completion *c, guint keyval, GdkModifierType state)
{
    if (!completion_is_visible(c))
        return FALSE;
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return FALSE;

    int sel = selected_index(c), n = (int)c->items->len;
    switch (keyval) {
    case GDK_KEY_Up:        select_index(c, sel <= 0 ? n - 1 : sel - 1); return TRUE;
    case GDK_KEY_Down:      select_index(c, sel >= n - 1 ? 0 : sel + 1); return TRUE;
    case GDK_KEY_Page_Up:   select_index(c, sel - 8); return TRUE;
    case GDK_KEY_Page_Down: select_index(c, sel + 8); return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: {
        /* word already fully typed (e.g. "int"): let Enter be a newline */
        if (sel >= 0 && sel < n) {
            const Item *it = g_ptr_array_index(c->items, sel);
            GtkTextBuffer *buf = gtk_text_view_get_buffer(c->view);
            GtkTextIter s, e;
            gtk_text_buffer_get_iter_at_mark(buf, &s, c->start_mark);
            gtk_text_buffer_get_iter_at_mark(buf, &e, gtk_text_buffer_get_insert(buf));
            char *typed = gtk_text_iter_get_text(&s, &e);
            gboolean same = strcmp(typed, it->label) == 0 && strcmp(it->insert, it->label) == 0;
            g_free(typed);
            if (same) {
                completion_hide(c);
                return FALSE;
            }
        }
        accept(c, sel);
        return TRUE;
    }
    case GDK_KEY_Tab:       accept(c, sel); return TRUE;
    case GDK_KEY_Escape:    completion_hide(c); return TRUE;
    default:                return FALSE;
    }
}
