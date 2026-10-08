# Database

SQLite schema revision 3 is authoritative in `include/treehound/db.h`.
`th_db_open` creates/migrates transactionally and rejects newer schema versions.
The daemon enables WAL; GUI and CLI must use IPC, never open the database.

Roots record path, device, state, scan timestamps, verification time and the
root entry. Entries record parent/root identities, raw-byte name/path, type,
logical size, allocated size (`st_blocks * 512`), aggregates, mtime, device,
inode, link count, consistency state and flags. Query indexes cover identities,
parents and paths. FTS5 trigram indexes narrow applicable Unicode name searches;
exact pattern matching verifies candidates. Short queries use a bounded-time
SQLite fallback. Prepared statements bind all user query values.

Names and paths preserve kernel bytes. They must not be normalized as Unicode.
Hardlink paths remain individually searchable. LINKDUP flags exclude duplicate
inode allocations from aggregates; file counts count pathnames. Directory
logical/allocated totals include directory metadata. Sparse logical sizes can
exceed allocated sizes. Allocated totals are not physical disk usage for
compressed files, reflinks, snapshots, shared extents or filesystem metadata.

Schema 1→2 adds `snapshots` (root, capture time, logical/allocated totals,
file/directory counts) and `snapshot_dirs` (snapshot, raw-byte path, aggregates).
The migration is a single transaction and preserves the entries and FTS tables.
A migration failure leaves the old version and data intact. Newer schemas are
rejected rather than downgraded.

Snapshots are written only on the scanner thread after a complete, error-free
reconciliation with no pending monitor work. Automatic captures occur no more
than once per 24 hours; the GUI can explicitly request a full verified capture.
Offline, stale and partially enumerated roots produce no fresh snapshot.
`snapshot_retention` bounds per-root captures (default 30); cascading deletes
remove directory records with expired snapshots or removed roots. Each capture
retains at most the 1,024 largest logical-size directories, so change summaries
compare only paths recorded in both captures. Renames, newly added/deleted
paths and directories outside this coverage have no inferred directory delta.
Root totals remain complete subject to the index's documented size semantics.
IPC returns at most 365 ordered captures and 20 common-path changes.

Schema 2→3 adds the ordered `(name COLLATE NOCASE, name, path)` index.
Broad name-sorted searches probe at most 4,097 FTS postings, then test FTS
membership while streaming the ordered index. Selective searches keep the
rowid-driven plan; optional total-count requests use that plan as well. All
matching and filters still run exactly, with the one-second IPC query guard.

Immediate-child listings explicitly use `entries_parent`; choosing the root
index on a million-entry root otherwise scans unrelated descendants.
