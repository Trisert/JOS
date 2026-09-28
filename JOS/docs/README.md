# Documentation Index

Welcome to the JOS (RedPill) On-Board Software documentation.

## Quick Start

| For... | Go to |
|--------|-------|
| Building the code | [docs/dev/building.md](dev/building.md) |
| ESP32 simulation vs HIL | [docs/dev/simulation.md](dev/simulation.md) |
| Build/CI evidence and verification limits | [docs/dev/ci-and-verification.md](dev/ci-and-verification.md) |
| Fault tolerance (boot CRC, watchdog) | [docs/dev/hardening.md](dev/hardening.md) |
| SRAM2 parity / critical data | [docs/dev/sram2_parity.md](dev/sram2_parity.md) |
| SEU mitigation / RAM scrubbing | [docs/dev/seu_mitigation.md](dev/seu_mitigation.md) |
| Power modes / sleep-wakeup status | [docs/dev/power_modes.md](dev/power_modes.md) |
| Module APIs | [docs/api/](api/) |
| System design / runtime diagrams | [docs/arch/README.md](arch/README.md) |
| Qualification snapshots | [docs/qual/](qual/) |
| Operating the satellite | [docs/user/README.md](user/README.md) |

## Documentation Structure

```
docs/
├── README.md           # This file
├── api/                # Module API reference
│   ├── obsw.md         # State machine, watchdog, LastStates pool
│   ├── bms.md          # Battery management (EPS interface)
│   ├── comms.md        # LoRa TT&C (SX1268)
│   ├── memory.md       # FRAM cyclic buffer, Flash write, LastStates
│   ├── aocs.md         # AOCS board/interface contract and implementation status
│   └── payloads.md     # CRYSTALS, CLOUD, CLEAR
├── arch/               # Architecture & system design
│   └── README.md
├── dev/                # Developer guides
│   ├── building.md     # ARM build, CRC stamping, flashing
│   ├── ci-and-verification.md # CI graph, evidence scope, qualification limits
│   ├── simulation.md   # ESP32 development aid vs hardware HIL
│   ├── hardening.md    # Boot CRC32 integrity + task watchdog monitoring
│   ├── sram2_parity.md # SRAM2 parity NMI for critical data (W2-3)
│   ├── seu_mitigation.md # Periodic RAM scrubbing + SEU counters (W2-5)
│   ├── power_modes.md  # MCU low-power capability vs OBSW requirement (T20)
│   ├── coding_standards.md
│   └── debugging.md
└── user/               # User manual (ground operators)
    └── README.md
```

## Hardware at a glance

- **MCU:** STM32L496VGTx (Cortex-M4 @ 80 MHz)
- **Flash:** 1024 KB (firmware reserves 512 KB; LastStates pool 8 KB @ `0x08080000`)
- **SRAM:** 320 KB (256 + 64)
- **FRAM:** 512 KB external (4 × FM24VN10-G on I2C1, PB8/PB9)

## Module Overview

### OBSW (Core)
- **State Machine:** 5-state FSM (OFF → INIT → CRIT → READY → ACTIVE)
- **Watchdog:** task monitoring, anomaly detection
- **LastStates:** Flash log of state transitions (@ `0x08080000`)

### Payloads
- **CRYSTALS:** crystal growth observation
- **CLOUD:** debris detection
- **CLEAR:** optical measurements

### Support
- **BMS:** OBC-side battery telemetry plumbing; EPS transaction is not implemented
- **Comms:** LoRa radio (SX1268)
- **Memory:** FRAM + Flash storage
- **AOCS:** separate subsystem board; OBC polling/task path is not integrated

## Support

- Check source in `App/`
- Review API docs in `docs/api/`
- See debugging guide in `docs/dev/debugging.md`
- See simulation guide in `docs/dev/simulation.md`
- See hardening / fault-tolerance notes in `docs/dev/hardening.md`
