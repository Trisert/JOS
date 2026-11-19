# JOS system architecture

This is a source-tree architecture map, not an approved system baseline. The
SharePoint document set is authoritative as a whole (`AGENTS.md` §1); where it
is unresolved, this document preserves the uncertainty instead of inferring a
flight interface. Source snapshot reviewed: commit `1a3eb35` (2026-11-10).

## System context

```mermaid
flowchart LR
  GS[Ground station] <-->|LoRa / SX1268<br/>SPI1 + DMA| OBC[OBC: STM32L496VGTx<br/>JOS + FreeRTOS]
  OBC <-->|Subsystem SPI transport documented;<br/>frame / ICD unresolved| EPS[EPS board<br/>telemetry transaction stub]
  OBC <-->|Subsystem SPI transport documented;<br/>frame / ICD unresolved| AOCS[AOCS board<br/>task/driver not wired]
  OBC <-->|I2C1 PB8/PB9| FRAM[4 × FM24VN10-G<br/>512 KB nominal]
  OBC -->|sensor / actuator connections<br/>implementation partial| PAY[Payload hardware<br/>CRYSTALS / CLOUD / CLEAR]
  OBC -->|I2C2| CAM[Camera bus]
```

The drawing distinguishes transport or board context from a functioning
end-to-end service. EPS/AOCS subsystem SPI frame formats are not specified in
the delivered documents and are not implemented as operational polling
protocols. EPS MCU/gas-gauge identity is an open decision (`TASKS.md`); do not
infer it from this diagram. Payload hardware details and electrical pin
assignments remain subject to the OBC netlist and applicable ICDs.

## Boot and task startup

```mermaid
flowchart TD
  R[Reset] --> M[MPU + independent IWDG]
  M --> H[HAL init + SRAM2 parity init]
  H --> P[Clock and peripheral init]
  P --> F[Fault handlers + image CRC check]
  F --> D[Dual-bank policy check]
  D --> I[Initialize BMS plumbing, FRAM, LastStates]
  I --> L[Persist staged boot faults / FRAM probe]
  L --> C[Apply boot CRC policy]
  C --> T[Temperature mux/sensors; radio init; FSM; watchdog; SEU]
  T --> Q[Create default, state-machine, watchdog, LoRa beacon/RX, SEU tasks]
  Q --> S[Start FreeRTOS scheduler]
  S --> W[Watchdog monitor declares boot good after uptime window]
```

Sequence is based on `JOS/Core/Src/main.c`. The boot CRC policy can reset on
failure; after its retry budget it continues in an untrusted, constrained mode
as described by `docs/dev/hardening.md`. The golden-image mechanism exists but
is compile-time inhibited because the bank-2 golden vector location conflicts
with the LastStates reservation; it is not an available flight fallback.
`lora_init()`'s return value is not checked at boot. The AOCS, CLOUD, and CLEAR
task factories are not created by `main()` in this snapshot.

## Runtime and fault/data paths

```mermaid
flowchart LR
  ISR[Radio DIO1 ISR] -->|notification| RX[LoRa RX task]
  RX --> FRAME[Frame layout discriminator]
  FRAME -->|JOS opcode frame| V[CRC / HMAC / opcode / range validation]
  FRAME -->|TT&C layout| TV[TT&C parse + MAC verifier seam]
  V -->|valid| CMD[Command dispatcher]
  TV -->|verified| TEC[TEC registry / supported handlers]
  CMD --> FSM[State machine]
  TEC --> FSM
  FSM -->|normal transition: log before commit| LS[LastStates: internal Flash]
  BMS[BMS cached status] -->|successful poll: update snapshot| FSM
  BMS -. poll transaction not implemented .-> EPS[EPS SPI seam]
  FSM -->|commit state shadow| SEU[SEU mitigation + SRAM2 shadow]
  SCRUB[Separate SEU scrub task] -->|vote and repair| SEU
  INIT[Boot SEU initialization] -->|restore CRC-valid records| SEU
  SEU -->|best-effort state persistence| FRAM[FRAM]
  BEACON[Beacon task] -->|128-byte staging buffer, chunked to max 64 B PHY payload| PHY[SX1268]
  PHY -->|LoRa| GS[Ground station]
  MON[Watchdog monitor] -->|task liveness scan / IWDG refresh| IWDG[Hardware watchdog]
```

