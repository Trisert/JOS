# Memory API — FRAM + Flash (`App/memory/`)

## FRAM Cyclic Buffer (`App/memory/fram.c`, `cyclic_buffer.c`)

4 MB external FRAM (SPI2) — primary payload data sink.

| Function | Purpose |
|----------|---------|
| `fram_init()` | Init SPI2 FRAM |
| `cyclic_buffer_write(uint8_t *row, size_t len)` | Append; overwrite oldest on wrap |
| `cyclic_buffer_read(...)` | Retrieve for downlink |

Oldest data overwritten first — graceful degradation, no fault.

## LastStates Pool (Flash)

See `docs/api/obsw.md` §LastStates Pool. Reserved 8 KB at `0x08080000`,
exposed as `LASTSTATES` region in `STM32L496VGTX_FLASH.ld`.

## Flash Write Primitive (`App/memory/memory.c`)

| Function | Purpose |
|----------|---------|
| `flash_write_row(uint32_t addr, uint64_t *dw, size_t n)` | Double-word program via HAL_FLASH; unlocks, clears errors, writes, relocks. **Pages** (not sectors) on STM32L4. |
| `flash_erase_pool()` | Erase/reclaim LastStates pages before reuse on wrap. |

> Implemented in PR #1. The pool erase uses STM32L4 **pages**
> (`FLASH_TYPEERASE_PAGES`, `Page/NbPages`, `VOLTAGE_RANGE_3`), not sectors.
