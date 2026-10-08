/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/db.h"
#include "treehound/util.h"

#include <locale.h>
#include <unistd.h>

static char dbdir[] = "/tmp/th-test-db-XXXXXX";

static char *db_path(const char *name)
{
    return th_path_join(dbdir, name);
}

static int64_t add_entry(sqlite3 *db, int64_t root, int64_t parent, const char *name, size_t nlen,
                         const char *path, size_t plen, int type, int64_t size)
{
    sqlite3_stmt *st = th_db_prepare(db, "INSERT INTO entries(root_id, parent_id, name, path, type, size) "
                                         "VALUES (?, ?, ?, ?, ?, ?)");
    REQUIRE(st);
    sqlite3_bind_int64(st, 1, root);
    sqlite3_bind_int64(st, 2, parent);
    th_bind_bytes(st, 3, name, nlen);
    th_bind_bytes(st, 4, path, plen);
    sqlite3_bind_int(st, 5, type);
    sqlite3_bind_int64(st, 6, size);
    CHECK_INT(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

#define ADD(db, r, p, name, path, t, sz) add_entry(db, r, p, name, strlen(name), path, strlen(path), t, sz)

static void test_open_migrate(void)
{
    char *p = db_path("a.sqlite3");
    th_strbuf err;
    th_sb_init(&err);
    sqlite3 *db = th_db_open(p, false, &err);
    REQUIRE(db);
    CHECK_INT(th_db_schema_version(db), TH_SCHEMA_VERSION);
    CHECK_INT(th_db_migrate(db, &err), 0); /* idempotent */
    CHECK_INT(th_db_meta_set(db, "k", "v1"), 0);
    CHECK_INT(th_db_meta_set(db, "k", "v2"), 0);
    char *v = th_db_meta_get(db, "k");
    CHECK_STR(v, "v2");
    free(v);
    CHECK(th_db_meta_get(db, "missing") == NULL);
    CHECK_INT(th_db_check(db, true, &err), 0);
    th_db_close(db);

    /* data survives reopen */
    db = th_db_open(p, false, &err);
    REQUIRE(db);
    v = th_db_meta_get(db, "k");
    CHECK_STR(v, "v2");
    free(v);
    /* pretend a newer release wrote it */
    CHECK_INT(th_db_exec(db, "PRAGMA user_version = 99"), 0);
    th_db_close(db);
    th_sb_reset(&err);
    db = th_db_open(p, false, &err);
    CHECK(db == NULL);
    CHECK(strstr(err.data, "newer") != NULL);
    th_sb_reset(&err);
    db = th_db_open(p, true, &err);
    CHECK(db == NULL);
    th_sb_free(&err);
    free(p);
}

static void test_entries(void)
{
    char *p = db_path("b.sqlite3");
    th_strbuf err;
    th_sb_init(&err);
    sqlite3 *db = th_db_open(p, false, &err);
    REQUIRE(db);
    int64_t r = th_db_root_ensure(db, "/home/u");
    CHECK(r > 0);
    CHECK_INT(th_db_root_ensure(db, "/home/u"), r);
    int64_t top = ADD(db, r, 0, "u", "/home/u", TH_TYPE_DIR, 0);
    ADD(db, r, top, "Report 2024.pdf", "/home/u/Report 2024.pdf", TH_TYPE_FILE, 100);
    ADD(db, r, top, "été.txt", "/home/u/été.txt", TH_TYPE_FILE, 5);
    int64_t bad = add_entry(db, r, top, "bad\xff.bin", 8, "/home/u/bad\xff.bin", 16, TH_TYPE_FILE, 7);
    ADD(db, r, top, "sub", "/home/u/sub", TH_TYPE_DIR, 0);
    ADD(db, r, top, "sub0", "/home/u/sub0", TH_TYPE_DIR, 0);
    ADD(db, r, top, "x", "/home/u/sub/x", TH_TYPE_FILE, 1);
    ADD(db, r, top, "y", "/home/u/sub/deep/y", TH_TYPE_FILE, 1);
    sqlite3_stmt *st = th_db_prepare(db, "UPDATE roots SET entry_id = ? WHERE id = ?");
    sqlite3_bind_int64(st, 1, top);
    sqlite3_bind_int64(st, 2, r);
    CHECK_INT(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    CHECK_INT(th_db_entry_count(db), 8);

    /* byte-exact round trip of invalid UTF-8 */
    th_entry e;
    REQUIRE(th_db_entry_by_path(db, "/home/u/bad\xff.bin", 16, &e) == 1);
    CHECK_INT(e.id, bad);
    CHECK_INT(e.name_len, 8);
    CHECK(memcmp(e.name, "bad\xff.bin", 8) == 0);
    CHECK_INT(e.size, 7);
    th_entry_clear(&e);
    CHECK_INT(th_db_entry_get(db, 999999, &e), 0);

    /* FTS trigram candidates confirmed by th_match */
    st = th_db_prepare(db, "SELECT e.path FROM entries_fts f JOIN entries e ON e.id = f.rowid "
                           "WHERE entries_fts MATCH ? AND th_match(?, e.name, 0)");
    REQUIRE(st);
    th_bind_str(st, 1, "\"rep\"");
    th_bind_str(st, 2, "rep");
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        CHECK_STR((const char *)sqlite3_column_text(st, 0), "/home/u/Report 2024.pdf");
        n++;
    }
    CHECK_INT(n, 1);
    sqlite3_reset(st);
    th_bind_str(st, 1, "\"\xc3\x89t\xc3\xa9\""); /* ÉTÉ is not a trigram match... */
    th_bind_str(st, 2, "ÉTÉ");
    n = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
        n++;
    CHECK(n <= 1); /* trigram folds ASCII only; search layer handles non-ASCII via scan */
    sqlite3_finalize(st);

    /* th_match over every row (glob, case-insensitive) */
    st = th_db_prepare(db, "SELECT count(*) FROM entries WHERE th_match('*.PDF', name, 0)");
    REQUIRE(st && sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
    st = th_db_prepare(db, "SELECT count(*) FROM entries WHERE th_match('ÉTÉ', name, 0)");
    REQUIRE(st && sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    /* subtree range selects /home/u/sub/... but not /home/u/sub0 */
    th_strbuf lo, hi;
    th_sb_init(&lo);
    th_sb_init(&hi);
    th_subtree_range("/home/u/sub", 11, &lo, &hi);
    st = th_db_prepare(db, "SELECT count(*) FROM entries WHERE path >= ? AND path < ?");
    th_bind_bytes(st, 1, lo.data, lo.len);
    th_bind_bytes(st, 2, hi.data, hi.len);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 2);
    sqlite3_reset(st);
    th_subtree_range("/", 1, &lo, &hi);
    th_bind_bytes(st, 1, lo.data, lo.len);
    th_bind_bytes(st, 2, hi.data, hi.len);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 8);
    sqlite3_finalize(st);
    th_sb_free(&lo);
    th_sb_free(&hi);

    /* rename keeps FTS in sync */
    CHECK_INT(th_db_exec(db, "UPDATE entries SET name = 'renamed.doc' WHERE name = 'x'"), 0);
    CHECK_INT(th_db_check(db, false, &err), 0);

    /* roots */
    th_root *roots;
    size_t nroots;
    CHECK_INT(th_db_roots(db, &roots, &nroots), 0);
    CHECK_INT(nroots, 1);
    CHECK_STR(roots[0].path, "/home/u");
    CHECK_INT(roots[0].entry_id, top);
    th_roots_free(roots, nroots);
    CHECK_INT(th_db_root_set_state(db, r, TH_STATE_ERROR, "boom"), 0);
    th_root root;
    CHECK_INT(th_db_root_get(db, r, &root), 1);
    CHECK_INT(root.state, TH_STATE_ERROR);
    CHECK_STR(root.error, "boom");
    th_root_clear(&root);

    /* cascade delete removes entries and their FTS rows */
    CHECK_INT(th_db_root_delete(db, r), 0);
    CHECK_INT(th_db_entry_count(db), 0);
    CHECK_INT(th_db_check(db, true, &err), 0);
    CHECK_INT(th_db_root_find(db, "/home/u"), 0);

    /* rollback discards a half-done batch */
    CHECK_INT(th_db_begin(db), 0);
    r = th_db_root_ensure(db, "/tmp/r");
    ADD(db, r, 0, "r", "/tmp/r", TH_TYPE_DIR, 0);
    th_db_rollback(db);
    CHECK_INT(th_db_entry_count(db), 0);
    CHECK_INT(th_db_root_find(db, "/tmp/r"), 0);

    th_db_close(db);
    th_sb_free(&err);
    free(p);
}

int main(void)
{
    setlocale(LC_CTYPE, "C.UTF-8");
    th_log_init("test_db");
    REQUIRE(mkdtemp(dbdir));
    RUN(test_open_migrate);
    RUN(test_entries);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dbdir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");
    return TEST_EXIT();
}
