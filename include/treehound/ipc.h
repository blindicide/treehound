/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_IPC_H
#define TREEHOUND_IPC_H

#include <stddef.h>

#include "treehound/json.h"
#include "treehound/strbuf.h"

/*
 * Daemon IPC: a Unix stream socket in the private runtime directory.
 *
 * Each message is a 4-byte big-endian length followed by that many bytes of
 * JSON.  Requests are limited to TH_IPC_MAX_REQUEST bytes and responses to
 * TH_IPC_MAX_RESPONSE.  Every request carries "v" (TH_PROTOCOL_VERSION) and
 * "cmd"; every response carries "v" and "ok", plus "error" and "code" when ok
 * is false.  Paths travel as JSON strings using the byte-safe escaping of
 * json.h, so names that are not valid UTF-8 round-trip exactly.
 */

/* Creates the listening socket at path (owner-only, mode 0600) in a private
 * directory.  A stale socket left by a dead daemon is replaced; a live one
 * makes this fail with EADDRINUSE.  Returns the fd or -1. */
int th_ipc_listen(const char *path, th_strbuf *err);

/* Accepts one connection and verifies that the peer runs as our uid.
 * Returns the fd, or -1 (errno EPERM for a foreign peer). */
int th_ipc_accept(int listen_fd);

/* Connects to the daemon socket.  Returns the fd or -1. */
int th_ipc_connect(const char *path, th_strbuf *err);

/* Sends one framed message.  Returns 0 or -1. */
int th_ipc_send(int fd, const char *data, size_t len);

/* Receives one framed message of at most max bytes into a NUL-terminated heap
 * buffer.  timeout_ms <= 0 waits forever.  Returns 0 on success, 1 on a clean
 * EOF before any byte, -1 on error (errno EMSGSIZE when too large, ETIMEDOUT
 * on timeout, EPROTO on a truncated frame). */
int th_ipc_recv(int fd, size_t max, int timeout_ms, char **out, size_t *len);

/* Client helper: connects, sends request, waits for the response and parses
 * it.  Returns the parsed response or NULL with a message in err. */
th_jval *th_ipc_call(const char *socket_path, const char *request, size_t len, int timeout_ms,
                     th_strbuf *err);

#endif
