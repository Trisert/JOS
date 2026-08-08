/**
  ******************************************************************************
  * @file    seu_mitigation.c
  * @brief   Single-Event-Upset mitigation: periodic memory scrubbing (W2-5).
  *
  * See seu_mitigation.h for the rationale and the ownership contract. In
  * short: SRAM2 parity (W2-3) detects a corrupted byte when it is read and
  * answers with a reset; this module reads the critical structures on a timer
  * so a dormant upset is found early, and keeps a redundant copy of the ones
  * that can be voted on so the upset is *corrected* instead of costing a
  * reboot.
  *
  * Redundancy layout for a SEU_POLICY_GOLDEN region - three copies of the
  * truth, in three different places, so no single upset can defeat the vote:
  *
  *     live object      SRAM2 or SRAM1, owned by the application
  *     shadow copy      SRAM2 shadow pool (parity covered, different address)
  *     reference CRC-32 SRAM1, inside this module's region table
  *
  * Standards: NASA-STD-8739.8 (fault tolerance, data integrity, no silent
  *            failure), ECSS-E-ST-40C, ECSS-Q-ST-80C Rev.2 (FDIR),
  *            NASA Power of Ten #3 (no allocation after init) and #2 (all
  *            loops have a fixed upper bound).
  ******************************************************************************
  */

#include "seu_mitigation.h"

#include "main.h"           /* HAL, CMSIS core (PRIMASK, RTC backup regs)    */
#include "FreeRTOS.h"
#include "task.h"

#include "sram2_parity.h"   /* SRAM2 placement + sram2_restore_from_image()  */
#include "memory.h"         /* laststates_write(), laststates_mirror_region()*/
#include "obsw_types.h"     /* laststates_entry_t, TRIGGER_SEU_SCRUB         */
#include "state_machine.h"  /* state_machine_critical_region()               */
#include "comms.h"          /* comms_*_buffer()                              */

#include <string.h>

/* Largest object the scrubber will vote on. Bounds the staging buffer and,
   with it, the worst-case duration of a scrub step. */
#ifndef SEU_MAX_REGION_BYTES
#define SEU_MAX_REGION_BYTES    256U
#endif

/* A scrub record travels to ground inside a LastStates context blob. */
_Static_assert(sizeof(seu_record_t) <=
                   sizeof(((laststates_entry_t *)0)->context),
               "seu_record_t does not fit in a LastStates context blob");

/* The scrub task cannot take the state mutex safely from every context, so
   the state fields are marked unknown - same convention as the fault and
   parity handlers. */
#define SEU_STATE_UNKNOWN       0xFFU

/* Backup-register word: "SE" in the upper half, saturating count in the lower. */
#define SEU_BKP_MAGIC           0x5345U
#define SEU_BKP_COUNT_MASK      0x0000FFFFU
#define SEU_BKP_WORD(count)     (((uint32_t)SEU_BKP_MAGIC << 16) | \
                                 ((count) & SEU_BKP_COUNT_MASK))

/* ---------- Module state (SRAM1) ----------
   The region table holds the reference CRCs, which are one leg of the vote,
   so it deliberately does not share a memory block with the shadow pool. */
typedef struct {
    void        *addr;        /* live object                                */
    size_t       len;         /* size in bytes                              */
    uint8_t     *shadow;      /* shadow copy (NULL for SEU_POLICY_TOUCH)    */
    uint32_t     crc;         /* reference CRC-32 taken at the last commit  */
    uint16_t     id;          /* seu_region_id_t                            */
    uint8_t      policy;      /* seu_policy_t                               */
    uint8_t      used;        /* slot occupied                              */
    uint32_t     mismatches;  /* per-region corruption count                */
} seu_region_t;

static seu_region_t seu_regions[SEU_MAX_REGIONS];
static uint32_t     seu_region_used;
static seu_stats_t  seu_stats;
static uint8_t      seu_initialised;
static uint8_t      seu_bkp_ready;          /* backup register accessible    */
static volatile uint32_t seu_nmi_events;    /* NMIs counted since power-on   */
static volatile uint32_t seu_lock_nesting;
static volatile uint32_t seu_lock_primask;

