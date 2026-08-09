# Building the RedPill OBSW

This guide covers how to compile and link the RedPill (JOS) on-board software
for the STM32L496VGTx target.

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| ARM GCC toolchain | `gcc-arm-none-eabi` ≥ 10 (CI uses 10.3; build container uses 15.2) | `arm-none-eabi-gcc`, `arm-none-eabi-ld`, `arm-none-eabi-size`, `arm-none-eabi-objcopy` |
| GNU Make | any 4.x | drives the `Makefile` at `JOS/` root |
| Git | any | for source checkout |

> **Warning — toolchain strictness:** GCC 15 treats implicit-function-declaration
> as a hard error; GCC 10 only warns. A build that is green on CI (GCC 10) may
> fail locally on GCC 15. Keep prototypes declared (see `App/comms/comms.h`).

## Warning / hardening flags

`JOS/Makefile` compiles every translation unit with:

| Flag | Applies to | Why |
|------|-----------|-----|
| `-Wall -Wextra` | C and C++ | Maximum practical diagnostic coverage (NASA Power of Ten rule 10, JPL-182, ECSS-E-ST-40C). |
| `-Wdouble-promotion` | C and C++ | The Cortex-M4F FPU is single precision; silent `float -> double` promotion falls back to soft-float. |
| `-Werror=implicit-function-declaration` | C only | Makes every toolchain fail the way GCC >= 14 does, so a missing prototype cannot pass CI on GCC 10. `cc1plus` rejects the option, so it is not passed to C++. |

`-Werror` is deliberately **not** enabled globally: the vendored trees (STM32
HAL, FreeRTOS, RadioLib) emit warnings we do not own, and hard-failing on them
would block the build for reasons unrelated to flight-software quality.

## Static analysis (cppcheck)

### Running it

The gate has **one** definition, the `cppcheck` target in `JOS/Makefile`. CI
calls exactly the same target, so the workflow and a local run cannot drift:

```bash
make -C JOS cppcheck          # the gate itself   -> exit 1 on any finding
make -C JOS cppcheck-canary   # self-test: the gate must reject a buggy file
make -C JOS cppcheck-print    # print the fully expanded cppcheck command line
```

The include paths and defines are derived from the `$(INCLUDES)` and
`$(CPPCHECK_DEFS)` variables the compiler already uses — `CPPCHECK_DEFS` is the
deduplicated union of `$(C_DEFS)` and `$(CXX_DEFS)`, so a C++ translation unit
is never analysed under the C macro set — and adding a header directory to the
build automatically adds it to the analysis.

`--enable` is `warning,style,performance,portability,information`, deliberately
not `all`: the two ids `all` adds on top (`unusedFunction`, `missingInclude`)
are suppressed anyway. The `error` severity — `arrayIndexOutOfBounds`,
`nullPointer`, `uninitvar`, … — is always on and is not part of `--enable`, so
this cannot weaken the gate.

### Pinned version

`CPPCHECK_VERSION` in `JOS/Makefile` pins the analyser (currently **2.13.0**,
the version shipped by the pinned `ubuntu-24.04` CI runner). `make cppcheck`
refuses to run against any other version, because the finding set is
version-dependent — for example `constParameter` was split into
`constParameterPointer` in cppcheck 2.11+. To upgrade: bump both
`CPPCHECK_VERSION` here and `PINNED_DEB`/`PINNED_VERSION` in the *Install
pinned cppcheck* step of `.github/workflows/build.yml`, re-run the gate, and
triage the new findings in the same PR.

That CI step is a three-step fallback chain — exact Ubuntu revision, then any
revision of the same upstream version, then a source build of the upstream tag
— so one archive change cannot hard-block every PR, and it asserts the
resulting `cppcheck --version` afterwards so a fallback can never smuggle in a
different analyser.

Install it locally with `sudo apt-get install cppcheck=2.13.0-2ubuntu3` on
Ubuntu 24.04, or override the pin for a one-off experiment:

```bash
make -C JOS cppcheck \
  CPPCHECK_VERSION=$(cppcheck --version | grep -oE '[0-9]+(\.[0-9]+)+' | head -n 1)
```

### Scope

| Tree | In the analysis? | Why |
|------|------------------|-----|
| `JOS/App` | **yes** | first-party flight software |
| `JOS/Core/Src` | **yes** | hand-written CubeMX sources |
| `JOS/Core/Src/{asm330lhh_reg,syscalls,sysmem,system_stm32l4xx}.c` | no (`-i`) | vendored ST driver / generated newlib stubs |
| `JOS/Drivers` (STM32 HAL, CMSIS) | **no** — `-I` only | vendored, not ours |
| `JOS/Middlewares` (FreeRTOS) | **no** — `-I` only | vendored, not ours |
| `JOS/Core/Inc/RadioLib` | **no** — `-I` only | vendored, not ours |

The vendored trees are on the include path so first-party headers resolve, but
they are never analysed; their findings are silenced with **path-scoped**
suppressions (`--suppress=*:JOS/Drivers/*` and friends). No check id is
disabled repo-wide, so `variableScope`, `constParameter`,
`arrayIndexOutOfBounds`, `nullPointer`, `uninitvar`, … all still apply to
first-party code.

