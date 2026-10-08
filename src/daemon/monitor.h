/* SPDX-License-Identifier: MIT */
#ifndef TH_MONITOR_H
#define TH_MONITOR_H
#include "daemon.h"
int monitor_start(th_daemon *d);
void monitor_stop(void);
void monitor_force(int64_t root_id);
bool monitor_enabled(void);
bool monitor_pending(int64_t root_id);
int monitor_scan(sqlite3 *db, int64_t root_id, const th_scan_opts *opts,
                 th_scan_stats *stats, th_strbuf *err);
#endif
