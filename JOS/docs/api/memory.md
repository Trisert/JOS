# Memory API Reference

## Overview

The Memory module provides non-volatile storage using FRAM (Ferroelectric RAM) and internal Flash. Located in `App/memory/`.

## Dependencies

- Header: `App/memory/memory.h`
- Types: `App/obsw_types.h`
- Hardware: SPI2 (FRAM), Internal Flash

## Hardware

- **FRAM:** MB85RS4MT (4 Mbit / 512 KB)
- **Interface:** SPI2
- **Flash:** Internal STM32L4 Flash (8 KB reserved at 0x08080000)

## Public API

### FRAM Driver

#### `void fram_init(void)`

Initialize FRAM.

**Parameters:** None

**Returns:** Nothing

**Notes:**
- TODO: Verify device ID via RDID command

---

#### `int fram_read(uint32_t addr, uint8_t *buf, size_t len)`

Read from FRAM.

**Parameters:**
- `addr` - FRAM address (0 to FRAM_SIZE-1)
- `buf` - Output buffer
- `len` - Number of bytes to read

**Returns:** `int` - 0 on success

---

#### `int fram_write(uint32_t addr, const uint8_t *buf, size_t len)`

Write to FRAM.

**Parameters:**
- `addr` - FRAM address (0 to FRAM_SIZE-1)
- `buf` - Input data
- `len` - Number of bytes to write

**Returns:** `int` - 0 on success

---

### Cyclic Buffer

#### `void cyclic_buffer_init(void)`

Initialize cyclic buffer.

**Parameters:** None

**Returns:** Nothing

**Notes:**
- TODO: Read persisted head from known FRAM address

---

#### `int cyclic_buffer_write(const uint8_t *data, size_t len)`

Write to cyclic buffer.

**Parameters:**
- `data` - Input data
- `len` - Data length

**Returns:** `int` - 0 on success

**Notes:**
- Handles wrap-around automatically

---

#### `int cyclic_buffer_read(uint32_t offset, uint8_t *buf, size_t len)`

Read from cyclic buffer.

**Parameters:**
- `offset` - Offset from buffer start
- `buf` - Output buffer
- `len` - Number of bytes to read

**Returns:** `int` - 0 on success, -1 on error

---

#### `uint32_t cyclic_buffer_head(void)`

Get current buffer head position.

**Parameters:** None

**Returns:** `uint32_t` - Current head index

---

### LastStates Pool

#### `void laststates_init(void)`

Initialize LastStates pool.

**Parameters:** None

**Returns:** Nothing

**Notes:**
- TODO: Scan Flash to find valid entries

---

#### `int laststates_write(const laststates_entry_t *entry)`

Write state transition entry.

**Parameters:**
- `entry` - Pointer to laststates_entry_t

**Returns:** `int` - 0 on success, -1 on error

---

#### `int laststates_dump_all(uint8_t *out, size_t *len)`

Dump all LastStates entries.

**Parameters:**
- `out` - Output buffer
- `len` - Pointer to output length (filled on return)

**Returns:** `int` - 0 on success, -1 on error

---

#### `uint32_t laststates_count(void)`

Get number of entries written.

**Parameters:** None

**Returns:** `uint32_t` - Entry count

---

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `FRAM_SIZE` | 4 MB | FRAM size (4UL * 1024 * 1024) |
| `LASTSTATES_FLASH_BASE` | 0x08080000 | Flash base address |
| `LASTSTATES_ENTRY_SIZE` | 128 | Entry size (bytes) |
| `LASTSTATES_MAX_ENTRIES` | 64 | Maximum entries |

## Notes

- FRAM allows infinite read/write cycles (no wear)
- Flash has limited write cycles (used only for critical LastStates)
- Cyclic buffer used for payload data logging