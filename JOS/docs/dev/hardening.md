# Software Hardening Roadmap — RedPill OBSW

Goal: bring the RedPill (JOS) on-board software to a hardened, space-ready /
QM-ready state for ESA Fly Your Satellite! 4. Target: STM32L496VGTx
(Cortex-M4 @ 80 MHz, 1 MB dual-bank flash, 320 KB SRAM — 256 KB SRAM1 +
64 KB SRAM2 with parity, 4 MB FRAM).

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
| 2.2 | **HardFault / MemManage / BusFault handlers** with register dump to LastStates + reboot | [NASA-STD-8739.8] (fault containment), [ECSS-E-ST-40C] | High | Low |
| 2.3 | **Stack overflow hook**: `vApplicationStackOverflowHook` currently empty — fill it to flag + record the offending task, then safe-reboot | [NASA-PoT], FreeRTOS `configCHECK_FOR_STACK_OVERFLOW=2` already set | High | Low |
| 2.4 | **Watchdog**: `watchdog_register_task` / `watchdog_alive` exist but have **no callers** in the current source — wire every task to register + periodicaly kick; keep IWDG ~32 s as backstop | [NASA-STD-8739.8] (watchdog/monitoring), [ECSS-E-ST-40C] | High | Med |
| 2.5 | **SRAM2 parity**: the 64 KB SRAM2 block has hardware parity (linker region `RAM2` @ 0x10000000). Place critical structures (state, comms buffers) there; enable parity error NMI | [NASA-STD-8739.8] (data integrity) | Med | Low |

## 3. Boot & image integrity

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 3.1 | **CRC32 of the firmware image** computed at build; verified at boot before jumping to `main()` | [ECSS-E-ST-40C] 5.4 (integrity), [NASA-STD-8739.8] | High | Low |
| 3.2 | **Dual-bank flash fallback**: STM32L496 has dual-bank (option byte `DUALBANK`). Keep a known-good golden image in bank 2; on CRC fail or repeated boot fault, boot bank 2 | [NASA-STD-8739.8] (graceful degradation), [ECSS-Q-ST-80C] | High | Med |
| 3.3 | **Option bytes** read-back + lock (RDP level 1) to prevent readout; verify at boot | [ECSS-Q-ST-80C] | Med | Low |

## 4. Uplink / command validation

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 4.1 | Validate every telecommand: range-check parameters, whitelist source, reject malformed packets before dispatch | [NASA-PoT] #1 (no fixed-width int overflow), [NASA-STD-8739.8] (command authentication) | High | Med |
| 4.2 | Rate-limit / sequence-check uplink to resist replay (current "shuffle algorithm" is a start, document it) | [NASA-STD-8739.8] | Med | Med |
| 4.3 | Asserts on internal invariants (`assert_param` style) compiled in for debug builds | [JPL-182], [MISRA] | Med | Low |

## 5. Persistence & radiation (LEO, PocketQube)

| # | Recommendation | Standard | Priority | Effort |
|---|----------------|----------|----------|--------|
| 5.1 | **LastStates pool wear**: erase uses STM32L4 **pages** (correct) but each entry rewrite erases a page — add a simple wear counter / rotate entries across pages to extend endurance | [ECSS-E-ST-40C] (reliability) | Med | Med |
| 5.2 | **SEU mitigation**: periodically re-write critical RAM structures (state, config) from FRAM; use the SRAM2 parity NMI to detect corruption | [NASA-STD-8739.8] (fault tolerance) | Med | Med |
| 5.3 | **FRAM**: already non-volatile and radiation-tolerant (FeRAM) — good choice; add a CRC per FRAM record | [ECSS-E-ST-40C] | Low | Low |

## 6. Cheap wins (do first)

1. Fill `vApplicationStackOverflowHook` (2.3) — Low effort, catches a real gap.
2. Add `-Werror=implicit-function-declaration` to `Makefile` (1.2) — already proven necessary (PR #5).
3. CRC32 check at boot (3.1) — Low effort, high value.
4. Wire `watchdog_register_task`/`watchdog_alive` into every task (2.4) — closes a stub.
5. HardFault handler with LastStates dump (2.2) — Low effort, essential for forensics.

## 7. Known stubs to finish (from source review)

The gap analysis found the following are **stubbed** in the current tree and
must be implemented before "space-ready" claims:

- `App/bms/` — EPS SPI slave interface (`TODO: init subsystem SPI master`)
- `App/aocs/` — control law largely placeholder
- Comms encryption key handling marked TBD
- Watchdog registration callers absent

Address these per the ECSS service alignment table in `docs/arch/README.md`.

---

## Implemented status & verification evidence

The items below are already realised in `main`; the roadmap numbering above
follows the PR descriptions (§2.x = image/data integrity, §3.x = execution
monitoring). This section is the post-implementation evidence trail; it does
not replace the roadmap but records what each item looks like on the OBC.

Standards drawn on: ECSS-E-ST-40C §5.4 (software integrity), ECSS-Q-ST-80C
§6.3.5 (post-mortem evidence), NASA-STD-8739.8 (fault detection and recovery).

### 2.4 / 3.1 Boot-time firmware image CRC32

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
and the IWDG is not configured, so it would be an unrecoverable brick. RedPill
carries no golden image and no bootloader, so "halt on mismatch" is not an
available safe state.

### 3.1 Task liveness monitoring (software watchdog)

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

> Not yet implemented: the hardware IWDG. `watchdog_kick()` in
> `state_machine.c` is still a stub, and the monitor's reaction to a flagged
> task is a `TODO` (log/suspend). Both are tracked separately from this
> document's roadmap items.

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
