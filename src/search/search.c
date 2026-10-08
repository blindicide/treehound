/* SPDX-License-Identifier: MIT */
#include "treehound/search.h"

#include <stdlib.h>
#include <string.h>

#include "treehound/match.h"
#include "treehound/utf8.h"
#include "treehound/util.h"

/* Size used for filtering and sorting: directories count their contents. */
#define SIZE_EXPR "(CASE WHEN e.type = 1 THEN e.agg_size ELSE e.size END)"

typedef struct {
    char *s;
    size_t len;
    bool neg;
    bool path;
} term;

typedef struct {
    term *v;
    size_t n, cap;
} term_list;

typedef struct {
    enum { P_INT, P_TEXT } kind;
    int64_t i;
    const char *s;
    size_t len;
} param;

typedef struct {
    param v[64];
    int n;
} param_list;

void th_search_opts_init(th_search_opts *o)
{
    memset(o, 0, sizeof *o);
    o->min_size = o->max_size = -1;
    o->min_mtime = o->max_mtime = -1;
    o->limit = 100;
    o->parent_id = -1;
    o->show_hidden = true;
    o->sort = TH_SORT_NAME;
}

static const char *const sort_names[] = {"name", "path", "size", "mtime", "type"};

bool th_sort_parse(const char *s, th_sort_key *out)
{
    for (size_t i = 0; i < TH_ARRAY_LEN(sort_names); i++) {
        if (strcmp(s, sort_names[i]) == 0) {
            *out = (th_sort_key)i;
            return true;
        }
    }
    return false;
}

const char *th_sort_name(th_sort_key k)
{
    return (unsigned)k < TH_ARRAY_LEN(sort_names) ? sort_names[k] : "name";
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Splits a query into terms.  Quotes group, backslash escapes stay in the term
 * so th_pattern sees them. */
static void parse_terms(const char *q, term_list *out)
{
    size_t i = 0, n = q ? strlen(q) : 0;
    th_strbuf cur;
    th_sb_init(&cur);
    while (i < n) {
        while (i < n && is_space(q[i]))
            i++;
        if (i >= n)
            break;
        bool neg = false, quoted = false, path = false;
        if (q[i] == '!') {
            neg = true;
            i++;
        }
        th_sb_reset(&cur);
        while (i < n && (quoted || !is_space(q[i]))) {
            char c = q[i++];
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (c == '\\' && i < n) {
                th_sb_putc(&cur, c);
                c = q[i++];
            }
            if (c == '/')
                path = true;
            th_sb_putc(&cur, c);
        }
        if (cur.len == 0)
            continue;
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->v = th_xrealloc(out->v, out->cap * sizeof *out->v);
        }
        out->v[out->n++] = (term){th_xmemdup(cur.data, cur.len), cur.len, neg, path};
    }
    th_sb_free(&cur);
}

static void terms_free(term_list *t)
{
    for (size_t i = 0; i < t->n; i++)
        free(t->v[i].s);
    free(t->v);
}

static size_t utf8_chars(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; n++) {
        size_t k = th_utf8_seqlen((const unsigned char *)s + i, len - i);
        i += k ? k : 1;
    }
    return n;
}

static void add_phrase(const char *run, size_t len, void *ud)
{
    th_strbuf *out = ud;
    /* the trigram tokenizer ignores phrases shorter than three characters, and
     * invalid UTF-8 might be decoded differently than th_match() does */
    if (utf8_chars(run, len) < 3 || !th_utf8_valid(run, len))
        return;
    if (out->len)
        th_sb_puts(out, " AND ");
    th_sb_putc(out, '"');
    for (size_t i = 0; i < len; i++) {
        if (run[i] == '"')
            th_sb_putc(out, '"');
        th_sb_putc(out, run[i]);
    }
    th_sb_putc(out, '"');
}

static void fts_query_terms(const term_list *t, th_strbuf *out)
{
    th_sb_reset(out);
    for (size_t i = 0; i < t->n; i++) {
        if (!t->v[i].neg && !t->v[i].path)
            th_pattern_literals(t->v[i].s, t->v[i].len, add_phrase, out);
    }
}

void th_search_fts_query(const char *query, int match_flags, th_strbuf *out)
{
    TH_UNUSED(match_flags); /* the index is case-insensitive: a superset either way */
    term_list t = {0};
    parse_terms(query, &t);
    fts_query_terms(&t, out);
    terms_free(&t);
}

static void push_int(param_list *p, int64_t v)
{
    p->v[p->n++] = (param){.kind = P_INT, .i = v};
}

static void push_text(param_list *p, const char *s, size_t len)
{
    p->v[p->n++] = (param){.kind = P_TEXT, .s = s, .len = len};
}

static void bind_all(sqlite3_stmt *st, const param_list *p)
{
    for (int i = 0; i < p->n; i++) {
        if (p->v[i].kind == P_INT)
            sqlite3_bind_int64(st, i + 1, p->v[i].i);
        else
            th_bind_bytes(st, i + 1, p->v[i].s, p->v[i].len);
    }
}

