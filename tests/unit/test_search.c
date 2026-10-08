/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/db.h"
#include "treehound/indexer.h"
#include "treehound/match.h"
#include "treehound/search.h"
#include "treehound/util.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/th-test-search-XXXXXX";
static char dbdir[] = "/tmp/th-test-searchdb-XXXXXX";
static sqlite3 *db;
static bool have_utf8_locale;

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

static void set_mtime(const char *rel, time_t t)
{
    char *p = tpath(rel);
    struct timeval tv[2] = {{t, 0}, {t, 0}};
    REQUIRE(utimes(p, tv) == 0);
    free(p);
}

static void rm_tree(const char *path)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup of %s failed\n", path);
}

static void search(const th_search_opts *o, th_search_result *r)
{
    th_strbuf err;
    th_sb_init(&err);
    int rc = th_search(db, o, r, &err);
    if (rc != 0)
        fprintf(stderr, "search: %s\n", err.data);
    REQUIRE(rc == 0);
    th_sb_free(&err);
}

/* Runs a query and returns the number of results (limit TH_MAX_PAGE). */
static size_t count_q(const char *q, int flags, bool *used_index)
{
    th_search_opts o;
    th_search_opts_init(&o);
    o.query = q;
    o.match_flags = flags;
    o.limit = TH_MAX_PAGE;
    th_search_result r;
    search(&o, &r);
    size_t n = r.n;
    if (used_index)
        *used_index = r.used_index;
    th_search_result_free(&r);
    return n;
}

static bool has_name(const th_search_result *r, const char *name)
{
    for (size_t i = 0; i < r->n; i++)
        if (strcmp(r->items[i].name, name) == 0)
            return true;
    return false;
}

static void build_tree(void)
{
    mk_dir("docs");
    mk_file("docs/report-2024.pdf", 100);
    mk_file("docs/Report-final.PDF", 5000);
    mk_file("docs/notes.txt", 10);
    mk_dir("src");
    mk_file("src/main.c", 300);
    mk_file("src/main.h", 20);
    mk_dir("src/util");
    mk_file("src/util/strutil.c", 50);
    mk_file("src/util/weird\"quote.txt", 1);
    mk_file("\xc3\xa9migr\xc3\xa9.txt", 7); /* émigré.txt */
    mk_dir("ab");
    mk_file("ab/x", 1);
    char *l = tpath("link");
    REQUIRE(symlink("src", l) == 0);
    free(l);
    set_mtime("docs/notes.txt", 1000000000);
    set_mtime("src/main.c", 1600000000);

    char *dbp = th_path_join(dbdir, "index.sqlite3");
    th_strbuf err;
    th_sb_init(&err);
    db = th_db_open(dbp, false, &err);
    REQUIRE(db);
    free(dbp);
    th_sb_free(&err);
}

static void scan_whole(void)
{
    th_strbuf err;
    th_sb_init(&err);
    int64_t id = th_db_root_ensure(db, tmpdir);
    REQUIRE(id > 0);
    th_scan_stats st;
    REQUIRE(th_scan_root(db, id, NULL, &st, &err) == TH_SCAN_OK);
    th_sb_free(&err);
}

static void test_fts_query(void)
{
    th_strbuf q;
    th_sb_init(&q);
    th_search_fts_query("ab *.c report \"x y z\" !excluded dir/sub", 0, &q);
    CHECK_STR(q.data, "\"report\" AND \"x y z\"");
    th_search_fts_query("*.pdf", 0, &q);
    CHECK_STR(q.data, "\".pdf\"");
    th_search_fts_query("weird\\\"quote", 0, &q);
    CHECK_STR(q.data, "\"weird\"\"quote\"");
    th_search_fts_query("a*b?c", 0, &q);
    CHECK_INT(q.len, 0);
    th_search_fts_query("", 0, &q);
    CHECK_INT(q.len, 0);
    th_sb_free(&q);
}

