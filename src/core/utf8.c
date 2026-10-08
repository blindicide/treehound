/* SPDX-License-Identifier: MIT */
#include "treehound/utf8.h"

size_t th_utf8_seqlen(const unsigned char *p, size_t avail)
{
    if (avail == 0)
        return 0;
    unsigned char c = p[0];
    if (c < 0x80)
        return 1;
    size_t n;
    uint32_t min;
    uint32_t cp;
    if (c >= 0xc2 && c <= 0xdf) {
        n = 2;
        min = 0x80;
        cp = c & 0x1fu;
    } else if (c >= 0xe0 && c <= 0xef) {
        n = 3;
        min = 0x800;
        cp = c & 0x0fu;
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 4;
        min = 0x10000;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (avail < n)
        return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3fu);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    return n;
}

size_t th_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

size_t th_utf8_decode(const unsigned char *p, size_t avail, uint32_t *cp)
{
    size_t n = th_utf8_seqlen(p, avail);
    if (n == 0) {
        *cp = 0x110000u + p[0];
        return 1;
    }
    uint32_t v;
    switch (n) {
    case 1: v = p[0]; break;
    case 2: v = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3fu); break;
    case 3: v = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3fu); break;
    default:
        v = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
            ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3fu);
    }
    *cp = v;
    return n;
}

int th_utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        size_t n = th_utf8_seqlen(p + i, len - i);
        if (n == 0)
            return 0;
        i += n;
    }
    return 1;
}
