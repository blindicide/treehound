/* SPDX-License-Identifier: MIT */
#include "daemon.h"
#include "monitor.h"
#include "treehound/common.h"
#include "treehound/db.h"
#include "treehound/match.h"
#include "treehound/search.h"
#include "treehound/mounts.h"
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
static int treemap_reply(sqlite3 *db, const th_jval *q, th_jw *w)
{
    int64_t parent = th_json_get_int(q, "parent_id", 0);
    const char *metric = th_json_get_str(q, "metric", "allocated");
    if (parent <= 0 || (strcmp(metric, "allocated") && strcmp(metric, "logical"))) return -1;
    const char *weight = !strcmp(metric, "allocated") ?
        "CASE WHEN e.type=1 THEN e.agg_alloc WHEN e.flags & 8 THEN 0 ELSE e.alloc END" :
        "CASE WHEN e.type=1 THEN e.agg_size WHEN e.flags & 8 THEN 0 ELSE e.size END";
    th_strbuf sql; th_sb_init(&sql);
    int rc = -1; sqlite3_stmt *st = NULL; th_root root = {0}; th_entry directory = {0};
    if (th_db_exec(db, "BEGIN") != 0) goto done;
    if (th_db_entry_get(db, parent, &directory) != 1 || directory.type != TH_TYPE_DIR ||
        th_db_root_get(db, directory.root_id, &root) != 1) goto rollback;
    bool hidden = th_json_get_bool(q, "show_hidden", true);
    const char *where = " FROM entries e WHERE e.parent_id=?1 AND (?2 OR substr(e.name,1,1)<>'.')";
    th_sb_printf(&sql, "SELECT count(*),coalesce(sum(%s),0)%s", weight, where);
    st = th_db_prepare(db, sql.data); if (!st) goto rollback;
    sqlite3_bind_int64(st, 1, parent); sqlite3_bind_int(st, 2, hidden);
    if (sqlite3_step(st) != SQLITE_ROW) goto rollback;
    int64_t count = sqlite3_column_int64(st, 0), total = sqlite3_column_int64(st, 1), shown = 0, sum = 0;
    sqlite3_finalize(st); st = NULL; th_sb_reset(&sql);
    th_jw_kv_int(w, "children", count); th_jw_kv_int(w, "total_weight", total); th_jw_kv_str(w, "metric", metric);
    th_jw_kv_str(w, "status", th_state_name(root.state));
    th_sb_printf(&sql, "SELECT " TH_ENTRY_COLS ",%s AS weight%s ORDER BY weight DESC,e.id LIMIT 512", weight, where);
    st = th_db_prepare(db, sql.data); if (!st) goto rollback;
    sqlite3_bind_int64(st, 1, parent); sqlite3_bind_int(st, 2, hidden);
    th_jw_key(w, "items"); th_jw_arr_begin(w);
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        th_entry e = {0}; th_entry_from_stmt(&e, st, 0); int64_t size = sqlite3_column_int64(st, TH_ENTRY_NCOLS);
        entry_json(w, &e, root.state); th_entry_clear(&e); sum += size; shown++;
    }
    if (step != SQLITE_DONE) goto rollback;
    th_jw_arr_end(w); th_jw_kv_int(w, "other_count", count - shown); th_jw_kv_int(w, "other_weight", total - sum);
    rc = 0;
rollback:
    th_db_exec(db, "ROLLBACK");
done:
    sqlite3_finalize(st); th_entry_clear(&directory); th_root_clear(&root); th_sb_free(&sql); return rc;
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
    } else if (!strcmp(cmd, "treemap")) {
        if (treemap_reply(*db, q, &w) != 0) { failure(out, "request", "invalid treemap scope or database query failed"); goto done; }
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
    } else if (!strcmp(cmd, "mounts")) {
        th_mounts mounts;
        if (th_mounts_load(&mounts, NULL) != 0) { failure(out, "mounts", "cannot read mountinfo"); goto done; }
        th_jw_key(&w, "mounts"); th_jw_arr_begin(&w);
        for (size_t i = 0; i < mounts.n; i++) {
            th_mount *mount = &mounts.v[i];
            if (th_fstype_is_pseudo(mount->fstype)) continue;
            th_jw_obj_begin(&w); th_jw_kv_str(&w, "path", mount->mnt);
            th_jw_kv_str(&w, "filesystem", mount->fstype);
            th_jw_kv_bool(&w, "accessible", access(mount->mnt, R_OK | X_OK) == 0);
            th_jw_obj_end(&w);
        }
        th_jw_arr_end(&w); th_mounts_free(&mounts);
    } else if (!strcmp(cmd, "config_save")) {
        const th_jval *text = th_json_get(q, "config");
        if (!text || text->type != TH_JSTR) { failure(out, "request", "config must be text"); goto done; }
        th_config next; th_config_defaults(&next);
        if (th_config_parse(&next, text->str, text->len, &err) != 0) {
            failure(out, "config", err.data ? err.data : "invalid configuration"); th_config_free(&next); goto done;
        }
        pthread_mutex_lock(&d->mu);
        bool restart = next.watch != d->cfg.watch;
        if (th_config_save(&next, d->config_path) != 0) {
            pthread_mutex_unlock(&d->mu); th_config_free(&next); failure(out, "config", "could not save configuration"); goto done;
        }
        next.watch = d->cfg.watch; /* watch thread lifecycle changes require daemon restart */
        th_config old = d->cfg; d->cfg = next;
        pthread_mutex_unlock(&d->mu); th_config_free(&old);
        monitor_force(0);
        daemon_enqueue(d, JOB_SYNC, 0, false);
        uint64_t seq = daemon_enqueue_all(d, *db);
        th_jw_kv_int(&w, "seq", (int64_t)seq); th_jw_kv_bool(&w, "restart_required", restart);
    } else if (!strcmp(cmd, "config")) {
        th_strbuf cfg; th_sb_init(&cfg);
        pthread_mutex_lock(&d->mu);
        th_config saved; th_config_defaults(&saved);
        if (th_config_load(&saved, d->config_path, &err) == 0) th_config_serialize(&saved, &cfg);
        else th_config_serialize(&d->cfg, &cfg);
        th_config_free(&saved); pthread_mutex_unlock(&d->mu);
        th_jw_kv_str(&w, "config", cfg.data ? cfg.data : ""); th_sb_free(&cfg);
    } else if (!strcmp(cmd, "shutdown")) {
        uint64_t one = 1; (void)!write(d->wake_fd, &one, sizeof one);
    } else { failure(out, "request", "unknown command"); goto done; }
    th_jw_obj_end(&w);
done:
    th_sb_free(&err); th_json_free(q);
}
