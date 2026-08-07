# Host unit tests (Ceedling + Unity)

Host-side unit tests for the RedPill / JOS flight software. They compile with
the **native** gcc and run on the build machine; the flight build
(`arm-none-eabi-gcc`, see `../Makefile`) is untouched by anything in this
directory.

## Running

```sh
gem install ceedling          # once; Unity and CMock come with the gem
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

Coverage of the modules under test (`ceedling gcov:all`):

```
boot_crc.c | Lines executed: 90.00% of 30   Branches executed: 100.00% of 10
memory.c   | Lines executed: 91.11% of 90   Branches executed: 100.00% of 46
```

(`boot_crc.c`'s remaining 10% is the `BOOT_CRC_BAD_REGION` block, which is
covered by the separate `test_bad_region` executable — gcov reports the two
executables separately.)

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
   `0xFF`, a double-word can only be programmed once per erase cycle, and the
   Flash must be unlocked. That is what makes the erase-before-wrap path in
   `laststates_write()` genuinely testable.

## References

* ECSS-E-ST-40C §5.5 — software validation
* NASA-STD-8739.8 — software assurance / unit-test evidence
* JPL-182 Rule 31 — all code exercised by tests
