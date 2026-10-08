/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_UTIL_H
#define TREEHOUND_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "treehound/common.h"

/* ---- XDG locations (all returned strings are heap-allocated) ---- */
char *th_home_dir(void);
char *th_config_dir(void);  /* $XDG_CONFIG_HOME/treehound */
char *th_state_dir(void);   /* $XDG_STATE_HOME/treehound */
char *th_runtime_dir(void); /* $XDG_RUNTIME_DIR/treehound (fallback /tmp/treehound-UID) */
char *th_config_path(void); /* <config>/treehound.conf */
char *th_gui_config_path(void);
char *th_db_path(void);     /* <state>/index.sqlite3 */
char *th_socket_path(void); /* <runtime>/treehoundd.sock */
char *th_lock_path(void);   /* <runtime>/treehoundd.lock */

/* mkdir -p; the last component gets mode, intermediates 0700. */
int th_mkdir_p(const char *path, mode_t mode);
/* Ensures a private (0700, owned by us) directory exists. */
int th_ensure_private_dir(const char *path);

char *th_path_join(const char *a, const char *b);
/* Lexically normalises an absolute path (removes //, /./, /../, trailing /). */
char *th_path_normalize(const char *path);
/* True when child equals parent or lies strictly below it. */
bool th_path_is_within(const char *child, const char *parent);

/* Writes data to path atomically (tmp file + rename). */
int th_write_file_atomic(const char *path, const char *data, size_t len, mode_t mode);
/* Reads a whole file; returns NULL on error (errno set). */
char *th_read_file(const char *path, size_t *len, size_t max);

/* ---- time ---- */
int64_t th_now(void);     /* wall clock, seconds */
int64_t th_mono_ms(void); /* monotonic, milliseconds */
double th_mono_sec(void);

/* ---- logging (stderr) ---- */
enum { TH_LOG_ERROR = 0, TH_LOG_WARN = 1, TH_LOG_INFO = 2, TH_LOG_DEBUG = 3 };
void th_log_init(const char *prog);
void th_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- formatting ---- */
/* Human readable size into buf, e.g. "1.5 GiB". */
const char *th_fmt_size(int64_t bytes, char buf[32]);
/* Local time "YYYY-MM-DD HH:MM" into buf. */
const char *th_fmt_time(int64_t t, char buf[32]);
/* Parses "10", "10k", "1.5M", "2G", "3T" (binary units). */
bool th_parse_size(const char *s, int64_t *out);
/* Parses "YYYY-MM-DD", "YYYY-MM-DD HH:MM[:SS]", or relative "7d"/"12h"/"30m". */
bool th_parse_time(const char *s, int64_t now, int64_t *out);

#endif
