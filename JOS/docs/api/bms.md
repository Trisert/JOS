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

> Status: integrated. The EPS SPI slave interface is stubbed in `bms.c`
> (`TODO: init subsystem SPI master to EPS STM32L496`).
