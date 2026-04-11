# AOCS (Attitude and Orbit Control System) API Reference

## Overview

The AOCS module handles satellite attitude control. Located in `App/aocs/`.

## Dependencies

- Header: `App/aocs/aocs.h`
- Hardware: ASM330LHH (IMU), IIS2MDC (magnetometer)

## Status

**Current Implementation:** Placeholder (stub)

The AOCS is developed by a separate team. Placeholder functions exist for integration.

## Hardware

- **IMU:** ASM330LHHXTR (3-axis gyroscope + accelerometer)
- **Magnetometer:** IIS2MDC
- **Interface:** I2C

## Public API

### `void aocs_init(void)`

Initialize AOCS hardware.

**Parameters:** None

**Returns:** Nothing

**Notes:**
- TODO: Initialize IMU (ASM330LHH) and magnetometer (IIS2MDC)

---

### `void aocs_task(void *arg)`

AOCS control task.

**Parameters:**
- `arg` - FreeRTOS task argument (unused)

**Returns:** Nothing

**Notes:**
- TODO: B-dot controller / EKF at 50 Hz
- Runs at 20ms period (50 Hz)

---

## Control Algorithm

The AOCS implements:
1. **B-dot controller** - Detumble algorithm using magnetometer
2. **EKF** - Extended Kalman Filter for attitude estimation (future)

## Task Parameters

| Parameter | Value |
|-----------|-------|
| Period | 20 ms |
| Frequency | 50 Hz |
| Priority | TBD |

## Notes

This module requires:
- IMU driver integration
- Magnetometer driver integration
- Control law implementation
- Sensor calibration routines