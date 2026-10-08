/* SPDX-License-Identifier: MIT */
#include "treehound/json.h"
#include "treehound/utf8.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- writer */

void th_jw_init(th_jw *w, th_strbuf *sb)
{
    w->sb = sb;
    w->depth = 0;
    w->first[0] = true;
    w->after_key = false;
}

static void sep(th_jw *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (!w->first[w->depth])
        th_sb_putc(w->sb, ',');
    w->first[w->depth] = false;
}

static void push(th_jw *w, char c)
{
    sep(w);
    th_sb_putc(w->sb, c);
    if (w->depth + 1 < TH_JSON_MAX_DEPTH)
        w->depth++;
    w->first[w->depth] = true;
}

static void pop(th_jw *w, char c)
{
    th_sb_putc(w->sb, c);
    if (w->depth > 0)
        w->depth--;
}

void th_jw_obj_begin(th_jw *w) { push(w, '{'); }
void th_jw_obj_end(th_jw *w) { pop(w, '}'); }
void th_jw_arr_begin(th_jw *w) { push(w, '['); }
void th_jw_arr_end(th_jw *w) { pop(w, ']'); }

void th_jw_key(th_jw *w, const char *key)
{
    sep(w);
    th_json_escape(w->sb, key, strlen(key));
    th_sb_putc(w->sb, ':');
    w->after_key = true;
}

void th_json_escape(th_strbuf *sb, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s;
    th_sb_reserve(sb, len + 2);
    th_sb_putc(sb, '"');
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c < 0x80) {
            switch (c) {
            case '"': th_sb_puts(sb, "\\\""); break;
            case '\\': th_sb_puts(sb, "\\\\"); break;
            case '\n': th_sb_puts(sb, "\\n"); break;
            case '\r': th_sb_puts(sb, "\\r"); break;
            case '\t': th_sb_puts(sb, "\\t"); break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
                    th_sb_puts(sb, esc);
                } else {
                    th_sb_putc(sb, (char)c);
                }
            }
            i++;
            continue;
        }
        size_t n = th_utf8_seqlen(p + i, len - i);
        if (n > 0) {
            th_sb_append(sb, p + i, n);
            i += n;
        } else {
            /* surrogateescape: invalid byte -> \udcXX */
            char esc[7] = {'\\', 'u', 'd', 'c', hex[c >> 4], hex[c & 15], 0};
            th_sb_puts(sb, esc);
            i++;
        }
    }
    th_sb_putc(sb, '"');
}

void th_jw_bytes(th_jw *w, const char *s, size_t len)
{
    sep(w);
    th_json_escape(w->sb, s, len);
}

void th_jw_str(th_jw *w, const char *s)
{
    if (!s) {
        th_jw_null(w);
        return;
    }
    th_jw_bytes(w, s, strlen(s));
}

void th_jw_int(th_jw *w, int64_t v)
{
    sep(w);
    th_sb_printf(w->sb, "%" PRId64, v);
}

void th_jw_double(th_jw *w, double v)
{
    sep(w);
    if (!isfinite(v))
        th_sb_puts(w->sb, "null");
    else
        th_sb_printf(w->sb, "%.6g", v);
}

void th_jw_bool(th_jw *w, bool v)
{
    sep(w);
    th_sb_puts(w->sb, v ? "true" : "false");
}

void th_jw_null(th_jw *w)
{
    sep(w);
    th_sb_puts(w->sb, "null");
}

void th_jw_kv_str(th_jw *w, const char *key, const char *s)
{
    th_jw_key(w, key);
    th_jw_str(w, s);
}

void th_jw_kv_bytes(th_jw *w, const char *key, const char *s, size_t len)
{
    th_jw_key(w, key);
    th_jw_bytes(w, s, len);
}

void th_jw_kv_int(th_jw *w, const char *key, int64_t v)
{
    th_jw_key(w, key);
    th_jw_int(w, v);
}

void th_jw_kv_double(th_jw *w, const char *key, double v)
{
    th_jw_key(w, key);
    th_jw_double(w, v);
}

void th_jw_kv_bool(th_jw *w, const char *key, bool v)
{
    th_jw_key(w, key);
    th_jw_bool(w, v);
}

