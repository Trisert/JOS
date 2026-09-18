# Flight qualification gap: CLOUD / ADC / FRAM

## Verdict

Production CLOUD remains **intentionally unwired** in this stacked PR. The
recovered document corpus defines the experiment at a system level, but it does
not provide one released, internally consistent production contract for the ADC
part, face selection, SPI mapping, acquisition semantics, or persistent record
format. Wiring `cloud_init()` or `cloud_task_create()` from `main()` would
therefore require choosing hardware and data-policy values that the documents
do not close.

This is a qualification blocker, not a firmware implementation decision. The
existing first-breach history and bounded SPI1 lock are retained unchanged.

## Source evidence and contradictions

The recovered corpus is explicitly not a released baseline: the recovery report
states that SPF V3 and the DREP documents are subteam versions with TBD fields
and internal inconsistencies (`sp_findings/ICD_RECOVERY_2026-09-12.md:113-119`).
The following evidence is sufficient to bound the work, but not to wire flight
hardware:

| Topic | Corpus evidence | Gap / contradiction |
|---|---|---|
| Experiment and cadence | `RED_SPF_V3.docx`, §3.2.2, Table 3.3 and CLOUD physical architecture (recovered text `RED_SPF_V3.docx.txt:1593-1615`): 16 stripes, two devices on +Y/−Y, 3.3 V, 5.4 mW, expected measurement once per orbit. `RED_SPF_V3.docx`, §1.2.2, Table 1.1 (`:517-529`) and `SW_DREP_Architecture_V01.docx`, §5.2 (`:361-373`) also say periodic ~1×/orbit. | `RED_SPF_V3.docx`, anticipated scientific product text (`:582-586`) says data is collected four times a day for each stripe. The operational trigger, cadence authority, and whether the four-per-day statement supersedes once-per-orbit are not resolved. |
| ADC identity and interface | `RED_SPF_V3.docx`, §3.2.2 (`:1608-1615`) and `SYS_SPF_V3Chapter3Paragraph3.1-3.2_V01.docx`, §3.2.1 (`:122-145`) name **MCP23017 + MAX11228**, with a dedicated payload SPI bus and I2C control. `ELE_DREP_Architecture_V01.docx`, payload interface (`:213-214`) and bus split (`:226-245`) repeat MCP23017 + MAX11228. | `SYS_SPF_V3Chapter3Paragraph3.6.docx`, §3.6.3 (`:81-87`) and `SW_DREP_Architecture_V01.docx`, §3.2 (`:164-170`) instead name **MAX11128 on SPI1**, and the latter describes SPI1 as shared. The corpus never issues a change notice selecting MAX11128 over MAX11228. |
| Two-face selection | `RED_SPF_V3.docx`, §3.2.2 (`:1609-1615`) and the chapter 3.2 copy (`:137-145`) establish two physical CLOUD devices/faces and mention the MCP23017 control channel. `ELE_DREP_Architecture_V01.docx` (`:100`, `:229-232`) says an external I2C GPIO expander controls SPI chip-selects. | No recovered source gives the MCP23017 I2C address, register/bit allocation, face-select electrical function, active levels, ADC CS assignment, power/settle sequence, or whether each face has a separate ADC/CS. A single 16-channel reader cannot be assumed to cover 16 stripes on both faces. |
| SPI mapping and ownership | `RED_SPF_V3.docx`, §3.2.2 (`:1609`) calls the payload link a dedicated payload SPI bus independent of subsystem SPI. `ELE_DREP_Architecture_V01.docx` (`:226-245`) distinguishes payload SPI from subsystem SPI and gives payload connector signals. `SYS_SPF_V3Chapter3Paragraph3.6.docx` (`:81-87`) and `SW_DREP_Architecture_V01.docx` (`:164-170`) give the conflicting SPI1/SPI2 assignment. | The production MCU peripheral mapping (SPI1 versus another SPI instance), CS ownership, DMA transaction contract, and expander-controlled CS wiring are not reconciled. JOS currently shares SPI1 between LoRa and CLOUD in code, but the dedicated payload-bus reading is also present in the corpus. |
| FRAM bus and geometry | `RED_SPF_V3.docx`, §1.2.2 (`:512-515`), `SYS_SPF_V3Chapter3Paragraph3.6.docx`, §3.6.4 Table 22 (`:230-245`), and `SW_DREP_Architecture_V01.docx`, §3.2-3.3 (`:164-178`) describe **4 MB FRAM on SPI2**, with `MB85RS4MT` in the bus list. `ELE_DREP_Architecture_V01.docx`, OBC specification (`:78-100`), names **four FM24VN10-G devices / 4 Mbit** and an I2C-controlled CS architecture. | The repository's current hardware baseline is 4 × FM24VN10-G = 512 KB on I2C1, but the recovered SPF/SW readings say SPI2/4 MB. The flight FRAM part, capacity, bus, address/CS wiring, and cyclic-buffer partition are not one consistent contract. |
| Data product and persistence schema | `RED_SPF_V3.docx`, §1.2.2 Table 1.1 (`:517-529`) and `SYS_SPF_V3Chapter3Paragraph3.6.docx`, Table 23 (`:254-268`) state a CLOUD sample of **528 B (16×2 matrix) plus 32 B overhead**. `RED_FMEA_V2.xlsx`, `FMEA-CLD-EL-01/02` (`RED_FMEA_V2.xlsx.txt:176-198`) only establishes that the ADC/stripes detect impacts and that stripes may be broken at first reading. | No source defines the byte layout, field widths, byte order, record header, timestamp source/units, zero-impact record policy, retry/error record, or monotonic breach rule. The current `cloud_sample_t` is a C struct whose storage size is compiler/layout dependent and `cloud.c` writes `sizeof(cloud_sample_t)`, not a documented 528 B + overhead record. `CLOUD_THRESH`, baseline formation, and raw ADC conversion are code choices, not a recovered production requirement. |
| Transfer behavior | `SYS_SPF_V3Chapter3Paragraph3.6.docx`, §3.6.3 (`:81-87`) says all hardware data flows are interrupt-driven and all SPI transfers use DMA. | The checked-in `MAX11128` implementation uses blocking `HAL_SPI_Transmit`, `HAL_SPI_Receive`, and `HAL_Delay(1)` (`JOS/Core/Src/MAX11128.c:78-96`). It has no transfer error/refusal result. It cannot be treated as the flight CLOUD transaction contract without a target-approved DMA/nonblocking implementation or an explicit exception. |

