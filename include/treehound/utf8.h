/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_UTF8_H
#define TREEHOUND_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Length of the valid, shortest-form UTF-8 sequence at p (1-4), or 0 if the
 * bytes at p do not start a valid sequence. */
size_t th_utf8_seqlen(const unsigned char *p, size_t avail);

/* Encodes cp (<= 0x10FFFF) into out, returns number of bytes (1-4). */
size_t th_utf8_encode(uint32_t cp, char out[4]);

/*
 * Decodes one code point.  Invalid bytes decode to 0x110000 + byte so they
 * remain distinct from every real code point and never fold.  Returns the
 * number of bytes consumed (>= 1 when avail > 0).
 */
size_t th_utf8_decode(const unsigned char *p, size_t avail, uint32_t *cp);

/* True when the whole buffer is valid UTF-8. */
int th_utf8_valid(const char *s, size_t len);

#endif
