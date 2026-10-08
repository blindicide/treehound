/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_COMMON_H
#define TREEHOUND_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "treehound/version.h"

#define TH_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define TH_UNUSED(x) ((void)(x))

/* IPC protocol revision; bumped on incompatible request/response changes. */
#define TH_PROTOCOL_VERSION 1

/* Hard caps for IPC framing (see docs/ARCHITECTURE.md). */
#define TH_IPC_MAX_REQUEST (64u * 1024u)
#define TH_IPC_MAX_RESPONSE (64u * 1024u * 1024u)

/* Maximum results a single search/list request may return. */
#define TH_MAX_PAGE 5000

/* Entry types as stored in the database. */
typedef enum {
    TH_TYPE_FILE = 0,
    TH_TYPE_DIR = 1,
    TH_TYPE_SYMLINK = 2,
    TH_TYPE_OTHER = 3,
} th_entry_type;

/* Per-entry flags (bitmask, stored in entries.flags). */
enum {
    TH_FLAG_DENIED = 1 << 0,     /* directory could not be read */
    TH_FLAG_MOUNTPOINT = 1 << 1, /* another filesystem is mounted here */
    TH_FLAG_SPARSE = 1 << 2,     /* allocated size < logical size */
    TH_FLAG_LINKDUP = 1 << 3,    /* hard link already counted elsewhere */
    TH_FLAG_EXCLUDED = 1 << 4,   /* reserved: excluded by configuration */
    TH_FLAG_NOWATCH = 1 << 5,    /* directory has no inotify watch */
};

/* Consistency state of a root (and, where applicable, of entries). */
typedef enum {
    TH_STATE_INDEXED = 0,  /* cached data from a previous session, not yet reconciled */
    TH_STATE_VERIFIED = 1, /* reconciled in this session (and watched when live indexing is on) */
    TH_STATE_UPDATING = 2, /* a scan or reconciliation is running */
    TH_STATE_STALE = 3,    /* known to be out of date (overflow, interruption, unclean exit) */
    TH_STATE_OFFLINE = 4,  /* root path is missing or unmounted */
    TH_STATE_ERROR = 5,    /* last scan failed */
} th_state;

const char *th_state_name(int state);
const char *th_type_name(int type);

/* Exit codes shared by the CLI and documented in README.md. */
enum {
    TH_EXIT_OK = 0,
    TH_EXIT_NO_MATCH = 1,
    TH_EXIT_USAGE = 2,
    TH_EXIT_UNAVAILABLE = 3,
    TH_EXIT_FAILED = 4,
};

#endif
