# Database

SQLite schema revision 1 is authoritative in `include/treehound/db.h`.
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