/* Staging copy of a live object, so the CRC is computed outside the lock. */
static uint8_t seu_work[SEU_MAX_REGION_BYTES];
/* Matching staging copy of the shadow: the three legs of the vote must come
   from the same instant, or a commit landing mid-pass looks like corruption. */
static uint8_t seu_shadow_work[SEU_MAX_REGION_BYTES];

/* Sink for the touch-scrub reads: volatile and file scope, so neither the
   reads nor the accumulation can be optimised away. */
static volatile uint32_t seu_touch_sink;

/* ---------- Shadow pool (SRAM2, hardware parity) ----------
   NOLOAD: the SRAM2 hardware erase performed by sram2_parity_init() zeroes it
   with valid parity, so it costs no Flash. Bump-allocated at registration
   time and never freed (NASA Power of Ten #3). */
static SRAM2_CRITICAL_NOINIT uint8_t seu_shadow_pool[SEU_SHADOW_POOL_BYTES];
static size_t seu_shadow_used;

static const osThreadAttr_t seu_task_attrs = {
    .name       = "seuScrub",
    .stack_size = SEU_SCRUB_TASK_STACK,
    .priority   = osPriorityLow,
};

/* ---------- Interrupt-safe mutual exclusion ---------- */

void seu_mitigation_lock(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    if (seu_lock_nesting == 0U) {
        seu_lock_primask = primask;
    }
    seu_lock_nesting++;
}

void seu_mitigation_unlock(void)
{
    if (seu_lock_nesting == 0U) {
        return;   /* unbalanced call: never re-enable interrupts by accident */
    }
    seu_lock_nesting--;
    if ((seu_lock_nesting == 0U) && (seu_lock_primask == 0U)) {
        __enable_irq();
    }
}

/* ---------- CRC-32 (reflected, polynomial 0xEDB88320) ----------
   Bitwise on purpose: no 1 KB lookup table in Flash, and the regions are a
   few tens of bytes each, so the cost is irrelevant next to the scrub period.
   The STM32 CRC peripheral is left free for the boot-image check (W3-1). */
static uint32_t seu_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            uint32_t mask = (uint32_t)0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t seu_popcount8(uint8_t v)
{
    uint32_t n = 0U;
    for (uint32_t i = 0U; i < 8U; i++) {
        n += ((uint32_t)v >> i) & 1U;
    }
    return n;
}

/* Count differing bytes/bits between two images; report the first bad offset. */
static uint32_t seu_diff(const uint8_t *a, const uint8_t *b, size_t len,
                         uint32_t *bit_errors, uint32_t *first_offset)
{
    uint32_t bytes = 0U;
    uint32_t bits  = 0U;
    uint32_t first = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i++) {
        uint8_t x = (uint8_t)(a[i] ^ b[i]);
        if (x != 0U) {
            bytes++;
            bits += seu_popcount8(x);
            if (first == 0xFFFFFFFFU) {
                first = (uint32_t)i;
            }
        }
    }

    if (bit_errors != NULL)   { *bit_errors = bits; }
    if (first_offset != NULL) { *first_offset = first; }
    return bytes;
}

/* ---------- Persistent SEU counter (RTC backup domain) ---------- */

#if (SEU_USE_BACKUP_REGISTER == 1)
static volatile uint32_t *seu_bkp(uint32_t index)
{
    /* BKP0R..BKP31R are contiguous 32-bit registers in the RTC block. */
    return &((volatile uint32_t *)&RTC->BKP0R)[index];
}

static void seu_bkp_init(void)
{
    uint32_t word;

    /* Backup registers sit behind the backup-domain write protection and
       behind the RTC APB interface clock; neither the RTC clock source nor
       the calendar is needed to use them as scratch storage. */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTCAPB_CLK_ENABLE();

    word = *seu_bkp(SEU_BKP_COUNT_INDEX);
    if ((word >> 16) != (uint32_t)SEU_BKP_MAGIC) {
        /* First boot, or the backup domain lost power / was reset. */
        *seu_bkp(SEU_BKP_COUNT_INDEX) = SEU_BKP_WORD(0U);
        *seu_bkp(SEU_BKP_ACK_INDEX)   = SEU_BKP_WORD(0U);
    }
    seu_bkp_ready = 1U;
}

