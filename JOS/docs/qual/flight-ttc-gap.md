# TT&C flight-qualification gap register

## Scope and authority

This register audits the TT&C radio, framing, reliability, security, beacon,
and command-dispatch path at `bbd7c49` (the `flight/ttc-qualification`
worktree). It is a qualification record, not a replacement protocol ICD.

The repository's authority order is the whole delivered project-document set,
then `AGENTS.md`/`JOS/docs`, then code (`AGENTS.md:14-42`). The recovered SPF
and DREP files are explicitly described as subteam documents with no release
status (`sp_findings/ICD_RECOVERY_2026-09-12.md:113-119`). The local TT&C API
document separately identifies the workbook as an unreleased, contradictory
source (`JOS/docs/api/ttc-frame.md:8-12`). The recovered corpus is an external
input at `/data/hermes/work/sp_findings`, not a tracked repository directory.
Where those sources disagree, this register records the contradiction and stops;
it does not choose a wire format, retry rule, power mode, or cryptographic
algorithm.

The existing ownership, DMA, DIO1, RX-rearm, and fail-closed radio changes on
`fix/radio-task-ownership` are preserved. This workstream made one narrow,
source-backed verification improvement: the native production-driver test now
captures and asserts the explicit LoRa settings passed to `SX1268::begin()`.
No flight protocol behavior was invented or enabled.

## Qualification matrix

