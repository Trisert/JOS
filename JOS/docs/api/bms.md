# BMS API — Battery Management (`App/bms/`)

The BMS runs on the **EPS STM32L1** (separate board). The OBC talks to it over
SPI (OBC = master, EPS = slave).

| Function | Purpose |
|----------|---------|
| `bms_get_soc(void)` | State of charge (%) from BQ27441 gas gauge (via EPS) |
| `bms_get_cell_temp(uint8_t idx)` | Per-cell temperature |
| `bms_get_charge_current(void)` | Charge current/voltage |
| threshold logic | Maps SoC to B_SCRIT / B_CRIT / B_COMMOK / B_OPOK |

The OBC uses these thresholds to drive state transitions (see
`docs/arch/README.md` §Battery thresholds).

> Status: integrated. `bms_init()` brings up the subsystem SPI master to the
> EPS STM32L496 (SPI2, PB13/14/15, mode 0, 8-bit MSB-first, 2.5 MHz from the
> 80 MHz PCLK1) and `bms_spi_ready()` reports the link state. The telemetry
> transaction itself is still stubbed in `bms.c`
> (`TODO: query EPS MCU over subsystem SPI for BQ76905 telemetry`), and the
> EPS chip-select pin is not assigned in the tree yet — define
> `BMS_EPS_CS_PORT` / `BMS_EPS_CS_PIN` once the SATPF pinout fixes it.
