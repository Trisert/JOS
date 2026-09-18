# Flight payload qualification gap

Status: **not flight-qualified**. This note records the current implementation,
call graph, evidence, unresolved contract decisions, and acceptance tests for
payload scheduling, beacon cadence/encoding, and telemetry layout. It does not
invent a scheduler, wire format, endianness, MAC rule, or telemetry value.

## Qualification conclusion

1. **No payload scheduler is implemented.** `STATE_ACTIVE` is a state-machine
   gate, not a deferred payload dispatcher. `ACTIVATE_PAYLOAD` is currently
   validated as a zero-byte command and only requests `STATE_ACTIVE`.
2. **The full telemetry beacon encoder is not integrated.** The repository has a
   tested 64-byte housekeeping (HB) serializer, but the radio task owns a
   separate 128-byte staging buffer and currently transmits that buffer without
   populating it from an approved 128-byte telemetry model.
3. **The 128-byte model is not defined by one consistent approved source.** The
   recovered SPF says 96 bytes of sensor values plus 32 bytes of timestamp/system
   parameters, while `SW_DATA_TYPES.xlsx` defines a 64-byte, 8-row HB table. A
   mapping between those objects, field scaling, and multi-byte byte order was
   not found.
4. **Beacon cadence has conflicting source readings.** JOS implements 16-minute
   CRIT, 4-minute READY, and 1-minute ACTIVE defaults plus a 10-second-to-16-
   minute override. The recovered corpus also contains an ACTIVE 1–10 minute
   range and a five-minute LEOP/search description. The approved state/cadence
   matrix is still an owner decision.
5. **Payload APIs are mostly isolated from production activation.** `main()`
   initializes the temperature bus and creates the state/watchdog/radio tasks,
   but it does not initialize or create CLEAR/CRYSTALS payload tasks and has no
   payload scheduler creation path.

Because the missing contracts are flight interfaces, no C integration was added
in this workstream. A documentation-only qualification gap is safer than
fabricating an interface.

## Evidence matrix