static void test_terms(void)
{
    bool idx = false;
    CHECK_INT(count_q("report", 0, &idx), 2);
    CHECK(idx);
    CHECK_INT(count_q("report", TH_MATCH_CASE, NULL), 1);
    CHECK_INT(count_q("REPORT", TH_MATCH_CASE, NULL), 0);
    CHECK_INT(count_q("*.c", 0, &idx), 2);
    CHECK(!idx);
    CHECK_INT(count_q("*.pdf", 0, &idx), 2);
    CHECK(idx);
    CHECK_INT(count_q("*.pdf", TH_MATCH_CASE, NULL), 1);
    CHECK_INT(count_q("main !*.h", 0, NULL), 1);
    CHECK_INT(count_q("main", 0, NULL), 2);
    CHECK_INT(count_q("src/main", 0, NULL), 2);
    CHECK_INT(count_q("util/str", 0, NULL), 1);
    CHECK_INT(count_q("weird\\\"quote", 0, &idx), 1);
    CHECK(idx);
    CHECK_INT(count_q("\"weird\\\"quote.txt\"", 0, NULL), 1);
    CHECK_INT(count_q("\xc3\xa9migr\xc3\xa9", 0, &idx), 1);
    CHECK(idx);
    if (have_utf8_locale)
        CHECK_INT(count_q("\xc3\x89MIGR\xc3\x89", 0, NULL), 1); /* ÉMIGRÉ */
    CHECK_INT(count_q("nothing-like-this", 0, NULL), 0);
    CHECK_INT(count_q("ab", 0, NULL), 1); /* the directory "ab", short term scans */
    /* every term must match */
    CHECK_INT(count_q("report 2024", 0, NULL), 1);
    CHECK_INT(count_q("report final 2024", 0, NULL), 0);
}

static void test_filters_and_sort(void)
{
    th_search_opts o;
    th_search_result r;

    th_search_opts_init(&o);
    o.types = TH_TYPEMASK(TH_TYPE_DIR);
    o.limit = TH_MAX_PAGE;
    search(&o, &r);
    /* tmpdir, docs, src, src/util, ab */
    CHECK_INT(r.n, 5);
    for (size_t i = 0; i < r.n; i++)
        CHECK_INT(r.items[i].type, TH_TYPE_DIR);
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.types = TH_TYPEMASK(TH_TYPE_SYMLINK);
    search(&o, &r);
    CHECK_INT(r.n, 1);
    if (r.n)
        CHECK_STR(r.items[0].name, "link");
    th_search_result_free(&r);

    /* largest file */
    th_search_opts_init(&o);
    o.types = TH_TYPEMASK(TH_TYPE_FILE);
    o.sort = TH_SORT_SIZE;
    o.descending = true;
    o.limit = 1;
    search(&o, &r);
    CHECK_INT(r.n, 1);
    if (r.n)
        CHECK_STR(r.items[0].name, "Report-final.PDF");
    th_search_result_free(&r);

    /* directories sort by aggregate size: the root top entry is largest */
    th_search_opts_init(&o);
    o.sort = TH_SORT_SIZE;
    o.descending = true;
    o.limit = 1;
    search(&o, &r);
    CHECK_INT(r.n, 1);
    if (r.n)
        CHECK_STR(r.items[0].path, tmpdir);
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.types = TH_TYPEMASK(TH_TYPE_FILE);
    o.min_size = 100;
    o.max_size = 300;
    o.limit = TH_MAX_PAGE;
    search(&o, &r);
    CHECK_INT(r.n, 2);
    CHECK(has_name(&r, "report-2024.pdf"));
    CHECK(has_name(&r, "main.c"));
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.max_mtime = 1500000000;
    search(&o, &r);
    CHECK_INT(r.n, 1);
    if (r.n)
        CHECK_STR(r.items[0].name, "notes.txt");
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.min_mtime = 1500000000;
    o.max_mtime = 1700000000;
    search(&o, &r);
    CHECK_INT(r.n, 1);
    if (r.n)
        CHECK_STR(r.items[0].name, "main.c");
    th_search_result_free(&r);

    /* subtree restriction, with and without a trailing slash */
    char *src = tpath("src/");
    th_search_opts_init(&o);
    o.under = src;
    o.limit = TH_MAX_PAGE;
    search(&o, &r);
    CHECK_INT(r.n, 5);
    CHECK(!has_name(&r, "src"));
    th_search_result_free(&r);
    o.query = "*.c";
    search(&o, &r);
    CHECK_INT(r.n, 2);
    th_search_result_free(&r);
    free(src);

    /* sort by name is case-insensitive, path ascending */
    th_search_opts_init(&o);
    o.query = "report";
    search(&o, &r);
    REQUIRE(r.n == 2);
    CHECK_STR(r.items[0].name, "report-2024.pdf");
    CHECK_STR(r.items[1].name, "Report-final.PDF");
    th_search_result_free(&r);
    o.descending = true;
    search(&o, &r);
    REQUIRE(r.n == 2);
    CHECK_STR(r.items[0].name, "Report-final.PDF");
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.types = TH_TYPEMASK(TH_TYPE_FILE);
    o.sort = TH_SORT_MTIME;
    o.limit = 1;
    search(&o, &r);
    REQUIRE(r.n == 1);
    CHECK_STR(r.items[0].name, "notes.txt");
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.sort = TH_SORT_PATH;
    o.limit = TH_MAX_PAGE;
    search(&o, &r);
    for (size_t i = 1; i < r.n; i++)
        CHECK(strcmp(r.items[i - 1].path, r.items[i].path) < 0);
    th_search_result_free(&r);

    th_search_opts_init(&o);
    o.sort = TH_SORT_TYPE;
    o.limit = TH_MAX_PAGE;
    search(&o, &r);
    for (size_t i = 1; i < r.n; i++)
        CHECK(r.items[i - 1].type <= r.items[i].type);
    REQUIRE(r.n > 0);
    CHECK_INT(r.items[0].type, TH_TYPE_FILE);
    CHECK_INT(r.items[r.n - 1].type, TH_TYPE_SYMLINK);
    th_search_result_free(&r);

    th_sort_key k;
    CHECK(th_sort_parse("mtime", &k) && k == TH_SORT_MTIME);
    CHECK(th_sort_parse("type", &k) && k == TH_SORT_TYPE);
    CHECK(!th_sort_parse("bogus", &k));
    CHECK_STR(th_sort_name(TH_SORT_SIZE), "size");
}

