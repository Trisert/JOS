#include "dual_bank.h"

#include "boot_crc.h"
#include "obsw_types.h"
#include "stm32l4xx_hal.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * W2-2 — Dual-bank golden-image fallback.
 * See Core/Inc/dual_bank.h for the design, the safety gates G1..G5 and the
 * current LastStates/bank-2 layout limitation.
 *
 * NASA-STD-8739.8 (graceful degradation), ECSS-Q-ST-80C §6.2.6 (fault
 * tolerance and recovery).
 * ------------------------------------------------------------------------- */

_Static_assert(sizeof(laststates_entry_t) == LASTSTATES_ENTRY_SIZE,
               "LastStates entry must be exactly one 128-byte pool slot");
_Static_assert((LASTSTATES_ENTRY_SIZE % 8U) == 0U,
               "LastStates entry must be a whole number of Flash double-words");

/* SYSCFG_MEMRMP.FB_MODE is the authoritative runtime answer to "which physical
 * bank am I executing from": the boot ROM sets it from BFB2 and the bank pair
 * is swapped in the address map accordingly (RM0351 §3.3.1, §11.2.1). */
#ifndef SYSCFG_MEMRMP_FB_MODE
#define SYSCFG_MEMRMP_FB_MODE  (0x1UL << 8U)
#endif

/* ---------------------------------------------------------------------------
 * Warm-reset scratch.
 *
 * Lives in SRAM1 through the .boot_fault (NOLOAD) linker section, outside
 * [_sbss,_ebss): the startup code neither loads nor zeroes it, and SRAM keeps
 * its contents across a system reset. It is deliberately NOT in SRAM2, whose
 * parity initialisation (W2-3) rewrites the whole block at boot. If the
 * content is lost anyway (power cycle) the magic check fails and we fall back
 * to the Flash-persisted count — degraded, never wrong.
 * ------------------------------------------------------------------------- */
#define DUAL_BANK_SCRATCH_MAGIC  0xB007FA17U

typedef struct {
    uint32_t magic;
    uint32_t fault_count;   /* faults recorded since the last good boot */
    uint32_t pending;       /* faults not yet written to LastStates     */
} dual_bank_scratch_t;

static volatile dual_bank_scratch_t db_scratch
    __attribute__((section(".boot_fault"), used));

static void scratch_init_if_needed(void)
{
    if (db_scratch.magic != DUAL_BANK_SCRATCH_MAGIC) {
        db_scratch.magic       = DUAL_BANK_SCRATCH_MAGIC;
        db_scratch.fault_count = 0U;
        db_scratch.pending     = 0U;
    }
}

/* ---------- Latched state (telemetry) ---------- */
static dual_bank_status_t db_status       = DUAL_BANK_PRIMARY_OK;
static uint32_t           db_active_bank  = 1U;
static uint32_t           db_fault_count  = 0U;
static bool               db_golden_valid = false;
static uint32_t           db_optr         = 0U;

/* ===========================================================================
 * Option bytes
 * ========================================================================= */

static uint32_t read_user_option_bytes(void)
{
    FLASH_OBProgramInitTypeDef ob;

    memset(&ob, 0, sizeof(ob));
    /* Values that match no WRP area / PCROP bank, so HAL_FLASHEx_OBGetConfig()
     * only reads RDP + USER and leaves the rest alone. */
    ob.WRPArea     = 0xFFFFFFFFU;
    ob.PCROPConfig = 0xFFFFFFFFU;

    HAL_FLASHEx_OBGetConfig(&ob);

    return ob.USERConfig;   /* FLASH->OPTR with the RDP field cleared */
}

/* G2: is the part really organised as two 512 KB banks?
 * On 1 MB STM32L4 parts dual bank is permanent and the DUALBANK option bit
 * only configures the 512/256 KB parts (RM0351 §3.7.8), so trust the reported
 * Flash size first and only consult the option bit on smaller devices. */
static bool geometry_is_dual_bank(uint32_t optr)
{
    const uint32_t flash_size = (uint32_t)FLASH_SIZE;

    if (flash_size != (2U * DUAL_BANK_BANK_SIZE)) {
        return false;               /* not the 1 MB part this layout assumes */
    }
    if (flash_size >= (1024U * 1024U)) {
        return true;
    }
#if defined(FLASH_OPTR_DUALBANK)
    return (optr & FLASH_OPTR_DUALBANK) != 0U;
#else
    (void)optr;
    return false;
#endif
}

