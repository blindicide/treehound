/* SPDX-License-Identifier: MIT */
/* Filesystem scanner: walks a root, diffs every directory against the index and
 * writes only the differences, then recomputes directory aggregates bottom-up. */
#include "treehound/indexer.h"

#include "treehound/db.h"
#include "treehound/util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- th_open_dir ---- */

int th_open_dir(const char *path, size_t len, bool follow)
{
    int last_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW);
    if (len < PATH_MAX && memchr(path, '\0', len) == NULL) {
        char *p = th_xmemdup(path, len);
        int fd = open(p, last_flags);
        int e = errno;
        free(p);
        errno = e;
        return fd;
    }
    /* Too long for one syscall: walk component by component. */
    int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t i = 0;
    char *comp = th_xmalloc(len + 1);
    while (i < len) {
        while (i < len && path[i] == '/')
            i++;
        if (i >= len)
            break;
        size_t j = i;
        while (j < len && path[j] != '/')
            j++;
        memcpy(comp, path + i, j - i);
        comp[j - i] = '\0';
        size_t k = j;
        while (k < len && path[k] == '/')
            k++;
        bool last = k >= len;
        int nfd = openat(fd, comp, last ? last_flags : (O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        int e = errno;
        close(fd);
        if (nfd < 0) {
            free(comp);
            errno = e;
            return -1;
        }
        fd = nfd;
        i = j;
    }
    free(comp);
    return fd;
}

/* ---- aggregates ---- */

#define AGG_SELECT_SQL                                                                     \
    "SELECT coalesce(sum(CASE WHEN type = 1 THEN agg_size WHEN flags & 8 THEN 0 ELSE size END), 0), " \
    "coalesce(sum(CASE WHEN type = 1 THEN agg_alloc WHEN flags & 8 THEN 0 ELSE alloc END), 0), "      \
    "coalesce(sum(CASE WHEN type = 1 THEN agg_files ELSE 1 END), 0), "                                \
    "coalesce(sum(CASE WHEN type = 1 THEN agg_dirs + 1 ELSE 0 END), 0) "                              \
    "FROM entries WHERE parent_id = ?"
#define AGG_UPDATE_SQL                                                                    \
    "UPDATE entries SET agg_size = size + ?2, agg_alloc = alloc + ?3, agg_files = ?4, agg_dirs = ?5 " \
    "WHERE id = ?1 AND (agg_size <> size + ?2 OR agg_alloc <> alloc + ?3 OR agg_files <> ?4 OR "       \
    "agg_dirs <> ?5)"

/* Returns 1 when the row changed, 0 when it was already current, -1 on error. */
static int refresh_dir_with(sqlite3_stmt *sel, sqlite3_stmt *upd, int64_t id)
{
    sqlite3_reset(sel);
    sqlite3_bind_int64(sel, 1, id);
    if (sqlite3_step(sel) != SQLITE_ROW)
        return -1;
    int64_t size = sqlite3_column_int64(sel, 0), alloc = sqlite3_column_int64(sel, 1);
    int64_t files = sqlite3_column_int64(sel, 2), dirs = sqlite3_column_int64(sel, 3);
    sqlite3_reset(sel);
    sqlite3_reset(upd);
    sqlite3_bind_int64(upd, 1, id);
    sqlite3_bind_int64(upd, 2, size);
    sqlite3_bind_int64(upd, 3, alloc);
    sqlite3_bind_int64(upd, 4, files);
    sqlite3_bind_int64(upd, 5, dirs);
    int rc = sqlite3_step(upd);
    sqlite3_reset(upd);
    if (rc != SQLITE_DONE)
        return -1;
    return sqlite3_changes(sqlite3_db_handle(upd)) > 0;
}

int th_index_refresh_dir(sqlite3 *db, int64_t dir_id)
{
    sqlite3_stmt *sel = th_db_prepare(db, AGG_SELECT_SQL);
    sqlite3_stmt *upd = th_db_prepare(db, AGG_UPDATE_SQL);
    int rc = sel && upd && refresh_dir_with(sel, upd, dir_id) >= 0 ? 0 : -1;
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    return rc;
}

int th_index_refresh_ancestors(sqlite3 *db, int64_t entry_id)
{
    sqlite3_stmt *sel = th_db_prepare(db, AGG_SELECT_SQL);
    sqlite3_stmt *upd = th_db_prepare(db, AGG_UPDATE_SQL);
    sqlite3_stmt *par = th_db_prepare(db, "SELECT parent_id FROM entries WHERE id = ?");
    int rc = sel && upd && par ? 0 : -1;
    int64_t id = entry_id;
    for (int guard = 0; rc == 0 && guard < 100000; guard++) {
        sqlite3_reset(par);
        sqlite3_bind_int64(par, 1, id);
        if (sqlite3_step(par) != SQLITE_ROW)
            break;
        id = sqlite3_column_int64(par, 0);
        sqlite3_reset(par);
        if (id == 0)
            break;
        if (refresh_dir_with(sel, upd, id) < 0)
            rc = -1;
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    sqlite3_finalize(par);
    return rc;
}

/* Deduplicate shared ancestors in SQLite; no filesystem traversal. */
int th_index_refresh_batch(sqlite3 *db, const int64_t *directories, size_t count)
{
    if (sqlite3_exec(db, "CREATE TEMP TABLE IF NOT EXISTS aggregate_pending(id INTEGER PRIMARY KEY); DELETE FROM aggregate_pending", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *ins = th_db_prepare(db, "INSERT OR IGNORE INTO aggregate_pending VALUES(?)");
    int rc = ins ? 0 : -1;
    for (size_t i = 0; i < count && rc == 0; i++) {
        sqlite3_reset(ins); sqlite3_bind_int64(ins, 1, directories[i]);
        if (sqlite3_step(ins) != SQLITE_DONE) rc = -1;
    }
    sqlite3_finalize(ins);
    sqlite3_stmt *rows = rc == 0 ? th_db_prepare(db,
        "WITH RECURSIVE a(id,parent_id,path) AS ("
        "SELECT e.id,e.parent_id,e.path FROM entries e JOIN aggregate_pending p ON e.id=p.id "
        "UNION SELECT e.id,e.parent_id,e.path FROM entries e JOIN a ON e.id=a.parent_id) "
        "SELECT id FROM a ORDER BY path DESC") : NULL;
    sqlite3_stmt *sel = th_db_prepare(db, AGG_SELECT_SQL), *upd = th_db_prepare(db, AGG_UPDATE_SQL);
    if (!rows || !sel || !upd || th_db_begin(db) != 0) rc = -1;
    bool transaction = rc == 0;
    int step = SQLITE_DONE;
    while (rc == 0 && (step = sqlite3_step(rows)) == SQLITE_ROW)
        if (refresh_dir_with(sel, upd, sqlite3_column_int64(rows, 0)) < 0) rc = -1;
    if (step != SQLITE_DONE) rc = -1;
    sqlite3_finalize(rows); sqlite3_finalize(sel); sqlite3_finalize(upd);
    if (transaction) {
        if (rc == 0 && th_db_commit(db) != 0) rc = -1;
        if (rc != 0) th_db_rollback(db);
    }
    return rc;
}

/* ---- scan context ---- */

typedef struct {
    int64_t id;
    char *path;
    size_t len;
    bool follow; /* only the root itself may be a symlink */
} work_item;

typedef struct {
    char *name;
    size_t name_len;
    struct stat st;
} fs_child;

typedef struct {
    int64_t id;
    char *name;
    size_t name_len;
    int type;
    int64_t size, alloc, mtime, dev, ino, nlink;
    int flags;
} db_child;

typedef struct {
    sqlite3 *db;
    int64_t root_id;
    const th_scan_opts *o;
    th_scan_stats *st;
    th_strbuf *err;
    bool cross_mounts;
    size_t batch, pending;
    unsigned progress_ms;
    int64_t last_progress;
    char **other_roots;
    size_t nother;

    sqlite3_stmt *q_children, *q_insert, *q_update, *q_del_range, *q_del_id, *q_linkdup,
        *q_set_flags, *q_get_flags, *q_agg_sel, *q_agg_upd, *q_touch;

    work_item *stack;
    size_t nstack, capstack;
    th_strbuf lo, hi, child;
} scan_ctx;

static int db_fail(scan_ctx *c, const char *what)
{
    if (c->err && c->err->len == 0)
        th_sb_printf(c->err, "%s: %s", what, sqlite3_errmsg(c->db));
    return -1;
}

static int step_done(scan_ctx *c, sqlite3_stmt *s, const char *what)
{
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? 0 : db_fail(c, what);
}

static int wrote(scan_ctx *c)
{
    if (++c->pending < c->batch)
        return 0;
    c->pending = 0;
    if (th_db_commit(c->db) != 0)
        return db_fail(c, "commit");
    if (th_db_begin(c->db) != 0)
        return db_fail(c, "begin");
    return 0;
}

static bool cancelled(const scan_ctx *c)
{
    return c->o && c->o->cancel && atomic_load(c->o->cancel);
}

static void progress(scan_ctx *c, const char *path, bool force)
{
    if (!c->o || !c->o->progress)
        return;
    int64_t now = th_mono_ms();
    if (!force && now - c->last_progress < (int64_t)c->progress_ms)
        return;
    c->last_progress = now;
    c->o->progress(c->st, path, c->o->ud);
}

static void push(scan_ctx *c, int64_t id, const char *path, size_t len, bool follow)
{
    if (c->nstack == c->capstack) {
        c->capstack = c->capstack ? c->capstack * 2 : 64;
        c->stack = th_xrealloc(c->stack, c->capstack * sizeof *c->stack);
    }
    work_item *w = &c->stack[c->nstack++];
    w->id = id;
    w->path = th_xmemdup(path, len);
    w->len = len;
    w->follow = follow;
}

static int prepare_all(scan_ctx *c)
{
    sqlite3 *db = c->db;
    c->q_children = th_db_prepare(db, "SELECT id, name, type, size, alloc, mtime, dev, ino, nlink, flags "
                                      "FROM entries WHERE parent_id = ? ORDER BY name");
    c->q_insert = th_db_prepare(
        db, "INSERT INTO entries(root_id, parent_id, name, path, type, size, alloc, agg_size, agg_alloc, "
            "mtime, dev, ino, nlink, flags) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, "
            "CASE WHEN ?5 = 1 THEN 0 ELSE ?6 END, CASE WHEN ?5 = 1 THEN 0 ELSE ?7 END, ?8, ?9, ?10, ?11, ?12)");
    c->q_update = th_db_prepare(
        db, "UPDATE entries SET size = ?2, alloc = ?3, agg_size = CASE WHEN type = 1 THEN agg_size ELSE ?2 END, "
            "agg_alloc = CASE WHEN type = 1 THEN agg_alloc ELSE ?3 END, mtime = ?4, dev = ?5, ino = ?6, "
            "nlink = ?7, flags = ?8, state = 0 WHERE id = ?1");
    c->q_del_range = th_db_prepare(db, "DELETE FROM entries WHERE root_id = ? AND path >= ? AND path < ?");
    c->q_del_id = th_db_prepare(db, "DELETE FROM entries WHERE id = ?");
    c->q_linkdup = th_db_prepare(db, "SELECT 1 FROM entries WHERE dev = ?1 AND ino = ?2 AND type <> 1 "
                                     "AND path <> ?3 AND root_id = ?4 AND (flags & 8) = 0 LIMIT 1");
    c->q_set_flags = th_db_prepare(db, "UPDATE entries SET flags = ?2 WHERE id = ?1 AND flags <> ?2");
    c->q_get_flags = th_db_prepare(db, "SELECT flags FROM entries WHERE id = ?");
    c->q_touch = th_db_prepare(db, "INSERT INTO temp.scan_links(dev,ino,size,alloc,mtime,nlink) VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(dev,ino) DO UPDATE SET size=excluded.size,alloc=excluded.alloc,mtime=excluded.mtime,nlink=excluded.nlink");
    c->q_agg_sel = th_db_prepare(db, AGG_SELECT_SQL);
    c->q_agg_upd = th_db_prepare(db, AGG_UPDATE_SQL);
    if (!c->q_children || !c->q_insert || !c->q_update || !c->q_del_range || !c->q_del_id ||
        !c->q_touch || !c->q_linkdup || !c->q_set_flags || !c->q_get_flags || !c->q_agg_sel || !c->q_agg_upd)
        return db_fail(c, "prepare");
    return 0;
}

static void load_other_roots(scan_ctx *c)
{
    sqlite3_stmt *s = th_db_prepare(c->db, "SELECT path FROM roots WHERE id <> ?");
    if (!s)
        return;
    sqlite3_bind_int64(s, 1, c->root_id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        c->other_roots = th_xrealloc(c->other_roots, (c->nother + 1) * sizeof *c->other_roots);
        c->other_roots[c->nother++] = th_column_dup(s, 0, NULL);
    }
    sqlite3_finalize(s);
}

static bool ctx_init(scan_ctx *c, sqlite3 *db, int64_t root_id, const th_scan_opts *o, th_scan_stats *st,
                     th_strbuf *err)
{
    memset(c, 0, sizeof *c);
    c->db = db;
    c->root_id = root_id;
    c->o = o;
    c->st = st;
    c->err = err;
    c->cross_mounts = o && o->cfg && o->cfg->cross_mounts;
    c->batch = o && o->batch_rows ? o->batch_rows : 10000;
    c->progress_ms = o && o->progress_ms ? o->progress_ms : 250;
    c->last_progress = th_mono_ms();
    th_sb_init(&c->lo);
    th_sb_init(&c->hi);
    th_sb_init(&c->child);
    load_other_roots(c);
    return th_db_exec(db, "CREATE TEMP TABLE IF NOT EXISTS scan_links(dev INTEGER,ino INTEGER,size INTEGER,alloc INTEGER,mtime INTEGER,nlink INTEGER,PRIMARY KEY(dev,ino)) WITHOUT ROWID; DELETE FROM temp.scan_links") == 0 && prepare_all(c) == 0;
}

static void ctx_free(scan_ctx *c)
{
    sqlite3_stmt *all[] = {c->q_children, c->q_insert,    c->q_update,  c->q_del_range, c->q_del_id,
                           c->q_linkdup,  c->q_set_flags, c->q_get_flags, c->q_agg_sel, c->q_agg_upd, c->q_touch};
    for (size_t i = 0; i < TH_ARRAY_LEN(all); i++)
        sqlite3_finalize(all[i]);
    for (size_t i = 0; i < c->nstack; i++)
        free(c->stack[i].path);
    free(c->stack);
    for (size_t i = 0; i < c->nother; i++)
        free(c->other_roots[i]);
    free(c->other_roots);
    th_sb_free(&c->lo);
    th_sb_free(&c->hi);
    th_sb_free(&c->child);
}

/* ---- row helpers ---- */

static int type_of(mode_t m)
{
    if (S_ISREG(m))
        return TH_TYPE_FILE;
    if (S_ISDIR(m))
        return TH_TYPE_DIR;
    if (S_ISLNK(m))
        return TH_TYPE_SYMLINK;
    return TH_TYPE_OTHER;
}

static int64_t alloc_of(const struct stat *s)
{
    return (int64_t)s->st_blocks * 512;
}

/* Deletes everything below path (not the row itself). */
static int delete_below(scan_ctx *c, const char *path, size_t len)
{
    th_subtree_range(path, len, &c->lo, &c->hi);
    sqlite3_stmt *links = th_db_prepare(c->db, "INSERT OR IGNORE INTO temp.scan_links(dev,ino) SELECT dev,ino FROM entries WHERE root_id=?1 AND path>=?2 AND path<?3 AND type=0 AND nlink>1");
    if (!links) return -1;
    sqlite3_bind_int64(links, 1, c->root_id); th_bind_bytes(links, 2, c->lo.data, c->lo.len); th_bind_bytes(links, 3, c->hi.data, c->hi.len);
    int captured = sqlite3_step(links); sqlite3_finalize(links);
    if (captured != SQLITE_DONE) return -1;
    sqlite3_reset(c->q_del_range);
    sqlite3_bind_int64(c->q_del_range, 1, c->root_id);
    th_bind_bytes(c->q_del_range, 2, c->lo.data, c->lo.len);
    th_bind_bytes(c->q_del_range, 3, c->hi.data, c->hi.len);
    if (step_done(c, c->q_del_range, "delete subtree") != 0)
        return -1;
    int n = sqlite3_changes(c->db);
    c->st->removed += n;
    return n > 0 ? wrote(c) : 0;
}

static int delete_entry(scan_ctx *c, int64_t id, const char *path, size_t len, bool is_dir)
{
    if (is_dir && delete_below(c, path, len) != 0)
        return -1;
    sqlite3_stmt *links = th_db_prepare(c->db, "INSERT OR IGNORE INTO temp.scan_links(dev,ino) SELECT dev,ino FROM entries WHERE id=? AND type=0 AND nlink>1");
    if (!links) return -1;
    sqlite3_bind_int64(links, 1, id); int captured = sqlite3_step(links); sqlite3_finalize(links);
    if (captured != SQLITE_DONE) return -1;
    sqlite3_reset(c->q_del_id);
    sqlite3_bind_int64(c->q_del_id, 1, id);
    if (step_done(c, c->q_del_id, "delete entry") != 0)
        return -1;
    c->st->removed += sqlite3_changes(c->db);
    return wrote(c);
}

static int set_flags(scan_ctx *c, int64_t id, int flags)
{
    sqlite3_reset(c->q_set_flags);
    sqlite3_bind_int64(c->q_set_flags, 1, id);
    sqlite3_bind_int(c->q_set_flags, 2, flags);
    if (step_done(c, c->q_set_flags, "update flags") != 0)
        return -1;
    return sqlite3_changes(c->db) > 0 ? wrote(c) : 0;
}

static bool is_other_root(const scan_ctx *c, const char *path)
{
    for (size_t i = 0; i < c->nother; i++)
        if (strcmp(c->other_roots[i], path) == 0)
            return true;
    return false;
}

/* Builds dir + "/" + name into c->child. */
static void child_path(scan_ctx *c, const char *dir, size_t dlen, const char *name, size_t nlen)
{
    th_sb_reset(&c->child);
    th_sb_append(&c->child, dir, dlen);
    if (!(dlen == 1 && dir[0] == '/'))
        th_sb_putc(&c->child, '/');
    th_sb_append(&c->child, name, nlen);
}

/* Computes the flags for an entry and whether a directory should be descended. */
static int classify(scan_ctx *c, const char *path, size_t len, const struct stat *s, dev_t parent_dev,
                    bool *descend)
{
    int flags = 0;
    *descend = false;
    int type = type_of(s->st_mode);
    if (type == TH_TYPE_DIR) {
        const th_mounts *m = c->o ? c->o->mounts : NULL;
        const th_mount *mnt = m ? th_mounts_at(m, path) : NULL;
        bool mountpoint = s->st_dev != parent_dev || mnt != NULL;
        *descend = true;
        if (mountpoint) {
            flags |= TH_FLAG_MOUNTPOINT;
            if (!c->cross_mounts || (mnt && th_fstype_is_pseudo(mnt->fstype)))
                *descend = false;
        }
        if (m && th_mounts_is_hidden(m, path)) {
            flags |= TH_FLAG_EXCLUDED;
            *descend = false;
        }
    } else if (type == TH_TYPE_FILE) {
        int64_t alloc = alloc_of(s);
        if (alloc < (int64_t)s->st_size)
            flags |= TH_FLAG_SPARSE;
        if (s->st_nlink > 1) {
            sqlite3_reset(c->q_touch);
            sqlite3_bind_int64(c->q_touch, 1, (int64_t)s->st_dev); sqlite3_bind_int64(c->q_touch, 2, (int64_t)s->st_ino);
            sqlite3_bind_int64(c->q_touch, 3, (int64_t)s->st_size); sqlite3_bind_int64(c->q_touch, 4, alloc);
            sqlite3_bind_int64(c->q_touch, 5, (int64_t)s->st_mtim.tv_sec); sqlite3_bind_int64(c->q_touch, 6, (int64_t)s->st_nlink);
            if (sqlite3_step(c->q_touch) != SQLITE_DONE) c->st->errors++;
            sqlite3_reset(c->q_touch);
            sqlite3_reset(c->q_linkdup);
            sqlite3_bind_int64(c->q_linkdup, 1, (int64_t)s->st_dev);
            sqlite3_bind_int64(c->q_linkdup, 2, (int64_t)s->st_ino);
            th_bind_bytes(c->q_linkdup, 3, path, len);
            sqlite3_bind_int64(c->q_linkdup, 4, c->root_id);
            if (sqlite3_step(c->q_linkdup) == SQLITE_ROW)
                flags |= TH_FLAG_LINKDUP;
            sqlite3_reset(c->q_linkdup);
        }
    }
    return flags;
}

static int insert_entry(scan_ctx *c, int64_t parent, const char *name, size_t nlen, const char *path,
                        size_t plen, const struct stat *s, int flags, int64_t *id_out)
{
    sqlite3_stmt *q = c->q_insert;
    sqlite3_reset(q);
    sqlite3_bind_int64(q, 1, c->root_id);
    sqlite3_bind_int64(q, 2, parent);
    th_bind_bytes(q, 3, name, nlen);
    th_bind_bytes(q, 4, path, plen);
    sqlite3_bind_int(q, 5, type_of(s->st_mode));
    sqlite3_bind_int64(q, 6, (int64_t)s->st_size);
    sqlite3_bind_int64(q, 7, alloc_of(s));
    sqlite3_bind_int64(q, 8, (int64_t)s->st_mtim.tv_sec);
    sqlite3_bind_int64(q, 9, (int64_t)s->st_dev);
    sqlite3_bind_int64(q, 10, (int64_t)s->st_ino);
    sqlite3_bind_int64(q, 11, (int64_t)s->st_nlink);
    sqlite3_bind_int(q, 12, flags);
    if (step_done(c, q, "insert entry") != 0)
        return -1;
    *id_out = sqlite3_last_insert_rowid(c->db);
    c->st->added++;
    return wrote(c);
}

static bool row_differs(const db_child *d, const struct stat *s, int flags)
{
    return d->size != (int64_t)s->st_size || d->alloc != alloc_of(s) || d->mtime != (int64_t)s->st_mtim.tv_sec ||
           d->dev != (int64_t)s->st_dev || d->ino != (int64_t)s->st_ino || d->nlink != (int64_t)s->st_nlink ||
           d->flags != flags;
}

static int update_entry(scan_ctx *c, int64_t id, const struct stat *s, int flags)
{
    sqlite3_stmt *q = c->q_update;
    sqlite3_reset(q);
    sqlite3_bind_int64(q, 1, id);
    sqlite3_bind_int64(q, 2, (int64_t)s->st_size);
    sqlite3_bind_int64(q, 3, alloc_of(s));
    sqlite3_bind_int64(q, 4, (int64_t)s->st_mtim.tv_sec);
    sqlite3_bind_int64(q, 5, (int64_t)s->st_dev);
    sqlite3_bind_int64(q, 6, (int64_t)s->st_ino);
    sqlite3_bind_int64(q, 7, (int64_t)s->st_nlink);
    sqlite3_bind_int(q, 8, flags);
    if (step_done(c, q, "update entry") != 0)
        return -1;
    c->st->updated++;
    return wrote(c);
}

/* ---- directory diff ---- */

static int cmp_fs(const void *a, const void *b)
{
    return strcmp(((const fs_child *)a)->name, ((const fs_child *)b)->name);
}

static void free_fs(fs_child *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i].name);
    free(v);
}

static void free_db(db_child *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i].name);
    free(v);
}

