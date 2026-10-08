/* SPDX-License-Identifier: MIT */
#include "treehound/history.h"

int th_history_capture(sqlite3 *db, int64_t root, int retention, int64_t now, bool force)
{
    th_root r = {0}; sqlite3_stmt *q = NULL; int rc = -1;
    if (retention <= 0) return 0;
    if (th_db_root_get(db, root, &r) != 1 || r.state != TH_STATE_VERIFIED) goto done;
    q = th_db_prepare(db, "SELECT coalesce(max(captured_at),0) FROM snapshots WHERE root_id=?1");
    if (!q) goto done;
    sqlite3_bind_int64(q,1,root);
    if (sqlite3_step(q) != SQLITE_ROW) goto done;
    int64_t last = sqlite3_column_int64(q,0);
    sqlite3_finalize(q); q = NULL;
    if (!force && last && now - last < 86400) { rc = 0; goto done; }
    if (th_db_begin(db) != 0) goto done;
    q = th_db_prepare(db,"INSERT INTO snapshots(root_id,captured_at,size,allocated,files,directories) VALUES(?1,?2,?3,?4,?5,?6)");
    if (!q) goto rollback;
    sqlite3_bind_int64(q,1,root); sqlite3_bind_int64(q,2,now);
    sqlite3_bind_int64(q,3,r.total_size); sqlite3_bind_int64(q,4,r.total_alloc);
    sqlite3_bind_int64(q,5,r.total_files); sqlite3_bind_int64(q,6,r.total_dirs);
    if (sqlite3_step(q) != SQLITE_DONE) goto rollback;
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(q);
    q = th_db_prepare(db,"INSERT INTO snapshot_dirs(snapshot_id,path,size,allocated) SELECT ?1,path,agg_size,agg_alloc FROM entries WHERE root_id=?2 AND type=1 ORDER BY agg_size DESC,id LIMIT 1024");
    if (!q) goto rollback;
    sqlite3_bind_int64(q,1,id); sqlite3_bind_int64(q,2,root);
    if (sqlite3_step(q) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(q);
    q = th_db_prepare(db,"DELETE FROM snapshots WHERE root_id=?1 AND id NOT IN (SELECT id FROM snapshots WHERE root_id=?1 ORDER BY id DESC LIMIT ?2)");
    if (!q) goto rollback;
    sqlite3_bind_int64(q,1,root); sqlite3_bind_int(q,2,retention);
    if (sqlite3_step(q) != SQLITE_DONE) goto rollback;
    rc = th_db_commit(db);
rollback:
    if (rc) th_db_rollback(db);
done:
    sqlite3_finalize(q); th_root_clear(&r); return rc;
}
int th_history_reply(sqlite3 *db, int64_t root, th_jw *out)
{
    th_root r = {0}; sqlite3_stmt *q = NULL; int rc = -1;
    if (root <= 0 || th_db_exec(db,"BEGIN") != 0) return -1;
    if (th_db_root_get(db,root,&r) != 1) goto done;
    th_jw_kv_str(out,"status",th_state_name(r.state));
    th_jw_kv_int(out,"root_id",root); th_jw_kv_int(out,"directory_limit",1024);
    q = th_db_prepare(db,"SELECT id,captured_at,size,allocated,files,directories FROM (SELECT * FROM snapshots WHERE root_id=?1 ORDER BY id DESC LIMIT 365) ORDER BY id");
    if (!q) goto done;
    sqlite3_bind_int64(q,1,root);
    int step; int64_t before = 0, after = 0;
    th_jw_key(out,"snapshots"); th_jw_arr_begin(out);
    while ((step = sqlite3_step(q)) == SQLITE_ROW) {
        before = after; after = sqlite3_column_int64(q,0);
        th_jw_obj_begin(out);
        const char *keys[] = {"id","time","size","allocated","files","directories"};
        for (int i = 0; i < 6; i++) th_jw_kv_int(out,keys[i],sqlite3_column_int64(q,i));
        th_jw_obj_end(out);
    }
    if (step != SQLITE_DONE) goto done;
    th_jw_arr_end(out); sqlite3_finalize(q); q = NULL;
    th_jw_key(out,"changes"); th_jw_arr_begin(out);
    if (before) {
        /* Missing top-directory coverage is unknown, rather than a false zero. */
        q = th_db_prepare(db,"SELECT a.path,a.size-b.size,a.allocated-b.allocated FROM snapshot_dirs a JOIN snapshot_dirs b ON b.snapshot_id=?2 AND b.path=a.path WHERE a.snapshot_id=?1 AND (a.size<>b.size OR a.allocated<>b.allocated) ORDER BY abs(a.size-b.size) DESC,a.path LIMIT 20");
        if (!q) goto done;
        sqlite3_bind_int64(q,1,after); sqlite3_bind_int64(q,2,before);
        while ((step = sqlite3_step(q)) == SQLITE_ROW) {
            th_jw_obj_begin(out); th_jw_key(out,"path");
            th_jw_bytes(out,(const char *)sqlite3_column_text(q,0),(size_t)sqlite3_column_bytes(q,0));
            th_jw_kv_int(out,"size_delta",sqlite3_column_int64(q,1));
            th_jw_kv_int(out,"allocated_delta",sqlite3_column_int64(q,2)); th_jw_obj_end(out);
        }
        if (step != SQLITE_DONE) goto done;
    }
    th_jw_arr_end(out); rc = 0;
done:
    sqlite3_finalize(q); th_root_clear(&r); th_db_exec(db,"ROLLBACK"); return rc;
}
