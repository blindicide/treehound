/* SPDX-License-Identifier: MIT */
#ifndef TH_HISTORY_H
#define TH_HISTORY_H
#include "treehound/db.h"
#include "treehound/json.h"
/* Writer-only, atomic snapshot plus retention. Automatic captures are daily. */
int th_history_capture(sqlite3 *db, int64_t root, int retention, int64_t now, bool force);
/* Reader-only, bounded series and largest changes between the last two captures. */
int th_history_reply(sqlite3 *db, int64_t root, th_jw *out);
#endif
