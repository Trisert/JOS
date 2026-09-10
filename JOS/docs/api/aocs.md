# AOCS API — Attitude Control (`App/aocs/`)

> **Status on the OBC: STUB.** `App/aocs/aocs.c` is 46 lines. `aocs_init()` is a
> no-op, `aocs_task()` polls nothing (its body is a `TODO` plus the watchdog
> kick), and `aocs_task_create()` is **not called from `main()`**. The AOCS
> control law does not live here — see below.

## Who owns what (corrected)

An earlier revision of this file said *"The OBC also acts as the AOCS
microcontroller"* and documented `iis2mdc_init()`, `asm330lhh_init()`,
`aocs_bdot_step()`, `aocs_ekf_predict()` and `aocs_ekf_update()` as existing
functions with *"Status: integrated"*. **None of those five functions exists
anywhere in `App/`**, and the claim contradicts both the SPF and the header of
`aocs.c` itself.

The SPF's subsystem interface table is explicit about the split:

| Subsystem | Key components | Interface to OBC |
|---|---|---|
| OBC | STM32L496VGT3, 4 Mbit FRAM, 4x TMP1827 | — |
| **AOCS** | **STM32L496VGT3, 2x IMU+MAG, magnetorquers** | **Subsystem SPI (slave)** |
| EPS / PDU | STM32L496VGT3, MP2650, BQ76905, LTC4368 | Subsystem SPI (slave) |

So: the satellite carries a **separate AOCS board with its own STM32L496VGT3**.
B-dot and the nadir-pointing EKF execute *there*. On the subsystem SPI the
**OBC is master and the AOCS is slave**, i.e. the OBC polls and reads; it does
not run the control law. The physical link is the Y-sliding plate, power and
SPI together through a MillMax `854-22-020-20-001101` pogo-pin cableless
connector (SPF §3.3.2). This bus is separate from the payload SPI.

`App/aocs/aocs.c` therefore holds the **OBC side only**: the polling task and
the telemetry it consumes.

## OBC-side contract — what the SPF actually fixes

Two telemetry entries (SPF operational database, "TELEMETRY PARAMETERS",
naming prefix `T_`):

| Telemetry | Pos | Len | Type | Values / scale |
|---|---|---|---|---|
| `T_AOCS_MODE` | 20 | 1 B | Integer | `0` = Detumbling, `1` = Nadir-Pointing |
| `T_ANGULAR_RATE` | 21 | 6 B | Integer (signed) | 3 axes, 2 B per axis, deg/s; raw → deg/s via the IMU datasheet scale factor |

The OBC is required to log `T_AOCS_MODE` in every beacon, and to monitor
`T_ANGULAR_RATE` against a critical-event threshold of **10 deg/s on any
axis**. Ground additionally alerts if detumbling persists longer than expected
(ground-side check).

Rate thresholds, **each with a different role — they are not
interchangeable**:

| Threshold | Value | Where it applies |
|---|---|---|
| Tumbling, X/Y | 10 deg/s | LEOP definition of "tumbling" (SPF §1.5) |
| Tumbling, Z | 20 deg/s | Same definition (see the inconsistency note below) |
| Critical event | 10 deg/s | Any axis, on `T_ANGULAR_RATE` |
| B-dot → EKF handover | 5 deg/s | All axes, in s3/s4 (SPF §3.6.1 notes) |

The constants live in `aocs.h` with their citations. Nothing else about this
interface is defined in code yet, on purpose.

## Reference: what the AOCS board does (not OBC code)

Recorded here because it defines the data the OBC will consume, **not** as
something to implement on the OBC:

- **Detumbling (B-dot)** — uses *only the magnetometers*, differentiated to
  derive the corrective dipole: `m(k) = −k·Ḃ(k)`, with
  `Ḃ(k) = −B(k) × ω₀(k)`. The IMU is used only to decide whether the target
  rates have been reached. Magnetorquer duty cycle 50 %, so the magnetometer
  stays undisturbed during acquisition.
- **Transition to nadir-pointing is commanded from GROUND**, not autonomous.
- **Nadir-pointing (EKF)** — target accuracy ±20°. The EKF uses IMU,
  photodiodes and magnetometers; the magnetometers are put on standby while
  the magnetorquers are working. Once per orbit the software resets the IMU to
  remove drift and reacquires a sun vector, immediately after eclipse, to
  minimise albedo disturbance. The pointing law is a manually tuned PD
  controller; a Sliding Mode Control variant is under study.

## Blockers — what is NOT specified, and why no driver exists yet

Writing an OBC-side driver today would mean **inventing a wire protocol**. The
delivered documentation specifies none of:

- the SPI frame format (request opcode, reply layout, field order);
- the register map / command set, or how the OBC asks for `T_AOCS_MODE` and
  `T_ANGULAR_RATE`;
- the baud rate, clock polarity/phase, chip-select line and pinout;
- the OBC-side polling rate;
- endianness and the numeric scaling beyond *"scale factor from IMU
  datasheet"*;
- timeouts, retry policy, CRC or any reply validation;
- the OBC→AOCS command that changes mode *even though* the SPF says the
  transition is ground-commanded — no telecommand is mapped to it;
- `T_AOCS_MODE` values beyond `0`/`1`;
- how an AOCS power-on-kill is reported.

These belong in an **OBC↔AOCS ICD**. Until it exists, `aocs.h` carries only
the SPF-fixed values, and the driver stays unimplemented.

## Open findings for the team (recorded, deliberately not resolved here)

1. **Tumbling predicate — the SPF contradicts itself.** §1.5 (LEOP): below
   "tumbling" while the rate about X *or* Y exceeds 10 deg/s, **or** about Z
   exceeds 20 deg/s. §3.3.3: "if the angular rate about x or y axis is bigger
   than 10 deg/s **and** the one about z axis is bigger than 20 deg/s". `or`
   vs `and` changes the predicate materially. One of the two must win.
2. **Task priority order.** The SPF's RTOS table gives
   `watchdog > AOCS > comms > state machine > payloads > idle`. The OBC
   currently runs `watchdog` (High) above `state_machine` (AboveNormal) above
   the comms tasks (Normal / BelowNormal) — i.e. **comms and state machine are
   inverted** relative to the SPF, and `aocs` at `osPriorityNormal` is not
   strictly above comms. This is a scheduling change with real consequences
   for the safety path, so it is not being flipped silently.
3. **The bus list omits this link.** The OBSW bus enumeration names
   `OBC ↔ EPS` but not `OBC ↔ AOCS`, even though the interface table lists
   both as subsystem-SPI slaves.
4. **§3.2/§3.3 attribution.** The subsystem/software table attributes
   "B-dot, EKF" to `App/aocs` (OBC firmware), which contradicts the interface
   table placing the AOCS MCU on its own board.

## Next implementable steps (no invention required)

1. Call `aocs_task_create()` from `main()` so the task exists and is
   watchdog-monitored. It is blocked today only by the driver above; creating
   it makes "AOCS monitored" true rather than absent.
2. Consume `T_AOCS_MODE` in the beacon payload once the ICD lands — the
   requirement to log it every beacon is already specified.
3. Everything else waits on the OBC↔AOCS ICD.