static int load_db_children(scan_ctx *c, int64_t parent, db_child **out, size_t *nout)
{
    db_child *v = NULL;
    size_t n = 0, cap = 0;
    sqlite3_stmt *q = c->q_children;
    sqlite3_reset(q);
    sqlite3_bind_int64(q, 1, parent);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = th_xrealloc(v, cap * sizeof *v);
        }
        db_child *d = &v[n++];
        d->id = sqlite3_column_int64(q, 0);
        d->name = th_column_dup(q, 1, &d->name_len);
        d->type = sqlite3_column_int(q, 2);
        d->size = sqlite3_column_int64(q, 3);
        d->alloc = sqlite3_column_int64(q, 4);
        d->mtime = sqlite3_column_int64(q, 5);
        d->dev = sqlite3_column_int64(q, 6);
        d->ino = sqlite3_column_int64(q, 7);
        d->nlink = sqlite3_column_int64(q, 8);
        d->flags = sqlite3_column_int(q, 9);
    }
    sqlite3_reset(q);
    *out = v;
    *nout = n;
    if (rc != SQLITE_DONE)
        return db_fail(c, "list children");
    return 0;
}

static int read_fs_children(scan_ctx *c, int fd, const char *dir, size_t dlen, fs_child **out, size_t *nout)
{
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return -1;
    }
    fs_child *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        struct stat s;
        if (fstatat(dirfd(d), nm, &s, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT)
                c->st->errors++;
            errno = 0;
            continue;
        }
        size_t nlen = strlen(nm);
        child_path(c, dir, dlen, nm, nlen);
        if (is_other_root(c, c->child.data))
            continue;
        if (c->o && c->o->cfg && th_config_is_excluded(c->o->cfg, c->child.data, nm))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = th_xrealloc(v, cap * sizeof *v);
        }
        v[n].name = th_xmemdup(nm, nlen);
        v[n].name_len = nlen;
        v[n].st = s;
        n++;
        errno = 0;
    }
    int e = errno;
    closedir(d);
    if (e != 0)
        c->st->errors++;
    if (n > 1)
        qsort(v, n, sizeof *v, cmp_fs);
    *out = v;
    *nout = n;
    return 0;
}