| Area | Verified contract and current implementation | Qualification status / decision needed |
|---|---|---|
| **Frequency** | The recovered SPF quick-facts table specifies 436 MHz (`sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.7-3.8-3.9-3.10-3.12_V011.docx.txt:5-11`). The driver calls `begin(436.0f, ...)` (`JOS/App/comms/radiolib_driver.cpp:188-196`), and the API records 436 MHz as TBC (`JOS/docs/api/comms.md:8-14`). | **Verified at source/config level; TBC remains an external release decision.** Acceptance: target readback/bench capture proves the SX1268 register configuration and RF frequency on the flight hardware. |
| **Bandwidth, SF, coding rate** | SPF specifies BW 125 kHz, SF10, and CR 4/8 (`sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.7-3.8-3.9-3.10-3.12_V011.docx.txt:12-21`). The driver passes `125.0f`, `10`, and RadioLib `cr=8`, whose direct denominator is documented in the driver (`JOS/App/comms/radiolib_driver.cpp:13-15,188-196`). The new native test asserts those four arguments. | **Verified in the production call and native regression.** Acceptance: target/HIL readback plus a ground interoperability vector at 436/BW125/SF10/CR4/8. Do not change `cr=8` to `cr=4`; that would not represent 4/8 in this RadioLib API. |
| **Output power** | The driver passes 22 to `SX1268::begin()` (`JOS/App/comms/radiolib_driver.cpp:188-196`). One recovered SPF table reports `+22 dBm` at the LNA input (`sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.7-3.8-3.9-3.10-3.12_V011.docx.txt:20-25`), while the recovered RED SPF module table reports 31.6 dBm output (`sp_findings/spf_v3/RED_SPF_V3.docx.txt:2723-2738`) and separately describes a reduced-power beacon scenario at +22 dBm (`sp_findings/spf_v3/RED_SPF_V3.docx.txt:2985-2987`). | **Contradictory / not flight-qualified.** The owner must decide whether the software value is module output power, receiver-input link-budget power, or a reduced-power beacon mode; define state transitions, limits, and hardware calibration. Acceptance: target RF power measurement in every approved mode, including beacon mode. No constant was changed. |
| **Application packet/RX budget** | `JOS/docs/api/comms.md:14` documents a 64-byte application packet limit. `COMMS_MAX_PACKET` and the RX/TX SRAM2 buffers are 64 bytes (`JOS/App/comms/comms.h:11-19`; `JOS/App/comms/comms.c:41-43`). The RX path rejects a received packet larger than the caller buffer and never truncates it (`JOS/App/comms/radiolib_driver.cpp:295-317`), while low-level `lora_tx()` accepts lengths through RadioLib's 255-byte API bound (`JOS/App/comms/radiolib_driver.cpp:210-218`). | **Verified for the current 64-byte application RX/staging path, but not reconciled with the TT&C frame.** Acceptance: target test proves exactly-64-byte RX/staging handling and rejection of an oversized received packet without truncation; a separate low-level TX test proves the documented >255-byte refusal. Do not claim direct `lora_tx()` refuses byte 65. |
| **TT&C frame size and fragmentation** | The current workbook-derived codec permits a 12-byte header plus 0–100-byte payload and up to 192 bytes with ECC (`JOS/App/comms/comms.h:109-118,188-197`; `JOS/docs/api/ttc-frame.md:149-150`). The header comment describes a six-byte ECC tail, while the declared layout and encoder place six parity bytes inside each 16-byte block (`JOS/App/comms/comms.h:118,136-146`; `JOS/App/comms/comms.c:404-414`); this internal geometry contradiction is recorded, not resolved here. The downlink beacon contract is 128 bytes, split by `lora_send_chunked()` into 64-byte packets with a 3-byte `msg|seq|total` header (`JOS/docs/api/comms.md:21-25,42-50`; `JOS/App/comms/comms.c:71-129`). The recovered DREP independently says maximum packet 64 bytes, 128-byte beacons, and 64-byte DATA fragments (`sp_findings/spf_v3/SW_DREP_Architecture_V01.docx.txt:417-436,460-481`). | **Major integration gap.** A legal ECC-on command frame can exceed the 64-byte application RX/staging path. The owners must choose either (a) an approved TT&C command subset bounded to one PHY packet, or (b) a specified block fragmentation/reassembly protocol and its authentication coverage. Acceptance: ground/flight vectors for the largest approved frame, loss/reorder/duplicate handling, and proof that no prefix is dispatched. Do not silently reuse the beacon chunk header for command frames: it is not the workbook's 12-byte command header. |
| **PHY CRC and frame integrity** | The SPF states LoRa FEC plus CRC and says CRC detects received-packet interference (`sp_findings/spf_v3/SYS_SPF_V3Chapter3Paragraph3.7-3.8-3.9-3.10-3.12_V011.docx.txt:12-15,42`). The legacy JOS validator checks CRC-16/CCITT before authenticated dispatch (`JOS/App/comms/comms_validate.c:237-244`). The TT&C codec parses the ECC layout and builds RS parity, but deliberately does not decode or verify RS on RX (`JOS/docs/api/ttc-frame.md:213-219,229-241`; `JOS/App/comms/comms.c:484-499`). | **Partial / not flight-qualified.** The owners must define whether SX1268 PHY CRC, TT&C RS parity, and the application MAC all apply, and the order of RS correction/detection versus MAC verification. Acceptance: corrupted PHY packet, corrupted data symbol, corrupted parity symbol, and uncorrectable block vectors each produce the specified refusal and telemetry result. |
| **Downlink reliability, ACK/NACK, retry** | Recovered DREP says telemetry is sent blind, with no GS ACK unless CRC error; it also says SEND_DATA uses 64-byte fragments, NACK on encoding failure, and ACK on success, and RX sends NACK on CRC/decryption failure (`sp_findings/spf_v3/SW_DREP_Architecture_V01.docx.txt:421-438`). JOS chunking aborts on the first radio failure, counts the failed sequence, and the beacon task retries the whole beacon only at the next cadence (`JOS/App/comms/comms.c:101-129,955-973`). RX accounts a PHY/rejected frame and rearms, but does not transmit an ACK/NACK (`JOS/App/comms/comms.c:1028-1057`). The command table leaves NACK length unspecified (`JOS/App/comms/tec.c:41-49,62-64`). | **Contradictory and incomplete.** Resolve telemetry-in-the-blind versus ACK-on-success, define NACK payload/error mapping, sequence identity, timeout, retry count, persistence, and whether retries are per fragment or whole message. Acceptance: deterministic tests for success ACK, each refusal NACK, lost ACK, duplicate ACK/NACK, timeout, retry exhaustion, and partial-message reassembly. No retry semantics were invented. |
| **Encryption, MAC, keying, freshness** | The recovered DREP requires encryption/decryption, whitelist validation, and NACK on invalid packets (`sp_findings/spf_v3/SW_DREP_Architecture_V01.docx.txt:401-426,437-438`); RED SPF says uplinks are encrypted and restricted to authorized stations (`sp_findings/spf_v3/RED_SPF_V3.docx.txt:967-993`). The workbook only calls MAC a 4-byte “hash to validate GS command” and has an unreleased current/old revision conflict (`JOS/docs/api/ttc-frame.md:61-65,171-179`). JOS legacy uplink authentication is a truncated 4-byte HMAC-SHA256 with a public compile-time fallback key (`JOS/App/comms/comms_validate.h:87-127`), while the TT&C frame MAC is opaque and its default verifier rejects every frame (`JOS/App/comms/comms.c:158-181,782-801`). Neither path has replay protection (`JOS/App/comms/comms_validate.h:13-22`; `JOS/docs/api/ttc-frame.md:229-241`). | **Security blocker.** The security owner must approve algorithm, encryption/decryption mode, whitelist semantics, key size/provisioning/rotation, authenticated coverage (including padding/ECC/fragment metadata), freshness/replay state, and failure response. Acceptance: published KATs; wrong-key, modified-header, modified-payload, modified-padding, stale/replayed, unauthorized-station, and sequence-reordering tests; key provisioning must be demonstrated without a public fallback secret. Do not substitute the existing HMAC for the workbook's undefined MAC. |
| **Beacon content and cadence** | The recovered DREP specifies 128 bytes: 96 bytes sensor telemetry plus 32 bytes timestamp/system parameters, on a state-dependent interval (`sp_findings/spf_v3/SW_DREP_Architecture_V01.docx.txt:428-434`; `sp_findings/ICD_RECOVERY_2026-09-12.md:80-99`). JOS allocates a 128-byte beacon and sends it in three chunks, but explicitly has no telemetry encoder and transmits the staging buffer as-is (`JOS/App/comms/comms.c:41-49,952-973`). A separate recovered `SW_DATA_TYPES.xlsx` extraction gives a byte-level housekeeping layout (`sp_findings/TROVATO.md:17-43`), but the recovery report warns that it is not confirmed to be the SPF 128-byte beacon (`sp_findings/TROVATO.md:86-96`). | **Implementation defect plus source ambiguity.** The current transmitted bytes are not proven to be a valid flight beacon. The telemetry owner must select and release one byte layout, endianness, validity/freshness rules, and state cadence. Acceptance: literal 128-byte vectors, field-offset tests, timestamp/system-field tests, non-stale sensor sourcing, and three-fragment ground reassembly. Do not fill the buffer with guessed fields or zeros. |
| **Command dispatch** | The workbook-derived table lists OBC reboot, exit state, variable change, time, EPS/ADCS reboot, TLE, LoRa state/config/ping, ACK/NACK, and LoRa link (`JOS/docs/api/ttc-frame.md:160-169`; `JOS/App/comms/tec.c:22-65`). The TT&C path dispatches only registered handlers for OBC reboot and Exit state; the other documented tasks remain unbound and are rejected (`JOS/App/comms/comms.c:646-655,662-740`). Parsing and MAC verification happen before dispatch, and unsupported/malformed commands are accounted as rejection (`JOS/App/comms/comms.c:782-801`). | **Verified fail-closed behavior; capability incomplete.** Each command owner must provide an approved handler contract, value/range rules, side effects, response, and tests before binding it. Acceptance: every listed task has one positive owner test and refusal tests for malformed, unauthorized, out-of-range, and owner-refused inputs; absent handlers remain explicit rejections. |
| **SET_CONFIG / SEND_DATA** | The legacy command path explicitly recognizes these IDs but returns `COMMS_TC_ERR_UNSUPPORTED` (`JOS/App/comms/comms.c:815-830`), and the API says they are reserved and rejected (`JOS/docs/api/comms.md:34-40`). Recovered material gives intent and partial fields only: SET_CONFIG is described as ADCS/clock/radio configuration and SEND_DATA as requested FRAM data, while the packet workbook has conflicting address widths and an unspecified NACK payload (`sp_findings/followup_20260917/FINDINGS.md:26-42`). | **Correct fail-closed status; do not enable.** The TT&C, payload, and security owners must release the complete wire contract, authorization, bounds, scheduling, memory-selection, fragmentation, ACK/NACK, and resource-ownership behavior. Acceptance: unsupported commands remain rejected until the contract and owner handler are merged; then add end-to-end positive and refusal vectors. |

