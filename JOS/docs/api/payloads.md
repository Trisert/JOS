# Payload APIs (`App/payloads/`)

## CRYSTALS (`crystals.c`)

Electrochromic device. OBSW: apply 3 V (TBC), heater (5 mW) below +5 °C,
trigger ArduCam (camera MCU) for transmittance, read photodiode pairs
(VBPW34S/R), compute obscuration %.

## CLOUD (`cloud.c`)

Sub-mm debris sensor: 16 copper resistive stripes × 2 faces (+Y/−Y).
`cloud_acquire()` samples both faces under the shared SPI1 ownership lock,
releases that lock before breach calculation and the FRAM cyclic-buffer write,
and writes the 16×2 matrix (boolean + 32-bit timestamp) only when a new breach
is found. `last_sample` is the historical state: an ongoing breach retains its
first-breach timestamp and is not counted again. The CLOUD task is not called
from production `main()` yet; this API and its host test do not prove payload
hardware wiring or flight integration.

## CLEAR (`clear.c`)

12 LEDs/face (+Z/−Z, 2 colours) + 1 photodiode/face. LED on/off via
`ACTIVATE_PAYLOAD`; photodiode burst one orbit on command.

## Command DB (`App/payloads/` + `docs/arch`)

`A_CRYSTALS_ON/OFF`, `A_REQUEST_BEACON`, `A_TAKE_PHOTO`, `A_CHANGE_CAM_SETTING`,
`A_RESTART_SATELLITE`; telemetry `T_CRYSTALS_STATUS`, `T_PHOTO_TAKEN`, etc.
Consolidation in progress.
