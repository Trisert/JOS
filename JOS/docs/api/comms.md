# Comms API — LoRa TT&C (`App/comms/`)

The OBC runs all TX/RX processing on-chip (no separate TT&C MCU). Radio is a
Semtech SX1268 on SPI1.

## Configuration

| Parameter | Value |
|-----------|-------|
| Modulation | LoRa (CSS) |
| Frequency | 436 MHz (TBC) |
| SF / BW / CR | 10 / 125 kHz / 4/8 |
| Data rate | 610 b/s |
| Max packet | 64 B |

## API

| Function | Return | Purpose |
|----------|--------|---------|
| `lora_init(void)` | `int` | Configure SX1268 |
| `lora_beacon_task(void *arg)` | `void` | FreeRTOS task: 128-B beacon at state-dependent interval |
| `lora_rx_task(void *arg)` | `void` | FreeRTOS task: interrupt-driven RX, CRC, decrypt, dispatch |
| `lora_beacon_task_create(void)` | `osThreadId_t` | Create beacon task (declared in `comms.h`) |
| `lora_rx_task_create(void)` | `osThreadId_t` | Create RX task (declared in `comms.h`) |
| `lora_send_chunked(uint8_t *data, size_t len)` | — | Fragment into ≤64 B packets w/ sequence numbers |

> `lora_beacon_task_create` / `lora_rx_task_create` prototypes were missing
> from `comms.h` and caused a build failure on GCC 15 (implicit-function-
> declaration is an error there). Fixed in PR #5 — both are now declared with
> the correct `osThreadId_t` return type (requires `cmsis_os2.h`).

## Telecommand Set

`RESET`, `EXIT_STATE`, `SET_CONFIG`, `SET_DOWNLINK`, `SEND_CONFIG`,
`SEND_DATA`, `SEND_TELEMETRY`, `ACTIVATE_PAYLOAD`. Each packet may carry a
set-delay field for out-of-view scheduling.

## Downlink Packet Types

`ACK` (8 B), `NACK` (8 B), `BEACON` (128 B), `TELEMETRY` (var),
`CONFIG` (var), `DATA` (≤64 B/fragment).