## Current JOS path (not production integration)

| Entry point / component | Current behavior | Qualification consequence |
|---|---|---|
| `cloud_init()` | `JOS/App/payloads/cloud.c:55-65` clears `last_sample` and `baseline`, configures the checked-in MAX11128 object on `hspi1`, then reads channels 0..15 twice as the two face baselines. | No caller exists in production `main()`. The path assumes one ADC and silently treats both faces as the same channels; no face-selection operation is present (`cloud.c:42-50` has a TODO). |
| `cloud_acquire()` | `cloud.c:67-121` rejects NULL/bus-lock failure, acquires the exported bounded SPI1 lock, reads both faces, releases the lock before breach calculation and `cyclic_buffer_write()`, retains the first-breach timestamp through `last_sample`, and writes only when a new breach is found. | These safety properties are the current scoped behavior and must remain. They do not prove the ADC part, CS/face wiring, raw-data validity, FRAM schema, or target electrical timing. |
| `cloud_task_create()` | `cloud.c:129-169` creates a below-normal `cloud` task and registers it with `WDG_PERIOD_CLOUD_MS`. The task samples only in `STATE_ACTIVE` and sleeps using `obsw_delay_ms()`. | No production caller exists. Enabling it would make the unresolved ADC, cadence, watchdog-failure, and persistence choices flight behavior. |
| `main()` boot/task path | `JOS/Core/Src/main.c:142-151` initializes ADC1, I2C1/2/4, SPI1/2; `:176-203` initializes FRAM/cyclic storage; `:222-241` initializes radio and safety services; `:261-286` creates the existing tasks and never calls `cloud_init()` or `cloud_task_create()`. | Leave this path unchanged until SYS/ELE close the contract. `main.h:67-71` marks PB4 as NJTRST/excluded and PA4 as FRAM CS, while `main.c:816-820` configures PB4 as an input; `cloud.c:18-20` nevertheless claims PB4 as CLOUD CS. This is not a safe basis for production wiring. |
| MAX11128 / ADC | `JOS/Core/Inc/MAX11128.h:169-190` exposes a 16-channel API. `MAX11128.c:78-99` selects a channel, performs polling SPI plus a 1 ms delay, and returns a raw word without an error result. CS is toggled by `MAX11128.c:103-109`. | The checked-in implementation is a useful native-test seam, not evidence of the recovered MAX11228/MAX11128 hardware contract or the required DMA path. |
| FRAM persistence | The repository implementation uses I2C1 and four FM24VN10-G devices (`JOS/App/memory/memory.c:24-47`, `:118-156`); `main.c` calls `fram_init()` before tasks. `cyclic_buffer_write()` is called by `cloud_acquire()` only after releasing SPI1 (`cloud.c:85-119`). | The I2C1 path is internally bounded and testable, but the corpus also specifies SPI2/4 MB. It cannot be called flight-qualified for CLOUD until the physical FRAM baseline and CLOUD record schema are approved. |

