# BMS (Battery Management System) API Reference

## Overview

The BMS module provides battery status monitoring. It communicates with the EPS (via SPI) to get battery state of charge, temperature, and voltage. Located in `App/bms/`.

## Dependencies

- Header: `App/bms/bms.h`
- Types: `App/obsw_types.h`

## Status

**Current Implementation:** Stub (returns fixed values)

The BMS is implemented as a stub that returns hardcoded values. The real implementation will communicate with the EPS STM32L1 via SPI.

## Public API

### `void bms_init(void)`

Initialize the BMS interface.

**Parameters:** None

**Returns:** Nothing

**Notes:**
- TODO: Initialize SPI to EPS STM32L1

---

### `bms_status_t bms_get_status(void)`

Get current battery status.

**Parameters:** None

**Returns:** `bms_status_t` - Current battery status

**Structure:**
```c
typedef struct {
    uint8_t soc;           // State of charge (0-100%)
    int16_t temp_c;        // Battery temperature (0.1°C units)
    uint16_t voltage_mv;   // Battery voltage in mV
} bms_status_t;
```

**Notes:**
- TODO: Read from BQ27441 via EPS SPI

---

## Battery Thresholds

Default thresholds used by the state machine (from `state_machine.c`):

| Threshold | Value | Description |
|-----------|-------|-------------|
| `b_opok` | 80% | Normal operations allowed |
| `b_commok` | 60% | PDT ok, payloads suspended |
| `b_crit` | 40% | Finish current PDT, then enter CRIT |
| `b_scrit` | 25% | Abort PDT immediately, enter CRIT |

---

## Hardware Interface

- **Target:** EPS microcontroller (STM32L1)
- **通信协议:** SPI
- **Fuel Gauge:** BQ27441 (optional)

---

## Notes

This module is a stub. For flight:
1. Implement SPI communication with EPS
2. Add I2C communication with BQ27441 fuel gauge (if used)
3. Implement proper SoC calculation algorithm
4. Add temperature monitoring and protection