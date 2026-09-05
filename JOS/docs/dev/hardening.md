# Software Hardening Roadmap — RedPill OBSW

Goal: bring the RedPill (JOS) on-board software to a hardened, space-ready /
QM-ready state for ESA Fly Your Satellite! 4. Target: STM32L496VGTx
(Cortex-M4 @ 80 MHz, 1 MB dual-bank flash, 320 KB SRAM — 256 KB SRAM1 +
64 KB SRAM2 with parity, 4× FM24VN10-G FRAM on I²C2 = 64 KB total (4 × 16 KB)).

Every recommendation below cites the standard that motivates it:

- **[NASA-PoT]** — Holzmann, "The Power of Ten — Rules for Developing Safety
  Critical Code" (JPL), https://en.wikipedia.org/wiki/The_Power_of_Ten
- **[NASA-STD-8739.8]** — NASA Software Safety Standard
- **[JPL-182]** — JPL Institutional Coding Standard
- **[ECSS-E-ST-40C]** — Space engineering — Software
- **[ECSS-Q-ST-80C]** — Space product assurance — Software product assurance
  (Rev.2, 30 Apr 2025)
- **[MISRA]** — MISRA C:2025 Guidelines for the use of the C language in
  critical systems

---

## 1. Coding standard & static analysis

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 1.1 | Adopt MISRA C:2025 as the coding standard; enforce with cppcheck + PC-lint/FlexeLint in CI | [MISRA], [ECSS-E-ST-40C] 5.5 (detailed design) | High | Med |
| 1.2 | Compiler flags: `-Wall -Wextra -Werror=implicit-function-declaration -Wdouble-promotion` | [JPL-182], [MISRA Dir 4.5] | High | Low |
| 1.3 | Enable `cppcheck --enable=all --std=c11` as a CI gate (already have GitHub Actions build) | [NASA-STD-8739.8] (verification), [ECSS-Q-ST-80C] 6.2.6.13 (independent V&V) | High | Low |
| 1.4 | No dynamic allocation after init; ban `malloc/free` inside tasks | [NASA-PoT] #5, [JPL-182] | High | Low (policy) |

> Note: the gcc 15 container already turned an implicit-function-declaration
> into a hard error that the CI (gcc 10) missed — see PR #5. Keeping
> `-Werror=implicit-function-declaration` closes that gap permanently.

## 2. Runtime protection (Cortex-M4 / FreeRTOS)

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 2.1 | **MPU**: configure regions to isolate kernel / task stacks / FRAM driver from app data; block execute on SRAM | [NASA-PoT] #4 (no pointer arithmetic on cast), [JPL-182] | Med | Med |
| 2.2 | **HardFault / MemManage / BusFault handlers** with register dump to LastStates + reboot — **DONE**: handlers in `Core/Src/faults.c` (`fault_log_*`, LastStates dump) wired to the Cortex-M fault vectors | [NASA-STD-8739.8] (fault containment), [ECSS-E-ST-40C] | High | Low |
| 2.3 | **Stack overflow hook** — **DONE**: `vApplicationStackOverflowHook` (`Core/Src/freertos.c:76-91`) already calls `fault_log_stack_overflow()` and reboots; `configCHECK_FOR_STACK_OVERFLOW=2` already set | [NASA-PoT], FreeRTOS `configCHECK_FOR_STACK_OVERFLOW=2` | High | Low |
| 2.4 | **Watchdog** — **DONE**: software monitor in `App/obsw/watchdog.c` (500 ms tick, flags silence >3× period); `watchdog_register_task()` is called from **7** task creators (`main.c:253` defaultTask, `comms.c:215/260`, `state_machine.c:426`, `clear.c:159`, `cloud.c:148`, `aocs.c:43`) and `watchdog_alive_self()` runs in every task loop. Hardware IWDG is **also active** (~31 s, `Core/Src/hw_watchdog.c` configures it, `main.c:115` inits, `watchdog.c`/main kick it) | [NASA-STD-8739.8] (watchdog/monitoring), [ECSS-E-ST-40C] | High | Med |
| 2.5 | **SRAM2 parity**: the 64 KB SRAM2 block has hardware parity (linker region `RAM2` @ 0x10000000). Place critical structures (state, comms buffers) there; enable parity error NMI | [NASA-STD-8739.8] (data integrity) | Med | Low |

