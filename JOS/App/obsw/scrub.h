#ifndef SCRUB_H
#define SCRUB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The Ceedling host build compiles this module for x86-64 with no HAL, no
 * CMSIS-RTOS and no I2C. It defines HOST_UNIT_TEST for every test executable
 * (JOS/test/project.yml), so derive the RTOS/FRAM opt-out from it instead of
 * requiring a second, easily-forgotten -D on the test build. */
#if defined(HOST_UNIT_TEST) && !defined(SCRUB_NO_RTOS)
#define SCRUB_NO_RTOS 1
#endif

/* ---------------------------------------------------------------------------
 * SEU scrubbing subsystem (W2-5) — periodic correction of critical RAM.
 *
 * W2-3 (sram2_parity.c) detects single-bit upsets in the parity-protected
 * SRAM2 block and contains them with a recorded reboot. Parity catches a
 * single flipped bit per byte but NOT multi-bit upsets or upsets in the
 * unprotected SRAM1 (256 KB) where most of the OBSW state actually lives, and
 * it only *detects* — it does not repair the RAM it protected.
 *
 * This module is the correction half. Each critical struct is given a
 * CRC-32-protected "golden" copy in FRAM (radiation-tolerant FeRAM, see
 * hardening.md §5.3). A low-priority task periodically:
 *   1. reads the golden record back from FRAM and verifies its CRC (so the
 *      *backup* itself is checked for bit flips),
 *   2. compares the live RAM CRC to the golden CRC,
 *   3. repairs the live RAM from the golden copy when they differ.
 * After a parity-NMI reboot the same refresh restores the last-good struct
 * from FRAM before the mission logic runs.
 *
 * FRAM record layout (fixed slot per region id, big-endian-free / packed):
 *   offset 0  : magic[4]      = "SCUR"
 *   offset 4  : region_id     (1 byte)
 *   offset 5  : reserved[3]
 *   offset 8  : crc32         (4) over payload[0 .. payload_len-1]
 *   offset 12 : payload_len   (4)
 *   offset 16 : payload[payload_len]
 *
 * References:
 *   - ECSS-E-ST-40C §5.4 (data integrity / fault tolerance)
 *   - NASA-STD-8739.8 (fault tolerance, no silent data corruption)
 *   - hardening.md §5.2 (SEU mitigation) and §5.3 (FRAM CRC per record)
 * ------------------------------------------------------------------------- */

/* Marker so ground can find these records inside the FRAM dump. */
#define SCRUB_MAGIC             0x53435552u   /* "SCUR" */

/* Hard cap on one registered struct. Covers obsw_state (~20 B) and any future
 * critical struct; fixed slot size keeps the FRAM offset arithmetic trivial. */
#define SCRUB_MAX_REGION_SIZE  256u
#define SCRUB_RECORD_HEADER    16u            /* magic+id+reserved+crc+len   */
#define SCRUB_RECORD_SIZE      (SCRUB_RECORD_HEADER + SCRUB_MAX_REGION_SIZE)
#define SCRUB_MAX_REGIONS      8u             /* FRAM slots reserved         */

/* First FRAM address used by the scrub pool. The FRAM (64 KB total, see
 * memory.c FM24VN_*) is shared with the cyclic_buffer, which starts at
 * offset 0 and advances a head pointer. To avoid clobbering each other at
 * runtime we deliberately place the scrub pool at the TOP of FRAM and grow
 * downward by slot index; the cyclic buffer owns the bottom region. Even on
 * an allocation collision the scrub records are CRC-protected and the
 * golden-copy check rejects any garbage it does not own, so a stray
 * cyclic_buffer write cannot silently restore wrong data. */
#define SCRUB_FRAM_BASE        (0x10000u - (SCRUB_MAX_REGIONS * SCRUB_RECORD_SIZE))

/* FRAM byte address of region id's golden record slot. */
#define SCRUB_SLOT_ADDR(id)    ((uint32_t)(SCRUB_FRAM_BASE) + (uint32_t)(id) * SCRUB_RECORD_SIZE)

/* Stable region ids. A region id is also its FRAM slot index. */
#define SCRUB_REGION_OBSW_STATE  0u

