# Payload APIs (`App/payloads/`)

## CRYSTALS (`crystals.c`)

Electrochromic device. OBSW: apply 3 V (TBC), heater (5 mW) below +5 °C,
trigger ArduCam (camera MCU) for transmittance, read photodiode pairs
(VBPW34S/R), compute obscuration %.

## CLOUD (`cloud.c`)

Sub-mm debris sensor: 16 copper resistive stripes × 2 faces (+Y/−Y).
`cloud_acquire()` reads 16-bit stripe word via SPI1, writes 16×2 matrix
(boolean + 32-bit timestamp) to FRAM on breach only.

## CLEAR (`clear.c`)

12 LEDs/face (+Z/−Z, 2 colours) + 1 photodiode/face. LED on/off via
`ACTIVATE_PAYLOAD`; photodiode burst one orbit on command.

## Camera (`App/payloads/`; driver not yet written — task 47)

ArduCam Mini 5MP Plus (B0068): image data over the payload SPI bus
(`SPI_PAY_*`, pogo pins 10–12) with CS through the MCP23017 on `I2C_EXT`;
sensor registers over `I2C_CAM` (I2C2, PB10/PB11). Only
`CRYSTALS_CAM_TRIG` (PC2, active-low 100 ms) exists in `crystals.c`; the
capture/FIFO driver does not.

Design and open hardware blockers: `JOS/docs/dev/camera-spi-driver.md`
(registered as open decision 11 in `TASKS.md`). Until ELE/SYS answer, the
task stays blocked: payload SPI peripheral not configured in `JOS.ioc`,
MCP23017 I2C address undefined, `I2C_EXT` = I2C4 unconfirmed.

## Command DB (`App/payloads/` + `docs/arch`)

`A_CRYSTALS_ON/OFF`, `A_REQUEST_BEACON`, `A_TAKE_PHOTO`, `A_CHANGE_CAM_SETTING`,
`A_RESTART_SATELLITE`; telemetry `T_CRYSTALS_STATUS`, `T_PHOTO_TAKEN`, etc.
Consolidation in progress.
