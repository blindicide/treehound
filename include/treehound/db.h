/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_DB_H
#define TREEHOUND_DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "treehound/common.h"
#include "treehound/strbuf.h"

/*
 * SQLite index database (see docs/DATABASE.md).
 *
 * Only treehoundd opens the database read-write.  The schema version lives in
 * PRAGMA user_version; th_db_open() upgrades older schemas inside a single
 * transaction and refuses to open databases written by a newer release.
 *
 * Names and paths are stored as TEXT holding the raw bytes from the kernel,
 * which may be invalid UTF-8.  Every comparison uses the BINARY collation, so
 * prefix ranges over `path` select exactly one subtree.
 */

#define TH_SCHEMA_VERSION 1

/* Column list matching th_entry_from_stmt(). */
#define TH_ENTRY_COLS                                                                    \
    "e.id, e.root_id, e.parent_id, e.name, e.path, e.type, e.size, e.alloc, e.agg_size, " \
    "e.agg_alloc, e.agg_files, e.agg_dirs, e.mtime, e.dev, e.ino, e.nlink, e.state, e.flags"
#define TH_ENTRY_NCOLS 18

typedef struct {
    int64_t id;
    int64_t root_id;
    int64_t parent_id; /* 0 for a root's top entry */
    char *name;
    size_t name_len;
    char *path;
    size_t path_len;
    int type;
    int64_t size;  /* logical size (st_size) */
    int64_t alloc; /* allocated size (st_blocks * 512) */
    int64_t agg_size;
    int64_t agg_alloc;
    int64_t agg_files;
    int64_t agg_dirs;
    int64_t mtime;
    int64_t dev;
    int64_t ino;
    int64_t nlink;
    int state;
    int flags;
} th_entry;

typedef struct {
    int64_t id;
    char *path;
    int64_t dev;
    int state;
    bool scan_in_progress;
    int64_t added_at;
    int64_t last_scan_start;
    int64_t last_scan_end;
    int64_t last_verified;
    char *error;
    int64_t entry_id; /* top entry, 0 when never scanned */
    /* aggregate of the top entry (copied for convenience) */
    int64_t total_size;
    int64_t total_alloc;
    int64_t total_files;
    int64_t total_dirs;
} th_root;

/*
 * Opens (creating when needed) the database.  Read-write handles enable WAL,
 * run migrations and register the SQL helper functions; read-only handles only
 * register the functions.  Returns NULL with a message in err on failure.
 */
sqlite3 *th_db_open(const char *path, bool readonly, th_strbuf *err);
void th_db_close(sqlite3 *db);

/* Brings the schema to TH_SCHEMA_VERSION.  Exposed for tests. */
int th_db_migrate(sqlite3 *db, th_strbuf *err);
int th_db_schema_version(sqlite3 *db);

/* Registers th_match(pattern, text, flags) used by searches. */
void th_db_register_functions(sqlite3 *db);

int th_db_exec(sqlite3 *db, const char *sql);
sqlite3_stmt *th_db_prepare(sqlite3 *db, const char *sql);
int th_db_begin(sqlite3 *db);
int th_db_commit(sqlite3 *db);
void th_db_rollback(sqlite3 *db);

/* Runs PRAGMA quick_check (or integrity_check when full); 0 when ok. */
int th_db_check(sqlite3 *db, bool full, th_strbuf *msg);

/* meta key/value store */
char *th_db_meta_get(sqlite3 *db, const char *key);
int th_db_meta_set(sqlite3 *db, const char *key, const char *value);

/* ---- binding helpers ---- */
void th_bind_bytes(sqlite3_stmt *st, int idx, const char *s, size_t len);
void th_bind_str(sqlite3_stmt *st, int idx, const char *s);
/* Copies a TEXT column (may contain any byte except NUL). */
char *th_column_dup(sqlite3_stmt *st, int col, size_t *len);

/* Fills e from a row selected with TH_ENTRY_COLS starting at column first. */
void th_entry_from_stmt(th_entry *e, sqlite3_stmt *st, int first);
void th_entry_clear(th_entry *e);

/* Computes the half-open [lo, hi) range of paths strictly below dir. */
void th_subtree_range(const char *dir, size_t len, th_strbuf *lo, th_strbuf *hi);

/* ---- roots ---- */
int th_db_roots(sqlite3 *db, th_root **out, size_t *n);
void th_roots_free(th_root *roots, size_t n);
int th_db_root_get(sqlite3 *db, int64_t id, th_root *out);
/* Returns the id of the root with this path, inserting it when absent. */
int64_t th_db_root_ensure(sqlite3 *db, const char *path);
int64_t th_db_root_find(sqlite3 *db, const char *path);
/* Deletes the root and all its entries. */
int th_db_root_delete(sqlite3 *db, int64_t id);
int th_db_root_set_state(sqlite3 *db, int64_t id, int state, const char *error);
void th_root_clear(th_root *r);

/* ---- entries ---- */
int th_db_entry_get(sqlite3 *db, int64_t id, th_entry *out);
int th_db_entry_by_path(sqlite3 *db, const char *path, size_t len, th_entry *out);
/* Number of rows in entries. */
int64_t th_db_entry_count(sqlite3 *db);

#endif
