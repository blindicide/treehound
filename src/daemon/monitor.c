/* SPDX-License-Identifier: MIT */
#include "monitor.h"
#include "treehound/common.h"
#include "treehound/db.h"
#include "treehound/history.h"
#include "treehound/util.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define MAX_DIRTY 4096
#define WATCH_MASK (IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_ONLYDIR | IN_DONT_FOLLOW)
typedef struct { int wd; int64_t root; char *path; } watch;
typedef struct { int64_t root; char *path; } dirty;
typedef struct { int64_t root; uint64_t epoch; bool snapshot; } coverage;
typedef struct { uint32_t cookie; int64_t root; char *from, *to; } move;
static struct {
    pthread_mutex_t mu;
    th_daemon *d;
    int fd, wake;
    pthread_t thread;
    bool running, failed, full;
    uint64_t epoch;
    coverage *covered; size_t ncovered;
    watch *watches;
    size_t nwatches, capacity;
    dirty paths[MAX_DIRTY];
    size_t npaths;
    move moves[256]; size_t nmoves;
    atomic_bool stop;
} m = {.mu = PTHREAD_MUTEX_INITIALIZER, .fd = -1};

bool monitor_enabled(void) { return m.running; }
bool monitor_pending(int64_t root)
{
    pthread_mutex_lock(&m.mu);
    bool result = m.full || m.failed;
    if (root) {
        bool found = false;
        for (size_t i = 0; i < m.ncovered; i++) if (m.covered[i].root == root) { found = true; result |= m.covered[i].epoch != m.epoch; }
        result |= !found;
    }
    for (size_t i = 0; i < m.npaths && !result; i++) result = !root || m.paths[i].root == root;
    pthread_mutex_unlock(&m.mu);
    return result;
}
void monitor_force(int64_t root)
{
    (void)root;
    pthread_mutex_lock(&m.mu); m.epoch++; pthread_mutex_unlock(&m.mu);
}
void monitor_snapshot(int64_t root)
{
    pthread_mutex_lock(&m.mu);
    size_t i;
    for (i=0; i<m.ncovered; i++) if (m.covered[i].root==root) break;
    if (i==m.ncovered) {
        m.covered=th_xrealloc(m.covered,(m.ncovered+1)*sizeof *m.covered);
        m.covered[m.ncovered++]=(coverage){.root=root,.epoch=UINT64_MAX};
    }
    m.covered[i].snapshot=true; m.epoch++;
    pthread_mutex_unlock(&m.mu);
}
static void queue_path(int64_t root, const char *path)
{
    for (size_t i = 0; i < m.npaths; i++)
        if (m.paths[i].root == root && !strcmp(m.paths[i].path, path)) return;
    if (m.npaths == MAX_DIRTY) { m.full = true; return; }
    m.paths[m.npaths++] = (dirty){root, th_xstrdup(path)};
}
static void add_watch(int64_t id, const char *path, size_t len, void *ud)
{
    (void)id; (void)len;
    int64_t root = *(int64_t *)ud;
    if (!m.running) return;
    pthread_mutex_lock(&m.mu);
    int wd = inotify_add_watch(m.fd, path, WATCH_MASK);
    if (wd < 0) {
        if (!m.failed) th_log(TH_LOG_WARN, "inotify coverage incomplete: %s", strerror(errno));
        m.failed = true;
    } else {
        size_t i;
        for (i = 0; i < m.nwatches; i++) if (m.watches[i].wd == wd) break;
        if (i == m.nwatches) {
            if (i == m.capacity) {
                m.capacity = m.capacity ? m.capacity * 2 : 64;
                m.watches = th_xrealloc(m.watches, m.capacity * sizeof *m.watches);
            }
            m.nwatches++;
        } else free(m.watches[i].path);
        m.watches[i] = (watch){wd, root, th_xstrdup(path)};
    }
    pthread_mutex_unlock(&m.mu);
}
static void *events(void *unused)
{
    (void)unused;
    _Alignas(struct inotify_event) char buffer[65536];
    while (!atomic_load(&m.stop)) {
        struct pollfd p[2] = {{m.fd, POLLIN, 0}, {m.wake, POLLIN, 0}};
        if (poll(p, 2, -1) <= 0) continue;
        if (p[1].revents) break;
        ssize_t n = read(m.fd, buffer, sizeof buffer);
        if (n <= 0) continue;
        int64_t roots[MAX_DIRTY]; size_t nr = 0;
        pthread_mutex_lock(&m.mu);
        for (size_t pos = 0; pos + sizeof(struct inotify_event) <= (size_t)n;) {
            struct inotify_event *e = (void *)(buffer + pos);
            pos += sizeof *e + e->len;
            if (e->mask & IN_Q_OVERFLOW) { m.full = true; continue; }
            for (size_t i = 0; i < m.nwatches; i++) {
                watch *w = &m.watches[i];
                if (w->wd != e->wd) continue;
                queue_path(w->root, w->path);
                bool seen = false;
                for (size_t j = 0; j < nr; j++) if (roots[j] == w->root) seen = true;
                if (!seen && nr < MAX_DIRTY) roots[nr++] = w->root;
                if (e->mask & IN_UNMOUNT) m.full = true;
                if (e->len && (e->mask & IN_MOVED_FROM)) {
                    if (m.nmoves == 256) {
                        free(m.moves[0].from); free(m.moves[0].to);
                        memmove(m.moves, m.moves + 1, 255 * sizeof *m.moves); m.nmoves--;
                    }
                    m.moves[m.nmoves++] = (move){e->cookie, w->root, th_path_join(w->path, e->name), NULL};
                }
                if (e->len && (e->mask & IN_MOVED_TO)) {
                    for (size_t j = 0; j < m.nmoves; j++) {
                        move *mv = &m.moves[j];
                        if (mv->cookie != e->cookie || mv->root != w->root || mv->to) continue;
                        mv->to = th_path_join(w->path, e->name);
                        for (size_t k = 0; k < m.nwatches; k++) {
                            watch *mapped = &m.watches[k];
                            if (mapped->root != mv->root || !th_path_is_within(mapped->path, mv->from)) continue;
                            char *next = th_xmalloc(strlen(mv->to) + strlen(mapped->path + strlen(mv->from)) + 1);
                            strcpy(next, mv->to); strcat(next, mapped->path + strlen(mv->from));
                            free(mapped->path); mapped->path = next;
                        }
                        break;
                    }
                }
                if (e->mask & IN_IGNORED) {
                    free(w->path); *w = m.watches[--m.nwatches];
                }
                break;
            }
        }
        bool full = m.full;
        if (full) { m.epoch++; m.full = false; }
        pthread_mutex_unlock(&m.mu);
        /* Coalesce the burst with queue entries. No database writes here. */
        for (size_t i = 0; i < nr; i++) daemon_enqueue(m.d, JOB_SCAN, roots[i], false);
        if (full) {
            th_strbuf err; th_sb_init(&err);
            sqlite3 *db = th_db_open(m.d->db_path, true, &err);
            if (db) daemon_enqueue_all(m.d, db);
            th_db_close(db); th_sb_free(&err);
        }
    }
    return NULL;
}
int monitor_start(th_daemon *d)
{
    m.d = d;
    if (!d->cfg.watch) return 0;
    m.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m.fd < 0) { m.failed = true; return -1; }
    m.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m.wake < 0) { close(m.fd); m.fd = -1; m.failed = true; return -1; }
    atomic_init(&m.stop, false);
    m.running = true;
    if (pthread_create(&m.thread, NULL, events, NULL) != 0) { close(m.fd); close(m.wake); m.fd = -1; m.running = false; m.failed = true; return -1; }
    return 0;
}
void monitor_stop(void)
{
    if (m.running) { atomic_store(&m.stop, true); uint64_t one = 1; (void)!write(m.wake, &one, sizeof one); pthread_join(m.thread, NULL); close(m.fd); close(m.wake); m.running = false; }
    for (size_t i = 0; i < m.nwatches; i++) free(m.watches[i].path);
    for (size_t i = 0; i < m.npaths; i++) free(m.paths[i].path);
    for (size_t i = 0; i < m.nmoves; i++) { free(m.moves[i].from); free(m.moves[i].to); }
    m.nmoves = 0;
    free(m.covered); m.covered = NULL; m.ncovered = 0;
    free(m.watches); m.watches = NULL; m.nwatches = m.npaths = 0;
}
/* Preserve identity when both halves of a same-root move are observed.
 * Raw-byte paths use substr on BLOBs, avoiding Unicode character offsets. */
