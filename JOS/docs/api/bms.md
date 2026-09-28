# BMS API — Battery Management (`App/bms/`)

The BMS module is OBC-side plumbing for battery telemetry from the separate
EPS board. The OBC configures SPI2 as master, but the EPS transaction and wire
format are not implemented. EPS MCU / battery-monitor identity is unresolved
across delivered documents; see `TASKS.md` and do not treat this module's
comments as an approved hardware baseline.

| Function | Purpose |
|----------|---------|
| `bms_init(void)` | Configure/bind the SPI2 master; this does not verify an EPS response |
| `bms_spi_ready(void)` | Report whether the local SPI handle initialized, not whether EPS is alive |
| `bms_get_status(void)` | Return cached status; initially invalid and conservative |
| `bms_poll(void)` | Pinned, unimplemented EPS transaction seam; currently returns failure |
| BMS policy helpers | Apply documented threshold rules to a status snapshot |

The state machine periodically attempts a poll and copies status only on a
successful result. Since the transaction currently returns failure, the
snapshot remains invalid; do not interpret default numeric fields as live
telemetry. Some state transitions consume BMS validity/SoC; the state-machine
documentation records the exact gates and exceptions.

> Status: partial. `bms_init()` configures the subsystem SPI master (SPI2,
> PB13/PB14/PB15, mode 0, 8-bit MSB-first, 2.5 MHz) when initialization
> succeeds. This is local peripheral readiness only. The EPS CS assignment,
> request/reply format, and telemetry transaction remain unresolved; `bms_poll()`
> returns `-1` and does not fabricate telemetry.
