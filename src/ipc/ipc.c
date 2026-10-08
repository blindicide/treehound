/* SPDX-License-Identifier: MIT */
#include "treehound/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "treehound/common.h"
#include "treehound/util.h"

static int fill_addr(const char *path, struct sockaddr_un *sa, th_strbuf *err)
{
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    size_t len = strlen(path);
    if (len >= sizeof sa->sun_path) {
        if (err)
            th_sb_printf(err, "socket path too long: %s", path);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(sa->sun_path, path, len + 1);
    return 0;
}

static char *parent_dir(const char *path)
{
    char *d = th_xstrdup(path);
    char *slash = strrchr(d, '/');
    if (slash && slash != d)
        *slash = '\0';
    else if (slash)
        slash[1] = '\0';
    return d;
}

int th_ipc_listen(const char *path, th_strbuf *err)
{
    struct sockaddr_un sa;
    if (fill_addr(path, &sa, err) != 0)
        return -1;
    char *dir = parent_dir(path);
    int rc = th_ensure_private_dir(dir);
    int e = errno;
    if (rc != 0) {
        th_sb_printf(err, "cannot use runtime directory %s: %s", dir, strerror(e));
        free(dir);
        errno = e;
        return -1;
    }
    free(dir);

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            th_sb_printf(err, "%s exists and is not a socket", path);
            errno = EEXIST;
            return -1;
        }
        int probe = th_ipc_connect(path, NULL);
        if (probe >= 0) {
            close(probe);
            th_sb_printf(err, "another daemon is listening on %s", path);
            errno = EADDRINUSE;
            return -1;
        }
        unlink(path); /* stale */
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        th_sb_printf(err, "socket: %s", strerror(errno));
        return -1;
    }
    mode_t old = umask(0177);
    rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    e = errno;
    umask(old);
    if (rc != 0 || chmod(path, 0600) != 0 || listen(fd, 64) != 0) {
        if (rc == 0)
            e = errno;
        th_sb_printf(err, "cannot listen on %s: %s", path, strerror(e));
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int th_ipc_accept(int listen_fd)
{
    int fd;
    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    struct ucred cred;
    socklen_t cl = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 || cred.uid != geteuid()) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

int th_ipc_connect(const char *path, th_strbuf *err)
{
    struct sockaddr_un sa;
    if (fill_addr(path, &sa, err) != 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (err)
            th_sb_printf(err, "socket: %s", strerror(errno));
        return -1;
    }
    int rc;
    do {
        rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        int e = errno;
        if (err)
            th_sb_printf(err, "cannot connect to %s: %s", path, strerror(e));
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int th_ipc_send(int fd, const char *data, size_t len)
{
    if (len > TH_IPC_MAX_RESPONSE || len > UINT32_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    unsigned char hdr[4] = {(unsigned char)(len >> 24), (unsigned char)(len >> 16),
                            (unsigned char)(len >> 8), (unsigned char)len};
    if (write_all(fd, (const char *)hdr, 4) != 0)
        return -1;
    return write_all(fd, data, len);
}

/* Reads exactly n bytes.  Returns n, the short count at EOF, or -1. */
static ssize_t read_full(int fd, char *p, size_t n, int64_t deadline)
{
    size_t got = 0;
    while (got < n) {
        if (deadline > 0) {
            int64_t left = deadline - th_mono_ms();
            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int pr = poll(&pfd, 1, left > INT32_MAX ? INT32_MAX : (int)left);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (pr == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int th_ipc_recv(int fd, size_t max, int timeout_ms, char **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    int64_t deadline = timeout_ms > 0 ? th_mono_ms() + timeout_ms : 0;
    unsigned char hdr[4];
    ssize_t r = read_full(fd, (char *)hdr, 4, deadline);
    if (r < 0)
        return -1;
    if (r == 0)
        return 1;
    if (r < 4) {
        errno = EPROTO;
        return -1;
    }
    size_t n = (size_t)hdr[0] << 24 | (size_t)hdr[1] << 16 | (size_t)hdr[2] << 8 | (size_t)hdr[3];
    if (n > max) {
        errno = EMSGSIZE;
        return -1;
    }
    char *buf = th_xmalloc(n + 1);
    r = read_full(fd, buf, n, deadline);
    if (r < 0 || (size_t)r < n) {
        if (r >= 0)
            errno = EPROTO;
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return 0;
}

th_jval *th_ipc_call(const char *socket_path, const char *request, size_t len, int timeout_ms,
                     th_strbuf *err)
{
    int fd = th_ipc_connect(socket_path, err);
    if (fd < 0)
        return NULL;
    if (th_ipc_send(fd, request, len) != 0) {
        th_sb_printf(err, "cannot send request: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    char *resp;
    size_t rlen;
    int rc = th_ipc_recv(fd, TH_IPC_MAX_RESPONSE, timeout_ms, &resp, &rlen);
    int e = errno;
    close(fd);
    if (rc != 0) {
        if (rc == 1)
            th_sb_puts(err, "daemon closed the connection");
        else
            th_sb_printf(err, "no valid response from daemon: %s", strerror(e));
        return NULL;
    }
    const char *perr = NULL;
    th_jval *v = th_json_parse(resp, rlen, &perr);
    free(resp);
    if (!v || v->type != TH_JOBJ) {
        th_sb_printf(err, "malformed response from daemon: %s", perr ? perr : "not an object");
        th_json_free(v);
        return NULL;
    }
    return v;
}