static uint32_t seu_bkp_count(void)
{
    uint32_t word;

    if (seu_bkp_ready == 0U) {
        return 0U;
    }
    word = *seu_bkp(SEU_BKP_COUNT_INDEX);
    if ((word >> 16) != (uint32_t)SEU_BKP_MAGIC) {
        return 0U;
    }
    return word & SEU_BKP_COUNT_MASK;
}

static void seu_bkp_increment(void)
{
    uint32_t count;

    if (seu_bkp_ready == 0U) {
        return;
    }
    count = seu_bkp_count();
    if (count < SEU_BKP_COUNT_MASK) {   /* saturate instead of wrapping */
        count++;
    }
    *seu_bkp(SEU_BKP_COUNT_INDEX) = SEU_BKP_WORD(count);
}

static uint32_t seu_bkp_acked(void)
{
    uint32_t word;

    if (seu_bkp_ready == 0U) {
        return 0U;
    }
    word = *seu_bkp(SEU_BKP_ACK_INDEX);
    if ((word >> 16) != (uint32_t)SEU_BKP_MAGIC) {
        return 0U;
    }
    return word & SEU_BKP_COUNT_MASK;
}

static void seu_bkp_ack(uint32_t count)
{
    if (seu_bkp_ready != 0U) {
        *seu_bkp(SEU_BKP_ACK_INDEX) = SEU_BKP_WORD(count);
    }
}
#else  /* SEU_USE_BACKUP_REGISTER */
static void     seu_bkp_init(void)            { seu_bkp_ready = 0U; }
static uint32_t seu_bkp_count(void)           { return seu_nmi_events; }
static void     seu_bkp_increment(void)       { }
static uint32_t seu_bkp_acked(void)           { return 0U; }
static void     seu_bkp_ack(uint32_t count)   { (void)count; }
#endif /* SEU_USE_BACKUP_REGISTER */

/* ---------- LastStates logging ---------- */

static void seu_log(uint32_t event_id, const seu_region_t *region,
                    uint32_t crc_live, uint32_t crc_shadow,
                    uint32_t byte_errors, uint32_t bit_errors,
                    uint32_t first_offset)
{
    laststates_entry_t entry;
    seu_record_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.magic            = SEU_RECORD_MAGIC;
    rec.event_id         = event_id;
    rec.crc_live         = crc_live;
    rec.crc_shadow       = crc_shadow;
    rec.byte_errors      = byte_errors;
    rec.bit_errors       = bit_errors;
    rec.first_bad_offset = first_offset;

    if (region != NULL) {
        rec.region_id     = (uint32_t)region->id;
        rec.region_addr   = (uint32_t)(uintptr_t)region->addr;
        rec.region_len    = (uint32_t)region->len;
        rec.crc_reference = region->crc;
    } else {
        rec.region_id = (uint32_t)SEU_REGION_ID_COUNT;   /* not region bound */
    }

    rec.stats = seu_stats;

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = HAL_GetTick();
    entry.state_from = SEU_STATE_UNKNOWN;
    entry.state_to   = SEU_STATE_UNKNOWN;
    entry.trigger    = TRIGGER_SEU_SCRUB;
    memcpy(entry.context, &rec, sizeof(rec));

    /* Best effort: a failed Flash write must not stop the scrub pass, the
       repair itself has already been applied. */
    (void)laststates_write(&entry);
}

/* ---------- Region helpers ---------- */

static seu_region_t *seu_find(seu_region_id_t id)
{
    for (uint32_t i = 0U; i < SEU_MAX_REGIONS; i++) {
        if ((seu_regions[i].used != 0U) &&
            (seu_regions[i].id == (uint16_t)id)) {
            return &seu_regions[i];
        }
    }
    return NULL;
}

