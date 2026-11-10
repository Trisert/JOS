# Camera SPI driver — integration design and open questions

Task #47: ArduCam Mini 5MP Plus (B0068) capture over the payload SPI bus, sensor
configuration over I2C_CAM. This note records what the repository and the
SharePoint corpus actually fix, what they leave open, and the two design
decisions that are safe to take now. It deliberately does **not** invent
register addresses, pin assignments, or bus peripherals (AGENTS.md §1).

## Bus facts

From `JOS.ioc` (the only authoritative peripheral configuration) and the
corpus:

| Bus | Peripheral | Pins | Use |
|---|---|---|---|
| I2C1 | I2C1 | PB8/PB9 | FRAM (4 × FM24VN10-G) |
| I2C2 | I2C2 | PB10/PB11 | camera control (`I2C_CAM`) |
| I2C4 | I2C4 | PD12/PD13 | enabled, **not referenced by any code** |
| SPI1 | SPI1 | PA5/PA6/PA7 | SX1268 radio + CLOUD ADC (`cloud.c` uses `hspi1`) |
| SPI2 | SPI2 | PB13/PB14/PB15 | subsystem bus, OBC master, EPS/AOCS slaves |
| payload SPI | — | pogo pins 10–12 (`SPI_PAY_MOSI/MISO/CLK`) | **no peripheral enabled** |

Corpus: `ELE_DREP_Architecture_V01` — the payload SPI bus is a separate SPI
interface for the CLOUD electronics (MCP23017 + MAX11228) and the ArduCam; it
corresponds to the `SPI_PAY_*` pogo pins 10–12. `AGENTS.md` §"Hardware" — the
`.ioc` enables only SPI1 and SPI2; do not assume SPI3 exists.

## Open questions that block implementation

1. **Payload SPI peripheral.** Which SPI peripheral are pogo pins 10–12 routed
   to? `SW_OBC.mdj` (MagicDraw) lists SPI1/SPI2/SPI3 as OBC ports, but SPI3 is
   not enabled in `JOS.ioc`. If it is SPI3, the `.ioc` must be regenerated in
   CubeMX — a board-configuration change, not a driver task. Until answered, no
   payload-SPI DMA code can be written.
2. **MCP23017 I2C address.** Set by strap pins A0–A2; absent from the corpus
   and from the simulation (which models only an 8-bit output snapshot). The
   register map itself is public (Microchip MCP23017 datasheet, referenced in
   `ELE_DREP`). Need ELE to confirm the address.
3. **I2C_EXT bus identity.** I2C4 (PD12/PD13) is the only enabled I2C bus not
   referenced by code, and the pogo-pin map puts `I2C_EXT` on pins 6/7. Treat
   I2C_EXT = I2C4 as the working hypothesis, to be confirmed by ELE.

## Contradiction to flag (do not resolve silently)

- `ELE_DREP_Architecture_V01`: the payload SPI bus is a **separate** SPI
  interface, isolated from the subsystem SPI bus.
- `AGENTS.md`: SPI1 = LoRa + CLOUD ADC; SPI2 = subsystem bus; the `.ioc` has
  only SPI1 and SPI2.
- `App/payloads/cloud.c`: the CLOUD ADC is driven on `hspi1`.

If the payload SPI is a third bus (SPI3), `cloud.c` contradicts the DREP. If it
is SPI1, the DREP's "separate bus" is separate only from SPI2. SYS/ELE must
state which; the driver follows the answer.

## Decision 1 — host-test split

`App/payloads` as a directory must never go on the Ceedling `:source:` path
(`JOS/test/project.yml`): the rest of it pulls `stm32l4xx_hal.h` and would link
into every test binary. The pattern already used for `temp.c` and `eps_fdir.c`
is a single file under `:files: :source:`.

- Pure logic — register tables, FIFO-length validation, capture state machine,
  timeout math — goes in a host-compilable TU listed as one file.
- Flight-only glue — HAL I2C/SPI/DMA calls, CS assertion — stays out of the
  host build.
- Coverage gate is enforced (`:fail_under_line: 90`, `:fail_under_branch: 75`),
  so a module that cannot be host-tested cannot ship.

## Decision 2 — DMA arbitration

PR #100 (`fix/radio-task-ownership`, OPEN) already adds static priority-inheritance
ownership for radio operations and the shared SPI1 bus, and releases SPI1 before
CLOUD calculations. The camera FIFO read must acquire the same owner before any
DMA transfer and fail closed on contention or timeout.

This L4 has **no DMAMUX**: DMA1 Ch2/Ch3 serve SPI1 RX/TX via `CSELR`. A second
SPI peripheral needs its own channels — that is part of open question 1.

## Capture sequence (port the protocol, not the HAL calls)

Reference: `TheColonelYoung/ArduCAM_STM32_HAL_Demo` (`SPI_camera.cpp`,
`OV5642_regs.hpp`). The demo polls `HAL_SPI_*` and `HAL_Delay`; JOS requires
interrupt-driven DMA and no blocking polling, so only the sequence ports:

1. Clear FIFO (ArduChip register `0x04` = `0x01`, written twice).
2. Start capture (`0x04` = `0x02`).
3. Wait for capture-done (register `0x41`, bit `0x08`) — interrupt or timeout,
   never a busy loop.
4. Read FIFO length (`0x42`/`0x43`/`0x44`).
5. Burst-read the FIFO over SPI via DMA into a static buffer, then deassert CS.
6. Sensor registers are configured over I2C_CAM (I2C2, PB10/PB11).

## References

- `ELE_DREP_Architecture_V01` — payload SPI, I2C_EXT, I2C_CAM, MCP23017.
- `AGENTS.md` §1 (do not invent), §4 (no polling, DMA + ISR).
- `JOS/test/project.yml` — host-build path rules and coverage gate.
- PR #100 — SPI1 ownership model to extend.
- `TheColonelYoung/ArduCAM_STM32_HAL_Demo` — OV5642 register tables, FIFO sequence.