uint32_t dual_bank_active_bank(void)
{
    return ((SYSCFG->MEMRMP & SYSCFG_MEMRMP_FB_MODE) != 0U) ? 2U : 1U;
}

/* ===========================================================================
 * Golden image validation (read-only — cannot brick anything)
 * ========================================================================= */

static bool address_is_sram(uint32_t addr)
{
    /* SRAM1 256 KB @ 0x20000000, SRAM2 64 KB @ 0x10000000 (alias 0x20040000).
     * The initial MSP points at the *end* of a RAM block, hence the inclusive
     * upper bounds. */
    if ((addr > 0x20000000U) && (addr <= 0x20050000U)) return true;
    if ((addr > 0x10000000U) && (addr <= 0x10010000U)) return true;
    return false;
}

/* G4: vector-table plausibility.
 * The golden image is linked for the bank-1 view (0x08000000): after the BFB2
 * swap its bank is mapped there. So the stored reset vector must point into
 * [DUAL_BANK_FLASH_BASE, +bank size), not into the physical window we are
 * reading it through. */
static bool golden_vector_table_sane(uint32_t base)
{
    const uint32_t msp = *(const volatile uint32_t *)(base);
    const uint32_t pc  = *(const volatile uint32_t *)(base + 4U);

    if (!address_is_sram(msp)) {
        return false;
    }
    if ((pc & 1U) == 0U) {                       /* Thumb bit mandatory */
        return false;
    }
    if ((pc < DUAL_BANK_FLASH_BASE) ||
        (pc >= (DUAL_BANK_FLASH_BASE + DUAL_BANK_BANK_SIZE))) {
        return false;
    }
    return true;
}

bool dual_bank_verify_golden(void)
{
    const dual_bank_golden_trailer_t *tr =
        (const dual_bank_golden_trailer_t *)DUAL_BANK_TRAILER_ADDR;

    db_golden_valid = false;

    if (!DUAL_BANK_GOLDEN_SLOT_AVAILABLE) {
        return false;                                            /* G1 */
    }
    if (tr->magic != DUAL_BANK_GOLDEN_MAGIC) {
        return false;                                            /* G3 */
    }
    if (tr->crc32_inv != ~tr->crc32) {
        return false;                       /* descriptor itself corrupt */
    }
    if ((tr->length < DUAL_BANK_GOLDEN_MIN_SIZE) ||
        (tr->length > (DUAL_BANK_BANK_SIZE - sizeof(*tr)))) {
        return false;
    }
    if (!golden_vector_table_sane(DUAL_BANK_GOLDEN_BASE)) {
        return false;                                            /* G4 */
    }
    if (boot_crc32((const void *)DUAL_BANK_GOLDEN_BASE, (size_t)tr->length)
            != tr->crc32) {
        return false;
    }

    db_golden_valid = true;
    return true;
}

/* ===========================================================================
 * Boot-fault counter — RAM scratch + LastStates persistence
 *
 * The counter is persisted as ordinary LastStates entries (trigger
 * TRIGGER_BOOT_FAULT / TRIGGER_BOOT_OK), so existing ground forensics tooling
 * sees them in the normal 64 x 128 B ring. This module only ever programs
 * slots that are still erased and never erases the pool, so it cannot destroy
 * transition history written by App/memory/memory.c.
 * ========================================================================= */

#define DB_LS_TAG  0x4B4E4244U   /* 'D','B','N','K' (LE) — marks our entries */

static const laststates_entry_t *ls_slot(uint32_t i)
{
    return (const laststates_entry_t *)
           (DUAL_BANK_LASTSTATES_BASE + (i * LASTSTATES_ENTRY_SIZE));
}

static bool ls_slot_is_erased(const laststates_entry_t *e)
{
    const uint32_t *w = (const uint32_t *)e;
    for (uint32_t i = 0U; i < (LASTSTATES_ENTRY_SIZE / 4U); i++) {
        if (w[i] != 0xFFFFFFFFU) {
            return false;
        }
    }
    return true;
}