/* Bounds check on a region descriptor (review C2).
   The descriptor table is itself in RAM and therefore itself a target: an
   upset in `len` would make the scrubber memcpy far past the staging buffers,
   and a NULL `addr` / `shadow` would fault inside the scrub task. Every path
   that dereferences a descriptor validates it first and treats a bad one as a
   recorded fault instead of trusting it. */
static int seu_region_valid(const seu_region_t *r)
{
    if (r == NULL) {
        return 0;
    }
    if ((r->addr == NULL) || (r->len == 0U) || (r->len > SEU_MAX_REGION_BYTES)) {
        return 0;
    }
    if (((seu_policy_t)r->policy == SEU_POLICY_GOLDEN) && (r->shadow == NULL)) {
        return 0;
    }
    if ((seu_policy_t)r->policy != SEU_POLICY_GOLDEN &&
        (seu_policy_t)r->policy != SEU_POLICY_TOUCH) {
        return 0;
    }
    return 1;
}

/* Record a descriptor that failed the bounds check. Never dereferences the
   suspect pointers - only the scalar fields go into the record. */
static void seu_region_reject(const seu_region_t *r)
{
    seu_stats.region_faults++;
    seu_log(SEU_EVENT_REGION_INVALID, r, 0U, 0U, 0U, 0U, 0xFFFFFFFFU);
}

/* Containment for an upset no vote can repair (review C1).
   The Flash load image is NOT written back: it holds the compile-time
   defaults (state OFF, soc 100 %) and restoring them in place would mask a
   dead battery as a healthy one. The honest, safe answer is to leave the
   corrupt object alone, record it, and drive the OBSW into the beacon-only
   safe state so ground decides what happens next. */
static void seu_enter_safe_state(const seu_region_t *r)
{
    if (state_machine_get_state() == STATE_CRIT) {
        return;   /* already contained */
    }
    if (state_machine_request_transition(STATE_CRIT, TRIGGER_SEU_SCRUB) == 0) {
        seu_stats.escalations++;
        seu_log(SEU_EVENT_SAFE_STATE, r, 0U, 0U, 0U, 0U, 0xFFFFFFFFU);
    }
}

int seu_mitigation_register_region(seu_region_id_t id, void *addr, size_t len,
                                   seu_policy_t policy)
{
    seu_region_t *slot = NULL;

    if ((addr == NULL) || (len == 0U)) {
        return -1;
    }
    if (seu_find(id) != NULL) {
        return -1;                       /* already registered */
    }
    if (seu_region_used >= SEU_MAX_REGIONS) {
        return -1;                       /* table full */
    }
    if ((policy == SEU_POLICY_GOLDEN) && (len > SEU_MAX_REGION_BYTES)) {
        return -1;                       /* would not fit the staging buffer */
    }

    slot = &seu_regions[seu_region_used];
    memset(slot, 0, sizeof(*slot));
    slot->addr   = addr;
    slot->len    = len;
    slot->id     = (uint16_t)id;
    slot->policy = (uint8_t)policy;

    if (policy == SEU_POLICY_GOLDEN) {
        if ((SEU_SHADOW_POOL_BYTES - seu_shadow_used) < len) {
            return -1;                   /* shadow pool exhausted */
        }
        slot->shadow = &seu_shadow_pool[seu_shadow_used];
        seu_shadow_used += len;
    }

    slot->used = 1U;
    seu_region_used++;

    /* Take the initial snapshot straight away, so a region is protected from
       the moment it is registered. */
    seu_mitigation_lock();
    if (slot->shadow != NULL) {
        memcpy(slot->shadow, slot->addr, slot->len);
        slot->crc = seu_crc32(slot->addr, slot->len);
    }
    seu_mitigation_unlock();

    return 0;
}

