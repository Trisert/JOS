# MAX11128 ADC Driver API Reference

## Overview

The MAX11128 is an 8-channel, 12-bit analog-to-digital converter used for CLOUD payload and general ADC readings. Located in `Core/Src/MAX11128.c`.

## Dependencies

- Header: `Core/Inc/MAX11128.h`
- Hardware: SPI1 (for CLOUD), SPI2 (for general ADC if configured)

## Hardware

| Parameter | Value |
|-----------|-------|
| Channels | 8 (AIN0-AIN15, multiplexed) |
| Resolution | 12-bit |
| Interface | SPI |
| VREF | 3.0V |

## Types

```c
typedef struct {
    SPI_HandleTypeDef *spiHandle;  // SPI handle
    float VREF;               // Reference voltage
    GPIO_TypeDef *csPort;      // Chip select port
    uint16_t csPin;          // Chip select pin
} MAX11128_t;
```

## Public API

### `void MAX11128_Initialize(MAX11128_t *adc, SPI_HandleTypeDef *spiHandle)`

Initialize SPI for MAX11128.

**Parameters:**
- `adc` - ADC structure (filled)
- `spiHandle` - HAL SPI handle

**Returns:** Nothing

---

### `void MAX11128_ADC_Config(MAX11128_t *adc)`

Configure ADC.

**Parameters:**
- `adc` - ADC structure

**Returns:** Nothing

---

### `void MAX11128_ADC_Uni_Setup(MAX11128_t *adc)`

Configure for unipolar mode.

**Parameters:**
- `adc` - ADC structure

**Returns:** Nothing

---

### `float MAX11128_ADC_ReadVoltCH(MAX11128_t *adc, uint8_t ch)`

Read channel voltage (float).

**Parameters:**
- `adc` - ADC structure
- `ch` - Channel (0-15)

**Returns:** `float` - Voltage in volts

---

### `uint16_t MAX11128_ADC_ReadRawCH(MAX11128_t *adc, uint8_t ch)`

Read channel raw value.

**Parameters:**
- `adc` - ADC structure
- `ch` - Channel (0-15)

**Returns:** `uint16_t` - Raw ADC value (0-4095)

---

## Constants

| Constant | Value |
|----------|-------|
| `MAX1112XVREF` | 3.0V |
| `MAX1112XBIT` | 12 |

## Usage Example

```c
MAX11128_t cloud_adc;
cloud_adc.spiHandle = &hspi1;
cloud_adc.csPort = GPIOB;
cloud_adc.csPin = GPIO_PIN_4;
cloud_adc.VREF = MAX1112XVREF;

MAX11128_Initialize(&cloud_adc, &hspi1);
MAX11128_ADC_Config(&cloud_adc);
MAX11128_ADC_Uni_Setup(&cloud_adc);

uint16_t raw = MAX11128_ADC_ReadRawCH(&cloud_adc, 0);
```

## Notes

- Used primarily by CLOUD payload for reading resistive stripe sensors
- Can also be used for general-purpose ADC readings
- Requires proper SPI DMA configuration for optimal performance