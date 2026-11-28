/*
 * sidebar.c — see sidebar.h.
 */
#include "sidebar.h"

#include <string.h>

#define ATTRS "standard::name,standard::display-name,standard::type,standard::is-hidden"

struct Sidebar {
    SidebarCallbacks cb;
    GtkWidget  *root;          /* the whole sidebar */
    GtkWidget  *title;
    GtkWidget  *stack;         /* "tree" | "empty" */
    GtkWidget  *list;
    char       *project;
    GFile      *menu_file;     /* the file the context menu is for */
    GtkWidget  *menu;
};

static void show_project(Sidebar *s, const char *dir);

static const char *audio_ext[] = { ".wav", ".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aiff", ".aif", NULL };
static const char *code_ext[]  = { ".cmp", ".c", ".h", ".txt", ".md", ".ini", NULL };

static gboolean has_ext(const char *name, const char **exts)
{
    char *low = g_ascii_strdown(name, -1);
    gboolean yes = FALSE;
    for (int i = 0; exts[i] && !yes; i++) yes = g_str_has_suffix(low, exts[i]);
    g_free(low);
    return yes;
}

gboolean sidebar_is_audio(const char *name) { return has_ext(name, audio_ext); }
gboolean sidebar_is_code(const char *name)  { return has_ext(name, code_ext); }

static GFile *info_file(GFileInfo *info)
{
    return G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
}

char *sidebar_relative_path(Sidebar *s, GFile *file)
{
    if (s->project) {
        GFile *root = g_file_new_for_path(s->project);
        char *rel = g_file_get_relative_path(root, file);
        g_object_unref(root);
        if (rel) return rel;
    }
    return g_file_get_path(file);
}

/* ------------------------------------------------------------------ */
/*  The tree model: directory lists, hidden files out, folders first   */
/* ------------------------------------------------------------------ */

static gboolean not_hidden(gpointer item, gpointer data)
{
    (void)data;
    GFileInfo *info = item;
    const char *name = g_file_info_get_name(info);
    return !g_file_info_get_is_hidden(info) && name && name[0] != '.';
}

static int folders_first(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    GFileInfo *x = (GFileInfo *)a, *y = (GFileInfo *)b;
    gboolean dx = g_file_info_get_file_type(x) == G_FILE_TYPE_DIRECTORY;
    gboolean dy = g_file_info_get_file_type(y) == G_FILE_TYPE_DIRECTORY;
    if (dx != dy) return dx ? -1 : 1;
    return g_utf8_collate(g_file_info_get_display_name(x), g_file_info_get_display_name(y));
}

static GListModel *dir_model(GFile *dir)
{
    GtkDirectoryList *dl = gtk_directory_list_new(ATTRS, dir);
    gtk_directory_list_set_monitored(dl, TRUE);          /* follows changes on disk */
    GtkFilterListModel *f = gtk_filter_list_model_new(G_LIST_MODEL(dl),
        GTK_FILTER(gtk_custom_filter_new(not_hidden, NULL, NULL)));
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(f),
        GTK_SORTER(gtk_custom_sorter_new(folders_first, NULL, NULL)));
    return G_LIST_MODEL(sorted);
}

static GListModel *child_model(gpointer item, gpointer data)
{
    (void)data;
    GFileInfo *info = item;
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) return NULL;
    return dir_model(info_file(info));
}

/* ------------------------------------------------------------------ */
/*  Rows                                                               */
/* ------------------------------------------------------------------ */

static void open_menu_for(Sidebar *s, GFileInfo *info, GtkWidget *row, double x, double y);

static void row_right_click(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    (void)n;
    Sidebar *s = data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GFileInfo *info = g_object_get_data(G_OBJECT(row), "info");
    if (info) open_menu_for(s, info, row, x, y);
}

static GdkContentProvider *row_drag(GtkDragSource *src, double x, double y, gpointer data)
{
    (void)x; (void)y;
    Sidebar *s = data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(src));
    GFileInfo *info = g_object_get_data(G_OBJECT(row), "info");
    if (!info) return NULL;
    const char *name = g_file_info_get_name(info);
    char *rel = sidebar_relative_path(s, info_file(info));
    char *text = sidebar_is_audio(name) ? g_strdup_printf("load_sample(\"%s\")", rel) : g_strdup(rel);
    GdkContentProvider *p = gdk_content_provider_new_typed(G_TYPE_STRING, text);
    g_free(text); g_free(rel);
    return p;
}

static void setup_row(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f;
    Sidebar *s = data;
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(item, expander);

    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(row_right_click), s);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(rc));
    GtkDragSource *drag = gtk_drag_source_new();
    g_signal_connect(drag, "prepare", G_CALLBACK(row_drag), s);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(drag));
}