int seu_mitigation_commit(seu_region_id_t id)
{
    seu_region_t *r;

    if (seu_initialised == 0U) {
        return -1;   /* owner initialised before the scrubber: nothing to do */
    }

    r = seu_find(id);
    if ((r == NULL) || (seu_region_valid(r) == 0) || (r->shadow == NULL)) {
        return -1;
    }

    seu_mitigation_lock();
    memcpy(r->shadow, r->addr, r->len);
    r->crc = seu_crc32(r->addr, r->len);
    seu_mitigation_unlock();

    return 0;
}

/* ---------- Scrubbing ---------- */

/* Read every word of a region so the SRAM2 parity hardware gets the chance to
   flag a dormant upset now rather than during a pass. The reads must survive
   the optimiser, hence the volatile accesses and the sink. */
static void seu_touch(const seu_region_t *r)
{
    const volatile uint8_t *p = (const volatile uint8_t *)r->addr;
    uint32_t acc = 0U;

    for (size_t i = 0U; i < r->len; i++) {
        acc += (uint32_t)p[i];
    }
    seu_touch_sink = acc;
}

/* Vote between the live object, the shadow copy and the reference CRC. */
static seu_scrub_result_t seu_scrub_golden(seu_region_t *r)
{
    uint32_t crc_live;
    uint32_t crc_shadow;
    uint32_t crc_ref;
    uint32_t byte_errors = 0U;
    uint32_t bit_errors  = 0U;
    uint32_t first_bad   = 0xFFFFFFFFU;
    seu_scrub_result_t result;

    /* Bounds check before the first dereference (review C2): a corrupt
       descriptor must not be able to drive a memcpy past seu_work /
       seu_shadow_work, which are exactly SEU_MAX_REGION_BYTES long. */
    if (seu_region_valid(r) == 0) {
        seu_region_reject(r);
        return SEU_RESULT_UNRECOVERED;
    }

    /* Take all three legs of the vote in one locked instant, then do the
       arithmetic with interrupts enabled - the lock must not be held for a
       CRC, and a commit landing between two of the reads would otherwise look
       exactly like a double corruption. */
    seu_mitigation_lock();
    memcpy(seu_work, r->addr, r->len);
    memcpy(seu_shadow_work, r->shadow, r->len);
    crc_ref = r->crc;
    seu_mitigation_unlock();

    crc_live   = seu_crc32(seu_work, r->len);
    crc_shadow = seu_crc32(seu_shadow_work, r->len);

    if (crc_live == crc_ref) {
        if (crc_shadow == crc_ref) {
            return SEU_RESULT_HEALTHY;
        }

        /* The live copy is the reference and it is intact: the upset hit the
           shadow. Measure it, then rebuild - no mission data was at risk. */
        byte_errors = seu_diff(seu_work, seu_shadow_work, r->len,
                               &bit_errors, &first_bad);
        seu_mitigation_lock();
        memcpy(r->shadow, r->addr, r->len);
        r->crc = seu_crc32(r->addr, r->len);
        seu_mitigation_unlock();

        r->mismatches++;
        seu_stats.mismatches++;
        seu_stats.shadow_repairs++;
        seu_stats.bit_errors += bit_errors;
        seu_log(SEU_EVENT_SHADOW_REPAIR, r, crc_live, crc_shadow,
                byte_errors, bit_errors, first_bad);
        return SEU_RESULT_REPAIRED;
    }

    byte_errors = seu_diff(seu_work, seu_shadow_work, r->len,
                           &bit_errors, &first_bad);

    if (crc_shadow == crc_ref) {
        /* Two witnesses (shadow + reference CRC) against one: the live object
           is the corrupt copy. Re-check under the lock that it has not been
           changed legitimately in the meantime - an owner that updated the
           object between the snapshot and now must not be "repaired" over. */
        int repaired = 0;

        seu_mitigation_lock();
        if (memcmp(seu_work, r->addr, r->len) == 0) {
            memcpy(r->addr, seu_shadow_work, r->len);
            repaired = 1;
        }
        seu_mitigation_unlock();

        if (repaired == 0) {
            /* Concurrent legitimate update, not an upset. The next pass sees
               the committed content. */
            return SEU_RESULT_HEALTHY;
        }

        r->mismatches++;
        seu_stats.mismatches++;
        seu_stats.repairs++;
        seu_stats.bit_errors += bit_errors;
        seu_log(SEU_EVENT_SCRUB_REPAIR, r, crc_live, crc_shadow,
                byte_errors, bit_errors, first_bad);
        return SEU_RESULT_REPAIRED;
    }

    if (crc_live == crc_shadow) {
        /* Both data copies agree, so the stored reference CRC is what flipped.
           Recompute it, but only if the live object still matches what was
           voted on. */
        int refreshed = 0;

        seu_mitigation_lock();
        if ((memcmp(seu_work, r->addr, r->len) == 0) && (r->crc == crc_ref)) {
            r->crc = crc_live;
            refreshed = 1;
        }
        seu_mitigation_unlock();

        if (refreshed == 0) {
            return SEU_RESULT_HEALTHY;   /* commit raced us; nothing to do */
        }

        r->mismatches++;
        seu_stats.mismatches++;
        seu_stats.crc_refreshes++;
        seu_log(SEU_EVENT_SCRUB_CRC, r, crc_live, crc_shadow,
                byte_errors, bit_errors, first_bad);
        return SEU_RESULT_CRC_REFRESH;
    }

    /* No two copies agree. The Flash load image is NOT a trustworthy source
       here: it holds the compile-time defaults (state OFF, soc 100 %), so
       writing it back would replace an unknown value with a *wrong* value and
       report a dead battery as a full one (review C1). Leave the live object
       untouched, record the loss, and contain the spacecraft in the
       beacon-only safe state. */
    seu_mitigation_lock();
    if (memcmp(seu_work, r->addr, r->len) != 0) {
        seu_mitigation_unlock();
        return SEU_RESULT_HEALTHY;       /* commit raced us; re-examine later */
    }
    seu_mitigation_unlock();

    result = SEU_RESULT_UNRECOVERED;

    r->mismatches++;
    seu_stats.mismatches++;
    seu_stats.unrecoverable++;
    seu_stats.bit_errors += bit_errors;
    seu_log(SEU_EVENT_SCRUB_FAILED, r, crc_live, crc_shadow,
            byte_errors, bit_errors, first_bad);

    seu_enter_safe_state(r);
    return result;
}

