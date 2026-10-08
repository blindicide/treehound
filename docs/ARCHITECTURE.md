# Architecture

Treehound targets Linux x86-64 and C17. `treehoundd` owns the writable SQLite
connection. Its scanner thread processes FIFO reconciliation jobs; connection
threads use separate read-only handles and remain available while scanning.
At most 32 connections are served simultaneously. Each connection handles one
request with a five-second framing deadline. Requests are capped at 64 KiB,
responses at 64 MiB, and pages at 5,000 rows. Search has a one-second SQLite
progress-handler deadline, including the short-query database fallback.

IPC is length-prefixed JSON over a mode-0600 Unix socket in a private runtime
directory. SO_PEERCRED checks the client's UID. Protocol revision is 1; clients
send `v` and `cmd`; replies carry `v`, `ok` and the product version. Failed
requests carry `code` and `error`. Embedded NUL in request strings is rejected.
Invalid UTF-8 filenames use surrogateescape JSON (`\udc80` through `\udcff`).

Commands currently implemented are status, roots, search, list, scan, verify,
rebuild, config (read), and shutdown. Scan/verify/rebuild return a sequence;
status's completed_seq is a FIFO completion watermark, not proof of success.
Check roots' states and errors as well. Rebuild requires an explicit root ID.
`list` currently selects descendants; immediate-child browsing is pending.

Startup marks cached roots Indexed (or Stale after an interrupted scan), then
reconciles asynchronously. A database lock prevents two writers even when
configured with different sockets. SIGTERM/SIGINT cancel scanning and drain
connections before joining the writer. SQLite WAL provides crash recovery.
No service is enabled automatically.

Inotify watches are installed before directory enumeration. The event reader
coalesces at most 4,096 dirty directories and queues writer jobs. Event diffs
visit affected directories and newly discovered subtrees, preserving unchanged
subtrees. Same-root paired moves update raw-byte paths and parent relations in
SQL, preserving descendant IDs and watch paths. Unpaired moves converge through
parent diffs. Overflow, unmount and dirty-queue exhaustion request whole-root
reconciliation; watch failures and enumeration errors leave roots Stale with
warnings. No watch limits are changed. Periodic reconciliation is a configurable
safety net, not the normal live-update path. The event thread blocks on inotify
and an explicit shutdown eventfd. Search replies inherit root consistency state.

Current development increment: daemon scanning, IPC, live updates and restart
reconciliation are tested. CLI/GTK client, visual analysis, history, CI and
release packaging remain pending. Kernel overflow, special filesystems and
mount namespace behavior still need dedicated validation. This increment is
not a finished release.

## Command-line client

`treehound search QUERY` and `status`, `roots`, `scan`, `verify`, `rebuild`, and `config` use only framed daemon IPC. `--json` preserves byte escapes. Search supports `--exact` (the whole query is a name or path), literal `--extension`, case/type/size/mtime/subtree filters, pagination and sorting. `--wait` on scan commands waits for the FIFO completion watermark and checks root consistency; an offline or stale root returns failure. Exit codes are 0 success, 1 no matches, 2 invalid usage, 3 unavailable daemon, 4 operation failure.
