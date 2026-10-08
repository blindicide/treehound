/* SPDX-License-Identifier: MIT */
/* Linker-only fault injection in the test daemon; production has no toggle. */
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int __real_inotify_add_watch(int fd, const char *path, uint32_t mask);
int __wrap_inotify_add_watch(int fd, const char *path, uint32_t mask);
int __wrap_inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    static atomic_bool failed;
    const char *target = getenv("TREEHOUND_TEST_WATCH_FAIL");
    if (target && !strcmp(target, path) && !atomic_exchange(&failed, 1)) {
        errno = ENOSPC;
        return -1;
    }
    return __real_inotify_add_watch(fd, path, mask);
}
