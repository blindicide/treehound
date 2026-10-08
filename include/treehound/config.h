/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_CONFIG_H
#define TREEHOUND_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "treehound/strbuf.h"

/*
 * Daemon configuration, stored as a small INI-style file at
 * $XDG_CONFIG_HOME/treehound/treehound.conf:
 *
 *   # comment
 *   root = /home/user
 *   root = /data
 *   exclude = *\/.cache
 *   cross_mounts = false
 *   watch = true
 *   reconcile_interval_hours = 24
 *   snapshot_retention = 30
 *   size_metric = allocated        # or logical
 *   case_sensitive = false
 *   background_service = false
 *
 * Unknown keys are preserved as warnings, never fatal.  GUI preferences live
 * in gui.conf in the same directory (written only by the GUI).
 */

enum { TH_METRIC_LOGICAL = 0, TH_METRIC_ALLOCATED = 1 };

typedef struct {
    char **roots;
    size_t nroots;
    char **excludes;
    size_t nexcludes;
    bool roots_set; /* at least one root= line was present */
    bool cross_mounts;
    bool watch;
    int reconcile_interval_hours; /* 0 = only at startup */
    int snapshot_retention;       /* number of snapshots kept per root */
    int size_metric;
    bool case_sensitive;
    bool background_service; /* informational; the unit is enabled by the user */
} th_config;

void th_config_defaults(th_config *c);
void th_config_free(th_config *c);
/*
 * Loads path into c (which must hold defaults).  A missing file is not an
 * error.  Returns 0 on success, -1 on syntax errors (messages in err).
 * Warnings (unknown keys) are appended to err as well but return 0.
 */
int th_config_load(th_config *c, const char *path, th_strbuf *err);
int th_config_parse(th_config *c, const char *text, size_t len, th_strbuf *err);
void th_config_serialize(const th_config *c, th_strbuf *out);
int th_config_save(const th_config *c, const char *path);

/* Adds/removes a root (normalised path).  Return true when changed. */
bool th_config_add_root(th_config *c, const char *path);
bool th_config_remove_root(th_config *c, const char *path);

/* True when the absolute path matches an exclude pattern (fnmatch on the
 * full path, or on the base name when the pattern has no '/'). */
bool th_config_is_excluded(const th_config *c, const char *path, const char *name);

bool th_parse_bool(const char *s, bool *out);

#endif
