/* SPDX-License-Identifier: MIT */
#include "treehound/db.h"
#include "treehound/match.h"
#include "treehound/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Schema history.  Each step upgrades from version i to i+1 and runs inside
 * the migration transaction; steps never drop user data.
 */
static const char *const migrations[] = {
    /* 0 -> 1: initial schema */
    "CREATE TABLE meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ") WITHOUT ROWID;"
    "CREATE TABLE roots ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  dev INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  scan_in_progress INTEGER NOT NULL DEFAULT 0,"
    "  added_at INTEGER NOT NULL DEFAULT 0,"
    "  last_scan_start INTEGER NOT NULL DEFAULT 0,"
    "  last_scan_end INTEGER NOT NULL DEFAULT 0,"
    "  last_verified INTEGER NOT NULL DEFAULT 0,"
    "  error TEXT,"
    "  entry_id INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE entries ("
    "  id INTEGER PRIMARY KEY,"
    "  root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
    "  parent_id INTEGER NOT NULL DEFAULT 0,"
    "  name TEXT NOT NULL,"
    "  path TEXT NOT NULL UNIQUE,"
    "  type INTEGER NOT NULL,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  alloc INTEGER NOT NULL DEFAULT 0,"
    "  agg_size INTEGER NOT NULL DEFAULT 0,"
    "  agg_alloc INTEGER NOT NULL DEFAULT 0,"
    "  agg_files INTEGER NOT NULL DEFAULT 0,"
    "  agg_dirs INTEGER NOT NULL DEFAULT 0,"
    "  mtime INTEGER NOT NULL DEFAULT 0,"
    "  dev INTEGER NOT NULL DEFAULT 0,"
    "  ino INTEGER NOT NULL DEFAULT 0,"
    "  nlink INTEGER NOT NULL DEFAULT 1,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  flags INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX entries_parent ON entries(parent_id);"
    "CREATE INDEX entries_inode ON entries(dev, ino);"
    "CREATE INDEX entries_size ON entries(size);"
    "CREATE INDEX entries_root ON entries(root_id);"
    "CREATE VIRTUAL TABLE entries_fts USING fts5("
    "  name, content='entries', content_rowid='id', tokenize='trigram');"
    "CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN"
    "  INSERT INTO entries_fts(rowid, name) VALUES (new.id, new.name);"
    "END;"
    "CREATE TRIGGER entries_ad AFTER DELETE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, name) VALUES ('delete', old.id, old.name);"
    "END;"
    "CREATE TRIGGER entries_au AFTER UPDATE OF name ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, name) VALUES ('delete', old.id, old.name);"
    "  INSERT INTO entries_fts(rowid, name) VALUES (new.id, new.name);"
    "END;",
};

_Static_assert(TH_ARRAY_LEN(migrations) == TH_SCHEMA_VERSION, "one migration per schema version");

int th_db_exec(sqlite3 *db, const char *sql)
{
    char *msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        th_log(TH_LOG_ERROR, "sqlite: %s", msg ? msg : sqlite3_errstr(rc));
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

sqlite3_stmt *th_db_prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        th_log(TH_LOG_ERROR, "sqlite prepare: %s", sqlite3_errmsg(db));
        return NULL;
    }
    return st;
}

int th_db_begin(sqlite3 *db)
{
    return th_db_exec(db, "BEGIN IMMEDIATE");
}

int th_db_commit(sqlite3 *db)
{
    return th_db_exec(db, "COMMIT");
}

void th_db_rollback(sqlite3 *db)
{
    if (!sqlite3_get_autocommit(db))
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
}