static void bind_row(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkTreeListRow *row = gtk_list_item_get_item(item);
    GFileInfo *info = gtk_tree_list_row_get_item(row);
    GtkWidget *expander = gtk_list_item_get_child(item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);
    const char *name = g_file_info_get_display_name(info);
    gboolean dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
        dir ? "folder-symbolic"
        : sidebar_is_audio(name) ? "audio-x-generic-symbolic"
        : g_str_has_suffix(name, ".cmp") ? "text-x-script-symbolic"
        : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(label), name);
    gtk_widget_set_tooltip_text(expander, sidebar_is_audio(name)
        ? "Double-click to preview · drag into the code for load_sample()" : NULL);
    g_object_set_data_full(G_OBJECT(expander), "info", info, g_object_unref);   /* get_item gave a ref */
}

static void unbind_row(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    g_object_set_data(G_OBJECT(gtk_list_item_get_child(item)), "info", NULL);
}

static void on_activate(GtkListView *lv, guint pos, gpointer data)
{
    Sidebar *s = data;
    GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(gtk_list_view_get_model(lv)), pos);
    if (!row) return;
    GFileInfo *info = gtk_tree_list_row_get_item(row);
    const char *name = g_file_info_get_name(info);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
    else if (sidebar_is_audio(name) && s->cb.play)
        s->cb.play(info_file(info), s->cb.data);
    else if (s->cb.open)
        s->cb.open(info_file(info), s->cb.data);
    g_object_unref(info);
    g_object_unref(row);
}

/* ------------------------------------------------------------------ */
/*  Context menu                                                       */
/* ------------------------------------------------------------------ */

static void act_open(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    Sidebar *s = data;
    if (!s->menu_file) return;
    char *name = g_file_get_basename(s->menu_file);
    if (sidebar_is_audio(name)) { if (s->cb.play) s->cb.play(s->menu_file, s->cb.data); }
    else if (s->cb.open) s->cb.open(s->menu_file, s->cb.data);
    g_free(name);
}

static void act_insert(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    Sidebar *s = data;
    if (!s->menu_file || !s->cb.insert) return;
    char *name = g_file_get_basename(s->menu_file);
    if (sidebar_is_audio(name)) {
        char *rel = sidebar_relative_path(s, s->menu_file);
        char *text = g_strdup_printf("load_sample(\"%s\")", rel);
        s->cb.insert(text, s->cb.data);
        g_free(text); g_free(rel);
    } else {
        char *contents = NULL;
        gsize len = 0;
        if (g_file_load_contents(s->menu_file, NULL, &contents, &len, NULL, NULL)
            && g_utf8_validate(contents, len, NULL))
            s->cb.insert(contents, s->cb.data);
        g_free(contents);
    }
    g_free(name);
}

static void act_reveal(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    Sidebar *s = data;
    if (!s->menu_file) return;
    GFile *parent = g_file_get_parent(s->menu_file);
    char *uri = g_file_get_uri(parent ? parent : s->menu_file);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_show_uri(GTK_WINDOW(gtk_widget_get_root(s->root)), uri, GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_free(uri);
    if (parent) g_object_unref(parent);
}

static void open_menu_for(Sidebar *s, GFileInfo *info, GtkWidget *row, double x, double y)
{
    g_set_object(&s->menu_file, info_file(info));
    const char *name = g_file_info_get_name(info);
    gboolean dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    GMenu *m = g_menu_new();
    if (!dir) {
        g_menu_append(m, sidebar_is_audio(name) ? "Play" : "Open", "side.open");
        g_menu_append(m, sidebar_is_audio(name) ? "Insert load__sample(\"…\")" : "Insert into the code", "side.insert");
    }
    g_menu_append(m, "Show in file manager", "side.reveal");
    if (s->menu) gtk_widget_unparent(s->menu);
    s->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    g_object_unref(m);
    gtk_widget_set_parent(s->menu, row);
    gtk_popover_set_has_arrow(GTK_POPOVER(s->menu), FALSE);
    GdkRectangle at = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(s->menu), &at);
    gtk_popover_popup(GTK_POPOVER(s->menu));
}

/* ------------------------------------------------------------------ */
/*  Building it                                                        */
/* ------------------------------------------------------------------ */

static void btn_new_project(GtkButton *b, gpointer data) { (void)b; Sidebar *s = data; if (s->cb.new_project) s->cb.new_project(s->cb.data); }
static void btn_open_project(GtkButton *b, gpointer data) { (void)b; Sidebar *s = data; if (s->cb.open_project) s->cb.open_project(s->cb.data); }

static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb, Sidebar *s)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, s);
    return b;
}

static void sidebar_free(gpointer data)
{
    Sidebar *s = data;
    g_free(s->project);
    g_clear_object(&s->menu_file);
    g_free(s);
}

