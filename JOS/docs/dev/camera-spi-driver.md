# Camera SPI driver — integration design and open questions

Task #47: ArduCam Mini 5MP Plus (B0068) capture over the payload SPI bus, sensor
configuration over I2C_CAM. This note records what the repository and the
SharePoint corpus actually fix, what they leave open, and the two design
decisions that are safe to take now. It deliberately does **not** invent
register addresses, pin assignments, or bus peripherals (AGENTS.md §1).

**Update 2026-11-10:** the OBC V2.0 manufacturing gerbers (`OBC_V2.0.zip`,
`ELE_april2026_orders/PCBs (drill & gerber)/`, 2026-04-08) now fix the payload
SPI peripheral, the I2C_EXT bus and the I2C_CAM bus. Only the MCP23017 I2C
address remains open (see below).

## Bus facts

From `JOS.ioc`, the OBC V2.0 gerbers, and the corpus:

| Bus | Peripheral | Pins | Use | Evidence |
|---|---|---|---|---|
| I2C1 | I2C1 | PB8/PB9 | FRAM (4 × FM24VN10-G) | `JOS.ioc` |
| I2C2 | I2C2 | PB10/PB11 | camera control (`I2C_CAM`) | `OBC_V2.0-In1_Cu.gbr`: J6 pin 9 `I2C2_SDA_CAM`, pin 8 `I2C2_SCL_CAM` |
| I2C4 | I2C4 | PD12/PD13 | `I2C_EXT` → MCP23017 GPIO expander | `OBC_V2.0-F_Cu.gbr`: MCU1 pin 59 PD12 `I2C4_SCL_EXT`, pin 60 PD13 `I2C4_SDA_EXT`; J6 pin 6/7 |
| SPI1 | SPI1 | PA5/PA6/PA7 | subsystem bus (`SPI1_BUS`) + SX1268 radio + CLOUD ADC (`cloud.c` uses `hspi1`) | `OBC_V2.0-F_Cu.gbr`: `SPI1_CLK_BUS`/`MISO_BUS`/`MOSI_BUS`; `JOS.ioc`; `cloud.c` |
| SPI2 | SPI2 | PB13/PB14/PB15 | **payload bus (`SPI2_PAY`)** → ArduCam camera | `OBC_V2.0-F_Cu.gbr`: MCU1 pin 52 PB13 `SPI2_CLK_PAY`, pin 53 PB14 `SPI2_MISO_PAY`, pin 54 PB15 `SPI2_MOSI_PAY`; J6 pin 10/11/12 |

Corpus: `ELE_DREP_Architecture_V01` — the payload SPI bus is a separate SPI
interface for the CLOUD electronics (MCP23017 + MAX11228) and the ArduCam; it
corresponds to the `SPI_PAY_*` pogo pins 10–12. The gerbers confirm this is
**SPI2**, not SPI3 and not SPI1. `AGENTS.md` §"Hardware" — the `.ioc` enables
only SPI1 and SPI2; do not assume SPI3 exists.

## Open questions that block implementation

1. **MCP23017 I2C address.** Set by strap pins A0–A2; absent from the corpus
   and from the simulation (which models only an 8-bit output snapshot). The
   register map itself is public (Microchip MCP23017 datasheet, referenced in
   `ELE_DREP`). The `GPIO_expander` board manufacturing outputs (gerbers + BOM,
   2026-04-08, `ELE_april2026_orders/`) confirm the part is `MCP23017T-E/SS` on
   a 26-pin header; the gerbers show A0/A1/A2 routed to header pins 22/21/20,
   but only the **schematic/netlist** would show the strap wiring to GND/VCC —
   and those are the files issue #54 reports missing. Need ELE. Until answered,
   the driver must fail closed (no device found → log error, no image).
2. ~~**Payload SPI peripheral.**~~ **RESOLVED 2026-11-10:** SPI2 (PB13/PB14/PB15),
   confirmed by `OBC_V2.0-F_Cu.gbr` net names `SPI2_CLK_PAY`/`SPI2_MISO_PAY`/
   `SPI2_MOSI_PAY` on MCU1 pins 52/53/54.
