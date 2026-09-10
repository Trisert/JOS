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
| Sheets used | `Packet structure`, `Task types`, `Task details`, `HK tasks`, `VarAddr`, plus a sheet `(old)` retained for a previous revision |
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
  | 2 | TEC type + TEC task | bit 1-2 = TEC type **MSB-first**, bit 3-8 = TEC task |
  | 3 | PL length | `0..100` |

  **Bit order (was open point O3 — now closed by the workbook itself).**
  `Packet structure`!B22:B26 lists byte 3 as *bit 1 = TEC type* … *bit 8 = TEC
  task*, i.e. bit 1 is the **most** significant bit, so the byte is
  `(tec_type << 6) | tec_task`. `Task details` column D ("TEC bin ID") proves
  it: it is the 8-bit byte made of the 2-bit type Bin ID (`Task types`!C4:C7:
  HK=`00`, DAQ=`01`, PE=`10`, DT=`11`) followed by the 6-bit task Bin ID
  (column C). Column E ("TEC hex ID") is that byte in hex. Worked rows:
  `Task details`!E5 (HK task 1, OBC reboot) = `0x01`; `Task details`!E21
  (HK task 17, TLE) = `0x11`; `Task details`!E55 (HK task 51, Lora link) =
  `0x33`. An earlier revision of this code had it inverted (`type` in the low
  two bits, `task` shifted left by 2), which encoded HK task 1 as `0x05`
  instead of `0x01`.

* **UNIX TIME** (4 B): seconds since the epoch, big-endian on the wire. Parsed
  and exposed in `comms_ttc_frame_t.unix_time`; **not compared to anything**
  (no freshness check — see O2).
* **MAC** (4 B): described only as *"hash to validate GS command"*.
* **PAYLOAD**: `0..100` bytes; the length is carried in INFO byte 3.

### Interleaving / alignment rule

The **DATA section** (header + payload + padding) that is handed to the radio
is a whole number of 16-byte blocks (`16*n`; `Packet structure`!A14 =
*"Packet should be 16*n length for correct interleaving"*). The padding is
zero-filled and is **explicitly an application-layer responsibility**
(`Packet structure`!J44, !J50: *"add padding … so total packet size is 16*N"*).

When ECC is on, a **6-byte Reed-Solomon parity tail** is appended after that
block-aligned data section, so an ECC frame is `16*n + 6`. The 6-byte size is
`Packet structure`!H54 (*RS PARITY = "6 bytes"*). An ECC-off frame carries no
tail.

### Reed-Solomon parity (the RS PARITY element)

Implemented as `comms_ttc_rs_ecc_encode()` in `comms.c` (single translation
unit). It is a systematic **RS(255,249) shortened** code:

* **Field**: GF(256) with primitive polynomial `0x11D`
  (`x^8 + x^4 + x^3 + x^2 + 1`), generator element `alpha = 2`. These are the
  standard RS(255,249) parameters — the same field used, for example, by
  python-`reedsolo`'s defaults (`prim=0x11D`, `generator=2`, `fcr=0`) and the
  systematic-encoding worked example in Wicker & Bhargava, *Reed-Solomon Codes
  and Their Applications*, IEEE Press 1994, ch. 5. They are **cited, not
  invented**.
* **Generator**: `g(x) = prod_{i=0..5} (x - alpha^i)` (6 parity symbols, first
  consecutive root `alpha^0`).
* **Parity byte order**: `parity[0..5]` are the coefficients of the remainder
  from the lowest degree (constant term) upward, appended in that order —
  identical to what `reedsolo.RSCodec(6).encode(data)[-6:]` returns.

**Known-answer validation.** The pinned parity vectors in
`test/test_comms.c` (`test_rs_ecc_known_answer_literals`,
`test_ttc_build_frame_ecc_on_appends_a_six_byte_parity_tail`,
`test_ttc_build_frame_ecc_header_only_matches_kat`) were produced by **two
independent implementations that agree bit-for-bit**:

1. python-`reedsolo` `RSCodec(6)` with default parameters; and
2. an independent GF(256) polynomial-division encoder written for this review.

Pinned vectors (data → 6-byte parity):