static int apply_move(sqlite3 *db, const move *mv)
{
    if (!mv->to) return 0;
    th_entry src = {0}, parent = {0};
    char *dir = th_xstrdup(mv->to); char *slash = strrchr(dir, '/');
    if (!slash) { free(dir); return -1; }
    *slash = 0;
    int found = th_db_entry_by_path(db, mv->from, strlen(mv->from), &src);
    int target = th_db_entry_by_path(db, *dir ? dir : "/", strlen(*dir ? dir : "/"), &parent);
    free(dir);
    if (found != 1 || target != 1 || src.root_id != mv->root || parent.root_id != mv->root) {
        th_entry_clear(&src); th_entry_clear(&parent); return 0;
    }
    th_strbuf lo, hi; th_sb_init(&lo); th_sb_init(&hi);
    th_subtree_range(mv->from, strlen(mv->from), &lo, &hi);
    int rc = -1;
    if (th_db_begin(db) == 0) {
        sqlite3_stmt *q = th_db_prepare(db,
            "UPDATE entries SET path=CAST(?1 || substr(CAST(path AS BLOB),?2) AS TEXT), "
            "name=CASE WHEN id=?3 THEN ?4 ELSE name END, "
            "parent_id=CASE WHEN id=?3 THEN ?5 ELSE parent_id END "
            "WHERE root_id=?6 AND (id=?3 OR (path>=?7 AND path<?8))");
        if (q) {
            th_bind_str(q, 1, mv->to); sqlite3_bind_int64(q, 2, (int64_t)strlen(mv->from) + 1);
            sqlite3_bind_int64(q, 3, src.id); th_bind_str(q, 4, strrchr(mv->to, '/') + 1);
            sqlite3_bind_int64(q, 5, parent.id); sqlite3_bind_int64(q, 6, mv->root);
            th_bind_str(q, 7, lo.data); th_bind_str(q, 8, hi.data);
            if (sqlite3_step(q) == SQLITE_DONE) rc = th_db_commit(db);
            sqlite3_finalize(q);
        }
        if (rc != 0) th_db_rollback(db);
        else {
            th_index_refresh_dir(db, src.parent_id); th_index_refresh_ancestors(db, src.parent_id);
            th_index_refresh_dir(db, parent.id); th_index_refresh_ancestors(db, parent.id);
        }
    }
    th_entry_clear(&src); th_entry_clear(&parent); th_sb_free(&lo); th_sb_free(&hi);
    return rc;
}
/* Progress uses the preserved scanner's userdata; watch callbacks need the root. */
typedef struct { const th_scan_opts *original; int64_t root; } scan_data;
static void watch_dir(int64_t id, const char *path, size_t len, void *ud)
{
    scan_data *data = ud; add_watch(id, path, len, &data->root);
    if (data->original->on_dir) data->original->on_dir(id, path, len, data->original->ud);
}
static void progress(const th_scan_stats *st, const char *path, void *ud)
{
    scan_data *data = ud;
    if (data->original->progress) data->original->progress(st, path, data->original->ud);
}
int monitor_scan(sqlite3 *db, int64_t root, const th_scan_opts *opts, th_scan_stats *stats, th_strbuf *err)
{
    dirty work[MAX_DIRTY]; size_t nw = 0;
    move moves[256]; size_t nm = 0;
    pthread_mutex_lock(&m.mu);
    size_t scope;
    for (scope = 0; scope < m.ncovered; scope++) if (m.covered[scope].root == root) break;
    if (scope == m.ncovered) {
        m.covered = th_xrealloc(m.covered, (m.ncovered + 1) * sizeof *m.covered);
        m.covered[m.ncovered++] = (coverage){.root=root, .epoch=UINT64_MAX};
    }
    bool full = m.covered[scope].epoch != m.epoch;
    m.covered[scope].epoch = m.epoch;
    size_t keep = 0;
    for (size_t i = 0; i < m.npaths; i++) {
        if (m.paths[i].root == root) work[nw++] = m.paths[i];
        else m.paths[keep++] = m.paths[i];
    }
    m.npaths = keep;
    keep = 0;
    for (size_t i = 0; i < m.nmoves; i++) {
        if (m.moves[i].root == root) moves[nm++] = m.moves[i];
        else m.moves[keep++] = m.moves[i];
    }
    m.nmoves = keep;
    pthread_mutex_unlock(&m.mu);
    for (size_t i = 0; i < nm; i++) {
        if (apply_move(db, &moves[i]) != 0) full = true;
        free(moves[i].from); free(moves[i].to);
    }
    scan_data data = {opts, root};
    th_scan_opts o = *opts;
    o.on_dir = watch_dir; o.progress = progress; o.ud = &data;
    int rc = TH_SCAN_OK;
    memset(stats, 0, sizeof *stats);
    if (full || nw == 0) rc = th_scan_root(db, root, &o, stats, err);
    else {
        th_db_root_set_state(db, root, TH_STATE_UPDATING, NULL);
        o.shallow = true;
        for (size_t i = 0; i < nw && rc == TH_SCAN_OK; i++) {
            th_scan_stats st;
            rc = th_scan_subtree(db, root, work[i].path, strlen(work[i].path), &o, &st, err);
            stats->dirs += st.dirs; stats->entries += st.entries; stats->added += st.added;
            stats->updated += st.updated; stats->removed += st.removed; stats->errors += st.errors;
        }
        th_db_root_set_state(db, root, rc == TH_SCAN_OK ? TH_STATE_VERIFIED : TH_STATE_STALE, NULL);
    }
    for (size_t i = 0; i < nw; i++) free(work[i].path);
    pthread_mutex_lock(&m.mu); bool failed = m.failed; pthread_mutex_unlock(&m.mu);
    if (rc == TH_SCAN_OK && (failed || stats->errors))
        th_db_root_set_state(db, root, TH_STATE_STALE, failed ? "inotify coverage incomplete" : "enumeration incomplete");
    if (rc == TH_SCAN_OK && !failed && !stats->errors && !monitor_pending(root)) {
        pthread_mutex_lock(&m.mu); bool capture = m.covered[scope].snapshot; m.covered[scope].snapshot = false; pthread_mutex_unlock(&m.mu);
        if (th_history_capture(db,root,opts->cfg->snapshot_retention,th_now(),capture) != 0) {
            th_log(TH_LOG_WARN,"snapshot failed for root %lld",(long long)root);
            pthread_mutex_lock(&m.mu); m.covered[scope].snapshot |= capture; pthread_mutex_unlock(&m.mu);
        }
    }
    return rc;
}
