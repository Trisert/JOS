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

CI runs `cppcheck --enable=all --std=c11 --error-exitcode=1` over the
first-party sources (`JOS/App` and the hand-written `JOS/Core/Src` files).
Vendored/generated code (`asm330lhh_reg.c`, `syscalls.c`, `sysmem.c`,
`system_stm32l4xx.c`, RadioLib, HAL, FreeRTOS) is excluded so findings stay
actionable. Reproduce locally with the same command as in
`.github/workflows/build.yml`, run from the repository root.

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

Pushing to `main` or opening a PR triggers `.github/workflows/build.yml`,
which installs `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi` on
`ubuntu-latest` and runs `make all` in `JOS/`. Artifacts (`JOS.elf/.bin/.hex`)
are uploaded on success. This is the authoritative build gate.

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
