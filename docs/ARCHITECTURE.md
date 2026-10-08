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
rebuild, config (read), config_save, mounts, treemap, history, snapshot and shutdown. Scan/verify/rebuild return a sequence;
status's completed_seq is a FIFO completion watermark, not proof of success.
Check roots' states and errors as well. Rebuild requires an explicit root ID.
`list` selects immediate children through the parent index.

Startup marks cached roots Indexed (or Stale after an interrupted scan), then
reconciles asynchronously. A database lock prevents two writers even when
configured with different sockets. SIGTERM/SIGINT cancel scanning and drain
connections before joining the writer. SQLite WAL provides crash recovery.
No service is enabled automatically.

Inotify watches are installed before directory enumeration. The event reader
coalesces at most 4,096 dirty directories and queues writer jobs. Event diffs
visit affected directories and newly discovered subtrees, preserving unchanged
subtrees. Same-root paired moves update raw-byte paths and parent relations in
SQL, preserving descendant IDs and watch paths. Atomic replacements delete the
old destination inside the same transaction before moving the source. Unpaired moves converge through
parent diffs. Overflow, unmount and dirty-queue exhaustion request whole-root
reconciliation; watch failures and enumeration errors leave roots Stale with
warnings. No watch limits are changed. Periodic reconciliation is a configurable
safety net, not the normal live-update path. The event thread blocks on inotify
and an explicit shutdown eventfd. Search replies inherit root consistency state.

The daemon/CLI/GTK explorer and search are tested end to end. Native Debian 13
and Fedora 44 package validation has passed on GitHub Actions. Integration tests
also stop the daemon, exceed the actual kernel inotify queue, and verify that
all roots recover. Global recovery generations are consumed separately by each
root, so an unrelated pending directory diff cannot hide lost events. Offline
roots retain cached searchable entries with an Offline state. SIGKILL/restart
reconciles offline edits and passes SQLite integrity checking. Special filesystem
and mount namespace behavior still require dedicated validation. Actual million-file measurements are recorded in [PERFORMANCE.md](PERFORMANCE.md).

## Command-line client

`treehound search QUERY` and `status`, `roots`, `scan`, `verify`, `rebuild`, and `config` use only framed daemon IPC. `--json` preserves byte escapes. Search supports `--exact` (the whole query is a name or path), literal `--extension`, case/type/size/mtime/subtree filters, pagination and sorting. `--wait` on scan commands waits for the FIFO completion watermark and checks root consistency; an offline or stale root returns failure. Exit codes are 0 success, 1 no matches, 2 invalid usage, 3 unavailable daemon, 4 operation failure.

## GTK client

The client links `th_base` and GTK, never the database engine. A GTask worker issues bounded IPC calls; the main thread updates a virtualized GtkColumnView backed by at most 200 records. One running request and one replacement request bound rapid typing work. Response generation numbers discard obsolete replies. Directory navigation stores indexed identities and byte paths. Search is debounced by GtkSearchEntry. Root state appears in sidebar and result status; refresh is explicit. Home, accessible mounts and up to 64 byte-safe bookmarks navigate through indexed directory identities. Opening files/folders uses asynchronous GIO. A real Xvfb test browses a fixture and searches it through the daemon.

## Treemap

The daemon reads a consistent SQLite snapshot and returns at most 512 immediate
children ordered by the selected logical/allocated metric, plus an exact count
and weight for all remaining children. Duplicate hard-link file weights are zero;
directory weights use the index aggregates. The GUI aggregates weights below
0.1% into Other and draws a balanced binary layout with Cairo. Tooltips retain
raw paths for actions and use replacement characters only for display. Clicking
a directory changes the indexed scope; Back/Up and Explorer remain available
for tiny or zero-size entries. Resize, scope changes and explicit refresh trigger
redraw; there is no animation timer. Allocated sizes have the same compression,
reflink and hard-link accounting caveats as the explorer.

## History

The monitor scan adapter captures successful verified reconciliations on the
original scanner's sole writer thread. Explicit capture requests coalesce into
a root flag and a normal full-scan job; automatic capture is eligible daily.
Capture and retention run in one transaction. Readers use a consistent SQLite
snapshot; the GTK History view receives bounded series and common-directory
changes through its IPC worker. Cairo redraws on data, metric and size changes.
The date/byte summary is also available as selectable text. Configuration controls
per-root retention. No continuous history polling or UI-thread scan is added.