int seu_mitigation_scrub_once(void)
{
    uint32_t corrupted = 0U;

    if (seu_initialised == 0U) {
        return -1;
    }

    for (uint32_t i = 0U; i < SEU_MAX_REGIONS; i++) {
        seu_region_t *r = &seu_regions[i];

        if (r->used == 0U) {
            continue;
        }

        seu_stats.regions_checked++;

        /* Bounds check before any dereference (review C2) — applies to the
           touch policy too: seu_touch() walks r->len bytes from r->addr. */
        if (seu_region_valid(r) == 0) {
            seu_region_reject(r);
            corrupted++;
            continue;
        }

        if ((seu_policy_t)r->policy == SEU_POLICY_TOUCH) {
            seu_touch(r);
            continue;
        }

        if (seu_scrub_golden(r) != SEU_RESULT_HEALTHY) {
            corrupted++;
        }
    }

    seu_stats.scrub_cycles++;
    seu_stats.last_scrub_tick    = HAL_GetTick();
    seu_stats.parity_events_boot = sram2_parity_error_count();
    seu_stats.parity_events_total = seu_bkp_count();
    seu_stats.parity_status      = (uint32_t)sram2_parity_get_status();

    return (int)corrupted;
}

/* ---------- Init ---------- */

void seu_mitigation_init(void)
{
    size_t len = 0U;
    void  *addr;
    uint32_t persistent;
    uint32_t acked;

    memset(seu_regions, 0, sizeof(seu_regions));
    memset(&seu_stats, 0, sizeof(seu_stats));
    seu_region_used = 0U;
    seu_shadow_used = 0U;

    seu_bkp_init();

    /* (a) Snapshot the critical structures. The owners are already
           initialised at this point, so the shadows capture the intended
           post-boot content, not whatever the linker left behind.
           A failed registration is a real loss of protection, so it is
           counted and never silently dropped (review M5). */
    addr = state_machine_critical_region(&len);
    if (seu_mitigation_register_region(SEU_REGION_OBSW_STATE, addr, len,
                                       SEU_POLICY_GOLDEN) != 0) {
        seu_stats.registration_failures++;
    }

    addr = laststates_mirror_region(&len);
    if (seu_mitigation_register_region(SEU_REGION_LASTSTATES, addr, len,
                                       SEU_POLICY_GOLDEN) != 0) {
        seu_stats.registration_failures++;
    }

    /* Comms frames change with every beacon and every uplink, so there is no
       golden content to vote on; reading them is still worth it because that
       is what makes the parity hardware report a dormant upset. */
    addr = comms_beacon_buffer(&len);
    if (seu_mitigation_register_region(SEU_REGION_COMMS_BEACON, addr, len,
                                       SEU_POLICY_TOUCH) != 0) {
        seu_stats.registration_failures++;
    }
    addr = comms_rx_buffer(&len);
    if (seu_mitigation_register_region(SEU_REGION_COMMS_RX, addr, len,
                                       SEU_POLICY_TOUCH) != 0) {
        seu_stats.registration_failures++;
    }
    addr = comms_tx_buffer(&len);
    if (seu_mitigation_register_region(SEU_REGION_COMMS_TX, addr, len,
                                       SEU_POLICY_TOUCH) != 0) {
        seu_stats.registration_failures++;
    }

    seu_stats.parity_events_boot  = sram2_parity_error_count();
    seu_stats.parity_events_total = seu_bkp_count();
    seu_stats.parity_status       = (uint32_t)sram2_parity_get_status();

    seu_initialised = 1U;

    if (seu_stats.registration_failures != 0U) {
        seu_log(SEU_EVENT_REGISTER_FAILED, NULL, 0U, 0U, 0U, 0U, 0xFFFFFFFFU);
    }

    /* (c) Parity NMIs reset the OBSW, so their count lives in the backup
           domain. If it moved since the last boot that was acknowledged, this
           boot is the consequence of one or more SEUs: record the history in
           the LastStates pool, then acknowledge it so the entry is written
           once per event and not once per boot. */
    persistent = seu_bkp_count();
    acked      = seu_bkp_acked();
    if (persistent != acked) {
        seu_log(SEU_EVENT_PARITY_HISTORY, NULL, 0U, 0U,
                0U, 0U, 0xFFFFFFFFU);
        seu_bkp_ack(persistent);
    }
}

