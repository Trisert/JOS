# Payloads API Reference

## Overview

The project includes three scientific payloads: CRYSTALS, CLOUD, and CLEAR. Located in `App/payloads/`.

---

## CRYSTALS

### Purpose

Crystal growth observation payload with photodiode measurements.

### Dependencies

- Header: `App/payloads/crystals.h`
- Hardware: GPIO (PC0, PC1, PC2), ADC1 CH4/CH5

### Pin Assignments

| Signal | GPIO | Description |
|--------|------|-------------|
| CRYSTALS_EN | PC0 | 3V potential difference on/off |
| CRYSTALS_HEATER | PC1 | 5 mW copper coil heater |
| CRYSTALS_CAM_TRIG | PC2 | Camera trigger (active-low) |
| Photodiode front | PA3 (ADC1 CH4) | Incident light |
| Photodiode rear | ADC1 CH5 | Transmitted light |

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CRYSTALS_HEATER_ON_TEMP` | 50 | Turn heater on at ≤ 5.0°C |
| `CRYSTALS_HEATER_OFF_TEMP` | 70 | Turn heater off at ≥ 7.0°C |

### Types

```c
typedef struct {
    uint8_t  enabled;      // Device on/off
    uint8_t  heater_on;   // Heater state
    uint8_t  cam_setting; // 0=default, 1=custom
    uint16_t pd_front_mv; // Front photodiode (mV)
    uint16_t pd_rear_mv;  // Rear photodiode (mV)
    uint8_t  obscuration; // 0-100%
} crystals_status_t;
```

### Public API

#### `void crystals_init(void)`

Initialize CRYSTALS payload.

**Parameters:** None

**Returns:** Nothing

---

#### `void crystals_enable(uint8_t on)`

Turn CRYSTALS on/off.

**Parameters:**
- `on` - 1 to enable, 0 to disable

**Returns:** Nothing

---

#### `uint8_t crystals_is_enabled(void)`

Check if CRYSTALS is enabled.

**Parameters:** None

**Returns:** `uint8_t` - 1 if enabled, 0 if disabled

---

#### `void crystals_manage_heater(int16_t temp_c)`

Manage heater based on temperature.

**Parameters:**
- `temp_c` - Temperature in 0.1°C units

**Returns:** Nothing

---

#### `void crystals_take_photo(void)`

Trigger camera for one image capture.

**Parameters:** None

**Returns:** Nothing

---

#### `uint8_t crystals_read_obscuration(void)`

Read photodiodes and compute obscuration %.

**Parameters:** None

**Returns:** `uint8_t` - Obscuration percentage (0-100%)

---

#### `crystals_status_t crystals_get_status(void)`

Get full status snapshot.

**Parameters:** None

**Returns:** `crystals_status_t` - Current status

---

## CLOUD

### Purpose

Conductive Layer Observation for Unified detection - detects conductive layer breaches on satellite exterior.

### Dependencies

- Header: `App/payloads/cloud.h`
- Hardware: MAX11128 ADC via SPI1 (PB4 CS)

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CLOUD_FACES` | 2 | +Y and -Y faces |
| `CLOUD_STRIPES` | 16 | Resistive stripes per face |
| `CLOUD_THRESH` | 500 | Breach threshold (ADC units) |

### Types

```c
typedef struct {
    uint8_t  breached;   // 0=intact, 1=breached
    uint32_t timestamp;   // First-breach tick (ms)
} cloud_stripe_t;

typedef struct {
    cloud_stripe_t stripes[CLOUD_STRIPES];
} cloud_face_t;

typedef struct {
    cloud_face_t face[CLOUD_FACES];
    uint32_t timestamp;   // Sample tick
} cloud_sample_t;
```

### Public API

#### `void cloud_init(void)`

Initialize CLOUD driver.

**Parameters:** None

**Returns:** Nothing

---

#### `int cloud_acquire(cloud_sample_t *out)`

Acquire sample from both faces.

**Parameters:**
- `out` - Output sample structure

**Returns:** `int` - Number of newly breached stripes

---

#### `const cloud_sample_t *cloud_last_sample(void)`

Get most recent sample.

**Parameters:** None

**Returns:** `const cloud_sample_t*` - Pointer to last sample

---

#### `osThreadId_t cloud_task_create(void)`

Create CLOUD FreeRTOS task.

**Parameters:** None

**Returns:** `osThreadId_t` - Task handle

---

## CLEAR

### Purpose

Cubesat Light Evaluation for Radiation - measures optical properties with LED stimulation.

### Dependencies

- Header: `App/payloads/clear.h`
- Hardware: GPIO (PE10, PE12, PE14), ADC1 CH8/CH9

### Pin Assignments

| Signal | GPIO | Description |
|--------|------|-------------|
| +Z LED A | PE10 | Colour A / white |
| +Z LED B | PE12 | Colour B / IR |
| -Z LED | PE14 | Single bank |
| +Z photodiode | ADC1 CH8 | +Z face |
| -Z photodiode | ADC1 CH9 | -Z face |

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CLOUD_BURST_MAX` | 5400 | Max burst samples (1 Hz × 90 min) |

### Types

```c
typedef enum {
    CLEAR_FACE_Z  = 0,
    CLEAR_FACE_MZ = 1,
    CLEAR_FACE_COUNT
} clear_face_t;

typedef enum {
    CLEAR_LED_OFF   = 0,
    CLEAR_LED_WHITE = 1,
    CLEAR_LED_IR    = 2,
} clear_led_colour_t;

typedef struct {
    uint8_t led_on[CLEAR_FACE_COUNT];
    uint8_t led_colour[CLEAR_FACE_COUNT];
    uint16_t pd_mv[CLEAR_FACE_COUNT];
    uint16_t burst_count;
    uint8_t  burst_active;
} clear_status_t;

typedef struct {
    uint32_t timestamp_ms;
    uint16_t pd_fz_mv;
    uint16_t pd_fmz_mv;
} __attribute__((packed)) clear_burst_point_t;
```

### Public API

#### `void clear_init(void)`

Initialize CLEAR payload.

**Parameters:** None

**Returns:** Nothing

---

#### `void clear_led_set(clear_face_t face, clear_led_colour_t colour)`

Set LED colour.

**Parameters:**
- `face` - Face identifier
- `colour` - LED colour

**Returns:** Nothing

---

#### `void clear_led_off(clear_face_t face)`

Turn off LED.

**Parameters:**
- `face` - Face identifier

**Returns:** Nothing

---

#### `uint16_t clear_read_pd(clear_face_t face)`

Read photodiode (mV).

**Parameters:**
- `face` - Face identifier

**Returns:** `uint16_t` - Photodiode voltage in mV

---

#### `void clear_burst_start(void)`

Start burst acquisition.

**Parameters:** None

**Returns:** Nothing

---

#### `void clear_burst_stop(void)`

Stop burst acquisition.

**Parameters:** None

**Returns:** Nothing

---

#### `clear_status_t clear_get_status(void)`

Get status snapshot.

**Parameters:** None

**Returns:** `clear_status_t` - Current status

---

#### `osThreadId_t clear_task_create(void)`

Create CLEAR FreeRTOS task.

**Parameters:** None

**Returns:** `osThreadId_t` - Task handle

---

## Payload Task Summary

| Payload | Task Period | Priority | State Required |
|---------|-------------|----------|--------------|
| CRYSTALS | Manual | TBD | Any |
| CLOUD | ~90 min | BelowNormal | ACTIVE |
| CLEAR | 1 Hz (burst) | BelowNormal | ACTIVE |