static int name_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    int r = memcmp(a, b, al < bl ? al : bl);
    if (r != 0)
        return r;
    return al < bl ? -1 : al > bl ? 1 : 0;
}

/* Handles one FS child: insert or update, and push directories to descend. */
static int sync_child(scan_ctx *c, const work_item *w, dev_t dev, const fs_child *f, const db_child *d)
{
    child_path(c, w->path, w->len, f->name, f->name_len);
    bool descend;
    int flags = classify(c, c->child.data, c->child.len, &f->st, dev, &descend);
    int type = type_of(f->st.st_mode);
    int64_t id;
    c->st->entries++;
    if (d && d->type != type) {
        if (delete_entry(c, d->id, c->child.data, c->child.len, d->type == TH_TYPE_DIR) != 0)
            return -1;
        child_path(c, w->path, w->len, f->name, f->name_len);
        d = NULL;
    }
    if (d) {
        id = d->id;
        if (descend)
            flags |= d->flags & TH_FLAG_DENIED; /* owned by the child's own visit */
        if (row_differs(d, &f->st, flags) && update_entry(c, id, &f->st, flags) != 0)
            return -1;
    } else if (insert_entry(c, w->id, f->name, f->name_len, c->child.data, c->child.len, &f->st, flags, &id) !=
               0) {
        return -1;
    }
    if (type == TH_TYPE_DIR) {
        if (descend && (!c->o || !c->o->shallow || !d))
            push(c, id, c->child.data, c->child.len, false);
        else if (!descend && d && delete_below(c, c->child.data, c->child.len) != 0)
            return -1;
    }
    return 0;
}