static void order_clause(th_strbuf *sql, th_sort_key k, bool desc)
{
    const char *dir = desc ? "DESC" : "ASC";
    switch (k) {
    case TH_SORT_PATH:
        th_sb_printf(sql, " ORDER BY e.path %s", dir);
        break;
    case TH_SORT_SIZE:
        th_sb_printf(sql, " ORDER BY " SIZE_EXPR " %s, e.path ASC", dir);
        break;
    case TH_SORT_MTIME:
        th_sb_printf(sql, " ORDER BY e.mtime %s, e.path ASC", dir);
        break;
    case TH_SORT_TYPE:
        th_sb_printf(sql, " ORDER BY e.type %s, e.name COLLATE NOCASE %s, e.path ASC", dir, dir);
        break;
    case TH_SORT_NAME:
    default:
        th_sb_printf(sql, " ORDER BY e.name COLLATE NOCASE %s, e.name %s, e.path ASC", dir, dir);
        break;
    }
}

static int deadline_cb(void *ud)
{
    return th_mono_sec() > *(const double *)ud;
}

static void report_error(sqlite3 *db, const th_search_opts *o, th_search_result *res, th_strbuf *err)
{
    if (o->timeout_ms > 0 && sqlite3_errcode(db) == SQLITE_INTERRUPT) {
        res->timed_out = true;
        th_sb_puts(err, "search timed out; refine the query");
        return;
    }
    th_sb_printf(err, "search: %s", sqlite3_errmsg(db));
}

