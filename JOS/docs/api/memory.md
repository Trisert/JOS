# Memory API — FRAM + Flash (`App/memory/`)

## FRAM Cyclic Buffer (`App/memory/memory.c`)

512 KB external FRAM (4 × FM24VN10-G on I2C1, PB8/PB9) — primary payload data sink.

| Function | Purpose |
|----------|---------|
| `fram_init()` | Probe all eight I2C1 FRAM device selects at boot (10 ms per trial, watchdog refreshed between selects while the scheduler is not running) |
| `cyclic_buffer_write(const uint8_t *data, size_t len)` | Append; overwrite oldest on wrap |
| `cyclic_buffer_read(uint32_t offset, uint8_t *buf, size_t len)` | Retrieve for downlink |

Oldest data overwritten first — graceful degradation, no fault.

**Partition** (`memory.h`): the cyclic buffer owns `[0, FRAM_CYCLIC_BYTES)` and
wraps inside it; the top `FRAM_GOLDEN_BYTES` (4 KB, from `FRAM_GOLDEN_BASE`)
hold the SEU golden records (`Core/Inc/seu_mitigation.h` `SEU_FRAM_*`). The
two never overlap (static asserts on both sides). The buffer head is not yet
persisted across a reset (`cyclic_buffer_init()` starts at 0).

## LastStates Pool (Flash)

See `docs/api/obsw.md` §LastStates Pool. Reserved 8 KB at `0x08080000`,
exposed as `LASTSTATES` region in `STM32L496VGTX_FLASH.ld`.

## Flash Write Primitive (`App/memory/memory.c`)

| Function | Purpose |
|----------|---------|
| `laststates_init()` | Re-derive the write cursor from the erase state: the first slot of the (single) erased run, i.e. right after the newest record. |
| `laststates_write(entry)` | Append one record at the cursor. When the write fills the last slot of a 2 KB page, the **next page — the oldest in ring order — is erased immediately** (erase-ahead). If that erase fails, the next write retries it (`laststates_erase_ahead_failures()`). A program failure returns -1: if nothing reached the cells the cursor stays and the next write retries the slot; if the slot is **torn** it is consumed like a written one (cursor advanced, erase-ahead run), so a torn last free slot of a wrapped ring can never make the next write fall back to slot 0 and erase the page with the newest records. |
| `laststates_dump_all(out, len)` / `laststates_count()` | Read back every complete record, **oldest first** (ring order from `laststates_oldest_slot()`). |
| `laststates_oldest_slot()` | Index of the oldest record in ring order; `LASTSTATES_MAX_ENTRIES` when empty. Readers that need time order use it (dual-bank boot-fault count). |
| `flash_write_row` / `flash_write_dword_bounded` | Double-word program bounded by the **DWT cycle counter** (not `HAL_GetTick()`), so it can never block indefinitely in a fault handler. |
| `flash_erase_page_bounded` | Erase one 2 KB page with a DWT-bounded wait; correctly selects **bank 2** (the LastStates pool sits at `0x08080000`). |

> The pool is a ring of 64 × 128 B slots. STM32L4 Flash is erased per 2 KB
> page (16 slots). The entry has no sequence number (frozen ICD), so the
> cursor is recovered from the erase state alone; that is why the ring always
> keeps one erased run (erase-ahead). Consequence: once the ring has wrapped
> it holds **48 to 63 records**, not 64. The oldest page is always the one
> recycled, so the newest records are never lost. (Before the fix the cursor
> was "first erased slot from index 0", which after the first wrap kept
> recycling page 0 — the newest records — and froze pages 1–3.) Every Flash
> operation is cycle-count bounded.
