# Changelog

## Unreleased

- A metadata or directory-read I/O failure retains cached children instead of interpreting partial enumeration as deletion. A syscall-injected restart/recovery regression runs without privileges; permission-denial checks explicitly skip privileged package-test UIDs.

- Parent reconciliation preserves same-parent directory/descendant identity when it observes a rename before the inotify pair reaches the writer. Bounded inode lookup and byte-safe path updates avoid a recursive rescan; tests cover both sort directions and Unicode ancestors.

- Enumeration gaps remain Stale across unrelated successful edits. Only a complete full verification clears them; real permission-denial/recovery coverage retains cached records and prevents false certification.

- Coalesced bursts deduplicate shared ancestor aggregation; subtree and hard-link repair queries use bounded path/inode plans. Real 20,000-directory, 1,000-change measurement improved convergence from 16.95s to 0.836s, with verified root totals; raw runs retained in `docs/benchmarks/v0.5-watch-burst.json`.

- Watch-registration failures are scoped to the affected root. A full verification retries coverage and clears a transient failure; a test-only linker wrapper verifies ENOSPC isolation and real live-update recovery without changing system limits.

- Successful full reconciliations prune watches outside the newly enumerated scope. Adding/removing a directory exclusion releases/reinstalls kernel coverage without background scan churn.

- IPC sends and client calls now use monotonic deadlines across partial I/O. Saturated listener probes fail promptly without unlinking the live daemon socket; oversized client requests are rejected before connecting.

- Live root loss retains Offline state and cached records. Watches are released for detached roots and moved-out directory subtrees; explicit verification reinstalls coverage after reconnection.

- Atomic same-root file and directory replacements now preserve the moved source and descendant identities, deleting the replaced destination within the move transaction. Real inotify regression covers both cases.

## 0.5.0 — 2026-10-08

- Transactional schema 2→3 migration adds ordered filename pagination. Bounded FTS probes choose selective or broad-query plans; immediate-child listings explicitly use the parent index.
- Per-root verify/snapshot recovery avoids changing unrelated root states; removing roots releases watches and queued work. Periodic safety reconciliation remains separate from normal incremental updates.
- Reject overlapping root paths that conflict with global pathname ownership.
- Persistent byte-safe bookmarks, Home/mount navigation, asynchronous file/folder opening and snapshot completion deadlines in GTK.
- Real million-file scan/search/idle-memory/GTK-launch measurements with retained raw evidence; exact timings and filesystem gaps in docs/PERFORMANCE.md.
- Native package upgrade gates install the published v0.4.0 build, populate index/history, then verify identity-preserving upgrades in Debian and Fedora.

## 0.4.0 — 2026-10-08

- Transactional schema 1→2 migration preserves indexed entries and FTS.
- Daily successful-reconciliation root snapshots plus explicit verified capture on the sole writer thread, with per-root bounded retention and cascading cleanup.
- History chart and dated byte totals, growth/shrink summary and largest changes between common recorded directory paths. At most 1,024 directories per capture and 365 chart points; missing directory coverage is not treated as zero.
- Real GTK capture/refresh, filesystem growth/retention, migration rollback and integrity tests.

## 0.3.1 — 2026-10-08

- Initialize empty treemap weights for strict optimized GCC builds; v0.3.0 publication was blocked by this compiler diagnostic.

## 0.3.0 — 2026-10-08

- Interactive Cairo treemap: indexed directory scope, logical/allocated metrics, type colors, byte-safe path tooltips and directory click navigation.
- Bounded 512-child IPC, exact remainder statistics and tiny-item aggregation; event-driven redraw only.
- Proportional area/non-overlap layout tests, real 600-child IPC aggregation and GTK geometry-driven navigation smoke.
- Remember the selected view and metric alongside search preferences.

## 0.2.0 — 2026-10-08

- Live indexing milestone: coalesced updates, stable same-root moves, multi-root overflow recovery, restart reconciliation and atomic background configuration.
- Includes the corrected native packaging foundation from v0.1.1.

## 0.1.1 — 2026-10-08

- Fix const qualification rejected by Fedora 44 strict compilation; v0.1.0 publication was blocked.
- Per-root recovery generations ensure every root reconciles after kernel overflow or explicit verification.
- Real kernel overflow, multi-root convergence, offline cached searches, SIGKILL restart and SQLite integrity tests.
- Close the shutdown event descriptor when the watch-thread creation fails.

## 0.1.0 — 2026-10-08

- Persistent byte-safe SQLite index, filename trigram search, filters and pagination.
- GTK-free daemon with owner-only framed IPC, asynchronous scans and restart reconciliation.
- Coalesced inotify updates and same-root directory moves preserving descendant identity.
- CLI search/status/roots/scan/verify/rebuild/config and verified scan completion.
- GTK4 indexed explorer/search with asynchronous IPC, usage bars, filters and result actions.
- Strict builds, unit tests, real daemon/CLI integration, and Xvfb GUI integration.
- Atomic configuration editor, mount discovery, exact/all-root search and persistent view preferences.
- Native Debian 13/Fedora 44 package validation and tag-gated automated publication.
- Cross-directory hard-link metadata updates and surviving-link aggregate repair.
