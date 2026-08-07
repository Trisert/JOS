#ifndef MEMORY_H
#define MEMORY_H

#include "obsw_types.h"
#include <stdint.h>
#include <stddef.h>

/* ---------- FRAM driver ---------- */
void fram_init(void);
int  fram_read(uint32_t addr, uint8_t *buf, size_t len);
int  fram_write(uint32_t addr, const uint8_t *buf, size_t len);

/* ---------- Cyclic buffer ---------- */
void cyclic_buffer_init(void);
int  cyclic_buffer_write(const uint8_t *data, size_t len);
int  cyclic_buffer_read(uint32_t offset, uint8_t *buf, size_t len);
uint32_t cyclic_buffer_head(void);

/* ---------- LastStates pool (internal Flash) ---------- */
void     laststates_init(void);
int      laststates_write(const laststates_entry_t *entry);
/* laststates_dump_all(): *len is IN/OUT -- set it to the capacity of `out`
 * before the call; on success it returns the number of bytes written. A
 * buffer smaller than laststates_count() * LASTSTATES_ENTRY_SIZE is refused
 * (returns -1, *len = required size) instead of being overrun. */
int      laststates_dump_all(uint8_t *out, size_t *len);
uint32_t laststates_count(void);

#endif /* MEMORY_H */