| data | parity |
|------|--------|
| `00 01 02 03 04 05 06 07 08 09` (10 B) | `0E 7B A9 55 D2 5A` |
| `00 .. 0F` (16 B) | `19 C6 88 16 C8 89` |
| header-only ECC frame `01 AA 01 00 00 00 00 00 00 00 00 00 00 00 00 00` | `76 EE 55 66 66 67` |
| 32-byte frame (station 3, ECC AA, HK|0x11, PL 5, unix 1, MAC 0, payload `0A 0B 0C 0D 0E`, zero pad) | `70 2A 4E 82 84 A0` |

### TEC types and the HK command table

TEC types are the **2-bit Bin IDs** (`Task types`!C4:C7): `HK=00`, `DAQ=01`,
`PE=10`, `DT=11`. (Column B carries the human-facing Name IDs 1..4; the code
uses the Bin IDs, because those are what the packed byte carries.) **DT is
therefore representable** — the earlier "DT=4 does not fit the 2-bit field"
open point (O4) came from reading column B: closed.

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
| INFO bitfield | absent | `comms_ttc_info_pack()` / `comms_ttc_info_unpack()`, `comms_ttc_info_t`; **bit 1 = MSB**, byte = `(type << 6) \| task` |
| `16*n` alignment + zero padding | not applied to commands | build pads with zeros to the block boundary; parse rejects a non-block-aligned data section |
| **Canonical frame length** | any 16-multiple within range parsed (length smuggling) | parse requires `len == comms_ttc_padded_len(HDR + PL, ecc)` exactly (`COMMS_TTC_ERR_LEN_MISMATCH` otherwise) |
| ECC flag `0x55`/`0xAA`, RS ECC | absent | flag decoded/validated; with ECC on a real **6-byte RS(255,249) parity tail** is computed (known-answer tested) — not zeroed |
| MAC 4 B opaque | JOS on-board auth is a **different** scheme (truncated HMAC-SHA256 + CRC over `opcode|len|payload`) | field carried verbatim; the frame is **verified through a pluggable `comms_ttc_set_mac_verifier()` seam** whose default rejects (fail closed). The MAC bytes themselves are never computed here |
| TEC type/task dispatch | closed opcode set `0x01..0x06` | `comms_rx_handle_ttc_frame()` maps `HK` task `0x01`→OBC reboot, `0x02`→Exit state (applies the requested new state); **unsupported types/tasks and malformed payloads return an explicit rejection verdict** (`COMMS_TTC_ERR_UNSUPPORTED` / `COMMS_TTC_ERR_PAYLOAD`) and are accounted as rejections, never accepted |
| Error codes / RX statistics | `comms_tc_result_t`, `comms_rx_stats_t` | TT&C verdicts kept in a dedicated `comms_ttc_result_t` and mapped onto the **existing** counters via `comms_ttc_to_tc_result()` |

### Where the code lives

* `App/comms/comms.h` — layout macros, `comms_ttc_result_t`, `comms_ttc_info_t`,
  `comms_ttc_frame_t`, the RS encoder prototype and the codec/seam prototypes.
* `App/comms/comms.c` — the codec, the RS(255,249) encoder, the discriminator
  `comms_frame_is_ttc_layout()`, the RX entry `comms_rx_handle_ttc_frame()` and
  the MAC seam. `comms_rx_handle_frame()` routes a TT&C-layout frame to the
  TT&C path; the legacy/authenticated layouts are unchanged for backward
  compatibility during migration.
* `test/test/test_comms.c` — codec tests asserting **literal** bytes (INFO
  bitfield, padding, ECC parity KATs, canonical length, truncation, PL length
  bound, MAC seam, dispatch verdicts, replay pin).

### What this change deliberately does **not** do

* It does **not** remove `sha256.c` or the existing HMAC-SHA256 uplink
  authentication. The MAC definition is a decision, not a code task (O2).
* It does **not** implement freshness / anti-replay (O2).

## 4. Assumptions (to confirm with TT&C)

