/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_DAEMON_H
#define TREEHOUND_DAEMON_H

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "treehound/config.h"
#include "treehound/indexer.h"
#include "treehound/json.h"
#include "treehound/strbuf.h"

/*
 * treehoundd internals.
 *
 * Threads:
 *   main      - accepts connections, handles signals, owns shutdown
 *   scanner   - the only writer: runs the job queue on the read-write
 *               database connection (sync, scan, rebuild)
 *   conn (N)  - one per client connection, each with its own read-only
 *               database connection for queries
 *
 * Every state-changing request becomes a job with a sequence number; clients
 * wait for "completed_seq" in the status reply to reach it.
 */

typedef enum {
    JOB_SYNC = 0,    /* make the roots table match the configuration */
    JOB_SCAN = 1,    /* reconcile one root with the filesystem */
    JOB_REBUILD = 2, /* drop one root's entries and scan it from scratch */
} job_kind;

typedef struct {
    job_kind kind;
    int64_t root_id;
    uint64_t seq;
} job;

#define MAX_CONNECTIONS 32

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;      /* job queue changes, completions */
    pthread_cond_t conn_cv; /* connection count changes */

    char *config_path;
    char *db_path;
    char *socket_path;
    th_config cfg; /* guarded by mu */

    sqlite3 *wdb; /* writer connection, used by the scanner thread only */

    job *queue;
    size_t nqueue, capqueue;
    uint64_t next_seq;
    uint64_t completed_seq; /* every job with seq <= this has finished */

    /* the job being run (valid while busy) */
    bool busy;
    job current;
    char *current_path;
    th_scan_stats current_stats;
    int64_t current_started;
    int64_t last_reconcile; /* wall clock of the last full reconcile pass */

    atomic_bool cancel_scan; /* cancels the running scan */
    atomic_bool stopping;

    int wake_fd; /* eventfd: asks the main loop to shut down */
    int64_t started_at;

    int conn_fds[MAX_CONNECTIONS];
    int nconn;
} th_daemon;

/* scanner.c */
int scanner_start(th_daemon *d, pthread_t *th);
/* Queues a job, coalescing it with an identical queued one.  Returns its seq. */
uint64_t daemon_enqueue(th_daemon *d, job_kind kind, int64_t root_id, bool front);
/* Queues a scan of every root; returns the highest seq. */
uint64_t daemon_enqueue_all(th_daemon *d, sqlite3 *db);
/* Cancels the running scan when it belongs to root_id (0: any root). */
void daemon_cancel_root(th_daemon *d, int64_t root_id);
/* Makes the roots table match the configured roots.  Queues scans for new
 * roots when queue_scans is set.  Runs on the writer connection. */
int daemon_sync_roots(th_daemon *d, sqlite3 *db, bool queue_scans);
/* Effective roots: the configured ones, or the home directory.  Caller frees
 * each string and the array.  Requires d->mu. */
char **daemon_effective_roots(const th_daemon *d, size_t *n);

/* handlers.c */
/* Handles one request; writes the complete response JSON to out.  *db is the
 * connection's lazily opened read-only database handle. */
void daemon_handle(th_daemon *d, sqlite3 **db, const char *req, size_t len, th_strbuf *out);

#endif
