/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_MOUNTS_H
#define TREEHOUND_MOUNTS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Snapshot of /proc/self/mountinfo.  The scanner uses it to recognise mount
 * points that st_dev alone misses (bind mounts), to skip pseudo filesystems
 * and to hide the encrypted lower directory of eCryptfs mounts so only the
 * decrypted view is indexed.
 */
typedef struct {
    char *mnt;    /* mount point (unescaped) */
    char *fstype; /* e.g. ext4, ecryptfs, fuse.sshfs */
    char *source; /* mount source (device, or lower dir for ecryptfs) */
    char *root;   /* root of the mount within its filesystem */
} th_mount;

typedef struct {
    th_mount *v;
    size_t n;
} th_mounts;

/* Parses a mountinfo file (NULL for /proc/self/mountinfo).  Returns 0 on
 * success; on failure m is left empty and -1 returned. */
int th_mounts_load(th_mounts *m, const char *path);
int th_mounts_parse(th_mounts *m, const char *text, size_t len);
void th_mounts_free(th_mounts *m);

/* The mount whose mount point is exactly path, or NULL. */
const th_mount *th_mounts_at(const th_mounts *m, const char *path);
/* The mount containing path (longest mount-point prefix), or NULL. */
const th_mount *th_mounts_find(const th_mounts *m, const char *path);
/* True for kernel/virtual filesystems that never hold user files. */
bool th_fstype_is_pseudo(const char *fstype);
/* True when path is (or lies below) the encrypted lower directory of a
 * mounted eCryptfs filesystem. */
bool th_mounts_is_hidden(const th_mounts *m, const char *path);

#endif
