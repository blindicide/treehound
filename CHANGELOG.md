# Changelog

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