static int entry_flags(scan_ctx *c, int64_t id)
{
    sqlite3_stmt *q = c->q_get_flags;
    sqlite3_reset(q);
    sqlite3_bind_int64(q, 1, id);
    int flags = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int(q, 0) : 0;
    sqlite3_reset(q);
    return flags;
}

static int scan_one_dir(scan_ctx *c, const work_item *w)
{
    int fd = th_open_dir(w->path, w->len, w->follow);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR || e == ELOOP) {
            /* vanished or replaced since its parent was read */
            return delete_entry(c, w->id, w->path, w->len, true);
        }
        c->st->errors++;
        if (set_flags(c, w->id, entry_flags(c, w->id) | TH_FLAG_DENIED) != 0)
            return -1;
        return delete_below(c, w->path, w->len);
    }
    struct stat ds;
    if (fstat(fd, &ds) != 0) {
        close(fd);
        c->st->errors++;
        return 0;
    }
    /* readable again: drop a DENIED mark left by an earlier visit */
    int own = entry_flags(c, w->id);
    if ((own & TH_FLAG_DENIED) && set_flags(c, w->id, own & ~TH_FLAG_DENIED) != 0) {
        close(fd);
        return -1;
    }
    if (c->o && c->o->on_dir)
        c->o->on_dir(w->id, w->path, w->len, c->o->ud);
    fs_child *fs = NULL;
    size_t nfs = 0;
    if (read_fs_children(c, fd, w->path, w->len, &fs, &nfs) != 0) {
        c->st->errors++;
        return 0;
    }
    c->st->dirs++;
    sqlite3_stmt *links = th_db_prepare(c->db, "INSERT OR IGNORE INTO temp.scan_links(dev,ino) SELECT dev,ino FROM entries WHERE parent_id=? AND type=0 AND nlink>1");
    if (!links) { free_fs(fs, nfs); return -1; }
    sqlite3_bind_int64(links, 1, w->id); int captured = sqlite3_step(links); sqlite3_finalize(links);
    if (captured != SQLITE_DONE) { free_fs(fs, nfs); return -1; }
    db_child *db = NULL;
    size_t ndb = 0;
    int rc = load_db_children(c, w->id, &db, &ndb);
    size_t i = 0, j = 0;
    while (rc == 0 && (i < nfs || j < ndb)) {
        int cmp = i >= nfs ? 1 : j >= ndb ? -1 : name_cmp(fs[i].name, fs[i].name_len, db[j].name, db[j].name_len);
        if (cmp < 0) {
            rc = sync_child(c, w, ds.st_dev, &fs[i++], NULL);
        } else if (cmp > 0) {
            child_path(c, w->path, w->len, db[j].name, db[j].name_len);
            rc = delete_entry(c, db[j].id, c->child.data, c->child.len, db[j].type == TH_TYPE_DIR);
            j++;
        } else {
            rc = sync_child(c, w, ds.st_dev, &fs[i++], &db[j++]);
        }
    }
    free_fs(fs, nfs);
    free_db(db, ndb);
    return rc;
}

