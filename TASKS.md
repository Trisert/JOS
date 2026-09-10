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
| 10 | todo   | **RedPill-T license grant** — `radiolib_driver.cpp`/`radiolib_hal.cpp` adapt code from Marco-42/RedPill-T (`license: null`, all-rights-reserved) | [#49](https://github.com/Trisert/JOS/issues/49) | Legal blocker for flight: need explicit grant or license alignment before shipping the vendored wrapper. 2026-08-24: JOS stesso ora è MIT (PR #52 merged) ma ciò NON copre il codice adattato da RedPill-T (`license: null` upstream); serve grant esplicito da Marco-42. |
| 11 | blocked | **Radio GPIO bring-up verification** — CS_TTC/LoRa_Busy/GPIO_INT/LoRa_NRST are placeholders until the OBC schematic lands; NRST is multiplexed with DEPLOY_SENSE (`radiolib_hal.cpp:139`) → validate reset-pulse vs deploy logic in HIL (ESP32 sim) | [#54](https://github.com/Trisert/JOS/issues/54) | Waiting on: OBC schematic. Then fix pin map in `radiolib_driver.cpp:lora_init()` + CubeMX .ioc, and run HIL matrix. Acceptance criteria tracciati in #54. |
| 12 | done    | **Unblock Ceedling CI** — root cause found: `test_lora_rx_task_loop_delays_at_the_registered_period` spins forever because the RX task is event-driven (no `osDelay` since PR #47); every Ceedling run since 2026-08-14 died at the 6 h timeout | PR [#50](https://github.com/Trisert/JOS/pull/50) | MERGED 2026-08-22. All 4 checks green on the PR (Ceedling 28 s vs 6 h hang). Local verify: 142/142 host tests on aarch64. Follow-up [#51](https://github.com/Trisert/JOS/pull/51) also MERGED: shared escape scaffold + SIGALRM hang ceiling (30 s, mutation-tested) — no more 6 h spins possible. |
| 13 | done    | **Branch cleanup** — 12 locali + 21 remoti cancellati (tutti con PR merged/chiusa; orfani protetti da tag `archive/*`) | — | Completato 2026-08-22. Su GitHub e in locale resta solo `main` + tag `archive/pr17head`, `archive/fix/ceedling-clean`, `archive/fix/linker-comment-fix`. |
| 14 | done    | **FRAM cyclic writes chip-boundary safe** — `fram_write` spezzato in write per-chip; rimosso guard irraggiungibile; test NULL + cross-boundary | PR [#53](https://github.com/Trisert/JOS/pull/53) | MERGED 2026-08-24. 2 round Kilo (primo: guard irraggiungibile + path NULL scoperto → fixati); tutte e 4 le check verdi sull'head finale `ddc1a19`. |
|| 15 | done    | **Full-codebase review** — watchdog/FRAM/radiolib vs convenzioni skill; suite host 144/144 su Pi; 2 warning documentali trovati e fixati | PR [#55](https://github.com/Trisert/JOS/pull/55) | MERGED 2026-08-24. Rimossi stub morto `watchdog_kick()` in state_machine.c + TODO stantio comms.c:10. 4/4 check verdi + Kilo "No Issues". Restano aperti solo #49 (grant RedPill-T) e #54 (HIL). |
| 16 | done    | **T33 — DMA SPI1 (SPF §3.6.4.2)**: DMA1 Channel2 (RX) / Channel3 (TX) via CSELR `DMA_REQUEST_1` (L496 ha NO DMAMUX), IRQ prio 5, path di transfer completo in `radiolib_hal.cpp` (chunk 0xFFFF, timeout derivato dal baud PCLK2/prescaler, `__WFI` su flag settato dalle callback, abort + zero-fill + latch `spiLastError`). | PR [#63](https://github.com/Trisert/JOS/pull/63) | **MERGED 2026-09-09** (merge `d7c2c51`). Rebase su main + rework `a09773d` (re-review APPROVE) + `c69c297` (cppcheck constParameterPointer). `make release` exit 0, 242/242 host, static-analysis verde. Nuovo header `spi_dma_sched.h` + `test_spi_dma.c` (10 test su chunk/timeout). |
| 17 | blocked | **T34 — Peripherals alignment (MAX11128 vs MAX11228, DEPLOY_SENSE/LoRa_NRST, TEMP_IN/OUT)**: verificare e risolvere gap rispetto SPF v3 §3.6.4.2 e §3.7.5.3.2. | — | Rate-limit ci ha bloccato i subagenti. Da rieseguire. |
| 18 | blocked | **T35 — Timebase TIM6 (SPF §3.6.4) + anomaly detection task (SPF §3.6.1)**: migrare da SysTick a TIM6 per FreeRTOS tick; aggiungere task anomaly detection dedicato se watchdog attuale non copre. | [#70](https://github.com/Trisert/JOS/issues/70) | SysTick condiviso HAL+RTOS (non TIM6 — gap). Anomaly: `watchdog_monitor_task` copre SPF §3.6.1 ✅. Raccomandazione: documentare, non migrare (TIM6 impatta catena HAL_GetTick). Timebase task 256 parole. |
| 19 | blocked | **T36 — FRAM DMA SPI2**: dopo decisione architettura radio e FRAM bus (T23/T25), configurare DMA2 per SPI2 se FRAM su SPI2. Dipende da decisione. | — | Dipende da decisione Nicola su FRAM bus (SPI2 vs I2C1). Stale fino a decisione. |
|| 20 | todo | **TBD — Power mode sleep/wakeup**: verificare supporto sleep mode compatibile con STM32L4 (SPF §3.6.4). | — | Da fare. |

| 21 | done | **T-PAY-verify (SPF §3.2 vs App/payloads)**: 11 gap. CRYSTALS: mancano H-bridge ±, driver I2C camera, macchina a stati 4-stati, interlock heater. CLOUD: MCP23017 assente, ADC MAX11228-vs-11128 da chiarire (SPF incoerente: §3.2.2.1 dice 11228, §3.6.4.2 dice 11128), 2ª faccia non indirizzata, breach non monotonico. CLEAR: 12 LED vs 3 banchi, nessuna fusione MAG/IMU, header 32B mancanti. | — | Report-only, nessuna modifica. Dettagli nel thread #JOS 08-09. |
| 22 | done | **T-AOCS-verify (SPF §3.3/§3.4 vs App/aocs, App/bms)**: AOCS quasi tutto placeholder (no B-dot, no EKF, task mai avviato, docs/api incoerente). BMS: SoC=100 fissi, BQ76905 mai interrogata, soglie 60/40 inventate (SPF fissa solo B_OPOK≈80%, B_SCRIT≈25%), mancano gate s3→s4 e s4→s2, SoH assente. | — | Report-only, nessuna modifica. |
| 23 | done | **T-COMMS-verify (SPF §3.7 TT&C vs App/comms)**: parametri LoRa, beacon, encryption/CRC/NACK/chunking. | [#68](https://github.com/Trisert/JOS/issues/68) | 5 gap: (1) ❌ **CR=4/9 bug** — `radiolib_driver.cpp:60` `cr=5` → `4/9` non standard, SPF vuole 4/8 (`cr=4`). (2) ❌ Power 22 vs 31.6 dBm. (3) ❌ Packet size 64 B vs SPF 128 B (chunk condiviso). (4) ❌ NACK/retransmission assente. (5) ❌ Encryption assente (CRC unkeyed — documentato). Beacon 1-16 min ✅, SF10 ✅, BW 125 ✅, F=436 ✅. |
| 24 | done | **T-SM-verify (SPF §1.5 modi/transizioni + §3.6.5 memoria vs state_machine.c + memory.c)**: trigger, gate SoC, LastStates pool, budget FRAM. | [#69](https://github.com/Trisert/JOS/issues/69) | Stati s0-s4 ✅ (5 state, s2 CRIT, s3 READY, s4 ACTIVE). **❌ Manca `TRIGGER_TASK_COMPLETE`** (s4→s3 per completamento task, SPF Tab.3.21). Gate SoC s2↔s3 ✅ (b_opok=80%, b_scrit definito). LastStates pool 128 B × max entry ✅ (static assert), FRAM 64 KB ciclica ✅ (FM24VN ×4), dual-bank Flash ✅, mutex lock ✅. ❌ **SoC hardcoded=100** (`state_machine.c:64`) in testing. |
| 25 | todo    | **T34-suddiviso**: task T34-suddiviso: suddividi T34 in sottotask specifici per parallelismo. Task T14 (MAX11228) | issue TBD | Suddividere T34 in: T14 (ADC driver MAX11128→MAX11228 VREF/risoluzione), T15 (DEPLOY_SENSE/LoRa_NRST multiplexing), T16 (TEMP_IN/OUT one-wire), T17 (FRAM SPI2 DMA). Ogni sottotask = worktree+branch+PR. |
| 26 | done | **PR #58 Kilo fix (seu_mitigation_sync fuori da state_mutex)** → PR #68 `fix/seu-fram-timing`: `try_transition()`/`enter_safe_state()` commit-only, write-through via `state_fram_sync()` dopo release mutex. | [#68](https://github.com/Trisert/JOS/pull/68) | CI VERDE (firmware 45s, ceedling 135/135, static-analysis, CodeRabbit skip). Il fail iniziale era solo upload artifact 403 infra — rerun ok. Kilo re-review in attesa. |
| 27 | done | **CodeRabbit #57 (5 finding docs) + #60 (2 finding EXTI0 prio)** — entrambi già fixati sui branch PR prima del mio intervento: #57 commit `a8dfaca` (verificato: MD040 ok, Thor workaround, stack 0x400, size path, pipefail+tee), #60 commit `a0daea3` (prio logica 5 + ICD doc). Subagent verifica-only, nessun push. | [#57](https://github.com/Trisert/JOS/pull/57) [#60](https://github.com/Trisert/JOS/pull/60) | Nulla da fare. |
| 28 | done | **Fix LoRa CR →4/8 (SPF Tab. 3.28)** → PR #69: `cr=8` (denominatore diretto RadioLib — il `4/(cr+4)` iniziale era sbagliato, `cr=4` = no coding). Commit `af29496`, build ok, 144/144. | [#69](https://github.com/Trisert/JOS/pull/69) | CI re-run dopo fix. Non mergiata (attende via). |
| 29 | done | **T35 TIM6 (SPF Tab. 3.18)** → PR #70 `fix/t35-tim6-timebase`: TIM6 HAL timebase + SysTick solo FreeRTOS, inline suppress constParameterPointer (`447c37d`). | [#70](https://github.com/Trisert/JOS/pull/70) | CI VERDE (firmware, ceedling 144/144, static-analysis). Non mergiata (attende via). |
| 30 | done | **FRAM→SPI2 (SPF §3.6.4.2)** → PR #71 CHIUSA come errata: BOM dice FM24VN10 = I2C (PB8/PB9), non SPI. FRAM resta su I2C1 (PR #62). Aperte: taglia 512KB vs 64KB asseriti, cablaggio radio al connettore. | [#71](https://github.com/Trisert/JOS/pull/71) | CLOSED unmerged. |
| 31 | done | **TEMP 1-wire PB2 + DEPLOY/LoRa_NRST mux PB1 (SPF §3.7.5.3.1)** → PR #72 `fix/deploy-temp-pins`: driver temp.c/h, deploy_sense.c/h, .ioc+main.h, 161/161 test. | [#72](https://github.com/Trisert/JOS/pull/72) | Subagent review: APPROVE (timing AN187 ok, mux sicuro, GPIO liberi). Nit: break vs continue su CRC, poll ~100ms task-only. |
| 32 | done | **Subagent reviews (criterio 3)**: #70 APPROVE, #63 APPROVE, #72 APPROVE, #68 APPROVE, #69 APPROVE (cr=8), #73 APPROVE, #74 APPROVE, #75 APPROVE (security), #77 APPROVE (hook `dd41dac` riverificato). | — | MERGED: #68, #69, #70, #72, #75. #77: CI re-run dopo hook fix. |
| 33 | done | **FRAM full-size 128KB/chip (512KB)** → PR #73: rework `414ae83` APPROVE; poi rebase su main (`68ab47f`), split alla select a 64KB (`9332675`), probe al boot consumato con `TRIGGER_FRAM_MISSING` (`4ca5882`). | [#73](https://github.com/Trisert/JOS/pull/73) | Driver: split a ogni device select 64KB (copre anche il boundary 128KB), cap chunk 0xFFFF, retry I2C limitato 3x, `fram_init()` interroga gli 8 select (`HAL_I2C_IsDeviceReady`) → `fram_missing_selects()` + `fram_report_boot()` in LastStates. 4 nuovi test (A16 page, >64KB, full-bank 512KB, probe+record). Resta: strapping A2/A1 (Nicola) + Kilo. |
| 34 | done | **TRIGGER_TASK_COMPLETE s4→s3** → PR #74: coverage fix `ab67c5a` (20 test, linee 92.2%, branch 82.8%). | [#74](https://github.com/Trisert/JOS/pull/74) | Subagent review + /review: APPROVE. CI re-run. |
| 35 | done | **HMAC uplink (design upstream)** → PR #75: Kilo fix `12f6974` verificato (DBL gone, comment EN :33, bool+out, KAT). | [#75](https://github.com/Trisert/JOS/pull/75) | Verdetto: APPROVE (verifica diretta; subagent glitchati). CI: fw+ceedling verdi, static/Kilo pending. |
| 36 | done | **Swarm review → fix**: #76 SPI robust (APPROVE, CHIUSA da Nicola senza merge), #77 RTOS safety APPROVE, #78 I2C/CubeMX (APPROVE), #79 framing APPROVE, #80 coverage APPROVE. | [#79](https://github.com/Trisert/JOS/pull/79) [#80](https://github.com/Trisert/JOS/pull/80) | MERGED: #77, #78, #79, #80. |

| 37 | done | **Chiusura board PR (criterio 4)**: merge della coda PR rimasta + chiusura delle superate. | — | **MERGED**: #56, #57, #59, #61 (rebase su main + 232/232), #63, #78, #79, #80. **Chiuse senza merge**: #58 (superata da #68), #60 (ICD pre-netlist: revertirebbe #72/#78), #62 (move I2C1 già su main via #78), #71 (errata SPI2), #74, #76 (chiuse da Nicola). **Issues chiuse**: #64 (MAX11128), #65 (DEPLOY mux), #66 (TEMP 1-wire). Aperte residue: #49 (grant RedPill-T, legale) e #54 (HIL, serve hardware). |

*Ultimo aggiornamento: 2026-09-09 — Allineamento SPF V3 chiuso sul codice: tutte le PR di codice mergiate o chiuse come superate; restano solo #49 (grant licenza RedPill-T) e #54 (HIL su hardware reale). Board PR: #63 DMA SPI1 completo, #73 FRAM 512KB con split alla select + probe al boot, #77/#78/#79/#80 mergiate. Decisioni Nicola ancora aperte: strapping A2/A1 FRAM, cablaggio radio al connettore J6, CR 4/8 JOS vs 4/5 upstream TT&C.*

## Notes

- Tasks 2–3 are children of task 1 (PR #31 epic). They unblock task 1 when done.
- The old kanban DB showed `consecutive_failures: 2` / "pid not alive" on the
  worktree-backed tasks — exactly the kind of worker-stampede symptom the
  SQLite-backed dispatcher produced. The new pattern avoids it.
- To revive the kanban board: `hermes kanban boards recover jos` (or move the
  archived dir back to `boards/jos/`).
