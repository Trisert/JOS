/**
  ******************************************************************************
  * @file    scrub.c
  * @brief   SEU scrubbing subsystem (W2-5) — periodic correction of critical RAM.
  *
  * Detection half (W2-3, sram2_parity.c) flags single-bit upsets in the
  * parity-protected SRAM2 and contains them with a recorded reboot. This module
  * is the correction half for the whole OBSW:
  *
  *   - Every critical struct is mirrored as a CRC-32-protected "golden" record
  *     in FRAM (radiation-tolerant FeRAM, hardening.md §5.3). The CRC guards the
  *     *backup* itself against bit flips — a backup with a flipped bit would
  *     otherwise silently restore wrong data.
  *   - A low-priority task periodically (scrub_tick) re-reads each golden record,
  *     verifies its CRC, and repairs the live RAM from the golden copy when the
  *     live CRC differs (SCRUB_REPAIRED). Multi-bit upsets, upsets in
  *     unprotected SRAM1, and upsets the parity check missed are all corrected.
  *   - On boot, scrub_init() refreshes every registered region from FRAM before
  *     the mission logic runs, restoring the last-good struct after a reboot.
  *
  * FRAM records are written through on every trusted mutation via scrub_sync()
  * (write-through cache semantics), so the golden copy never lags the live
  * truth and the very first scrub refresh after boot already has a valid backup.
  *
  * References:
  *   - ECSS-E-ST-40C §5.4 (data integrity / fault tolerance)
  *   - NASA-STD-8739.8 (fault tolerance, no silent data corruption)
  *   - hardening.md §5.2 (SEU mitigation) and §5.3 (FRAM CRC per record)
  ******************************************************************************
  */

#include "scrub.h"
#include "boot_crc.h"     /* boot_crc32() — reused from W2.1 boot-image CRC   */

#include <string.h>

/* The FRAM transport. In the firmware this defaults to the real I2C driver
 * (memory.c); in the host unit-test build (SCRUB_NO_RTOS) the test injects a
 * fake backend via scrub_bind_fram() so no HAL/I2C symbols are needed. */
#ifndef SCRUB_NO_RTOS
#include "memory.h"       /* fram_read() / fram_write()                        */
#endif

/* ---------- Constants ---------- */
/* SCRUB_RECORD_SIZE is defined in scrub.h (public layout constant). */

/* ---------- Backend seam ----------
 * The function-pointer types are public (scrub.h) so the host unit test can
 * inject an in-memory FRAM model. */
static scrub_fram_read_fn_t  g_fram_read  = NULL;
static scrub_fram_write_fn_t g_fram_write = NULL;

#ifdef SCRUB_NO_RTOS
/* Host build only: the firmware always binds the real I2C driver in
 * scrub_ensure_fram_bound(), so exposing an override on orbit would be a way
 * to silently redirect the golden copies away from the FRAM. */
void scrub_bind_fram(scrub_fram_read_fn_t rd, scrub_fram_write_fn_t wr)
{
    g_fram_read  = rd;
    g_fram_write = wr;
}
#endif

/* ---------- Registered regions and telemetry ---------- */
typedef struct {
    void   *ram;
    size_t  len;
    bool    registered;
} scrub_region_t;

static scrub_region_t g_regions[SCRUB_MAX_REGIONS];
static uint32_t       g_repair_count   = 0U;
static uint32_t       g_fram_err_count = 0U;

/* ---------- Helpers ---------- */

/* Lazily bind the firmware FRAM backend on first registration so call order
 * between fram_init() and the first scrub_register() does not matter. */
static void scrub_ensure_fram_bound(void)
{
#ifndef SCRUB_NO_RTOS
    if (g_fram_read == NULL) {
        g_fram_read = fram_read;
    }
    if (g_fram_write == NULL) {
        g_fram_write = fram_write;
    }
#endif
}

static scrub_region_t *scrub_lookup(uint8_t region_id)
{
    if (region_id >= SCRUB_MAX_REGIONS) {
        return NULL;
    }
    if (!g_regions[region_id].registered) {
        return NULL;
    }
    return &g_regions[region_id];
}

static void scrub_put_magic(scrub_record_t *rec)
{
    rec->magic[0] = (uint8_t)(SCRUB_MAGIC >> 24);
    rec->magic[1] = (uint8_t)(SCRUB_MAGIC >> 16);
    rec->magic[2] = (uint8_t)(SCRUB_MAGIC >> 8);
    rec->magic[3] = (uint8_t)(SCRUB_MAGIC);
}

static bool scrub_check_magic(const scrub_record_t *rec)
{
    return (rec->magic[0] == (uint8_t)(SCRUB_MAGIC >> 24)) &&
           (rec->magic[1] == (uint8_t)(SCRUB_MAGIC >> 16)) &&
           (rec->magic[2] == (uint8_t)(SCRUB_MAGIC >> 8)) &&
           (rec->magic[3] == (uint8_t)(SCRUB_MAGIC));
}

/* ---------- Public API ---------- */

scrub_status_t scrub_register(void *ram, size_t len, uint8_t region_id)
{
    if ((ram == NULL) || (len == 0U) || (len > SCRUB_MAX_REGION_SIZE)) {
        return SCRUB_ERR_INVALID;
    }
    if (region_id >= SCRUB_MAX_REGIONS) {
        return SCRUB_ERR_INVALID;
    }

    scrub_ensure_fram_bound();

    g_regions[region_id].ram        = ram;
    g_regions[region_id].len        = len;
    g_regions[region_id].registered = true;
    return SCRUB_OK;
}