| Item | Current JOS behavior | Evidence | Status / owner |
|---|---|---|---|
| HB object | `beacon_build_hb()` emits exactly 64 bytes, packs the documented STATUS/PAYLOADS bitfields, writes the GPS/temperature fields, and zeroes reserved bytes. | `JOS/App/obsw/beacon.h:14-105,111-177,334-343`; `JOS/App/obsw/beacon.c:139-195`; `JOS/test/test/test_beacon.c:93-556` | Unit-qualified only against the working `SW_DATA_TYPES.xlsx` reading. TT&C/SW must approve the interface and byte order. |
| 128-byte beacon | `COMMS_LORA_BEACON_SIZE` is 128, but the task obtains the staging buffer and sends it as-is; no call reaches `beacon_build_hb()` or another full-frame encoder. | `JOS/App/comms/comms.h:11-29`; `JOS/App/comms/comms.c:952-975`; `JOS/App/obsw/beacon.c:139-195` | **Blocking.** SYS + TT&C/SW must publish the 96-byte sensor and 32-byte system map, source ownership, scaling, and byte order. |
| 128 vs 64 bytes | SPF V3 says a beacon carries 128 bytes: 96 bytes of sensor values and 32 bytes of timestamp/system parameters. The HB workbook is an 8×8-byte table (64 bytes) and does not state how it composes the 128-byte object. | `/data/hermes/work/sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.5_V01.docx.txt:18-34`; `/data/hermes/work/sp_findings/SW_DATA_TYPES.xlsx:60-72`; `/data/hermes/work/sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.6.docx.txt:254-292` | **Blocking contradiction.** SYS owns the baseline selection. |
| Telemetry schema | The HB workbook uses 8-bit `BAT_SOC`/`V_BAT`, a 2-bit four-state `AOCS_STATUS`, and GPS/HB positions. The separate operational `T_*` table uses different widths and positions, including 2-byte SoC, `T_AOCS_MODE` at position 20, and timestamp/uptime at positions 32/36. | `/data/hermes/work/sp_findings/SW_DATA_TYPES.xlsx:2-49`; `/data/hermes/work/sp_findings/SW_DATA_TYPES.xlsx:60-72`; `/data/hermes/work/sp_findings/spf_v3/RED_SPF_V3.docx.txt:1216-1342` | **Blocking contradiction.** SYS + TT&C/SW must select one decommutation ICD and release a golden vector. |
| Multi-byte order and meaning | HB source does not state endianness; JOS currently emits little-endian GPS/timestamp fields and treats timestamp/scaling as pass-through. The header explicitly marks this unresolved. | `JOS/App/obsw/beacon.h:75-105`; `JOS/App/obsw/beacon.c:20-41,166-193`; `/data/hermes/work/sp_findings/SW_DATA_TYPES.xlsx:15,49` | **Blocking.** TT&C/SW must decide endianness, epoch/unit/scaling, and signedness before vector qualification. |
| Cadence defaults | JOS returns CRIT=16 min, READY=4 min, ACTIVE=1 min unless overridden. Non-zero overrides are accepted only in [10 s, 16 min], and zero clears the override. | `JOS/App/obsw_types.h:98-115`; `JOS/App/obsw/state_machine.c:660-706`; `JOS/test/test/test_state_machine.c:520-575` | Defaults are tested as current project behavior, not a fully approved flight matrix. SYS + TT&C must resolve alternatives and define whether the interval is start-to-start or transmit-complete-to-next-start. |
| Cadence source conflict | SPF state tables give 16/4/1-minute defaults; SW DREP gives ACTIVE 1–10 minutes; SPF quick facts say five-minute beacons during satellite search. | `/data/hermes/work/sp_findings/spf_v3/RED_SPF_V3.docx.txt:644-725,945-965`; `/data/hermes/work/sp_findings/spf_v3/SW_DREP_Architecture_V01.docx.txt:428-434`; `/data/hermes/work/sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.6.docx.txt:285-288` | **Blocking contradiction.** SYS owns the approved state/phase/cadence matrix. |
| Scheduled activation | The recovered operational table describes `ACTIVATE_PAYLOAD` as payload ID (1 B) plus optional delay (4 B, milliseconds), valid in READY/ACTIVE. It also says time-based scheduling is TBD and position-based scheduling is not implemented. | `/data/hermes/work/sp_findings/spf_v3/RED_SPF_V3.docx.txt:1054-1101,1209-1215`; `JOS/App/comms/comms_validate.c:45-53`; `JOS/App/comms/comms.c:827-832` | **Blocking.** SW/SYS + payload owners must approve command shape, queue/expiry/duplicate rules, and completion/ACK/NACK semantics. |
| State gates | READY/CRIT to ACTIVE is guarded by valid battery telemetry at or above B_OPOK; CRIT to ACTIVE additionally requires a ground trigger. JOS has no payload dispatch after the state request. | `JOS/App/obsw/state_machine.c:181-239,422-437`; `JOS/App/comms/comms.c:824-832`; `/data/hermes/work/sp_findings/spf_v3/RED_SPF_V3.docx.txt:869-905` | The gate is testable now; payload execution and task-completion integration are not. Numeric B_COMMOK/B_CRIT values remain unspecified in the source set. |
| Payload activation path | `main()` calls `temp_init()` and creates state/watchdog/radio/RX/SEU tasks. It does not call CLEAR/CRYSTALS initialization, create `clear_task`, or create a scheduler. | `JOS/Core/Src/main.c:217-285`; `JOS/App/payloads/clear.c:46-55,122-161`; `JOS/App/payloads/crystals.c:39-103`; `JOS/docs/api/payloads.md:1-29` | **Blocking integration gap.** Flight software owner + payload owners must approve task creation, priorities, watchdog periods, and power-state entry conditions. |
| Payload production cadence | CLEAR implements a bounded one-orbit burst API and task loop; CRYSTALS exposes direct control/read APIs. These APIs have no production command dispatcher or telemetry producer in the allowed scope. | `JOS/App/payloads/clear.c:99-161`; `JOS/App/payloads/crystals.c:49-103`; `JOS/docs/api/payloads.md:20-29`; `/data/hermes/work/sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.5_V01.docx.txt:18-23` | Payload owners must define command-to-API mapping, state gating, data validity, and telemetry ownership. |

## Current call graph

### Boot and radio beacon path

```text
main()
  ├─ temp_init()
  ├─ lora_init()
  ├─ state_machine_init()
  ├─ state_machine_task_create()
  ├─ watchdog_task_create()
  ├─ lora_beacon_task_create()       [only when lora_init() succeeds]
  ├─ lora_rx_task_create()
  └─ seu_scrub_task_create()

lora_beacon_task()
  ├─ state_machine_get_beacon_interval()
  ├─ watchdog_register_task()/watchdog_alive_self()
  ├─ comms_beacon_buffer()           [128-byte staging buffer]
  ├─ lora_send_chunked()             [3-byte chunk header; 3 chunks for 128 B]
  └─ osDelay(interval)

No edge exists from lora_beacon_task() to beacon_build_hb() or a full 128-byte
telemetry encoder. The task comment explicitly marks encoding TODO and says the
staging buffer is transmitted as-is (`JOS/App/comms/comms.c:952-975`).
```

### Payload APIs

```text
clear_task_create()
  └─ clear_task()
       ├─ clear_read_pd(CLEAR_FACE_Z)
       ├─ clear_read_pd(CLEAR_FACE_MZ)
       └─ cyclic_buffer_write() on burst stop/full burst

crystals_init()/crystals_enable()/crystals_manage_heater()
crystals_take_photo()/crystals_read_obscuration()
  └─ direct APIs only; no production scheduler/command edge

temp_init()
  └─ 1-Wire enumeration only; no production periodic telemetry edge
```

