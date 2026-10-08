/* SPDX-License-Identifier: MIT */
#include "daemon.h"
#include "monitor.h"
#include "treehound/common.h"
#include "treehound/db.h"
#include "treehound/ipc.h"
#include "treehound/util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { th_daemon *d; int fd; } connection;
static void *serve(void *arg)
{
    connection *c = arg;
    th_daemon *d = c->d;
    int fd = c->fd;
    free(c);
    sqlite3 *db = NULL;
    char *req = NULL;
    size_t len = 0;
    th_strbuf out;
    th_sb_init(&out);
    if (th_ipc_recv(fd, TH_IPC_MAX_REQUEST, 5000, &req, &len) == 0) {
        daemon_handle(d, &db, req, len, &out);
        if (out.len <= TH_IPC_MAX_RESPONSE)
            th_ipc_send(fd, out.data, out.len);
    }
    free(req);
    th_sb_free(&out);
    th_db_close(db);
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < d->nconn; i++) {
        if (d->conn_fds[i] == fd) {
            d->conn_fds[i] = d->conn_fds[--d->nconn];
            break;
        }
    }
    close(fd);
    pthread_cond_broadcast(&d->conn_cv);
    pthread_mutex_unlock(&d->mu);
    return NULL;
}
static int private_parent(const char *path)
{
    char *p = th_xstrdup(path);
    char *s = strrchr(p, '/');
    if (!s || s == p) { free(p); return -1; }
    *s = 0;
    int rc = th_ensure_private_dir(p);
    free(p);
    return rc;
}
static int early_exit(th_daemon *d, th_strbuf *err, int sigfd, int lockfd, int code)
{
    if (sigfd >= 0) close(sigfd);
    if (lockfd >= 0) close(lockfd);
    th_db_close(d->wdb);
    th_config_free(&d->cfg);
    free(d->config_path); free(d->db_path); free(d->socket_path);
    free(d->queue);
    if (err) th_sb_free(err);
    return code;
}
int main(int argc, char **argv)
{
    th_daemon d = {0};
    d.config_path = th_config_path();
    d.db_path = th_db_path();
    d.socket_path = th_socket_path();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { puts("treehoundd " TH_VERSION); return early_exit(&d, NULL, -1, -1, 0); }
        if (!strcmp(argv[i], "--help")) {
            puts("treehoundd [--config FILE] [--database FILE] [--socket FILE]\nUnprivileged index daemon; does not enable a persistent service.");
            return early_exit(&d, NULL, -1, -1, 0);
        }
        char **dst = !strcmp(argv[i], "--config") ? &d.config_path :
                     !strcmp(argv[i], "--database") ? &d.db_path :
                     !strcmp(argv[i], "--socket") ? &d.socket_path : NULL;
        if (!dst || i + 1 >= argc) { fprintf(stderr, "invalid option: %s\n", argv[i]); return early_exit(&d, NULL, -1, -1, TH_EXIT_USAGE); }
        free(*dst); *dst = th_xstrdup(argv[++i]);
    }
    umask(0077);
    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    th_log_init("treehoundd");
    th_strbuf err; th_sb_init(&err);
    pthread_mutex_init(&d.mu, NULL);
    pthread_cond_init(&d.cv, NULL); pthread_cond_init(&d.conn_cv, NULL);
    atomic_init(&d.stopping, false); atomic_init(&d.cancel_scan, false);
    th_config_defaults(&d.cfg);
    if (sigfd < 0 || private_parent(d.db_path) != 0 || private_parent(d.socket_path) != 0 ||
        th_config_load(&d.cfg, d.config_path, &err) != 0) {
        fprintf(stderr, "initialization failed: %s (%s)\n", err.data ? err.data : "", strerror(errno)); return early_exit(&d, &err, sigfd, -1, TH_EXIT_FAILED);
    }
    th_strbuf lockpath; th_sb_init(&lockpath); th_sb_printf(&lockpath, "%s.lock", d.db_path);
    int lockfd = open(lockpath.data, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    th_sb_free(&lockpath);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) { fputs("database is already locked\n", stderr); return early_exit(&d, &err, sigfd, lockfd, TH_EXIT_UNAVAILABLE); }
    d.wdb = th_db_open(d.db_path, false, &err);
    if (!d.wdb) { fprintf(stderr, "%s\n", err.data); return early_exit(&d, &err, sigfd, lockfd, TH_EXIT_FAILED); }
    /* Cached roots cannot be called verified before this session reconciles. */
    if (th_db_exec(d.wdb, "UPDATE roots SET state=CASE WHEN scan_in_progress THEN 3 ELSE 0 END") != 0 ||
        daemon_sync_roots(&d, d.wdb, false) != 0) return early_exit(&d, &err, sigfd, lockfd, TH_EXIT_FAILED);
    int listener = th_ipc_listen(d.socket_path, &err);
    if (listener < 0) { fprintf(stderr, "%s\n", err.data); return early_exit(&d, &err, sigfd, lockfd, TH_EXIT_UNAVAILABLE); }
    d.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    d.started_at = d.last_reconcile = th_now();
    if (monitor_start(&d) != 0) th_log(TH_LOG_WARN, "live monitoring unavailable");
    daemon_enqueue_all(&d, d.wdb);
    pthread_t scanner;
    if (d.wake_fd < 0 || scanner_start(&d, &scanner) != 0) return TH_EXIT_FAILED;
    struct pollfd fds[3] = {{listener, POLLIN, 0}, {sigfd, POLLIN, 0}, {d.wake_fd, POLLIN, 0}};
    while (!atomic_load(&d.stopping)) {
        int rc = poll(fds, 3, -1);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents || fds[2].revents) break;
        if (!(fds[0].revents & POLLIN)) continue;
        int fd = th_ipc_accept(listener);
        if (fd < 0) continue;
        pthread_mutex_lock(&d.mu);
        if (d.nconn >= MAX_CONNECTIONS) { close(fd); pthread_mutex_unlock(&d.mu); continue; }
        connection *c = th_xmalloc(sizeof *c); *c = (connection){&d, fd};
        d.conn_fds[d.nconn++] = fd;
        pthread_t worker;
        int result = pthread_create(&worker, NULL, serve, c);
        if (result == 0) pthread_detach(worker);
        else { d.nconn--; close(fd); free(c); }
        pthread_mutex_unlock(&d.mu);
    }
    atomic_store(&d.stopping, true); atomic_store(&d.cancel_scan, true);
    pthread_mutex_lock(&d.mu);
    pthread_cond_broadcast(&d.cv);
    for (int i = 0; i < d.nconn; i++) shutdown(d.conn_fds[i], SHUT_RDWR);
    while (d.nconn) pthread_cond_wait(&d.conn_cv, &d.mu);
    pthread_mutex_unlock(&d.mu);
    pthread_join(scanner, NULL);
    monitor_stop();
    close(listener); unlink(d.socket_path); close(sigfd); close(d.wake_fd);
    th_db_close(d.wdb); close(lockfd);
    th_config_free(&d.cfg); free(d.queue); free(d.current_path);
    free(d.config_path); free(d.db_path); free(d.socket_path); th_sb_free(&err);
    pthread_cond_destroy(&d.cv); pthread_cond_destroy(&d.conn_cv); pthread_mutex_destroy(&d.mu);
    return 0;
}
