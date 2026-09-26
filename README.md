# JOS: RedPill On-Board Software

JOS is the flight on-board software (OBSW) of the **RedPill** PocketQube. It
runs on the on-board computer (OBC), an STM32L496VGTx (Arm Cortex-M4F at 80 MHz),
on FreeRTOS through the CMSIS-RTOS v2 API.

This software is intended for flight. The repository is therefore organised
around explicit verification: every change is gated by a firmware build,
static analysis, host unit tests with an enforced coverage threshold, and
CodeQL.

## Scope

The OBSW is responsible for:

- **Operational state machine.** Five states: OFF, INIT, CRIT, READY and
  ACTIVE. Transitions are driven by deployment, battery state of charge and
  ground commands, and every transition is logged.
- **TT&C.** A LoRa link on a Semtech SX1268. It covers periodic beacons,
  chunked downlink and an authenticated uplink.
- **Persistent storage:**
  - the LastStates forensic log, a Flash ring at `0x08080000`;
  - 512 KB of external FRAM (4 x FM24VN10-G) for payload data and system state.
- **Fault tolerance:**
  - task liveness monitoring, which records and resets on a stall;
  - the independent and external hardware watchdogs;
  - a boot image CRC and a dual-bank golden-image fallback;
  - MPU isolation, SRAM2 parity and periodic RAM scrubbing against
    single-event upsets.
- **Payload commanding** and consumption of the telemetry published by the
  EPS and AOCS boards. EPS and AOCS are separate microcontrollers, slaves on
  the subsystem SPI bus; the OBC does not run attitude control.

## Repository layout

| Path | Contents |
|---|---|
| `JOS/App/` | Application modules: `obsw/`, `comms/`, `memory/`, `bms/`, `aocs/`, `payloads/` |
| `JOS/Core/` | STM32CubeMX-generated sources and the hand-written platform layer (boot, faults, watchdog, dual bank, SEU mitigation) |
| `JOS/test/` | Host unit tests (Ceedling, Unity, CMock) with the doubles they use |
| `JOS/docs/` | Module contracts (`api/`), architecture (`arch/`), developer guides (`dev/`), operator manual (`user/`) |
| `JOS/simulation/` | Dual-ESP32 hardware-in-the-loop harness (development aid) |
| `JOS/tools/` | Image CRC stamping and self-test utilities |
| `JOS/Drivers/`, `JOS/Middlewares/`, `JOS/Core/Inc/RadioLib/` | Vendored third-party code (STM32 HAL, CMSIS, FreeRTOS, RadioLib) |

## Building

### Prerequisites

- Arm GNU Toolchain (`arm-none-eabi-gcc`). CI pins **14.3.Rel1**, the
  STM32CubeIDE 2.x reference, and CI is the authoritative build. The Nix flake
  (`nix develop`) and the devcontainer provide older toolchains, so a local
  build can differ from CI.
- GNU Make 4.x.
- For host tests: Ruby with the Ceedling 1.0.1 gem, and a host `gcc`.
- For the static-analysis gate: cppcheck 2.13.0. The Makefile refuses any
  other version.
- For flashing: `st-flash` (stlink).

### Firmware

All commands run from `JOS/`.

```sh
make release COMMS_AUTH_KEY=<8 hex digits>   # optimised build with the mission uplink key
make crc-stamp                               # stamp the image CRC; required before flashing
make flash                                   # program the stamped image with st-flash
```

Profiles:

| Target | Purpose |
|---|---|
| `make debug` | Default. No optimisation, full debug information. |
| `make release` | Size-optimised build with debug information. |
| `make release-strip` | Size-optimised build without debug information, for Flash budget checks. |
| `make bench` | Bench-only build. It relaxes the boot-integrity policy and accepts the public uplink key. It must never be flashed to flight hardware. |

**Uplink key.**
- Without `COMMS_AUTH_KEY`, the image rejects every authenticated
  telecommand.
- The key is written to a generated, git-ignored header and never appears on a
  compiler command line.
- Later invocations without the variable keep the provisioned key.
  `COMMS_AUTH_KEY=` (an empty value) or `make clean` removes it.
- Never commit the key.

Build outputs (`JOS.elf`, `.bin`, `.hex`, `.map`) are written to `JOS/build/`.

### Tests and analysis

```sh
make test                  # host unit tests (Ceedling)
make coverage              # coverage gate: at least 90% lines and 75% branches
make cppcheck              # static analysis gate
make cppcheck-canary       # proves the static analysis gate can still fail
```

To run a single test file:

```sh
cd test && ceedling test:test_watchdog
```

## Continuous integration

The GitHub Actions workflows (`.github/workflows/`) run on every pull request
and on every push to `main`:

- the firmware build;
- cppcheck;
- the Ceedling suite with the coverage gate;
- actionlint;
- CodeQL.

A change is not considered complete until all of these are green on the exact
commit proposed for merge.

## Documentation

- [Documentation index](JOS/docs/README.md)
- [System architecture](JOS/docs/arch/README.md)
- [Building in detail](JOS/docs/dev/building.md)
- [Fault tolerance and hardening](JOS/docs/dev/hardening.md)
- [Operator manual](JOS/docs/user/README.md)

The authoritative specification is the project document set: SPF v3,
`SW_DATA_TYPES.xlsx`, the OBC V2.0 netlist, the ICDs and the FDIR tables. That
set is maintained outside this repository. Where the code and those documents
disagree, the documents prevail.

## Contributing

- [`AGENTS.md`](AGENTS.md) defines the working rules for this repository:
  order of authority, required gates, coding rules, test evidence and change
  workflow.
- [`REVIEWS.md`](REVIEWS.md) defines the review contract applied to every pull
  request.
- [`TASKS.md`](TASKS.md) tracks the work, the open decisions, and the
  interfaces still blocked on missing documentation.

## Status

The software is under active development.

The EPS-to-OBC and AOCS-to-OBC frame formats are not yet defined by the project
documentation. The corresponding interfaces remain open and are tracked in
`TASKS.md`.

## License

Released under the MIT License; see [`LICENSE`](LICENSE). Vendored third-party
components keep their own licenses. The reuse terms of the code adapted from
RedPill-T in `JOS/App/comms/radiolib_driver.cpp` and `radiolib_hal.cpp` are an
open question tracked in `AGENTS.md`.
