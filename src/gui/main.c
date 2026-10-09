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
#include "treehound/treemap.h"

typedef struct {
    int64_t id, parent, root, size;
    char *path, *type, *text[8];
    double fraction;
} Row;
typedef struct { Row *row; double weight; th_rect rect; } Tile;
typedef struct { int64_t root, parent, size; char *path; } Location;
typedef struct {
    GtkApplication *app;
    GtkWidget *window, *roots, *bookmarks, *mounts, *search, *status, *scan_label, *scan_progress, *breadcrumb, *view, *settings, *config_text;
    GtkWidget *hidden, *sensitive, *descending, *extension, *minimum, *maximum, *after, *before, *exact, *global;
    GtkDropDown *sort, *types, *metric;
    GtkWidget *stack, *map, *history_chart, *history_text; GArray *tiles, *snapshots;
    GListStore *model;
    GtkSingleSelection *selection;
    char *socket, *path, *pending;
    int64_t root, parent, parent_size, offset;
    guint generation, pending_kind, smoke_stage, retry_source, smoke_source, scan_pulse_source;
    bool busy, closing, smoke, smoke_failed, benchmark, mounts_loaded;
    GPtrArray *saved_paths;
    GHashTable *observed_first_scans, *completed_first_scans;
    GQueue *actions;
    double launched, mapped;
    GArray *history;
} Ui;
typedef struct { char *request, *socket; guint generation, kind; bool start; } Work;
static void query(Ui *u);
static void show_settings(Ui *u, const char *text);
static void submit(Ui *u, char *request, guint kind);
static void start_work(Ui *u, Work *w);
static bool mutation(guint kind) { return kind == 3 || kind == 5 || kind == 9; }
static void preferences(Ui *u, bool save);
static void path_clicked(GtkButton *button, gpointer data)
{
    Ui *u=data; const char *path=g_object_get_data(G_OBJECT(button),"path");
    th_strbuf b;th_sb_init(&b);th_jw w;th_jw_init(&w,&b);th_jw_obj_begin(&w);
    th_jw_kv_int(&w,"v",1);th_jw_kv_str(&w,"cmd","search");th_jw_kv_bytes(&w,"query",path,strlen(path));
    th_jw_kv_bool(&w,"exact",true);th_jw_kv_str(&w,"type","dir");th_jw_kv_int(&w,"limit",1);th_jw_obj_end(&w);
    submit(u,g_strdup(b.data),6);th_sb_free(&b);
}
static GtkWidget *path_button(GtkWidget *box,const char *path,const char *suffix,Ui *u)
{
    char *valid=g_utf8_make_valid(path,-1),*label=g_strdup_printf("%s%s%s",valid,*suffix?" · ":"",suffix);
    GtkWidget *b=gtk_button_new_with_label(label);GtkLabel *caption=GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b)));
    gtk_label_set_ellipsize(caption,PANGO_ELLIPSIZE_MIDDLE);gtk_label_set_max_width_chars(caption,24);gtk_widget_set_tooltip_text(b,label);
    g_object_set_data_full(G_OBJECT(b),"path",g_strdup(path),g_free);g_signal_connect(b,"clicked",G_CALLBACK(path_clicked),u);
    gtk_box_append(GTK_BOX(box),b);g_free(valid);g_free(label);return b;
}
static void bookmarks_refresh(Ui *u)
{
    GtkWidget *child;while((child=gtk_widget_get_first_child(u->bookmarks)))gtk_box_remove(GTK_BOX(u->bookmarks),child);
    for(guint i=0;i<u->saved_paths->len;i++)path_button(u->bookmarks,g_ptr_array_index(u->saved_paths,i),"",u);
}
static void bookmark_clicked(GtkButton *button,gpointer data)
{
    (void)button;Ui *u=data;if(!u->path)return;
    guint i;for(i=0;i<u->saved_paths->len;i++)if(!strcmp(g_ptr_array_index(u->saved_paths,i),u->path))break;
    if(i<u->saved_paths->len)g_ptr_array_remove_index(u->saved_paths,i);
    else if(u->saved_paths->len<64)g_ptr_array_add(u->saved_paths,g_strdup(u->path));
    else {gtk_label_set_text(GTK_LABEL(u->status),"Bookmark limit reached (64)");return;}
    bookmarks_refresh(u);preferences(u,true);
}
static gboolean retry_roots(gpointer data) { Ui *u = data; u->retry_source = 0; if (!u->closing) submit(u, g_strdup("{\"v\":1,\"cmd\":\"roots\"}"), 1); return G_SOURCE_REMOVE; }
static gboolean pulse_scan(gpointer data)
{
    Ui *u = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(u->scan_progress));
    return G_SOURCE_CONTINUE;
}
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
    if (res && w->kind == 9 && th_json_get_bool(res,"ok",false)) {
        int64_t seq = th_json_get_int(res,"seq",0);
        double deadline=th_mono_sec()+120.; bool complete=false;
        while (th_mono_sec()<deadline) {
            th_jval *status = th_ipc_call(w->socket,"{\"v\":1,\"cmd\":\"status\"}",strlen("{\"v\":1,\"cmd\":\"status\"}"),1000,&err);
            complete = status && th_json_get_int(status,"completed_seq",0)>=seq;
            th_json_free(status); if (complete) break; g_usleep(100000);
        }
        if (!complete) { th_json_free(res); res=NULL; th_sb_reset(&err); th_sb_puts(&err,"Snapshot is still queued; refresh History after verification completes"); }
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
        r->text[5] = g_strdup_printf("%lld", (long long)(!strcmp(r->type, "dir") ? th_json_get_int(v, "files", 0) : 1));
        GDateTime *date = g_date_time_new_from_unix_local(th_json_get_int(v, "mtime", 0));
        r->text[6] = date ? g_date_time_format(date, "%Y-%m-%d %H:%M") : g_strdup("—");
        if (date) g_date_time_unref(date);
        r->text[7] = g_strdup_printf("%s · %s", r->type, th_json_get_str(v, "status", "indexed"));
        GObject *obj = g_object_new(G_TYPE_OBJECT, NULL); g_object_set_data_full(obj, "row", r, row_free);
        g_list_store_append(u->model, obj); g_object_unref(obj);
    }
}
static void map_clear(Ui *u)
{
    for (guint i = 0; i < u->tiles->len; i++) row_free(g_array_index(u->tiles, Tile, i).row);
    g_array_set_size(u->tiles, 0);
}
static void map_populate(Ui *u, const th_jval *res)
{
    map_clear(u); const th_jval *items = th_json_get(res, "items");
    bool allocated = !strcmp(th_json_get_str(res, "metric", "allocated"), "allocated");
    int64_t other = th_json_get_int(res, "other_count", 0);
    double remainder = (double)th_json_get_int(res, "other_weight", 0), total = (double)th_json_get_int(res, "total_weight", 0);
    if (items) for (size_t i = 0; i < items->n; i++) {
        const th_jval *v = &items->items[i]; Row *r = g_new0(Row, 1);
        r->id = th_json_get_int(v, "id", 0); r->root = th_json_get_int(v, "root_id", 0);
        r->path = th_xstrdup(th_json_get_str(v, "path", "")); r->type = th_xstrdup(th_json_get_str(v, "type", ""));
        r->size = th_json_get_int(v, "size", 0); r->text[0] = g_utf8_make_valid(th_json_get_str(v, "name", ""), -1);
        double weight = (double)th_json_get_int(v, allocated ? "allocated" : "size", 0);
        if (strcmp(r->type, "dir") && (th_json_get_int(v, "flags", 0) & 8)) weight = 0;
        if (weight < total * .001) { other++; remainder += MAX(weight,0.); row_free(r); continue; }
        Tile tile = {.row = r, .weight = MAX(weight, 0.)}; g_array_append_val(u->tiles, tile);
    }
    if (other) {
        Row *r = g_new0(Row, 1); r->text[0] = g_strdup_printf("Other %lld items", (long long)other);
        r->path = th_xstrdup(u->path); r->type = th_xstrdup("aggregate");
        Tile tile = {.row = r, .weight = remainder}; g_array_append_val(u->tiles, tile);
    }
    char *size = g_format_size((guint64)MAX(th_json_get_int(res, "total_weight", 0), 0));
    char *text = g_strdup_printf("%lld children · %s %s · %s · up to 512 largest items; remainder aggregated",
        (long long)th_json_get_int(res, "children", 0), size, allocated ? "allocated" : "logical", th_json_get_str(res, "status", "indexed"));
    gtk_label_set_text(GTK_LABEL(u->status), text); g_free(size); g_free(text); gtk_widget_queue_draw(u->map);
}
static gboolean map_smoke_navigate(gpointer data);
static void map_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area; Ui *u = data; size_t n = u->tiles->len; double weights[513] = {0}; th_rect rectangles[513];
    for (size_t i = 0; i < n; i++) weights[i] = g_array_index(u->tiles, Tile, i).weight;
    th_treemap(weights, n, (th_rect){0,0,(double)width,(double)height}, rectangles);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        Tile *t = &g_array_index(u->tiles, Tile, i); t->rect = rectangles[i]; th_rect r = t->rect;
        if (r.width < 1 || r.height < 1) continue;
        any = true; const char *extension = strrchr(t->row->text[0], '.');
        guint hash = g_str_hash(extension ? extension : t->row->type);
        double tint = (double)(hash % 100) / 250.;
        if (!strcmp(t->row->type, "dir")) cairo_set_source_rgb(cr, .15, .38+tint, .65);
        else if (!t->row->id) cairo_set_source_rgb(cr, .45, .45, .45);
        else cairo_set_source_rgb(cr, .32+tint, .52, .28+tint);
        cairo_rectangle(cr, r.x+1, r.y+1, MAX(r.width-2,0), MAX(r.height-2,0)); cairo_fill(cr);
        if (r.width > 65 && r.height > 25) {
            cairo_save(cr); cairo_rectangle(cr,r.x+4,r.y+4,r.width-8,r.height-8); cairo_clip(cr);
            cairo_set_source_rgb(cr,1,1,1); cairo_move_to(cr,r.x+7,r.y+18); cairo_set_font_size(cr,12); cairo_show_text(cr,t->row->text[0]); cairo_restore(cr);
        }
    }
    if (u->smoke && u->smoke_stage == 3) { u->smoke_stage = 4; g_idle_add(map_smoke_navigate, u); }
    if (!any) { cairo_set_source_rgb(cr,.4,.4,.4); cairo_move_to(cr,20,35); cairo_show_text(cr,"No nonzero indexed sizes in this directory"); }
}
static Tile *map_hit(Ui *u, double x, double y)
{
    for (guint i = 0; i < u->tiles->len; i++) {
        Tile *t = &g_array_index(u->tiles, Tile, i); th_rect r = t->rect;
        if (x >= r.x && x < r.x+r.width && y >= r.y && y < r.y+r.height) return t;
    }
    return NULL;
}
static gboolean map_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard, GtkTooltip *tip, gpointer data)
{
    (void)widget; (void)keyboard; Tile *t = map_hit(data, x, y); if (!t) return FALSE;
    char *path = g_utf8_make_valid(t->row->path, -1), *size = g_format_size((guint64)t->weight);
    char *text = g_strdup_printf("%s\n%s\n%s",t->row->text[0],path,size); gtk_tooltip_set_text(tip,text);
    g_free(text); g_free(path); g_free(size); return TRUE;
}
static void map_pressed(GtkGestureClick *gesture, int presses, double x, double y, gpointer data)
{
    (void)gesture; (void)presses; Ui *u = data; Tile *t = map_hit(u,x,y); if (!t || !t->row->id) return;
    Row *r = t->row;
    if (!strcmp(r->type,"dir")) navigate(u,r->root,r->id,r->path,r->size);
    else { char *uri = g_filename_to_uri(r->path,NULL,NULL); if (uri) { g_app_info_launch_default_for_uri_async(uri,NULL,NULL,NULL,NULL); g_free(uri); } }
}
static gboolean map_smoke_navigate(gpointer data)
{
    Ui *u = data;
    for (guint i = 0; i < u->tiles->len; i++) {
        Tile *t = &g_array_index(u->tiles, Tile, i);
        if (!strcmp(t->row->type,"dir") && t->rect.width > 0 && t->rect.height > 0) {
            map_pressed(NULL,1,t->rect.x+t->rect.width*.5,t->rect.y+t->rect.height*.5,u); return G_SOURCE_REMOVE;
        }
    }
    u->smoke_failed = true; g_application_quit(G_APPLICATION(u->app)); return G_SOURCE_REMOVE;
}
static void tab_changed(GObject *object, GParamSpec *param, gpointer data) { (void)object; (void)param; query(data); }