/* Walks everything currently on the stack.  Returns 0, 1 when cancelled, -1 on error. */
static int walk(scan_ctx *c)
{
    while (c->nstack > 0) {
        if (cancelled(c))
            return 1;
        work_item w = c->stack[--c->nstack];
        int rc = scan_one_dir(c, &w);
        progress(c, w.path, false);
        free(w.path);
        if (rc != 0)
            return -1;
    }
    return 0;
}

static int refresh_ancestors_for(scan_ctx *c, int64_t entry)
{
    if (!c->o || !c->o->defer_aggregate) return th_index_refresh_ancestors(c->db, entry);
    th_entry e = {0};
    int found = th_db_entry_get(c->db, entry, &e);
    if (found > 0 && e.parent_id) c->o->defer_aggregate(e.parent_id, c->o->ud);
    th_entry_clear(&e);
    return found < 0 ? -1 : 0;
}

/* Only touched hard-link inodes: retain one canonical row per root, and
 * propagate shared metadata without traversing unrelated filesystem paths. */
static int repair_links(scan_ctx *c)
{
    sqlite3_stmt *rows = th_db_prepare(c->db,
        "SELECT e.id,e.parent_id,e.id=(SELECT min(x.id) FROM entries x WHERE x.root_id=e.root_id AND x.dev=e.dev AND x.ino=e.ino AND x.type=0),"
        "coalesce(t.size,e.size),coalesce(t.alloc,e.alloc),coalesce(t.mtime,e.mtime),coalesce(t.nlink,e.nlink),e.flags "
        "FROM temp.scan_links t CROSS JOIN entries e INDEXED BY entries_inode ON e.dev=t.dev AND e.ino=t.ino WHERE e.root_id=? AND e.type=0");
    sqlite3_stmt *upd = th_db_prepare(c->db,
        "UPDATE entries SET flags=?2,size=?3,alloc=?4,agg_size=?3,agg_alloc=?4,mtime=?5,nlink=?6 "
        "WHERE id=?1 AND (flags<>?2 OR size<>?3 OR alloc<>?4 OR mtime<>?5 OR nlink<>?6)");
    if (!rows || !upd) { sqlite3_finalize(rows); sqlite3_finalize(upd); return -1; }
    sqlite3_bind_int64(rows, 1, c->root_id);
    int64_t *changed = NULL; size_t n = 0; int rc;
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(rows, 0);
        int flags = sqlite3_column_int(rows, 7) & ~TH_FLAG_LINKDUP;
        if (!sqlite3_column_int(rows, 2)) flags |= TH_FLAG_LINKDUP;
        sqlite3_reset(upd); sqlite3_bind_int64(upd, 1, id); sqlite3_bind_int(upd, 2, flags);
        for (int i = 3; i <= 6; i++) sqlite3_bind_int64(upd, i, sqlite3_column_int64(rows, i));
        if (sqlite3_step(upd) != SQLITE_DONE) { rc = SQLITE_ERROR; break; }
        if (sqlite3_changes(c->db)) { changed = th_xrealloc(changed, (n + 1) * sizeof *changed); changed[n++] = id; }
    }
    sqlite3_finalize(rows); sqlite3_finalize(upd);
    for (size_t i = 0; rc == SQLITE_DONE && i < n; i++) if (refresh_ancestors_for(c, changed[i]) != 0) rc = SQLITE_ERROR;
    free(changed); return rc == SQLITE_DONE ? 0 : -1;
}

