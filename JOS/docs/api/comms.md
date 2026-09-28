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
| `lora_rx_task(void *arg)` | `void` | FreeRTOS task: interrupt-driven RX, frame validation/authentication, dispatch |
| `lora_beacon_task_create(void)` | `osThreadId_t` | Create beacon task (declared in `comms.h`) |
| `lora_rx_task_create(void)` | `osThreadId_t` | Create RX task (declared in `comms.h`) |
| `lora_send_chunked(const uint8_t *data, size_t len)` | `int` | Fragment into ≤64 B packets; each chunk carries a 3-B header (byte 0 = message id, byte 1 = 0-based seq, byte 2 = total count), 128-B beacon ships as 3 chunks; aborts + counts on radio error; **re-arms RX on every exit** (`startTransmit()` leaves the SX1268 in standby) |
| `comms_tx_get_stats(comms_tx_stats_t *out)` | `void` | TX counters: sequences ok/failed, chunks sent, RX re-arm failures |
| `comms_rx_handle_frame(const uint8_t *frame, size_t len)` | `comms_tc_result_t` | Validate + dispatch uplink; the verdict is what the dispatcher reports: `OK` only when the command was carried out, `REFUSED` when its subsystem refused it (state gates, beacon bounds), `OPCODE` for validated-but-unimplemented opcodes (`SET_CONFIG`, `SEND_DATA`); every verdict counted (incl. `PHY` for radio-level drops) |
| `comms_flags_have(flags, want)` | `bool` | Correct test of an `osThreadFlagsWait()` result (mask + error bit), used by the RX task |

Radio concurrency: every `lora_*()` entry point that talks to the SX1268 holds
a radio mutex (`radiolib_driver.cpp`) for its whole command sequence, because
the beacon task and the RX task share the chip, SPI1 and the DMA state.
`lora_start_receive()` refuses while a TX is in flight. The HAL's SPI wait
blocks on a thread flag set by the DMA IRQ (other tasks keep running), and
`delay()` uses `osDelay()` once the scheduler runs.

## Uplink authentication key

Authenticated frames carry a truncated HMAC-SHA256 tag (upstream `makeMAC`,
4-byte key). The key is provisioned at build time:
`make release COMMS_AUTH_KEY=<8 hex digits>` (-> `COMMS_AUTH_KEY0..3` in the
generated, mode-0600 `build/gen/comms_auth_key.h`, never on a logged command
line; a later `make` without the variable keeps the provisioned header,
`COMMS_AUTH_KEY=` removes it — see `docs/dev/building.md`). Without
it the image only knows the **published** upstream default `A1 B2 C3 D4`, and
every authenticated frame is rejected with `MAC` (fail closed). The `bench`
profile opts back into the public key (`COMMS_AUTH_ALLOW_PUBLIC_KEY=1`).
Changing the key rebuilds the affected objects automatically (the generated
header is tracked by the dependency files).

Open (TASKS.md): 4-byte key and 4-byte tag are brute-forceable offline from
one captured frame, and there is no replay protection — both are properties of
the upstream frame/MAC design shared with the ground segment.

> `lora_beacon_task_create` / `lora_rx_task_create` prototypes were missing
> from `comms.h` and caused a build failure on GCC 15 (implicit-function-
> declaration is an error there). Fixed in PR #5 — both are now declared with
> the correct `osThreadId_t` return type (requires `cmsis_os2.h`).

## Telecommand Set

The JOS opcode dispatcher currently handles `RESET`, `EXIT_STATE`,
`SET_CONFIG`, `SEND_DATA`, `ACTIVATE_PAYLOAD`, and beacon-interval updates.
`SET_CONFIG` and `SEND_DATA` are currently TODO/no-op handlers; accepting a structurally valid frame does
not mean every requested operation is implemented. See the dispatcher in
`App/comms/comms.c` for current behavior. The separate TT&C layout uses the TEC
registry; only handlers bound in the current adapter are operational.

## Uplink validation paths

`comms_rx_handle_frame()` selects the TT&C layout or the JOS opcode layout.
The JOS path checks structure, CRC, opcode and parameter bounds, and under the
flight default requires the truncated HMAC-SHA256 authentication tag. CRC is
unkeyed integrity, not authentication. The TT&C path parses its own layout and
calls a MAC-verifier seam. No production binding for that verifier is present
in this snapshot; the NULL verifier rejects TT&C frames before dispatch. Neither path
provides a replay/freshness check. Neither path encrypts payloads. The JOS
validator currently uses a four-byte HMAC key default and a four-byte tag; the
default is public source material, not a mission secret. Mission key provisioning
and accepted authentication strength must be confirmed before flight use; do not
copy test/default key bytes into operational documentation.

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
`lora_tx()` routes DIO1 to the sender **before** `startTransmit()`, and
`lora_tx_wait_done()` accepts a wake-up only when the SX1268 IRQ status
(`getIrqFlags()`) carries `TX_DONE`, so neither a preemption after
`startTransmit()` nor an RX_DONE pending from receive mode can be mistaken
for the other.

The confirmation loop is plain C in `lora_tx_wait.c` (host-tested by
`test/test/test_lora_tx_wait.c`):

| Function | Contract |
|---|---|
| `lora_tx_wait_confirm(timeout_ms)` | Waits for `LORA_FLAG_TX_DONE`; accepts a wake-up only if `lora_irq_status()` has `LORA_IRQ_TX_DONE` (SX126x IRQ bit 0), otherwise keeps waiting for the remaining budget. `osWaitForever` is unbounded. 0 on confirmed TX_DONE, -1 on timeout or RTOS error. |
| `lora_irq_status()` | Flight implementation in `radiolib_driver.cpp`: `getIrqFlags()` under the radio mutex; a failed SPI read returns 0 (never TX_DONE). |

`lora_tx_wait_done()` in the driver is `lora_tx_wait_confirm()` followed by the
release of DIO1 routing. The DIO1-before-`startTransmit()` ordering itself is
C++ driver code and is not host-tested.
No mid-sequence watchdog kick is needed: the task is monitored against its
1..16 min cadence and flagged only after 3× the period (≥ 3 min), so the block
sits two orders of magnitude inside the monitor window.

## Downlink Packet Types

`ACK` (8 B), `NACK` (8 B), `BEACON` (128 B), `TELEMETRY` (var),
`CONFIG` (var), `DATA` (≤64 B/fragment).