static void test_pagination(void)
{
    th_search_opts o;
    th_search_result all, page;
    th_search_opts_init(&o);
    o.limit = TH_MAX_PAGE;
    o.want_total = true;
    search(&o, &all);
    CHECK_INT(all.total, (int64_t)all.n);
    CHECK_INT(all.total, th_db_entry_count(db));

    o.limit = 3;
    for (int64_t off = 0; off < (int64_t)all.n + 3; off += 3) {
        o.offset = off;
        search(&o, &page);
        CHECK_INT(page.total, (int64_t)all.n);
        for (size_t i = 0; i < page.n; i++)
            CHECK_STR(page.items[i].path, all.items[(size_t)off + i].path);
        th_search_result_free(&page);
    }
    th_search_result_free(&all);

    th_search_opts_init(&o);
    o.limit = 0; /* defaults to 100 */
    o.query = "!*";
    o.want_total = true;
    search(&o, &page);
    CHECK_INT(page.n, 0);
    CHECK_INT(page.total, 0);
    th_search_result_free(&page);

    th_strbuf err;
    th_sb_init(&err);
    th_search_opts_init(&o);
    o.query = "a b c d e f g h i j k l m n o p q r s t u v w x y z";
    CHECK(th_search(db, &o, &page, &err) == -1);
    CHECK(err.len > 0);
    th_sb_free(&err);
}

/* Index-assisted results must equal a brute-force th_match over every name. */
static void test_crosscheck(void)
{
    mk_dir("rand");
    static const char *alpha[] = {"a", "b", "c", "A", "B", ".", "-", "_", "\xc3\xa9", "\xc3\x89", "x"};
    unsigned seed = 12345;
    for (int i = 0; i < 1500; i++) {
        char name[64] = "rand/";
        int len = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 9;
        for (int j = 0; j < len; j++)
            strcat(name, alpha[((seed = seed * 1103515245u + 12345u) >> 16) % TH_ARRAY_LEN(alpha)]);
        if (strcmp(name, "rand/.") == 0 || strcmp(name, "rand/..") == 0)
            continue;
        char *p = tpath(name);
        FILE *f = fopen(p, "wb");
        if (f)
            fclose(f);
        free(p);
    }
    scan_whole();

    static const char *queries[] = {"abc", "ABC", "aab", "\xc3\xa9\xc3\xa9x", "a.b", "-_-", "*abc*",
                                    "a*b*c", "ab?", "*.ab", "bca", "x\xc3\xa9" "a", "cab*", "?bc?", "aaa",
                                    "\xc3\x89" "ab", "bb", "a", "*", "_x_"};
    sqlite3_stmt *all = th_db_prepare(db, "SELECT name FROM entries");
    REQUIRE(all);
    for (size_t qi = 0; qi < TH_ARRAY_LEN(queries); qi++) {
        for (int flags = 0; flags <= TH_MATCH_CASE; flags++) {
            if (!flags && !have_utf8_locale && strchr(queries[qi], '\xc3'))
                continue;
            size_t expect = 0;
            sqlite3_reset(all);
            while (sqlite3_step(all) == SQLITE_ROW) {
                const char *n = (const char *)sqlite3_column_text(all, 0);
                size_t nl = (size_t)sqlite3_column_bytes(all, 0);
                expect += th_match(queries[qi], strlen(queries[qi]), n, nl, flags);
            }
            th_search_opts o;
            th_search_opts_init(&o);
            o.query = queries[qi];
            o.match_flags = flags;
            o.limit = 1;
            o.want_total = true;
            th_search_result r;
            search(&o, &r);
            if ((size_t)r.total != expect)
                fprintf(stderr, "query '%s' flags %d: %lld vs brute %zu\n", queries[qi], flags,
                        (long long)r.total, expect);
            CHECK_INT(r.total, (int64_t)expect);
            th_search_result_free(&r);
        }
    }
    sqlite3_finalize(all);
}