| # | Assumption | Rationale / source |
|---|-----------|--------------------|
| A1 | The RS parity is a **6-byte tail after** the 16-byte-aligned DATA section, so an ECC frame is `16*n + 6` and the `16*n` rule applies to the data section. | `Packet structure`!H54 fixes the element at 6 bytes; !A14 fixes `16*n`. The sheet's own block diagram (rows 10-14) does not disambiguate whether the `16*n` bound covers the parity. **The main item to confirm.** |
| A2 | The codeword geometry is nominally **10 data + 6 parity = 16** bytes (RS(255,249) shortened), but the encoder computes parity over the whole DATA section, which may span several 16-byte blocks. | `Task types`/`Packet structure`!O10:U11 sketch a 10+6 block; a single tail over the padded data section is the simplest reading that keeps the header contiguous. Confirm the intended granularity (per 16-byte block vs whole data section). |
| A3 | Field/generator: GF(256) poly `0x11D`, `alpha=2`, `g(x)=prod(x-alpha^i)`, parity appended low-degree-first. | Standard RS(255,249) parameters (reedsolo defaults; Wicker & Bhargava 1994). If TT&C used a different `fcr`/primitive, the parity changes. |

## 5. Open points (unknowns)

| # | Unknown | Impact |
|---|---------|--------|
| O1 | **Which revision is current.** Two revisions coexist in the workbook (`(old)` = 5-byte INFO). Confirmation from the TT&C subteam is pending. | If `(old)` is authoritative, the whole INFO layout changes. |
| O2 | **MAC semantics and freshness/replay.** The source says only "hash to validate GS command": algorithm, key and coverage (does it include the padding/ECC tail?) are undefined, and **no freshness/replay protection exists**. `unix_time` is parsed and never used. **This is a blocking item** for a trust boundary. | The seam is opaque and **fail-closed**: frames parse but never dispatch until a verifier is installed. A captured, MAC-valid frame replays cleanly — `test_rx_ttc_replay_is_accepted_today` pins that behaviour so it cannot be forgotten. Replay protection is a system-level decision (recorded here, not implemented). |
| O3 | **INFO byte 2 bit endianness.** — **CLOSED**: `Task details`!D/E prove bit 1 is the MSB and the byte is `(type << 6) \| task`. | Implemented and tested with literal bytes. |
| O4 | **TEC type `DT=4` vs a 2-bit field.** — **CLOSED**: the 2-bit type field carries the `Bin ID` (`Task types`!C7: DT = `11` = 3). | DT is representable; `COMMS_TTC_TEC_DT = 3`. |
| O5 | **TEC task range.** The prose says `0..31` (5 bits) while its own command table uses tasks up to `0x33` (51, 6 bits). The code accepts the full 6-bit width (`0..63`). | If tasks really are 5 bits, the bitfield layout (bits 3-7) differs. |
| O6 | **PHY budget.** A command frame is 16..118 B, but the current radio path caps the PHY payload at 64 B (`COMMS_MAX_PACKET`, `COMMS_TC_MAX_FRAME`) and the SRAM2 RX buffer is 64 B. | Frames larger than 64 B cannot actually be received/sent until the LoRa packet budget and the RX buffer are raised, or the frame is interleaved block-by-block. The codec itself is size-correct; the transport is the constraint. |
| O7 | **Padding not validated on RX.** The spec mandates zero padding; the RX parse tolerates any padding value (corruption in the padding region is not detected by the codec; the RS parity does cover it). | Tolerant by choice; tighten once O2 defines what the MAC covers. |
| O8 | **Payload semantics of `Exit state` / `Variable change`.** Exit state is implemented (2-byte old/new payload, new state applied, short/out-of-range/no-op payloads rejected); the remaining HK commands (variable change, set time, TLE, reboots, LoRa, ACK/NACK) are still documented TODOs and are rejected as unsupported. | Dispatcher handles only `OBC reboot` and `Exit state` today. |

## 6. References

* `TTC packets.xlsx` — TT&C/TTC Operations (this document's source).
* `DB tables.xlsx` — ground database schema corroborating the frame fields.
* python-`reedsolo` (default `RSCodec`) — RS parity cross-check for the KATs.
* Wicker & Bhargava, *Reed-Solomon Codes and Their Applications*, IEEE Press
  1994, ch. 5 — GF(256) systematic encoding.
* `docs/api/comms.md` — LoRa link, downlink chunking, existing validator/AUTH.
* `App/comms/comms_validate.h` — the pre-existing opcode/CRC/HMAC uplink
  validator (unchanged by this work).
