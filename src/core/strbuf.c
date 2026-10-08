/* SPDX-License-Identifier: MIT */
#include "treehound/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void)
{
    fputs("treehound: out of memory\n", stderr);
    abort();
}

void *th_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *th_xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom();
    return p;
}

void *th_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom();
    return q;
}

char *th_xmemdup(const void *p, size_t n)
{
    char *q = th_xmalloc(n + 1);
    if (n)
        memcpy(q, p, n);
    q[n] = '\0';
    return q;
}

char *th_xstrdup(const char *s)
{
    return th_xmemdup(s, strlen(s));
}

/* Shared terminator so an empty buffer's data is always a valid string. */
static char sb_empty[1];

void th_sb_init(th_strbuf *sb)
{
    sb->data = sb_empty;
    sb->len = 0;
    sb->cap = 0;
}

void th_sb_free(th_strbuf *sb)
{
    if (sb->cap)
        free(sb->data);
    th_sb_init(sb);
}

void th_sb_reset(th_strbuf *sb)
{
    sb->len = 0;
    if (sb->cap)
        sb->data[0] = '\0';
}

void th_sb_reserve(th_strbuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    sb->data = th_xrealloc(sb->cap ? sb->data : NULL, cap);
    sb->cap = cap;
}

void th_sb_append(th_strbuf *sb, const void *data, size_t len)
{
    th_sb_reserve(sb, len);
    if (len)
        memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void th_sb_puts(th_strbuf *sb, const char *s)
{
    th_sb_append(sb, s, strlen(s));
}

void th_sb_putc(th_strbuf *sb, char c)
{
    th_sb_append(sb, &c, 1);
}

void th_sb_vprintf(th_strbuf *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return;
    th_sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    sb->len += (size_t)n;
}

void th_sb_printf(th_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    th_sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

void th_sb_truncate(th_strbuf *sb, size_t len)
{
    if (len < sb->len) {
        sb->len = len;
        sb->data[len] = '\0';
    }
}

char *th_sb_steal(th_strbuf *sb)
{
    char *p = sb->cap ? sb->data : th_xstrdup("");
    th_sb_init(sb);
    return p;
}