/* Recomputes aggregates for path itself and every directory below it, deepest first. */
static int aggregate_subtree(scan_ctx *c, int64_t top_id, const char *path, size_t len)
{
    th_subtree_range(path, len, &c->lo, &c->hi);
    sqlite3_stmt *s = th_db_prepare(c->db, "SELECT id FROM entries INDEXED BY sqlite_autoindex_entries_1 WHERE root_id = ? AND type = 1 "
                                           "AND path >= ? AND path < ? ORDER BY path DESC");
    if (!s)
        return db_fail(c, "prepare aggregate");
    sqlite3_bind_int64(s, 1, c->root_id);
    th_bind_bytes(s, 2, c->lo.data, c->lo.len);
    th_bind_bytes(s, 3, c->hi.data, c->hi.len);
    int64_t *ids = NULL;
    size_t n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            ids = th_xrealloc(ids, cap * sizeof *ids);
        }
        ids[n++] = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        free(ids);
        return db_fail(c, "list directories");
    }
    int out = 0;
    for (size_t i = 0; i <= n && out == 0; i++) {
        int64_t id = i < n ? ids[i] : top_id;
        int r = refresh_dir_with(c->q_agg_sel, c->q_agg_upd, id);
        if (r < 0)
            out = db_fail(c, "aggregate");
        else if (r > 0)
            out = wrote(c);
    }
    free(ids);
    return out;
}

/* ---- roots ---- */

