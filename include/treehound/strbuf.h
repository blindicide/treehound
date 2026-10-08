/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_STRBUF_H
#define TREEHOUND_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

/* Growable byte buffer that is always NUL-terminated (data may contain NULs). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} th_strbuf;

void th_sb_init(th_strbuf *sb);
void th_sb_free(th_strbuf *sb);
void th_sb_reset(th_strbuf *sb);
void th_sb_reserve(th_strbuf *sb, size_t extra);
void th_sb_append(th_strbuf *sb, const void *data, size_t len);
void th_sb_puts(th_strbuf *sb, const char *s);
void th_sb_putc(th_strbuf *sb, char c);
void th_sb_printf(th_strbuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void th_sb_vprintf(th_strbuf *sb, const char *fmt, va_list ap);
void th_sb_truncate(th_strbuf *sb, size_t len);
/* Transfers ownership of the buffer to the caller and reinitialises sb. */
char *th_sb_steal(th_strbuf *sb);

/* Allocation helpers that abort on out-of-memory. */
void *th_xmalloc(size_t n);
void *th_xcalloc(size_t n, size_t size);
void *th_xrealloc(void *p, size_t n);
char *th_xstrdup(const char *s);
char *th_xmemdup(const void *p, size_t n); /* NUL-terminated copy */

#endif