The two uplink layouts are separate code paths (`App/comms/comms.c`). The JOS
opcode path validates CRC and, under the flight default, HMAC-authenticates the
frame before dispatch. The TT&C path parses its distinct layout and rejects
unless its MAC-verifier seam accepts; no replay/freshness check is provided.
Neither diagram nor API name should imply encryption. The beacon's 128-byte
buffer is transmitted as staged, not yet fully populated telemetry, in chunks
because the PHY limit is 64 bytes.

## Operational state model

| State | Name | Source-level meaning |
|---|---|---|
| s0 | OFF | Initial/off state |
| s1 | INIT | Boot initialization state |
| s2 | CRIT | Constrained/critical state |
| s3 | READY | Ready/idle state |
| s4 | ACTIVE | Active operation state |

```mermaid
stateDiagram-v2
    [*] --> OFF
    OFF --> INIT: TRIGGER_BOOT
    INIT --> READY: TRIGGER_ANTENNA_DONE
    INIT --> CRIT: boot fault
    CRIT --> READY: battery recovery (valid SoC >= B_OPOK)
    CRIT --> ACTIVE: ground command (valid SoC >= B_OPOK)
    READY --> ACTIVE: ground command / scheduled task
    READY --> CRIT: low battery / critical event
    ACTIVE --> CRIT: low battery / critical event

    note right of INIT
        INIT to READY currently assumes antenna
        deployment and self-test succeeded.
        deploy_fdir_step() is not wired.
    end note

    note right of ACTIVE
        TRIGGER_TASK_COMPLETE (s4 to s3) is defined
        in obsw_types.h but has no transition in
        state_machine.c in this snapshot.
    end note

    note left of CRIT
        Boot-CRC untrusted or SRAM2 parity latch:
        try_transition() allows only CRIT and INIT.
    end note
```

The diagram shows the transitions implemented in `App/obsw/state_machine.c`.
Two operational caveats are part of the source, not decoration: `INIT -> READY`
does not sample `deploy_is_deployed()` and currently assumes a successful
deployment, and `ACTIVE -> READY` on `TRIGGER_TASK_COMPLETE` is absent even
though the trigger is defined in `App/obsw_types.h`. The only autonomous exit
from ACTIVE is CRIT. `CRIT -> CRIT` is a no-op, deliberately not logged, so a
sustained low battery does not write a LastStates record ten times per second.

Transitions are mediated by `App/obsw/state_machine.c`; normal transitions log
to LastStates before state commit. On a log failure `enter_safe_state()` can
force CRIT without a persisted record, prioritizing containment over evidence.
The task loop runs at 10 Hz and attempts a BMS refresh at
1 Hz. A failed BMS poll does not refresh the state-machine snapshot. CRIT
recovery and CRIT→ACTIVE require valid SoC; CRIT→ACTIVE also requires a ground
command. INIT→READY currently assumes antenna
deployment/self-test succeeded, and READY→ACTIVE currently does not enforce a
valid BMS reading. `TRIGGER_TASK_COMPLETE` exists in the type definitions, but
this source snapshot contains no completion transition in the state-machine
implementation. This conflicts with the completed entry in `TASKS.md` for PR #74;
reconcile the discrepancy before relying on s4→s3 task completion.

Do not treat ACTIVE as proof that payload workers are running: the payload and
AOCS task creators are not wired in `main.c`. Payload helper-level actuation
and state interlocks require separate verification; this overview is not an
assurance claim.

## Memory and resilience overview

| Resource | Current documented role | Boundary / caveat |
|---|---|---|
| Internal Flash | Firmware image; LastStates pool at `0x08080000` | Linker reserves application region and LastStates; golden bank fallback inhibited due to address conflict |
| SRAM1 / SRAM2 | RTOS and runtime state; SRAM2 parity-protected critical data | SRAM2 parity init must precede use of protected data |
| External FRAM | Cyclic payload/logging area and SEU golden records | Review exact address ownership before assuming non-overlap or persistence guarantees |
| IWDG + software monitor | Reset backstop and task-liveness supervision | Host doubles do not exercise target suspend/Flash backends |

```mermaid
flowchart TB
  subgraph FLASH["Internal Flash — 1 MB, 0x08000000"]
    APP["Application image<br/>0x08000000–0x0807FFFF<br/>512 KB"]
    LS["LastStates pool<br/>0x08080000–0x08081FFF<br/>8 KB"]
    GOLD["Golden slot (bank 2)<br/>0x08080000–0x080FFFFF<br/>512 KB"]
  end
  GOLD -.->|"overlaps LastStates: DUAL_BANK_GOLDEN_SLOT_AVAILABLE = 0"| LS

  subgraph RAM["SRAM"]
    SRAM1["SRAM1<br/>0x20000000–0x2003FFFF<br/>256 KB"]
    SRAM2["SRAM2<br/>0x10000000–0x1000FFFF<br/>64 KB<br/>.sram2 + .sram2_noinit"]
  end

  subgraph EXT["External FRAM — I2C1 PB8/PB9"]
    FRAM["4 × FM24VN10-G<br/>512 KB total"]
  end
```

