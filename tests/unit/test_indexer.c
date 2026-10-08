/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/db.h"
#include "treehound/indexer.h"
#include "treehound/mounts.h"
#include "treehound/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/th-test-idx-XXXXXX";

static char *tpath(const char *rel)
{
    return th_path_join(tmpdir, rel);
}

static void mk_dir(const char *rel)
{
    char *p = tpath(rel);
    REQUIRE(mkdir(p, 0755) == 0 || errno == EEXIST);
    free(p);
}

static void mk_file(const char *rel, size_t size)
{
    char *p = tpath(rel);
    FILE *f = fopen(p, "wb");
    REQUIRE(f);
    for (size_t i = 0; i < size; i++)
        fputc('x', f);
    fclose(f);
    free(p);
}

static void rm_tree(const char *path)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s'", path, path);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup of %s failed\n", path);
}

static sqlite3 *open_db(const char *name)
{
    char *p = tpath(name);
    th_strbuf err;
    th_sb_init(&err);
    sqlite3 *db = th_db_open(p, false, &err);
    if (!db)
        fprintf(stderr, "open: %s\n", err.data);
    REQUIRE(db);
    th_sb_free(&err);
    free(p);
    return db;
}

static int scan(sqlite3 *db, int64_t root, const th_scan_opts *o, th_scan_stats *st)
{
    th_strbuf err;
    th_sb_init(&err);
    th_scan_stats local;
    int rc = th_scan_root(db, root, o, st ? st : &local, &err);
    if (rc == TH_SCAN_FAILED)
        fprintf(stderr, "scan: %s\n", err.data);
    th_sb_free(&err);
    return rc;
}

static int get(sqlite3 *db, const char *rel, th_entry *e)
{
    char *p = rel[0] ? tpath(rel) : th_xstrdup(tmpdir);
    int rc = th_db_entry_by_path(db, p, strlen(p), e);
    free(p);
    return rc;
}

static bool exists(sqlite3 *db, const char *rel)
{
    th_entry e;
    memset(&e, 0, sizeof e);
    int rc = get(db, rel, &e);
    th_entry_clear(&e);
    return rc == 1;
}

static int64_t agg_size(sqlite3 *db, const char *rel)
{
    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, rel, &e) == 1);
    int64_t v = e.agg_size;
    th_entry_clear(&e);
    return v;
}

/* Every directory's aggregates must equal a fresh recomputation. */
static void check_consistent(sqlite3 *db)
{
    sqlite3_stmt *st = th_db_prepare(db, "SELECT d.id, d.agg_size, d.agg_files, d.agg_dirs, d.size, "
                                         "(SELECT coalesce(sum(CASE WHEN c.type = 1 THEN c.agg_size "
                                         "WHEN c.flags & 8 THEN 0 ELSE c.size END), 0) FROM entries c "
                                         "WHERE c.parent_id = d.id) FROM entries d WHERE d.type = 1");
    REQUIRE(st);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t agg = sqlite3_column_int64(st, 1), own = sqlite3_column_int64(st, 4);
        int64_t kids = sqlite3_column_int64(st, 5);
        CHECK_INT(agg, own + kids);
    }
    sqlite3_finalize(st);
    /* parent links point at directories one component up */
    st = th_db_prepare(db, "SELECT count(*) FROM entries c JOIN entries p ON p.id = c.parent_id "
                           "WHERE p.type <> 1 OR substr(c.path, 1, length(p.path)) <> p.path");
    REQUIRE(st);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);
}

