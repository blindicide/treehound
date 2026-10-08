/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/common.h"
#include "treehound/ipc.h"
#include "treehound/util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/th-test-ipc-XXXXXX";
static char sockpath[256];

static void test_framing(void)
{
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(th_ipc_send(sv[0], "{\"a\":1}", 7) == 0);
    CHECK(th_ipc_send(sv[0], "", 0) == 0);
    char *m;
    size_t n;
    CHECK(th_ipc_recv(sv[1], 100, 1000, &m, &n) == 0);
    CHECK(n == 7 && strcmp(m, "{\"a\":1}") == 0);
    free(m);
    CHECK(th_ipc_recv(sv[1], 100, 1000, &m, &n) == 0);
    CHECK(n == 0 && m && m[0] == '\0');
    free(m);

    /* too large for the receiver's limit */
    CHECK(th_ipc_send(sv[0], "0123456789", 10) == 0);
    errno = 0;
    CHECK(th_ipc_recv(sv[1], 9, 1000, &m, &n) == -1);
    CHECK_INT(errno, EMSGSIZE);
    CHECK(m == NULL);
    close(sv[0]);
    close(sv[1]);

    /* timeout with nothing to read */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int64_t t0 = th_mono_ms();
    errno = 0;
    CHECK(th_ipc_recv(sv[1], 100, 50, &m, &n) == -1);
    CHECK_INT(errno, ETIMEDOUT);
    CHECK(th_mono_ms() - t0 >= 40);

    /* truncated frame: header promises 10 bytes, peer sends 3 then closes */
    unsigned char hdr[4] = {0, 0, 0, 10};
    CHECK(write(sv[0], hdr, 4) == 4);
    CHECK(write(sv[0], "abc", 3) == 3);
    close(sv[0]);
    errno = 0;
    CHECK(th_ipc_recv(sv[1], 100, 1000, &m, &n) == -1);
    CHECK_INT(errno, EPROTO);
    close(sv[1]);

    /* clean EOF before any byte */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[0]);
    CHECK(th_ipc_recv(sv[1], 100, 1000, &m, &n) == 1);
    close(sv[1]);
}

typedef struct { int fd; bool valid; } receiver_arg;
static void *large_receiver(void *arg)
{
    receiver_arg *r = arg;
    char *message = NULL; size_t len = 0;
    r->valid = th_ipc_recv(r->fd, TH_IPC_MAX_RESPONSE, 2000, &message, &len) == 0;
    r->valid = r->valid && len == 1024 * 1024;
    for (size_t i = 0; r->valid && i < len; i++) r->valid = message[i] == 'x';
    free(message);
    return NULL;
}
static void test_send_deadline(void)
{
    int sv[2]; REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int small = 4096;
    REQUIRE(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0);
    char *data = malloc(1024 * 1024); REQUIRE(data); memset(data, 'x', 1024 * 1024);
    int64_t started = th_mono_ms();
    CHECK(th_ipc_send_timeout(sv[0], data, 1024 * 1024, 50) == -1);
    CHECK_INT(errno, ETIMEDOUT);
    CHECK(th_mono_ms() - started >= 40 && th_mono_ms() - started < 1000);
    close(sv[0]); close(sv[1]);
    /* Partial writes still deliver one complete frame when the peer drains it. */
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small) == 0);
    receiver_arg receiver = {.fd = sv[1]}; pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, large_receiver, &receiver) == 0);
    CHECK(th_ipc_send_timeout(sv[0], data, 1024 * 1024, 2000) == 0);
    pthread_join(thread, NULL); CHECK(receiver.valid);
    close(sv[1]);
    CHECK(th_ipc_send_timeout(sv[0], "closed", 6, 50) == -1);
    CHECK_INT(errno, EPIPE);
    close(sv[0]); free(data);
}

typedef struct {
    int lfd;
    int served;
} server_arg;

/* Answers each request with {"v":1,"ok":true,"echo":<request>}. */
static void *server(void *p)
{
    server_arg *a = p;
    for (;;) {
        int fd = th_ipc_accept(a->lfd);
        if (fd < 0)
            return NULL;
        char *m;
        size_t n;
        int rc = th_ipc_recv(fd, TH_IPC_MAX_REQUEST, 2000, &m, &n);
        if (rc == 0) {
            if (strcmp(m, "quit") == 0) {
                free(m);
                close(fd);
                return NULL;
            }
            th_strbuf r;
            th_sb_init(&r);
            th_sb_puts(&r, "{\"v\":1,\"ok\":true,\"echo\":");
            th_sb_append(&r, m, n);
            th_sb_putc(&r, '}');
            th_ipc_send(fd, r.data, r.len);
            th_sb_free(&r);
            free(m);
            a->served++;
        }
        close(fd);
    }
}