typedef enum {
    SCRUB_OK           =  0,   /* operation completed                          */
    SCRUB_NO_CHANGE    =  1,   /* refresh ran, RAM already matched golden      */
    SCRUB_REPAIRED     =  2,   /* RAM was corrupted, restored from golden      */
    SCRUB_ERR_INVALID  = -1,   /* NULL/bad arg or unknown region id            */
    SCRUB_ERR_FRAM     = -2,   /* FRAM read/write transport failed             */
    SCRUB_ERR_MAGIC    = -3,   /* golden record missing / garbage (never sync) */
    SCRUB_ERR_CRC      = -4,   /* golden record CRC mismatch (FRAM bit flip)   */
} scrub_status_t;

/* Wire-format FRAM record (packed to match the byte layout above). */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];              /* SCRUB_MAGIC                                */
    uint8_t  region_id;            /* SLOT index / region id                     */
    uint8_t  reserved[3];
    uint32_t crc32;                /* CRC-32 (IEEE) over payload[0..len-1]       */
    uint32_t payload_len;          /* bytes of valid payload                     */
    uint8_t  payload[SCRUB_MAX_REGION_SIZE];
} scrub_record_t;

/* ---------- Public API ---------- */

/* FRAM transport seam. The firmware binds the real I2C driver
 * (memory.c fram_read/fram_write) automatically; the host unit-test build
 * injects an in-memory model with scrub_bind_fram() below. */
typedef int (*scrub_fram_read_fn_t)(uint32_t addr, uint8_t *buf, size_t len);
typedef int (*scrub_fram_write_fn_t)(uint32_t addr, const uint8_t *buf, size_t len);

/* Register a critical RAM region to be scrubbed.
 *   ram       : address of the struct (SRAM1 or SRAM2)
 *   len       : size in bytes, must be <= SCRUB_MAX_REGION_SIZE
 *   region_id : stable slot index 0..SCRUB_MAX_REGIONS-1
 * Call once at boot, before scrub_init() / the scrub task start. */
scrub_status_t scrub_register(void *ram, size_t len, uint8_t region_id);

/* Write-through: snapshot the current (trusted) RAM into the FRAM golden
 * record, computing and storing its CRC. Call after every trusted mutation
 * (e.g. a committed state transition) so the backup never lags the truth. */
scrub_status_t scrub_sync(uint8_t region_id);

/* Refresh one region: verify the golden CRC, compare live vs golden, and
 * repair the live RAM from the golden copy if they differ. Returns
 * SCRUB_REPAIRED on a corrected upset, SCRUB_NO_CHANGE if already healthy. */
scrub_status_t scrub_refresh(uint8_t region_id);

/* One scrub pass over all registered regions. Returns the number of repairs
 * performed this pass (0 = all healthy). */
uint32_t scrub_tick(void);

/* Boot-time repair: refresh every registered region from FRAM before the
 * mission logic runs. Call after fram_init() and after all scrub_register()
 * calls (i.e. after state_machine_init()). */
scrub_status_t scrub_init(void);

/* Telemetry counters (read by beacon / TM). */
uint32_t scrub_repair_count(void);      /* total upsets corrected since boot */
uint32_t scrub_fram_error_count(void);  /* FRAM transport failures since boot */

/* Reset all registered regions and telemetry counters. Primarily a host-unit-
 * test hook (Ceedling runs every case in one executable); the firmware boots
 * with zeroed statics and does not call this on orbit. Does not touch FRAM. */
void scrub_reset(void);

/* ---------- RTOS glue (compiled only in the firmware build) ----------
 * In the host unit-test build SCRUB_NO_RTOS is defined, the task wrapper is
 * compiled out, and the FRAM backend is supplied by the test instead. */
#ifndef SCRUB_NO_RTOS
#include "cmsis_os.h"
osThreadId_t scrub_task_create(void);
#else
/* ---------- Host-test seam ----------
 * With SCRUB_NO_RTOS the module cannot reach the real I2C FRAM, so the test
 * injects an in-memory model. Declared here (not only defined in the .c) so
 * the host build has a prototype: the test build runs with -Wall -Wextra
 * -Werror, where an undeclared call is an implicit-declaration error. */
void scrub_bind_fram(scrub_fram_read_fn_t rd, scrub_fram_write_fn_t wr);
#endif

#endif /* SCRUB_H */
