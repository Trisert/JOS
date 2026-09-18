# Flight qualification gap: EPS/AOCS interfaces

**Audit basis:** branch `flight/eps-aocs-contracts`, base `bbd7c49`.

This record qualifies only what the recovered document set actually fixes. The
recovered SPF, DREP, data-dictionary, and FDIR files are subteam working
 documents; the corpus notes that they have no release status and contain
contradictions. The whole set is authoritative for finding those contradictions,
not for silently selecting a preferred value. A missing or contradictory field
stays unimplemented until its interface owner publishes a controlled decision.

No EPS or AOCS application frame is fabricated in this change.

## Current OBC seams

| Area | Current seam | What it does now | Qualification status |
|---|---|---|---|
| EPS | `JOS/App/bms/bms.c`: `bms_init()`, `bms_spi_ready()`, `bms_get_status()`, `bms_poll()` | Binds the existing OBC `hspi2` handle and leaves the cached status invalid until a completed poll. `bms_poll()` returns `-1`; no request, response parse, or telemetry promotion exists. | Fail-closed plumbing only. The current SPI2/mode-0/8-bit/MSB-first/2.5-MHz and optional GPIO-CS settings in `bms.c` are implementation choices, not a qualified EPS ICD. |
| EPS FDIR | `JOS/App/bms/eps_fdir.c/.h` | Pure host-testable decisions for the documented charger and battery-monitor windows, plus an unarmed OBC-side EPS-heartbeat reset request. It performs no I2C, SPI, or NRST transaction. | Decision logic only. The heartbeat value remains unarmed because the source says `TBC`. |
| AOCS | `JOS/App/aocs/aocs.c`: `aocs_init()`, `aocs_task()`, `aocs_task_create()` | `aocs_init()` is a no-op; the task only kicks the watchdog and reaches a `TODO` before a 20-ms delay; the task is not created from `main()`. | Stub. No AOCS request, response, decode, validity gate, or mode command exists. The 20-ms delay is not a specified AOCS polling rate. |
| AOCS data names | `JOS/App/aocs/aocs.h`, `aocs_status.h` | Carries the SPF-fixed `T_AOCS_MODE` position/length/value names and the separate four-name `AOCS_STATUS` model. | Telemetry naming is partly pinned; numeric `AOCS_STATUS` codes and OFF/FAULT encoding in `T_AOCS_MODE` are not pinned by the source set. |

`bms_status_t.valid == false` and the `bms_poll() == -1` path are deliberate:
unknown EPS telemetry must not become a nominal SoC. The same rule must apply to
every future AOCS/EPS reply decoder: no valid bit, sequence/age policy, or
validation result means no consumer may use the sample for a safety decision.
The current implementation does **not** yet provide freshness fail-closed
semantics after a valid sample: `bms_poll()` leaves the last cached sample alone
on failure (`JOS/App/bms/bms.c:162-191`), and `state_machine.c` can continue to
read that snapshot. A released ICD must define the maximum age/sequence rule
before that path can be called flight-qualified; this record does not invent one.

## Evidence that is pinned

### Common transport and physical path

The following transport facts are repeated by the recovered architecture set:

- `RED_SPF_V3.docx.txt:1837-1849` (SPF Table 3.9) lists AOCS and EPS/PDU as
  STM32L496VGT3 subsystem-SPI slaves to the OBC; it lists EPS components as
  MP2650, BQ76905, and LTC4368.
- `ELE_DREP_Architecture_V01.docx.txt:147-180` describes the OBC as the
  subsystem master, AOCS and EPS as slaves on dedicated SPI links, and the EPS
  MCU's local I2C connection to the BQ76905. It says the OBC can query EPS
  battery telemetry and issue power-switching commands, but it does not define
  an application packet.
- `ELE_DREP_Architecture_V01.docx.txt:217-232` says the subsystem SPI is the
  OBC-master bus for OBC↔AOCS and OBC↔EPS, with chip-selects driven by the OBC
  I2C GPIO expander; it separately assigns local I2C to AOCS sensors and the EPS
  BQ76905. `ELE_DREP_Architecture_V01.docx.txt:100-108` and `147-151` identify
  the OBC interrupt lines `INT1`/`INT2` used for asynchronous subsystem events.
  The source does not assign event types, polarity, timing, or ownership to
  those lines.