static void test_socket(void)
{
    th_strbuf err;
    th_sb_init(&err);
    int lfd = th_ipc_listen(sockpath, &err);
    REQUIRE(lfd >= 0);
    struct stat st;
    REQUIRE(lstat(sockpath, &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK_INT(st.st_mode & 0777, 0600);
    char *dir = th_path_join(tmpdir, "run");
    REQUIRE(stat(dir, &st) == 0);
    CHECK_INT(st.st_mode & 0777, 0700);
    free(dir);

    /* a second daemon must not steal a live socket */
    th_sb_reset(&err);
    errno = 0;
    CHECK(th_ipc_listen(sockpath, &err) == -1);
    CHECK_INT(errno, EADDRINUSE);

    server_arg arg = {.lfd = lfd};
    pthread_t th;
    REQUIRE(pthread_create(&th, NULL, server, &arg) == 0);

    th_sb_reset(&err);
    const char *req = "{\"v\":1,\"cmd\":\"ping\",\"p\":\"a\\udcffb\"}";
    th_jval *v = th_ipc_call(sockpath, req, strlen(req), 2000, &err);
    REQUIRE(v);
    CHECK(th_json_get_bool(v, "ok", false));
    const th_jval *echo = th_json_get(v, "echo");
    REQUIRE(echo && echo->type == TH_JOBJ);
    size_t pl = 0;
    const char *p = th_json_get_bytes(echo, "p", &pl);
    CHECK(p && pl == 3 && memcmp(p, "a\xff" "b", 3) == 0);
    th_json_free(v);

    /* oversize requests are refused by the server */
    size_t big = TH_IPC_MAX_REQUEST + 1;
    char *buf = malloc(big);
    REQUIRE(buf);
    memset(buf, ' ', big);
    th_sb_reset(&err);
    CHECK(th_ipc_call(sockpath, buf, big, 2000, &err) == NULL);
    free(buf);

    int qfd = th_ipc_connect(sockpath, NULL);
    REQUIRE(qfd >= 0);
    th_ipc_send(qfd, "quit", 4);
    pthread_join(th, NULL);
    close(qfd);
    CHECK_INT(arg.served, 1);

    /* a dead daemon's socket file is replaced */
    close(lfd);
    th_sb_reset(&err);
    lfd = th_ipc_listen(sockpath, &err);
    CHECK(lfd >= 0);
    close(lfd);
    unlink(sockpath);

    /* connecting to nothing reports an error */
    th_sb_reset(&err);
    CHECK(th_ipc_call(sockpath, "{}", 2, 500, &err) == NULL);
    CHECK(err.len > 0);

    /* a non-socket at the path is never removed */
    FILE *f = fopen(sockpath, "w");
    REQUIRE(f);
    fclose(f);
    th_sb_reset(&err);
    CHECK(th_ipc_listen(sockpath, &err) == -1);
    CHECK(access(sockpath, F_OK) == 0);
    unlink(sockpath);
    th_sb_free(&err);
}

static void test_saturated_listener(void)
{
    th_strbuf err; th_sb_init(&err);
    int lfd = th_ipc_listen(sockpath, &err);
    REQUIRE(lfd >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    REQUIRE(strlen(sockpath) < sizeof addr.sun_path);
    strcpy(addr.sun_path, sockpath);
    int peers[128]; size_t n = 0;
    while (n < 128) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        REQUIRE(fd >= 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
            CHECK_INT(errno, EAGAIN); close(fd); break;
        }
        peers[n++] = fd;
    }
    REQUIRE(n < 128 && n > 0);
    struct stat before; REQUIRE(lstat(sockpath, &before) == 0);
    /* A watchdog bounds the regression on the old blocking-connect code. */
    pid_t child = fork(); REQUIRE(child >= 0);
    if (child == 0) {
        int probe = th_ipc_listen(sockpath, &err);
        struct stat after;
        bool intact = probe == -1 && errno == EADDRINUSE &&
            lstat(sockpath, &after) == 0 && before.st_ino == after.st_ino;
        _exit(intact ? 0 : 1);
    }
    int status = 0; pid_t done = 0;
    int64_t deadline = th_mono_ms() + 1000;
    while (!(done = waitpid(child, &status, WNOHANG)) && th_mono_ms() < deadline)
        usleep(1000);
    if (!done) { kill(child, SIGKILL); REQUIRE(waitpid(child, &status, 0) == child); }
    CHECK(done == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    th_sb_reset(&err);
    int64_t start = th_mono_ms();
    /* Nonblocking probes also make clients fail promptly on a full backlog. */
    if (done == child) {
        CHECK(th_ipc_call(sockpath, "{}", 2, 50, &err) == NULL);
        CHECK(th_mono_ms() - start < 500);
    }
    for (size_t i = 0; i < n; i++) close(peers[i]);
    close(lfd); unlink(sockpath); th_sb_free(&err);
}

int main(void)
{
    REQUIRE(mkdtemp(tmpdir));
    snprintf(sockpath, sizeof sockpath, "%s/run/treehoundd.sock", tmpdir);
    RUN(test_framing);
    RUN(test_send_deadline);
    RUN(test_socket);
    RUN(test_saturated_listener);
    char *dir = th_path_join(tmpdir, "run");
    rmdir(dir);
    free(dir);
    rmdir(tmpdir);
    return TEST_EXIT();
}
