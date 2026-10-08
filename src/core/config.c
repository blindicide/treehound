/* SPDX-License-Identifier: MIT */
#include "treehound/config.h"
#include "treehound/util.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

void th_config_defaults(th_config *c)
{
    memset(c, 0, sizeof *c);
    c->cross_mounts = false;
    c->watch = true;
    c->reconcile_interval_hours = 24;
    c->snapshot_retention = 30;
    c->size_metric = TH_METRIC_ALLOCATED;
    c->case_sensitive = false;
    c->background_service = false;
}

static void free_list(char **v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

void th_config_free(th_config *c)
{
    free_list(c->roots, c->nroots);
    free_list(c->excludes, c->nexcludes);
    c->roots = c->excludes = NULL;
    c->nroots = c->nexcludes = 0;
}

static void list_push(char ***v, size_t *n, const char *s)
{
    *v = th_xrealloc(*v, (*n + 1) * sizeof **v);
    (*v)[(*n)++] = th_xstrdup(s);
}

bool th_parse_bool(const char *s, bool *out)
{
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on") || !strcmp(s, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off") || !strcmp(s, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < lo || v > hi)
        return false;
    *out = (int)v;
    return true;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

bool th_config_add_root(th_config *c, const char *path)
{
    char *norm = th_path_normalize(path);
    for (size_t i = 0; i < c->nroots; i++) {
        if (strcmp(c->roots[i], norm) == 0) {
            free(norm);
            return false;
        }
    }
    list_push(&c->roots, &c->nroots, norm);
    free(norm);
    c->roots_set = true;
    return true;
}

bool th_config_remove_root(th_config *c, const char *path)
{
    char *norm = th_path_normalize(path);
    bool changed = false;
    for (size_t i = 0; i < c->nroots; i++) {
        if (strcmp(c->roots[i], norm) == 0) {
            free(c->roots[i]);
            memmove(&c->roots[i], &c->roots[i + 1], (c->nroots - i - 1) * sizeof *c->roots);
            c->nroots--;
            changed = true;
            break;
        }
    }
    free(norm);
    c->roots_set = true;
    return changed;
}

int th_config_parse(th_config *c, const char *text, size_t len, th_strbuf *err)
{
    char *copy = th_xmemdup(text, len);
    int rc = 0;
    int lineno = 0;
    bool in_main = true;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char *l = trim(line);
        if (*l == '\0' || *l == '#' || *l == ';')
            continue;
        if (*l == '[') {
            in_main = strncmp(l, "[treehound]", 11) == 0 || strncmp(l, "[daemon]", 8) == 0;
            continue;
        }
        if (!in_main)
            continue;
        char *eq = strchr(l, '=');
        if (!eq) {
            if (err)
                th_sb_printf(err, "line %d: expected key = value\n", lineno);
            rc = -1;
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        bool ok = true;
        if (!strcmp(key, "root")) {
            if (val[0] != '/')
                ok = false;
            else
                th_config_add_root(c, val);
        } else if (!strcmp(key, "exclude")) {
            if (*val)
                list_push(&c->excludes, &c->nexcludes, val);
        } else if (!strcmp(key, "cross_mounts")) {
            ok = th_parse_bool(val, &c->cross_mounts);
        } else if (!strcmp(key, "watch")) {
            ok = th_parse_bool(val, &c->watch);
        } else if (!strcmp(key, "reconcile_interval_hours")) {
            ok = parse_int(val, 0, 24 * 365, &c->reconcile_interval_hours);
        } else if (!strcmp(key, "snapshot_retention")) {
            ok = parse_int(val, 1, 100000, &c->snapshot_retention);
        } else if (!strcmp(key, "size_metric")) {
            if (!strcmp(val, "logical"))
                c->size_metric = TH_METRIC_LOGICAL;
            else if (!strcmp(val, "allocated"))
                c->size_metric = TH_METRIC_ALLOCATED;
            else
                ok = false;
        } else if (!strcmp(key, "case_sensitive")) {
            ok = th_parse_bool(val, &c->case_sensitive);
        } else if (!strcmp(key, "background_service")) {
            ok = th_parse_bool(val, &c->background_service);
        } else {
            if (err)
                th_sb_printf(err, "line %d: unknown key '%s' (ignored)\n", lineno, key);
            continue;
        }
        if (!ok) {
            if (err)
                th_sb_printf(err, "line %d: invalid value for '%s'\n", lineno, key);
            rc = -1;
        }
    }
    /* Paths are globally unique in the index; overlapping roots would steal
     * entry ownership and produce inconsistent totals. Validate the final set. */
    for(size_t i=0;i<c->nroots;i++)for(size_t j=i+1;j<c->nroots;j++) {
        const char *a=c->roots[i],*b=c->roots[j];size_t al=strlen(a),bl=strlen(b);
        if((al<bl && !memcmp(a,b,al) && (al==1 || b[al]=='/')) ||
           (bl<al && !memcmp(a,b,bl) && (bl==1 || a[bl]=='/'))) {
            if(err)th_sb_printf(err,"overlapping roots: %s and %s; select disjoint roots\n",a,b);
            rc=-1;
        }
    }
    free(copy);
    return rc;
}

int th_config_load(th_config *c, const char *path, th_strbuf *err)
{
    size_t len;
    char *text = th_read_file(path, &len, 1u << 20);
    if (!text) {
        if (errno == ENOENT)
            return 0;
        if (err)
            th_sb_printf(err, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    int rc = th_config_parse(c, text, len, err);
    free(text);
    return rc;
}

void th_config_serialize(const th_config *c, th_strbuf *out)
{
    th_sb_puts(out, "# Treehound daemon configuration (see README.md).\n"
                    "# Written by treehound; comments are not preserved.\n");
    for (size_t i = 0; i < c->nroots; i++)
        th_sb_printf(out, "root = %s\n", c->roots[i]);
    if (c->roots_set && c->nroots == 0)
        th_sb_puts(out, "# all roots removed\n");
    for (size_t i = 0; i < c->nexcludes; i++)
        th_sb_printf(out, "exclude = %s\n", c->excludes[i]);
    th_sb_printf(out, "cross_mounts = %s\n", c->cross_mounts ? "true" : "false");
    th_sb_printf(out, "watch = %s\n", c->watch ? "true" : "false");
    th_sb_printf(out, "reconcile_interval_hours = %d\n", c->reconcile_interval_hours);
    th_sb_printf(out, "snapshot_retention = %d\n", c->snapshot_retention);
    th_sb_printf(out, "size_metric = %s\n", c->size_metric == TH_METRIC_LOGICAL ? "logical" : "allocated");
    th_sb_printf(out, "case_sensitive = %s\n", c->case_sensitive ? "true" : "false");
    th_sb_printf(out, "background_service = %s\n", c->background_service ? "true" : "false");
}

int th_config_save(const th_config *c, const char *path)
{
    char *dir = th_xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        th_mkdir_p(dir, 0700);
    }
    free(dir);
    th_strbuf sb;
    th_sb_init(&sb);
    th_config_serialize(c, &sb);
    int rc = th_write_file_atomic(path, sb.data, sb.len, 0600);
    th_sb_free(&sb);
    return rc;
}

bool th_config_is_excluded(const th_config *c, const char *path, const char *name)
{
    for (size_t i = 0; i < c->nexcludes; i++) {
        const char *pat = c->excludes[i];
        if (strchr(pat, '/')) {
            if (fnmatch(pat, path, FNM_PATHNAME) == 0)
                return true;
        } else if (fnmatch(pat, name, 0) == 0) {
            return true;
        }
    }
    return false;
}
