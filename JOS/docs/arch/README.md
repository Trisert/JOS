# System Architecture

This document describes the RedPill (JOS) on-board software architecture for the
STM32L496VGTx target.

## Hardware Context

| Parameter | Value |
|-----------|-------|
| MCU | STM32L496VGTx — ARM Cortex-M4 @ 80 MHz |
| Internal Flash | 1024 KB (firmware reserves 512 KB; LastStates pool 8 KB @ `0x08080000`) |
| Internal SRAM | 320 KB (256 KB SRAM1 + 64 KB SRAM2) |
| External memory | 4 MB FRAM (SPI2) |
| IMU | ASM330LHHXTR (gyro + accel) |
| Radio | Semtech SX1268 (LoRa, SPI1) |
| Watchdog | Independent IWDG (~32 s) |

### Multi-processor split

| Processor | Location | Workload |
|-----------|----------|----------|
| STM32L496 (OBC) | OBC board | Core OBSW: state machine, telemetry, TT&C, payload commanding, AOCS, memory |
| STM32L1 | EPS board | BMS: SoC, per-cell temperature, charge current/voltage, safe-mode triggers |
| Camera MCU | Camera PCB | ArduCam image capture + compression (offloaded from OBC) |

## Software Architecture

- **RTOS:** FreeRTOS (CMSIS-V2, `heap_4`), pre-emptive scheduling.
- **Watchdog task:** monitors all tasks via tick counters; terminates anomalous tasks.
- **Communication:** interrupt-driven SPI (no polling). SPI1 = LoRa + CLOUD; SPI2 = FRAM; I2C1 = magnetometer.
- **Storage:** cyclic buffers. FRAM (4 MB) = primary payload sink; internal Flash = OBSW binary + LastStates pool (8 KB @ `0x08080000`) + beacon/ACK buffers.
- **Chunking:** LoRa max packet 64 B; large objects fragmented on-board, reassembled at GS.

## Operational State Machine

Five-state FSM; all transitions logged to the LastStates pool.

| State | Name | Key activities |
|-------|------|----------------|
| s0 | OFF | Kill switches active; awaiting deployment |
| s1 | INIT | Antenna deployment retry; self-tests; COMMS disabled |
| s2 | CRIT | Low/supercritical battery; charging; beacon every 16 min |
| s3 | READY | Idle; beacon every 4 min; uplink listening |
| s4 | ACTIVE | Payload + PDT execution; beacon every 1–10 min |

### Battery thresholds (from EPS STM32L1 / BQ27441)

| Threshold | ≈ SoC | Behaviour |
|-----------|-------|-----------|
| B_OPOK | 80% | Normal ops; payload + PDT allowed |
| B_COMMOK | intermediate | PDTs allowed; payload suspended; → s2 until B_OPOK |
| B_CRIT | low | Current PDT may finish, then → s2 |
| B_SCRIT | 25% | Ongoing PDT interrupted immediately; → s2 |

## Memory Budget

| Region | Capacity | Contents |
|--------|----------|----------|
| Flash (internal) | 1024 KB | OBSW binary (≤512 KB reserved); LastStates pool (8 KB @ `0x08080000`); beacon/ACK buffers |
| SRAM (internal) | 320 KB | FreeRTOS kernel; task stacks; heap; runtime vars |
| FRAM (external) | 4 MB | All payload data + system logging |

Linker script: `JOS/STM32L496VGTX_FLASH.ld` (FLASH capped at 512K; `LASTSTATES`
region 8K at `0x08080000`).

### Dual-bank golden-image fallback (W2-2)

The STM32L496VGTx is permanently dual bank: bank 1 = `0x08000000`..`0x0807FFFF`
(primary image), bank 2 = `0x08080000`..`0x080FFFFF` (golden image slot). The
`BFB2` option bit makes the boot ROM swap the two banks in the address map, so
the golden image is *linked for `0x08000000`* even though it is stored in
bank 2. `Core/Src/dual_bank.c` falls back to it when the primary image fails
its boot CRC32 or has taken three consecutive boot-phase faults
(NASA-STD-8739.8 graceful degradation, ECSS-Q-ST-80C §6.2.6).

Golden image descriptor (written by ground tooling), last 16 bytes of bank 2:
`magic 'GLDN'` + `length` + `crc32` + `~crc32`.

**Open layout conflict:** the LastStates pool occupies `0x08080000`, i.e. the
exact address the boot ROM fetches the golden vector table from after a `BFB2`
swap. The two cannot share it, so the fallback is *compile-time inhibited*
(`DUAL_BANK_GOLDEN_SLOT_AVAILABLE == 0`) and will never arm `BFB2` into an
unbootable configuration. Relocating the pool (proposal: `0x080FE000`, top of
bank 2) in `App/memory/memory.c`, the `LASTSTATES` linker region and the ground
forensics tooling enables the fallback with no code change.

## TT&C Layer

- **Modulation:** LoRa (CSS), 436 MHz (TBC), SF10, BW125, CR4/8, 610 b/s
- **Max packet:** 64 B
- **Security:** encryption (whitelist + shuffle), CRC on RX, NACK on failure
- **Workflows:** Beacon TX, Data TX (on `SEND_DATA`), RX (uplink listening)

See `docs/api/comms.md` for the LoRa task API.

## AOCS

The OBC also runs AOCS: B-dot detumbling (IMU @ 50 Hz) → Nadir-Pointing EKF
(IMU + IIS2MDC magnetometer fusion, 50 Hz). Three magnetorquers driven via TIM2 PWM.

## ECSS Service Alignment

| ST | Service | Status |
|----|---------|--------|
| ST[02] | Device Access | Y |
| ST[03] | Housekeeping | Y |
| ST[06] | Memory Management | Y |
| ST[08] | Function Management | Y |
| ST[09] | Time Management | Y |
| ST[11] | Time-Based Scheduling | Y |
| ST[12] | On-Board Monitoring | Y |
| ST[13] | Large Data Transfer | Y |
| ST[15] | On-Board Storage | Y |
| ST[17] | Test | Y |
| ST[20] | On-Board Parameter Mgmt | Y |
| ST[23] | File Management | Y |
| ST[01] | Request Verification | N (FreeRTOS priority list used) |
| ST[04]/[05]/[14] | Param Stats / Event Report / RT Forwarding | TBD |
| ST[18] | On-Board Control Procedure | N |
| ST[22] | Position-Based Scheduling | N |

## Known Limitations

| Item | Status | Notes |
|------|--------|-------|
| In-flight RAM update | Not planned | Restricted to 320 KB SRAM |
| Context save/restore across reset | Not planned | Only LastStates pool preserved |
| In-flight SW patching | TBD | No formal post-launch commitment |
| Full command DB consolidation | In progress | Being simplified |
| CRYSTALS voltage calibration | TBC (3 V nominal) | In-orbit validation |

See `docs/api/` for module-level detail and `docs/dev/` for build/verify.
