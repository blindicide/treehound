/* SPDX-License-Identifier: MIT */
#include "daemon.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "treehound/common.h"
#include "treehound/db.h"
#include "treehound/mounts.h"
#include "treehound/util.h"

char **daemon_effective_roots(const th_daemon *d, size_t *n)
{
    char **v;
    if (d->cfg.roots_set) {
        *n = d->cfg.nroots;
        v = th_xcalloc(d->cfg.nroots ? d->cfg.nroots : 1, sizeof *v);
        for (size_t i = 0; i < d->cfg.nroots; i++)
            v[i] = th_xstrdup(d->cfg.roots[i]);
    } else {
        *n = 1;
        v = th_xcalloc(1, sizeof *v);
        v[0] = th_home_dir();
    }
    return v;
}

static void free_strv(char **v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

/* Requires d->mu. */
static uint64_t enqueue_locked(th_daemon *d, job_kind kind, int64_t root_id, bool front)
{
    for (size_t i = 0; i < d->nqueue; i++) {
        job *q = &d->queue[i];
        if (q->kind == kind && q->root_id == root_id) {
            /* an identical job is already waiting: it will cover this request,
             * but must not complete before jobs queued after it */
            q->seq = ++d->next_seq;
            if (!front && i + 1 < d->nqueue) {
                job moved = *q;
                memmove(q, q + 1, (d->nqueue - i - 1) * sizeof *q);
                d->queue[d->nqueue - 1] = moved;
            }
            pthread_cond_broadcast(&d->cv);
            return d->next_seq;
        }
    }
    if (d->nqueue == d->capqueue) {
        d->capqueue = d->capqueue ? d->capqueue * 2 : 16;
        d->queue = th_xrealloc(d->queue, d->capqueue * sizeof *d->queue);
    }
    job j = {.kind = kind, .root_id = root_id, .seq = ++d->next_seq};
    if (front) {
        memmove(d->queue + 1, d->queue, d->nqueue * sizeof *d->queue);
        d->queue[0] = j;
    } else {
        d->queue[d->nqueue] = j;
    }
    d->nqueue++;
    pthread_cond_broadcast(&d->cv);
    return j.seq;
}

uint64_t daemon_enqueue(th_daemon *d, job_kind kind, int64_t root_id, bool front)
{
    pthread_mutex_lock(&d->mu);
    uint64_t seq = enqueue_locked(d, kind, root_id, front);
    pthread_mutex_unlock(&d->mu);
    return seq;
}

uint64_t daemon_enqueue_all(th_daemon *d, sqlite3 *db)
{
    th_root *roots = NULL;
    size_t n = 0;
    uint64_t seq = 0;
    if (th_db_roots(db, &roots, &n) != 0)
        return 0;
    pthread_mutex_lock(&d->mu);
    for (size_t i = 0; i < n; i++)
        seq = enqueue_locked(d, JOB_SCAN, roots[i].id, false);
    if (n == 0)
        seq = d->completed_seq;
    pthread_mutex_unlock(&d->mu);
    th_roots_free(roots, n);
    return seq;
}

void daemon_cancel_root(th_daemon *d, int64_t root_id)
{
    pthread_mutex_lock(&d->mu);
    if (d->busy && (root_id == 0 || d->current.root_id == root_id))
        atomic_store(&d->cancel_scan, true);
    /* queued work for a removed root is pointless */
    size_t k = 0;
    for (size_t i = 0; i < d->nqueue; i++) {
        if (root_id != 0 && d->queue[i].root_id == root_id && d->queue[i].kind != JOB_SYNC) {
            /* keep the seq visible as completed: the next job will cover it */
            continue;
        }
        d->queue[k++] = d->queue[i];
    }
    d->nqueue = k;
    pthread_mutex_unlock(&d->mu);
}

int daemon_sync_roots(th_daemon *d, sqlite3 *db, bool queue_scans)
{
    pthread_mutex_lock(&d->mu);
    size_t nwant = 0;
    char **want = daemon_effective_roots(d, &nwant);
    pthread_mutex_unlock(&d->mu);

    th_root *have = NULL;
    size_t nhave = 0;
    if (th_db_roots(db, &have, &nhave) != 0) {
        free_strv(want, nwant);
        return -1;
    }
    int rc = 0;
    /* roots that are no longer configured lose their entries */
    for (size_t i = 0; i < nhave; i++) {
        bool keep = false;
        for (size_t j = 0; j < nwant && !keep; j++)
            keep = strcmp(have[i].path, want[j]) == 0;
        if (!keep) {
            th_log(TH_LOG_INFO, "removing root %s", have[i].path);
            if (th_db_root_delete(db, have[i].id) != 0)
                rc = -1;
        }
    }
    for (size_t j = 0; j < nwant; j++) {
        if (th_db_root_find(db, want[j]) > 0)
            continue;
        int64_t id = th_db_root_ensure(db, want[j]);
        if (id <= 0) {
            th_log(TH_LOG_ERROR, "cannot add root %s", want[j]);
            rc = -1;
            continue;
        }
        th_log(TH_LOG_INFO, "added root %s", want[j]);
        if (queue_scans)
            daemon_enqueue(d, JOB_SCAN, id, false);
    }
    th_roots_free(have, nhave);
    free_strv(want, nwant);
    return rc;
}

/* Deep copy of the configuration (taken under d->mu) for a scan to use
 * without holding the lock. */
static void config_snapshot(th_daemon *d, th_config *out)
{
    th_strbuf sb;
    th_sb_init(&sb);
    pthread_mutex_lock(&d->mu);
    th_config_serialize(&d->cfg, &sb);
    pthread_mutex_unlock(&d->mu);
    th_config_defaults(out);
    if (th_config_parse(out, sb.data ? sb.data : "", sb.len, NULL) != 0)
        th_log(TH_LOG_ERROR, "internal: configuration did not round-trip");
    th_sb_free(&sb);
}

static void on_progress(const th_scan_stats *st, const char *path, void *ud)
{
    th_daemon *d = ud;
    pthread_mutex_lock(&d->mu);
    d->current_stats = *st;
    free(d->current_path);
    d->current_path = path ? th_xstrdup(path) : NULL;
    pthread_mutex_unlock(&d->mu);
}

static void run_scan(th_daemon *d, int64_t root_id)
{
    th_config cfg;
    config_snapshot(d, &cfg);
    th_mounts mounts;
    memset(&mounts, 0, sizeof mounts);
    if (th_mounts_load(&mounts, NULL) != 0)
        th_log(TH_LOG_WARN, "cannot read the mount table; mount points are not detected");

    th_scan_opts o;
    memset(&o, 0, sizeof o);
    o.cfg = &cfg;
    o.mounts = &mounts;
    o.cancel = &d->cancel_scan;
    o.progress = on_progress;
    o.ud = d;
    o.progress_ms = 250;

    th_scan_stats st;
    th_strbuf err;
    th_sb_init(&err);
    int64_t t0 = th_mono_ms();
    int rc = th_scan_root(d->wdb, root_id, &o, &st, &err);
    int64_t ms = th_mono_ms() - t0;
    switch (rc) {
    case TH_SCAN_OK:
        th_log(TH_LOG_INFO,
               "root %lld scanned in %lld ms: %lld entries, %lld added, %lld updated, %lld removed, %lld errors",
               (long long)root_id, (long long)ms, (long long)st.entries, (long long)st.added,
               (long long)st.updated, (long long)st.removed, (long long)st.errors);
        break;
    case TH_SCAN_CANCELLED:
        th_log(TH_LOG_INFO, "scan of root %lld cancelled", (long long)root_id);
        break;
    case TH_SCAN_OFFLINE:
        th_log(TH_LOG_WARN, "root %lld is offline: %s", (long long)root_id, err.data ? err.data : "");
        break;
    default:
        th_log(TH_LOG_ERROR, "scan of root %lld failed: %s", (long long)root_id, err.data ? err.data : "");
        th_db_root_set_state(d->wdb, root_id, TH_STATE_ERROR, err.data ? err.data : "scan failed");
        break;
    }
    th_sb_free(&err);
    th_mounts_free(&mounts);
    th_config_free(&cfg);
}

static void run_rebuild(th_daemon *d, int64_t root_id)
{
    th_log(TH_LOG_INFO, "rebuilding root %lld", (long long)root_id);
    sqlite3_stmt *st = NULL;
    bool ok = th_db_begin(d->wdb) == 0;
    if (ok) {
        st = th_db_prepare(d->wdb, "DELETE FROM entries WHERE root_id = ?");
        ok = st != NULL;
    }
    if (ok) {
        sqlite3_bind_int64(st, 1, root_id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        st = th_db_prepare(d->wdb, "UPDATE roots SET entry_id = 0, state = 3, last_verified = 0 WHERE id = ?");
        ok = ok && st != NULL;
    }
    if (ok) {
        sqlite3_bind_int64(st, 1, root_id);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    if (ok && th_db_commit(d->wdb) == 0) {
        run_scan(d, root_id);
    } else {
        th_db_rollback(d->wdb);
        th_log(TH_LOG_ERROR, "cannot clear root %lld: %s", (long long)root_id, sqlite3_errmsg(d->wdb));
    }
}

static void *scanner_main(void *arg)
{
    th_daemon *d = arg;
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (d->nqueue == 0 && !atomic_load(&d->stopping)) {
            int hours = d->cfg.reconcile_interval_hours;
            if (hours <= 0) {
                pthread_cond_wait(&d->cv, &d->mu);
                continue;
            }
            int64_t due = d->last_reconcile + (int64_t)hours * 3600;
            int64_t now = th_now();
            if (now >= due) {
                /* periodic safety net for changes the incremental path missed */
                d->last_reconcile = now;
                pthread_mutex_unlock(&d->mu);
                th_log(TH_LOG_INFO, "periodic reconcile");
                daemon_enqueue_all(d, d->wdb);
                pthread_mutex_lock(&d->mu);
                continue;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(due - now);
            pthread_cond_timedwait(&d->cv, &d->mu, &ts);
        }
        if (atomic_load(&d->stopping))
            break;
        d->current = d->queue[0];
        memmove(d->queue, d->queue + 1, (d->nqueue - 1) * sizeof *d->queue);
        d->nqueue--;
        d->busy = true;
        d->current_started = th_now();
        memset(&d->current_stats, 0, sizeof d->current_stats);
        free(d->current_path);
        d->current_path = NULL;
        atomic_store(&d->cancel_scan, false);
        job j = d->current;
        pthread_mutex_unlock(&d->mu);

        switch (j.kind) {
        case JOB_SYNC:
            daemon_sync_roots(d, d->wdb, true);
            break;
        case JOB_SCAN:
            run_scan(d, j.root_id);
            break;
        case JOB_REBUILD:
            run_rebuild(d, j.root_id);
            break;
        }

        pthread_mutex_lock(&d->mu);
        d->busy = false;
        free(d->current_path);
        d->current_path = NULL;
        if (j.seq > d->completed_seq)
            d->completed_seq = j.seq;
        /* jobs dropped from the queue (cancelled roots) leave gaps; once the
         * queue is empty everything issued so far is complete */
        if (d->nqueue == 0)
            d->completed_seq = d->next_seq;
        pthread_cond_broadcast(&d->cv);
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

int scanner_start(th_daemon *d, pthread_t *th)
{
    int rc = pthread_create(th, NULL, scanner_main, d);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}
