/* SPDX-License-Identifier: MIT */
/* Adapter preserves the handoff scanner source byte-for-byte. All scanner
 * writes, including event jobs, still happen on its sole writer thread. */
#include "monitor.h"
#include "treehound/db.h"
static int monitored_root_delete(sqlite3 *db,int64_t root)
{
    int rc=th_db_root_delete(db,root);
    if(rc==0)monitor_forget(root);
    return rc;
}
#define th_db_root_delete monitored_root_delete
#define th_scan_root monitor_scan
#include "scanner.c"