static void test_basic_tree(void)
{
    mk_dir("t1");
    mk_dir("t1/a");
    mk_dir("t1/a/deep");
    mk_dir("t1/empty");
    mk_file("t1/one", 100);
    mk_file("t1/a/two", 200);
    mk_file("t1/a/deep/three", 300);
    mk_file("t1/with space", 7);
    mk_file("t1/\xc3\xa9t\xc3\xa9", 5);  /* UTF-8 */
    mk_file("t1/bad\xff\xfe", 3);        /* invalid UTF-8 */
    char *target = tpath("t1/a");
    char *link = tpath("t1/link");
    REQUIRE(symlink(target, link) == 0);
    free(target);
    free(link);

    sqlite3 *db = open_db("basic.sqlite3");
    char *root = tpath("t1");
    int64_t rid = th_db_root_ensure(db, root);
    REQUIRE(rid > 0);
    th_scan_stats st;
    memset(&st, 0, sizeof st);
    CHECK_INT(scan(db, rid, NULL, &st), TH_SCAN_OK);
    CHECK(st.added >= 10);

    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, "t1", &e) == 1);
    CHECK_INT(e.type, TH_TYPE_DIR);
    CHECK_INT(e.parent_id, 0);
    CHECK_INT(e.agg_files, 7); /* includes the symlink */
    CHECK_INT(e.agg_dirs, 3);
    int64_t top_own = e.size;
    th_entry_clear(&e);

    REQUIRE(get(db, "t1/link", &e) == 1);
    CHECK_INT(e.type, TH_TYPE_SYMLINK);
    int64_t link_size = e.size;
    th_entry_clear(&e);
    CHECK(!exists(db, "t1/link/two")); /* symlinks are not followed */
    CHECK(exists(db, "t1/bad\xff\xfe"));
    CHECK(exists(db, "t1/\xc3\xa9t\xc3\xa9"));
    CHECK(exists(db, "t1/with space"));
    CHECK(exists(db, "t1/empty"));
    CHECK(exists(db, "t1/a/deep/three"));

    int64_t dirs_own = 0;
    const char *dirs[] = {"t1/a", "t1/a/deep", "t1/empty"};
    for (size_t i = 0; i < TH_ARRAY_LEN(dirs); i++) {
        REQUIRE(get(db, dirs[i], &e) == 1);
        dirs_own += e.size;
        th_entry_clear(&e);
    }
    CHECK_INT(agg_size(db, "t1"), top_own + dirs_own + link_size + 100 + 200 + 300 + 7 + 5 + 3);

    th_root r;
    memset(&r, 0, sizeof r);
    REQUIRE(th_db_root_get(db, rid, &r) == 1);
    CHECK_INT(r.state, TH_STATE_VERIFIED);
    CHECK(!r.scan_in_progress);
    th_root_clear(&r);
    check_consistent(db);

    /* An unchanged rescan writes nothing. */
    memset(&st, 0, sizeof st);
    CHECK_INT(scan(db, rid, NULL, &st), TH_SCAN_OK);
    CHECK_INT(st.added, 0);
    CHECK_INT(st.removed, 0);
    CHECK_INT(st.updated, 0);

    /* add, modify, delete, type change */
    mk_file("t1/new", 50);
    mk_file("t1/a/two", 250);
    char *p = tpath("t1/a/deep/three");
    REQUIRE(unlink(p) == 0);
    free(p);
    p = tpath("t1/empty");
    REQUIRE(rmdir(p) == 0);
    free(p);
    mk_file("t1/empty", 11);
    memset(&st, 0, sizeof st);
    CHECK_INT(scan(db, rid, NULL, &st), TH_SCAN_OK);
    CHECK(exists(db, "t1/new"));
    CHECK(!exists(db, "t1/a/deep/three"));
    REQUIRE(get(db, "t1/empty", &e) == 1);
    CHECK_INT(e.type, TH_TYPE_FILE);
    CHECK_INT(e.size, 11);
    th_entry_clear(&e);
    REQUIRE(get(db, "t1/a/two", &e) == 1);
    CHECK_INT(e.size, 250);
    th_entry_clear(&e);
    check_consistent(db);

    /* removing a whole directory removes its subtree */
    p = tpath("t1/a");
    rm_tree(p);
    free(p);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    CHECK(!exists(db, "t1/a"));
    CHECK(!exists(db, "t1/a/two"));
    check_consistent(db);

    free(root);
    th_db_close(db);
}

static void test_hardlink_sparse(void)
{
    mk_dir("t2");
    mk_file("t2/orig", 4096);
    char *a = tpath("t2/orig"), *b = tpath("t2/hard");
    REQUIRE(link(a, b) == 0);
    free(a);
    free(b);
    char *sp = tpath("t2/sparse");
    int fd = open(sp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(ftruncate(fd, 64 * 1024 * 1024) == 0);
    close(fd);
    free(sp);

    sqlite3 *db = open_db("hard.sqlite3");
    char *root = tpath("t2");
    int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);

    th_entry x, y, s, top;
    memset(&x, 0, sizeof x);
    memset(&y, 0, sizeof y);
    memset(&s, 0, sizeof s);
    memset(&top, 0, sizeof top);
    REQUIRE(get(db, "t2/orig", &x) == 1);
    REQUIRE(get(db, "t2/hard", &y) == 1);
    REQUIRE(get(db, "t2/sparse", &s) == 1);
    REQUIRE(get(db, "t2", &top) == 1);
    CHECK_INT(((x.flags | y.flags) & TH_FLAG_LINKDUP) != 0, 1);
    CHECK_INT(((x.flags & y.flags) & TH_FLAG_LINKDUP) != 0, 0);
    CHECK_INT(x.nlink, 2);
    CHECK(s.flags & TH_FLAG_SPARSE);
    CHECK(s.alloc < s.size);
    CHECK_INT(top.agg_files, 3);
    CHECK_INT(top.agg_size, top.size + 4096 + 64 * 1024 * 1024);
    th_entry_clear(&x);
    th_entry_clear(&y);
    th_entry_clear(&s);
    th_entry_clear(&top);
    check_consistent(db);
    free(root);
    th_db_close(db);
}