/* ---------------------------------------------------------------- parser */

typedef struct {
    const char *p;
    const char *end;
    const char *err;
    int depth;
} parser;

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static bool parse_value(parser *ps, th_jval *out);

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool read_hex4(parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4)
        return false;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0)
            return false;
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static bool parse_string(parser *ps, char **out, size_t *outlen)
{
    /* assumes *ps->p == '"' */
    ps->p++;
    th_strbuf sb;
    th_sb_init(&sb);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            *outlen = sb.len;
            *out = th_sb_steal(&sb);
            return true;
        }
        if (c < 0x20) {
            ps->err = "control character in string";
            goto fail;
        }
        if (c != '\\') {
            th_sb_putc(&sb, (char)c);
            ps->p++;
            continue;
        }
        ps->p++;
        if (ps->p >= ps->end)
            break;
        char e = *ps->p++;
        switch (e) {
        case '"': th_sb_putc(&sb, '"'); break;
        case '\\': th_sb_putc(&sb, '\\'); break;
        case '/': th_sb_putc(&sb, '/'); break;
        case 'b': th_sb_putc(&sb, '\b'); break;
        case 'f': th_sb_putc(&sb, '\f'); break;
        case 'n': th_sb_putc(&sb, '\n'); break;
        case 'r': th_sb_putc(&sb, '\r'); break;
        case 't': th_sb_putc(&sb, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!read_hex4(ps, &cp)) {
                ps->err = "bad \\u escape";
                goto fail;
            }
            if (cp >= 0xd800 && cp <= 0xdbff) {
                unsigned lo;
                if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    const char *save = ps->p;
                    ps->p += 2;
                    if (read_hex4(ps, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    } else {
                        ps->p = save;
                        ps->err = "unpaired surrogate";
                        goto fail;
                    }
                } else {
                    ps->err = "unpaired surrogate";
                    goto fail;
                }
            } else if (cp >= 0xdc80 && cp <= 0xdcff) {
                /* surrogateescape: original raw byte */
                th_sb_putc(&sb, (char)(unsigned char)(cp - 0xdc00));
                break;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                ps->err = "unpaired surrogate";
                goto fail;
            }
            if (cp == 0) {
                ps->err = "NUL in string";
                goto fail;
            }
            char tmp[4];
            size_t n = th_utf8_encode(cp, tmp);
            th_sb_append(&sb, tmp, n);
            break;
        }
        default:
            ps->err = "bad escape";
            goto fail;
        }
    }
    if (!ps->err)
        ps->err = "unterminated string";
fail:
    th_sb_free(&sb);
    return false;
}

static bool parse_number(parser *ps, th_jval *out)
{
    const char *start = ps->p;
    bool integral = true;
    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9')) {
        ps->err = "bad number";
        return false;
    }
    if (*ps->p == '0' && ps->p + 1 < ps->end && ps->p[1] >= '0' && ps->p[1] <= '9') {
        ps->err = "leading zero";
        return false;
    }
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c >= '0' && c <= '9') {
            ps->p++;
        } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            integral = false;
            ps->p++;
        } else {
            break;
        }
    }
    size_t n = (size_t)(ps->p - start);
    char buf[64];
    if (n >= sizeof buf) {
        ps->err = "number too long";
        return false;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    char *endp;
    out->type = TH_JNUM;
    errno = 0;
    if (integral) {
        long long v = strtoll(buf, &endp, 10);
        if (*endp == '\0' && errno == 0) {
            out->is_int = true;
            out->i = (int64_t)v;
            out->num = (double)v;
            return true;
        }
    }
    errno = 0;
    double d = strtod(buf, &endp);
    if (*endp != '\0') {
        ps->err = "bad number";
        return false;
    }
    out->num = d;
    out->is_int = false;
    out->i = (d >= -9.2e18 && d <= 9.2e18) ? (int64_t)d : 0;
    return true;
}

static bool lit(parser *ps, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        ps->err = "unexpected token";
        return false;
    }
    ps->p += n;
    return true;
}

static void free_contents(th_jval *v);

