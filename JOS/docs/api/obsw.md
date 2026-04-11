# OBSW (On-Board Software) API Reference

## Overview

The OBSW module handles the core satellite state machine, watchdog monitoring, and system-level operations. It is located in `App/obsw/`.

## Modules

- **state_machine** - Operational state management
- **watchdog** - Task monitoring and anomaly detection

---

## state_machine

### Dependencies

- Header: `App/obsw/state_machine.h`
- Types: `App/obsw_types.h`

### Operational States

Defined in `obsw_types.h`:

| State | Value | Description |
|-------|-------|-------------|
| `STATE_OFF` | 0 | System off, kill switches active |
| `STATE_INIT` | 1 | Boot, antenna deployment, self-tests |
| `STATE_CRIT` | 2 | Critical/safe mode, beacon only |
| `STATE_READY` | 3 | Idle, uplink listening, housekeeping |
| `STATE_ACTIVE` | 4 | Payload operations, PDT, scheduled tasks |

### Beacon Intervals

| State | Interval |
|-------|---------|
| `STATE_CRIT` | 16 min |
| `STATE_READY` | 4 min |
| `STATE_ACTIVE` | 1 min |

### Public API

#### `void state_machine_init(void)`

Initialize the state machine. Must be called once before `osKernelStart`.

**Parameters:** None

**Returns:** Nothing

---

#### `osThreadId_t state_machine_task_create(void)`

Create the state machine FreeRTOS task.

**Parameters:** None

**Returns:** `osThreadId_t` - Handle to the created task

---

#### `obw_state_t state_machine_get_state(void)`

Get the current operational state (thread-safe).

**Parameters:** None

**Returns:** `obw_state_t` - Current state

---

#### `int state_machine_request_transition(obw_state_t target, uint8_t trigger)`

Request a state transition. Called from BMS, comms, or other subsystems.

**Parameters:**
- `target` - Target state (`obw_state_t`)
- `trigger` - Trigger ID (see transition triggers below)

**Returns:** `int` - 0 on success, -1 on failure

**Transition Triggers:**

| Trigger | Value | Description |
|---------|-------|-------------|
| `TRIGGER_BOOT` | 0 | Initial boot |
| `TRIGGER_BATTERY_LOW` | 1 | Battery SoC below critical |
| `TRIGGER_BATTERY_OK` | 2 | Battery recovered |
| `TRIGGER_CRIT_EVENT` | 3 | Critical event detected |
| `TRIGGER_GROUND_CMD` | 4 | Ground command received |
| `TRIGGER_TASK_COMPLETE` | 5 | Scheduled task completed |
| `TRIGGER_ANTENNA_DONE` | 6 | Antenna deployment complete |
| `TRIGGER_WATCHDOG` | 7 | Watchdog timeout |

---

#### `uint32_t state_machine_get_beacon_interval(void)`

Get the beacon interval for the current state.

**Parameters:** None

**Returns:** `uint32_t` - Beacon interval in milliseconds

---

## watchdog

### Dependencies

- Header: `App/obsw/watchdog.h`

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `WDG_MAX_TASKS` | 8 | Maximum number of tasks that can be monitored |

### Public API

#### `void watchdog_monitor_init(void)`

Initialize the watchdog monitor.

**Parameters:** None

**Returns:** Nothing

---

#### `int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms)`

Register a task for monitoring. Call during task initialization.

**Parameters:**
- `handle` - FreeRTOS task handle (`osThreadId_t`)
- `expected_period_ms` - Expected period between alive signals in milliseconds

**Returns:** `int` - 0 on success, -1 if no slots available

---

#### `osThreadId_t watchdog_task_create(void)`

Create the watchdog monitor task.

**Parameters:** None

**Returns:** `osThreadId_t` - Handle to the monitor task

---

#### `void watchdog_alive(osThreadId_t handle)`

Signal liveness from a monitored task. Must be called at least every 3x the expected period.

**Parameters:**
- `handle` - FreeRTOS task handle (`osThreadId_t`)

**Returns:** Nothing

---

## Types

### `bms_status_t`

Battery Management System status structure.

```c
typedef struct {
    uint8_t soc;           // State of charge (0-100%)
    int16_t temp_c;       // Battery temperature (0.1°C units)
    uint16_t voltage_mv;  // Battery voltage in mV
} bms_status_t;
```

### `bms_thresholds_t`

Battery SoC thresholds.

```c
typedef struct {
    uint8_t b_opok;    // ~80%: normal ops allowed
    uint8_t b_commok;  // Intermediate: PDT ok, payloads suspended
    uint8_t b_crit;    // Low: finish current PDT, then enter CRIT
    uint8_t b_scrit;   // ~25%: abort PDT immediately, enter CRIT
} bms_thresholds_t;
```

### `laststates_entry_t`

State transition log entry (stored in Flash).

```c
#define LASTSTATES_ENTRY_SIZE 128
#define LASTSTATES_MAX_ENTRIES 64

typedef struct {
    uint32_t timestamp;   // System tick at transition
    uint8_t  state_from; // Previous state
    uint8_t  state_to;   // New state
    uint8_t  trigger;    // What caused the transition
    uint8_t  reserved[3];
    uint8_t  context[116]; // Free-form context data
} laststates_entry_t;
```