/* The guard interrupts long queries instead of letting them block the caller. */
static void test_timeout(void)
{
    th_strbuf err;
    th_sb_init(&err);
    REQUIRE(th_db_exec(db,
                       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300000) "
                       "INSERT INTO entries(root_id, name, path, type) "
                       "SELECT (SELECT min(id) FROM roots), 'bulk' || i, '/bulk/' || i, 0 FROM n") == 0);
    th_search_opts o;
    th_search_opts_init(&o);
    o.query = "*u*k*";
    o.under = "/bulk";
    o.sort = TH_SORT_NAME;
    o.want_total = true;
    o.timeout_ms = 1;
    th_search_result r;
    th_sb_reset(&err);
    CHECK(th_search(db, &o, &r, &err) == -1);
    CHECK(r.timed_out);
    CHECK(strstr(err.data, "timed out") != NULL);
    CHECK(r.n == 0 && r.items == NULL);

    o.timeout_ms = 60000;
    th_sb_reset(&err);
    CHECK(th_search(db, &o, &r, &err) == 0);
    CHECK(!r.timed_out);
    CHECK_INT(r.total, 300000);
    th_search_result_free(&r);

    /* Broad FTS posting lists still return ordered, filtered pages. */
    o.query = "bulk"; o.want_total = false; o.limit = 3; o.offset = 1;
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK(r.used_index); CHECK_INT(r.n,3);
    CHECK_STR(r.items[0].name,"bulk10"); CHECK_STR(r.items[1].name,"bulk100");
    th_search_result_free(&r);
    o.offset = 10000; o.timeout_ms = 1000;
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_INT(r.n,3); CHECK(!r.timed_out);
    CHECK_STR(r.items[0].name,"bulk108999");
    th_search_result_free(&r);
    o.timeout_ms = 60000; o.descending = true; o.offset = 0;
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_STR(r.items[0].name,"bulk99999"); th_search_result_free(&r);
    /* Streaming must retain all exact predicates, not only the FTS literal. */
    o.descending = false; o.query = "BULK* !bulk1* /bulk/2";
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_INT(r.n,3); CHECK_STR(r.items[0].name,"bulk2");
    CHECK_STR(r.items[1].name,"bulk20"); th_search_result_free(&r);
    o.match_flags = TH_MATCH_CASE;
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_INT(r.n,0); th_search_result_free(&r);
    o.match_flags = 0; o.query = "bulk"; o.min_size = 1;
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_INT(r.n,0); th_search_result_free(&r);
    o.min_size = -1; o.under = "/absent";
    th_sb_reset(&err); CHECK_INT(th_search(db,&o,&r,&err),0);
    CHECK_INT(r.n,0); th_search_result_free(&r);

    CHECK(th_db_exec(db, "DELETE FROM entries WHERE path >= '/bulk/' AND path < '/bulk0'") == 0);
    th_sb_free(&err);
}

int main(void)
{
    have_utf8_locale = setlocale(LC_CTYPE, "C.UTF-8") != NULL;
    REQUIRE(mkdtemp(tmpdir));
    REQUIRE(mkdtemp(dbdir));
    build_tree();
    scan_whole();

    RUN(test_fts_query);
    RUN(test_terms);
    RUN(test_filters_and_sort);
    RUN(test_pagination);
    RUN(test_crosscheck);
    RUN(test_timeout);
    th_db_close(db);
    rm_tree(tmpdir);
    rm_tree(dbdir);
    return TEST_EXIT();
}