typedef struct { int64_t time, size, allocated; } Snapshot;
static void history_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area; Ui *u=data; double max=1; bool allocated=gtk_drop_down_get_selected(u->metric)!=0;
    for (guint i=0;i<u->snapshots->len;i++) { Snapshot v=g_array_index(u->snapshots,Snapshot,i); max=MAX(max,(double)(allocated?v.allocated:v.size)); }
    cairo_set_source_rgb(cr,.4,.4,.4); cairo_move_to(cr,20,22); cairo_show_text(cr,allocated?"Allocated root size — oldest to newest":"Logical root size — oldest to newest");
    if (!u->snapshots->len) return;
    cairo_set_source_rgb(cr,.15,.45,.7); cairo_set_line_width(cr,2);
    for (guint i=0;i<u->snapshots->len;i++) {
        Snapshot v=g_array_index(u->snapshots,Snapshot,i);
        double x=25+(double)i/MAX((double)u->snapshots->len-1,1.)*MAX(width-50,1);
        double y=height-25-(double)(allocated?v.allocated:v.size)/max*MAX(height-60,1);
        if (i) cairo_line_to(cr,x,y); else cairo_move_to(cr,x,y);
    }
    cairo_stroke(cr);
    if (u->snapshots->len==1) { Snapshot v=g_array_index(u->snapshots,Snapshot,0); double y=height-25-(double)(allocated?v.allocated:v.size)/max*MAX(height-60,1); cairo_arc(cr,25,y,4,0,6.283185); cairo_fill(cr); }
}
static void history_populate(Ui *u,const th_jval *res)
{
    g_array_set_size(u->snapshots,0); GString *text=g_string_new("Root snapshots (up to 365 shown). Automatic capture after a successful reconciliation, at most once daily.\n");
    const th_jval *items=th_json_get(res,"snapshots");
    if (items) for(size_t i=0;i<items->n;i++) {
        const th_jval *v=&items->items[i]; Snapshot snapshot={th_json_get_int(v,"time",0),th_json_get_int(v,"size",0),th_json_get_int(v,"allocated",0)}; g_array_append_val(u->snapshots,snapshot);
        GDateTime *date=g_date_time_new_from_unix_local(snapshot.time); char *when=date?g_date_time_format(date,"%Y-%m-%d %H:%M:%S"):g_strdup("Unknown date");
        g_string_append_printf(text,"%s   logical %lld B   allocated %lld B\n",when,(long long)snapshot.size,(long long)snapshot.allocated); g_free(when); if(date)g_date_time_unref(date);
    }
    if(u->snapshots->len>1) { Snapshot a=g_array_index(u->snapshots,Snapshot,u->snapshots->len-2),b=g_array_index(u->snapshots,Snapshot,u->snapshots->len-1); g_string_append_printf(text,"\nLast capture change: logical %+lld B; allocated %+lld B\n",(long long)(b.size-a.size),(long long)(b.allocated-a.allocated)); }
    g_string_append(text,"\nLargest changes in directories present in both captures (largest 1024 directories retained per capture):\n");
    items=th_json_get(res,"changes"); if(items)for(size_t i=0;i<items->n;i++) { const th_jval *v=&items->items[i]; char *path=g_utf8_make_valid(th_json_get_str(v,"path",""),-1); g_string_append_printf(text,"%+lld B logical / %+lld B allocated   %s\n",(long long)th_json_get_int(v,"size_delta",0),(long long)th_json_get_int(v,"allocated_delta",0),path); g_free(path); }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(u->history_text)),text->str,-1);g_string_free(text,true);gtk_widget_queue_draw(u->history_chart);
    gtk_label_set_text(GTK_LABEL(u->status),th_json_get_str(res,"status","indexed"));
}
static void snapshot_clicked(GtkButton *button,gpointer data)
{
    (void)button; Ui *u=data; if(u->root)submit(u,g_strdup_printf("{\"v\":1,\"cmd\":\"snapshot\",\"root_id\":%lld}",(long long)u->root),9);
}
static void received(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Ui *u = data; GTask *task = G_TASK(result); Work *w = g_task_get_task_data(task);
    GError *error = NULL; th_jval *res = g_task_propagate_pointer(task, &error);
    if (w->kind == 9 && !u->closing) {
        gtk_widget_set_visible(u->scan_label, false);
        gtk_widget_set_visible(u->scan_progress, false);
        if (u->scan_pulse_source) { g_source_remove(u->scan_pulse_source); u->scan_pulse_source = 0; }
        if (!u->retry_source) u->retry_source = g_timeout_add(250, retry_roots, u);
    }
    if (!u->closing && (mutation(w->kind) || w->generation == u->generation)) {
        if (!res || !th_json_get_bool(res, "ok", false)) {
            gtk_label_set_text(GTK_LABEL(u->status), error ? error->message : th_json_get_str(res, "error", "Request failed"));
            if (u->smoke) u->smoke_failed = true;
        } else if (w->kind == 1) {
            GtkWidget *child; while ((child = gtk_widget_get_first_child(u->roots))) gtk_box_remove(GTK_BOX(u->roots), child);
            bool updating = false, scanning = false;
            char *summary = NULL, *scan_path = NULL;
            int summary_priority = 0;
            int64_t scan_entries = 0;
            const th_jval *roots = th_json_get(res, "roots");
            if (roots) for (size_t i = 0; i < roots->n; i++) {
                const th_jval *v = &roots->items[i]; Row *r = g_new0(Row, 1);
                r->id = th_json_get_int(v, "entry_id", 0); r->root = th_json_get_int(v, "id", 0);
                r->size = th_json_get_int(v, "size", 0); r->path = th_xstrdup(th_json_get_str(v, "path", ""));
                char *valid = g_utf8_make_valid(r->path, -1);
                const char *state = th_json_get_str(v, "status", "indexed");
                const char *error_text = th_json_get_str(v, "error", "");
                bool first = th_json_get_bool(v, "first_scan", false);
                bool active = th_json_get_bool(v, "scan_active", false);
                bool queued = th_json_get_bool(v, "scan_queued", false);
                bool interrupted = th_json_get_bool(v, "scan_interrupted", false);
                bool completed_first = false;
                int64_t root_id = r->root;
                if (active) g_hash_table_remove(u->completed_first_scans, &root_id);
                if (first) {
                    g_hash_table_remove(u->completed_first_scans, &root_id);
                    if (!g_hash_table_contains(u->observed_first_scans, &root_id)) {
                        int64_t *key = g_new(int64_t, 1); *key = root_id;
                        g_hash_table_add(u->observed_first_scans, key);
                    }
                } else if (!strcmp(state, "verified")) {
                    if (g_hash_table_remove(u->observed_first_scans, &root_id)) {
                        int64_t *key = g_new(int64_t, 1); *key = root_id;
                        g_hash_table_add(u->completed_first_scans, key);
                    }
                    completed_first = g_hash_table_contains(u->completed_first_scans, &root_id);
                }
                updating |= !strcmp(state, "indexed") || !strcmp(state, "updating") || active || queued;
                char *label = g_strdup_printf("%s\n%s · %s", valid, first ? "First scan" : state, error_text);
                int priority = active ? 5 : interrupted ? 4 : first ? 3 : queued || !strcmp(state, "indexed") || !strcmp(state, "updating") ? 2 : completed_first ? 1 : !strcmp(state, "verified") ? 0 : 1;
                if (priority > summary_priority) {
                    g_free(summary); g_free(scan_path);
                    summary_priority = priority; scanning = active;
                    scan_entries = th_json_get_int(v, "scan_entries", 0);
                    scan_path = g_utf8_make_valid(th_json_get_str(v, "scan_path", ""), -1);
                    if (active) summary = g_strdup_printf("%s configured root %s: %lld entries scanned%s",
                        first ? "First scan of" : "Reconciling", valid, (long long)scan_entries,
                        first ? "" : "; cached index remains available");
                    else if (interrupted) summary = g_strdup_printf("%s of %s interrupted; partial index may be available. Verify to resume.", first ? "First scan" : "Scan", valid);
                    else if (first && (!strcmp(state, "error") || !strcmp(state, "offline") || (!strcmp(state, "stale") && *error_text))) summary = g_strdup_printf("First scan of %s failed: %s", valid, *error_text ? error_text : state);
                    else if (first) summary = g_strdup_printf("First scan pending for configured root %s; no complete index yet.", valid);
                    else if (queued || !strcmp(state, "indexed") || !strcmp(state, "updating")) summary = g_strdup_printf("Cached index for %s; reconciliation pending.", valid);
                    else if (completed_first) summary = g_strdup_printf("First scan complete for configured root %s; index verified.", valid);
                    else summary = g_strdup_printf("Index for %s is %s%s%s", valid, state, *error_text ? ": " : "", error_text);
                }
                GtkWidget *b = gtk_button_new_with_label(label);
                GtkLabel *caption = GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b))); gtk_label_set_ellipsize(caption, PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_max_width_chars(caption, 24);
                g_free(label); g_free(valid);
                g_object_set_data_full(G_OBJECT(b), "row", r, row_free);
                g_signal_connect(b, "clicked", G_CALLBACK(root_clicked), u); gtk_box_append(GTK_BOX(u->roots), b);
                if (!u->parent && r->id && i == 0) navigate(u, r->root, r->id, r->path, r->size);
                else if (u->parent == r->id) u->parent_size = r->size;
            }
            gtk_widget_set_visible(u->scan_label, summary != NULL);
            gtk_label_set_text(GTK_LABEL(u->scan_label), summary ? summary : "");
            gtk_widget_set_tooltip_text(u->scan_label, scanning && scan_path && *scan_path ? scan_path : NULL);
            gtk_widget_set_visible(u->scan_progress, scanning);
            if (scanning) {
                char *count = g_strdup_printf("%lld entries scanned", (long long)scan_entries);
                gtk_progress_bar_set_text(GTK_PROGRESS_BAR(u->scan_progress), count);
                g_free(count);
                if (!u->scan_pulse_source) u->scan_pulse_source = g_timeout_add(250, pulse_scan, u);
            } else if (u->scan_pulse_source) {
                g_source_remove(u->scan_pulse_source); u->scan_pulse_source = 0;
            }
            g_free(summary); g_free(scan_path);
            if (updating && !u->retry_source) u->retry_source = g_timeout_add(1000, retry_roots, u);
            if (u->parent) query(u);
        } else if (w->kind == 2) {
            populate(u, th_json_get(res, "items"));
            char *text = g_strdup_printf("%u results on page %lld · %.1f ms · %s", g_list_model_get_n_items(G_LIST_MODEL(u->model)),
                (long long)(u->offset / 200 + 1), th_json_get_double(res, "elapsed_ms", 0),
                th_json_get_bool(res, "used_index", false) ? "trigram index" : "indexed records");
            gtk_label_set_text(GTK_LABEL(u->status), text); g_free(text);
            if (!u->mounts_loaded && !u->benchmark) { submit(u,g_strdup("{\"v\":1,\"cmd\":\"mounts\"}"),10); }
            if (u->benchmark) {
                g_print("{\"window_mapped_ms\":%.3f,\"indexed_listing_ms\":%.3f}\n",u->mapped*1000,(th_mono_sec()-u->launched)*1000);
                g_application_quit(G_APPLICATION(u->app));
            }
            if (u->smoke) {
                if (!g_list_model_get_n_items(G_LIST_MODEL(u->model))) u->smoke_failed = true;
                u->smoke_stage++;
                if (u->smoke_stage == 1) {
                    if (!u->saved_paths->len) bookmark_clicked(NULL,u);
                    if (u->saved_paths->len!=1 || strcmp(g_ptr_array_index(u->saved_paths,0),u->path))u->smoke_failed=true;
                    gtk_editable_set_text(GTK_EDITABLE(u->search),"smoke");
                }
                else if (u->smoke_stage == 2) gtk_stack_set_visible_child_name(GTK_STACK(u->stack), "treemap");
            }
        } else if (w->kind == 10) {
            u->mounts_loaded=true;
            GtkWidget *child;while((child=gtk_widget_get_first_child(u->mounts)))gtk_box_remove(GTK_BOX(u->mounts),child);
            const th_jval *mounts=th_json_get(res,"mounts");if(mounts)for(size_t i=0;i<mounts->n;i++) {
                const th_jval *v=&mounts->items[i];if(!th_json_get_bool(v,"accessible",false))continue;
                path_button(u->mounts,th_json_get_str(v,"path",""),th_json_get_str(v,"filesystem",""),u);
            }
        } else if (w->kind == 7) {
            map_populate(u, res);
            if (u->smoke && u->smoke_stage == 2) { u->smoke_stage = 3; if (!u->tiles->len) u->smoke_failed = true; }
            else if (u->smoke && u->smoke_stage == 4) { u->smoke_stage = 5; if (!u->tiles->len) u->smoke_failed = true; gtk_stack_set_visible_child_name(GTK_STACK(u->stack),"history"); }
        } else if (w->kind == 8) {
            history_populate(u,res);
            if(u->smoke && u->smoke_stage==5) { if(!u->snapshots->len)u->smoke_failed=true;u->smoke_stage=6; snapshot_clicked(NULL,u); snapshot_clicked(NULL,u); snapshot_clicked(NULL,u); query(u); }
            else if(u->smoke && u->smoke_stage==6) { if(u->snapshots->len<4)u->smoke_failed=true;u->smoke_stage=7;g_application_quit(G_APPLICATION(u->app)); }
        } else if (w->kind == 9) { query(u);
        } else if (w->kind == 6) {
            const th_jval *items = th_json_get(res, "items");
            if (items && items->n) { const th_jval *v = &items->items[0]; navigate(u, th_json_get_int(v, "root_id", 0), th_json_get_int(v, "id", 0), th_json_get_str(v, "path", ""), th_json_get_int(v, "size", 0)); }
            else gtk_label_set_text(GTK_LABEL(u->status),"Directory is outside the index; add an accessible root in Settings.");
        } else if (w->kind == 4) {
            show_settings(u, th_json_get_str(res, "config", ""));
        } else if (w->kind == 5) {
            gtk_label_set_text(GTK_LABEL(u->status), th_json_get_bool(res, "restart_required", false) ?
                "Saved; restart daemon to change watch coverage. Other changes reconcile now." : "Saved; roots and exclusions are reconciling. Reopen to refresh sidebar.");
            if (u->settings) { gtk_window_destroy(GTK_WINDOW(u->settings)); u->settings = NULL; }
            if (!u->retry_source) u->retry_source = g_timeout_add(250, retry_roots, u);
        } else {
            gtk_label_set_text(GTK_LABEL(u->status), "Verification queued; scan progress will appear below.");
            if (!u->retry_source) u->retry_source = g_timeout_add(250, retry_roots, u);
        }
    }
    th_json_free(res); g_clear_error(&error);
    u->busy = false;
    if (!u->closing && !g_queue_is_empty(u->actions)) {
        start_work(u, g_queue_pop_head(u->actions));
    } else if (u->pending && !u->closing) {
        Work *next = g_new0(Work, 1);
        next->request = u->pending; next->kind = u->pending_kind;
        next->generation = u->generation; u->pending = NULL;
        start_work(u, next);
    }
}
static void submit(Ui *u, char *request, guint kind)
{
    if (u->closing) { g_free(request); return; }
    if (!mutation(kind)) u->generation++;
    if (u->busy && !mutation(kind)) {
        g_free(u->pending); u->pending = request; u->pending_kind = kind; return;
    }
    if (u->busy && g_queue_get_length(u->actions) >= 64) {
        gtk_label_set_text(GTK_LABEL(u->status), "Action queue full; wait before retrying.");
        g_free(request); return;
    }
    Work *w = g_new0(Work, 1); w->request = request;
    w->generation = u->generation; w->kind = kind;
    if (u->busy) g_queue_push_tail(u->actions, w);
    else start_work(u, w);
}
static void start_work(Ui *u, Work *w)
{
    w->socket = g_strdup(u->socket); w->start = w->kind == 1;
    GTask *task = g_task_new(u->app, NULL, received, u); g_task_set_task_data(task, w, work_free);
    u->busy = true; gtk_label_set_text(GTK_LABEL(u->status), w->kind == 9 ? "Verifying root before snapshot…" : "Loading indexed data…");
    if (w->kind == 9) {
        gtk_label_set_text(GTK_LABEL(u->scan_label), "Verifying configured root before snapshot; progress is indeterminate.");
        gtk_widget_set_visible(u->scan_label, true);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(u->scan_progress), "Verifying…");
        gtk_widget_set_visible(u->scan_progress, true);
        if (!u->scan_pulse_source) u->scan_pulse_source = g_timeout_add(250, pulse_scan, u);
    }
    g_task_run_in_thread(task, worker); g_object_unref(task);
}
static void query(Ui *u)
{
    if (!u->parent || u->closing) return;
    char *valid_path = g_utf8_make_valid(u->path,-1); gtk_label_set_text(GTK_LABEL(u->breadcrumb),valid_path); g_free(valid_path);
    if (!strcmp(gtk_stack_get_visible_child_name(GTK_STACK(u->stack)),"history")) { submit(u,g_strdup_printf("{\"v\":1,\"cmd\":\"history\",\"root_id\":%lld}",(long long)u->root),8);return; }
    if (!strcmp(gtk_stack_get_visible_child_name(GTK_STACK(u->stack)), "treemap")) {
        char *request = g_strdup_printf("{\"v\":1,\"cmd\":\"treemap\",\"parent_id\":%lld,\"metric\":\"%s\",\"show_hidden\":%s}", (long long)u->parent,
            gtk_drop_down_get_selected(u->metric) ? "allocated" : "logical", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->hidden)) ? "true" : "false");
        submit(u, request, 7); return;
    }
    const char *text = gtk_editable_get_text(GTK_EDITABLE(u->search));
    th_strbuf b; th_sb_init(&b); th_jw w; th_jw_init(&w, &b); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", 1); th_jw_kv_str(&w, "cmd", *text ? "search" : "list");
    th_jw_kv_str(&w, "query", text); th_jw_kv_int(&w, "root_id", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->global)) && *text ? 0 : u->root);
    if (*text) { if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(u->global))) th_jw_kv_str(&w, "under", u->path); } else th_jw_kv_int(&w, "parent_id", u->parent);
    th_jw_kv_bool(&w, "exact", gtk_check_button_get_active(GTK_CHECK_BUTTON(u->exact)));
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
static void changed(GtkWidget *widget, gpointer data) { Ui *u = data; u->offset = 0; if (widget == u->search && *gtk_editable_get_text(GTK_EDITABLE(u->search))) gtk_stack_set_visible_child_name(GTK_STACK(u->stack),"explorer"); query(u); }
static void dropdown_changed(GObject *object, GParamSpec *param, gpointer data) { (void)object; (void)param; changed(NULL, data); }
static void refresh(GtkButton *button, gpointer data) { (void)button; submit(data, g_strdup("{\"v\":1,\"cmd\":\"roots\"}"), 1); }
static void up(GtkButton *button, gpointer data)
{
    (void)button; Ui *u = data; if (!u->path || !strcmp(u->path, "/")) return;
    char *parent = g_path_get_dirname(u->path); th_strbuf b; th_sb_init(&b); th_jw w; th_jw_init(&w, &b);
    th_jw_obj_begin(&w); th_jw_kv_int(&w, "v", 1); th_jw_kv_str(&w, "cmd", "search"); th_jw_kv_bytes(&w, "query", parent, strlen(parent));
    th_jw_kv_bool(&w, "exact", true); th_jw_kv_int(&w, "root_id", u->root); th_jw_kv_int(&w, "limit", 1); th_jw_obj_end(&w);
    submit(u, g_strdup(b.data), 6); th_sb_free(&b); g_free(parent);
}
static void page(GtkButton *button, gpointer data)
{
    Ui *u = data; int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    if (direction > 0 && g_list_model_get_n_items(G_LIST_MODEL(u->model)) < 200) return;
    u->offset = MAX(0, u->offset + direction * 200); query(u);
}
static void opened(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; GtkApplication *app=data; Ui *u=g_object_get_data(G_OBJECT(app),"ui");
    GError *error=NULL;
    if(!g_app_info_launch_default_for_uri_finish(result,&error) && u && !u->closing)
        gtk_label_set_text(GTK_LABEL(u->status),error->message);
    g_clear_error(&error);g_object_unref(app);
}
static void open_path(Ui *u,const char *path)
{
    GError *error=NULL;char *uri=g_filename_to_uri(path,NULL,&error);
    if(uri)g_app_info_launch_default_for_uri_async(uri,NULL,NULL,opened,g_object_ref(u->app));
    if(error)gtk_label_set_text(GTK_LABEL(u->status),error->message);
    g_clear_error(&error);g_free(uri);
}
static void activate_row(GtkColumnView *view, guint position, gpointer data)
{
    (void)view; Ui *u = data; GObject *obj = g_list_model_get_item(G_LIST_MODEL(u->model), position); Row *r = row_of(obj);
    if (r && !strcmp(r->type, "dir")) navigate(u, r->root, r->id, r->path, r->size);
    else if (r) {
        open_path(u,r->path);
    }
    g_clear_object(&obj);
}
static void selected_action(GtkButton *button, gpointer data)
{
    Ui *u = data; Row *r = row_of(gtk_single_selection_get_selected_item(u->selection)); if (!r) return;
    const char *action = g_object_get_data(G_OBJECT(button), "action");
    if (!strcmp(action, "copy")) { gdk_clipboard_set_text(gtk_widget_get_clipboard(u->window), r->text[1]); return; }
    if (!strcmp(action, "open")) { activate_row(GTK_COLUMN_VIEW(u->view), gtk_single_selection_get_selected(u->selection), u); return; }
    char *dir = g_path_get_dirname(r->path);open_path(u,dir);g_free(dir);
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
static void save_settings(GtkButton *button, gpointer data)
{
    (void)button; Ui *u = data; GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(u->config_text));
    GtkTextIter start, end; gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, false);
    th_strbuf b; th_sb_init(&b); th_jw w; th_jw_init(&w, &b); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", 1); th_jw_kv_str(&w, "cmd", "config_save"); th_jw_kv_str(&w, "config", text); th_jw_obj_end(&w);
    submit(u, g_strdup(b.data), 5); th_sb_free(&b); g_free(text);
}
static gboolean settings_closed(GtkWindow *window, gpointer data)
{
    (void)window; Ui *u = data; u->settings = NULL; return FALSE;
}
static void show_settings(Ui *u, const char *text)
{
    if (u->settings) { gtk_window_present(GTK_WINDOW(u->settings)); return; }
    u->settings = gtk_window_new(); gtk_window_set_transient_for(GTK_WINDOW(u->settings), GTK_WINDOW(u->window));
    gtk_window_set_modal(GTK_WINDOW(u->settings), true); gtk_window_set_title(GTK_WINDOW(u->settings), "Treehound configuration");
    gtk_window_set_default_size(GTK_WINDOW(u->settings), 620, 500);
    g_signal_connect(u->settings, "close-request", G_CALLBACK(settings_closed), u);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_window_set_child(GTK_WINDOW(u->settings), box);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Roots/exclusions, mount crossing, reconciliation and retention.\nWatch changes require daemon restart. Background service is enabled\nonly by running: systemctl --user enable --now treehound.service"));
    u->config_text = gtk_text_view_new(); gtk_text_view_set_monospace(GTK_TEXT_VIEW(u->config_text), true);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(u->config_text)), text, -1);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), u->config_text); gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *save = gtk_button_new_with_label("Save and reconcile"); g_signal_connect(save, "clicked", G_CALLBACK(save_settings), u); gtk_box_append(GTK_BOX(box), save);
    gtk_window_present(GTK_WINDOW(u->settings));
}
static void settings(GtkButton *button, gpointer data)
{
    (void)button; submit(data, g_strdup("{\"v\":1,\"cmd\":\"config\"}"), 4);
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
static void preferences(Ui *u, bool save)
{
    char *path = th_gui_config_path(); GKeyFile *key = g_key_file_new();
    const char *names[] = {"hidden", "case_sensitive", "descending", "exact", "all_roots"};
    GtkWidget *widgets[] = {u->hidden, u->sensitive, u->descending, u->exact, u->global};
    if (save) {
        for (size_t i = 0; i < 5; i++) g_key_file_set_boolean(key, "view", names[i], gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets[i])));
        g_key_file_set_integer(key, "view", "sort", (int)gtk_drop_down_get_selected(u->sort));
        g_key_file_set_integer(key, "view", "metric", (int)gtk_drop_down_get_selected(u->metric));
        g_key_file_set_string(key, "view", "page", gtk_stack_get_visible_child_name(GTK_STACK(u->stack)));
        char **encoded=g_new0(char *,u->saved_paths->len+1);
        for(guint i=0;i<u->saved_paths->len;i++) { const char *p=g_ptr_array_index(u->saved_paths,i);encoded[i]=g_base64_encode((const guchar *)p,strlen(p)); }
        g_key_file_set_string_list(key,"navigation","bookmarks",(const gchar *const *)encoded,u->saved_paths->len);g_strfreev(encoded);
        gsize n; char *text = g_key_file_to_data(key, &n, NULL); char *dir = th_config_dir(); th_mkdir_p(dir, 0700); free(dir);
        th_write_file_atomic(path, text, n, 0600); g_free(text);
    } else if (g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL)) {
        for (size_t i = 0; i < 5; i++) gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets[i]), g_key_file_get_boolean(key, "view", names[i], NULL));
        if (g_key_file_has_key(key,"view","metric",NULL)) gtk_drop_down_set_selected(u->metric,g_key_file_get_integer(key,"view","metric",NULL) == 0 ? 0u : 1u);
        char *page = g_key_file_get_string(key,"view","page",NULL); if (page && (!strcmp(page,"treemap") || !strcmp(page,"history"))) gtk_stack_set_visible_child_name(GTK_STACK(u->stack),page); g_free(page);
        int sort = g_key_file_get_integer(key, "view", "sort", NULL); if (sort >= 0 && sort < 5) gtk_drop_down_set_selected(u->sort, (guint)sort);
    }
    if (!save) {
        gsize n=0;char **encoded=g_key_file_get_string_list(key,"navigation","bookmarks",&n,NULL);
        for(gsize i=0;encoded && i<MIN(n,64);i++) { gsize length=0;guchar *raw=g_base64_decode(encoded[i],&length);
            if(length>0 && length<4096 && raw[0]=='/' && !memchr(raw,0,length))
                g_ptr_array_add(u->saved_paths,g_strndup((const char *)raw,length));
            g_free(raw);
        }
        g_strfreev(encoded);bookmarks_refresh(u);
    }
    g_key_file_unref(key); free(path);
}
static gboolean close_window(GtkWindow *window, gpointer data) { (void)window; Ui *u = data; preferences(u, true); u->closing = true; return FALSE; }
static gboolean smoke_timeout(gpointer data) { Ui *u = data; u->smoke_source = 0; u->smoke_failed = true; g_application_quit(G_APPLICATION(u->app)); return G_SOURCE_REMOVE; }
static void mapped(GtkWidget *widget, gpointer data) { (void)widget; Ui *u=data; if (!u->mapped) u->mapped=th_mono_sec()-u->launched; }
static void activate(GtkApplication *app, gpointer data)
{
    Ui *u = data; u->window = gtk_application_window_new(app); gtk_window_set_title(GTK_WINDOW(u->window), "Treehound " TH_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(u->window), 1180, 720);
    g_signal_connect(u->window, "map", G_CALLBACK(mapped), u);
    g_signal_connect(u->window, "close-request", G_CALLBACK(close_window), u);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_window_set_child(GTK_WINDOW(u->window), box);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_append(GTK_BOX(box), toolbar);
    gtk_box_append(GTK_BOX(toolbar), gtk_label_new("Treehound " TH_VERSION));
    u->search = gtk_search_entry_new(); gtk_widget_set_hexpand(u->search, true); gtk_box_append(GTK_BOX(toolbar), u->search);
    g_signal_connect(u->search, "search-changed", G_CALLBACK(changed), u);
    button(toolbar, "Settings", G_CALLBACK(settings), u);
    button(toolbar, "Verify", G_CALLBACK(verify), u); button(toolbar, "Refresh", G_CALLBACK(refresh), u);
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), filters);
    const char *sorts[] = {"Name", "Size", "Path", "Modified", "Type", NULL};
    const char *types[] = {"All types", "Files", "Directories", NULL};
    u->sort = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sorts)); u->types = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    gtk_box_append(GTK_BOX(filters), GTK_WIDGET(u->sort)); gtk_box_append(GTK_BOX(filters), GTK_WIDGET(u->types));
    g_signal_connect(u->sort, "notify::selected", G_CALLBACK(dropdown_changed), u); g_signal_connect(u->types, "notify::selected", G_CALLBACK(dropdown_changed), u);
    u->descending = gtk_check_button_new_with_label("Descending"); u->hidden = gtk_check_button_new_with_label("Hidden"); u->sensitive = gtk_check_button_new_with_label("Case sensitive");
    u->exact = gtk_check_button_new_with_label("Exact"); u->global = gtk_check_button_new_with_label("All roots");
    GtkWidget *checks[] = {u->descending, u->hidden, u->sensitive, u->exact, u->global};
    for (size_t i = 0; i < 5; i++) { gtk_box_append(GTK_BOX(filters), checks[i]); g_signal_connect(checks[i], "toggled", G_CALLBACK(changed), u); }
    GtkWidget *advanced = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); GtkWidget *expander = gtk_expander_new("Search filters (Enter to apply)");
    gtk_expander_set_child(GTK_EXPANDER(expander), advanced); gtk_box_append(GTK_BOX(box), expander);
    u->extension = entry(advanced, "Extension", u); u->minimum = entry(advanced, "Min size", u); u->maximum = entry(advanced, "Max size", u);
    u->after = entry(advanced, "After / 7d", u); u->before = entry(advanced, "Before", u);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), nav); button(nav, "Back", G_CALLBACK(back), u); button(nav, "Up", G_CALLBACK(up), u);
    u->breadcrumb = gtk_label_new("Indexed roots"); gtk_label_set_ellipsize(GTK_LABEL(u->breadcrumb), PANGO_ELLIPSIZE_START); gtk_label_set_xalign(GTK_LABEL(u->breadcrumb), 0);
    gtk_widget_set_hexpand(u->breadcrumb, true); gtk_box_append(GTK_BOX(nav), u->breadcrumb);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_widget_set_vexpand(paned, true); gtk_box_append(GTK_BOX(box), paned);
    GtkWidget *sidebar=gtk_box_new(GTK_ORIENTATION_VERTICAL,6),*side_scroll=gtk_scrolled_window_new();
    gtk_widget_set_size_request(side_scroll,205,-1);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll),sidebar);gtk_paned_set_start_child(GTK_PANED(paned),side_scroll);
    path_button(sidebar,g_get_home_dir(),"Home",u);button(sidebar,"Bookmark / remove current",G_CALLBACK(bookmark_clicked),u);
    gtk_box_append(GTK_BOX(sidebar),gtk_label_new("Indexed roots"));u->roots=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_box_append(GTK_BOX(sidebar),u->roots);
    gtk_box_append(GTK_BOX(sidebar),gtk_label_new("Bookmarks"));u->bookmarks=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_box_append(GTK_BOX(sidebar),u->bookmarks);
    gtk_box_append(GTK_BOX(sidebar),gtk_label_new("Mounted filesystems"));u->mounts=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_box_append(GTK_BOX(sidebar),u->mounts);
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
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), u->view); u->stack = gtk_stack_new(); gtk_stack_add_titled(GTK_STACK(u->stack), scroll, "explorer", "Explorer / Search");
    u->map = gtk_drawing_area_new(); gtk_widget_set_hexpand(u->map,true); gtk_widget_set_vexpand(u->map,true);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(u->map), map_draw, u, NULL);
    gtk_widget_set_has_tooltip(u->map,true); g_signal_connect(u->map,"query-tooltip",G_CALLBACK(map_tooltip),u);
    GtkGesture *click = gtk_gesture_click_new(); g_signal_connect(click,"pressed",G_CALLBACK(map_pressed),u); gtk_widget_add_controller(u->map,GTK_EVENT_CONTROLLER(click));
    gtk_stack_add_titled(GTK_STACK(u->stack),u->map,"treemap","Treemap"); gtk_paned_set_end_child(GTK_PANED(paned),u->stack);
    GtkWidget *history_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6); button(history_box,"Capture snapshot after verification",G_CALLBACK(snapshot_clicked),u);
    u->history_chart=gtk_drawing_area_new();gtk_widget_set_size_request(u->history_chart,-1,200);gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(u->history_chart),history_draw,u,NULL);gtk_box_append(GTK_BOX(history_box),u->history_chart);
    u->history_text=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(u->history_text),false);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(u->history_text),GTK_WRAP_WORD_CHAR);
    GtkWidget *history_scroll=gtk_scrolled_window_new();gtk_widget_set_vexpand(history_scroll,true);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(history_scroll),u->history_text);gtk_box_append(GTK_BOX(history_box),history_scroll);gtk_stack_add_titled(GTK_STACK(u->stack),history_box,"history","History");
    GtkWidget *switcher = gtk_stack_switcher_new(); gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),GTK_STACK(u->stack)); gtk_box_append(GTK_BOX(nav),switcher);
    const char *metrics[] = {"Logical", "Allocated", NULL}; u->metric = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(metrics)); gtk_drop_down_set_selected(u->metric,1);
    gtk_box_append(GTK_BOX(nav),GTK_WIDGET(u->metric)); g_signal_connect(u->metric,"notify::selected",G_CALLBACK(tab_changed),u);
    g_signal_connect(u->stack,"notify::visible-child-name",G_CALLBACK(tab_changed),u);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), footer);
    GtkWidget *prev = button(footer, "Previous", G_CALLBACK(page), u); g_object_set_data(G_OBJECT(prev), "direction", GINT_TO_POINTER(-1));
    GtkWidget *next = button(footer, "Next", G_CALLBACK(page), u); g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
    const char *actions[] = {"open", "folder", "copy"}; const char *labels[] = {"Open", "Open folder", "Copy path"};
    for (size_t i = 0; i < 3; i++) { GtkWidget *b = button(footer, labels[i], G_CALLBACK(selected_action), u); g_object_set_data(G_OBJECT(b), "action", (gpointer)actions[i]); }
    u->status = gtk_label_new("Connecting…"); gtk_label_set_xalign(GTK_LABEL(u->status), 0); gtk_box_append(GTK_BOX(box), u->status);
    u->scan_label = gtk_label_new(NULL); gtk_label_set_xalign(GTK_LABEL(u->scan_label), 0);
    gtk_label_set_wrap(GTK_LABEL(u->scan_label), true); gtk_widget_set_visible(u->scan_label, false);
    gtk_box_append(GTK_BOX(box), u->scan_label);
    u->scan_progress = gtk_progress_bar_new(); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(u->scan_progress), true);
    gtk_widget_set_visible(u->scan_progress, false); gtk_box_append(GTK_BOX(box), u->scan_progress);
    preferences(u, false);
    gtk_window_present(GTK_WINDOW(u->window)); submit(u, g_strdup("{\"v\":1,\"cmd\":\"roots\"}"), 1);
    if (u->smoke) u->smoke_source = g_timeout_add_seconds(10, smoke_timeout, u);
}
static void ui_free(gpointer data)
{
    Ui *u = data;
    for (guint i = 0; i < u->history->len; i++) free(g_array_index(u->history, Location, i).path);
    map_clear(u); g_array_unref(u->tiles); g_array_unref(u->snapshots);
    g_clear_object(&u->model); g_clear_object(&u->selection); g_array_unref(u->history);
    g_queue_free_full(u->actions, work_free);
    g_ptr_array_unref(u->saved_paths); g_hash_table_unref(u->observed_first_scans);
    g_hash_table_unref(u->completed_first_scans);
    free(u->socket); free(u->path); g_free(u->pending); g_free(u);
}
int th_gui_run(int argc, char **argv)
{
    Ui *u = g_new0(Ui, 1); u->socket = th_socket_path(); u->history = g_array_new(false, false, sizeof(Location));
    u->saved_paths=g_ptr_array_new_with_free_func(g_free); u->actions=g_queue_new();
    u->observed_first_scans = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    u->completed_first_scans = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    u->tiles = g_array_new(false, false, sizeof(Tile)); u->snapshots = g_array_new(false,false,sizeof(Snapshot));
    u->smoke = g_getenv("TREEHOUND_GUI_SMOKE") != NULL;
    u->benchmark = g_getenv("TREEHOUND_GUI_BENCHMARK") != NULL; u->launched=th_mono_sec();
    GtkApplication *app = gtk_application_new("io.github.blindicide.treehound", G_APPLICATION_NON_UNIQUE); u->app = app;
    g_object_set_data_full(G_OBJECT(app), "ui", u, ui_free); g_signal_connect(app, "activate", G_CALLBACK(activate), u);
    int result = g_application_run(G_APPLICATION(app), argc, argv); if (u->smoke && (u->smoke_failed || u->smoke_stage != 7)) result = 4;
    u->closing = true; if (u->retry_source) g_source_remove(u->retry_source); if (u->smoke_source) g_source_remove(u->smoke_source);
    if (u->scan_pulse_source) g_source_remove(u->scan_pulse_source);
    g_object_unref(app); return result;
}
