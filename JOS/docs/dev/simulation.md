# Simulation & HIL Verification

RedPill verification uses a dual-board ESP32 setup (`simulation/esp32-obc/`)
that mimics the OBC + a ground station, letting us exercise TT&C and state
logic without flight hardware.

## ESP32 OBC Simulation

`simulation/esp32-obc/main/` contains a FreeRTOS port of the OBSW task
structure (stubs for `lora_beacon_task_create` / `lora_rx_task_create` live in
`obsw_stubs.h`). It lets us:

- Validate the state machine + beacon cadence in a fast loop
- Exercise telecommand dispatch (CRC, decrypt stub, dispatch)
- Run the LastStates logging path against a simulated Flash

## Workflow

1. Flash the ESP32 OBC sim + a second ESP32 as a TinyGS-style ground node.
2. Drive state transitions from the GS; capture beacons.
3. Compare observed cadence/transitions against `docs/arch/README.md`.

This is a development aid — flight firmware is built per `docs/dev/building.md`.