typedef struct { int64_t ids[64]; size_t n; } aggregate_queue;
static void queue_aggregate(int64_t id, void *ud)
{
    aggregate_queue *q = ud;
    REQUIRE(q->n < 64);
    q->ids[q->n++] = id;
}
static void flush_aggregates(sqlite3 *db, aggregate_queue *q)
{
    CHECK_INT(th_index_refresh_batch(db, q->ids, q->n), 0);
    q->n = 0;
}

static void test_hardlink_subtree(void)
{
    mk_dir("links"); mk_dir("links/a"); mk_dir("links/b"); mk_file("links/a/orig", 4096);
    char *a = tpath("links/a/orig"), *b = tpath("links/b/hard"), *root = tpath("links");
    REQUIRE(link(a, b) == 0);
    sqlite3 *db = open_db("links.sqlite3"); int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    th_entry x, y; REQUIRE(get(db, "links/a/orig", &x) == 1); REQUIRE(get(db, "links/b/hard", &y) == 1);
    /* Modify through just one directory: the other indexed link must update. */
    mk_file("links/a/orig", 8192); char *dir = tpath("links/a");
    th_strbuf err; th_sb_init(&err); th_scan_stats st;
    aggregate_queue queue = {0};
    th_scan_opts opts = {.defer_aggregate=queue_aggregate, .ud=&queue};
    CHECK_INT(th_scan_subtree(db, rid, dir, strlen(dir), &opts, &st, &err), TH_SCAN_OK);
    flush_aggregates(db, &queue);
    th_entry e; REQUIRE(get(db, "links/b/hard", &e) == 1); CHECK_INT(e.size, 8192); th_entry_clear(&e);
    /* Remove whichever row contributes to totals, leaving its sibling unscanned. */
    const char *canonical = x.flags & TH_FLAG_LINKDUP ? b : a;
    const char *survivor = x.flags & TH_FLAG_LINKDUP ? "links/a/orig" : "links/b/hard";
    REQUIRE(unlink(canonical) == 0); free(dir); dir = th_xstrdup(root);
    const char *slash = strrchr(canonical, '/'); size_t length = (size_t)(slash - canonical);
    char *parent = th_xmemdup(canonical, length);
    CHECK_INT(th_scan_subtree(db, rid, parent, length, &opts, &st, &err), TH_SCAN_OK);
    flush_aggregates(db, &queue);
    REQUIRE(get(db, survivor, &e) == 1); CHECK(!(e.flags & TH_FLAG_LINKDUP)); CHECK_INT(e.size, 8192); th_entry_clear(&e);
    REQUIRE(get(db, "links", &e) == 1);
    struct stat rs, as, bs; REQUIRE(stat(root, &rs) == 0);
    char *ad = tpath("links/a"), *bd = tpath("links/b"); REQUIRE(stat(ad, &as) == 0); REQUIRE(stat(bd, &bs) == 0);
    CHECK_INT(e.agg_size, rs.st_size + as.st_size + bs.st_size + 8192);
    CHECK_INT(e.agg_files, 1); th_entry_clear(&e); th_entry_clear(&x); th_entry_clear(&y);
    check_consistent(db); th_db_close(db); th_sb_free(&err);
    free(a); free(b); free(root); free(dir); free(parent); free(ad); free(bd);
}