3. ~~**I2C_EXT bus identity.**~~ **RESOLVED 2026-11-10:** I2C4 (PD12/PD13),
   confirmed by `OBC_V2.0-F_Cu.gbr` (`MCU1 pin 59 PD12 → I2C4_SCL_EXT`, pin 60
   PD13 → I2C4_SDA_EXT`) and pogo pins 6/7 (`OBC_V2.0-In1_Cu.gbr`).

## Resolved contradictions (2026-11-10)

- **SPI2 is the payload bus, not the subsystem bus.** The earlier draft of this
  note (PR #110) listed SPI2 as the subsystem bus and "payload SPI = no
  peripheral enabled". The OBC V2.0 gerbers correct this: `SPI1_BUS` is the
  subsystem bus (with radio + CLOUD ADC on `hspi1`), `SPI2_PAY` is the payload
  bus. `cloud.c` using `hspi1` for the CLOUD ADC is consistent with SPI1 = BUS.
- **No third SPI peripheral needed.** The `.ioc` enables SPI1 and SPI2 only;
  the payload bus is SPI2. No CubeMX regeneration is required for the bus
  selection. (DMA channel assignment for SPI2 is still an implementation
  detail — see Decision 2.)

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

This L4 has **no DMAMUX**: DMA1 Ch2/Ch3 serve SPI1 RX/TX via `CSELR`. The payload
bus is SPI2 (resolved 2026-11-10), so its RX/TX DMA channels must be assigned
from the remaining DMA1 channels — the exact channel pair for SPI2 on STM32L496
is an implementation detail to pin from the reference manual, not a bus-selection
question.

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

## Authoritative corpus documents

- **`OV5642_DS (camera).pdf`** — OmniVision OV5642 5 MP sensor datasheet
  (preliminary specification, ~232 pp), SharePoint `CRYSTALS FILES/Schede
  tecniche/`, uploaded 2026-10-19. Authoritative register map for the sensor
  side of subtask `t_ed55cc67`.
- **`ELE_april2026_orders/PCBs (drill & gerber)/OBC_V2.0.zip`** (2026-04-08) —
  OBC V2.0 manufacturing gerbers + drill. **Authoritative for the bus wiring:**
  `V2.0-F_Cu.gbr` / `V2.0-In1_Cu.gbr` / `V2.0-B_Cu.gbr` fix `SPI1_BUS`,
  `SPI2_PAY`, `I2C2_SDA_CAM`/`I2C2_SCL_CAM`, `I2C4_SDA_EXT`/`I2C4_SCL_EXT`,
  and the MCU1 pin assignments (PB13/14/15, PD12/13). This is the evidence that
  resolved the payload-SPI and I2C_EXT questions on 2026-11-10.
- **`ELE_april2026_orders/PCBs (drill & gerber)/GPIO_expander.zip`** (2026-04-08)
  — GPIO expander gerbers; show A0/A1/A2 routed to header J1 pins 22/21/20,
  but not the strap wiring (no schematic/netlist).
- **`ELE_april2026_orders/BOMs from KiCad/OBC_V2.0.csv`** (2026-04-08): row 5
  `IC10,IC13,IC12,IC11` = 4 × `FM24VN10-G` (FRAM); row 15
  `TMP1,TMP3,TMP4,TMP2` = 4 × `TMP1827NGRT`; row 16 `J6` =
  `854-22-020-20-001101` (pogo connector).
- **`ELE_april2026_orders/BOMs from KiCad/GPIO_expander.csv`** (2026-04-08):
  row 3 `IC1` = `MCP23017T-E_SS`; row 2 `J1` = `Conn_01x26`.
- `ELE_DREP_Architecture_V01` — payload SPI, I2C_EXT, I2C_CAM, MCP23017.

## References

- `ELE_DREP_Architecture_V01` — payload SPI, I2C_EXT, I2C_CAM, MCP23017.
- `AGENTS.md` §1 (do not invent), §4 (no polling, DMA + ISR).
- `JOS/test/project.yml` — host-build path rules and coverage gate.
- PR #100 — SPI1 ownership model to extend.
- `TheColonelYoung/ArduCAM_STM32_HAL_Demo` — OV5642 register tables, FIFO sequence.
- Gantt `marco-42.github.io/j2050` (tasks.json 2026-11-09): F2 MCP23017 driver
  due 30 nov 2026, H7 camera ArduCam SPI driver due 15 dic 2026.
