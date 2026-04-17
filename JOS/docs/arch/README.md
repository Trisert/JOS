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
JOS/
├── App/                  # Application layer
│   ├── obsw/            # State machine, watchdog
│   ├── bms/             # Battery management
│   ├── comms/           # LoRa communications
│   ├── aocs/            # Attitude control
│   ├── memory/          # FRAM, Flash, cyclic buffer
│   └── payloads/        # CRYSTALS, CLOUD, CLEAR
├── Core/                 # STM32CubeMX generated (HAL, startup)
├── Drivers/              # STM32 HAL + CMSIS
├── Middlewares/          # FreeRTOS
├── simulation/           # ESP32 dual-board HIL verification
│   ├── shared/          # Communication protocol
│   ├── esp32-obc/       # JOS ported to ESP-IDF
│   └── esp32-simulator/ # Satellite environment simulator
└── docs/                 # Documentation

## Dependencies

| Library | Purpose |
|---------|---------|
| FreeRTOS | Real-time operating system |
| RadioLib | LoRa communication |
| STM32L4 HAL | Hardware abstraction |
| CMSIS | ARM Cortex-M interface |

## Verification Test Bed

For hardware-in-the-loop verification without real satellite hardware, the OBSW can be ported to ESP32 and tested against a second ESP32 that simulates the satellite environment:

```
┌──────────────────────┐       SPI + UART       ┌──────────────────────┐
│    ESP32-OBC         │◄──────────────────────►│   ESP32-Simulator   │
│  (JOS OBSW ported)   │                        │  (satellite env)     │
│                      │                        │                      │
│  - State Machine     │                        │  - BMS/EPS sim       │
│  - Watchdog          │                        │  - IMU/Mag sim       │
│  - LoRa comms stub   │                        │  - ADC sensor sim    │
│  - Payload drivers   │                        │  - Ground station    │
│  - FreeRTOS tasks    │                        │  - Scenario engine   │
└──────────────────────┘                        └──────────────────────┘
```

Build and run with:
```bash
make sim OBC_PORT=COM3 SIM_PORT=COM4
```

See [docs/dev/simulation.md](../dev/simulation.md) for full documentation.