int th_search(sqlite3 *db, const th_search_opts *o, th_search_result *res, th_strbuf *err)
{
    double t0 = th_mono_sec();
    double deadline = t0 + o->timeout_ms / 1000.0;
    memset(res, 0, sizeof *res);
    res->total = -1;

    term_list terms = {0};
    if ((o->match_flags & TH_MATCH_FULL) && o->query && *o->query) {
        terms.v = th_xmalloc(sizeof *terms.v);
        terms.n = terms.cap = 1;
        terms.v[0] = (term){th_xstrdup(o->query), strlen(o->query), false, strchr(o->query, '/') != NULL};
    } else parse_terms(o->query, &terms);
    if (terms.n > 24) {
        terms_free(&terms);
        th_sb_puts(err, "too many search terms (at most 24)");
        return -1;
    }

    th_strbuf fts, where, lo, hi, sql, extension;
    th_sb_init(&extension);
    th_sb_init(&fts);
    th_sb_init(&where);
    th_sb_init(&lo);
    th_sb_init(&hi);
    th_sb_init(&sql);
    param_list params = {.n = 0};

    fts_query_terms(&terms, &fts);
    /* Probe only a bounded posting prefix. Broad matches can then stream the
     * ordered filename index, testing FTS membership per candidate instead of
     * materializing and sorting a million rowids before returning one page. */
    bool broad = false;
    if (o->timeout_ms > 0)
        sqlite3_progress_handler(db, 1000, deadline_cb, &deadline);
    if (fts.len && o->sort == TH_SORT_NAME && !o->want_total) {
        sqlite3_stmt *probe = th_db_prepare(db,"SELECT rowid FROM entries_fts WHERE entries_fts MATCH ? LIMIT 4097");
        if (probe) {
            th_bind_bytes(probe,1,fts.data,fts.len);
            int n = 0;
            while (n < 4097 && sqlite3_step(probe) == SQLITE_ROW) n++;
            broad = n == 4097;
            sqlite3_finalize(probe);
        }
    }
    th_sb_puts(&where, " WHERE 1");
    for (size_t i = 0; i < terms.n; i++) {
        th_sb_printf(&where, " AND %sth_match(?, e.%s, ?)", terms.v[i].neg ? "NOT " : "",
                     terms.v[i].path ? "path" : "name");
        push_text(&params, terms.v[i].s, terms.v[i].len);
        push_int(&params, o->match_flags);
    }
    if (o->parent_id >= 0) {
        th_sb_puts(&where, " AND e.parent_id = ?");
        push_int(&params, o->parent_id);
    }
    if (!o->show_hidden) th_sb_puts(&where, " AND substr(CAST(e.name AS BLOB),1,1) != x'2e'");
    if (o->extension && *o->extension) {
        th_sb_puts(&extension, "*.");
        const char *ext = o->extension;
        if (*ext == '.') ext++;
        for (; *ext; ext++) {
            if (*ext == '*' || *ext == '?' || *ext == '\\') th_sb_putc(&extension, '\\');
            th_sb_putc(&extension, *ext);
        }
        th_sb_puts(&where, " AND th_match(?, e.name, ?)");
        push_text(&params, extension.data, extension.len);
        push_int(&params, o->match_flags | TH_MATCH_FULL);
    }
    unsigned types = o->types & TH_TYPEMASK_ALL;
    if (types && types != TH_TYPEMASK_ALL) {
        th_sb_puts(&where, " AND e.type IN (");
        bool first = true;
        for (int t = 0; t < 4; t++) {
            if (types & TH_TYPEMASK(t)) {
                th_sb_printf(&where, first ? "%d" : ",%d", t);
                first = false;
            }
        }
        th_sb_putc(&where, ')');
    }
    if (o->min_size >= 0) {
        th_sb_puts(&where, " AND " SIZE_EXPR " >= ?");
        push_int(&params, o->min_size);
    }
    if (o->max_size >= 0) {
        th_sb_puts(&where, " AND " SIZE_EXPR " <= ?");
        push_int(&params, o->max_size);
    }
    if (o->min_mtime >= 0) {
        th_sb_puts(&where, " AND e.mtime >= ?");
        push_int(&params, o->min_mtime);
    }
    if (o->max_mtime >= 0) {
        th_sb_puts(&where, " AND e.mtime <= ?");
        push_int(&params, o->max_mtime);
    }
    if (o->root_id > 0) {
        th_sb_puts(&where, " AND e.root_id = ?");
        push_int(&params, o->root_id);
    }
    if (o->under && *o->under) {
        size_t ulen = strlen(o->under);
        while (ulen > 1 && o->under[ulen - 1] == '/')
            ulen--;
        th_subtree_range(o->under, ulen, &lo, &hi);
        th_sb_puts(&where, " AND e.path >= ? AND e.path < ?");
        push_text(&params, lo.data, lo.len);
        push_text(&params, hi.data, hi.len);
    }

    if (fts.len) {
        th_sb_puts(&where, broad ? " AND EXISTS (SELECT 1 FROM entries_fts WHERE rowid=e.id AND entries_fts MATCH ?)" : " AND e.id IN (SELECT rowid FROM entries_fts WHERE entries_fts MATCH ?)");
        push_text(&params, fts.data, fts.len);
        res->used_index = true;
    }

    int64_t limit = o->limit;
    if (limit <= 0 || limit > TH_MAX_PAGE)
        limit = limit <= 0 ? 100 : TH_MAX_PAGE;
    int64_t offset = o->offset > 0 ? o->offset : 0;

    th_sb_puts(&sql, "SELECT " TH_ENTRY_COLS " FROM entries e");
    if(o->parent_id>=0)th_sb_puts(&sql," INDEXED BY entries_parent");
    th_sb_append(&sql, where.data, where.len);
    order_clause(&sql, o->sort, o->descending);
    th_sb_puts(&sql, " LIMIT ? OFFSET ?");

    int rc = -1;
    if (o->timeout_ms > 0)
        sqlite3_progress_handler(db, 1000, deadline_cb, &deadline);
    sqlite3_stmt *st = th_db_prepare(db, sql.data);
    if (!st) {
        th_sb_printf(err, "search: %s", sqlite3_errmsg(db));
        goto out;
    }
    bind_all(st, &params);
    sqlite3_bind_int64(st, params.n + 1, limit);
    sqlite3_bind_int64(st, params.n + 2, offset);

    size_t cap = 0;
    int s;
    while ((s = sqlite3_step(st)) == SQLITE_ROW) {
        if (res->n == cap) {
            cap = cap ? cap * 2 : 64;
            res->items = th_xrealloc(res->items, cap * sizeof *res->items);
        }
        th_entry_from_stmt(&res->items[res->n++], st, 0);
    }
    if (s != SQLITE_DONE) {
        report_error(db, o, res, err);
        sqlite3_finalize(st);
        th_search_result_free(res);
        goto out;
    }
    sqlite3_finalize(st);

    if (o->want_total) {
        if ((int64_t)res->n < limit && (res->n > 0 || offset == 0)) {
            res->total = offset + (int64_t)res->n;
        } else {
            th_sb_reset(&sql);
            th_sb_puts(&sql, "SELECT count(*) FROM entries e");
            if(o->parent_id>=0)th_sb_puts(&sql," INDEXED BY entries_parent");
            th_sb_append(&sql, where.data, where.len);
            st = th_db_prepare(db, sql.data);
            if (!st) {
                th_sb_printf(err, "search: %s", sqlite3_errmsg(db));
                th_search_result_free(res);
                goto out;
            }
            bind_all(st, &params);
            s = sqlite3_step(st);
            if (s == SQLITE_ROW) {
                res->total = sqlite3_column_int64(st, 0);
            } else if (sqlite3_errcode(db) == SQLITE_INTERRUPT) {
                res->total = -1; /* counting is best effort under the guard */
            }
            sqlite3_finalize(st);
        }
    }
    rc = 0;

out:
    if (o->timeout_ms > 0)
        sqlite3_progress_handler(db, 0, NULL, NULL);
    res->elapsed_ms = (th_mono_sec() - t0) * 1000.0;
    th_sb_free(&fts);
    th_sb_free(&where);
    th_sb_free(&lo);
    th_sb_free(&hi);
    th_sb_free(&sql);
    th_sb_free(&extension);
    terms_free(&terms);
    return rc;
}

void th_search_result_free(th_search_result *res)
{
    for (size_t i = 0; i < res->n; i++)
        th_entry_clear(&res->items[i]);
    free(res->items);
    res->items = NULL;
    res->n = 0;
}
