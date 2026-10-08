/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/config.h"
#include "treehound/json.h"
#include "treehound/match.h"
#include "treehound/strbuf.h"
#include "treehound/utf8.h"
#include "treehound/util.h"

#include <locale.h>

static void test_strbuf(void)
{
    th_strbuf sb;
    th_sb_init(&sb);
    CHECK_INT(sb.len, 0);
    CHECK_STR(sb.data, "");
    for (int i = 0; i < 1000; i++)
        th_sb_printf(&sb, "%d,", i);
    CHECK(sb.len > 1000);
    th_sb_truncate(&sb, 4);
    CHECK_STR(sb.data, "0,1,");
    th_sb_append(&sb, "a\0b", 3);
    CHECK_INT(sb.len, 7);
    char *s = th_sb_steal(&sb);
    CHECK(sb.data != s);
    free(s);
    th_sb_free(&sb);
}

static void test_utf8(void)
{
    char out[4];
    CHECK_INT(th_utf8_encode(0x41, out), 1);
    CHECK_INT(th_utf8_encode(0xe9, out), 2);
    CHECK_INT(th_utf8_encode(0x20ac, out), 3);
    CHECK_INT(th_utf8_encode(0x1f600, out), 4);
    CHECK(th_utf8_valid("h\xc3\xa9llo", 6));
    CHECK(!th_utf8_valid("bad\xff", 4));
    CHECK(!th_utf8_valid("\xc0\xaf", 2));     /* overlong */
    CHECK(!th_utf8_valid("\xed\xa0\x80", 3)); /* surrogate */
    uint32_t cp;
    CHECK_INT(th_utf8_decode((const unsigned char *)"\xff", 1, &cp), 1);
    CHECK_INT(cp, 0x110000 + 0xff);
}

static void test_json_roundtrip(void)
{
    th_strbuf sb;
    th_sb_init(&sb);
    th_jw w;
    th_jw_init(&w, &sb);
    th_jw_obj_begin(&w);
    th_jw_kv_str(&w, "cmd", "search");
    th_jw_kv_bytes(&w, "name", "caf\xc3\xa9 \xff\xfe.txt", 11);
    th_jw_kv_int(&w, "big", INT64_C(9007199254740993));
    th_jw_kv_int(&w, "neg", -42);
    th_jw_kv_double(&w, "d", 1.5);
    th_jw_kv_bool(&w, "t", true);
    th_jw_key(&w, "arr");
    th_jw_arr_begin(&w);
    th_jw_int(&w, 1);
    th_jw_null(&w);
    th_jw_str(&w, "q\"\\\n\t\x01");
    th_jw_arr_end(&w);
    th_jw_obj_end(&w);
    CHECK(th_utf8_valid(sb.data, sb.len));
    CHECK(strstr(sb.data, "\\udcff\\udcfe") != NULL);

    const char *err = NULL;
    th_jval *v = th_json_parse(sb.data, sb.len, &err);
    REQUIRE(v);
    CHECK_STR(th_json_get_str(v, "cmd", NULL), "search");
    size_t len = 0;
    const char *name = th_json_get_bytes(v, "name", &len);
    CHECK_INT(len, 11);
    CHECK(name && memcmp(name, "caf\xc3\xa9 \xff\xfe.txt", 11) == 0);
    CHECK_INT(th_json_get_int(v, "big", 0), INT64_C(9007199254740993));
    CHECK_INT(th_json_get_int(v, "neg", 0), -42);
    CHECK(th_json_get_double(v, "d", 0) == 1.5);
    CHECK(th_json_get_bool(v, "t", false));
    const th_jval *arr = th_json_get(v, "arr");
    REQUIRE(arr && arr->type == TH_JARR && arr->n == 3);
    CHECK(arr->items[1].type == TH_JNULL);
    CHECK_STR(arr->items[2].str, "q\"\\\n\t\x01");
    th_json_free(v);
    th_sb_free(&sb);
}

static void test_json_errors(void)
{
    const char *bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "nul", "\"\\x\"", "01", "[1] x",
                         "\"\\ud800\"", "{\"a\":1,}"};
    for (size_t i = 0; i < TH_ARRAY_LEN(bad); i++) {
        const char *err = NULL;
        th_jval *v = th_json_parse(bad[i], strlen(bad[i]), &err);
        if (v) {
            fprintf(stderr, "accepted bad json: %s\n", bad[i]);
            th_json_free(v);
        }
        CHECK(v == NULL);
        CHECK(err != NULL);
    }
    /* depth limit */
    th_strbuf sb;
    th_sb_init(&sb);
    for (int i = 0; i < 200; i++)
        th_sb_putc(&sb, '[');
    for (int i = 0; i < 200; i++)
        th_sb_putc(&sb, ']');
    const char *err = NULL;
    th_jval *v = th_json_parse(sb.data, sb.len, &err);
    CHECK(v == NULL);
    th_json_free(v);
    th_sb_free(&sb);
    /* unicode escapes */
    const char *ok = "\"\\u00e9\\ud83d\\ude00\"";
    v = th_json_parse(ok, strlen(ok), &err);
    REQUIRE(v);
    CHECK_STR(v->str, "\xc3\xa9\xf0\x9f\x98\x80");
    th_json_free(v);
}

