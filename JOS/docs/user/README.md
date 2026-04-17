# User Manual

## Overview

This manual covers satellite operation from a ground segment perspective. It describes operational states, telecommands, telemetry, and troubleshooting procedures.

## Operational States

The satellite operates in five states:

| State | Name | Behavior |
|-------|------|----------|
| OFF | Off | Kill switches active, no operation |
| INIT | Initialization | Boot, antenna deployment, tests |
| CRIT | Critical | Safe mode, beacon only |
| READY | Ready | Idle, uplink listening |
| ACTIVE | Active | Payload operations |

## Beacon Intervals

Beacon timing is state-dependent:

| State | Interval | Description |
|-------|----------|-------------|
| CRIT | 16 min | Survival mode |
| READY | 4 min | Regular status |
| ACTIVE | 1-10 min | Active operations |

## Telecommands

### Command Format

All commands use the following format:
- Max 64 bytes per packet
- Header: Command ID (1 byte)
- Payload: Variable length

### Available Commands

| ID | Command | Description | Payload |
|----|---------|-------------|---------|
| 0x01 | RESET | Soft reset OBC | None |
| 0x02 | EXIT_STATE | Resume from CRIT | Target state |
| 0x03 | SET_CONFIG | Update configuration | Config data |
| 0x04 | SEND_DATA | Transmit stored data | Address, length |
| 0x05 | ACTIVATE_PAYLOAD | Enter ACTIVE state | Payload ID |

### Command Sequence

1. Send command packet (64 bytes or less)
2. Satellite validates CRC
3. If valid: Execute command, send ACK
4. If invalid: Send NACK

## Telemetry

### Beacon Format (128 bytes)

**Bytes 0-95: Telemetry**
- Battery voltage, SoC, temperature
- Current state
- Payload status flags
- Power budget

**Bytes 96-127: System**
- Timestamp
- Task health indicators
- Last state transition info
- Error counters

### LastStates Pool

Each state transition is logged:
- Timestamp
- From state → To state
- Trigger (battery, ground, etc.)
- Context data (116 bytes)

To retrieve:
```
Command: SEND_DATA
Address: 0x08080000
Length: 128 * N entries
```

## Troubleshooting

### No Beacon

1. Check if satellite deployed (kill switches disconnected)
2. Wait for INIT → READY transition
3. Verify antenna deployed

### Commands Not Acknowledged

1. Check CRC in command
2. Verify encryption/authentication
3. Retry with longer delay

### Stuck in CRIT

- Battery may be low
- Wait for battery recovery (sunlight)
- Send EXIT_STATE command after recovery

### Payloads Not Responding

- Verify in ACTIVE state
- Check power budget
- Verify payload enabled via SET_CONFIG

## Ground Station Setup

### LoRa Settings

| Parameter | Value |
|------------|-------|
| Frequency | 436 MHz |
| Spreading Factor | 10 |
| Bandwidth | 125 kHz |
| Coding Rate | 4/8 |
| Max Packet | 64 bytes |

### Simulation Testing

Before operating with real hardware, test all commands using the ESP32 verification simulator:

```bash
make sim OBC_PORT=COM3 SIM_PORT=COM4
```

On the simulator console:
```bash
# Simulate ground station commands
activate           # Send ACTIVATE_PAYLOAD
ready               # Send EXIT_STATE
bms 20 150 3200     # Simulate low battery

# Run automated scenarios
scenario low_battery
scenario full_orbit
```

See [docs/dev/simulation.md](../dev/simulation.md) for details.

### Transmission Sequence

1. Wait for beacon to confirm satellite health
2. Send telecommand (encrypted)
3. Wait for ACK/NACK
4. If ACK: Command accepted
5. If NACK: Retry or check format

## Safety Considerations

- Do not send RESET during critical operations
- Verify battery SoC before ACTIVE commands
- Maintain 20% power margin
- Download data before critical maneuvers