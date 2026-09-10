# TT&C command frame — spec conformance

Scope: the on-air **telecommand (TC) frame** used on the JOS uplink/downlink
link, and its implementation in `App/comms/comms.c` / `App/comms/comms.h`.

Status of the source: the specification below is transcribed from a subteam
workbook that **has no release status** and contains contradictory revisions
between its own sheets. It is the best available evidence, not an approved
baseline — every choice derived from it is listed, and the open points are
called out explicitly. Do not treat this document as a contract.

---

## 1. Source

| Field | Value |
|-------|-------|
| Document | `TTC packets.xlsx` |
| Site path | SharePoint `J2050space` → `TT&C` → `TTC Operations` |
| Sheets used | the **current** sheet (command/TEC tables) and a sheet `(old)` retained for a previous revision |
| Corroboration | `DB tables.xlsx` (ground database schema) independently lists the same `ECC`, `TEC/TER`, `PL LENGTH`, `UNIX`, `MAC` columns |
| Status | subteam draft, two conflicting revisions in one file — **not approved** |

## 2. Frame layout (current sheet) — transcribed

```
              HEADER = 12 byte
┌──────────────┬──────────────┬──────────────┬──────────────────────┐
│  INFO (4 B)  │ UNIX TIME(4B)│   MAC (4 B)  │   PAYLOAD (0 .. 100 B)│
└──────────────┴──────────────┴──────────────┴──────────────────────┘
       0..3            4..7           8..11            12 ..
```

* **INFO** (4 B), byte by byte:

  | byte | field | encoding |
  |------|-------|----------|
  | 0 | Station ID | `1..255` (0 illegal) |
  | 1 | ECC flag | `0x55` = ECC off, `0xAA` = ECC on |
  | 2 | TEC type + TEC task | bit 1-2 = TEC type (`0..3`), bit 3-8 = TEC task |
  | 3 | PL length | `0..100` |

* **UNIX TIME** (4 B): seconds since the epoch, big-endian on the wire.
* **MAC** (4 B): described only as *"hash to validate GS command"*.
* **PAYLOAD**: `0..100` bytes; the length is carried in INFO byte 3.

### Interleaving / alignment rule

The frame handed to the radio **must be a whole number of 16-byte blocks**
(`16*n`). When ECC is on, a **16-byte RS tail** is appended; when ECC is off,
no tail. The padding to the block boundary is **explicitly an
application-layer responsibility** and uses the value `0`.

### TEC types and the HK command table

TEC types: `HK=1` (housekeeping), `DAQ=2` (data acquisition), `PE=3` (payload
execution), `DT=4` (data transfer).

TEC tasks (uplink HK commands): `0x01` OBC reboot · `0x02` Exit state (2 B:
old + new state) · `0x03` Variable change (uint16 address + float32 value,
repeatable) · `0x04` Set time · `0x08` EPS reboot · `0x10` ADCS reboot ·
`0x11` TLE (43 B) · `0x18` LoRa state · `0x19` LoRa config · `0x1A` LoRa ping ·
`0x31` ACK · `0x32` NACK · `0x33` Lora link.

Variable addresses (VarAddr, R/W via TEC): `1` SoC (R) · `2` Vbat ·
`3-6` Vbat1-4 · `7` V_in · `8` V_out · `9` POK_CLOUD · `10` POK_CRYSTALS ·
`11` POK_AOCS · `12` POK_MAGN · `13` POK_LED · `14` OBC_STATE (R/W) ·
`15` AOCS_STATE (R/W).

### The `(old)` sheet — how it differs

The `(old)` sheet documents a **previous revision** whose INFO field is
**5 bytes** wide and carries `Packets tot` / `Packet ID` in a bitfield. The
current sheet uses the 4-byte INFO above and carries no packet-total/packet-id
fields at all. **The two layouts are not interoperable.** This module
implements the **current** sheet; the `(old)` 5-byte-INFO revision is not
implemented. *Which revision is authoritative has not been confirmed with the
TT&C subteam* (open point O1).

## 3. Specification vs. code (what this change implements)

