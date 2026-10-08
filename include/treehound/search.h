/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_SEARCH_H
#define TREEHOUND_SEARCH_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "treehound/db.h"
#include "treehound/strbuf.h"

/*
 * Indexed path search.
 *
 * A query is a list of whitespace-separated terms (double quotes keep spaces
 * inside one term); every term must match.  A term starting with '!' must not
 * match.  Terms use th_pattern syntax: without '*' or '?' a term is a
 * substring match, otherwise an anchored glob.  Terms containing '/' are
 * matched against the full path, all others against the name.
 *
 * Positive name terms with a literal run of at least three characters are
 * looked up in the trigram FTS index; the candidates are then verified with
 * th_match().  Queries without such a run scan the entries table (still the
 * index database, never the live filesystem).
 */

typedef enum {
    TH_SORT_NAME = 0,
    TH_SORT_PATH = 1,
    TH_SORT_SIZE = 2, /* directories sort by their aggregate size */
    TH_SORT_MTIME = 3,
    TH_SORT_TYPE = 4, /* entry type, then name */
} th_sort_key;

/* bit masks for th_search_opts.types */
#define TH_TYPEMASK(t) (1u << (t))
#define TH_TYPEMASK_ALL 0xfu

typedef struct {
    const char *query; /* NULL or "" matches everything */
    int match_flags;   /* TH_MATCH_CASE */
    unsigned types;    /* TH_TYPEMASK bits; 0 means all */
    int64_t min_size;  /* -1: unbounded */
    int64_t max_size;
    int64_t min_mtime; /* -1: unbounded */
    int64_t max_mtime;
    int64_t root_id;   /* 0: all roots */
    const char *under; /* only entries strictly below this directory */
    int64_t parent_id; /* -1: any; otherwise immediate children */
    const char *extension; /* literal extension, without leading dot */
    bool show_hidden;
    th_sort_key sort;
    bool descending;
    int64_t offset;
    int64_t limit;     /* clamped to 1..TH_MAX_PAGE */
    bool want_total;   /* compute the total number of matches */
    int timeout_ms;    /* responsiveness guard; 0: none */
} th_search_opts;

typedef struct {
    th_entry *items;
    size_t n;
    int64_t total;   /* -1 when not computed */
    bool used_index; /* FTS candidates or broad-match ordered-index streaming */
    double elapsed_ms;
    bool timed_out;  /* th_search() failed because timeout_ms elapsed */
} th_search_result;

void th_search_opts_init(th_search_opts *o);

/* Parses a sort key name ("name", "path", "size", "mtime", "type"); false if unknown. */
bool th_sort_parse(const char *s, th_sort_key *out);
const char *th_sort_name(th_sort_key k);

/* Runs a search on db (read-only handles are fine).  Returns 0, or -1 with a
 * message in err. */
int th_search(sqlite3 *db, const th_search_opts *o, th_search_result *res, th_strbuf *err);
void th_search_result_free(th_search_result *res);

/* Builds the FTS5 MATCH expression for a query; empty when the query cannot
 * use the index.  Exposed for tests. */
void th_search_fts_query(const char *query, int match_flags, th_strbuf *out);

#endif
