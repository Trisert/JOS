# Comms (Communications) API Reference

## Overview

The Comms module handles LoRa radio communications for telemetry (beacon TX) and telecommand reception (uplink). Located in `App/comms/`.

## Dependencies

- Header: `App/comms/comms.h`
- External: RadioLib (C++ library for SX1268)

## Hardware

- **Transceiver:** SX1268 LoRa chip
- **Frequency:** 436 MHz (amateur satellite band)
- **SPI:** SPI1
- **Settings:** SF10, BW125, CR4/8

## Status

**Current Implementation:** Stub (placeholder functions)

The module contains stub implementations. Real implementation uses RadioLib with STM32 HAL wrapper.

## Public API

### `int lora_init(void)`

Initialize the LoRa transceiver.

**Parameters:** None

**Returns:** `int` - 0 on success

**Notes:**
- TODO: Configure SX1268 via RadioLib — SF10, BW125, CR4/8, 436 MHz

---

### `void lora_beacon_task(void *arg)`

Beacon TX task. Sends beacon at state-dependent interval.

**Parameters:**
- `arg` - FreeRTOS task argument (unused)

**Returns:** Nothing

**Behavior:**
- Gets beacon interval from state machine
- Builds beacon packet (96 B telemetry + 32 B system)
- Sends via LoRa TX
- Delays for interval

---

### `osThreadId_t lora_beacon_task_create(void)`

Create the beacon TX task.

**Parameters:** None

**Returns:** `osThreadId_t` - Handle to created task

---

### `void lora_rx_task(void *arg)`

RX task. Continuous uplink listening and command dispatch.

**Parameters:**
- `arg` - FreeRTOS task argument (unused)

**Returns:** Nothing

**Behavior:**
- Enters RX mode
- Waits for interrupt on incoming packet
- Decodes packet
- CRC check → decrypt → dispatch command

---

### `osThreadId_t lora_rx_task_create(void)`

Create the RX task.

**Parameters:** None

**Returns:** `osThreadId_t` - Handle to created task

---

### `int lora_send_chunked(const uint8_t *data, size_t len)`

Send data in 64-byte chunks.

**Parameters:**
- `data` - Data buffer
- `len` - Data length in bytes

**Returns:** `int` - 0 on success

**Notes:**
- TODO: Fragment into 64-byte chunks with sequence numbers

---

### `void comms_dispatch_command(uint8_t cmd_id, const uint8_t *payload, size_t len)`

Dispatch a decoded telecommand.

**Parameters:**
- `cmd_id` - Command ID
- `payload` - Command payload data
- `len` - Payload length

**Returns:** Nothing

**Supported Commands:**

| Command ID | Name | Description |
|-----------|------|-------------|
| 0x01 | RESET | System reset |
| 0x02 | EXIT_STATE | Transition to READY state |
| 0x03 | SET_CONFIG | Apply configuration |
| 0x04 | SEND_DATA | Read FRAM and send chunked |
| 0x05 | ACTIVATE_PAYLOAD | Transition to ACTIVE state |
| 0x06 | SET_BEACON_INTERVAL | Override beacon transmission interval |

### SET_BEACON_INTERVAL (0x06)

Override the default per-state beacon interval.

**Payload:** 4 bytes, big-endian (interval in milliseconds)

**Behavior:**
- Sets a custom beacon interval that overrides the state-dependent defaults
- Sending `0x00000000` (0 ms) clears the override and reverts to the default per-state intervals defined in `obsw_types.h`:
  - `BEACON_INTERVAL_CRIT` — 16 min (960 000 ms)
  - `BEACON_INTERVAL_READY` — 4 min (240 000 ms)
  - `BEACON_INTERVAL_ACTIVE` — 1 min (60 000 ms)

**Example:** To set a 16-minute beacon interval, send payload `0x000EA600`.

---

## Beacon Format

Beacon packet structure (128 bytes total):
- 96 bytes: Telemetry
- 32 bytes: System info (state, timestamps, etc.)

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `LORA_MAX_PACKET` | 64 | Maximum LoRa packet size (bytes) |
| `BEACON_SIZE` | 128 | Beacon packet size (bytes) |

## Notes

This module requires:
1. RadioLib integration with STM32 HAL
2. Proper SPI DMA configuration
3. CRC and encryption implementation
4. Command authentication