## 3. Boot & image integrity

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 3.1 | **CRC32 of the firmware image** computed at build; verified at boot before jumping to `main()` | [ECSS-E-ST-40C] 5.4 (integrity), [NASA-STD-8739.8] | High | Low |
| 3.2 | **Dual-bank flash fallback** — **IMPLEMENTED BUT INHIBITED on this build**: the mechanism exists (`Core/Src/dual_bank.c`, BFB2 boot-from-bank-2, golden-image self-test) but `dual_bank.h:75-83` documents that the golden slot collides with the LastStates pool at the bank-2 base, so `DUAL_BANK_GOLDEN_SLOT_AVAILABLE` is hard-wired to `0` and BFB2 can never arm. The fallback is exercised in test but **not active in flight**. Must be re-enabled (relocate LastStates or shrink golden) before claiming graceful degradation | [NASA-STD-8739.8] (graceful degradation), [ECSS-Q-ST-80C] | High | Med |
| 3.3 | **Option bytes** read-back + lock (RDP level 1) to prevent readout; verify at boot | [ECSS-Q-ST-80C] | Med | Low |

## 4. Uplink / command validation

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 4.1 | Validate every telecommand: range-check parameters, whitelist source, reject malformed packets before dispatch | [NASA-PoT] #1 (no fixed-width int overflow), [NASA-STD-8739.8] (command authentication) | High | Med |
| 4.2 | Rate-limit / sequence-check uplink to resist replay (current "shuffle algorithm" is a start, document it) | [NASA-STD-8739.8] | Med | Med |
| 4.3 | Asserts on internal invariants (`assert_param` style) compiled in for debug builds | [JPL-182], [MISRA] | Med | Low |

## 5. Persistence & radiation (LEO, PocketQube)

| # | Recommendation | Standard | Priority | Effort | Status |
|---|----------------|----------|----------|--------|--------|
| 5.1 | **LastStates pool wear**: the ring erases the oldest 2 KB page **only on wrap** (`memory.c:884-895` — the Flash erase-on-wrap path), not on every entry rewrite — this is already the proposed mitigation. Optional: add a wear counter for telemetry | [ECSS-E-ST-40C] (reliability) | Med | Med | DONE (mitigation present) |
| 5.2 | **SEU mitigation**: periodically re-write critical RAM structures (state, config) from FRAM; use the SRAM2 parity NMI to detect corruption | [NASA-STD-8739.8] (fault tolerance) | Med | Med | **DONE (W2-5, T1.6 unified)** — a single subsystem: `Core/Src/seu_mitigation.c` (in-RAM redundant shadows in parity-protected SRAM2, 3-way vote/repair, PRIMASK NMI-safe lock, RTC backup-register event counter, escalation policy). The parallel `App/obsw/scrub.c` FRAM-golden-copy module was removed in T1.6 as redundant: the SRAM2 shadow already covers the same regions and the hardware parity covers the shadow itself. Docs `docs/dev/seu_mitigation.md`, `docs/dev/scrub.md` (removal changelog) |
| 5.3 | **FRAM**: already non-volatile and radiation-tolerant (FeRAM) — good choice; add a CRC per FRAM record | [ECSS-E-ST-40C] | Low | Low | **DONE (W2-5)** — no longer needed for SEU after T1.6 (shadows live in SRAM2). The cyclic science buffer in FRAM is still write-protected by the standard I²C CRC-on-write of the FM24VN10 driver. |

## 6. Cheap wins (do first)

