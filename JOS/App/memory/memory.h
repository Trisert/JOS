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
int      laststates_dump_all(uint8_t *out, size_t *len);
uint32_t laststates_count(void);

/* ---------- LastStates bookkeeping mirror (SEU scrubbing, W2-5) ----------
   The write index and the entry count decide where the next post-mortem
   record lands: a bit flip there silently overwrites history, or skips the
   page erase and turns the next write into a Flash programming error. The
   pair is therefore grouped into one structure with a magic marker and
   registered with the scrubber (seu_mitigation.c), which keeps a redundant
   copy of it and rewrites it if it changes without the owner saying so.

   It stays in SRAM1 on purpose: the SRAM2 parity NMI handler writes a
   LastStates entry, and the block that just reported a parity error must not
   be on the path that records its own failure. */
#define LASTSTATES_MIRROR_MAGIC  0x4C534D52U   /* "LSMR" */

typedef struct {
    uint32_t magic;   /* LASTSTATES_MIRROR_MAGIC                            */
    uint32_t count;   /* entries written since boot (capped at the pool)    */
    uint32_t idx;     /* next write slot (circular)                         */
} laststates_mirror_t;

/* Address and size of that structure, for seu_mitigation_register_region(). */
void *laststates_mirror_region(size_t *len);

#endif /* MEMORY_H */