static bool ls_slot_is_ours(const laststates_entry_t *e)
{
    uint32_t tag;
    memcpy(&tag, e->context, sizeof(tag));
    return tag == DB_LS_TAG;
}

/* Boot faults recorded after the most recent successful boot. */
static uint32_t ls_count_boot_faults(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        const laststates_entry_t *e = ls_slot(i);

        if (ls_slot_is_erased(e)) {
            break;                    /* pool is written in order */
        }
        if (!ls_slot_is_ours(e)) {
            continue;                 /* a normal state transition */
        }
        if (e->trigger == TRIGGER_BOOT_OK) {
            count = 0U;
        } else if (e->trigger == TRIGGER_BOOT_FAULT) {
            count++;
        }
    }
    return count;
}

/* Append one entry to the first still-erased slot. Returns 0 on success,
 * -1 if the pool is full (we deliberately do not erase: forensic history is
 * worth more than the counter, and the RAM scratch still carries it). */
static int ls_append(uint8_t trigger, uint32_t value)
{
    laststates_entry_t entry;
    uint32_t slot = LASTSTATES_MAX_ENTRIES;

    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (ls_slot_is_erased(ls_slot(i))) {
            slot = i;
            break;
        }
    }
    if (slot >= LASTSTATES_MAX_ENTRIES) {
        return -1;
    }

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = HAL_GetTick();
    entry.state_from = (uint8_t)dual_bank_active_bank();
    entry.state_to   = (uint8_t)((value > 0xFFU) ? 0xFFU : value);
    entry.trigger    = trigger;
    {
        const uint32_t tag = DB_LS_TAG;
        memcpy(entry.context, &tag, sizeof(tag));
        memcpy(entry.context + 4, &value, sizeof(value));
    }

    const uint32_t addr = DUAL_BANK_LASTSTATES_BASE + (slot * LASTSTATES_ENTRY_SIZE);
    HAL_StatusTypeDef rc = HAL_OK;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    for (uint32_t off = 0U; off < LASTSTATES_ENTRY_SIZE; off += 8U) {
        uint64_t dword;
        memcpy(&dword, ((const uint8_t *)&entry) + off, sizeof(dword));
        rc = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + off, dword);
        if (rc != HAL_OK) {
            break;
        }
    }
    (void)HAL_FLASH_Lock();

    return (rc == HAL_OK) ? 0 : -1;
}

void dual_bank_mark_boot_fault(void)
{
    /* ISR context (HardFault/NMI): no HAL, no Flash, no blocking. */
    scratch_init_if_needed();
    if (db_scratch.fault_count < 0xFFFFU) {
        db_scratch.fault_count++;
    }
    db_scratch.pending = 1U;
    __DSB();
}

uint32_t dual_bank_boot_fault_count(void)
{
    return db_fault_count;
}

void dual_bank_boot_complete(void)
{
    scratch_init_if_needed();

    /* Only touch Flash when there is evidence to clear — a nominal boot must
     * not consume a pool slot on every power cycle. */
    if (db_fault_count > 0U) {
        (void)ls_append(TRIGGER_BOOT_OK, db_fault_count);
    }
    db_scratch.fault_count = 0U;
    db_scratch.pending     = 0U;
    db_fault_count         = 0U;
}

/* ===========================================================================
 * Bank switch
 * ========================================================================= */