int th_db_schema_version(sqlite3 *db)
{
    sqlite3_stmt *st = th_db_prepare(db, "PRAGMA user_version");
    int v = -1;
    if (st && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

int th_db_migrate(sqlite3 *db, th_strbuf *err)
{
    int v = th_db_schema_version(db);
    if (v < 0) {
        th_sb_printf(err, "cannot read schema version: %s", sqlite3_errmsg(db));
        return -1;
    }
    if (v > TH_SCHEMA_VERSION) {
        th_sb_printf(err, "database schema %d is newer than supported (%d); upgrade treehound", v,
                     TH_SCHEMA_VERSION);
        return -1;
    }
    if (v == TH_SCHEMA_VERSION)
        return 0;
    if (th_db_begin(db) != 0) {
        th_sb_printf(err, "cannot start migration: %s", sqlite3_errmsg(db));
        return -1;
    }
    for (int i = v; i < TH_SCHEMA_VERSION; i++) {
        char *msg = NULL;
        if (sqlite3_exec(db, migrations[i], NULL, NULL, &msg) != SQLITE_OK) {
            th_sb_printf(err, "migration %d -> %d failed: %s", i, i + 1, msg ? msg : "?");
            sqlite3_free(msg);
            th_db_rollback(db);
            return -1;
        }
        th_log(TH_LOG_INFO, "database migrated to schema %d", i + 1);
    }
    char sql[64];
    snprintf(sql, sizeof sql, "PRAGMA user_version = %d", TH_SCHEMA_VERSION);
    if (th_db_exec(db, sql) != 0 || th_db_commit(db) != 0) {
        th_sb_printf(err, "cannot finish migration: %s", sqlite3_errmsg(db));
        th_db_rollback(db);
        return -1;
    }
    return 0;
}

/* th_match(pattern, text, flags): exact verification of FTS candidates. */
static void free_pattern(void *p)
{
    th_pattern_free(p);
    free(p);
}

static void sql_th_match(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    TH_UNUSED(argc);
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_int(ctx, 0);
        return;
    }
    th_pattern *p = sqlite3_get_auxdata(ctx, 0);
    if (!p) {
        const char *pat = (const char *)sqlite3_value_text(argv[0]);
        size_t plen = (size_t)sqlite3_value_bytes(argv[0]);
        p = th_xmalloc(sizeof *p);
        th_pattern_compile(p, pat ? pat : "", pat ? plen : 0, sqlite3_value_int(argv[2]));
        sqlite3_set_auxdata(ctx, 0, p, free_pattern);
        /* set_auxdata may free p immediately on OOM */
        p = sqlite3_get_auxdata(ctx, 0);
        if (!p) {
            sqlite3_result_error_nomem(ctx);
            return;
        }
    }
    const char *text = (const char *)sqlite3_value_text(argv[1]);
    size_t tlen = (size_t)sqlite3_value_bytes(argv[1]);
    sqlite3_result_int(ctx, text && th_pattern_match(p, text, tlen));
}

void th_db_register_functions(sqlite3 *db)
{
    sqlite3_create_function(db, "th_match", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_th_match,
                            NULL, NULL);
}

sqlite3 *th_db_open(const char *path, bool readonly, th_strbuf *err)
{
    sqlite3 *db = NULL;
    int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        th_sb_printf(err, "%s: %s", path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 5000);
    th_db_register_functions(db);
    if (th_db_exec(db, "PRAGMA foreign_keys = ON") != 0)
        goto fail;
    if (!readonly) {
        if (th_db_exec(db, "PRAGMA journal_mode = WAL") != 0 ||
            th_db_exec(db, "PRAGMA synchronous = NORMAL") != 0)
            goto fail;
        if (th_db_migrate(db, err) != 0) {
            sqlite3_close(db);
            return NULL;
        }
    } else {
        th_db_exec(db, "PRAGMA query_only = ON");
        int v = th_db_schema_version(db);
        if (v != TH_SCHEMA_VERSION) {
            th_sb_printf(err, "%s: unexpected schema version %d (want %d)", path, v, TH_SCHEMA_VERSION);
            sqlite3_close(db);
            return NULL;
        }
    }
    th_db_exec(db, "PRAGMA cache_size = -16000"); /* 16 MiB per connection */
    th_db_exec(db, "PRAGMA temp_store = MEMORY");
    return db;
fail:
    th_sb_printf(err, "%s: %s", path, sqlite3_errmsg(db));
    sqlite3_close(db);
    return NULL;
}

void th_db_close(sqlite3 *db)
{
    if (!db)
        return;
    /* Every caller finalizes its own statements; FTS5 owns internal ones that
     * must not be finalized from outside, so only report leaks here. */
    if (sqlite3_close(db) != SQLITE_OK) {
        th_log(TH_LOG_WARN, "closing database with unfinalized statements");
        sqlite3_close_v2(db);
    }
}

int th_db_check(sqlite3 *db, bool full, th_strbuf *msg)
{
    sqlite3_stmt *st = th_db_prepare(db, full ? "PRAGMA integrity_check" : "PRAGMA quick_check");
    if (!st)
        return -1;
    int bad = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *row = (const char *)sqlite3_column_text(st, 0);
        if (row && strcmp(row, "ok") != 0) {
            bad = 1;
            if (msg)
                th_sb_printf(msg, "%s\n", row);
        }
    }
    sqlite3_finalize(st);
    if (!bad) {
        /* also verify the full-text index matches its content table */
        if (sqlite3_exec(db, "INSERT INTO entries_fts(entries_fts, rank) VALUES ('integrity-check', 1)",
                         NULL, NULL, NULL) != SQLITE_OK) {
            bad = 1;
            if (msg)
                th_sb_printf(msg, "fts: %s\n", sqlite3_errmsg(db));
        }
    }
    return bad ? -1 : 0;
}