## Explicit implementation defects and safe disposition

1. **Beacon bytes are not encoded.** `lora_beacon_task()` sends the staging
   buffer while the source requires telemetry/system content. The field layout
   is not uniquely released, so this workstream documents the defect instead of
   inventing an encoder.
2. **The TT&C codec can produce frames larger than the PHY path.** This is a
   transport/protocol boundary mismatch, not a reason to truncate, change the
   64-byte buffer, or invent segmentation. The current driver correctly refuses
   oversize RX payloads.
3. **TT&C ECC is encode-only.** The codec's RS parity construction is test-pinned,
   but RX does not verify/correct it. The source does not state the decoder
   geometry or error policy clearly enough for a safe implementation.
4. **TT&C MAC is fail-closed but not flight-capable.** The default seam rejects
   all commands, which is safer than dispatching with undefined semantics. The
   approved cryptographic contract is missing.
5. **ACK/NACK/retry is not an implemented protocol.** Existing chunk retry is
   only a whole-beacon retry on the next cadence; it is not proof of command or
   DATA retransmission semantics.

No production radio parameter, wire layout, crypto, packet limit, retry rule,
beacon field, or command handler was changed because the available sources do
not provide a single released contract for those choices.

## Changes in this workstream

- `JOS/test/native/test_radio_lifecycle.cpp`: native regression now captures
  and asserts the production driver's explicit 436 MHz / 125 kHz / SF10 /
  RadioLib `cr=8` initialization arguments.
- `JOS/docs/qual/flight-ttc-gap.md`: this source-cited matrix, owner/decision
  register, and acceptance-test list.

`TASKS.md`, `REVIEWS.md`, `JOS/App/payloads`, `JOS/App/bms`, `JOS/App/aocs`,
`main.c`, vendor trees, configuration, credentials, and other worktrees were
not modified.