static int set_root_fields(sqlite3 *db, const char *sql, int64_t id, int64_t a, int64_t b)
{
    sqlite3_stmt *s = th_db_prepare(db, sql);
    if (!s)
        return -1;
    int np = sqlite3_bind_parameter_count(s);
    sqlite3_bind_int64(s, 1, id);
    if (np >= 2)
        sqlite3_bind_int64(s, 2, a);
    if (np >= 3)
        sqlite3_bind_int64(s, 3, b);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

static const char *base_name(const char *path, size_t len, size_t *nlen)
{
    if (len == 1 && path[0] == '/') {
        *nlen = 1;
        return path;
    }
    size_t i = len;
    while (i > 0 && path[i - 1] != '/')
        i--;
    *nlen = len - i;
    return path + i;
}

/* Inserts or refreshes the root's own entry; takes over rows indexed under another root. */
static int ensure_root_entry(scan_ctx *c, const char *path, size_t len, const struct stat *s, int64_t *id_out)
{
    th_entry e;
    int found = th_db_entry_by_path(c->db, path, len, &e);
    if (found < 0)
        return db_fail(c, "find root entry");
    if (found > 0 && e.type != TH_TYPE_DIR) {
        int rc = delete_entry(c, e.id, path, len, false);
        th_entry_clear(&e);
        if (rc != 0)
            return -1;
        found = 0;
    }
    if (found > 0) {
        *id_out = e.id;
        bool foreign = e.root_id != c->root_id || e.parent_id != 0;
        int flags = e.flags & TH_FLAG_DENIED;
        bool differs = e.size != (int64_t)s->st_size || e.alloc != alloc_of(s) ||
                       e.mtime != (int64_t)s->st_mtim.tv_sec || e.dev != (int64_t)s->st_dev ||
                       e.ino != (int64_t)s->st_ino || e.nlink != (int64_t)s->st_nlink || e.flags != flags;
        th_entry_clear(&e);
        if (foreign) {
            th_subtree_range(path, len, &c->lo, &c->hi);
            sqlite3_stmt *q = th_db_prepare(c->db, "UPDATE entries SET root_id = ?1, parent_id = CASE WHEN id = ?2 "
                                                   "THEN 0 ELSE parent_id END WHERE id = ?2 OR "
                                                   "(path >= ?3 AND path < ?4)");
            if (!q)
                return db_fail(c, "adopt entries");
            sqlite3_bind_int64(q, 1, c->root_id);
            sqlite3_bind_int64(q, 2, *id_out);
            th_bind_bytes(q, 3, c->lo.data, c->lo.len);
            th_bind_bytes(q, 4, c->hi.data, c->hi.len);
            int rc = sqlite3_step(q);
            sqlite3_finalize(q);
            if (rc != SQLITE_DONE)
                return db_fail(c, "adopt entries");
            if (wrote(c) != 0)
                return -1;
        }
        if (differs && update_entry(c, *id_out, s, flags) != 0)
            return -1;
        return 0;
    }
    size_t nlen;
    const char *name = base_name(path, len, &nlen);
    return insert_entry(c, 0, name, nlen, path, len, s, 0, id_out);
}

int th_scan_root(sqlite3 *db, int64_t root_id, const th_scan_opts *o, th_scan_stats *st, th_strbuf *err)
{
    th_scan_stats local;
    if (!st)
        st = &local;
    memset(st, 0, sizeof *st);
    th_root r;
    int got = th_db_root_get(db, root_id, &r);
    if (got <= 0) {
        if (err)
            th_sb_printf(err, "unknown root %lld", (long long)root_id);
        return TH_SCAN_FAILED;
    }
    char *path = r.path;
    r.path = NULL;
    th_root_clear(&r);
    size_t len = strlen(path);

    struct stat s;
    if (stat(path, &s) != 0 || !S_ISDIR(s.st_mode)) {
        const char *why = errno ? strerror(errno) : "not a directory";
        th_db_root_set_state(db, root_id, TH_STATE_OFFLINE, why);
        if (err)
            th_sb_printf(err, "%s: %s", path, why);
        free(path);
        return TH_SCAN_OFFLINE;
    }

    scan_ctx c;
    int result = TH_SCAN_FAILED;
    bool ok = ctx_init(&c, db, root_id, o, st, err);
    if (ok)
        ok = set_root_fields(db, "UPDATE roots SET state = 2, scan_in_progress = 1, last_scan_start = ?2, "
                                 "dev = ?3, error = NULL WHERE id = ?1",
                             root_id, th_now(), (int64_t)s.st_dev) == 0;
    bool in_tx = ok && th_db_begin(db) == 0;
    int64_t top = 0;
    int w = -1;
    if (in_tx && ensure_root_entry(&c, path, len, &s, &top) == 0) {
        st->entries++;
        push(&c, top, path, len, true);
        progress(&c, path, true);
        w = walk(&c);
        if (w == 0 && repair_links(&c) != 0) w = -1;
        if (w == 0 && aggregate_subtree(&c, top, path, len) != 0)
            w = -1;
    }
    if (in_tx && w >= 0 && th_db_commit(db) != 0) {
        db_fail(&c, "commit");
        w = -1;
    }
    if (in_tx && w < 0)
        th_db_rollback(db);

    if (w == 0) {
        sqlite3_stmt *q = th_db_prepare(db, "UPDATE roots SET state = 1, scan_in_progress = 0, last_scan_end = ?2, "
                                            "last_verified = ?2, entry_id = ?3, error = NULL WHERE id = ?1");
        if (q) {
            sqlite3_bind_int64(q, 1, root_id);
            sqlite3_bind_int64(q, 2, th_now());
            sqlite3_bind_int64(q, 3, top);
            if (sqlite3_step(q) == SQLITE_DONE)
                result = TH_SCAN_OK;
            sqlite3_finalize(q);
        }
        if (result != TH_SCAN_OK)
            db_fail(&c, "finish root");
    } else if (w == 1) {
        /* partial results are committed; the root stays flagged for a resume */
        set_root_fields(db, "UPDATE roots SET entry_id = ?2 WHERE id = ?1", root_id, top, 0);
        th_db_root_set_state(db, root_id, TH_STATE_STALE, NULL);
        result = TH_SCAN_CANCELLED;
    } else {
        if (err && err->len == 0)
            th_sb_puts(err, sqlite3_errmsg(db));
        th_db_root_set_state(db, root_id, TH_STATE_ERROR, err ? err->data : "scan failed");
    }
    progress(&c, path, true);
    ctx_free(&c);
    free(path);
    return result;
}

int th_scan_subtree(sqlite3 *db, int64_t root_id, const char *path, size_t len, const th_scan_opts *o,
                    th_scan_stats *st, th_strbuf *err)
{
    th_scan_stats local;
    if (!st)
        st = &local;
    memset(st, 0, sizeof *st);

    /* nearest indexed ancestor (or the path itself) belonging to this root */
    th_entry e;
    memset(&e, 0, sizeof e);
    size_t l = len;
    while (len > 1 && l > 0 && path[l - 1] == '/')
        l--;
    int found = 0;
    while (l > 0) {
        found = th_db_entry_by_path(db, path, l, &e);
        if (found < 0) {
            if (err)
                th_sb_printf(err, "lookup failed: %s", sqlite3_errmsg(db));
            return TH_SCAN_FAILED;
        }
        if (found > 0 && e.root_id == root_id && e.type == TH_TYPE_DIR)
            break;
        if (found > 0)
            th_entry_clear(&e);
        found = 0;
        if (l == 1)
            break;
        while (l > 0 && path[l - 1] != '/')
            l--;
        while (l > 1 && path[l - 1] == '/')
            l--;
    }
    if (!found)
        return th_scan_root(db, root_id, o, st, err);

    scan_ctx c;
    int result = TH_SCAN_FAILED;
    int64_t id = e.id, parent = e.parent_id;
    int64_t dev_parent = 0;
    char *dir = th_xmemdup(e.path, e.path_len);
    size_t dlen = e.path_len;
    th_entry_clear(&e);
    struct stat s;
    bool is_top = parent == 0;
    if (is_top && (stat(dir, &s) != 0 || !S_ISDIR(s.st_mode))) {
        free(dir);
        return th_scan_root(db, root_id, o, st, err);
    }
    /* a vanished directory may have taken indexed ancestors with it: drop the highest one */
    while (!is_top && (lstat(dir, &s) != 0 || !S_ISDIR(s.st_mode))) {
        th_entry pe;
        if (th_db_entry_get(db, parent, &pe) <= 0)
            break;
        struct stat ps;
        bool gone = pe.parent_id != 0 && (lstat(pe.path, &ps) != 0 || !S_ISDIR(ps.st_mode));
        if (gone) {
            free(dir);
            dir = th_xmemdup(pe.path, pe.path_len);
            dlen = pe.path_len;
            id = pe.id;
            parent = pe.parent_id;
        }
        th_entry_clear(&pe);
        if (!gone)
            break;
    }
    if (!ctx_init(&c, db, root_id, o, st, err) || th_db_begin(db) != 0) {
        db_fail(&c, "begin");
        ctx_free(&c);
        free(dir);
        return TH_SCAN_FAILED;
    }
    int w = 0;
    if (!is_top && (lstat(dir, &s) != 0 || !S_ISDIR(s.st_mode))) {
        w = delete_entry(&c, id, dir, dlen, true) != 0 ? -1 : 0;
        id = 0;
    } else {
        /* refresh the directory's own row against its parent's device */
        th_entry pe;
        if (!is_top && th_db_entry_get(db, parent, &pe) > 0) {
            dev_parent = pe.dev;
            th_entry_clear(&pe);
        } else {
            dev_parent = (int64_t)s.st_dev;
        }
        db_child d;
        memset(&d, 0, sizeof d);
        th_entry cur;
        if (th_db_entry_get(db, id, &cur) > 0) {
            d.id = cur.id;
            d.type = cur.type;
            d.size = cur.size;
            d.alloc = cur.alloc;
            d.mtime = cur.mtime;
            d.dev = cur.dev;
            d.ino = cur.ino;
            d.nlink = cur.nlink;
            d.flags = cur.flags;
            th_entry_clear(&cur);
        }
        bool descend;
        int flags = classify(&c, dir, dlen, &s, (dev_t)dev_parent, &descend);
        if (is_top)
            flags &= ~TH_FLAG_MOUNTPOINT, descend = true;
        if (descend)
            flags |= d.flags & TH_FLAG_DENIED;
        if (row_differs(&d, &s, flags) && update_entry(&c, id, &s, flags) != 0)
            w = -1;
        if (w == 0) {
            if (descend) {
                push(&c, id, dir, dlen, is_top);
                w = walk(&c);
            } else if (delete_below(&c, dir, dlen) != 0) {
                w = -1;
            }
        }
        if (w == 0 && repair_links(&c) != 0) w = -1;
        if (w == 0 && aggregate_subtree(&c, id, dir, dlen) != 0)
            w = -1;
    }
    if (w == 0 && !id && repair_links(&c) != 0) w = -1;
    if (w >= 0 && th_db_commit(db) != 0) {
        db_fail(&c, "commit");
        w = -1;
    }
    if (w < 0) {
        th_db_rollback(db);
    } else {
        int rc;
        if (id)
            rc = refresh_ancestors_for(&c, id);
        else if (o && o->defer_aggregate) { o->defer_aggregate(parent, o->ud); rc = 0; }
        else
            rc = th_index_refresh_dir(db, parent) == 0 ? th_index_refresh_ancestors(db, parent) : -1;
        if (rc != 0)
            db_fail(&c, "refresh ancestors");
        result = rc != 0 ? TH_SCAN_FAILED : w == 1 ? TH_SCAN_CANCELLED : TH_SCAN_OK;
    }
    if (result == TH_SCAN_FAILED && err && err->len == 0)
        th_sb_puts(err, sqlite3_errmsg(db));
    ctx_free(&c);
    free(dir);
    return result;
}