- `RED_SPF_V3.docx.txt:1720-1725` places the AOCS link on the Y-sliding plate
  and identifies the MillMax `854-22-020-20-001101` pogo-pin path. This is a
  physical/transport fact, not a packet definition.

These facts do **not** pin the OBC peripheral instance, CS expander register
writes, electrical polarity, interrupt meaning, clock rate, SPI mode, transfer
length, or transaction ownership rules. The source set must provide those
before the transport can be called flight-qualified at the register level.

### EPS-local telemetry ownership

`SW_DATA_TYPES.xlsx` (recovered text, sheet `Data Types`, lines 2-16) assigns
`V_BAT`, `BAT_SOC`, `BAT_TEMP`, `I_BAT_CHG`, and `I_BAT_DSG` to `Read By=EPS`
and `Read BUS=CHG_I2C`; only `V_BAT` has the documented address `0x26`.
The same rows assign `BAT_STATUS` to the EPS bus and `LINES_FAULT` to the
internal bus. Sheet `HB` lines 60-72 places `STATUS`, battery current, `SC_CHG`,
`V_BAT`, `BAT_SOC`, and `PAYLOADS` in the housekeeping layout and places
`AOCS_STATUS` in two STATUS bits.

This means `0x26` is not an OBC-side EPS-SPI address, and it must not be copied
to the other data fields. The OBC-side question remains open: the EPS may
export the EPS-owned values over its OBC link, but the recovered corpus does not
define how.

### AOCS telemetry and mode semantics

`RED_SPF_V3.docx.txt:1301-1314` fixes the ground-telemetry fields:

- `T_AOCS_MODE`: position 20, length 1 byte, `0=Detumbling`,
  `1=Nadir-Pointing`.
- `T_ANGULAR_RATE`: position 21, length 6 bytes, signed 3-axis value, 2 bytes
  per axis; conversion uses the IMU scale factor. The field is flagged when
  absolute rate exceeds 10 deg/s on any axis.

`SW_DATA_TYPES.xlsx` (recovered text, `Data Types` line 13 and `HB` lines
60-72) separately defines `AOCS_STATUS` as a 2-bit field with the names
`OFF, DET, POINTING, FAULT`, carried in the housekeeping STATUS byte. The data
sheet does not assign numeric codes to those four names. The current enum's
0..3 order is therefore a declared project assumption, not a flight-qualified
wire encoding.

`RED_SPF_V3.docx.txt:603` and `1723-1725`, and
`AOCS_DREP_ArchitectureFile_V01.pdf:83-93`, state that the transition from
Detumbling to Pointing is commanded from ground. They do not define an
OBC-to-AOCS command, opcode, payload, authorization, acknowledgement, or
safe rejection behavior. `T_AOCS_MODE` is not permission to invent one.

## Unresolved EPS contract