## Decision required from SYS/ELE

Approve one signed/released CLOUD interface row (or an authoritative change notice) that answers all of the following without inference:

1. **Part and channel contract:** MAX11228 or MAX11128; channel count, resolution, reference, command/read format, raw-value validity/error indication, and approved driver/DMA behavior.
2. **Electrical mapping:** payload SPI peripheral and pins, ADC CS, MCP23017 I2C bus/address, face-select GPIO bits and active levels, power/settle timing, and the +Y/−Y channel map. Confirm whether the LoRa and CLOUD devices really share SPI1.
3. **Sampling policy:** baseline/calibration procedure, threshold or resistance decision rule, first-reading/broken-strip behavior, cadence (once/orbit versus four/day), ACTIVE-state gate, and failure/refusal policy.
4. **Persistent record:** exact CLOUD record layout, size (including the meaning of “528 B” and “32 B overhead”), endianness, timestamp units, zero-impact policy, and whether `last_sample` history is part of the persisted product.
5. **FRAM baseline:** part number, total usable capacity, bus/peripheral, device addressing/CS, cyclic-buffer region, and the authoritative source that supersedes the conflicting SPI2/4 MB and I2C1/512 KB readings.
6. **Qualification evidence:** target/DMA transaction tests, face-selection test on both physical faces, FRAM write/readback and power-cycle test, and the concurrency test showing the SPI1 lock covers ADC transfers only and is released before calculations and I2C persistence.

**Proposed SYS/ELE decision:** publish a released CLOUD ICD/netlist revision that resolves the six items above, then have software implement only that revision. Until it exists, keep production CLOUD disabled and do not reinterpret the current native doubles or existing JOS comments as hardware approval.

## Exit criteria for a follow-up implementation PR

After the decision is released, a follow-up PR may wire production CLOUD only if it:

- adds a failing host/native contract test for each newly pinned behavior before implementation;
- implements the approved ADC and face-select path with bounded, DMA-compatible transfers;
- preserves first-breach timestamps, the `last_sample` history, and the bounded SPI1 ownership window;
- serializes the approved I2C FRAM record through the existing memory API without holding the SPI1 lock;
- calls `cloud_init()` only at the approved boot point and creates the task only after initialization success;
- adds target/HIL evidence for both faces, ADC identity/CS, DMA, FRAM readback, watchdog behavior, and power-cycle persistence; and
- updates `JOS/docs/api/payloads.md` only to the released contract (never to the unresolved readings above).

No production source or task wiring is changed by this gap PR.
