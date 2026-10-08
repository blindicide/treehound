/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_JSON_H
#define TREEHOUND_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "treehound/strbuf.h"

/*
 * Minimal JSON support used by the IPC protocol and the CLI's --json output.
 *
 * Byte-safety: file names are arbitrary byte strings.  Bytes that are not part
 * of a valid UTF-8 sequence are emitted as lone low surrogates \udc80-\udcff
 * (the PEP 383 "surrogateescape" convention) and decoded back to the original
 * byte by th_json_parse(), so names round-trip exactly.
 */

#define TH_JSON_MAX_DEPTH 64

typedef struct {
    th_strbuf *sb;
    int depth;
    bool first[TH_JSON_MAX_DEPTH];
    bool after_key;
} th_jw;

void th_jw_init(th_jw *w, th_strbuf *sb);
void th_jw_obj_begin(th_jw *w);
void th_jw_obj_end(th_jw *w);
void th_jw_arr_begin(th_jw *w);
void th_jw_arr_end(th_jw *w);
void th_jw_key(th_jw *w, const char *key);
void th_jw_str(th_jw *w, const char *s);
void th_jw_bytes(th_jw *w, const char *s, size_t len);
void th_jw_int(th_jw *w, int64_t v);
void th_jw_double(th_jw *w, double v);
void th_jw_bool(th_jw *w, bool v);
void th_jw_null(th_jw *w);

void th_jw_kv_str(th_jw *w, const char *key, const char *s);
void th_jw_kv_bytes(th_jw *w, const char *key, const char *s, size_t len);
void th_jw_kv_int(th_jw *w, const char *key, int64_t v);
void th_jw_kv_double(th_jw *w, const char *key, double v);
void th_jw_kv_bool(th_jw *w, const char *key, bool v);

/* Append the JSON string encoding (with quotes) of the given bytes. */
void th_json_escape(th_strbuf *sb, const char *s, size_t len);

typedef enum {
    TH_JNULL,
    TH_JBOOL,
    TH_JNUM,
    TH_JSTR,
    TH_JARR,
    TH_JOBJ,
} th_jtype;

typedef struct th_jval th_jval;
struct th_jval {
    th_jtype type;
    bool b;
    bool is_int;
    int64_t i;
    double num;
    char *str; /* NUL-terminated, may also contain embedded bytes */
    size_t len;
    size_t n;      /* number of array items / object members */
    char **keys;   /* object member names (NUL-terminated) */
    th_jval *items;
};

/* Parses len bytes.  Returns NULL on error and sets *err to a static message. */
th_jval *th_json_parse(const char *text, size_t len, const char **err);
void th_json_free(th_jval *v);

const th_jval *th_json_get(const th_jval *obj, const char *key);
/* Convenience accessors returning def when absent or of the wrong type. */
const char *th_json_get_str(const th_jval *obj, const char *key, const char *def);
const char *th_json_get_bytes(const th_jval *obj, const char *key, size_t *len);
int64_t th_json_get_int(const th_jval *obj, const char *key, int64_t def);
bool th_json_get_bool(const th_jval *obj, const char *key, bool def);
double th_json_get_double(const th_jval *obj, const char *key, double def);

#endif
