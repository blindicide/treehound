/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_MATCH_H
#define TREEHOUND_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Name/path matching used by search.  A pattern without '*' or '?' is a
 * substring match; a pattern with wildcards is an anchored glob over the
 * whole text ('*' = any run, '?' = one character).  '\' escapes the next
 * pattern character.  Matching is per code point; case folding is simple
 * Unicode lower-casing.  Invalid UTF-8 bytes only match themselves.
 */

enum {
    TH_MATCH_CASE = 1 << 0, /* case-sensitive */
};

typedef struct {
    uint32_t *cps; /* folded pattern code points */
    bool *lit;     /* true when the code point is a literal (escaped or plain) */
    size_t n;
    bool glob;
    int flags;
} th_pattern;

void th_pattern_compile(th_pattern *p, const char *pat, size_t len, int flags);
void th_pattern_free(th_pattern *p);
bool th_pattern_match(const th_pattern *p, const char *text, size_t len);

/* One-shot helper. */
bool th_match(const char *pat, size_t plen, const char *text, size_t tlen, int flags);

/* True when the pattern contains unescaped wildcards. */
bool th_pattern_has_wildcards(const char *pat, size_t len);

/*
 * Splits the pattern into literal runs (wildcards and escapes removed).
 * Calls cb for each run with byte length >= 1.  Used to build FTS queries.
 */
void th_pattern_literals(const char *pat, size_t len,
                         void (*cb)(const char *run, size_t runlen, void *ud), void *ud);

/* Lower-cases a code point (simple case mapping, locale-independent for
 * ASCII, uses towlower for the rest). */
uint32_t th_fold(uint32_t cp);

#endif
