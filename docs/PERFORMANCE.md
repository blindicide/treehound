# Measured performance and filesystem coverage

Measurements on 2026-10-08 used the actual daemon and GTK binaries on a four-CPU
Linux 6.8 x86-64 VM, glibc 2.39, an ext4 workspace on /dev/vda3 and Cairo/Xvfb.
The underlying storage medium is unknown; this is not a certified SSD/HDD comparison.
The fixture contains 1,000,000 actual files, 1,000 directories and one root;
70% of files are empty, 30% contain one byte. Watches were enabled and the root
reported Verified. File names contain deterministic hexadecimal tokens.

| Measurement | v0.4.0 baseline | v0.5.0 |
|---|---:|---:|
| Initial indexing, 1,001,001 entries | 62.35 s | 59.88 s |
| Warm three-character search p95 | 44.95 ms | 74.50 ms |
| Warm six-character search p95 | 2.99 ms | 3.52 ms |
| Broad `item` page, 200 rows | 1,002.55 ms, timeout | 29.93 ms, success |
| Broad `dat` page, 200 rows | 1,003.02 ms, timeout | 16.79 ms, success |
| Short `it` page, 200 rows | 834.83 ms | 3.93 ms |
| Idle resident memory | 23.2 MB | 23.6 MB |
| Peak resident memory including queries | 51.3 MB | 34.7 MB |
| CPU consumed over ten idle seconds | 0.01 s | 0.00 s |
| SQLite main database size | 485.4 MB | 679.0 MB |
| GTK first indexed listing p95 | not measured | 643.23 ms |

The search p95 uses the nearest-rank statistic over 30 measured requests per
length, each immediately preceded by the same query to warm it. Times include
IPC. Results are bounded to 200 rows; they do not count all matches. Three- and
six-character requests use real FTS5 and exact matching. The broader queries
are individual measurements, not p95 estimates. This single-host run meets the
250 ms warm-query and 100 MB daemon-memory targets but is not a universal promise.
Selective queries became slightly slower and the additional ordered filename
index costs approximately 194 MB per million entries. It prevents broad-query
posting-list materialization and sorting before returning the first page.

Ten real GTK launches mapped the window in 245–303 ms and populated the first
indexed listing in 518–643 ms. The measured interval starts inside the client;
separate process-with-Xvfb wall times are retained in the raw evidence. Window
mapping is not a first-paint measurement. A development run initially missed
one second (1,136 ms listing p95): SQLite chose the root index and examined all
descendants. Explicit immediate-child parent-index selection removed that
bottleneck. Desktop sessions, portals and display hardware can change timings.

Raw samples: [v0.4 baseline](benchmarks/v0.4-baseline.json) and
[v0.5 final build](benchmarks/v0.5-million.json). Public root paths are normalized
for privacy; results, timestamps and executable hashes are unchanged. `stat -f`
labels this filesystem “ext2/ext3”; the live mount table identifies ext4.
The repeatable fixture/measurement harness is `tests/benchmarks/million.py`;
see [BUILDING.md](BUILDING.md). Retained local fixture/index data are not packaged.

## Continuation: deep pagination

The prepared 0.5.1 search plan timed out at offset 10,000 on the retained
million-file index: 1,001.00 ms, with no returned page. A bounded FTS probe now
selects ordered filename-index streaming with exact term/filter matching; it
avoids redundant per-row FTS posting-list lookups. The uncontended patched
probe returned 200 distinct matching rows at offsets 0, 200, 400 and 10,000
in 6.08, 4.73, 4.08 and 12.63 ms respectively. These are individual requests,
not p95 estimates. [Raw baseline and dirty-candidate provenance](benchmarks/v0.5-deep-pagination.json)
records executable hashes, base commits and the exact changed file list. The
benchmark harness also retains partial failure evidence and checks page IDs.
The one-second query guard remains; arbitrary deep offsets or expensive filters
can still require refining the query.

## Correctness and remaining validation gaps

Tests exercise actual sparse files, multiple hard-link paths, invalid UTF-8,
Unicode/spaces, symlinks, denied enumeration, paths longer than PATH_MAX,
interrupted scans, live changes, same-root paired moves, multi-root kernel
inotify overflow, offline roots, SIGKILL/restart, migration rollback, SQLite
integrity, snapshot retention and installed package upgrades. Paired moves
preserve indexed descendant identities; unpaired moves converge through diffs.
Mount-table fixtures cover escaped mount paths and eCryptfs backing detection;
these parser tests are not physical special-filesystem certification.

The host provides no mounted Btrfs, XFS or eCryptfs test view. Creating those
would require unavailable fixtures or host-level mount/privilege changes outside
this mandate. Real mount/unmount and compressed/reflink storage behavior are
therefore unverified. Select a decrypted eCryptfs view as a root; avoid selecting
its encrypted backing view simultaneously unless intentionally analyzing both.
Roots must have disjoint path ownership. Treehound keeps logical bytes separate
from `st_blocks * 512`; allocated totals do not measure unique physical storage
under compression, shared extents or reflinks. GTK behavior is tested under
Xvfb, not a physical desktop. These are explicit coverage gaps, not claimed passes.