static void test_rename_during_reconcile(void)
{
    mk_dir("rename-race"); mk_dir("rename-race/unicodé-old");
    mk_dir("rename-race/unicodé-old/deep"); mk_file("rename-race/unicodé-old/deep/item", 7);
    sqlite3 *db = open_db("rename-race.sqlite3"); char *root = tpath("rename-race");
    int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    th_entry before = {0}, after = {0};
    REQUIRE(get(db, "rename-race/unicodé-old/deep/item", &before) == 1);
    char *old = tpath("rename-race/unicodé-old"), *next = tpath("rename-race/z-new");
    REQUIRE(rename(old, next) == 0);
    th_scan_opts opts = {.shallow = true}; th_scan_stats stats; th_strbuf err; th_sb_init(&err);
    CHECK_INT(th_scan_subtree(db, rid, root, strlen(root), &opts, &stats, &err), TH_SCAN_OK);
    REQUIRE(get(db, "rename-race/z-new/deep/item", &after) == 1);
    CHECK_INT(after.id, before.id); CHECK_INT(stats.dirs, 1);
    th_entry_clear(&after);
    REQUIRE(rename(next, old) == 0);
    CHECK_INT(th_scan_subtree(db, rid, root, strlen(root), &opts, &stats, &err), TH_SCAN_OK);
    REQUIRE(get(db, "rename-race/unicodé-old/deep/item", &after) == 1);
    CHECK_INT(after.id, before.id); CHECK_INT(stats.dirs, 1);
    CHECK(!exists(db, "rename-race/z-new/deep/item")); check_consistent(db);
    th_entry_clear(&before); th_entry_clear(&after); th_sb_free(&err);
    th_db_close(db); free(root); free(old); free(next);
}

static void test_file_rename_during_reconcile(void)
{
    mk_dir("file-rename"); mk_file("file-rename/unicodé-old", 7);
    mk_file("file-rename/linked", 9);
    char *link_source = tpath("file-rename/linked"), *alias = tpath("file-rename/new-alias");
    char *symbolic = tpath("file-rename/old-symlink"), *new_symbolic = tpath("file-rename/z-symlink");
    REQUIRE(symlink("linked", symbolic) == 0);
    sqlite3 *db = open_db("file-rename.sqlite3"); char *root = tpath("file-rename");
    int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    th_entry before = {0}, after = {0}, link_before = {0}, symlink_before = {0};
    REQUIRE(get(db, "file-rename/unicodé-old", &before) == 1);
    REQUIRE(get(db, "file-rename/linked", &link_before) == 1);
    REQUIRE(get(db, "file-rename/old-symlink", &symlink_before) == 1);
    char *old = tpath("file-rename/unicodé-old"), *next = tpath("file-rename/z-new");
    REQUIRE(rename(old, next) == 0); REQUIRE(rename(symbolic, new_symbolic) == 0);
    REQUIRE(link(link_source, alias) == 0);
    th_scan_opts opts = {.shallow = true}; th_scan_stats stats; th_strbuf err; th_sb_init(&err);
    CHECK_INT(th_scan_subtree(db, rid, root, strlen(root), &opts, &stats, &err), TH_SCAN_OK);
    REQUIRE(get(db, "file-rename/z-new", &after) == 1); CHECK_INT(after.id, before.id);
    th_entry_clear(&after);
    REQUIRE(get(db, "file-rename/z-symlink", &after) == 1); CHECK_INT(after.id, symlink_before.id);
    th_entry_clear(&after);
    REQUIRE(get(db, "file-rename/linked", &after) == 1); CHECK_INT(after.id, link_before.id);
    th_entry_clear(&after);
    REQUIRE(get(db, "file-rename/new-alias", &after) == 1); CHECK(after.id != link_before.id);
    th_entry_clear(&after);
    REQUIRE(rename(next, old) == 0);
    CHECK_INT(th_scan_subtree(db, rid, root, strlen(root), &opts, &stats, &err), TH_SCAN_OK);
    REQUIRE(get(db, "file-rename/unicodé-old", &after) == 1); CHECK_INT(after.id, before.id);
    CHECK_INT(stats.dirs, 1); check_consistent(db);
    th_entry_clear(&before); th_entry_clear(&after); th_entry_clear(&link_before); th_entry_clear(&symlink_before);
    th_sb_free(&err); th_db_close(db);
    free(root); free(old); free(next); free(link_source); free(alias); free(symbolic); free(new_symbolic);
}

