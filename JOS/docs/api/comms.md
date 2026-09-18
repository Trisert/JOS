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
| `lora_send_chunked(const uint8_t *data, size_t len)` | `int` | Fragment into ≤64 B packets; each chunk carries a 3-B header (byte 0 = message id, byte 1 = 0-based seq, byte 2 = total count), 128-B beacon ships as 3 chunks; aborts + counts on radio error |
| `comms_tx_get_stats(comms_tx_stats_t *out)` | `void` | TX counters: sequences ok/failed, chunks sent |
| `comms_rx_handle_frame(const uint8_t *frame, size_t len)` | `comms_tc_result_t` | Validate + dispatch uplink; every verdict counted (incl. `PHY` for radio-level drops below the validator) |

> `lora_beacon_task_create` / `lora_rx_task_create` prototypes were missing
> from `comms.h` and caused a build failure on GCC 15 (implicit-function-
> declaration is an error there). Fixed in PR #5 — both are now declared with
> the correct `osThreadId_t` return type (requires `cmsis_os2.h`).

## Telecommand Set

`RESET`, `EXIT_STATE`, `SET_CONFIG`, `SET_DOWNLINK`, `SEND_CONFIG`,
`SEND_DATA`, `SEND_TELEMETRY`, `ACTIVATE_PAYLOAD`. Each packet may carry a
set-delay field for out-of-view scheduling.

## Downlink framing

Each ≤64 B chunk carries `msg | seq | total`: a monotonic message id (consecutive
beacons get consecutive ids, mod 256), the 0-based chunk index, and the chunk
count. Ground reassembles by `seq`, flags a gap when fewer than `total` chunks
arrive for an id, flags a duplicate when the same id+seq repeats, and never
merges chunks across ids. A sequence aborted mid-TX (radio error, counted in
`comms_tx_stats_t.sequences_failed`) therefore always shows up as a short
`total` — never as a silently complete message.

## Beacon blocking vs watchdog

One beacon TX holds `loraBeacon` for up to ~6 s (3 chunks × 2 s TX_DONE wait).
No mid-sequence watchdog kick is needed: the task is monitored against its
1..16 min cadence and flagged only after 3× the period (≥ 3 min), so the block
sits two orders of magnitude inside the monitor window.

## Task-level ownership

`radioOp` is a statically allocated, priority-inheritance RTOS mutex created
by `lora_init()` before task creation. TX holds it from `lora_tx()` through the
owner-only `lora_tx_wait_done()` cleanup; RX/read/rearm and competing TX calls
use the same mutex with a bounded 10 ms acquisition (10 RTOS ticks at the
configured 1 kHz tick rate) and refuse on failure.
DIO1 ISRs never acquire it or perform SPI. The boot path records the `lora_init()`
result and does not create the beacon task after an init failure; the RX task
retries receive arming and withholds watchdog liveness until the receiver is
armed, so persistent failure remains fail-closed for watchdog recovery.

SPI1 physical-bus ownership is a separate statically allocated mutex. The
RadioLib HAL holds it only for one SPI transaction, and the CLOUD task holds
it only while sampling the ADC faces; calculations and the subsequent I2C FRAM
write occur after release. This covers the first-party SPI1 users
currently present; any future SPI1 device must use the exported
`lora_spi_bus_acquire()` / `lora_spi_bus_release()` pair. DMA and TIM6 are not
masked. The mutexes are initialized before `osKernelInitialize()` because the
existing boot sequence initializes the radio before the scheduler and creates
tasks only afterward; pre-scheduler initialization is single-threaded.
`cloud_init()` is a boot-only ADC setup/baseline path, but the current
production `main()` does not call `cloud_init()` or create the CLOUD task. If
CLOUD is integrated later, its initialization must remain before concurrent
tasks and its sampling path must use the exported bus lock.

The supplied UML (`SW_OBC.mdj`, Activity1) records SPI mutual-exclusion intent
but does not define this complete algorithm. HIL validation of concurrent
STM32/SX1268/DMA behavior remains open.

## Downlink Packet Types

`ACK` (8 B), `NACK` (8 B), `BEACON` (128 B), `TELEMETRY` (var),
`CONFIG` (var), `DATA` (≤64 B/fragment).