| Field | Finding | Current producer/consumer seam | Decision owner | Acceptance evidence required |
|---|---|---|---|---|
| Transport binding | Subsystem SPI/OBC-master/EPS-slave is repeated, but the OBC peripheral and CS implementation are not consistently pinned. `SW_DREP_Architecture_V01.docx.txt:731-772` configures SPI2 as the dedicated FRAM bus and `:1077-1095` assigns `hspi2` to the memory module, while `:1061-1075` describes the EPS path as `hspi1` (or a dedicated EPS bus). The current firmware uses `hspi2` in `bms.c:28-38`, while `main.c:636-669` configures SPI2 independently. | Producer: EPS MCU. Consumer: `bms_poll()` and future EPS FDIR caller. Physical owner: ELE/SYS hardware. | SYS/ELE + EPS firmware owners. | Controlled netlist/ICD identifies the exact OBC SPI instance, SCK/MOSI/MISO electrical assignment, CS owner/expander register, idle level, interrupt lines, and bus-sharing/locking rule. Target test proves CS selects EPS only and no FRAM/payload device is selected. |
| MCU identity | Contradictory: SPF operational monitoring says `EPS STM32L1` (`RED_SPF_V3.docx.txt:1059-1061`); SW DREP says `STM32L1` on the EPS (`SW_DREP_Architecture_V01.docx.txt:100-108`, `232-233`); SPF Table 3.9 says `STM32L496VGT3` (`RED_SPF_V3.docx.txt:1837-1849`); ELE DREP says the same part (`ELE_DREP_Architecture_V01.docx.txt:177-182`, `1900-1914`). | `bms.c` currently names STM32L496; no runtime identity check exists. | SYS/ELE/EPS hardware owner. | Released BOM/schematic/ICD and target readback or manufacturing record identify the fitted EPS MCU; build-time documentation and tests use only that identity. |
| Gas-gauge identity | Contradictory: SPF telemetry names a `BQ27441 Gas Gauge` (`RED_SPF_V3.docx.txt:1224-1230`); SPF Table 3.9 and ELE DREP name `BQ76905` (`RED_SPF_V3.docx.txt:1847-1849`; `ELE_DREP_Architecture_V01.docx.txt:178-182`, `1910-1914`). | `bms.c` comments and `bms_status_t` expose EPS-owned SoC/voltage/current/temperature, but no IC register driver exists. | EPS electronics + EPS firmware owners. | Released schematic/BOM and EPS firmware ICD identify the monitor part, local bus, register source, units, and conversion; a bench trace demonstrates each exported field against an independently measured value. |
| Application frame | No recovered source gives request opcode, response layout, field order, status/sequence bytes, or command set for EPS↔OBC SPI. `ELE_DREP_Architecture_V01.docx.txt:177-180` only says the OBC can query telemetry and issue power commands. | `bms_poll()` returns `-1` and never parses bytes. | EPS firmware owner with SYS interface authority. | Versioned ICD includes request/response examples and negative cases; host tests cover exact encode/decode vectors and reject wrong opcode/length/status/sequence. Target test captures the same vectors over the real harness. |
| Length and endianness | Not specified for EPS replies or commands. The 8-bit data-dictionary field sizes describe housekeeping values, not an EPS SPI frame or byte order. | No serializer/deserializer exists. | EPS/SYS interface owner. | ICD states total transfer length, per-field width, signedness, byte order, alignment, and maximum response; tests include boundary and truncated frames. |
| CRC / validation | No EPS CRC polynomial, coverage, seed, byte order, checksum, timeout, retry, or freshness rule is present in the recovered EPS/AOCS sources. STM32 hardware CRC is disabled in current `bms.c:93-106`, but that is not a protocol decision. | `bms_poll()` has no validation path; invalid data remains invalid. | EPS firmware owner and OBC safety owner. | ICD specifies integrity/freshness rules; tests prove CRC/status/age failures do not update the cache and do not enable SoC-gated operation. |
| Polling/rate/timeout | `ELE_DREP_Architecture_V01.docx.txt:151` calls the AOCS master/slave arrangement deterministic polling, but the EPS section `:177-180` only says the OBC can query telemetry and issue power commands. Neither gives an OBC EPS application period. `SW_DREP_Architecture_V01.docx.txt:164-170` says hardware flows use interrupts/no polling and DMA, but that is a general architecture statement, not an EPS application period. | `state_machine.c:559-580` labels the seam “1 Hz”, but `bms_tick++ >= 10U` refreshes every 11 100-ms loop iterations (about 0.91 Hz); either way this is an OBC project choice, not a source-pinned EPS contract. | OBC/EPS integration owner. | ICD specifies request cadence, response deadline, retry budget, and stale-data age. HIL trace shows the OBC never overlaps requests and enters the documented stale/fail-closed behavior. |
| Heartbeat and reset semantics | `RED_FMEA_V2.xlsx.txt:124-175` says the EPS uC communication-bus failure loses telemetry and says the OBC resets the EPS uC through `NRST` after `TBC ms` from the last heartbeat. `TBC` has no value or heartbeat wire/event definition. | `eps_fdir` records an EPS heartbeat only through a caller event and leaves the default timeout unarmed; it does not drive NRST. | EPS/OBC FDIR owner and SYS/ELE hardware owner. | ICD/FDIR controlled revision defines heartbeat source, transport/event, period, timeout value, reset pulse/timing, boot grace, re-arm, and escalation. Target test proves one missed-window reset and recovery without reset hammering. |

## Unresolved AOCS contract