1. ~~Fill `vApplicationStackOverflowHook` (2.3)~~ — **DONE** (`freertos.c:76-91`).
2. Add `-Werror=implicit-function-declaration` to `Makefile` (1.2) — already proven necessary (PR #5); verify it is actually set.
3. ~~CRC32 check at boot (3.1)~~ — **DONE**: implemented in `App/obsw/boot_crc.c`, verified at boot, build-stamped (see §Implemented status).
4. ~~Wire `watchdog_register_task`/`watchdog_alive` into every task (2.4)~~ — **DONE** (7 task creators register).
5. ~~HardFault handler with LastStates dump (2.2)~~ — **DONE** (`faults.c`).

## 7. Known stubs to finish (from source review)

The gap analysis found the following are **stubbed** in the current tree and
must be implemented before "space-ready" claims:

- `App/bms/` — subsystem SPI master to the EPS is initialised, but the
  telemetry transaction is still a stub (`TODO: query EPS MCU over subsystem
  SPI for BQ76905 telemetry`) and the EPS chip-select pin is unassigned
- `App/aocs/` — control law largely placeholder
- Comms encryption key handling marked TBD

Address these per the ECSS service alignment table in `docs/arch/README.md`.

---

## Implemented status & verification evidence

The items below are already realised in `main`; the roadmap numbering above
follows the PR descriptions (§2.x = image/data integrity, §3.x = execution
monitoring). This section is the post-implementation evidence trail; it does
not replace the roadmap but records what each item looks like on the OBC.

Standards drawn on: ECSS-E-ST-40C §5.4 (software integrity), ECSS-Q-ST-80C
§6.3.5 (post-mortem evidence), NASA-STD-8739.8 (fault detection and recovery).

### Evidence: boot-time firmware image CRC32 (roadmap §3.1)

`App/obsw/boot_crc.c` computes a pure-software CRC-32 (IEEE 802.3, reflected,
poly `0xEDB88320`, i.e. bit-identical to `zlib.crc32`) over

```
region = [ __fw_image_start , __fw_crc_start )    /* 0x08000000 .. end of image */
```

`main()` calls `boot_crc_verify()` before any RTOS object exists, then
`boot_crc_apply_policy()` immediately after `laststates_init()` (the fault has
to be persistable). No HAL/peripheral CRC unit is used, so the check cannot be
defeated by a mis-configured peripheral and has no clock dependency.

The expected CRC lives in the `.fw_crc` section, kept as the **last loaded
section** of the image by `STM32L496VGTX_FLASH.ld`, so it is always the final
four bytes of `build/JOS.bin`.

| Value        | Meaning                          | Firmware status      | Trusted |
|--------------|----------------------------------|----------------------|---------|
| `0x00000000` | placeholder, image never stamped | `BOOT_CRC_UNSTAMPED` | yes     |
| `0xFFFFFFFF` | erased Flash                     | `BOOT_CRC_ERASED`    | **no**  |
| other        | real stamp, must match           | `BOOT_CRC_OK` / `BOOT_CRC_MISMATCH` | match only |

`Error_Handler()` is **never** used for this: it is `__disable_irq(); while(1)`
and the IWDG (LSI-clocked, unmaskable) would simply reset the part in a loop
rather than halt it. RedPill carries **no golden image in flight** (the
dual-bank fallback is implemented yet inhibited — see §3.2), so "halt on
mismatch" is not an available safe state and the CRC policy degrades to
beacon-only.

### Evidence: task liveness monitoring (software watchdog, roadmap §2.4)

`App/obsw/watchdog.c` keeps a table of monitored tasks (`WDG_MAX_TASKS = 12`).
A task registers with the loop period it promises to honour; the monitor task
(500 ms tick, `osPriorityHigh`) flags any task silent for more than **3× its
declared period**.

| Task           | Registration site            | Declared period                    |
|----------------|------------------------------|------------------------------------|
| `defaultTask`  | `main.c` (created by CubeMX)  | `WDG_PERIOD_DEFAULT_TASK_MS` 1 s   |
| `stateMachine` | `state_machine_task_create()` | `WDG_PERIOD_STATE_MACHINE_MS` 100 ms |
| `loraBeacon`   | `lora_beacon_task_create()`   | bootstrap `BEACON_INTERVAL_MAX`, then the live cadence |
| `loraRX`       | `lora_rx_task_create()`       | `WDG_PERIOD_LORA_RX_MS` 100 ms     |
| `clear`        | `clear_task_create()`         | `WDG_PERIOD_CLEAR_MS` 1 s          |
| `cloud`        | `cloud_task_create()`         | `WDG_PERIOD_CLOUD_MS` 90 min       |
| `aocs`         | `aocs_task_create()`          | `WDG_PERIOD_AOCS_MS` 20 ms         |

**Only the first four are live today**: `clear_task_create()`,
`cloud_task_create()` and `aocs_task_create()` are not called from `main()`
yet (payload/AOCS bring-up pending), but they register on creation, so those
tasks are monitored from the moment they are enabled. The monitor task itself
is deliberately not monitored.

> Note: the hardware IWDG is **active** (~31 s, `Core/Src/hw_watchdog.c`, kicked
> from `main.c` and `watchdog.c`); the software monitor in `App/obsw/watchdog.c`
> runs alongside it. The monitor's reaction to a flagged task is still a
> `TODO` (`watchdog.c:217`: "log anomaly, optionally suspend/delete task"), so
> the dual-bank golden-image fallback is **not** the only recovery path still
> pending — closing that TODO is the other one.

### Verification

| Item | Evidence |
|------|----------|
| Image stamped in every artefact | `make all` depends on `crc-stamp`; CI re-runs `crc-stamp` + `crc-check` before upload |
| Flight CRC == stamping tool | `make crc-selftest` (KAT `0xCBF43926` + stamped image) |
| Unstamped / erased / corrupted image rejected | `crc-check` and `crc-selftest` exit 1 on all three (byte-flip, `0x00000000`, `0xFFFFFFFF`) |
| `.fw_crc` still last | `--crc-addr` cross-check against `nm __fw_crc_start` |

---

## References (primary sources)

- Holzmann, G.J. *The Power of Ten — Rules for Developing Safety Critical Code*,
  IEEE Computer, 2006. https://en.wikipedia.org/wiki/The_Power_of_Ten
- NASA-STD-8739.8 — NASA Software Safety Standard.
- JPL Institutional Coding Standard (JPL-182).
- ECSS-E-ST-40C — Space engineering — Software.
- ECSS-Q-ST-80C Rev.2 (30 Apr 2025) — Space product assurance — Software
  product assurance.
- MISRA C:2025 — Guidelines for the use of the C language in critical systems.
- STM32L496xx Reference Manual — dual-bank flash (DUALBANK), SRAM2 parity.
