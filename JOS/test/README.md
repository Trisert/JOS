# Host unit tests (Ceedling + Unity)

Host-side unit tests for the RedPill / JOS flight software. They compile with
the **native** gcc and run on the build machine; the flight build
(`arm-none-eabi-gcc`, see `../Makefile`) is untouched by anything in this
directory.

## Running

```sh
gem install ceedling          # once; Unity and CMock come with the gem
pipx install gcovr            # once; required by `ceedling gcov:all`
cd JOS/test && ceedling test:all
```

or from `JOS/`:

```sh
make test        # ceedling test:all
make coverage    # ceedling gcov:all
```

CI runs the same command in the `unit-tests` job of
`.github/workflows/build.yml`.

## Unity provenance

Unity and CMock are used **only** from the Ceedling gem
(`:project: :which_ceedling: gem` in `project.yml`). There is deliberately no
`vendor/unity` directory in this repository: two Unity versions in one build
silently disagree about assertion macros and the `UNITY_*` configuration, and
the resulting failures look like flight-code bugs. One version, from one place.

## Layout

| Path | Contents |
|---|---|
| `project.yml` | Ceedling configuration |
| `test/` | test suites (`test_*.c`) |
| `support/` | hand-written host doubles, linked into every test executable |
| `fakes/` | minimal stand-ins for target-only headers (`main.h` -> HAL) |
| `build/` | generated; git-ignored |

## Suites

| Suite | Under test | Notes |
|---|---|---|
| `test_boot_crc.c` | `App/obsw/boot_crc.c` | CRC-32 known-answer vectors, `boot_crc_verify()` OK / MISMATCH / UNSTAMPED, all four latching accessors |
| `test_bad_region.c` | `App/obsw/boot_crc.c` | the `BOOT_CRC_BAD_REGION` guard, built with `-DHOST_FW_BAD_REGION` |
| `test_laststates.c` | `App/memory/memory.c` | LastStates Flash round trip, wrap/erase protocol, FRAM + cyclic buffer |
| `test_state_machine.c` | `App/obsw/state_machine.c` | transitions via the public API (INIT/OFF rules, CRIT recovery SoC-gated, boot-CRC + parity confinement, LastStates-refusal, beacon cadence), plus the task boot sequence + autonomous loop through the captured entry point |
| `test_temp.c` | `App/payloads/temp.c` | emulator + flight PB2 backend via the GPIO doubles, no-device triplet, mid-read reset loss, stuck-conversion timeout |
| `test_comms.c` | `App/comms/comms.c` | validation gate, dispatch, RX accounting, plus `lora_send_chunked()` radio/timeout aborts (incl. mid-transfer) via the radio failure injection |
| `test_comms_legacy.c`, `test_tec.c` | Comms / TEC contracts | Legacy framing and command registry behavior; TT&C frame cases are part of the current comms tests |
| `test_spi_dma.c` | SPI DMA scheduler helpers | Chunk boundaries, timeout calculation, and completion/error behavior |
| `test_watchdog.c` | `App/obsw/watchdog.c` | Host-side monitor policy; target task-suspend/Flash backends are not exercised |
| `test_eps_fdir.c` | `App/bms/eps_fdir.c` | Pure FDIR decision logic with caller-supplied snapshots, not live EPS telemetry |
| `test_aocs_contract.c` | AOCS data contract | Declared telemetry contract; no production AOCS SPI driver is tested |
| `test_beacon.c` | Beacon buffer/encoding contracts | Host-side beacon behavior only |
| `test_deploy_sense.c`, `test_thermal_guard.c` | Deployment and thermal guards | Helper behavior with host doubles; not hardware qualification |
| `test_memory_faults.c` | Memory fault paths | Injected host failures; not target Flash/FRAM electrical behavior |
| `test_boot_policy.c`, `test_boot_unstamped_bench.c` | Boot CRC policy | Host policy cases for flight and bench configurations |

## Coverage gate

`ceedling gcov:all` is an **enforced** gate, not a report. `project.yml` sets

```yaml
:fail_under_line:   90
:fail_under_branch: 75
:exception_on_fail: TRUE     # without this the plugin only warns and exits 0
```

so the command exits non-zero when coverage of the modules under test drops
below those numbers. It needs `gcovr` on `PATH`; CI installs it explicitly and
prints its version, because a missing gcovr silently degrades the gate.

Do not use a historical count or percentage as a current pass claim. The test
inventory and coverage change with the tree; run the commands above and report
the commit, actual result, tool versions, and measured coverage together. A
previous snapshot in this document was stale and has been removed.

Only host-compilable modules are on the Ceedling source path. `../Core/Src` is
deliberately **not**: it is CubeMX-generated and would let `main.c` be fed to
the host gcc for any test that includes `main.h` (the tests use `fakes/main.h`).

## How the target-only bits are faked

Three things normally only exist after linking for the STM32L496VGTx:

1. **`__fw_image_start` / `__fw_crc_start`** — supplied by
   `STM32L496VGTX_FLASH.ld`. `support/stubs.c` defines a single 64-byte
   `const uint8_t` array and declares `__fw_crc_start` as a GNU-as symbol at
   its one-past-the-end address. Both pointers therefore address the *same*
   object, so the `__fw_crc_start - __fw_image_start` subtraction in
   `boot_crc_verify()` is well defined (C11 6.5.6p9), and the types match the
   `extern const uint8_t []` declarations in `boot_crc.c` exactly.

2. **`fw_crc_stored`** — the word in the `.fw_crc` section that
   `tools/fw_crc_stamp.py` patches post-build. `host_fw_crc_stamp()` performs
   the same patch at run time. It is reached through a *weak* reference so the
   support file still links into test executables that do not include
   `boot_crc.c`.

3. **The LastStates pool at `0x08080000`** — `App/memory/memory.c` addresses it
   by absolute address (`laststates_dump_all()` memcpy's straight from
   `0x08080000`), so `support/host_flash.c` puts real writable pages there with
   `mmap(MAP_FIXED_NOREPLACE)` and reproduces NOR-Flash behaviour: erased is
   `0xFF`, a double-word can only be programmed once per erase cycle, and both
   programming *and* page erase require the Flash to be unlocked (RM0351 3.3.5
   — PG and PER/STRT are both in the write-protected FLASH_CR). That is what
   makes the erase-before-wrap path in `laststates_write()` genuinely testable.

4. **The FM24VN10-G FRAM behind `hi2c1` (PB8/PB9)** — the doubles accept only the *8-bit*
   (already left-shifted) device addresses the STM32 HAL expects: `0xA0`,
   `0xA2`, …, `0xAE` (4 chips × 2 A16 pages). The raw 7-bit values `0x50..0x57`
   are rejected, so a driver that forgets the shift fails the tests instead of
   silently addressing the wrong device on the real bus.

## References

* ECSS-E-ST-40C §5.5 — software validation
* NASA-STD-8739.8 — software assurance / unit-test evidence
* JPL-182 Rule 31 — all code exercised by tests