static void test_denied(void)
{
    if (geteuid() == 0) {
        fprintf(stderr, "skipping denied-directory test as root\n");
        return;
    }
    mk_dir("t3");
    mk_dir("t3/locked");
    mk_file("t3/locked/secret", 10);
    mk_file("t3/ok", 1);
    sqlite3 *db = open_db("denied.sqlite3");
    char *root = tpath("t3");
    int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    CHECK(exists(db, "t3/locked/secret"));

    char *lk = tpath("t3/locked");
    REQUIRE(chmod(lk, 0) == 0);
    th_scan_stats st;
    memset(&st, 0, sizeof st);
    CHECK_INT(scan(db, rid, NULL, &st), TH_SCAN_OK);
    CHECK(st.errors >= 1);
    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, "t3/locked", &e) == 1);
    CHECK(e.flags & TH_FLAG_DENIED);
    th_entry_clear(&e);
    CHECK(exists(db, "t3/locked/secret"));
    CHECK(exists(db, "t3/ok"));

    /* readable again: flag clears and cached children reconcile */
    REQUIRE(chmod(lk, 0755) == 0);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    REQUIRE(get(db, "t3/locked", &e) == 1);
    CHECK(!(e.flags & TH_FLAG_DENIED));
    th_entry_clear(&e);
    CHECK(exists(db, "t3/locked/secret"));
    check_consistent(db);
    free(lk);
    free(root);
    th_db_close(db);
}

static void cancel_on_dir(int64_t id, const char *path, size_t len, void *ud)
{
    (void)id;
    (void)path;
    (void)len;
    atomic_store((atomic_bool *)ud, true);
}

static void test_cancel_and_large(void)
{
    mk_dir("t4");
    char rel[64];
    for (int d = 0; d < 20; d++) {
        snprintf(rel, sizeof rel, "t4/d%02d", d);
        mk_dir(rel);
        for (int f = 0; f < 100; f++) {
            snprintf(rel, sizeof rel, "t4/d%02d/f%03d", d, f);
            mk_file(rel, (size_t)f);
        }
    }
    sqlite3 *db = open_db("cancel.sqlite3");
    char *root = tpath("t4");
    int64_t rid = th_db_root_ensure(db, root);

    atomic_bool cancel = false;
    th_scan_opts o;
    memset(&o, 0, sizeof o);
    o.cancel = &cancel;
    o.on_dir = cancel_on_dir;
    o.ud = &cancel;
    o.batch_rows = 7;
    CHECK_INT(scan(db, rid, &o, NULL), TH_SCAN_CANCELLED);
    th_root r;
    memset(&r, 0, sizeof r);
    REQUIRE(th_db_root_get(db, rid, &r) == 1);
    CHECK_INT(r.state, TH_STATE_STALE);
    CHECK(r.scan_in_progress);
    th_root_clear(&r);

    /* a full scan afterwards completes with small batches */
    memset(&o, 0, sizeof o);
    o.batch_rows = 13;
    CHECK_INT(scan(db, rid, &o, NULL), TH_SCAN_OK);
    CHECK_INT(th_db_entry_count(db), 1 + 20 + 2000);
    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, "t4", &e) == 1);
    CHECK_INT(e.agg_files, 2000);
    CHECK_INT(e.agg_dirs, 20);
    th_entry_clear(&e);
    check_consistent(db);
    free(root);
    th_db_close(db);
}

static void test_subtree(void)
{
    mk_dir("t5");
    mk_dir("t5/x");
    mk_dir("t5/x/y");
    mk_file("t5/x/y/f", 10);
    mk_file("t5/top", 1);
    sqlite3 *db = open_db("sub.sqlite3");
    char *root = tpath("t5");
    int64_t rid = th_db_root_ensure(db, root);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    int64_t before = agg_size(db, "t5");

    mk_file("t5/x/y/g", 1000);
    mk_file("t5/top", 5); /* outside the subtree: must not be noticed */
    char *sub = tpath("t5/x/y");
    th_scan_stats st;
    memset(&st, 0, sizeof st);
    th_strbuf err;
    th_sb_init(&err);
    aggregate_queue queue = {0};
    th_scan_opts opts = {.defer_aggregate=queue_aggregate, .ud=&queue};
    CHECK_INT(th_scan_subtree(db, rid, sub, strlen(sub), &opts, &st, &err), TH_SCAN_OK);
    flush_aggregates(db, &queue);
    CHECK_INT(st.added, 1);
    CHECK(exists(db, "t5/x/y/g"));
    CHECK_INT(agg_size(db, "t5"), before + 1000);
    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, "t5/top", &e) == 1);
    CHECK_INT(e.size, 1);
    th_entry_clear(&e);

    /* subtree path that has not been indexed yet: nearest indexed ancestor is rescanned */
    mk_dir("t5/x/new");
    mk_dir("t5/x/new/inner");
    mk_file("t5/x/new/inner/h", 20);
    char *nsub = tpath("t5/x/new/inner");
    CHECK_INT(th_scan_subtree(db, rid, nsub, strlen(nsub), &opts, &st, &err), TH_SCAN_OK);
    flush_aggregates(db, &queue);
    CHECK(exists(db, "t5/x/new/inner/h"));
    CHECK(agg_size(db, "t5/x/new") >= agg_size(db, "t5/x/new/inner") + 20);

    /* deleted subtree */
    char *xp = tpath("t5/x");
    rm_tree(xp);
    CHECK_INT(th_scan_subtree(db, rid, sub, strlen(sub), &opts, &st, &err), TH_SCAN_OK);
    flush_aggregates(db, &queue);
    CHECK(!exists(db, "t5/x"));
    CHECK(!exists(db, "t5/x/y/f"));
    check_consistent(db);

    th_sb_free(&err);
    free(xp);
    free(nsub);
    free(sub);
    free(root);
    th_db_close(db);
}

