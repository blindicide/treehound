/* SPDX-License-Identifier: MIT */
#include "daemon.h"
#include "monitor.h"
#include "treehound/common.h"
#include "treehound/db.h"
#include "treehound/match.h"
#include "treehound/search.h"
#include "treehound/util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void failure(th_strbuf *out, const char *code, const char *error)
{
    th_sb_reset(out);
    th_jw w; th_jw_init(&w, out); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", TH_PROTOCOL_VERSION); th_jw_kv_bool(&w, "ok", false);
    th_jw_kv_str(&w, "code", code); th_jw_kv_str(&w, "error", error); th_jw_obj_end(&w);
}
static void entry_json(th_jw *w, const th_entry *e, int state)
{
    th_jw_obj_begin(w);
    th_jw_kv_int(w, "id", e->id); th_jw_kv_int(w, "parent_id", e->parent_id);
    th_jw_kv_int(w, "root_id", e->root_id);
    th_jw_kv_bytes(w, "name", e->name, e->name_len); th_jw_kv_bytes(w, "path", e->path, e->path_len);
    th_jw_kv_str(w, "type", th_type_name(e->type));
    th_jw_kv_int(w, "size", e->type == TH_TYPE_DIR ? e->agg_size : e->size);
    th_jw_kv_int(w, "allocated", e->type == TH_TYPE_DIR ? e->agg_alloc : e->alloc);
    th_jw_kv_int(w, "files", e->agg_files); th_jw_kv_int(w, "directories", e->agg_dirs);
    th_jw_kv_int(w, "mtime", e->mtime); th_jw_kv_int(w, "flags", e->flags);
    th_jw_kv_str(w, "status", th_state_name(state)); th_jw_obj_end(w);
}
static bool valid_strings(const th_jval *v)
{
    if (v->type == TH_JSTR && memchr(v->str, 0, v->len)) return false;
    for (size_t i = 0; i < v->n; i++) if (!valid_strings(&v->items[i])) return false;
    return true;
}
void daemon_handle(th_daemon *d, sqlite3 **db, const char *req, size_t len, th_strbuf *out)
{
    const char *parse_error = NULL;
    th_jval *q = th_json_parse(req, len, &parse_error);
    if (!q || q->type != TH_JOBJ || !valid_strings(q) ||
        th_json_get_int(q, "v", 0) != TH_PROTOCOL_VERSION) {
        failure(out, "request", parse_error ? parse_error : "invalid request or protocol version"); th_json_free(q); return;
    }
    const char *cmd = th_json_get_str(q, "cmd", "");
    th_strbuf err; th_sb_init(&err);
    if (!*db) *db = th_db_open(d->db_path, true, &err);
    if (!*db) { failure(out, "database", err.data ? err.data : "database unavailable"); goto done; }
    th_jw w; th_jw_init(&w, out); th_jw_obj_begin(&w);
    th_jw_kv_int(&w, "v", TH_PROTOCOL_VERSION); th_jw_kv_bool(&w, "ok", true);
    th_jw_kv_str(&w, "version", TH_VERSION);
    if (!strcmp(cmd, "status")) {
        pthread_mutex_lock(&d->mu);
        th_jw_kv_bool(&w, "busy", d->busy); th_jw_kv_int(&w, "queued", (int64_t)d->nqueue);
        th_jw_kv_int(&w, "completed_seq", (int64_t)d->completed_seq);
        th_jw_kv_int(&w, "issued_seq", (int64_t)d->next_seq);
        th_jw_kv_int(&w, "uptime", th_now() - d->started_at);
        th_jw_kv_int(&w, "scanned_entries", d->current_stats.entries);
        th_jw_kv_str(&w, "current_path", d->current_path ? d->current_path : "");
        th_jw_kv_bool(&w, "live_indexing", monitor_enabled());
        pthread_mutex_unlock(&d->mu);
        th_jw_kv_int(&w, "entries", th_db_entry_count(*db));
    } else if (!strcmp(cmd, "roots")) {
        th_root *roots = NULL; size_t n = 0;
        if (th_db_roots(*db, &roots, &n) != 0) { failure(out, "database", "cannot read roots"); goto done; }
        th_jw_key(&w, "roots"); th_jw_arr_begin(&w);
        for (size_t i = 0; i < n; i++) {
            th_root *r = &roots[i]; th_jw_obj_begin(&w);
            th_jw_kv_int(&w, "id", r->id); th_jw_kv_int(&w, "entry_id", r->entry_id);
            th_jw_kv_str(&w, "path", r->path); th_jw_kv_str(&w, "status", th_state_name(monitor_pending(r->id) && r->state == TH_STATE_VERIFIED ? TH_STATE_UPDATING : r->state));
            th_jw_kv_str(&w, "error", r->error ? r->error : "");
            th_jw_kv_int(&w, "size", r->total_size); th_jw_kv_int(&w, "allocated", r->total_alloc);
            th_jw_kv_int(&w, "files", r->total_files); th_jw_kv_int(&w, "directories", r->total_dirs);
            th_jw_kv_int(&w, "last_verified", r->last_verified); th_jw_obj_end(&w);
        }
        th_jw_arr_end(&w); th_roots_free(roots, n);
    } else if (!strcmp(cmd, "search") || !strcmp(cmd, "list")) {
        th_search_opts o; th_search_opts_init(&o);
        o.query = th_json_get_str(q, "query", "");
        o.under = th_json_get_str(q, "under", NULL);
        o.extension = th_json_get_str(q, "extension", NULL);
        o.show_hidden = th_json_get_bool(q, "show_hidden", true);
        if (!strcmp(cmd, "list")) {
            o.parent_id = th_json_get_int(q, "parent_id", -1);
            if (o.parent_id < 0) { failure(out, "request", "list requires parent_id"); goto done; }
        }
        o.root_id = th_json_get_int(q, "root_id", 0);
        o.min_size = th_json_get_int(q, "min_size", -1); o.max_size = th_json_get_int(q, "max_size", -1);
        o.min_mtime = th_json_get_int(q, "min_mtime", -1); o.max_mtime = th_json_get_int(q, "max_mtime", -1);
        o.offset = th_json_get_int(q, "offset", 0); o.limit = th_json_get_int(q, "limit", 200);
        o.descending = th_json_get_bool(q, "descending", false);
        o.want_total = th_json_get_bool(q, "total", false); o.timeout_ms = 1000;
        pthread_mutex_lock(&d->mu);
        o.match_flags = th_json_get_bool(q, "case_sensitive", d->cfg.case_sensitive) ? TH_MATCH_CASE : 0;
        pthread_mutex_unlock(&d->mu);
        if (th_json_get_bool(q, "exact", false)) o.match_flags |= TH_MATCH_FULL;
        const char *type = th_json_get_str(q, "type", "all");
        o.types = !strcmp(type, "file") ? TH_TYPEMASK(TH_TYPE_FILE) : !strcmp(type, "dir") ? TH_TYPEMASK(TH_TYPE_DIR) : TH_TYPEMASK_ALL;
        if (!th_sort_parse(th_json_get_str(q, "sort", "name"), &o.sort) || o.offset < 0 ||
            (strcmp(type, "all") && strcmp(type, "file") && strcmp(type, "dir"))) {
            failure(out, "request", "invalid sort, type or offset"); goto done;
        }
        th_search_result r;
        if (th_search(*db, &o, &r, &err) != 0) { failure(out, "search", err.data ? err.data : "search failed"); goto done; }
        th_jw_kv_int(&w, "total", r.total); th_jw_kv_bool(&w, "used_index", r.used_index);
        th_jw_kv_double(&w, "elapsed_ms", r.elapsed_ms);
        th_jw_key(&w, "items"); th_jw_arr_begin(&w);
        int64_t last_root = -1; int state = TH_STATE_INDEXED;
        for (size_t i = 0; i < r.n; i++) {
            if (r.items[i].root_id != last_root) {
                th_root root = {0}; last_root = r.items[i].root_id;
                if (th_db_root_get(*db, last_root, &root) == 1) state = root.state;
                th_root_clear(&root);
                if (monitor_pending(last_root) && state == TH_STATE_VERIFIED) state = TH_STATE_UPDATING;
            }
            entry_json(&w, &r.items[i], state);
        }
        th_jw_arr_end(&w); th_search_result_free(&r);
    } else if (!strcmp(cmd, "scan") || !strcmp(cmd, "verify") || !strcmp(cmd, "rebuild")) {
        int64_t id = th_json_get_int(q, "root_id", 0);
        if (!id && !strcmp(cmd, "rebuild")) { failure(out, "request", "rebuild requires root_id"); goto done; }
        if (id) {
            th_root r = {0}; int found = th_db_root_get(*db, id, &r); th_root_clear(&r);
            if (found != 1) { failure(out, "request", "unknown root_id"); goto done; }
        }
        /* FIFO only: the preserved scanner's completion watermark assumes this ordering. */
        monitor_force(id);
        uint64_t seq = id ? daemon_enqueue(d, !strcmp(cmd, "rebuild") ? JOB_REBUILD : JOB_SCAN, id, false) : daemon_enqueue_all(d, *db);
        th_jw_kv_int(&w, "seq", (int64_t)seq);
    } else if (!strcmp(cmd, "config")) {
        th_strbuf cfg; th_sb_init(&cfg);
        pthread_mutex_lock(&d->mu); th_config_serialize(&d->cfg, &cfg); pthread_mutex_unlock(&d->mu);
        th_jw_kv_str(&w, "config", cfg.data ? cfg.data : ""); th_sb_free(&cfg);
    } else if (!strcmp(cmd, "shutdown")) {
        uint64_t one = 1; (void)!write(d->wake_fd, &one, sizeof one);
    } else { failure(out, "request", "unknown command"); goto done; }
    th_jw_obj_end(&w);
done:
    th_sb_free(&err); th_json_free(q);
}
