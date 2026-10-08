/* SPDX-License-Identifier: MIT */
#include "treehound/mounts.h"
#include "treehound/strbuf.h"
#include "treehound/util.h"

#include <stdlib.h>
#include <string.h>

/* mountinfo escapes space, tab, newline and backslash as \ooo. */
static char *unescape(const char *s, size_t len)
{
    char *out = th_xmalloc(len + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 3 < len && s[i + 1] >= '0' && s[i + 1] <= '3' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7') {
            out[o++] = (char)((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* Splits a line into space-separated fields (at most max). */
static size_t split(const char *line, size_t len, const char **f, size_t *fl, size_t max)
{
    size_t n = 0, i = 0;
    while (i < len && n < max) {
        while (i < len && line[i] == ' ')
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && line[i] != ' ')
            i++;
        f[n] = line + start;
        fl[n] = i - start;
        n++;
    }
    return n;
}

int th_mounts_parse(th_mounts *m, const char *text, size_t len)
{
    m->v = NULL;
    m->n = 0;
    size_t cap = 0;
    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(text + pos, '\n', len - pos);
        size_t llen = nl ? (size_t)(nl - (text + pos)) : len - pos;
        const char *line = text + pos;
        pos += llen + 1;

        const char *f[64];
        size_t fl[64];
        size_t nf = split(line, llen, f, fl, TH_ARRAY_LEN(f));
        /* 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue */
        size_t sep = 0;
        for (size_t i = 6; i < nf; i++) {
            if (fl[i] == 1 && f[i][0] == '-') {
                sep = i;
                break;
            }
        }
        if (sep == 0 || sep + 2 >= nf) {
            if (nf > 0)
                th_log(TH_LOG_DEBUG, "mountinfo: skipping malformed line");
            continue;
        }
        if (m->n == cap) {
            cap = cap ? cap * 2 : 32;
            m->v = th_xrealloc(m->v, cap * sizeof *m->v);
        }
        th_mount *mt = &m->v[m->n++];
        mt->root = unescape(f[3], fl[3]);
        mt->mnt = unescape(f[4], fl[4]);
        mt->fstype = unescape(f[sep + 1], fl[sep + 1]);
        mt->source = unescape(f[sep + 2], fl[sep + 2]);
    }
    return 0;
}

int th_mounts_load(th_mounts *m, const char *path)
{
    size_t len = 0;
    char *text = th_read_file(path ? path : "/proc/self/mountinfo", &len, 16u * 1024u * 1024u);
    if (!text) {
        m->v = NULL;
        m->n = 0;
        return -1;
    }
    int rc = th_mounts_parse(m, text, len);
    free(text);
    return rc;
}

void th_mounts_free(th_mounts *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].mnt);
        free(m->v[i].fstype);
        free(m->v[i].source);
        free(m->v[i].root);
    }
    free(m->v);
    m->v = NULL;
    m->n = 0;
}

const th_mount *th_mounts_at(const th_mounts *m, const char *path)
{
    /* Later entries shadow earlier ones mounted on the same point. */
    for (size_t i = m->n; i-- > 0;) {
        if (strcmp(m->v[i].mnt, path) == 0)
            return &m->v[i];
    }
    return NULL;
}

const th_mount *th_mounts_find(const th_mounts *m, const char *path)
{
    const th_mount *best = NULL;
    size_t bestlen = 0;
    for (size_t i = 0; i < m->n; i++) {
        size_t l = strlen(m->v[i].mnt);
        if (th_path_is_within(path, m->v[i].mnt) && (!best || l >= bestlen)) {
            best = &m->v[i];
            bestlen = l;
        }
    }
    return best;
}

bool th_fstype_is_pseudo(const char *fstype)
{
    static const char *const pseudo[] = {
        "proc",     "sysfs",     "devtmpfs", "devpts",     "cgroup",   "cgroup2", "securityfs",
        "debugfs",  "tracefs",   "pstore",   "bpf",        "configfs", "fusectl", "mqueue",
        "hugetlbfs", "autofs",   "binfmt_misc", "efivarfs", "nsfs",    "rpc_pipefs", "selinuxfs",
        "ramfs",
    };
    for (size_t i = 0; i < TH_ARRAY_LEN(pseudo); i++) {
        if (strcmp(fstype, pseudo[i]) == 0)
            return true;
    }
    return false;
}

bool th_mounts_is_hidden(const th_mounts *m, const char *path)
{
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->v[i].fstype, "ecryptfs") == 0 && m->v[i].source[0] == '/' &&
            th_path_is_within(path, m->v[i].source))
            return true;
    }
    return false;
}
