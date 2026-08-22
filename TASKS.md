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
| 1 | done    | **PR #31 — resolve the 5 Kilo findings and merge to main** | [#33](https://github.com/Trisert/JOS/issues/33) · PR [#31](https://github.com/Trisert/JOS/pull/31) | MERGED 2026-08-10. Children #40+#41 folded in via `fix/build-hardening-flags`. |
| 2 | done    | **PR #31 — Makefile: proof-of-life `cppcheck-includes` + contradictory comments + "To-bump" checklist** | [#34](https://github.com/Trisert/JOS/issues/34) · PR [#40](https://github.com/Trisert/JOS/pull/40) | MERGED. `cppcheck-includes-proof` is a static assertion wired into `build.yml`. Kilo: 7→2→clean. |
| 3 | done    | **PR #31 — `memory.c`: re-verify `unmatchedSuppression` suppression** | [#35](https://github.com/Trisert/JOS/issues/35) · PR [#41](https://github.com/Trisert/JOS/pull/41) | MERGED. No per-line suppression existed; clarified staleness comment (build-level flag in Makefile:219). |
| 4 | done    | **Watchdog — suspend/delete task on anomaly (`watchdog.c:217`)** | [#36](https://github.com/Trisert/JOS/issues/36) · PR [#42](https://github.com/Trisert/JOS/pull/42) | MERGED. Suspend + LastStates log via `TRIGGER_WATCHDOG`; `stalled` latch; escalation outside `wdg_mutex`. |
| 5 | done    | **BMS — init subsystem SPI master to EPS STM32L496** | [#37](https://github.com/Trisert/JOS/issues/37) · PR [#43](https://github.com/Trisert/JOS/pull/43) | MERGED. Bound `bms.c` to existing SPI2 master via `extern hspi2` + `bms_spi_init()` (2.5 MHz, mode 0). EPS CS + `bms_get_status()` txn remain stubs. |
| 6 | done    | **Evaluate PR #17 (SEU mitigation) — closed, unmerged** | [#38](https://github.com/Trisert/JOS/issues/38) | CLOSED: superseded by #14 (sram2 parity NMI) + #16 (SEU scrub). No unique code to recover. |
| 8 | done    | **scrub.c const-correctness (cppcheck constVariablePointer, 2.17.1)** | — | PR [#44](https://github.com/Trisert/JOS/pull/44) | MERGED. `scrub_sync()` lookup result marked `const`. cppcheck 2.17.1 now reports 0 findings on `main`. |
| 9 | done    | **RadioLib SX1268 (LoRa1268F30) integration — B1 scaffolding + B2/B3/B4 wiring** | PR [#47](https://github.com/Trisert/JOS/pull/47) | MERGED 2026-08-13. B1 HAL wrapper + C driver (`extern "C"` verified), B2 TX chunked + beacon, B3 RX task → `comms_rx_handle_frame` + DIO1 ISR, B4 tests. Follow-ups split into tasks 10–11 below. |
| 10 | todo   | **RedPill-T license grant** — `radiolib_driver.cpp`/`radiolib_hal.cpp` adapt code from Marco-42/RedPill-T (`license: null`, all-rights-reserved) | [#49](https://github.com/Trisert/JOS/issues/49) | Legal blocker for flight: need explicit grant or license alignment before shipping the vendored wrapper. |
| 11 | blocked | **Radio GPIO bring-up verification** — CS_TTC/LoRa_Busy/GPIO_INT/LoRa_NRST are placeholders until the OBC schematic lands; NRST is multiplexed with DEPLOY_SENSE (`radiolib_hal.cpp:139`) → validate reset-pulse vs deploy logic in HIL (ESP32 sim) | — | Waiting on: OBC schematic. Then fix pin map in `radiolib_driver.cpp:lora_init()` + CubeMX .ioc, and run HIL matrix. |
| 12 | done    | **Unblock Ceedling CI** — root cause found: `test_lora_rx_task_loop_delays_at_the_registered_period` spins forever because the RX task is event-driven (no `osDelay` since PR #47); every Ceedling run since 2026-08-14 died at the 6 h timeout | PR [#50](https://github.com/Trisert/JOS/pull/50) | MERGED 2026-08-22. All 4 checks green on the PR (Ceedling 28 s vs 6 h hang). Local verify: 142/142 host tests on aarch64. Follow-up [#51](https://github.com/Trisert/JOS/pull/51) also MERGED: shared escape scaffold + SIGALRM hang ceiling (30 s, mutation-tested) — no more 6 h spins possible. |
| 13 | done    | **Branch cleanup** — 12 locali + 21 remoti cancellati (tutti con PR merged/chiusa; orfani protetti da tag `archive/*`) | — | Completato 2026-08-22. Su GitHub e in locale resta solo `main` + tag `archive/pr17head`, `archive/fix/ceedling-clean`, `archive/fix/linker-comment-fix`. |

## Notes

- Tasks 2–3 are children of task 1 (PR #31 epic). They unblock task 1 when done.
- The old kanban DB showed `consecutive_failures: 2` / "pid not alive" on the
  worktree-backed tasks — exactly the kind of worker-stampede symptom the
  SQLite-backed dispatcher produced. The new pattern avoids it.
- To revive the kanban board: `hermes kanban boards recover jos` (or move the
  archived dir back to `boards/jos/`).
