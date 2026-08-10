# JOS — Task Board (markdown, no SQLite)

This file is the **human-readable source of truth** for JOS work tracking.
It replaces the old Hermes kanban board (SQLite), which was archived on
2026-08-10 due to documented concurrency-corruption issues (Hermes issues
#33334, #30687, #31502). The kanban DB is still recoverable under
`~/.hermes/kanban/boards/_archived/jos-*/` but the dispatcher is disabled.

## Workflow (the "normal" parallel pattern, no race conditions)

- The **orchestrator (Hermes)** splits work into isolated scopes.
- Each unit of work runs as a **separate subagent** via `delegate_task`,
  each in its **own git worktree** (`JOS/.worktrees/<task>`) on its own branch.
- Subagents do **not** share a state DB — the only shared write surface is git
  itself (branch + PR), which is designed for concurrent, conflict-safe merges.
- Results land as **GitHub PRs**; `kilo-review-loop` drives each PR through
  Kilo Code Review until clean, then merge.
- `max_concurrent_children = 3` (RAM is not the limit on this Pi; the cap
  protects the `hy3:free` API rate limit and `state.db` write contention).

## Legend

- `todo`    — defined, ready to be worked
- `blocked` — waiting on a dependency / decision
- `done`    — completed & merged (or verified)

## Tasks

| # | Status  | Task | GitHub Issue / PR | Notes |
|---|---------|------|-------------------|-------|
| 1 | blocked | **PR #31 — resolve the 5 Kilo findings and merge to main** | [#33](https://github.com/Trisert/JOS/issues/33) · PR [#40](https://github.com/Trisert/JOS/pull/40) [#41](https://github.com/Trisert/JOS/pull/41) | Epic. Children #2–#3 (PR #40, #41) in Kilo review; when clean, this is ready to merge. Branch: `fix/build-hardening-flags`. |
| 2 | review   | **PR #31 — Makefile: proof-of-life `cppcheck-includes` + contradictory comments + "To-bump" checklist** | [#34](https://github.com/Trisert/JOS/issues/34) · PR [#40](https://github.com/Trisert/JOS/pull/40) | 7→2→round-3 Kilo findings resolved; `cppcheck-includes-proof` is now a static assertion wired into `build.yml`. Round-3 in Kilo review. Branch: `jos/pr31-makefile`. |
| 3 | review   | **PR #31 — `memory.c`: re-verify `unmatchedSuppression` suppression** | [#35](https://github.com/Trisert/JOS/issues/35) · PR [#41](https://github.com/Trisert/JOS/pull/41) | Investigation: no per-line `cppcheck-suppress unmatchedSuppression` exists — it is a build-level flag in Makefile:219 (legitimate). Clarified the staleness comment. PR open, in Kilo review. Branch: `jos/pr31-memory`. |
| 4 | review   | **Watchdog — suspend/delete task on anomaly (`watchdog.c:217`)** | [#36](https://github.com/Trisert/JOS/issues/36) · PR [#42](https://github.com/Trisert/JOS/pull/42) | Implemented: suspend (not delete) + LastStates log via `TRIGGER_WATCHDOG`; `stalled` latch; escalation outside `wdg_mutex`. PR open, in Kilo review. Branch: `jos/watchdog-anomaly`. |
| 5 | review   | **BMS — init subsystem SPI master to EPS STM32L496** | [#37](https://github.com/Trisert/JOS/issues/37) · PR [#43](https://github.com/Trisert/JOS/pull/43) | SPI2 master already initialised (main.c/MspInit); `bms.c` was unbound — bound it via `extern hspi2` + `bms_spi_init()` (2.5 MHz, mode 0). EPS CS pin + `bms_get_status()` txn still stubs. PR open, CI pending. Branch: `jos/bms-spi`. |
| 6 | done    | **Evaluate PR #17 (SEU mitigation) — closed, unmerged** | [#38](https://github.com/Trisert/JOS/issues/38) | CLOSED: superseded by #14 (sram2 parity NMI) + #16 (SEU scrub). Every file/logic of #17 is present in `main`; no unique code to recover. |
| 7 | done    | **PR #31 — `building.md` + clean merge vs main** | [#39](https://github.com/Trisert/JOS/issues/39) | Verified: `git merge-tree` / worktree merge against current `main` is clean (no conflicts). |

## Notes

- Tasks 2–3 are children of task 1 (PR #31 epic). They unblock task 1 when done.
- The old kanban DB showed `consecutive_failures: 2` / "pid not alive" on the
  worktree-backed tasks — exactly the kind of worker-stampede symptom the
  SQLite-backed dispatcher produced. The new pattern avoids it.
- To revive the kanban board: `hermes kanban boards recover jos` (or move the
  archived dir back to `boards/jos/`).
