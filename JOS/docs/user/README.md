# Operating the Satellite — User Manual

This page is for ground operators. For build/architecture, see `docs/dev/`
and `docs/arch/`.

## Operational States

| State | Name | Beacon interval | Behaviour |
|-------|------|----------------|-----------|
| s0 | OFF | — | Kill switches active |
| s1 | INIT | — | Antenna deploy retry; COMMS disabled |
| s2 | CRIT | 16 min | Charging; non-vital suspended |
| s3 | READY | 4 min | Idle; uplink listening |
| s4 | ACTIVE | 1–10 min | Payload + PDT execution |

## Telecommands

Send via encrypted uplink (whitelist + shuffle). Each packet may carry a
set-delay field for out-of-view scheduling.

| TC | Effect |
|----|--------|
| `RESET` | Soft reset OBC |
| `EXIT_STATE` | Exit recovery state |
| `SET_CONFIG` | Upload config (ADCS, clock, LoRa) |
| `SET_DOWNLINK` | Enable/disable all TX (commanding-in-blind) |
| `SEND_CONFIG` | Downlink current config |
| `SEND_DATA` | Downlink FRAM data (LastStates alias available) |
| `SEND_TELEMETRY` | Immediate extended telemetry |
| `ACTIVATE_PAYLOAD` | Run payload action (optional delay) |

## Downlink

| Packet | Size | Trigger |
|--------|------|---------|
| BEACON | 128 B | Periodic (state-dependent) |
| TELEMETRY | var | `SEND_TELEMETRY` |
| CONFIG | var | `SEND_CONFIG` |
| DATA | ≤64 B/frag | `SEND_DATA` |
| ACK / NACK | 8 B | On command receipt / error |

## Forensics — LastStates Pool

Request `SEND_DATA` with the LastStates alias to retrieve the 64-entry ring
buffer of state transitions (8 KB @ `0x08080000`). Use it to reconstruct the
sequence leading to an anomaly.

## Ground Segment

Based on a TinyGS fork (ESP32 + SX1268). Features: full uplink encryption,
automatic NACK retransmit, scheduled command delay, web UI. Packets published
to TinyGS public DB + local backup.