| Spec element | Before this change | After |
|---|---|---|
| 12-byte header `INFO(4)+UNIX(4)+MAC(4)` | JOS used `opcode(1)+len(1)` for uplink validation and a `msg(1)+seq(1)+total(1)` header for downlink chunking — **no spec command frame existed** | `comms_ttc_build_frame()` / `comms_ttc_parse_frame()` implement the 12-byte header |
| INFO bitfield | absent | `comms_ttc_info_pack()` / `comms_ttc_info_unpack()`, `comms_ttc_info_t` |
| `16*n` alignment + zero padding | not applied to commands | build pads with zeros to the block boundary; parse rejects a non-multiple of 16 |
| ECC flag `0x55`/`0xAA`, 16-byte tail | absent | flag decoded/validated; the 16-byte RS tail is appended (zeroed, reserved) when ECC is on |
| MAC 4 B opaque | JOS on-board auth is a **different** scheme (truncated HMAC-SHA256 + CRC over `opcode|len|payload`) | field carried verbatim, **never computed or verified**; a pluggable `comms_ttc_set_mac_verifier()` seam, fail-closed by default |
| TEC type/task dispatch | closed opcode set `0x01..0x06` | `comms_rx_handle_ttc_frame()` maps `HK` task `0x01`→OBC reboot, `0x02`→Exit state, others documented TODO |
| Error codes / RX statistics | `comms_tc_result_t`, `comms_rx_stats_t` | TT&C verdicts kept in a dedicated `comms_ttc_result_t` and mapped onto the **existing** counters via `comms_ttc_to_tc_result()` |

### Where the code lives

* `App/comms/comms.h` — layout macros, `comms_ttc_result_t`, `comms_ttc_info_t`,
  `comms_ttc_frame_t`, and the codec/seam prototypes.
* `App/comms/comms.c` — the codec, the discriminator
  `comms_frame_is_ttc_layout()`, the RX entry `comms_rx_handle_ttc_frame()` and
  the MAC seam. `comms_rx_handle_frame()` routes a TT&C-layout frame to the
  TT&C path; the legacy/authenticated layouts are unchanged for backward
  compatibility during migration.
* `test/test/test_comms.c` — codec tests asserting **literal** bytes (INFO
  bitfield, padding, ECC tail, truncation, PL length bound, MAC seam).

### What this change deliberately does **not** do

* It does **not** remove `sha256.c` or the existing HMAC-SHA256 uplink
  authentication. The MAC definition is a decision, not a code task (O2).
* It does **not** change the downlink chunking transport (`msg|seq|total` for
  the 128-B beacon). That header is a **downlink transport** concern and is not
  the spec's *command* frame; the beacon (128 B) cannot fit a 100-B-payload
  command frame in any case.

## 4. Open points (unknowns)

| # | Unknown | Impact |
|---|---------|--------|
| O1 | **Which revision is current.** Two revisions coexist in the workbook (`(old)` = 5-byte INFO). Confirmation from the TT&C subteam is pending. | If `(old)` is authoritative, the whole INFO layout changes. |
| O2 | **Semantics of the 4-byte MAC.** The source says only "hash to validate GS command": algorithm, key, coverage (does it include the padding/ECC tail?) and freshness are undefined. JOS already carries a different scheme (truncated HMAC-SHA256). | The seam is opaque and **fail-closed**: frames parse but never dispatch until a verifier is installed. No replay protection exists on the TT&C path. |
| O3 | **INFO byte 2 bit endianness.** The source does not state whether bit 1 is the LSB or the MSB. This code takes **bit 1 = LSB** (`type` in bits 1-2, `task` in bits 3-8). | If the subteam meant MSB-first, the INFO byte's meaning inverts. |
| O4 | **TEC type `DT=4` vs a 2-bit field.** The table lists four types (1..4) but the field is stated as 2 bits (0..3), so `DT=4` is unrepresentable. The code implements the 2-bit field and flags `DT`. | `DT` frames cannot be encoded until the width is confirmed. |
| O5 | **TEC task range.** The text says `0..31` (5 bits) while its own command table uses tasks up to `0x33` (51, 6 bits). The code accepts the full 6-bit width (`0..63`). | If tasks really are 5 bits, the bitfield layout (bits 3-7) differs. |
| O6 | **PHY budget.** A command frame is 16..112 B (128 B with ECC), but the current radio path caps the PHY payload at 64 B (`COMMS_MAX_PACKET`, `COMMS_TC_MAX_FRAME`) and the SRAM2 RX buffer is 64 B. | Frames larger than 64 B cannot actually be received/sent until the LoRa packet budget and the RX buffer are raised, or the frame is interleaved block-by-block. The codec itself is size-correct; the transport is the constraint. |
| O7 | **Padding not validated on RX.** The spec mandates zero padding; the RX parse tolerates any padding value (corruption in the padding region is not detected). | Tolerant by choice; tighten once O2 defines what the MAC covers. |
| O8 | **Payload semantics of `Exit state` / `Variable change`.** The commands' payload encodings are known from the source, but the handler side (state pair application, VarAddr table) is not implemented here. | Dispatcher handles only `OBC reboot` and `Exit state` today; the rest are documented TODOs. |

## 5. References

* `TTC packets.xlsx` — TT&C/TTC Operations (this document's source).
* `DB tables.xlsx` — ground database schema corroborating the frame fields.
* `docs/api/comms.md` — LoRa link, downlink chunking, existing validator/AUTH.
* `App/comms/comms_validate.h` — the pre-existing opcode/CRC/HMAC uplink
  validator (unchanged by this work).
