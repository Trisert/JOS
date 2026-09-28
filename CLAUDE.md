# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The binding working rules for this repository live in `AGENTS.md`: order of
authority, gates, code rules, tests, change workflow and definition of done.
They are imported below and are not repeated here. This file adds only the
practical detail that `AGENTS.md` leaves out.

@AGENTS.md

## Commands

All build and gate commands run from `JOS/`, not from the repo root. The full
list is in `AGENTS.md` §2. These are the ones that list does not spell out:

```sh
# Flight build with the uplink HMAC key (without it, authenticated TCs are rejected)
make release COMMS_AUTH_KEY=<8 hex digits>
make crc-stamp               # a later make without the key KEEPS the provisioned key
make release COMMS_AUTH_KEY= # an explicit empty value removes it (so does make clean)

# Host tests: run inside JOS/test (Ceedling 1.0.1 gem, host gcc)
cd test
ceedling test:all
ceedling test:test_laststates   # one test file (the name of test/test/test_<x>.c)
ceedling gcov:all               # coverage gate (>=90% lines, >=75% branches)
```

Environment notes:
- If Ceedling fails with `invalid byte sequence in US-ASCII`, export
  `LANG=C.UTF-8 LC_ALL=C.UTF-8`.
- In the cloud container the gem binary may not be on `PATH`: it lives under
  `/opt/rbenv/versions/*/bin`.
- To prove a new test can fail (`AGENTS.md` §5), stash the fix with
  `git stash push <file>`, rerun the single test file, then `git stash pop`.

## Architecture: the big picture

**Boot path (`Core/Src/main.c`, inside `USER CODE` blocks).** The order
matters, and several modules rely on it:
1. `mpu_init()` locks down memory.
2. `hw_watchdog_init()` arms the IWDG.
3. HAL init.
4. `sram2_parity_init()`, which erases SRAM2 and restores it, so nothing in
   SRAM2 may be touched earlier.
5. The peripherals are initialised and the boot CRC is checked.
6. `dual_bank_init()` persists boot-fault evidence and decides whether to fall
   back to the golden image. It runs **before** `laststates_init()`, so it
   writes to the pool while the cursor is still unknown.
7. `fram_init()`.
8. `laststates_init()` re-derives the Flash ring cursor.
9. `lora_init()`.
10. `seu_mitigation_init()` registers the SEU regions and restores them from
    FRAM. It must come after the modules that own those regions.
11. The tasks are created (state machine, watchdog, LoRa beacon, LoRa RX, SEU
    scrubber, default task), then `osKernelStart()`.

A boot is declared good only later, after 5 s of scheduler uptime, by the
watchdog task (`dual_bank_boot_complete()`). It is never declared at
`osKernelStart()`.

**LastStates pool** (`App/memory/memory.c`) is the single forensic log that
everything writes into:
- Writers: state transitions, fault handlers, `configASSERT`, the watchdog
  escalation and the dual-bank boot evidence.
- Layout: 64 × 128 B Flash slots in 2 KB pages. The 128 B entry format is a
  frozen ground ICD.
- Cursor: there is no sequence number, so the cursor is recovered only from
  the Flash erase state. This depends on the **erase-ahead** invariant: exactly
  one erased run is kept, so a wrapped ring holds 48–63 records.
- Locking: one mutex serialises all writers. The watchdog asks who holds that
  mutex before it escalates, because escalating a task that holds it would
  deadlock the logging.

**Fault containment chain:**
1. Task liveness: `App/obsw/watchdog.c` checks it every 500 ms.
2. A stalled task leads to a LastStates record, then
   `dual_bank_mark_boot_fault()`, then `NVIC_SystemReset()`.
3. The boot-fault counter in the `.boot_fault` NOLOAD scratch (survives a reset)
   decides the fallback to the golden image in bank 2.
4. The IWDG and the external STWD100 are the backstop for lockups.

The watchdog task is the single owner of the IWDG refresh once the scheduler
runs.

**State machine** (`App/obsw/state_machine.c`): five states, OFF → INIT →
CRIT → READY → ACTIVE. It is the only place where state changes. It consumes
BMS state of charge through the `bms.h` contract; EPS is a separate MCU, and
its frame format is still undefined (see `TASKS.md` blockers).

**Radio path** (`App/comms/`):
- `comms.c` owns the beacon and RX tasks and the TC dispatcher.
- `comms_validate.c` checks frames, including the HMAC with the key from the
  generated `build/gen/comms_auth_key.h`.
- `radiolib_driver.cpp` wraps the vendored RadioLib SX1268 behind a
  C API (`lora_*`) and a radio mutex.
- The DIO1 ISR only routes thread flags: to the TX waiter during a transmit
  sequence, otherwise to the RX task.
- The sender re-arms RX after each transmit sequence.

**SEU mitigation** (`Core/Src/seu_mitigation.c`): critical RAM regions (for
example the LastStates bookkeeping mirror) are registered, then snapshotted
with `seu_mitigation_commit()` under a PRIMASK lock. They are scrubbed every
5 minutes and persisted to FRAM, so an owner must commit after every legitimate
update.

## Host test structure (`JOS/test`)

- Only host-compilable modules are on the Ceedling source path:
  `App/obsw`, `App/memory`, `App/comms` (C only), `App/aocs`, and
  `App/payloads/temp.c`.
- **Not host-tested: build and cppcheck are their only evidence.** This covers
  `Core/Src/*` (`dual_bank.c` included), `App/bms`, the other payloads and
  the C++ radio driver. Say so in the PR body.
- There are two doubling strategies. Never mix them on one symbol:
  - Behavioural fakes, used for Flash, FRAM and HAL:
    - `fakes/` shadows target headers.
    - `support/host_flash.c` emulates the Flash pool, with failure injection:
      `host_flash_fail_program_after(n)` and `host_flash_fail_next_erases(n)`.
  - CMock mocks, used for the RTOS-facing modules (watchdog, comms),
    generated from `fakes/cmsis_os.h`, `fakes/task.h` and App headers.
- Anything on the `support/` path links into every test binary, so its header
  must not also be mocked.
- The infinite task loops are tested by capturing the entry pointer from
  the `osThreadNew` stub, then using an `osDelay` stub that `longjmp`s out
  after N iterations (see `run_monitor_scans()` in
  `test/test/test_watchdog.c`).
- Per-test defines go in `project.yml`, matched by a regex on the test file
  path (`/test_comms\.c$/`). A bare name would substring-match other tests.

## Repo gotchas

- **CRLF files.** Several CubeMX files are CRLF: `Core/Src/main.c`,
  `stm32l4xx_it.c`, `freertos.c`, `stm32l4xx_hal_msp.c`,
  `Core/Inc/FreeRTOSConfig.h`, `main.h` and others. Preserve the line endings
  when editing them. A scripted rewrite that converts them to LF shows up as a
  whole-file diff.
- **Commit messages** are usually in Italian. Code, comments and docs are
  English only.
- **`TASKS.md`** has one row per task. Refresh its footer date on every PR.