static void test_nested_roots_and_offline(void)
{
    mk_dir("t6");
    mk_dir("t6/inner");
    mk_file("t6/inner/f", 10);
    mk_file("t6/g", 3);
    sqlite3 *db = open_db("nested.sqlite3");
    char *outer = tpath("t6"), *inner = tpath("t6/inner");
    int64_t ro = th_db_root_ensure(db, outer);
    CHECK_INT(scan(db, ro, NULL, NULL), TH_SCAN_OK);
    CHECK(exists(db, "t6/inner/f"));

    /* adding a nested root adopts the existing rows */
    int64_t ri = th_db_root_ensure(db, inner);
    CHECK_INT(scan(db, ri, NULL, NULL), TH_SCAN_OK);
    th_entry e;
    memset(&e, 0, sizeof e);
    REQUIRE(get(db, "t6/inner/f", &e) == 1);
    CHECK_INT(e.root_id, ri);
    th_entry_clear(&e);
    REQUIRE(get(db, "t6/inner", &e) == 1);
    CHECK_INT(e.parent_id, 0);
    th_entry_clear(&e);

    /* the outer root skips it now */
    CHECK_INT(scan(db, ro, NULL, NULL), TH_SCAN_OK);
    REQUIRE(get(db, "t6/inner/f", &e) == 1);
    CHECK_INT(e.root_id, ri);
    th_entry_clear(&e);
    check_consistent(db);

    /* missing root path */
    char *gone = tpath("does-not-exist");
    int64_t rg = th_db_root_ensure(db, gone);
    CHECK_INT(scan(db, rg, NULL, NULL), TH_SCAN_OFFLINE);
    th_root r;
    memset(&r, 0, sizeof r);
    REQUIRE(th_db_root_get(db, rg, &r) == 1);
    CHECK_INT(r.state, TH_STATE_OFFLINE);
    th_root_clear(&r);

    free(gone);
    free(outer);
    free(inner);
    th_db_close(db);
}

static void test_excludes(void)
{
    mk_dir("t7");
    mk_dir("t7/node_modules");
    mk_file("t7/node_modules/x", 1);
    mk_file("t7/keep", 1);
    th_config cfg;
    th_config_defaults(&cfg);
    th_strbuf err;
    th_sb_init(&err);
    char *cpath = tpath("cfg.conf");
    FILE *f = fopen(cpath, "w");
    REQUIRE(f);
    fputs("exclude = node_modules\n", f);
    fclose(f);
    CHECK_INT(th_config_load(&cfg, cpath, &err), 0);

    sqlite3 *db = open_db("excl.sqlite3");
    char *root = tpath("t7");
    int64_t rid = th_db_root_ensure(db, root);
    th_scan_opts o;
    memset(&o, 0, sizeof o);
    o.cfg = &cfg;
    CHECK_INT(scan(db, rid, &o, NULL), TH_SCAN_OK);
    CHECK(exists(db, "t7/keep"));
    CHECK(!exists(db, "t7/node_modules"));
    th_config_free(&cfg);
    th_sb_free(&err);
    free(cpath);
    free(root);
    th_db_close(db);
}