static bool parse_container(parser *ps, th_jval *out, bool is_obj)
{
    if (++ps->depth > TH_JSON_MAX_DEPTH) {
        ps->err = "nesting too deep";
        return false;
    }
    ps->p++;
    out->type = is_obj ? TH_JOBJ : TH_JARR;
    size_t cap = 0;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (is_obj ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return true;
    }
    for (;;) {
        skip_ws(ps);
        char *key = NULL;
        if (is_obj) {
            size_t klen;
            if (ps->p >= ps->end || *ps->p != '"') {
                ps->err = "expected key";
                return false;
            }
            if (!parse_string(ps, &key, &klen))
                return false;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                ps->err = "expected ':'";
                return false;
            }
            ps->p++;
        }
        if (out->n == cap) {
            cap = cap ? cap * 2 : 4;
            out->items = th_xrealloc(out->items, cap * sizeof *out->items);
            if (is_obj)
                out->keys = th_xrealloc(out->keys, cap * sizeof *out->keys);
        }
        th_jval *item = &out->items[out->n];
        memset(item, 0, sizeof *item);
        if (is_obj)
            out->keys[out->n] = key;
        out->n++;
        if (!parse_value(ps, item))
            return false;
        skip_ws(ps);
        if (ps->p >= ps->end) {
            ps->err = "unexpected end";
            return false;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == (is_obj ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return true;
        }
        ps->err = "expected ',' or end of container";
        return false;
    }
}

static bool parse_value(parser *ps, th_jval *out)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        ps->err = "unexpected end";
        return false;
    }
    switch (*ps->p) {
    case '{': return parse_container(ps, out, true);
    case '[': return parse_container(ps, out, false);
    case '"':
        out->type = TH_JSTR;
        return parse_string(ps, &out->str, &out->len);
    case 't':
        out->type = TH_JBOOL;
        out->b = true;
        return lit(ps, "true");
    case 'f':
        out->type = TH_JBOOL;
        out->b = false;
        return lit(ps, "false");
    case 'n':
        out->type = TH_JNULL;
        return lit(ps, "null");
    default:
        return parse_number(ps, out);
    }
}

th_jval *th_json_parse(const char *text, size_t len, const char **err)
{
    parser ps = {text, text + len, NULL, 0};
    th_jval *v = th_xcalloc(1, sizeof *v);
    if (!parse_value(&ps, v)) {
        if (err)
            *err = ps.err ? ps.err : "parse error";
        th_json_free(v);
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        if (err)
            *err = "trailing data";
        th_json_free(v);
        return NULL;
    }
    return v;
}

static void free_contents(th_jval *v)
{
    free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        free_contents(&v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
}

void th_json_free(th_jval *v)
{
    if (!v)
        return;
    free_contents(v);
    free(v);
}

const th_jval *th_json_get(const th_jval *obj, const char *key)
{
    if (!obj || obj->type != TH_JOBJ)
        return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0)
            return &obj->items[i];
    return NULL;
}

const char *th_json_get_str(const th_jval *obj, const char *key, const char *def)
{
    const th_jval *v = th_json_get(obj, key);
    return (v && v->type == TH_JSTR) ? v->str : def;
}

const char *th_json_get_bytes(const th_jval *obj, const char *key, size_t *len)
{
    const th_jval *v = th_json_get(obj, key);
    if (!v || v->type != TH_JSTR) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = v->len;
    return v->str;
}

int64_t th_json_get_int(const th_jval *obj, const char *key, int64_t def)
{
    const th_jval *v = th_json_get(obj, key);
    if (!v)
        return def;
    if (v->type == TH_JNUM)
        return v->i;
    if (v->type == TH_JBOOL)
        return v->b ? 1 : 0;
    return def;
}

bool th_json_get_bool(const th_jval *obj, const char *key, bool def)
{
    const th_jval *v = th_json_get(obj, key);
    if (!v)
        return def;
    if (v->type == TH_JBOOL)
        return v->b;
    if (v->type == TH_JNUM)
        return v->i != 0;
    return def;
}

double th_json_get_double(const th_jval *obj, const char *key, double def)
{
    const th_jval *v = th_json_get(obj, key);
    return (v && v->type == TH_JNUM) ? v->num : def;
}