static void test_match(void)
{
#define M(p, t, f) th_match(p, strlen(p), t, strlen(t), f)
    CHECK(M("report", "Annual-REPORT.pdf", 0));
    CHECK(!M("report", "Annual-REPORT.pdf", TH_MATCH_CASE));
    CHECK(M("REPORT", "Annual-REPORT.pdf", TH_MATCH_CASE));
    CHECK(M("*.pdf", "a.pdf", 0));
    CHECK(M("*.pdf", "A.PDF", 0));
    CHECK(!M("*.pdf", "a.pdf.txt", 0));
    CHECK(M("a?c", "abc", 0));
    CHECK(!M("a?c", "abbc", 0));
    CHECK(M("*a*b*c*", "xxaxxbxxcxx", 0));
    CHECK(M("*", "", 0));
    CHECK(M("", "anything", 0));
    CHECK(M("\\*", "*", 0));
    CHECK(!M("\\*", "x", 0));
    CHECK(M("ÉTÉ", "l'été.txt", 0));
    CHECK(M("?.txt", "é.txt", 0)); /* ? is one code point, not one byte */
    CHECK(M("\xff", "bad\xffname", 0));
    CHECK(!M("\xfe", "bad\xffname", 0));
    CHECK(M("*name", "bad\xffname", 0));
    CHECK(th_pattern_has_wildcards("a*", 2));
    CHECK(!th_pattern_has_wildcards("a\\*", 3));
#undef M
}

static void lit_cb(const char *run, size_t len, void *ud)
{
    th_strbuf *sb = ud;
    th_sb_append(sb, run, len);
    th_sb_putc(sb, '|');
}

static void test_literals(void)
{
    th_strbuf sb;
    th_sb_init(&sb);
    th_pattern_literals("*foo?bar*\\*x", 12, lit_cb, &sb);
    CHECK_STR(sb.data, "foo|bar|*x|");
    th_sb_free(&sb);
}

static void test_paths(void)
{
    char *p = th_path_normalize("/a//b/./c/../d/");
    CHECK_STR(p, "/a/b/d");
    free(p);
    p = th_path_normalize("/../..");
    CHECK_STR(p, "/");
    free(p);
    p = th_path_join("/", "x");
    CHECK_STR(p, "/x");
    free(p);
    p = th_path_join("/a", "x");
    CHECK_STR(p, "/a/x");
    free(p);
    CHECK(th_path_is_within("/a/b", "/a"));
    CHECK(th_path_is_within("/a", "/a"));
    CHECK(!th_path_is_within("/ab", "/a"));
    CHECK(th_path_is_within("/x", "/"));
}

static void test_parse(void)
{
    int64_t v;
    CHECK(th_parse_size("10", &v) && v == 10);
    CHECK(th_parse_size("1k", &v) && v == 1024);
    CHECK(th_parse_size("1.5M", &v) && v == 1572864);
    CHECK(th_parse_size("2GiB", &v) && v == INT64_C(2147483648));
    CHECK(!th_parse_size("abc", &v));
    CHECK(!th_parse_size("-1", &v));
    CHECK(th_parse_time("7d", 1000000, &v) && v == 1000000 - 7 * 86400);
    CHECK(th_parse_time("2024-01-02", 0, &v));
    CHECK(!th_parse_time("yesterday", 0, &v));
    char buf[32];
    CHECK_STR(th_fmt_size(0, buf), "0 B");
    CHECK_STR(th_fmt_size(1536, buf), "1.5 KiB");
}

static void test_config(void)
{
    th_config c;
    th_config_defaults(&c);
    const char *text = "# c\nroot = /home/u/\nroot=/data\nexclude = *.tmp\nexclude=/home/u/.cache\n"
                       "cross_mounts = yes\nsize_metric = logical\nfuture_key = 1\n"
                       "snapshot_retention = 5\n";
    th_strbuf err;
    th_sb_init(&err);
    CHECK_INT(th_config_parse(&c, text, strlen(text), &err), 0);
    CHECK(strstr(err.data, "future_key") != NULL);
    CHECK_INT(c.nroots, 2);
    CHECK_STR(c.roots[0], "/home/u");
    CHECK(c.cross_mounts);
    CHECK_INT(c.size_metric, TH_METRIC_LOGICAL);
    CHECK_INT(c.snapshot_retention, 5);
    CHECK(th_config_is_excluded(&c, "/x/a.tmp", "a.tmp"));
    CHECK(th_config_is_excluded(&c, "/home/u/.cache", ".cache"));
    CHECK(!th_config_is_excluded(&c, "/home/u/.cache/x", "x"));
    CHECK(!th_config_add_root(&c, "/data/"));
    CHECK(th_config_remove_root(&c, "/data"));
    CHECK_INT(c.nroots, 1);

    th_strbuf out;
    th_sb_init(&out);
    th_config_serialize(&c, &out);
    th_config c2;
    th_config_defaults(&c2);
    th_sb_reset(&err);
    CHECK_INT(th_config_parse(&c2, out.data, out.len, &err), 0);
    CHECK_INT(c2.nroots, 1);
    CHECK_INT(c2.nexcludes, 2);
    CHECK_INT(c2.size_metric, TH_METRIC_LOGICAL);

    th_config c3;
    th_config_defaults(&c3);
    th_sb_reset(&err);
    const char *bad = "root = relative\nwatch = maybe\nnoequals\n";
    CHECK_INT(th_config_parse(&c3, bad, strlen(bad), &err), -1);
    CHECK_INT(c3.nroots, 0);

    th_config_free(&c);
    th_config_free(&c2);
    th_config_free(&c3);
    th_sb_free(&out);
    th_sb_free(&err);
}

int main(void)
{
    setlocale(LC_CTYPE, "C.UTF-8");
    RUN(test_strbuf);
    RUN(test_utf8);
    RUN(test_json_roundtrip);
    RUN(test_json_errors);
    RUN(test_match);
    RUN(test_literals);
    RUN(test_paths);
    RUN(test_parse);
    RUN(test_config);
    return TEST_EXIT();
}