static void test_long_path(void)
{
    /* Build a path longer than PATH_MAX using relative mkdir via fds. */
    mk_dir("t8");
    char *base = tpath("t8");
    int fd = open(base, O_RDONLY | O_DIRECTORY);
    REQUIRE(fd >= 0);
    char comp[201];
    memset(comp, 'n', 200);
    comp[200] = '\0';
    th_strbuf full;
    th_sb_init(&full);
    th_sb_puts(&full, base);
    int depth = (PATH_MAX / 201) + 3;
    for (int i = 0; i < depth; i++) {
        REQUIRE(mkdirat(fd, comp, 0755) == 0);
        int nfd = openat(fd, comp, O_RDONLY | O_DIRECTORY);
        REQUIRE(nfd >= 0);
        close(fd);
        fd = nfd;
        th_sb_putc(&full, '/');
        th_sb_puts(&full, comp);
    }
    int ffd = openat(fd, "leaf", O_CREAT | O_WRONLY, 0644);
    REQUIRE(ffd >= 0);
    close(ffd);
    close(fd);
    CHECK(full.len > PATH_MAX);

    int dfd = th_open_dir(full.data, full.len, false);
    CHECK(dfd >= 0);
    if (dfd >= 0)
        close(dfd);

    sqlite3 *db = open_db("long.sqlite3");
    int64_t rid = th_db_root_ensure(db, base);
    CHECK_INT(scan(db, rid, NULL, NULL), TH_SCAN_OK);
    th_sb_puts(&full, "/leaf");
    th_entry e;
    memset(&e, 0, sizeof e);
    CHECK_INT(th_db_entry_by_path(db, full.data, full.len, &e), 1);
    th_entry_clear(&e);
    CHECK_INT(th_db_entry_count(db), 1 + depth + 1);
    th_db_close(db);
    th_sb_free(&full);
    free(base);
}

static void test_mountinfo(void)
{
    const char *text =
        "22 1 8:1 / / rw,relatime shared:1 - ext4 /dev/sda1 rw\n"
        "23 22 0:5 / /proc rw - proc proc rw\n"
        "24 22 0:20 / /home/u/My\\040Disk rw - ext4 /dev/sdb1 rw\n"
        "25 22 0:30 / /home/u/Private rw - ecryptfs /home/u/.Private rw\n"
        "26 22 0:31 / /run/user/1000/gvfs rw - fuse.gvfsd-fuse gvfsd-fuse rw\n"
        "garbage line\n";
    th_mounts m;
    CHECK_INT(th_mounts_parse(&m, text, strlen(text)), 0);
    CHECK_INT(m.n, 5);
    const th_mount *x = th_mounts_at(&m, "/home/u/My Disk");
    REQUIRE(x);
    CHECK_STR(x->fstype, "ext4");
    CHECK(th_mounts_at(&m, "/home/u") == NULL);
    x = th_mounts_find(&m, "/home/u/My Disk/sub/file");
    REQUIRE(x);
    CHECK_STR(x->mnt, "/home/u/My Disk");
    x = th_mounts_find(&m, "/etc/passwd");
    REQUIRE(x);
    CHECK_STR(x->mnt, "/");
    CHECK(th_fstype_is_pseudo("proc"));
    CHECK(th_fstype_is_pseudo("sysfs"));
    CHECK(!th_fstype_is_pseudo("ext4"));
    CHECK(!th_fstype_is_pseudo("btrfs"));
    CHECK(th_mounts_is_hidden(&m, "/home/u/.Private"));
    CHECK(!th_mounts_is_hidden(&m, "/home/u/Private"));
    th_mounts_free(&m);

    /* the live table of this machine parses and contains / */
    th_mounts live;
    CHECK_INT(th_mounts_load(&live, NULL), 0);
    CHECK(live.n > 0);
    CHECK(th_mounts_at(&live, "/") != NULL);
    th_mounts_free(&live);
}

int main(void)
{
    REQUIRE(mkdtemp(tmpdir));
    RUN(test_mountinfo);
    RUN(test_basic_tree);
    RUN(test_hardlink_sparse);
    RUN(test_hardlink_subtree);
    RUN(test_rename_during_reconcile);
    RUN(test_file_rename_during_reconcile);
    RUN(test_denied);
    RUN(test_cancel_and_large);
    RUN(test_subtree);
    RUN(test_nested_roots_and_offline);
    RUN(test_excludes);
    RUN(test_long_path);
    rm_tree(tmpdir);
    return TEST_EXIT();
}
