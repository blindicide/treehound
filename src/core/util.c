/* SPDX-License-Identifier: MIT */
#include "treehound/util.h"
#include "treehound/strbuf.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *th_state_name(int state)
{
    switch (state) {
    case TH_STATE_INDEXED: return "indexed";
    case TH_STATE_VERIFIED: return "verified";
    case TH_STATE_UPDATING: return "updating";
    case TH_STATE_STALE: return "stale";
    case TH_STATE_OFFLINE: return "offline";
    case TH_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

const char *th_type_name(int type)
{
    switch (type) {
    case TH_TYPE_FILE: return "file";
    case TH_TYPE_DIR: return "dir";
    case TH_TYPE_SYMLINK: return "symlink";
    default: return "other";
    }
}

/* ------------------------------------------------------------------ paths */

char *th_home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && h[0] == '/')
        return th_xstrdup(h);
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir)
        return th_xstrdup(pw->pw_dir);
    return th_xstrdup("/");
}

static char *xdg(const char *env, const char *fallback_rel)
{
    const char *v = getenv(env);
    char *base;
    if (v && v[0] == '/') {
        base = th_xstrdup(v);
    } else {
        char *home = th_home_dir();
        base = th_path_join(home, fallback_rel);
        free(home);
    }
    char *r = th_path_join(base, "treehound");
    free(base);
    return r;
}

char *th_config_dir(void) { return xdg("XDG_CONFIG_HOME", ".config"); }
char *th_state_dir(void) { return xdg("XDG_STATE_HOME", ".local/state"); }

char *th_runtime_dir(void)
{
    const char *v = getenv("XDG_RUNTIME_DIR");
    if (v && v[0] == '/')
        return th_path_join(v, "treehound");
    th_strbuf sb;
    th_sb_init(&sb);
    th_sb_printf(&sb, "/tmp/treehound-%u", (unsigned)getuid());
    return th_sb_steal(&sb);
}

static char *in_dir(char *dir, const char *name)
{
    char *r = th_path_join(dir, name);
    free(dir);
    return r;
}

char *th_config_path(void) { return in_dir(th_config_dir(), "treehound.conf"); }
char *th_gui_config_path(void) { return in_dir(th_config_dir(), "gui.conf"); }
char *th_db_path(void) { return in_dir(th_state_dir(), "index.sqlite3"); }
char *th_socket_path(void) { return in_dir(th_runtime_dir(), "treehoundd.sock"); }
char *th_lock_path(void) { return in_dir(th_runtime_dir(), "treehoundd.lock"); }

char *th_path_join(const char *a, const char *b)
{
    th_strbuf sb;
    th_sb_init(&sb);
    th_sb_puts(&sb, a);
    if (sb.len == 0 || sb.data[sb.len - 1] != '/')
        th_sb_putc(&sb, '/');
    while (*b == '/')
        b++;
    th_sb_puts(&sb, b);
    return th_sb_steal(&sb);
}

char *th_path_normalize(const char *path)
{
    size_t len = strlen(path);
    char *out = th_xmalloc(len + 2);
    size_t o = 0;
    out[o++] = '/';
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - s);
        if (n == 1 && s[0] == '.')
            continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (o > 1) {
                o--; /* drop trailing slash */
                while (o > 1 && out[o - 1] != '/')
                    o--;
            }
            continue;
        }
        memcpy(out + o, s, n);
        o += n;
        out[o++] = '/';
    }
    if (o > 1)
        o--;
    out[o] = '\0';
    return out;
}

bool th_path_is_within(const char *child, const char *parent)
{
    size_t pl = strlen(parent);
    if (pl == 1 && parent[0] == '/')
        return child[0] == '/';
    if (strncmp(child, parent, pl) != 0)
        return false;
    return child[pl] == '\0' || child[pl] == '/';
}

int th_mkdir_p(const char *path, mode_t mode)
{
    char *tmp = th_xstrdup(path);
    size_t len = strlen(tmp);
    int rc = 0;
    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        char saved = tmp[i];
        tmp[i] = '\0';
        if (mkdir(tmp, i == len ? mode : 0700) != 0 && errno != EEXIST) {
            rc = -1;
            tmp[i] = saved;
            break;
        }
        tmp[i] = saved;
    }
    free(tmp);
    return rc;
}

int th_ensure_private_dir(const char *path)
{
    if (th_mkdir_p(path, 0700) != 0)
        return -1;
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        errno = EPERM;
        return -1;
    }
    if ((st.st_mode & 0077) != 0 && chmod(path, 0700) != 0)
        return -1;
    return 0;
}

