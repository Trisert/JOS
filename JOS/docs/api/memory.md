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
| `laststates_init()` | Scan the pool for the first free (erased) slot; that is the next write index so post-mortem readback recovers the existing trail. |
| `laststates_write(entry)` | Append one record at the write index. If the slot still holds valid data the ring has wrapped, so the **oldest 2 KB page** it belongs to is erased — never the newest record. |
| `laststates_dump_all(out, len)` / `laststates_count()` | Read back every valid slot (the pool is scanned whole, because page recycling can leave gaps). |
| `flash_write_row` / `flash_write_dword_bounded` | Double-word program bounded by the **DWT cycle counter** (not `HAL_GetTick()`), so it can never block indefinitely in a fault handler. |
| `flash_erase_page_bounded` | Erase one 2 KB page with a DWT-bounded wait; correctly selects **bank 2** (the LastStates pool sits at `0x08080000`). |

> The pool is a ring of 64 × 128 B records. STM32L4 Flash is erased per
> 2 KB page, so a full page (16 slots) is recycled when the cursor wraps.
> Erasing only that page preserves every newer page, so the newest valid
> record is never overwritten. Every Flash operation is cycle-count bounded.
