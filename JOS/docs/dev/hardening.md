# Fault-tolerance hardening

Status of the hardening items implemented in the OBSW, with the rationale and
the verification evidence for each. Numbering follows the hardening roadmap
used in the PR descriptions (§2.x = image/data integrity, §3.x = execution
monitoring).

Standards drawn on: ECSS-E-ST-40C §5.4 (software integrity), ECSS-Q-ST-80C
§6.3.5 (post-mortem evidence), NASA-STD-8739.8 (fault detection and recovery).

---

## 2.4 Boot-time firmware image CRC32

### What runs on the OBC

`App/obsw/boot_crc.c` computes a pure-software CRC-32 (IEEE 802.3, reflected,
poly `0xEDB88320`, i.e. bit-identical to `zlib.crc32`) over

```
region = [ __fw_image_start , __fw_crc_start )    /* 0x08000000 .. end of image */
```

`main()` calls `boot_crc_verify()` before any RTOS object exists, then
`boot_crc_apply_policy()` immediately after `laststates_init()` (the fault has
to be persistable). No HAL/peripheral CRC unit is used, so the check cannot be
defeated by a mis-configured peripheral and has no clock dependency.

### The stored word and its sentinels

The expected CRC lives in the `.fw_crc` section, kept as the **last loaded
section** of the image by `STM32L496VGTX_FLASH.ld`, so it is always the final
four bytes of `build/JOS.bin`.

| Value        | Meaning                          | Firmware status      | Trusted |
|--------------|----------------------------------|----------------------|---------|
| `0x00000000` | placeholder, image never stamped | `BOOT_CRC_UNSTAMPED` | yes     |
| `0xFFFFFFFF` | erased Flash                     | `BOOT_CRC_ERASED`    | **no**  |
| other        | real stamp, must match           | `BOOT_CRC_OK` / `BOOT_CRC_MISMATCH` | match only |

The placeholder is deliberately **not** the erased-Flash pattern. If it were,
a CRC word that had been erased or had decayed would make a corrupted image
report "nothing to check" — i.e. corruption would *upgrade* the verdict.
`tools/fw_crc_stamp.py` also refuses to write a computed CRC that happens to
equal either sentinel.

### Stamping (mandatory, enforced by the build)

Nothing stamps the image at compile time — the word has to be patched
post-link:

```sh
make all          # links, objcopies, then stamps (all depends on crc-stamp)
make crc-stamp    # nm-derived __fw_crc_start -> tools/fw_crc_stamp.py --verify
make crc-check    # read-only gate: unstamped / erased / inconsistent => fail
make crc-selftest # host build of the flight routine, run over the .bin
```

* `crc-stamp` derives the expected address of the CRC word from the ELF symbol
  table (`arm-none-eabi-nm ... __fw_crc_start`) and passes it as `--crc-addr`.
  The tool cross-checks it against the actual last word of the `.bin`, so a
  linker-script regression that stops `.fw_crc` being last fails the build
  instead of shipping an image that can never verify.
* `crc-selftest` compiles `boot_crc32()` itself for the host
  (`-DBOOT_CRC_HOST_BUILD`, `tools/crc_selftest.c`) and runs it over the
  stamped artefact plus the `"123456789"` → `0xCBF43926` known-answer vector.
  This is what proves the on-orbit routine and the zlib-based stamping tool
  cannot silently diverge.
* CI (`.github/workflows/build.yml`) runs `make all`, `make crc-stamp`,
  `make crc-check` and `make crc-selftest` **before** the artefact upload, so a
  published artefact is always stamped and self-consistent.

### Fault policy — the safe state

`BOOT_CRC_FATAL` is defined to `1` by the Makefile (`C_DEFS`) and defaults to
`1` in `boot_crc.h`, so the fault path can never be compiled out by accident.
On `BOOT_CRC_MISMATCH`, `BOOT_CRC_ERASED` or `BOOT_CRC_BAD_REGION`:

1. **Record.** A `laststates_entry_t` with trigger `TRIGGER_IMAGE_CRC_FAIL`
   (context: status, expected, computed, region length, attempt number) is
   written to the LastStates pool, so the fault is downlinkable through the
   existing dump path even after the reset.