The map is not to scale; the addresses are exact from
`STM32L496VGTX_FLASH.ld` and `Core/Inc/dual_bank.h`. The dashed overlap is the
reason the golden-image fallback is compile-time inhibited: bank 2 starts at
the same address as the LastStates pool, so `DUAL_BANK_GOLDEN_SLOT_AVAILABLE`
is `0` and `dual_bank_switch_to_golden()` returns `SWITCH_INHIBITED`. A real
golden image requires relocating the pool (for example to the top of bank 1 or
to FRAM) and updating both the linker script and the ground tooling.

The memory APIs and protection paths have different semantics: LastStates
logging can veto a state transition; FRAM write-through for committed state is
best-effort; cyclic science writes are not equivalent to protected golden
records. See `docs/api/memory.md`, `docs/dev/seu_mitigation.md`, and
`docs/dev/hardening.md` for contracts and limitations.

## Hardware and unresolved interfaces

- OBC: STM32L496VGTx, Cortex-M4F at 80 MHz; 1 MB internal Flash and 320 KB SRAM.
- Radio: SX1268 on SPI1; SPI transfers use DMA. On-air wiring/pin bring-up still
  has a HIL dependency tracked by `TASKS.md`.
- FRAM: four FM24VN10-G devices on I2C1 (PB8/PB9), nominal 512 KB total.
- EPS/AOCS: subsystem SPI transport is described; request/reply formats remain
  unspecified. EPS processor and battery-monitor identifiers conflict across
  delivered sources; tracked in `TASKS.md` and must not be silently resolved.
- Power management: no mission-level Sleep/Stop implementation is present in
  this snapshot. The radio HAL's WFI is a bounded DMA wait, not low-power mode.
- Flight readiness: CI and host tests do not close radio GPIO HIL, interface
  ICD, or licensing gates recorded in `TASKS.md`.

### Bus topology

```mermaid
flowchart LR
  MCU["OBC<br/>STM32L496VGTx"]
  MCU -- "SPI1 — subsystem bus<br/>PA5/PA6/PA7, DMA" --> RADIO["SX1268 LoRa"]
  MCU -- "SPI1" --> EPS["EPS/PDU"]
  MCU -- "SPI1" --> AOCS["AOCS"]
  MCU -- "SPI2_PAY — payload bus<br/>PB13/PB14/PB15" --> CAM["ArduCam OV5642"]
  MCU -- "SPI2_PAY" --> CLOUD["CLOUD MAX11128"]
  MCU -- "I2C1 PB8/PB9" --> FRAM["FRAM 512 KB"]
  MCU -- "I2C2 PB10/PB11 (I2C_CAM)" --> CAM
  MCU -- "I2C4 PD12/PD13 (I2C_EXT)" --> EXP["MCP23017 CS expander"]
  EXP -. "payload CS (I2C address TBD)" .-> CAM
  EXP -. "payload CS (I2C address TBD)" .-> CLOUD
  MCU -. "current code: bms.c binds hspi2 (SPI2) — defect" .-> EPS
  MCU -. "current code: cloud.c binds hspi1 (SPI1) — unresolved" .-> CLOUD
```

Solid edges are the wiring fixed by `JOS.ioc` and the OBC V2.0 gerbers
(`SPI1_BUS`, `SPI2_PAY`, `I2C_CAM`, `I2C_EXT`). Dashed edges are the current
code bindings that differ from that wiring: `App/bms/bms.c` binds the EPS link
to `hspi2` (the payload bus) and `App/payloads/cloud.c` binds the MAX11128 to
`hspi1` (the subsystem/radio bus). Both are open defects or decisions, not the
intended topology; do not enable EPS or CLOUD transactions until they are
reconciled. The MCP23017 strap address (A0–A2) is not in the local corpus.

## Related documentation

- [API contracts](../api/)
- [Build and CI](../dev/building.md), [verification scope](../dev/ci-and-verification.md)
- [Hardening](../dev/hardening.md), [power-mode status](../dev/power_modes.md)
- [Open work and decisions](../../../TASKS.md)
