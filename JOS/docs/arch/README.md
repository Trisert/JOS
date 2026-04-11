# System Architecture

## Overview

The JOS (RedPill) On-Board Software (OBSW) is flight software for a CubeSat/PocketQube satellite. It manages all satellite operations including power, communications, payloads, and state transitions.

## Architecture Layers

```
┌─────────────────────────────────────┐
│         Application Layer          │
│  ┌──────┬──────┬──────┬─────────┐ │
│  │ AOCS │ BMS  │Comms │ Payloads│ │
│  │      │      │      │CRY/C/L  │ │
│  └──────┴──────┴──────┴─────────┘ │
│   State Machine + Watchdog        │
├─────────────────────────────────────┤
│     FreeRTOS Kernel               │
├─────────────────────────────────────┤
│   STM32L4 HAL + Drivers            │
├─────────────────────────────────────┤
│        Hardware                   │
└─────────────────────────────────────┘
```

## Hardware Platform

| Component | Specification |
|------------|---------------|
| MCU | STM32L496VGTx (ARM Cortex-M4) |
| Flash | 1024 KB |
| RAM | 128 KB |
| External Storage | 4 MB FRAM |
| Communication | SX1268 LoRa (436 MHz) |

## Operational States

| State | Description | Beacon Interval |
|-------|-------------|-----------------|
| OFF | System off | None |
| INIT | Boot, antenna deploy | None |
| CRIT | Safe mode | 16 min |
| READY | Idle, listening | 4 min |
| ACTIVE | Payload operations | 1-10 min |

## Key Design Decisions

### Multi-Processor Distribution
- **OBC (STM32L4):** Core OBSW, state machine, comms, payloads
- **EPS (STM32L1):** Battery management
- **Camera MCU:** Image acquisition

### Memory Strategy
- **FRAM (4 MB):** Payload data, cyclic buffer
- **Internal Flash:** OBSW + LastStates (8 KB)
- **RAM:** Runtime stacks, FreeRTOS

### Communication
- LoRa with 64-byte max packets
- Chunked transfer for large data
- No polling — interrupt-driven

### Reliability
- Hardware watchdog
- Software task monitoring
- LastStates pool for anomalies
- State-based beacon intervals

## Detailed Documentation

The comprehensive system architecture is documented in the parent directory:

- **RedPill_OBSW_Report.md** — Full technical report (ESA Fly Your Satellite! 4)

## File Structure

```
docs/
├── api/          # Module API references
│   ├── obsw.md
│   ├── bms.md
│   ├── comms.md
│   ├── memory.md
│   ├── aocs.md
│   └── payloads.md
├── arch/         # This directory
│   └── README.md # This file
├── dev/          # Developer guides
│   ├── building.md
│   ├── coding_standards.md
│   └── debugging.md
└── user/        # User manual (pending)
```

## Dependencies

| Library | Purpose |
|---------|---------|
| FreeRTOS | Real-time operating system |
| RadioLib | LoRa communication |
| STM32L4 HAL | Hardware abstraction |
| CMSIS | ARM Cortex-M interface |