/* SPDX-License-Identifier: MIT */
/* Adapter preserves the handoff scanner source byte-for-byte. All scanner
 * writes, including event jobs, still happen on its sole writer thread. */
#include "monitor.h"
#define th_scan_root monitor_scan
#include "scanner.c"