2. **Recover.** `NVIC_SystemReset()`, up to `BOOT_CRC_MAX_RESET_ATTEMPTS` (2)
   times, to clear a transient (SEU-induced) corruption of the Flash read
   path. The attempt counter lives in the `.noinit` RAM region — the startup
   code only zeroes `[_sbss,_ebss)`, so it survives the warm reset — and is
   guarded by a magic word so a cold start does not inherit garbage.
3. **Degrade, do not brick.** Once the retry budget is exhausted the OBC
   *continues to boot* with the image marked untrusted:
   `boot_crc_image_trusted()` returns 0, and `state_machine.c` then rejects
   every transition except to `STATE_CRIT` (and the `STATE_INIT` boot
   bookkeeping). The satellite is beacon-only with payloads inhibited, which
   is exactly the condition ground needs in order to diagnose and re-upload.

`Error_Handler()` is **never** used for this: it is `__disable_irq(); while(1)`
and the IWDG is not configured, so it would be an unrecoverable brick. RedPill
carries no golden image and no bootloader, so "halt on mismatch" is not an
available safe state.

An unstamped bench build (`0x00000000`) is trusted, so a debugger session on a
freshly flashed `.elf` behaves normally; CI can never produce such an artefact
because of the gates above.

### Telemetry

`boot_crc_get_status() / _computed() / _expected() / _region_len() /
_reset_attempts()` expose the latched result for the beacon and housekeeping
packets.

---

## 3.1 Task liveness monitoring (software watchdog)

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

Tasks signal liveness with `watchdog_alive_self()` (a wrapper around
`watchdog_alive(osThreadGetId())`) at the top of their loop.

`watchdog_register_task()` refuses a NULL handle, a zero period, or a call
before `watchdog_monitor_init()` — it returns `-1` rather than pretending a
task is covered. Registering a handle that is already present takes the
**duplicate-refresh path**: the existing slot is updated in place and its
`last_tick` is reset, so no slot leaks and a period change cannot false-flag
the task.

> Not yet implemented: the hardware IWDG. `watchdog_kick()` in
> `state_machine.c` is still a stub, and the monitor's reaction to a flagged
> task is a `TODO` (log/suspend). Both are tracked separately from this
> document's §2.4/§3.1 items.

### Beacon cadence and the watchdog

The beacon period is state-dependent (1–16 min) and can be retargeted from
ground with `CMD_SET_BEACON_INTERVAL`. Two things keep that safe:

* **Validation.** `comms_dispatch_command()` requires a non-NULL 4-byte
  argument and hands the value to `state_machine_set_beacon_interval()`, which
  accepts `0` (clear the override) or a value inside
  `[BEACON_INTERVAL_MIN (10 s), BEACON_INTERVAL_MAX (16 min)]`. Anything else
  is **rejected** (returns `-1`) and the current cadence is left intact — an
  erroneous or corrupted uplink can neither silence the beacon nor hammer the
  RF chain. Values are never silently clamped, so a bad telecommand is visible
  as a failure.
* **Tracking.** `lora_beacon_task()` re-registers its own handle whenever the
  effective interval changes, using the duplicate-refresh path above. The
  monitor therefore always watches the cadence actually in force instead of a
  worst-case period registered once at creation (which would leave it blind
  for up to 3 × 16 min while the beacon was supposed to run every minute).
  `BEACON_INTERVAL_MAX` bounds the worst-case detection time by construction.

---

## Verification

| Item | Evidence |
|------|----------|
| Image stamped in every artefact | `make all` depends on `crc-stamp`; CI re-runs `crc-stamp` + `crc-check` before upload |
| Flight CRC == stamping tool | `make crc-selftest` (KAT `0xCBF43926` + stamped image) |
| Unstamped / erased / corrupted image rejected | `crc-check` and `crc-selftest` exit 1 on all three (byte-flip, `0x00000000`, `0xFFFFFFFF`) |
| `.fw_crc` still last | `--crc-addr` cross-check against `nm __fw_crc_start` |
| `.noinit` survives reset | placed after `_ebss` (`objdump -h`: NOBITS, outside the loaded image) |
| Beacon interval validation | out-of-range and malformed `CMD_SET_BEACON_INTERVAL` rejected in `state_machine_set_beacon_interval()` |