Sidebar *sidebar_new(const SidebarCallbacks *cb)
{
    Sidebar *s = g_new0(Sidebar, 1);
    s->cb = *cb;
    s->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(s->root, "composer-sidebar");
    gtk_widget_set_size_request(s->root, 200, -1);
    g_object_set_data_full(G_OBJECT(s->root), "sidebar", s, sidebar_free);

    GSimpleActionGroup *ag = g_simple_action_group_new();
    static const GActionEntry entries[] = {
        { .name = "open", .activate = act_open },
        { .name = "insert", .activate = act_insert },
        { .name = "reveal", .activate = act_reveal },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(ag), entries, G_N_ELEMENTS(entries), s);
    gtk_widget_insert_action_group(s->root, "side", G_ACTION_GROUP(ag));
    g_object_unref(ag);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(head, "composer-sidebar-title");
    s->title = gtk_label_new("PROJECT");
    gtk_label_set_xalign(GTK_LABEL(s->title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(s->title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(s->title, TRUE);
    gtk_box_append(GTK_BOX(head), s->title);
    gtk_box_append(GTK_BOX(head), icon_button("folder-new-symbolic", "New project…", G_CALLBACK(btn_new_project), s));
    gtk_box_append(GTK_BOX(head), icon_button("document-open-symbolic", "Open a project folder…", G_CALLBACK(btn_open_project), s));
    gtk_box_append(GTK_BOX(s->root), head);

    s->stack = gtk_stack_new();
    gtk_widget_set_vexpand(s->stack, TRUE);

    /* empty state */
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(empty, 16); gtk_widget_set_margin_end(empty, 16);
    GtkWidget *msg = gtk_label_new("No project open.\nA project is a folder for your songs, code snippets and samples.");
    gtk_label_set_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_justify(GTK_LABEL(msg), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(msg, "dim-label");
    GtkWidget *np = gtk_button_new_with_label("New project…");
    GtkWidget *op = gtk_button_new_with_label("Open folder…");
    g_signal_connect(np, "clicked", G_CALLBACK(btn_new_project), s);
    g_signal_connect(op, "clicked", G_CALLBACK(btn_open_project), s);
    gtk_box_append(GTK_BOX(empty), msg);
    gtk_box_append(GTK_BOX(empty), np);
    gtk_box_append(GTK_BOX(empty), op);
    gtk_stack_add_named(GTK_STACK(s->stack), empty, "empty");

    GtkSignalListItemFactory *f = GTK_SIGNAL_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
    g_signal_connect(f, "setup", G_CALLBACK(setup_row), s);
    g_signal_connect(f, "bind", G_CALLBACK(bind_row), s);
    g_signal_connect(f, "unbind", G_CALLBACK(unbind_row), s);
    s->list = gtk_list_view_new(NULL, GTK_LIST_ITEM_FACTORY(f));
    g_signal_connect(s->list, "activate", G_CALLBACK(on_activate), s);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s->list);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_stack_add_named(GTK_STACK(s->stack), scroll, "tree");

    gtk_box_append(GTK_BOX(s->root), s->stack);
    GtkWidget *hint = gtk_label_new("double-click: open / play · right-click: insert");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_add_css_class(hint, "composer-sidebar-title");
    gtk_label_set_ellipsize(GTK_LABEL(hint), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(s->root), hint);

    show_project(s, NULL);                 /* initial state: nothing to announce */
    return s;
}

GtkWidget *sidebar_widget(Sidebar *s) { return s->root; }
const char *sidebar_project(Sidebar *s) { return s->project; }

void sidebar_set_project(Sidebar *s, const char *dir)
{
    show_project(s, dir);
    if (s->cb.project_changed) s->cb.project_changed(s->project, s->cb.data);
}

static void show_project(Sidebar *s, const char *dir)
{
    if (dir && !g_file_test(dir, G_FILE_TEST_IS_DIR)) dir = NULL;
    if (dir != s->project) {
        char *copy = g_strdup(dir);
        g_free(s->project);
        s->project = copy;
    }
    if (!s->project) {
        gtk_label_set_text(GTK_LABEL(s->title), "PROJECT");
        gtk_list_view_set_model(GTK_LIST_VIEW(s->list), NULL);
        gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "empty");
    } else {
        char *base = g_path_get_basename(s->project);
        char *up = g_utf8_strup(base, -1);
        gtk_label_set_text(GTK_LABEL(s->title), up);
        gtk_widget_set_tooltip_text(s->title, s->project);
        g_free(up); g_free(base);
        GFile *root = g_file_new_for_path(s->project);
        GtkTreeListModel *tree = gtk_tree_list_model_new(dir_model(root), FALSE, FALSE, child_model, NULL, NULL);
        g_object_unref(root);
        GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(tree));
        gtk_single_selection_set_autoselect(sel, FALSE);
        gtk_list_view_set_model(GTK_LIST_VIEW(s->list), GTK_SELECTION_MODEL(sel));
        g_object_unref(sel);
        gtk_stack_set_visible_child_name(GTK_STACK(s->stack), "tree");
    }
}