The module documentation confirms that CLOUD is not called from production
`main()` and that command/telemetry consolidation is in progress; this note does
not change or audit `cloud.c`/`cloud.h` in accordance with the workstream scope
(`JOS/docs/api/payloads.md:9-18`).

### Activation path

```text
lora_rx_task()
  └─ comms_rx_handle_frame()
       └─ comms_validate_tc[_auth]()
            └─ ACTIVATE_PAYLOAD: payload length exactly 0
                 └─ state_machine_request_transition(STATE_ACTIVE, GROUND_CMD)
                      └─ state gate / LastStates record

No payload ID decode, delay queue, timer expiry, payload dispatch, or task
completion signal is connected to this path.
```

## Required decisions before implementation

1. **Telemetry/beacon ICD — owner: SYS + TT&C/SW.** Select the 128-byte
   composition, the 96-byte sensor field map, the 32-byte timestamp/system map,
   field widths/scaling/signedness, multi-byte order, timestamp epoch/unit, and
   whether the 64-byte HB table is a separate product or a subframe.
2. **Cadence matrix — owner: SYS + TT&C.** Resolve 16/4/1 minutes versus the
   1–10-minute ACTIVE range and five-minute search description. State whether a
   configured interval is measured between beacon starts or after TX completion,
   and define reset/persistence semantics for the override.
3. **Payload command contract — owner: SW/SYS + payload owners.** Define the
   payload-ID namespace, optional delay encoding and bounds, queue depth,
   duplicate/cancel/expiry behavior, READY/ACTIVE admission, battery-loss
   behavior, task completion trigger, and ACK/NACK contents. Do not infer these
   from the zero-length placeholder currently accepted by JOS.
4. **Activation integration — owner: flight software + payload owners.** Define
   which tasks are created at boot, their watchdog periods/priorities, the
   state/power interlocks, and how invalid or unavailable payload hardware is
   reported without starting an unsafe operation.

## Acceptance tests

These tests are the minimum evidence once the owners close the decisions above.
Tests that depend on an unresolved value remain blocked rather than asserting a
local assumption.

1. **HB serializer vector (available now).** Populate every HB field with
   distinct values; assert exactly 64 bytes, literal `SW_DATA_TYPES.xlsx` row/
   byte offsets, STATUS/PAYLOADS masks, and zeroes in row 1 byte 8, rows 2–4,
   and row 5 bytes 5–8. Keep endian-dependent assertions marked blocked until
   the owner decision. Existing test: `JOS/test/test/test_beacon.c`.
2. **Full beacon golden vector (blocked).** With the approved 128-byte ICD,
   encode all 96 sensor bytes and 32 system bytes, assert the exact vector and
   that no staging-buffer byte is transmitted before a successful encode.
3. **Beacon transport reassembly (partly available).** Feed the approved
   128-byte vector through the actual chunker, assert three framed chunks with
   the expected message/sequence/total values, reassemble byte-for-byte, and
   assert partial-radio failure is detectable and retried as a whole message.
4. **Cadence state matrix (blocked on approval).** Measure the defined interval
   in CRIT, READY, ACTIVE, and override/clear cases; cover state transitions,
   watchdog re-registration, the [minimum,maximum] boundary, rejected values,
   and the chosen start-to-start/completion-to-start interpretation.
5. **Deferred activation (blocked).** Build valid and invalid
   `ACTIVATE_PAYLOAD` commands with payload ID and optional millisecond delay;
   assert bounds, queueing, duplicate/expiry behavior, execution without LOS,
   battery/state rejection, completion signal, and ACK/NACK verdicts.
6. **Production activation (blocked).** On the STM32 path, prove the approved
   payload task creation/initialization sequence, watchdog registration,
   state/power interlocks, and refusal on missing/invalid hardware. Exercise
   INIT/CRIT/READY rejection, READY/ACTIVE acceptance, unknown/stale battery,
   and the B_OPOK boundary.
7. **Telemetry decommutation (blocked).** Compare a ground decoder against the
   approved 128-byte golden vector for every field; separately prove that the
   64-byte HB object is not mistakenly treated as the 128-byte beacon.
8. **Reset and persistence (blocked).** Change cadence/configuration, reset,
   and verify the owner-approved persistence/discard behavior. Explicitly test
   whether queued payload work is retained or discarded across reset.
9. **Target/HIL evidence (blocked).** Verify beacon start timing under the real
   SX1268/DMA path and contention, and verify CLEAR/CRYSTALS GPIO/ADC behavior
   and payload activation on the actual STM32 hardware. Host tests do not close
   these electrical/timing claims.

## Verification boundary

The repository's existing qualification note already states that no payload
scheduler or telemetry encoder was invented and that those integration blockers
remain (`JOS/docs/qual/direct-review-safety.md:54-63`). This workstream preserves
that boundary. The missing contracts are not safely fillable from the recovered
subteam drafts because those documents have no release status and contain the
contradictions cited above.
