/* SPDX-License-Identifier: MIT */
#include "th_test.h"

#include "treehound/common.h"
#include "treehound/ipc.h"
#include "treehound/util.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

int main(void)
{
    REQUIRE(mkdtemp(tmpdir));
    snprintf(sockpath, sizeof sockpath, "%s/run/treehoundd.sock", tmpdir);
    RUN(test_framing);
    RUN(test_socket);
    char *dir = th_path_join(tmpdir, "run");
    rmdir(dir);
    free(dir);
    rmdir(tmpdir);
    return TEST_EXIT();
}
