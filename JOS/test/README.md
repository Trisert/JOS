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

Current numbers (gcovr, `App/obsw` + `App/memory` only):

```
lines:     93.5% (116 / 124)
functions: 100.0% (22 / 22)
branches:  77.6% (45 / 58)
```

(`boot_crc.c`'s uncovered lines in the `test_boot_crc` executable are the
`BOOT_CRC_BAD_REGION` block, which is covered by the separate
`test_bad_region` executable — gcov reports the two executables separately.)

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

4. **The FM24VN10-G FRAM behind `hi2c2`** — the doubles accept only the *8-bit*
   (already left-shifted) device addresses the STM32 HAL expects: `0xA0`,
   `0xA2`, `0xA4`, `0xA6`. The raw 7-bit values `0x50..0x53` are rejected, so a
   driver that forgets the shift fails the tests instead of silently addressing
   the wrong device on the real bus.

## References

* ECSS-E-ST-40C §5.5 — software validation
* NASA-STD-8739.8 — software assurance / unit-test evidence
* JPL-182 Rule 31 — all code exercised by tests