char *th_db_meta_get(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st = th_db_prepare(db, "SELECT value FROM meta WHERE key = ?");
    if (!st)
        return NULL;
    th_bind_str(st, 1, key);
    char *v = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = th_column_dup(st, 0, NULL);
    sqlite3_finalize(st);
    return v;
}

int th_db_meta_set(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *st = th_db_prepare(db, "INSERT INTO meta(key, value) VALUES (?, ?) "
                                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!st)
        return -1;
    th_bind_str(st, 1, key);
    th_bind_str(st, 2, value);
    int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

void th_bind_bytes(sqlite3_stmt *st, int idx, const char *s, size_t len)
{
    sqlite3_bind_text64(st, idx, s, (sqlite3_uint64)len, SQLITE_TRANSIENT, SQLITE_UTF8);
}

void th_bind_str(sqlite3_stmt *st, int idx, const char *s)
{
    if (s)
        th_bind_bytes(st, idx, s, strlen(s));
    else
        sqlite3_bind_null(st, idx);
}

char *th_column_dup(sqlite3_stmt *st, int col, size_t *len)
{
    if (sqlite3_column_type(st, col) == SQLITE_NULL) {
        if (len)
            *len = 0;
        return NULL;
    }
    const unsigned char *t = sqlite3_column_text(st, col);
    size_t n = (size_t)sqlite3_column_bytes(st, col);
    if (len)
        *len = n;
    return th_xmemdup(t ? (const char *)t : "", t ? n : 0);
}

void th_entry_from_stmt(th_entry *e, sqlite3_stmt *st, int c)
{
    e->id = sqlite3_column_int64(st, c + 0);
    e->root_id = sqlite3_column_int64(st, c + 1);
    e->parent_id = sqlite3_column_int64(st, c + 2);
    e->name = th_column_dup(st, c + 3, &e->name_len);
    e->path = th_column_dup(st, c + 4, &e->path_len);
    e->type = sqlite3_column_int(st, c + 5);
    e->size = sqlite3_column_int64(st, c + 6);
    e->alloc = sqlite3_column_int64(st, c + 7);
    e->agg_size = sqlite3_column_int64(st, c + 8);
    e->agg_alloc = sqlite3_column_int64(st, c + 9);
    e->agg_files = sqlite3_column_int64(st, c + 10);
    e->agg_dirs = sqlite3_column_int64(st, c + 11);
    e->mtime = sqlite3_column_int64(st, c + 12);
    e->dev = sqlite3_column_int64(st, c + 13);
    e->ino = sqlite3_column_int64(st, c + 14);
    e->nlink = sqlite3_column_int64(st, c + 15);
    e->state = sqlite3_column_int(st, c + 16);
    e->flags = sqlite3_column_int(st, c + 17);
}

void th_entry_clear(th_entry *e)
{
    free(e->name);
    free(e->path);
    memset(e, 0, sizeof *e);
}

void th_subtree_range(const char *dir, size_t len, th_strbuf *lo, th_strbuf *hi)
{
    th_sb_reset(lo);
    th_sb_reset(hi);
    if (len == 1 && dir[0] == '/') {
        /* everything except "/" itself; paths never contain NUL */
        th_sb_append(lo, "/\x01", 2);
        th_sb_putc(hi, '0'); /* '/' + 1 */
        return;
    }
    th_sb_append(lo, dir, len);
    th_sb_putc(lo, '/');
    th_sb_append(hi, dir, len);
    th_sb_putc(hi, '0');
}

/* ---- roots ---- */

#define ROOT_COLS                                                                                      \
    "r.id, r.path, r.dev, r.state, r.scan_in_progress, r.added_at, r.last_scan_start, r.last_scan_end, " \
    "r.last_verified, r.error, r.entry_id, coalesce(e.agg_size, 0), coalesce(e.agg_alloc, 0), "          \
    "coalesce(e.agg_files, 0), coalesce(e.agg_dirs, 0)"
#define ROOT_FROM " FROM roots r LEFT JOIN entries e ON e.id = r.entry_id"

static void root_from_stmt(th_root *r, sqlite3_stmt *st)
{
    r->id = sqlite3_column_int64(st, 0);
    r->path = th_column_dup(st, 1, NULL);
    r->dev = sqlite3_column_int64(st, 2);
    r->state = sqlite3_column_int(st, 3);
    r->scan_in_progress = sqlite3_column_int(st, 4) != 0;
    r->added_at = sqlite3_column_int64(st, 5);
    r->last_scan_start = sqlite3_column_int64(st, 6);
    r->last_scan_end = sqlite3_column_int64(st, 7);
    r->last_verified = sqlite3_column_int64(st, 8);
    r->error = th_column_dup(st, 9, NULL);
    r->entry_id = sqlite3_column_int64(st, 10);
    r->total_size = sqlite3_column_int64(st, 11);
    r->total_alloc = sqlite3_column_int64(st, 12);
    r->total_files = sqlite3_column_int64(st, 13);
    r->total_dirs = sqlite3_column_int64(st, 14);
}

void th_root_clear(th_root *r)
{
    free(r->path);
    free(r->error);
    memset(r, 0, sizeof *r);
}

int th_db_roots(sqlite3 *db, th_root **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st = th_db_prepare(db, "SELECT " ROOT_COLS ROOT_FROM " ORDER BY r.path");
    if (!st)
        return -1;
    size_t cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (*n == cap) {
            cap = cap ? cap * 2 : 4;
            *out = th_xrealloc(*out, cap * sizeof **out);
        }
        root_from_stmt(&(*out)[(*n)++], st);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

void th_roots_free(th_root *roots, size_t n)
{
    for (size_t i = 0; i < n; i++)
        th_root_clear(&roots[i]);
    free(roots);
}

int th_db_root_get(sqlite3 *db, int64_t id, th_root *out)
{
    memset(out, 0, sizeof *out);
    sqlite3_stmt *st = th_db_prepare(db, "SELECT " ROOT_COLS ROOT_FROM " WHERE r.id = ?");
    if (!st)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    int step = sqlite3_step(st);
    int rc = step == SQLITE_DONE ? 0 : -1;
    if (step == SQLITE_ROW) {
        root_from_stmt(out, st);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int64_t th_db_root_find(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = th_db_prepare(db, "SELECT id FROM roots WHERE path = ?");
    if (!st)
        return -1;
    th_bind_str(st, 1, path);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}

int64_t th_db_root_ensure(sqlite3 *db, const char *path)
{
    int64_t id = th_db_root_find(db, path);
    if (id != 0)
        return id;
    sqlite3_stmt *st = th_db_prepare(db, "INSERT INTO roots(path, added_at) VALUES (?, ?)");
    if (!st)
        return -1;
    th_bind_str(st, 1, path);
    sqlite3_bind_int64(st, 2, th_now());
    id = sqlite3_step(st) == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
    sqlite3_finalize(st);
    return id;
}

int th_db_root_delete(sqlite3 *db, int64_t id)
{
    sqlite3_stmt *st = th_db_prepare(db, "DELETE FROM roots WHERE id = ?");
    if (!st)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

int th_db_root_set_state(sqlite3 *db, int64_t id, int state, const char *error)
{
    sqlite3_stmt *st = th_db_prepare(db, "UPDATE roots SET state = ?, error = ? WHERE id = ?");
    if (!st)
        return -1;
    sqlite3_bind_int(st, 1, state);
    th_bind_str(st, 2, error);
    sqlite3_bind_int64(st, 3, id);
    int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* ---- entries ---- */

int th_db_entry_get(sqlite3 *db, int64_t id, th_entry *out)
{
    memset(out, 0, sizeof *out);
    sqlite3_stmt *st = th_db_prepare(db, "SELECT " TH_ENTRY_COLS " FROM entries e WHERE e.id = ?");
    if (!st)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    int step = sqlite3_step(st);
    int rc = step == SQLITE_DONE ? 0 : -1;
    if (step == SQLITE_ROW) {
        th_entry_from_stmt(out, st, 0);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int th_db_entry_by_path(sqlite3 *db, const char *path, size_t len, th_entry *out)
{
    memset(out, 0, sizeof *out);
    sqlite3_stmt *st = th_db_prepare(db, "SELECT " TH_ENTRY_COLS " FROM entries e WHERE e.path = ?");
    if (!st)
        return -1;
    th_bind_bytes(st, 1, path, len);
    int step = sqlite3_step(st);
    int rc = step == SQLITE_DONE ? 0 : -1;
    if (step == SQLITE_ROW) {
        th_entry_from_stmt(out, st, 0);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int64_t th_db_entry_count(sqlite3 *db)
{
    sqlite3_stmt *st = th_db_prepare(db, "SELECT count(*) FROM entries");
    int64_t n = -1;
    if (st && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}