int th_write_file_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    th_strbuf tmp;
    th_sb_init(&tmp);
    th_sb_printf(&tmp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp.data, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        th_sb_free(&tmp);
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp.data);
            th_sb_free(&tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp.data, path) != 0) {
        int e = errno;
        unlink(tmp.data);
        th_sb_free(&tmp);
        errno = e;
        return -1;
    }
    th_sb_free(&tmp);
    return 0;
}

char *th_read_file(const char *path, size_t *len, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    th_strbuf sb;
    th_sb_init(&sb);
    char buf[8192];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            th_sb_free(&sb);
            errno = e;
            return NULL;
        }
        if (r == 0)
            break;
        th_sb_append(&sb, buf, (size_t)r);
        if (sb.len > max) {
            close(fd);
            th_sb_free(&sb);
            errno = EFBIG;
            return NULL;
        }
    }
    close(fd);
    if (len)
        *len = sb.len;
    return th_sb_steal(&sb);
}

/* ------------------------------------------------------------------- time */

int64_t th_now(void)
{
    return (int64_t)time(NULL);
}

int64_t th_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

double th_mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- logging */

static const char *log_prog = "treehound";
static int log_level = TH_LOG_INFO;

void th_log_init(const char *prog)
{
    log_prog = prog;
    const char *v = getenv("TREEHOUND_LOG");
    if (v) {
        if (strcmp(v, "debug") == 0)
            log_level = TH_LOG_DEBUG;
        else if (strcmp(v, "info") == 0)
            log_level = TH_LOG_INFO;
        else if (strcmp(v, "warn") == 0)
            log_level = TH_LOG_WARN;
        else if (strcmp(v, "error") == 0)
            log_level = TH_LOG_ERROR;
    }
}

void th_log(int level, const char *fmt, ...)
{
    static const char *names[] = {"error", "warning", "info", "debug"};
    if (level > log_level)
        return;
    th_strbuf sb;
    th_sb_init(&sb);
    th_sb_printf(&sb, "%s: %s: ", log_prog, names[level < 0 ? 0 : level > 3 ? 3 : level]);
    va_list ap;
    va_start(ap, fmt);
    th_sb_vprintf(&sb, fmt, ap);
    va_end(ap);
    th_sb_putc(&sb, '\n');
    fputs(sb.data, stderr);
    th_sb_free(&sb);
}

/* ------------------------------------------------------------- formatting */

const char *th_fmt_size(int64_t bytes, char buf[32])
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = (double)bytes;
    int u = 0;
    while ((v >= 1024.0 || v <= -1024.0) && u < 5) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, 32, "%lld B", (long long)bytes);
    else
        snprintf(buf, 32, "%.1f %s", v, units[u]);
    return buf;
}

const char *th_fmt_time(int64_t t, char buf[32])
{
    time_t tt = (time_t)t;
    struct tm tm;
    if (t <= 0 || !localtime_r(&tt, &tm)) {
        snprintf(buf, 32, "-");
        return buf;
    }
    strftime(buf, 32, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool th_parse_size(const char *s, int64_t *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno != 0 || v < 0)
        return false;
    double mult = 1;
    switch (tolower((unsigned char)*end)) {
    case '\0': break;
    case 'b': break;
    case 'k': mult = 1024.0; break;
    case 'm': mult = 1024.0 * 1024; break;
    case 'g': mult = 1024.0 * 1024 * 1024; break;
    case 't': mult = 1024.0 * 1024 * 1024 * 1024; break;
    default: return false;
    }
    if (*end) {
        end++;
        /* allow "KiB", "KB", "K" */
        if (tolower((unsigned char)*end) == 'i')
            end++;
        if (tolower((unsigned char)*end) == 'b')
            end++;
        if (*end)
            return false;
    }
    double r = v * mult;
    if (r > 9.0e18)
        return false;
    *out = (int64_t)r;
    return true;
}

bool th_parse_time(const char *s, int64_t now, int64_t *out)
{
    size_t len = strlen(s);
    if (len >= 2 && isdigit((unsigned char)s[0])) {
        char unit = s[len - 1];
        long mult = 0;
        if (unit == 'd')
            mult = 86400;
        else if (unit == 'h')
            mult = 3600;
        else if (unit == 'm')
            mult = 60;
        else if (unit == 'w')
            mult = 7 * 86400;
        if (mult) {
            char *end;
            long n = strtol(s, &end, 10);
            if (end == s + len - 1 && n >= 0) {
                *out = now - (int64_t)n * mult;
                return true;
            }
        }
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h = 0, mi = 0, sec = 0;
    int n = sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
    if (n < 3)
        n = sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
    if (n != 3 && n != 5 && n != 6)
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60)
        return false;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
        return false;
    *out = (int64_t)t;
    return true;
}
