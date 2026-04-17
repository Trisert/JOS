# Documentation Index

Welcome to the JOS (RedPill) On-Board Software documentation.

## Quick Start

| For... | Go to |
|--------|-------|
| Building the code | [docs/dev/building.md](dev/building.md) |
| Verification simulation | [docs/dev/simulation.md](dev/simulation.md) |
| Understanding modules | [docs/api/](api/) |
| System design | [docs/arch/README.md](arch/README.md) |
| Operating the satellite | [docs/user/README.md](user/README.md) |

## Documentation Structure

```
docs/
├── README.md           # This file
├── api/              # Module API reference
│   ├── obsw.md       # State machine, watchdog
│   ├── bms.md        # Battery management
│   ├── comms.md      # Communications
│   ├── memory.md    # FRAM, Flash storage
│   ├── aocs.md      # Attitude control
│   └── payloads.md  # CRYSTALS, CLOUD, CLEAR
├── arch/             # Architecture
│   └── README.md
├── dev/              # Developer guides
│   ├── building.md   # Build STM32 firmware
│   ├── simulation.md # ESP32 dual-board HIL verification
│   ├── coding_standards.md
│   └── debugging.md
└── user/            # User manual
    └── README.md
```

## Module Overview

### OBSW (Core)
- **State Machine:** 5-state FSM (OFF → INIT → CRIT → READY → ACTIVE)
- **Watchdog:** Task monitoring, anomaly detection
- **LastStates:** Flash log of state transitions

### Payloads
- **CRYSTALS:** Crystal growth observation
- **CLOUD:** Debris detection
- **CLEAR:** Optical measurements

### Support
- **BMS:** Battery status (stub)
- **Comms:** LoRa radio (stub)
- **Memory:** FRAM + Flash storage
- **AOCS:** Attitude control (placeholder)

## Additional Resources

- **Full Technical Report:** `RedPill_OBSW_Report.md` (in parent directory)
- **Source Code:** `App/`, `Core/`
- **Simulation:** `simulation/` (ESP32 dual-board HIL verification)

## Support

For questions:
- Check the source code in `App/`
- Review API docs in `docs/api/`
- See debugging guide in `docs/dev/debugging.md`
- See simulation guide in `docs/dev/simulation.md`