| Field | Finding | Current producer/consumer seam | Decision owner | Acceptance evidence required |
|---|---|---|---|---|
| Application frame | AOCS transport is SPI slave to the OBC master, but no request opcode, response layout, register map, or command set is present. `AOCS_DREP_ArchitectureFile_V01.pdf:13-29` describes the board and modes but no OBC packet contract. | `aocs_task()` has only a TODO; no producer/consumer bytes exist. | AOCS firmware owner with SYS interface authority. | Versioned ICD with exact request/response vectors; host tests cover valid, wrong-opcode, wrong-length, fault-status, and absent-device paths; target capture matches vectors. |
| Clock/CS/length/endian/CRC | Not specified by the AOCS DREP/SPF set. ELE DREP fixes OBC-master/AOCS-slave and CS-expander topology, not SPI mode, baud, CS timing, reply length, byte order, or integrity. | No AOCS driver or validation code. | AOCS/ELE/SYS interface owner. | Controlled ICD plus electrical timing review and logic-analyzer capture at the selected rate. |
| Polling and validity | No AOCS OBC polling period, deadline, retry, stale age, sequence, or fail-closed action is pinned. The current `osDelay(20 ms)` is a placeholder and must not be treated as 50 Hz qualification evidence. | `aocs_task()` kicks the watchdog even though it has no valid sample; no state/attitude consumer is gated by link validity. | AOCS/OBC safety owner. | ICD and safety analysis define cadence/deadline/staleness; tests prove missing, malformed, stale, and FAULT replies cannot assert Pointing or feed a control decision. |
| Mode-change semantics | Source says ground commands the Detumbling→Pointing transition, but no OBC→AOCS telecommand mapping, payload, acknowledgement, authorization, or rejection behavior is defined. | No command path exists in `App/aocs`; `aocs_state_to_tlm_mode()` is only a telemetry projection. | Mission operations + AOCS firmware + OBC command owner. | Approved command matrix identifies the ground command, on-board dispatcher, AOCS opcode/payload, preconditions, acknowledgement, timeout, and refusal behavior. HIL test covers command during DET, OFF, FAULT, stale-link, and insufficient-rate cases. |
| State encoding | `AOCS_STATUS` has four names/two bits in `SW_DATA_TYPES.xlsx`, but no numeric code order. `aocs_status.h:21-25` records that absence as an assumption; `aocs_status.h:32-36` contains the current provisional 0..3 declaration. `T_AOCS_MODE` has only 0/1 and no OFF/FAULT encoding. | `aocs_status.h:32-36` declares the current enum and `aocs.h:68-103` conservatively projects OFF/FAULT to 0. Both are explicitly provisional. | AOCS/SYS telemetry owner and ground-segment owner. | Data dictionary/ICD assigns numeric codes and defines OFF/FAULT representation. Contract tests assert those released values; until then tests must label the current values assumptions, not qualification evidence. |
| Producer identity and sensor scaling | AOCS DREP identifies a dedicated STM32L496VGT3 (`AOCS_DREP_ArchitectureFile_V01.pdf:15-25`) and describes IMU/magnetometer algorithms (`:83-113`), while ELE DREP identifies local I2C sensors and the SPI link (`:149-175`). It does not define the exported raw-rate scale, frame timestamp, or sample age. | No decode path exists; `aocs.h` only records “IMU datasheet scale factor”. | AOCS firmware owner. | ICD identifies sensor source, calibration/version, units, signedness, scale, timestamp/age, and range; SIL/HIL vectors and target captures prove conversion and stale handling. |

## Qualification rules until the gaps close

1. Do not add an EPS or AOCS opcode, register, frame length, endian choice, CRC,
   baud, CS pin, polling rate, timeout, retry count, or mode command from this
   repository's code or from a single contradictory document.
2. Keep `bms_poll()` and the AOCS seam fail-closed. A failed, absent, stale,
   malformed, or unvalidated reply must not overwrite a valid snapshot with a
   nominal default and must not enable a safety-gated operation.
3. Treat the data-dictionary `CHG_I2C`/`0x26` information as EPS-local
   ownership evidence. Do not implement an OBC direct charger read without an
   approved electrical interface and register contract.
4. Treat current SPI2/mode/CS settings in `App/bms/bms.c`, the AOCS 20-ms delay,
   and the enum's 0..3 order as provisional implementation assumptions, not
   flight qualification evidence.
5. When the controlled ICD exists, implement one vertical slice at a time:
   fail-first host tests for exact vectors and refusal paths, minimal decoder /
   transport integration, then target/HIL captures and fault injection. Keep the
   old fail-closed path until the new path is independently qualified.

## Exit criteria for this gap record

This document can be retired only when the decision owners attach controlled
EPS↔OBC and AOCS↔OBC ICD revisions that resolve every row above, reconcile the
MCU/gas-gauge contradictions, and provide the acceptance evidence. Until then,
the absence of a production protocol driver is intentional and safer than a
plausible-looking fabricated wire format.
