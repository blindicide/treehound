/* SPDX-License-Identifier: MIT */
#include "treehound/match.h"
#include "treehound/strbuf.h"
#include "treehound/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

uint32_t th_fold(uint32_t cp)
{
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp > 0x10ffff)
        return cp;
    return (uint32_t)towlower((wint_t)cp);
}

#define WILD_STAR 0xffffffffu
#define WILD_ANY 0xfffffffeu

void th_pattern_compile(th_pattern *p, const char *pat, size_t len, int flags)
{
    const unsigned char *s = (const unsigned char *)pat;
    p->cps = th_xmalloc((len + 1) * sizeof *p->cps);
    p->lit = th_xmalloc((len + 1) * sizeof *p->lit);
    p->n = 0;
    p->glob = false;
    p->flags = flags;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        bool escaped = false;
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            escaped = true;
        }
        if (!escaped && s[i] == '*') {
            p->glob = true;
            i++;
            /* collapse consecutive stars */
            if (p->n > 0 && p->cps[p->n - 1] == WILD_STAR && !p->lit[p->n - 1])
                continue;
            p->cps[p->n] = WILD_STAR;
            p->lit[p->n++] = false;
            continue;
        }
        if (!escaped && s[i] == '?') {
            p->glob = true;
            i++;
            p->cps[p->n] = WILD_ANY;
            p->lit[p->n++] = false;
            continue;
        }
        i += th_utf8_decode(s + i, len - i, &cp);
        if (!(flags & TH_MATCH_CASE))
            cp = th_fold(cp);
        p->cps[p->n] = cp;
        p->lit[p->n++] = true;
    }
}

void th_pattern_free(th_pattern *p)
{
    free(p->cps);
    free(p->lit);
    p->cps = NULL;
    p->lit = NULL;
    p->n = 0;
}

static size_t decode_text(const char *text, size_t len, int flags, uint32_t *buf)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t n = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        i += th_utf8_decode(s + i, len - i, &cp);
        buf[n++] = (flags & TH_MATCH_CASE) ? cp : th_fold(cp);
    }
    return n;
}

static bool glob_match(const th_pattern *p, const uint32_t *t, size_t tn)
{
    size_t pi = 0, ti = 0;
    size_t star_p = (size_t)-1, star_t = 0;
    while (ti < tn) {
        if (pi < p->n) {
            uint32_t c = p->cps[pi];
            bool wild = !p->lit[pi];
            if (wild && c == WILD_STAR) {
                star_p = pi++;
                star_t = ti;
                continue;
            }
            if ((wild && c == WILD_ANY) || (!wild && c == t[ti])) {
                pi++;
                ti++;
                continue;
            }
        }
        if (star_p != (size_t)-1) {
            pi = star_p + 1;
            ti = ++star_t;
            continue;
        }
        return false;
    }
    while (pi < p->n && !p->lit[pi] && p->cps[pi] == WILD_STAR)
        pi++;
    return pi == p->n;
}

static bool substr_match(const th_pattern *p, const uint32_t *t, size_t tn)
{
    if (p->n == 0)
        return true;
    if (p->n > tn)
        return false;
    for (size_t i = 0; i + p->n <= tn; i++) {
        if (t[i] != p->cps[0])
            continue;
        size_t k = 1;
        while (k < p->n && t[i + k] == p->cps[k])
            k++;
        if (k == p->n)
            return true;
    }
    return false;
}

bool th_pattern_match(const th_pattern *p, const char *text, size_t len)
{
    uint32_t stackbuf[512];
    uint32_t *buf = len <= 512 ? stackbuf : th_xmalloc(len * sizeof *buf);
    size_t tn = decode_text(text, len, p->flags, buf);
    bool r = p->glob ? glob_match(p, buf, tn) : substr_match(p, buf, tn);
    if (buf != stackbuf)
        free(buf);
    return r;
}

bool th_match(const char *pat, size_t plen, const char *text, size_t tlen, int flags)
{
    th_pattern p;
    th_pattern_compile(&p, pat, plen, flags);
    bool r = th_pattern_match(&p, text, tlen);
    th_pattern_free(&p);
    return r;
}

bool th_pattern_has_wildcards(const char *pat, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (pat[i] == '\\') {
            i++;
            continue;
        }
        if (pat[i] == '*' || pat[i] == '?')
            return true;
    }
    return false;
}

void th_pattern_literals(const char *pat, size_t len,
                         void (*cb)(const char *run, size_t runlen, void *ud), void *ud)
{
    th_strbuf run;
    th_sb_init(&run);
    for (size_t i = 0; i < len; i++) {
        char c = pat[i];
        if (c == '\\' && i + 1 < len) {
            th_sb_putc(&run, pat[++i]);
            continue;
        }
        if (c == '*' || c == '?') {
            if (run.len)
                cb(run.data, run.len, ud);
            th_sb_reset(&run);
            continue;
        }
        th_sb_putc(&run, c);
    }
    if (run.len)
        cb(run.data, run.len, ud);
    th_sb_free(&run);
}