int dual_bank_switch_to_golden(void)
{
    FLASH_OBProgramInitTypeDef ob;

    if (!DUAL_BANK_GOLDEN_SLOT_AVAILABLE) {           /* G1 */
        db_status = DUAL_BANK_INHIBITED;
        return DUAL_BANK_SWITCH_INHIBITED;
    }
    if (dual_bank_active_bank() != 1U) {              /* G5 — no ping-pong */
        db_status = DUAL_BANK_ON_GOLDEN;
        return DUAL_BANK_SWITCH_ALREADY;
    }
    if (!geometry_is_dual_bank(read_user_option_bytes())) {   /* G2 */
        db_status = DUAL_BANK_NOT_DUAL;
        return DUAL_BANK_SWITCH_NOT_DUAL;
    }
    if (!dual_bank_verify_golden()) {                 /* G3 + G4 */
        db_status = DUAL_BANK_DEGRADED;
        return DUAL_BANK_SWITCH_NO_GOLDEN;
    }

    memset(&ob, 0, sizeof(ob));
    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType   = OB_USER_BFB2;
    ob.USERConfig = OB_BFB2_ENABLE;

    if ((HAL_FLASH_Unlock() != HAL_OK) || (HAL_FLASH_OB_Unlock() != HAL_OK)) {
        (void)HAL_FLASH_Lock();
        db_status = DUAL_BANK_ERROR;
        return DUAL_BANK_SWITCH_OB_FAILED;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    /* HAL preserves every user option bit it was not asked to change, so RDP,
     * watchdog and BOR settings survive untouched. */
    if (HAL_FLASHEx_OBProgram(&ob) != HAL_OK) {
        (void)HAL_FLASH_OB_Lock();
        (void)HAL_FLASH_Lock();
        db_status = DUAL_BANK_ERROR;
        return DUAL_BANK_SWITCH_OB_FAILED;
    }

    db_status = DUAL_BANK_FALLBACK_ARMED;

    /* OBL_LAUNCH reloads the option bytes and resets the device; the calls
     * below are only reached if that somehow does not happen. */
    (void)HAL_FLASH_OB_Launch();
    (void)HAL_FLASH_OB_Lock();
    (void)HAL_FLASH_Lock();
    __DSB();
    NVIC_SystemReset();

    return 0;   /* unreachable */
}

/* ===========================================================================
 * Boot-time entry point
 * ========================================================================= */

dual_bank_status_t dual_bank_init(void)
{
    scratch_init_if_needed();

    db_optr        = read_user_option_bytes();
    db_active_bank = dual_bank_active_bank();

    /* Fold the evidence: what previous boots persisted, plus anything the
     * fault handlers recorded in RAM since the last successful boot. */
    const uint32_t persisted = ls_count_boot_faults();
    const uint32_t in_ram    = db_scratch.fault_count;

    if (db_scratch.pending != 0U) {
        /* Write the RAM evidence through to Flash now that we are in thread
         * mode with the HAL available. One entry per faulting boot. */
        if (ls_append(TRIGGER_BOOT_FAULT, persisted + 1U) == 0) {
            db_fault_count     = persisted + 1U;
            db_scratch.pending = 0U;
        } else {
            db_fault_count = (persisted > in_ram) ? persisted : in_ram;
        }
    } else {
        db_fault_count = (persisted > in_ram) ? persisted : in_ram;
    }

    if (db_active_bank == 2U) {
        /* Already running the golden image: report and stay. Recovery of the
         * primary image is a ground operation (re-upload + clear BFB2). */
        db_status = DUAL_BANK_ON_GOLDEN;
        return db_status;
    }

    if (!geometry_is_dual_bank(db_optr)) {
        db_status = DUAL_BANK_NOT_DUAL;
        return db_status;
    }

    const bool image_corrupt =
        (boot_crc_get_status() == BOOT_CRC_MISMATCH);
    const bool boot_looping =
        (db_fault_count >= DUAL_BANK_BOOT_FAULT_THRESHOLD);

    if (!image_corrupt && !boot_looping) {
        db_status = DUAL_BANK_PRIMARY_OK;
        (void)dual_bank_verify_golden();   /* keep the health flag current */
        return db_status;
    }

    /* Fallback wanted. dual_bank_switch_to_golden() runs the safety gates and
     * either resets into the golden image or refuses; refusing leaves us on
     * the primary image in a reported degraded state (graceful degradation,
     * NASA-STD-8739.8) rather than in an unbootable one. */
    const int rc = dual_bank_switch_to_golden();

    if (rc == DUAL_BANK_SWITCH_INHIBITED) {
        db_status = DUAL_BANK_INHIBITED;
    } else if (rc == DUAL_BANK_SWITCH_NOT_DUAL) {
        db_status = DUAL_BANK_NOT_DUAL;
    } else if (rc == DUAL_BANK_SWITCH_OB_FAILED) {
        db_status = DUAL_BANK_ERROR;
    } else {
        db_status = DUAL_BANK_DEGRADED;
    }
    return db_status;
}

/* ---------- Accessors ---------- */
dual_bank_status_t dual_bank_get_status(void)      { return db_status; }
bool               dual_bank_golden_valid(void)    { return db_golden_valid; }
uint32_t           dual_bank_get_optr_snapshot(void) { return db_optr; }
