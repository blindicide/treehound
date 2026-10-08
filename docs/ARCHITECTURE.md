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

Current development increment: scanning and IPC are tested; live monitoring,
CLI/GTK client, visual analysis, history, CI and release packaging are pending.
A completed scan currently means a point-in-time filesystem reconciliation,
not continuous monitoring coverage. This increment is not a finished release.