/* ---------- Scrub task ---------- */

static void seu_scrub_task(void *arg)
{
    (void)arg;

    for (;;) {
        osDelay(pdMS_TO_TICKS(SEU_SCRUB_INTERVAL_MS));
        (void)seu_mitigation_scrub_once();
    }
}

osThreadId_t seu_scrub_task_create(void)
{
    return osThreadNew(seu_scrub_task, NULL, &seu_task_attrs);
}

/* ---------- Telemetry / NMI ---------- */

void seu_mitigation_get_stats(seu_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    seu_mitigation_lock();
    *out = seu_stats;
    seu_mitigation_unlock();
    out->parity_events_boot  = sram2_parity_error_count();
    out->parity_events_total = seu_bkp_count();
}

uint32_t seu_mitigation_event_count(void)
{
    return seu_stats.mismatches + seu_nmi_events;
}

void seu_mitigation_nmi_hook(void)
{
    /* Runs inside the NMI, ahead of sram2_parity_nmi_handler(), which records
       the fault context and resets. Only two stores: an RTC backup register
       (survives that reset) and a RAM counter for a debugger. No HAL calls,
       no Flash, no locking - the handler must stay re-entrancy free. */
    seu_nmi_events++;

    /* The clock-security system vectors here too, so only a set SRAM2 parity
       flag counts as an upset. The flag is cleared by the parity handler that
       runs immediately after this hook. */
    if (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_PE) != 0U) {
        seu_bkp_increment();
    }
}
