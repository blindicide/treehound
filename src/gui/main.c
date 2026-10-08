/* SPDX-License-Identifier: MIT */
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "treehound/common.h"
#include "treehound/ipc.h"
#include "treehound/json.h"
#include "treehound/strbuf.h"
#include "treehound/util.h"

typedef struct {
    int64_t id, parent, root, size;
    char *path, *type, *text[8];
    double fraction;
} Row;
typedef struct { int64_t root, parent, size; char *path; } Location;
typedef struct {
    GtkApplication *app;
    GtkWidget *window, *roots, *search, *status, *breadcrumb, *view;
    GtkWidget *hidden, *sensitive, *descending, *extension, *minimum, *maximum, *after, *before;
    GtkDropDown *sort, *types;
    GListStore *model;
    GtkSingleSelection *selection;
    char *socket, *path, *pending;
    int64_t root, parent, parent_size, offset;
    guint generation, pending_kind, smoke_stage;
    bool busy, closing, smoke, smoke_failed;
    GArray *history;
} Ui;
typedef struct { char *request, *socket; guint generation, kind; bool start; } Work;
static void query(Ui *u);
static void submit(Ui *u, char *request, guint kind);
static void row_free(gpointer data)
{
    Row *r = data;
    free(r->path); free(r->type);
    for (size_t i = 0; i < 8; i++) g_free(r->text[i]);
    g_free(r);
}
static Row *row_of(gpointer object) { return object ? g_object_get_data(G_OBJECT(object), "row") : NULL; }
static void work_free(gpointer data)
{
    Work *w = data; g_free(w->request); g_free(w->socket); g_free(w);
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel; Work *w = data; th_strbuf err; th_sb_init(&err);
    th_jval *res = th_ipc_call(w->socket, w->request, strlen(w->request), 3000, &err);
    if (!res && w->start) {
        char executable[4096]; ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
        if (n > 0) {
            executable[n] = 0; char *directory = g_path_get_dirname(executable);
            char *daemon = g_build_filename(directory, "treehoundd", NULL);
            char *args[] = {daemon, NULL};
            GError *spawn_error = NULL;
            if (g_spawn_async(NULL, args, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &spawn_error)) {
                for (int i = 0; i < 30 && !res; i++) {
                    g_usleep(100000); th_sb_reset(&err);
                    res = th_ipc_call(w->socket, w->request, strlen(w->request), 1000, &err);
                }
            }
            g_clear_error(&spawn_error); g_free(daemon); g_free(directory);
        }
    }
    if (!res) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err.data ? err.data : "Daemon unavailable");
    else g_task_return_pointer(task, res, (GDestroyNotify)th_json_free);
    th_sb_free(&err);
}
static void navigate(Ui *u, int64_t root, int64_t parent, const char *path, int64_t size)
{
    if (u->parent) {
        Location location = {u->root, u->parent, u->parent_size, th_xstrdup(u->path)};
        g_array_append_val(u->history, location);
    }
    u->root = root; u->parent = parent; u->parent_size = size; u->offset = 0;
    free(u->path); u->path = th_xstrdup(path);
    gtk_editable_set_text(GTK_EDITABLE(u->search), ""); query(u);
}
static void root_clicked(GtkButton *button, gpointer data)
{
    Ui *u = data; Row *r = row_of(button);
    navigate(u, r->root, r->id, r->path, r->size);
}
static void setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; guint column = GPOINTER_TO_UINT(data);
    GtkWidget *child;
    if (column == 3) { child = gtk_progress_bar_new(); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(child), true); }
    else {
        child = gtk_label_new(NULL); gtk_label_set_xalign(GTK_LABEL(child), column == 0 || column == 1 || column == 7 ? 0.0f : 1.0f);
        gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
    }
    gtk_list_item_set_child(item, child);
}
static void bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory; guint column = GPOINTER_TO_UINT(data);
    Row *r = row_of(gtk_list_item_get_item(item)); GtkWidget *child = gtk_list_item_get_child(item);
    if (column == 3) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(child), r->fraction);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(child), r->text[column]);
    } else gtk_label_set_text(GTK_LABEL(child), r->text[column]);
    gtk_widget_set_tooltip_text(child, r->text[column]);
}
static void populate(Ui *u, const th_jval *items)
{
    g_list_store_remove_all(u->model);
    if (!items || items->type != TH_JARR) return;
    for (size_t i = 0; i < items->n; i++) {
        const th_jval *v = &items->items[i]; Row *r = g_new0(Row, 1);
        r->id = th_json_get_int(v, "id", 0); r->parent = th_json_get_int(v, "parent_id", 0);
        r->root = th_json_get_int(v, "root_id", 0); r->size = th_json_get_int(v, "size", 0);
        r->path = th_xstrdup(th_json_get_str(v, "path", "")); r->type = th_xstrdup(th_json_get_str(v, "type", ""));
        r->text[0] = g_utf8_make_valid(th_json_get_str(v, "name", ""), -1);
        r->text[1] = g_utf8_make_valid(r->path, -1);
        r->text[2] = g_format_size((guint64)MAX(r->size, 0));
        r->fraction = u->parent_size > 0 ? CLAMP((double)r->size / (double)u->parent_size, 0., 1.) : 0.;
        r->text[3] = g_strdup_printf("%.1f%%", r->fraction * 100.);
        r->text[4] = g_format_size((guint64)MAX(th_json_get_int(v, "allocated", 0), 0));
        r->text[5] = g_strdup_printf("%lld", (long long)th_json_get_int(v, "files", 0));
        GDateTime *date = g_date_time_new_from_unix_local(th_json_get_int(v, "mtime", 0));
        r->text[6] = date ? g_date_time_format(date, "%Y-%m-%d %H:%M") : g_strdup("—");
        if (date) g_date_time_unref(date);
        r->text[7] = g_strdup_printf("%s · %s", r->type, th_json_get_str(v, "status", "indexed"));
        GObject *obj = g_object_new(G_TYPE_OBJECT, NULL); g_object_set_data_full(obj, "row", r, row_free);
        g_list_store_append(u->model, obj); g_object_unref(obj);
    }
}
static void received(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Ui *u = data; GTask *task = G_TASK(result); Work *w = g_task_get_task_data(task);
    GError *error = NULL; th_jval *res = g_task_propagate_pointer(task, &error);
    u->busy = false;
    if (!u->closing && w->generation == u->generation) {
        if (!res || !th_json_get_bool(res, "ok", false)) {
            gtk_label_set_text(GTK_LABEL(u->status), error ? error->message : th_json_get_str(res, "error", "Request failed"));
            if (u->smoke) u->smoke_failed = true;
        } else if (w->kind == 1) {
            const th_jval *roots = th_json_get(res, "roots");
            if (roots) for (size_t i = 0; i < roots->n; i++) {
                const th_jval *v = &roots->items[i]; Row *r = g_new0(Row, 1);
                r->id = th_json_get_int(v, "entry_id", 0); r->root = th_json_get_int(v, "id", 0);
                r->size = th_json_get_int(v, "size", 0); r->path = th_xstrdup(th_json_get_str(v, "path", ""));
                char *valid = g_utf8_make_valid(r->path, -1);
                char *label = g_strdup_printf("%s\n%s", valid, th_json_get_str(v, "status", "indexed"));
                GtkWidget *b = gtk_button_new_with_label(label); g_free(label); g_free(valid);
                g_object_set_data_full(G_OBJECT(b), "row", r, row_free);
                g_signal_connect(b, "clicked", G_CALLBACK(root_clicked), u); gtk_box_append(GTK_BOX(u->roots), b);
                if (i == 0) navigate(u, r->root, r->id, r->path, r->size);
            }
        } else if (w->kind == 2) {
            populate(u, th_json_get(res, "items"));
            char *text = g_strdup_printf("%u results on page %lld · %.1f ms · %s", g_list_model_get_n_items(G_LIST_MODEL(u->model)),
                (long long)(u->offset / 200 + 1), th_json_get_double(res, "elapsed_ms", 0),
                th_json_get_bool(res, "used_index", false) ? "trigram index" : "indexed records");
            gtk_label_set_text(GTK_LABEL(u->status), text); g_free(text);
            if (u->smoke) {
                if (!g_list_model_get_n_items(G_LIST_MODEL(u->model))) u->smoke_failed = true;
                u->smoke_stage++;
                if (u->smoke_stage == 1) gtk_editable_set_text(GTK_EDITABLE(u->search), "smoke");
                else if (u->smoke_stage == 2) g_application_quit(G_APPLICATION(u->app));
            }
        } else { gtk_label_set_text(GTK_LABEL(u->status), "Verification queued; refresh to view progress."); }
    }
    th_json_free(res); g_clear_error(&error);
    if (u->pending && !u->closing) {
        char *pending = u->pending; guint kind = u->pending_kind; u->pending = NULL;
        submit(u, pending, kind);
    }
}
static void submit(Ui *u, char *request, guint kind)
{
    u->generation++;
    if (u->busy) { g_free(u->pending); u->pending = request; u->pending_kind = kind; return; }
    Work *w = g_new0(Work, 1); w->request = request; w->socket = g_strdup(u->socket);
    w->generation = u->generation; w->kind = kind; w->start = kind == 1;
    GTask *task = g_task_new(u->app, NULL, received, u); g_task_set_task_data(task, w, work_free);
    u->busy = true; gtk_label_set_text(GTK_LABEL(u->status), "Loading indexed data…");
    g_task_run_in_thread(task, worker); g_object_unref(task);
}
static void query(Ui *u)
{
    if (!u->parent || u->closing) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(u->search));
    th_strbuf b; th_sb_init(&b); th_jw w; th_jw_init(&w, &b); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", 1); th_jw_kv_str(&w, "cmd", *text ? "search" : "list");
    th_jw_kv_str(&w, "query", text); th_jw_kv_int(&w, "root_id", u->root);
    if (*text) th_jw_kv_str(&w, "under", u->path); else th_jw_kv_int(&w, "parent_id", u->parent);
    th_jw_kv_int(&w, "limit", 200); th_jw_kv_int(&w, "offset", u->offset);
    const char *sorts[] = {"name", "size", "path", "mtime", "type"};
    const char *types[] = {"all", "file", "dir"};
    th_jw_kv_str(&w, "sort", sorts[gtk_drop_down_get_selected(u->sort)]);
    th_jw_kv_str(&w, "type", types[gtk_drop_down_get_selected(u->types)]);
    th_jw_kv_bool(&w, "descending", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->descending)));
    th_jw_kv_bool(&w, "show_hidden", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->hidden)));
    th_jw_kv_bool(&w, "case_sensitive", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->sensitive)));
    th_jw_kv_str(&w, "extension", gtk_editable_get_text(GTK_EDITABLE(u->extension)));
    const char *keys[] = {"min_size", "max_size", "min_mtime", "max_mtime"};
    GtkWidget *fields[] = {u->minimum, u->maximum, u->after, u->before};
    for (size_t i = 0; i < 4; i++) {
        const char *v = gtk_editable_get_text(GTK_EDITABLE(fields[i])); int64_t n;
        if (!*v) continue;
        if (!(i < 2 ? th_parse_size(v, &n) : th_parse_time(v, th_now(), &n))) {
            gtk_label_set_text(GTK_LABEL(u->status), "Invalid size or date filter"); th_sb_free(&b); return;
        }
        th_jw_kv_int(&w, keys[i], n);
    }
    th_jw_obj_end(&w);
    char *valid = g_utf8_make_valid(u->path, -1); gtk_label_set_text(GTK_LABEL(u->breadcrumb), valid); g_free(valid);
    submit(u, g_strdup(b.data), 2); th_sb_free(&b);
}
static void changed(GtkWidget *widget, gpointer data) { (void)widget; Ui *u = data; u->offset = 0; query(u); }
static void dropdown_changed(GObject *object, GParamSpec *param, gpointer data) { (void)object; (void)param; changed(NULL, data); }
static void refresh(GtkButton *button, gpointer data) { (void)button; query(data); }
static void page(GtkButton *button, gpointer data)
{
    Ui *u = data; int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    if (direction > 0 && g_list_model_get_n_items(G_LIST_MODEL(u->model)) < 200) return;
    u->offset = MAX(0, u->offset + direction * 200); query(u);
}
static void activate_row(GtkColumnView *view, guint position, gpointer data)
{
    (void)view; Ui *u = data; GObject *obj = g_list_model_get_item(G_LIST_MODEL(u->model), position); Row *r = row_of(obj);
    if (r && !strcmp(r->type, "dir")) navigate(u, r->root, r->id, r->path, r->size);
    else if (r) {
        GError *error = NULL; char *uri = g_filename_to_uri(r->path, NULL, &error);
        if (uri) g_app_info_launch_default_for_uri(uri, NULL, &error);
        if (error) gtk_label_set_text(GTK_LABEL(u->status), error->message);
        g_clear_error(&error); g_free(uri);
    }
    g_clear_object(&obj);
}
static void selected_action(GtkButton *button, gpointer data)
{
    Ui *u = data; Row *r = row_of(gtk_single_selection_get_selected_item(u->selection)); if (!r) return;
    const char *action = g_object_get_data(G_OBJECT(button), "action");
    if (!strcmp(action, "copy")) { gdk_clipboard_set_text(gtk_widget_get_clipboard(u->window), r->text[1]); return; }
    if (!strcmp(action, "open")) { activate_row(GTK_COLUMN_VIEW(u->view), gtk_single_selection_get_selected(u->selection), u); return; }
    char *dir = g_path_get_dirname(r->path); GError *error = NULL; char *uri = g_filename_to_uri(dir, NULL, &error);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, &error);
    if (error) gtk_label_set_text(GTK_LABEL(u->status), error->message);
    g_clear_error(&error); g_free(uri); g_free(dir);
}
static void back(GtkButton *button, gpointer data)
{
    (void)button; Ui *u = data; if (!u->history->len) return;
    Location location = g_array_index(u->history, Location, u->history->len - 1); g_array_set_size(u->history, u->history->len - 1);
    u->parent = location.parent; u->root = location.root; u->parent_size = location.size;
    free(u->path); u->path = location.path;
    u->offset = 0; gtk_editable_set_text(GTK_EDITABLE(u->search), ""); query(u);
}
static void verify(GtkButton *button, gpointer data)
{
    (void)button; Ui *u = data; char *request = g_strdup_printf("{\"v\":1,\"cmd\":\"verify\",\"root_id\":%lld}", (long long)u->root); submit(u, request, 3);
}
static GtkWidget *entry(GtkWidget *box, const char *placeholder, Ui *u)
{
    GtkWidget *w = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(w), placeholder); gtk_widget_set_size_request(w, 95, -1);
    gtk_box_append(GTK_BOX(box), w); g_signal_connect(w, "activate", G_CALLBACK(changed), u); return w;
}
static GtkWidget *button(GtkWidget *box, const char *text, GCallback callback, Ui *u)
{
    GtkWidget *b = gtk_button_new_with_label(text); gtk_box_append(GTK_BOX(box), b); g_signal_connect(b, "clicked", callback, u); return b;
}
static gboolean close_window(GtkWindow *window, gpointer data) { (void)window; Ui *u = data; u->closing = true; return FALSE; }
static gboolean smoke_timeout(gpointer data) { Ui *u = data; u->smoke_failed = true; g_application_quit(G_APPLICATION(u->app)); return G_SOURCE_REMOVE; }
static void activate(GtkApplication *app, gpointer data)
{
    Ui *u = data; u->window = gtk_application_window_new(app); gtk_window_set_title(GTK_WINDOW(u->window), "Treehound " TH_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(u->window), 1180, 720);
    g_signal_connect(u->window, "close-request", G_CALLBACK(close_window), u);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_window_set_child(GTK_WINDOW(u->window), box);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_append(GTK_BOX(box), toolbar);
    gtk_box_append(GTK_BOX(toolbar), gtk_label_new("Treehound " TH_VERSION));
    u->search = gtk_search_entry_new(); gtk_widget_set_hexpand(u->search, true); gtk_box_append(GTK_BOX(toolbar), u->search);
    g_signal_connect(u->search, "search-changed", G_CALLBACK(changed), u);
    button(toolbar, "Verify", G_CALLBACK(verify), u); button(toolbar, "Refresh", G_CALLBACK(refresh), u);
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), filters);
    const char *sorts[] = {"Name", "Size", "Path", "Modified", "Type", NULL};
    const char *types[] = {"All types", "Files", "Directories", NULL};
    u->sort = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sorts)); u->types = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    gtk_box_append(GTK_BOX(filters), GTK_WIDGET(u->sort)); gtk_box_append(GTK_BOX(filters), GTK_WIDGET(u->types));
    g_signal_connect(u->sort, "notify::selected", G_CALLBACK(dropdown_changed), u); g_signal_connect(u->types, "notify::selected", G_CALLBACK(dropdown_changed), u);
    u->descending = gtk_check_button_new_with_label("Descending"); u->hidden = gtk_check_button_new_with_label("Hidden"); u->sensitive = gtk_check_button_new_with_label("Case sensitive");
    GtkWidget *checks[] = {u->descending, u->hidden, u->sensitive};
    for (size_t i = 0; i < 3; i++) { gtk_box_append(GTK_BOX(filters), checks[i]); g_signal_connect(checks[i], "toggled", G_CALLBACK(changed), u); }
    GtkWidget *advanced = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); GtkWidget *expander = gtk_expander_new("Search filters (Enter to apply)");
    gtk_expander_set_child(GTK_EXPANDER(expander), advanced); gtk_box_append(GTK_BOX(box), expander);
    u->extension = entry(advanced, "Extension", u); u->minimum = entry(advanced, "Min size", u); u->maximum = entry(advanced, "Max size", u);
    u->after = entry(advanced, "After / 7d", u); u->before = entry(advanced, "Before", u);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), nav); button(nav, "Back", G_CALLBACK(back), u);
    u->breadcrumb = gtk_label_new("Indexed roots"); gtk_label_set_ellipsize(GTK_LABEL(u->breadcrumb), PANGO_ELLIPSIZE_START); gtk_label_set_xalign(GTK_LABEL(u->breadcrumb), 0);
    gtk_widget_set_hexpand(u->breadcrumb, true); gtk_box_append(GTK_BOX(nav), u->breadcrumb);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_widget_set_vexpand(paned, true); gtk_box_append(GTK_BOX(box), paned);
    u->roots = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_widget_set_size_request(u->roots, 205, -1); gtk_paned_set_start_child(GTK_PANED(paned), u->roots);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), false);
    u->model = g_list_store_new(G_TYPE_OBJECT); u->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(u->model)));
    u->view = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(u->selection))); gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(u->view), true);
    g_signal_connect(u->view, "activate", G_CALLBACK(activate_row), u);
    const char *columns[] = {"Name", "Path", "Logical", "% parent", "Allocated", "Files", "Modified", "Type · Status"};
    for (guint i = 0; i < 8; i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup", G_CALLBACK(setup), GUINT_TO_POINTER(i)); g_signal_connect(factory, "bind", G_CALLBACK(bind), GUINT_TO_POINTER(i));
        GtkColumnViewColumn *c = gtk_column_view_column_new(columns[i], factory); gtk_column_view_column_set_resizable(c, true);
        gtk_column_view_column_set_fixed_width(c, i == 0 ? 170 : i == 1 ? 220 : i == 6 ? 145 : 100);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(u->view), c); g_object_unref(c);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), u->view); gtk_paned_set_end_child(GTK_PANED(paned), scroll);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), footer);
    GtkWidget *prev = button(footer, "Previous", G_CALLBACK(page), u); g_object_set_data(G_OBJECT(prev), "direction", GINT_TO_POINTER(-1));
    GtkWidget *next = button(footer, "Next", G_CALLBACK(page), u); g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
    const char *actions[] = {"open", "folder", "copy"}; const char *labels[] = {"Open", "Open folder", "Copy path"};
    for (size_t i = 0; i < 3; i++) { GtkWidget *b = button(footer, labels[i], G_CALLBACK(selected_action), u); g_object_set_data(G_OBJECT(b), "action", (gpointer)actions[i]); }
    u->status = gtk_label_new("Connecting…"); gtk_label_set_xalign(GTK_LABEL(u->status), 0); gtk_box_append(GTK_BOX(box), u->status);
    gtk_window_present(GTK_WINDOW(u->window)); submit(u, g_strdup("{\"v\":1,\"cmd\":\"roots\"}"), 1);
    if (u->smoke) g_timeout_add_seconds(10, smoke_timeout, u);
}
static void ui_free(gpointer data)
{
    Ui *u = data;
    for (guint i = 0; i < u->history->len; i++) free(g_array_index(u->history, Location, i).path);
    g_clear_object(&u->model); g_clear_object(&u->selection); g_array_unref(u->history);
    free(u->socket); free(u->path); g_free(u->pending); g_free(u);
}
int th_gui_run(int argc, char **argv)
{
    Ui *u = g_new0(Ui, 1); u->socket = th_socket_path(); u->history = g_array_new(false, false, sizeof(Location));
    u->smoke = g_getenv("TREEHOUND_GUI_SMOKE") != NULL;
    GtkApplication *app = gtk_application_new("io.github.blindicide.treehound", G_APPLICATION_NON_UNIQUE); u->app = app;
    g_object_set_data_full(G_OBJECT(app), "ui", u, ui_free); g_signal_connect(app, "activate", G_CALLBACK(activate), u);
    int result = g_application_run(G_APPLICATION(app), argc, argv); if (u->smoke && (u->smoke_failed || u->smoke_stage != 2)) result = 4;
    u->closing = true; g_object_unref(app); return result;
}
