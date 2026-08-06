# Documentation Index

Welcome to the JOS (RedPill) On-Board Software documentation.

## Quick Start

| For... | Go to |
|--------|-------|
| Building the code | [docs/dev/building.md](dev/building.md) |
| Verification simulation | [docs/dev/simulation.md](dev/simulation.md) |
| SRAM2 parity / critical data | [docs/dev/sram2_parity.md](dev/sram2_parity.md) |
| Understanding modules | [docs/api/](api/) |
| System design | [docs/arch/README.md](arch/README.md) |
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
│   ├── aocs.md         # Attitude control (B-dot, EKF)
│   └── payloads.md     # CRYSTALS, CLOUD, CLEAR
├── arch/               # Architecture & system design
│   └── README.md
├── dev/                # Developer guides
│   ├── building.md     # Build (NixOS container / local / CI / CubeIDE)
│   ├── simulation.md   # ESP32 dual-board HIL verification
│   ├── sram2_parity.md # SRAM2 parity NMI for critical data (W2-3)
│   ├── coding_standards.md
│   └── debugging.md
└── user/               # User manual (ground operators)
    └── README.md
```

## Hardware at a glance

- **MCU:** STM32L496VGTx (Cortex-M4 @ 80 MHz)
- **Flash:** 1024 KB (firmware reserves 512 KB; LastStates pool 8 KB @ `0x08080000`)
- **SRAM:** 320 KB (256 + 64)
- **FRAM:** 4 MB external (SPI2)

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
- **BMS:** battery status (EPS STM32L1 interface)
- **Comms:** LoRa radio (SX1268)
- **Memory:** FRAM + Flash storage
- **AOCS:** attitude control (B-dot + EKF)

## Support

- Check source in `App/`
- Review API docs in `docs/api/`
- See debugging guide in `docs/dev/debugging.md`
- See simulation guide in `docs/dev/simulation.md`
