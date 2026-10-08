/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_INDEXER_H
#define TREEHOUND_INDEXER_H

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "treehound/config.h"
#include "treehound/mounts.h"
#include "treehound/strbuf.h"

/*
 * Filesystem scanner.
 *
 * A scan walks a directory tree iteratively (explicit stack, no recursion),
 * compares each directory's children with the rows already in the database
 * and writes only the differences, in batched transactions.  The same code
 * performs the first full index, the startup reconciliation of a cached index
 * and the rescan of a single stale subtree.  Symbolic links are recorded but
 * never followed; other filesystems are not entered unless cross_mounts is on.
 * Directory aggregates are recomputed bottom-up once the walk has finished.
 */

typedef struct {
    int64_t dirs;    /* directories read */
    int64_t entries; /* children examined */
    int64_t added;
    int64_t updated;
    int64_t removed; /* rows deleted, including whole subtrees */
    int64_t errors;  /* unreadable directories or entries */
} th_scan_stats;

typedef void (*th_scan_progress_fn)(const th_scan_stats *st, const char *path, void *ud);
/* Called for every directory that was read successfully. */
typedef void (*th_scan_dir_fn)(int64_t entry_id, const char *path, size_t len, void *ud);

typedef struct {
    const th_config *cfg;      /* exclusions and cross_mounts; NULL for defaults */
    const th_mounts *mounts;   /* mount table; NULL to rely on st_dev only */
    const atomic_bool *cancel; /* polled between directories */
    th_scan_progress_fn progress;
    th_scan_dir_fn on_dir;
    void *ud;               /* passed to progress and on_dir */
    unsigned progress_ms;   /* minimum interval between progress calls; 0 = 250 */
    size_t batch_rows;      /* writes per transaction; 0 = 10000 */
} th_scan_opts;

enum {
    TH_SCAN_FAILED = -1,
    TH_SCAN_OK = 0,
    TH_SCAN_CANCELLED = 1, /* stopped early; root left Stale with scan_in_progress */
    TH_SCAN_OFFLINE = 2,   /* root path missing or its filesystem unmounted */
};

/*
 * Scans or reconciles a whole root and updates its state: Updating while
 * running, then Verified, Offline, Error or (when cancelled) Stale.  Must not
 * be called inside a transaction.
 */
int th_scan_root(sqlite3 *db, int64_t root_id, const th_scan_opts *o, th_scan_stats *st, th_strbuf *err);

/*
 * Rescans the directory at path (or its nearest indexed ancestor when path is
 * not indexed) and propagates aggregate changes up to the root.  Root state is
 * left alone.
 */
int th_scan_subtree(sqlite3 *db, int64_t root_id, const char *path, size_t len, const th_scan_opts *o,
                    th_scan_stats *st, th_strbuf *err);

/* Recomputes the aggregates of a directory from its children's rows. */
int th_index_refresh_dir(sqlite3 *db, int64_t dir_id);
/* Recomputes aggregates of every ancestor of entry_id, nearest first. */
int th_index_refresh_ancestors(sqlite3 *db, int64_t entry_id);

/* Opens a directory by absolute path, walking component by component when
 * the path exceeds PATH_MAX.  The last component is not followed unless
 * follow is set. */
int th_open_dir(const char *path, size_t len, bool follow);

#endif