The single exception is `constParameterPointer` on
`Core/Src/stm32l4xx_hal_msp.c` (file-scoped, one id): the HAL MSP callbacks are
weak overrides whose signatures are fixed by `<stm32l4xx_hal_*.h>`, so adding
`const` would conflict with the HAL prototype, and the file is CubeMX-generated
so an inline suppression outside a `USER CODE` block would be regenerated away.
Every other check still runs on that file.

### Why the extra flags matter

| Flag | Why |
|------|-----|
| `-D__GNUC__=10` | `Drivers/CMSIS/Include/cmsis_compiler.h` ends in `#error Unknown compiler` when no compiler macro is defined. Without this, cppcheck abandons the configuration and **exits 0 having analysed nothing**. |
| `-I…/FreeRTOS/Source/portable/GCC/ARM_CM4F` | same path the `Makefile` passes to GCC; without it `portmacro.h` is unresolvable. |
| `--platform=arm32-wchar_t4` | the target is 32-bit ARM ILP32; the default native (x86-64 LP64) model gives wrong `sizeof()`/overflow reasoning. |
| no `--suppress=preprocessorErrorDirective` | that suppression hides exactly the `#error` abort above and makes the gate vacuous. |
| `--suppress=missingInclude` | information-level noise here, not a signal: it fires for the vendored trees we only `-I`, and a genuinely unresolvable *first-party* header is already a hard error in the `build` job, which compiles every translation unit. Not to be confused with `preprocessorErrorDirective`, which stays un-suppressed. |

### The canary

`make -C JOS cppcheck-canary` generates a throwaway C file containing an
out-of-bounds array write and a NULL dereference, runs cppcheck over it with
the **exact same flags** as the real gate, deletes the file, and fails unless
cppcheck both exits non-zero **and** actually names `arrayIndexOutOfBounds` and
`nullPointer` in its output. Checking the exit code alone would accept a
broken-but-noisy analyser — a bad flag or an unreadable include path exits
non-zero too — so the canary asserts the findings, not the exit status.
It `#include`s `main.h` and `FreeRTOS.h` so it walks
the same HAL/CMSIS/FreeRTOS include chain as real code — which is what makes it
reproduce (and therefore catch) the "aborted configuration, exit 0" regression
class. CI runs it *before* the gate itself, so a green gate is never trusted
without first proving the gate can go red.

## Build (canonical — NixOS container + CI)

The reference build environment is a dedicated NixOS container (`josbuilder`)
with the ARM toolchain installed, reachable from the dev machine over Tailscale.
This isolates the build from the desktop and matches CI exactly.

```bash
# On the dev machine (Pi / laptop), hop into the container:
ssh -A nicola@nixos "ssh -A root@10.250.0.2"
cd /srv/josbuilder/JOS/JOS
make all
```

Artifacts land in `JOS/build/`:

| File | Purpose |
|------|---------|
| `JOS.elf` | Linked executable (for GDB / `arm-none-eabi-size`) |
| `JOS.bin` | Raw binary for flashing |
| `JOS.hex` | Intel HEX for flashing |
| `JOS.map` | Linker map (for memory analysis) |

Verify size budget:

```bash
arm-none-eabi-size build/JOS.elf
# text + data  must be < 1,048,576  (1 MB Flash, 512K reserved for firmware)
# bss  + data  must be <   327,680  (320 KB SRAM)
```

## Build (local — any Linux/macOS with the toolchain)

```bash
cd JOS
make all
arm-none-eabi-size build/JOS.elf
```

## Build (CI — GitHub Actions)

Pushing to `main` or opening a PR triggers `.github/workflows/build.yml`, which
runs two independent jobs:

| Job | Runner | What it does |
|-----|--------|--------------|
| `static-analysis` | `ubuntu-24.04` (pinned) | installs the pinned cppcheck, runs `make -C JOS cppcheck-canary` then `make -C JOS cppcheck` |
| `build` | `ubuntu-latest` | installs `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi` and runs `make all` in `JOS/`; uploads `JOS.elf/.bin/.hex` |

Together these are the authoritative gate.

## Build (alternative — STM32CubeIDE)

CubeIDE can also build the project: it bundles the same ARM GCC and drives the
generated `Makefile`. Open the `.ioc` in CubeIDE, **Generate Code**, then build
the Debug configuration. The command-line equivalent (if CubeIDE generated a
`STM32CubeIDE/Debug` tree) is `make -C STM32CubeIDE/Debug all`.

> CubeIDE is **optional** — the canonical path above (container / local `make
> all` / CI) does not require it. Keep CubeIDE only if you prefer the IDE
> workflow or need the RTOS Viewer debugger panel.

## Flashing

```bash
# Via OpenOCD + ST-LINK (requires the toolchain + openocd):
make -C JOS flash        # if a flash target exists; otherwise use STM32CubeIDE / STM32CubeProgrammer
```

The LastStates pool is reserved at `0x08080000` (8 KB) — never overwrite it
with the application image (see `JOS/STM32L496VGTX_FLASH.ld`).
