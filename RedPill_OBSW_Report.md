# RedPill PocketQube — On-Board Software Report

**Project:** J2050 – New Epoch Technologies  
**Institution:** University of Padova, Italy  
**Programme:** ESA Fly Your Satellite! 4  
**Based on:** RED\_SATPF\_2024-12-22\_V2.0 (Satellite Project File)  
**Software Team Leader:** Nicola Destro  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [OBSW Quick Facts](#2-obsw-quick-facts)
3. [Hardware Context and Multi-Processor Architecture](#3-hardware-context-and-multi-processor-architecture)
4. [On-Board Software Architecture](#4-on-board-software-architecture)
5. [Memory Budget](#5-memory-budget)
6. [TT&C Software Layer](#6-ttc-software-layer)
7. [Payload Software Interfaces](#7-payload-software-interfaces)
8. [System Logging — The LastStates Pool](#8-system-logging--the-laststates-pool)
9. [On-Board Services (ECSS Alignment)](#9-on-board-services-ecss-alignment)
10. [AOCS Software Integration](#10-aocs-software-integration)
11. [Ground Segment Software](#11-ground-segment-software)
12. [Software Development and Verification Plan](#12-software-development-and-verification-plan)
13. [Known Limitations and Open Items](#13-known-limitations-and-open-items)
14. [Conclusions](#14-conclusions)
15. [Implementing the OBSW Using STM32CubeIDE and Claude Code](#15-implementing-the-obsw-using-stm32cubeide-and-claude-code)

---

## 1. Executive Summary

The RedPill PocketQube satellite carries three scientific payloads — CRYSTALS, CLOUD, and CLEAR — on an in-house On-Board Computer (OBC). The On-Board Software (OBSW) is one of the most critical subsystems: it drives every operational mode, manages all payload interactions, controls communications, and ensures the satellite's survival throughout its planned 1.5-year mission lifetime in Low Earth Orbit.

This report provides a fully detailed account of the software architecture, design decisions, memory and CPU budgets, communication protocols, development process, and verification strategy as documented in the Satellite Project File v2.0 (22 December 2024).

> **Key highlight:** The software team deliberately separated workloads across dedicated microcontrollers (OBC, EPS, camera) to avoid CPU overload on a single resource-constrained STM32L4 processor.

---

## 2. OBSW Quick Facts

| Parameter | Value / Description |
|---|---|
| Kernel / RTOS | FreeRTOS (latest stable version) |
| Programming Language | C |
| Coding Standard | MISRA guidelines for maintainability and safety |
| Primary Development IDE | STM32CubeIDE (Eclipse/CDT + GCC + GDB) |
| Secondary Tools | Arduino IDE (payload prototyping), MATLAB R2023 (simulation) |
| Initial Dev Hardware | STM32L562E-DK development boards |
| Target Hardware (OBC) | STM32L4 low-power microcontroller |
| Version Control | GitHub |
| COTS Software | None — all in-house, except the ArduCam camera library |
| Team Size | 4 members (2 core OBSW developers) |
| Anomaly Detection | Dedicated watchdog task using FreeRTOS tick counters |

---

## 3. Hardware Context and Multi-Processor Architecture

### 3.1 On-Board Computer (OBC)

The OBC is developed entirely in-house. Key parameters:

| OBC Parameter | Value |
|---|---|
| Weight | 23 g |
| Dimensions | 48.5 × 48.5 mm |
| Peak Power | 50 mW |
| Thermal Range | −40°C to +85°C |
| Processor | STM32L4 (ARM Cortex-M4, low-power) |
| Analog Inputs | 30 |
| IMU | Integrated (ASM330LHHXTR — gyros + accelerometers) |
| Internal Flash | 1024 KB |
| Internal RAM | 128 KB |
| External Memory | 4 MB FRAM |
| Hardware Watchdog | Yes |
| Communication Bus | SPI (to TT&C, CLOUD, EPS, CLEAR) |

### 3.2 Multi-Processor Distribution Strategy

Computational workload is deliberately split across three microcontrollers:

| Processor | Location | Workload |
|---|---|---|
| STM32L4 (main OBC) | OBC board | Core OBSW: operational states, telemetry, TT&C, payload commanding, memory management, AOCS command dispatching |
| STM32L1 | EPS board | Battery Management System (BMS): SoC monitoring, temperature per cell, charging current/voltage, safe-mode triggers via SPI |
| Camera MCU | Camera PCB | ArduCam B0068 image acquisition, compression, and transfer — completely offloaded from OBC |

This separation ensures that image acquisition and power management do not compete with critical OBSW on the main processor. The AOCS control law is also executed on the main OBC, which represents the second most resource-intensive workload after the camera.

> **Design rationale:** Literature review of successful picosatellite missions confirmed that the combined workload across three boards stays well within the available computing budget. This split also provides a patching opportunity per board if errors occur post-launch.

---

## 4. On-Board Software Architecture

### 4.1 Operating System: FreeRTOS

FreeRTOS is used in its latest stable version, natively supported by STM32CubeIDE. It provides:

- Pre-emptive task scheduling with priority levels
- Tick-based timing for watchdog counters and periodic tasks
- Semaphores and queues for inter-task communication
- Low-power idle modes compatible with STM32L4 sleep states

A dedicated anomaly detection task monitors all other tasks via FreeRTOS tick counters. Any task whose tick count deviates significantly from its nominal profile is terminated as dormant or anomalous.

### 4.2 Interrupt-Driven Communication

All hardware data flows use an interrupt handling approach — no polling. The SPI bus is the primary communication backbone, connecting:

- OBC ↔ LoRa1268F30 transceiver (TT&C)
- OBC ↔ CLOUD debris detector (ADC readings)
- OBC ↔ EPS / BMS (STM32L1 on EPS board)
- OBC ↔ CLEAR photodiodes and LED driver

### 4.3 Storage Strategy: Cyclic Memory Management

The OBSW uses a ring-buffer approach for all persistent memory writes, assuming data has been downloaded to the Ground Station before the pointer wraps around:

- **External FRAM (4 MB):** Primary payload data sink. Overwrites oldest data first.
- **Internal Flash (1024 KB):** Hosts the OBSW binary, LastStates pool (8 KB), beacon buffers, and ACK buffers. Non-critical data may be discarded on overflow; the OBSW image and LastStates pool are always preserved.
- **Internal RAM (128 KB):** Runtime stack, heap, and FreeRTOS kernel structures. Not persistent across resets.

> **Memory overflow policy:** Graceful degradation — oldest data overwritten; no system fault triggered.

### 4.4 Chunked Data Transfer

The LoRa radio enforces a maximum packet size of 64 bytes. All large data objects (camera images, extended telemetry, bulk CLOUD data) are fragmented on-board and reassembled at the Ground Station. This chunking logic is transparent to higher-level payload and telemetry services.

### 4.5 Operational State Machine

The OBSW manages satellite behaviour through a five-state finite state machine. All transitions are logged to the LastStates pool.

| State | Name | Description | Key Activities |
|---|---|---|---|
| s0 | OFF | System fully off; kill switches activated | None — awaiting deployment |
| s1 | INIT | Boot/wake-up state; manages antenna deployment and start-up checks; COMMS disabled | Antenna deployment retry loop; timer init; self-tests |
| s2 | CRIT | Entered on critical event, reboot, or low/supercritical battery; non-vital systems suspended | Battery charging; beacon every 16 min |
| s3 | READY | Post-task idle; satellite available for uplink; controllers in low-power mode | Beacon every 4 min; uplink listening; housekeeping |
| s4 | ACTIVE | Priority given to payload and PDT operations; controllers in medium-power mode | Payload execution; PDT transmission; scheduled task dispatch |

#### 4.5.1 State Transition Rules

| Transition | Type | Trigger Condition |
|---|---|---|
| s1 → s3 | Autonomous | Successful boot and antenna deployment |
| Any → s2 | Autonomous or Manual | Low battery (B_COMMOK threshold) / critical event / ground command |
| s2 → s3 | Autonomous | Battery recovers to B_OPOK (~80%) and no active critical events |
| s3 → s4 | Automatic or Manual | Scheduled payload task begins / ground ACTIVATE_PAYLOAD command |
| s4 → s3 | Autonomous | Task completion |
| s2 → s4 | Manual only | Ground command + stable battery + resolved critical events |

### 4.6 Battery Management Integration

Four SoC thresholds (provided by the BQ27441 Gas Gauge via the EPS STM32L1) govern operational restrictions:

| Threshold | Approx. SoC | OBSW Behaviour |
|---|---|---|
| B_OPOK | ~80% | Normal operations; payload tasks and PDTs allowed |
| B_COMMOK | Intermediate | PDTs still allowed; ongoing payload tasks suspended; OBSW enters s2 until B_OPOK reached |
| B_CRIT | Low | Current PDT may complete, then s2 entered |
| B_SCRIT | ~25% | Any ongoing PDT immediately interrupted; OBSW enters s2 |

---

## 5. Memory Budget

### 5.1 Memory Map

| Memory Region | Capacity | Allocated Contents |
|---|---|---|
| Flash (STM32L4 internal) | 1024 KB | OBSW binary; LastStates pool (8 KB); beacon buffers; ACK buffers |
| RAM (STM32L4 internal) | 128 KB | FreeRTOS kernel; task stacks; heap; runtime variables |
| External FRAM | 4 MB | All payload data (camera, CLOUD, CLEAR photodiodes, IMU, temperature); system logging |

### 5.2 Data Production Rates

| Data Source | Packet Size | Overhead | Sampling Interval |
|---|---|---|---|
| Camera Unit | Variable (resolution + compression) | 32 B | On command |
| CLOUD Debris Detector | 528 B (16×2 matrix) | 32 B | Periodic, ~1×/orbit |
| CLEAR Photodiodes | Variable | 32 B | On command (one orbit) |
| IMU (AOCS) | Variable | 32 B | On command (one orbit) |
| Temperature Sensors | Variable | 32 B | On command (one orbit) |
| LastStates Pool | 128 B per entry (max 64) | 32 B | On critical events / state transitions |
| Beacons | 128 B | 32 B | Periodic (every 1–16 min, state-dependent) |
| ACK Signals | 8 B | 4 B | On command receipt |

**Fill-time estimates:**

- **FRAM (4 MB):** ~4 days at 1 MB/day average payload data rate
- **LastStates pool:** ~64 hours (2.67 days) at one new state per hour; oldest entry then overwritten
- **Flash beacon/ACK buffer:** ~5.5 days at 1 beacon/min + 1 ACK/10 min

> **Design margin:** A minimum 20% margin is reserved on both Flash and FRAM.

---

## 6. TT&C Software Layer

### 6.1 Radio Link Summary

All TX/RX processing — packet framing, CRC checking, encryption/decryption — is implemented in the OBSW on the OBC. No independent TT&C microcontroller is present.

| Parameter | Value |
|---|---|
| Modulation | LoRa (CSS) |
| Frequency | 436 MHz (TBC), UHF amateur band 435–438 MHz |
| Coding Rate (CR) | 4/8 (50% FEC redundancy) |
| Spreading Factor (SF) | 10 |
| Bandwidth | 125 kHz |
| Data Rate | 610 b/s |
| Maximum Packet Size | 64 bytes |
| Error Detection | CRC on all received packets |
| Uplink Security | Encryption (whitelisting + shuffle algorithm) |
| Downlink Mode | Telemetry-in-the-blind (no ACK from GS unless CRC error) |

### 6.2 Three Parallel TT&C Workflows

#### Beacon TX
Transmitted autonomously on a state-dependent interval:

- s2 (CRIT): every 16 minutes
- s3 (READY): every 4 minutes
- s4 (ACTIVE): every 1–10 minutes depending on available power

Each beacon carries 128 bytes: 96 bytes of sensor telemetry and 32 bytes of timestamp plus system parameters.

#### Data TX
Activated exclusively by a ground telecommand (SEND_DATA). The OBSW retrieves data from FRAM, fragments it into 64-byte chunks, and transmits sequentially. NACK sent on encoding failure; ACK sent on success.

#### RX (Uplink Listening)
When not transmitting, the satellite listens continuously. Each received packet is processed as follows:

1. **CRC check** — if CRC fails, send NACK and halt processing.
2. **Decryption** — validate against whitelist; invalid packets result in NACK.
3. **Command dispatch** — extract and execute the embedded command.

### 6.3 Telecommand Set

| Telecommand | Description |
|---|---|
| RESET | Soft reset of the OBC |
| EXIT_STATE | Exit a recovery state and resume operations |
| SET_CONFIG | Upload new configuration (ADCS settings, clock sync, LoRa parameters) |
| SET_DOWNLINK | Enable or disable all transmissions (allows commanding-in-blind) |
| SEND_CONFIG | Request the satellite to transmit its current configuration |
| SEND_DATA | Request specific payload data from OBC FRAM |
| SEND_TELEMETRY | Request an extended telemetry packet immediately |
| ACTIVATE_PAYLOAD | Execute a specific payload action (with optional time delay) |

Every packet may include a set-delay field, enabling scheduling of out-of-view actions.

### 6.4 Downlink Packet Types

| Packet Type | Contents | Size |
|---|---|---|
| ACK | 4 B ID + 4 B reserved | 8 B |
| NACK | Error indicator on CRC/decryption failure | 8 B |
| BEACON | 96 B sensor telemetry + 32 B timestamp and system parameters | 128 B |
| TELEMETRY | Extended health data on SEND_TELEMETRY command | Variable |
| CONFIG | Current satellite configuration on SEND_CONFIG command | Variable |
| DATA | Requested payload data in ≤64 B fragments | ≤64 B/packet |

---

## 7. Payload Software Interfaces

### 7.1 CRYSTALS

CRYSTALS is an electrochromic device. The OBSW must:

- Apply a 3 V potential difference to initiate the darkening/clearing cycle (TBC)
- Activate the integrated copper coil heater (5 mW) when temperature is below +5°C
- Command the ArduCam B0068 (via camera MCU) to capture images for transmittance measurement
- Read photodiode pairs (Vishay VBPW34S/VBPW34SR): front measures incident light; rear measures transmitted light; the OBSW calculates obscuration percentage

Camera images are compressed on the camera MCU before transfer. The OBSW stores them in FRAM and transmits on command in chunks. Camera settings are adjustable via `A_CHANGE_CAM_SETTING`.

### 7.2 CLOUD

CLOUD is a sub-millimetre debris impact sensor (16 copper resistive stripes per face, two faces: +Y and −Y). The OBSW:

- Activates the CLOUD ADC periodically (not continuously); frequency is mission-phase dependent
- Reads the 16-bit stripe status word via SPI
- Writes a 16×2 matrix to FRAM on any stripe breach: boolean status (1 bit) + first-breach timestamp (32 bits) per row; total matrix weight: 528 bits
- Appends only breached rows to the PDT; zero-impact passes generate minimal data

### 7.3 CLEAR

CLEAR uses 12 LEDs per face (+Z and −Z, two colours) and one photodiode per face. OBSW responsibilities:

- **LED control:** on/off via ground telecommand (ACTIVATE_PAYLOAD)
- **Photodiode acquisition:** nominally always-on; one orbit's worth of data acquired on ground alias command, then stored in FRAM and retrieved via SEND_DATA

### 7.4 Operational Command Database

| Identifier | Type | Description |
|---|---|---|
| A_CRYSTALS_ON | Command | Turn on CRYSTALS payload |
| A_CRYSTALS_OFF | Command | Turn off CRYSTALS payload |
| A_REQUEST_BEACON | Command | Request beacon with telemetry |
| A_TAKE_PHOTO | Command | Trigger camera |
| A_CHANGE_CAM_SETTING | Command | Change camera configuration profile |
| A_RESTART_SATELLITE | Command | Soft restart |
| T_CRYSTALS_STATUS | Telemetry | CRYSTALS on/off status (0/1) |
| T_BEACON_REQUESTED | Telemetry | Beacon request status (0/1) |
| T_PHOTO_TAKEN | Telemetry | Photo taken status (0/1) |
| T_CAM_SETTING | Telemetry | Camera setting — 0 = default, 1 = custom |
| T_SATELLITE_STATUS | Telemetry | Global satellite status (0/1) |

---

## 8. System Logging — The LastStates Pool

| Parameter | Value |
|---|---|
| Storage location | Internal Flash, reserved 8 KB section |
| Entry size | 128 bytes per state entry |
| Maximum entries | 64 (chronologically distinguishable) |
| Total pool size | 8 KB |
| Overwrite policy | Oldest entry replaced when full (circular) |
| Write triggers | Critical events; operational state transitions |
| Downlink access | Transmitted to GS on command (SEND_DATA with LastStates alias) |

The LastStates pool is the primary forensic tool for post-anomaly analysis. Ground teams can request the pool to reconstruct the sequence of state transitions leading to any event.

---

## 9. On-Board Services (ECSS Alignment)

| ST ID | Service Name | Status | Notes |
|---|---|---|---|
| ST[01] | Request Verification | N | FreeRTOS process priority list used instead |
| ST[02] | Device Access | Y | In-house framework for system component commanding |
| ST[03] | Housekeeping | Y | Beacon/mini-beacon system |
| ST[04] | Parameter Statistics | TBD | — |
| ST[05] | Event Reporting | TBD | — |
| ST[06] | Memory Management | Y | Per ECSS-E-ST-70-41C Chapter 6 |
| ST[08] | Function Management | Y | Implemented by declaration and request |
| ST[09] | Time Management | Y | On-board clock sync via SET_CONFIG |
| ST[11] | Time-Based Scheduling | Y | FreeRTOS standard scheduling |
| ST[12] | On-Board Monitoring | Y | Data integrity checks and testing functions |
| ST[13] | Large Data Transfer | Y | Chunking on-board; reassembly at GS |
| ST[14] | Real-Time Forwarding | TBD | — |
| ST[15] | On-Board Storage | Y | FAT FS ST specification |
| ST[17] | Test | Y | Dedicated testing functions for all data paths |
| ST[18] | On-Board Control Procedure | N | Not implemented |
| ST[20] | On-Board Parameter Management | Y | In-house camera library with configurable profiles |
| ST[22] | Position-Based Scheduling | N | No subsystem or payload requires it |
| ST[23] | File Management | Y | FreeRTOS standard file management |

---

## 10. AOCS Software Integration

The OBSW on the main OBC also acts as the AOCS microcontroller, executing attitude determination and control algorithms alongside all other OBSW functions.

### 10.1 AOCS Modes

- **Detumbling Mode:** B-dot controller. Angular rate from all three axes read from the ASM330LHHXTR IMU at 50 Hz. Magnetometer data (IIS2MDC, 50 Hz) provides field vector. OBC drives three magnetorquers (two torquerods + one air-core) directly. Transition to Nadir-Pointing Mode is automatic when all-axis angular rate < 5 deg/s.
- **Nadir-Pointing Mode:** Extended Kalman Filter (EKF) fusing IMU and magnetometer data. State vector includes attitude quaternion (body-to-NED) and angular velocity. Prediction step propagates state with Euler equations; update step fuses magnetometer observation. Validated in MATLAB/Simulink and ported to C for flight.

### 10.2 Sensor Sampling Rates

| Sensor | Maximum Rate | Planned Rate |
|---|---|---|
| IIS2MDC Magnetometer | 100 Hz (high res) / 150 Hz (low power) | 50 Hz |
| ASM330LHHXTR IMU (gyro + accel) | 6667 Hz | 50 Hz |
| CLEAR Photodiodes (attitude support) | On command | One orbit burst on command |

---

## 11. Ground Segment Software

### 11.1 TinyGS Integration

The GS software is based on a fork of the TinyGS open-source firmware, running on an ESP32 MCU with the same LoRa1268F30 module as the satellite — making the hardware architecturally symmetrical.

Additional features on top of base TinyGS firmware:

- Full encryption of all uplink telecommands (whitelisting + shuffle algorithm)
- Automatic NACK retransmission requests on CRC error detection
- Scheduled command time-delay for out-of-window payload scheduling
- Web interface for full remote operation

### 11.2 Database and Packet Archiving

All downlink packets are automatically published to the TinyGS public database and backed up locally. The TinyGS network connects 1,657+ stations worldwide, so packets are captured even when the satellite is not passing over Padova.

### 11.3 Uplink Command Flow

1. Telecommand packets are generated and encrypted by a dedicated MATLAB script on the ground operator's computer.
2. Encrypted packets are transmitted from the GS transceiver when the satellite is overhead.
3. A scheduled delay field within the packet allows scheduling of future out-of-sight actions.

---

## 12. Software Development and Verification Plan

### 12.1 Development Phases

| Phase | Weeks | Key Deliverables |
|---|---|---|
| Phase 1 — Initial | 12–17 | GitHub setup; project management infra; core documentation; hardware analysis (camera, FRAM, TT&C) |
| Phase 2 — Requirements & Design | 18–22 | Pseudocode for OBC, BMS, all payloads, and system-level logic |
| Phase 3 — Implementation I | 23–36 | Unit tests begin; code integration for CLOUD, camera, CLEAR, BMS, COMMS; CI pipelines |
| Phase 4 — Implementation II | 37–52 | Remaining subsystems; subsystem testing; continued integration |
| Phase 5 — System Integration & Test | Yr+1 Wks 1–13 | Final integration; EM simulation; umbilical testing; mission rehearsal; timings DB; reference values report |

### 12.2 Key Milestones

| Week | Milestone |
|---|---|
| 25 | Comprehensive understanding of all component requirements achieved |
| 39 | Code coherence validated on Development Model (DM) |
| 48 | Internal code correctness verified |
| Yr+1 Wk 6 | QubOS ready for mission rehearsal on Engineering Model; flight software deemed complete |

### 12.3 Verification Strategy

| Test Type | Period | Objective |
|---|---|---|
| Unit Tests | Week 23 onward (continuous) | Verify individual software modules |
| Integration Tests | Weeks 23–52 (continuous) | Ensure correct inter-module interfaces |
| Subsystem Tests | Weeks 37–52 | Validate each integrated subsystem |
| Simulation on EM | Yr+1 Weeks 1–10 | Validate OBSW on representative hardware |
| Hardware-in-the-Loop | Yr+1 Weeks 1–13 | Umbilical testing and mission rehearsal |

### 12.4 EGSE and Ground Software

Development of EGSE and GS software proceeds in parallel with flight software. Both must be integration-ready by Phase 5. The umbilical interface supports battery charging, telecommand/telemetry exchange, and power on/off switching.

---

## 13. Known Limitations and Open Items

| Item | Status | Notes |
|---|---|---|
| RAM memory update in-flight | Not planned | Restricted to STM32L4 RAM |
| Context save/restore across resets | Not planned | Only LastStates pool is preserved |
| Last history save across resets | Not planned | Runtime history lost on reset |
| In-flight software patching | TBD | No formal commitment post-launch |
| Full command database consolidation | In progress | Team consolidating and simplifying commands |
| Position-based scheduling (ST[22]) | Not implemented | No payload requires it |
| Camera payload COTS | COTS (ArduCam B0068) | Only COTS software component |
| CRYSTALS voltage calibration | TBC (3 V nominal) | In-orbit validation required |
| AOCS residual dipole measurement | Pending | To be measured during actuator manufacturing |

---

## 14. Conclusions

The RedPill OBSW is a carefully designed, resource-aware software system for a 2P PocketQube. Key strengths:

- FreeRTOS-based task architecture with a dedicated anomaly watchdog for long-duration autonomous operation
- Multi-processor workload separation (image acquisition and BMS on dedicated MCUs)
- Cyclic memory management across FRAM and Flash — graceful degradation on overflow, never a fatal fault
- Adaptive beacon intervals (1–16 min) tied to battery SoC
- LoRa-based TT&C with encryption, CRC, NACK retransmission, and TinyGS global downlink coverage
- MISRA-compliant C developed in STM32CubeIDE with continuous unit, subsystem, and integration testing

Flight-ready status is targeted for Week 6 of the year following kick-off, aligned with Engineering Model mission rehearsal.

---

## 15. Implementing the OBSW Using STM32CubeIDE and Claude Code

This section covers the full workflow for building the RedPill OBSW from scratch: first setting up a correctly configured STM32CubeIDE project (which generates the HAL, FreeRTOS kernel, and linker scripts), then handing the repo to Claude Code to implement application logic.

---

### 15.1 Setting Up the Project in STM32CubeIDE

Before Claude Code can touch any firmware file, the project must exist and build cleanly inside STM32CubeIDE. CubeIDE bundles CubeMX as its embedded configuration tool — it generates HAL drivers, the FreeRTOS port, linker scripts, and startup code from a single `.ioc` file.

#### 15.1.1 Install STM32CubeIDE

Download STM32CubeIDE from [st.com/stm32cubeide](https://www.st.com/en/development-tools/stm32cubeide.html). It runs on Windows, macOS, and Linux.

On Linux, run the provided udev rules script after installation so the ST-LINK debugger is accessible without root:

```bash
# Install udev rules for ST-LINK
sudo cp /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.stlink*/tools/udev/rules.d/*.rules \
    /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

#### 15.1.2 Create a New STM32 Project

Go to **File → New → STM32 Project**. In the MCU/MPU selector:

| Field | Value |
|---|---|
| MCU/MPU filter | STM32L4 series — select the exact variant on the OBC (e.g. STM32L476RGTx) |
| Project name | `redpill-obsw` |
| Project location | Your Git repository root |
| Targeted language | C |
| Targeted binary type | Executable |
| Targeted project type | STM32Cube |

Click **Finish**. CubeIDE downloads the firmware package for the selected variant if not cached, then opens the `.ioc` configuration file.

#### 15.1.3 Configure Peripherals in the .ioc File

The `.ioc` editor is CubeMX embedded inside CubeIDE. Every peripheral configured here generates the corresponding HAL initialisation code. Configure the following for the RedPill OBC:

| Peripheral / Category | Setting | Purpose |
|---|---|---|
| System Core → RCC | HSE: Crystal/Ceramic Resonator; PLL to 80 MHz | Maximum clock for AOCS/EKF at low power |
| **System Core → SYS** | **Timebase Source: TIM6 (not SysTick)** | **FreeRTOS owns SysTick — this prevents conflict** |
| Connectivity → SPI1 | Full-Duplex Master; DMA Rx + Tx; 8-bit; CPOL/CPHA per LoRa1268F30 datasheet | LoRa transceiver + CLOUD ADC |
| Connectivity → SPI2 | Full-Duplex Master; DMA Rx + Tx; 8-bit | External FRAM (MB85RS4MT or equivalent) |
| Connectivity → I2C1 | Fast Mode (400 kHz) | IIS2MDC magnetometer |
| Connectivity → SPI3 / UART | Per camera MCU interface spec | Camera MCU data transfer |
| Timers → TIM2 | PWM Generation CH1/CH2/CH3; period per magnetorquer drive frequency | Three magnetorquer PWM outputs |
| Timers → IWDG | Prescaler /256; Reload ~4095 (~32 s timeout) | Hardware watchdog |
| GPIO | NSS pins as software-controlled GPIO outputs | Manual CS management per SPI device |
| Middleware → FREERTOS | Interface: CMSIS_V2; USE_PREEMPTION: Enabled; Heap: heap_4.c | RTOS kernel |

> **Critical:** Always move the HAL timebase source from SysTick to a spare hardware timer (TIM6 is the standard choice). If SysTick remains the HAL timebase while FreeRTOS is enabled, they will compete for the same interrupt, causing missed ticks and undefined behaviour.

#### 15.1.4 FreeRTOS Configuration in Detail

After enabling **Middleware → FREERTOS** with CMSIS_V2, open the **Config parameters** tab:

| FreeRTOS Config Parameter | Value | Reason |
|---|---|---|
| USE_PREEMPTION | 1 | Higher-priority tasks (AOCS, watchdog) preempt lower-priority ones |
| TICK_RATE_HZ | 1000 | 1 ms tick resolution — sufficient for beacon timing and 50 Hz sensor polling |
| MAX_PRIORITIES | 8 | watchdog > AOCS > comms > state machine > payload > idle |
| TOTAL_HEAP_SIZE | 32768 (32 KB) | Leaves ~96 KB for task stacks and static buffers |
| USE_IDLE_HOOK | 1 | Enter STM32L4 SLEEP mode in idle task |
| USE_TICK_HOOK | 0 | Not needed |
| CHECK_FOR_STACK_OVERFLOW | 2 | Full stack check at every context switch — essential during development |
| USE_MALLOC_FAILED_HOOK | 1 | Trap heap exhaustion rather than returning NULL silently |
| USE_TRACE_FACILITY | 1 | Required for the CubeIDE RTOS Viewer debugger panel |
| Heap scheme | heap_4.c | Supports malloc/free with fragmentation merging |

Under the **Tasks and Queues** tab, **delete the default `defaultTask`**. Claude Code will generate all task definitions in C via `xTaskCreate()` or `osThreadNew()` directly from application code — you do not need to pre-define them here.

#### 15.1.5 NVIC Interrupt Priority Configuration

This is the most common source of hard faults in FreeRTOS projects on Cortex-M4. FreeRTOS requires that no ISR calling a FreeRTOS API function runs at a priority numerically lower than `configMAX_SYSCALL_INTERRUPT_PRIORITY`.

In **CubeMX → NVIC**, apply these settings:

| Interrupt | Priority | Reason |
|---|---|---|
| SPI DMA Rx/Tx (SPI1, SPI2) | 5 | These ISRs call FreeRTOS queue/semaphore APIs |
| I2C event/error | 5 | Same reason |
| IWDG | 0 (highest) | Must never be masked |
| SysTick | 15 (lowest) | FreeRTOS manages this |

Set `configMAX_SYSCALL_INTERRUPT_PRIORITY` to **5** in `FreeRTOSConfig.h` (generated from the .ioc) — this corresponds to priority level 80 in the raw 8-bit register with 4-bit priority grouping.

> **Rule of thumb:** If an ISR calls any FreeRTOS API ending in `...FromISR()`, its NVIC priority must be numerically **equal to or greater than** `configMAX_SYSCALL_INTERRUPT_PRIORITY`. Getting this wrong produces subtle crashes under load that are extremely difficult to reproduce.

#### 15.1.6 Generate Code

Save the `.ioc` file (**Ctrl+S**) or click **Project → Generate Code**. CubeIDE creates or updates:

| Generated File/Directory | Contents |
|---|---|
| `Core/Src/main.c` | HAL init, clock config, peripheral init calls, `osKernelStart()` |
| `Core/Src/freertos.c` | FreeRTOS init; add `osThreadNew()` calls inside `MX_FREERTOS_Init()` |
| `Core/Inc/FreeRTOSConfig.h` | All RTOS parameters from the .ioc file as `#define`s |
| `Middlewares/Third_Party/FreeRTOS/` | Complete FreeRTOS kernel source |
| `Drivers/STM32L4xx_HAL_Driver/` | All HAL peripheral drivers |
| `STM32L4xxxx_FLASH.ld` | Linker script defining Flash and RAM regions |

> CubeIDE marks generated code with `/* USER CODE BEGIN */` and `/* USER CODE END */` comment pairs. **Only write application code between these markers** — code outside them is overwritten on the next regeneration.

#### 15.1.7 Verify the Baseline Build

Before writing any application code, confirm the skeleton builds cleanly:

```bash
# Build from command line (matches what Claude Code will use)
cd redpill-obsw/
make -C STM32CubeIDE/Debug all 2>&1 | tail -5

# Expected output:
#    text    data     bss     dec     hex filename
#   12340     xxx   xxxxx   xxxxx   xxxxx redpill-obsw.elf
# Finished building: redpill-obsw.elf

# Budget validation:
# text + data  must be < 1,048,576  (1 MB Flash)
# bss  + data  must be < 131,072    (128 KB RAM)
```

#### 15.1.8 Initialise Git and Add .gitignore

Before handing the repository to Claude Code, initialise Git and add a `.gitignore` that excludes build artefacts while keeping the `.ioc` file and all generated source under version control:

```gitignore
# Exclude build output
STM32CubeIDE/Debug/
STM32CubeIDE/Release/
*.d       # dependency files
*.o       # object files
*.elf     # linked binary
*.map     # linker map
*.bin
*.hex

# Keep in Git:
# *.ioc  *.c  *.h  *.ld  Makefile  CLAUDE.md
```

Once the baseline builds and the repo is initialised, the project is ready for Claude Code. The generated HAL drivers and FreeRTOS kernel are in place; Claude Code's role is to populate the `App/` subdirectories with application logic.

---

### 15.2 Installation and Setup (Claude Code)

Claude Code requires Node.js and a Claude Pro, Max, or API account.

```bash
# Install Claude Code
npm install -g @anthropic-ai/claude-code

# Navigate to your firmware repo
cd redpill-obsw/

# Launch Claude Code
claude
```

On first launch, Claude Code asks you to authenticate via browser (recommended for Pro/Max) or by entering an Anthropic API key. Once authenticated, it indexes your working directory.

> **NixOS note:** On a NixOS flake-based config, add `nodejs` to your `devShell` and ensure the npm global bin path is in `PATH`. A `nix-shell -p nodejs` also works for one-off use.

---

### 15.3 Repository Structure to Establish First

Claude Code performs best when the directory structure is meaningful and predictable. Create the `App/` layout before your first session:

```
redpill-obsw/
├── Core/                  # FreeRTOS kernel + STM32 HAL (CubeIDE-generated)
├── Drivers/               # STM32L4 peripheral drivers (HAL)
├── App/
│   ├── obsw/             # State machine, scheduler, watchdog
│   ├── comms/            # LoRa TT&C: TX, RX, beacon, chunking
│   ├── payloads/         # CRYSTALS, CLOUD, CLEAR drivers
│   ├── aocs/             # B-dot controller, EKF, magnetorquer driver
│   ├── memory/           # FRAM driver, cyclic buffer, LastStates pool
│   └── bms/              # EPS SPI interface, SoC threshold logic
├── Tests/                # Unit + integration test stubs
├── CLAUDE.md             # Claude Code project instructions  ← critical
└── STM32CubeIDE/         # IDE project files and Makefile
```

---

### 15.4 The CLAUDE.md Project Instructions File

The most important step before starting any Claude Code session. Claude Code reads `CLAUDE.md` automatically at startup and uses it to understand the project's constraints. For RedPill:

```markdown
# RedPill OBSW — Claude Code Instructions

## Target Hardware
- MCU: STM32L4 (ARM Cortex-M4, 80 MHz)
- Flash: 1024 KB internal + 4 MB external FRAM (SPI2)
- RAM: 128 KB internal
- RTOS: FreeRTOS (latest stable, CMSIS_V2 interface, CubeIDE integration)

## Coding Rules
- Language: C (C99), MISRA guidelines
- No dynamic memory allocation after init (no malloc/free inside tasks)
- All SPI calls must be interrupt-driven — no blocking HAL_SPI_Transmit
- Every task must register with the watchdog on init
- State machine lives in App/obsw/state_machine.c only
- USER CODE sections only — never edit outside /* USER CODE BEGIN/END */

## Build System
- Build:      make -C STM32CubeIDE/Debug all
- Flash:      make -C STM32CubeIDE/Debug flash   (via OpenOCD + ST-LINK)
- Size check: arm-none-eabi-size STM32CubeIDE/Debug/redpill-obsw.elf

## Operational States
- s0=OFF, s1=INIT, s2=CRIT, s3=READY, s4=ACTIVE
- Battery thresholds: B_SCRIT ≈ 25%, B_CRIT, B_COMMOK, B_OPOK ≈ 80%
- Full transition table: see docs/state_machine.md

## Key Interfaces
- SPI1: LoRa1268F30 (TT&C) + CLOUD ADC
- SPI2: FRAM (MB85RS4MT)
- I2C1: IIS2MDC magnetometer
- TIM2 PWM CH1/2/3: magnetorquer drive
- IWDG: hardware watchdog (~32 s timeout, kicked by main OBSW task)
```

Claude Code will reference this file on every interaction, ensuring it never generates blocking SPI calls, heap allocations inside tasks, or code that forgets to register with the watchdog.

---

### 15.5 Recommended Prompting Workflow

Work through the OBSW subsystems in order, starting with low-level infrastructure and working upward — mirroring the development phases in Section 12.

#### Phase 1 — State Machine and Watchdog

```
> Implement the operational state machine in App/obsw/state_machine.c.
  Use a FreeRTOS task running at 10 Hz. States are s0–s4 as defined in
  CLAUDE.md. Battery thresholds come from bms_get_soc() in App/bms/bms.h
  (stub it for now). All transitions must be logged to the LastStates pool
  via memory_laststates_write(). Include a hardware watchdog kick in the
  main task loop. Run make and fix any errors.
```

#### Phase 2 — FRAM Cyclic Buffer and LastStates Pool

```
> Implement the FRAM driver in App/memory/fram.c using SPI2 DMA
  (interrupt-driven, no blocking calls). Then implement the cyclic buffer
  in App/memory/cyclic_buffer.c — 4 MB total, overwrite oldest data on
  wrap. Separately, implement the LastStates pool in
  App/memory/laststates.c: 64 entries × 128 B each, stored in internal
  Flash at address 0x08080000. Provide:
    memory_laststates_write(state_entry_t *entry)
    memory_laststates_dump_all(uint8_t *out, size_t *len)
  Run make and fix any errors.
```

#### Phase 3 — TT&C Subsystem (LoRa)

```
> Implement the LoRa TT&C layer in App/comms/. The transceiver is a
  Semtech SX1268 on SPI1. Implement:
    (1) lora_init() — configure SF10, BW125, CR4/8, freq 436 MHz
    (2) lora_beacon_task() — FreeRTOS task sending 128-byte beacons at
        interval_ms set by the state machine
    (3) lora_rx_task() — interrupt-driven RX, CRC check, decryption stub
        (key TBD), command dispatch via comms_dispatch_command()
    (4) lora_send_chunked(uint8_t *data, size_t len) — fragment into
        64-byte packets with sequence numbers
  All SPI must be interrupt-driven. Run make and fix any errors.
```

#### Phase 4 — Payload Drivers

```
> Implement the CLOUD debris sensor driver in App/payloads/cloud.c.
  The sensor exposes 16 binary stripe states via ADC read over SPI1.
  cloud_acquire() reads all 16 stripes, builds a 16×2 matrix (boolean
  status + uint32 timestamp per stripe), writes only breached rows to
  FRAM via cyclic_buffer_write(), and returns the breach count. Call
  cloud_acquire() once per orbit from the state machine when in s4 ACTIVE.
```

#### Phase 5 — AOCS: B-dot Controller and EKF

```
> Implement the AOCS controller in App/aocs/. First implement sensor
  drivers: iis2mdc.c (magnetometer over I2C1, 50 Hz) and asm330lhh.c
  (IMU over SPI1, 50 Hz). Then:
    - B-dot controller in aocs_bdot.c: read dB/dt from consecutive
      magnetometer samples, compute corrective dipole moment, drive three
      magnetorquers via TIM2 PWM. Exit condition: all-axis rate < 5 deg/s.
    - EKF in aocs_ekf.c: state vector = [quaternion(4), omega(3)].
      Predict with Euler equations at 50 Hz; update with magnetometer
      measurement. Port the algorithm from the validated MATLAB model in
      docs/aocs_ekf.m.
  Run make and fix any compiler errors.
```

---

### 15.6 Autonomous Multi-File Editing

For larger tasks, use non-interactive (headless) mode or provide a high-level goal and let Claude Code explore the codebase to understand existing interfaces before writing:

```bash
# Non-interactive: pipe a task directly
claude -p "Read the state machine in App/obsw/ and the comms layer in \
  App/comms/. Add a SET_CONFIG telecommand handler that updates the beacon \
  interval and LoRa SF/BW from the packet payload. Validate input ranges \
  before applying. Run make and fix any compiler errors."

# In interactive mode, ask Claude to build-and-fix autonomously
> Run make, then fix every compiler warning and error.
  Do not change any function signatures.
```

---

### 15.7 Unit Testing with Claude Code

For embedded firmware, use a native-compilation test harness (Unity or cmocka) that stubs out HAL calls so tests run on x86 without hardware:

```
> Generate Unity unit tests for App/memory/laststates.c. Stub out
  Flash_Write and Flash_Read with in-memory arrays. Test:
    (1) writing 64 entries fills the pool
    (2) writing entry 65 overwrites entry 0 (circular)
    (3) dump_all returns entries in chronological order
  Compile and run the tests natively with gcc and report results.
```

Claude Code will write the test files, stubs, and a native-build Makefile target, execute them, and iterate on failures — all autonomously.

---

### 15.8 Useful Claude Code Commands for This Project

| Command / Flag | Purpose for RedPill OBSW |
|---|---|
| `claude` | Launch interactive session in the firmware repo root |
| `claude -p "<task>"` | Run a single task non-interactively (good for CI) |
| `/clear` | Clear conversation context to start a fresh subsystem — prevents context confusion between tasks |
| `/add App/obsw/state_machine.c` | Pin a file into context so Claude always reads it (useful for the state machine while working on comms) |
| `Ctrl+B` | Background a long-running task (e.g. full EKF implementation) and return to the terminal |
| `/bug` | Report a Claude Code issue to Anthropic directly from within the session |
| `--allowedTools Bash,Read,Edit` | Restrict Claude Code to specific tools — use during code review to prevent writes |

---

### 15.9 CI/CD Integration for Continuous Firmware Verification

Claude Code can be embedded in CI pipelines to automate build verification and static analysis. On NixOS with GitHub Actions or a local Gitea runner:

```yaml
# .github/workflows/obsw_verify.yml (excerpt)
- name: Build and verify OBSW
  run: |
    claude -p "Run make -C STM32CubeIDE/Debug all. Fix any errors.
      Then run arm-none-eabi-size STM32CubeIDE/Debug/redpill-obsw.elf
      and confirm text+data < 1048576 and bss+data < 131072.
      Report PASS or FAIL with the actual values."
```

---

> **Further reading:**
> - Claude Code documentation: [code.claude.com/docs](https://code.claude.com/docs)
> - npm package: `@anthropic-ai/claude-code`
> - Claude Agent SDK: [anthropic.com/engineering/building-agents-with-the-claude-agent-sdk](https://www.anthropic.com/engineering/building-agents-with-the-claude-agent-sdk)
> - STM32CubeIDE User Manual: UM2609 (available on st.com)
> - FreeRTOS + STM32 quick start: UM2553