scrub_status_t scrub_sync(uint8_t region_id)
{
    const scrub_region_t *r;
    scrub_record_t    rec;
    uint32_t          slot;

    r = scrub_lookup(region_id);
    if (r == NULL) {
        return SCRUB_ERR_INVALID;
    }
    if ((g_fram_read == NULL) || (g_fram_write == NULL)) {
        return SCRUB_ERR_FRAM;
    }

    memset(&rec, 0, sizeof(rec));
    scrub_put_magic(&rec);
    rec.region_id   = region_id;
    rec.payload_len = (uint32_t)r->len;
    memcpy(rec.payload, r->ram, r->len);
    rec.crc32 = boot_crc32(rec.payload, r->len);

    slot = SCRUB_SLOT_ADDR(region_id);
    if (g_fram_write(slot, (const uint8_t *)&rec, SCRUB_RECORD_SIZE) != 0) {
        g_fram_err_count++;
        return SCRUB_ERR_FRAM;
    }
    return SCRUB_OK;
}

scrub_status_t scrub_refresh(uint8_t region_id)
{
    const scrub_region_t *r;
    scrub_record_t    rec;
    uint32_t          slot;
    uint32_t          live_crc;
    uint32_t          golden_crc;

    r = scrub_lookup(region_id);
    if (r == NULL) {
        return SCRUB_ERR_INVALID;
    }
    if ((g_fram_read == NULL) || (g_fram_write == NULL)) {
        return SCRUB_ERR_FRAM;
    }

    slot = SCRUB_SLOT_ADDR(region_id);
    if (g_fram_read(slot, (uint8_t *)&rec, SCRUB_RECORD_SIZE) != 0) {
        g_fram_err_count++;
        return SCRUB_ERR_FRAM;
    }

    /* The golden record itself must be trustworthy before we use it. */
    if (!scrub_check_magic(&rec) || (rec.payload_len != r->len)) {
        return SCRUB_ERR_MAGIC;
    }
    golden_crc = boot_crc32(rec.payload, rec.payload_len);
    if (golden_crc != rec.crc32) {
        /* FRAM bit flip: the backup is corrupt. Do NOT copy it into live RAM. */
        return SCRUB_ERR_CRC;
    }

    /* Compare live RAM to the verified golden copy. */
    live_crc = boot_crc32(r->ram, r->len);
    if (live_crc != golden_crc) {
        memcpy(r->ram, rec.payload, r->len);
        g_repair_count++;
        return SCRUB_REPAIRED;
    }
    return SCRUB_NO_CHANGE;
}

uint32_t scrub_tick(void)
{
    uint32_t repairs = 0U;
    for (uint8_t id = 0U; id < SCRUB_MAX_REGIONS; id++) {
        if (!g_regions[id].registered) {
            continue;
        }
        if (scrub_refresh(id) == SCRUB_REPAIRED) {
            repairs++;
        }
    }
    return repairs;
}

scrub_status_t scrub_init(void)
{
    scrub_status_t worst = SCRUB_OK;

    /* Boot-time repair: restore every registered region from its golden copy
     * before any mission logic depends on it. Errors here mean "no valid
     * backup yet" (first boot) or "backup corrupt", not a live-RAM fault. */
    for (uint8_t id = 0U; id < SCRUB_MAX_REGIONS; id++) {
        scrub_status_t s;
        if (!g_regions[id].registered) {
            continue;
        }
        s = scrub_refresh(id);
        if ((s == SCRUB_ERR_FRAM) || (s == SCRUB_ERR_MAGIC) || (s == SCRUB_ERR_CRC)) {
            worst = s;
        }
    }
    return worst;
}

uint32_t scrub_repair_count(void)
{
    return g_repair_count;
}

uint32_t scrub_fram_error_count(void)
{
    return g_fram_err_count;
}

/* Reset all registered regions and telemetry counters. Intended for host
 * unit tests (which run many cases in one executable) and for any debug
 * re-initialisation; the firmware boots with zeroed statics and never calls
 * this on orbit. Does not touch the FRAM contents. */
void scrub_reset(void)
{
    for (uint8_t id = 0U; id < SCRUB_MAX_REGIONS; id++) {
        g_regions[id].ram        = NULL;
        g_regions[id].len        = 0U;
        g_regions[id].registered = false;
    }
    g_repair_count   = 0U;
    g_fram_err_count = 0U;
}

/* ---------- RTOS task (firmware build only) ---------- */
#ifndef SCRUB_NO_RTOS

#include "cmsis_os.h"
#include "FreeRTOS.h"     /* pdMS_TO_TICKS()                                */
#include "task.h"

/* Scrub cadence. LEO PocketQube: a few-second interval keeps the SEU window
 * tiny without perturbing the 10 Hz state machine or the comms tasks. */
#ifndef SCRUB_PERIOD_MS
#define SCRUB_PERIOD_MS  5000u
#endif

static void scrub_task(void *arg)
{
    (void)arg;
    for (;;) {
        scrub_tick();
        osDelay(pdMS_TO_TICKS(SCRUB_PERIOD_MS));
    }
}

osThreadId_t scrub_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "scrub",
        .stack_size = 256 * 4,
        .priority   = osPriorityLow,
    };
    return osThreadNew(scrub_task, NULL, &attrs);
}

#endif /* SCRUB_NO_RTOS */
