#include "dual_bank.h"

#include "boot_crc.h"
#include "memory.h"      /* laststates_pool_lock/unlock — shared pool (W2-2) */
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
_Static_assert((LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)
               <= DUAL_BANK_LASTSTATES_SIZE,
               "LastStates ring does not fit in the reserved pool");

/* G1 belt-and-braces: even a hand-tuned DUAL_BANK_GOLDEN_MAX_SIZE must never
 * leave the golden trailer page shared with the forensic pool — writing the
 * pool would then destroy the descriptor the switch is validated against. */
#if DUAL_BANK_GOLDEN_SLOT_AVAILABLE
#if ((DUAL_BANK_TRAILER_PAGE_BASE) < (DUAL_BANK_LASTSTATES_END)) && \
    (((DUAL_BANK_TRAILER_PAGE_BASE) + (DUAL_BANK_PAGE_SIZE)) > (DUAL_BANK_LASTSTATES_BASE))
#error "G1: LastStates pool shares the golden trailer page — relocate it outside bank 2"
#endif
#endif

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
 * its contents across a system reset — including the NVIC_SystemReset() the
 * fault handler issues. It is deliberately NOT in SRAM2, whose parity
 * initialisation (W2-3) rewrites the whole block at boot. If the content is
 * lost anyway (power cycle) the magic check fails and we fall back to the
 * Flash-persisted count — degraded, never wrong.
 * ------------------------------------------------------------------------- */
#define DUAL_BANK_SCRATCH_MAGIC  0xB007FA17U

typedef struct {
    uint32_t magic;
    uint32_t fault_count;   /* faults recorded since the last good boot */
    uint32_t pending;       /* faults not yet written to LastStates     */
    uint32_t ok_pending;    /* boot-OK marker not yet written           */
} dual_bank_scratch_t;

static volatile dual_bank_scratch_t db_scratch
    __attribute__((section(".boot_fault"), used));

static void scratch_init_if_needed(void)
{
    if (db_scratch.magic != DUAL_BANK_SCRATCH_MAGIC) {
        db_scratch.magic       = DUAL_BANK_SCRATCH_MAGIC;
        db_scratch.fault_count = 0U;
        db_scratch.pending     = 0U;
        db_scratch.ok_pending  = 0U;
    }
}

/* ---------- Latched state (telemetry) ---------- */
static dual_bank_status_t db_status       = DUAL_BANK_PRIMARY_OK;
static uint32_t           db_active_bank  = 1U;
static uint32_t           db_fault_count  = 0U;
static bool               db_golden_valid = false;
static uint32_t           db_optr         = 0U;
static bool               db_bfb2_armed   = false;

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

/* Is the boot-from-bank-2 option bit programmed? Used two ways:
 *   - G5, so we never reprogram an option byte that is already armed (each
 *     OBProgram costs an erase/program cycle of the option area and an
 *     OBL_LAUNCH reset — repeating it would be a reset loop);
 *   - to detect the "armed but still executing bank 1" state, i.e. the switch
 *     was programmed and the swap has not taken effect yet. */
static bool option_bfb2_enabled(uint32_t optr)
{
#if defined(FLASH_OPTR_BFB2)
    return (optr & FLASH_OPTR_BFB2) != 0U;
#else
    (void)optr;
    return false;
#endif
}

/* G2: is the part really organised as two 512 KB banks?
 * Two independent conditions, both required (RM0351 §3.7.8):
 *   - the device reports 1 MB of Flash, i.e. two banks of DUAL_BANK_BANK_SIZE;
 *   - the DUALBANK option bit is set. On the 1 MB STM32L496 this bit is
 *     factory-programmed to 1, but it is writable, and a device shipped or
 *     re-provisioned with it cleared has 4 KB pages and a single 1 MB bank —
 *     BFB2 would then mean nothing and the switch must not happen. Checking
 *     the reported size alone (the previous behaviour) silently trusted a bit
 *     this gate claims to verify. */
static bool geometry_is_dual_bank(uint32_t optr)
{
    const uint32_t flash_size = (uint32_t)FLASH_SIZE;

    if (flash_size != (2U * DUAL_BANK_BANK_SIZE)) {
        return false;               /* not the 1 MB part this layout assumes */
    }
#if defined(FLASH_OPTR_DUALBANK)
    return (optr & FLASH_OPTR_DUALBANK) != 0U;
#elif defined(FLASH_OPTR_DBANK)
    return (optr & FLASH_OPTR_DBANK) != 0U;
#else
    (void)optr;
    return false;                   /* cannot prove it — refuse to switch */
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
        (tr->length > (DUAL_BANK_GOLDEN_MAX_SIZE - sizeof(*tr)))) {
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
 * sees them in the normal 64 x 128 B ring.
 *
 * Coexistence contract with App/memory/memory.c (the other writer):
 *   - both writers append to the first still-erased slot; memory.c re-scans
 *     before every write (laststates_write()), so an entry appended here
 *     between two of its writes shifts its cursor instead of colliding with
 *     it — programming a non-erased slot would fail and, before that fix,
 *     wedged the forensic log permanently;
 *   - this module NEVER erases the pool. memory.c owns the erase (only when
 *     the ring wraps), which also clears our evidence. That is fail-safe: a
 *     lost counter can only inhibit a fallback, never trigger one.
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

/* Index of the first erased slot, or LASTSTATES_MAX_ENTRIES if the pool is
 * full (no room left for new evidence, in either direction). */
static uint32_t ls_first_free_slot(void)
{
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (ls_slot_is_erased(ls_slot(i))) {
            return i;
        }
    }
    return LASTSTATES_MAX_ENTRIES;
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
 * worth more than the counter, and the RAM scratch still carries it).
 *
 * The pool is SHARED with App/memory/memory.c:laststates_write(), which runs
 * the same select-slot/unlock/program/lock sequence from stateMachine and
 * loraRX while we run from the watchdog monitor task at osPriorityHigh. The
 * whole sequence therefore runs under the pool mutex, and the chosen slot is
 * re-checked for "still erased" while holding it (W2-2 review, CRITICAL).
 *
 * `entry` is file-scope static on purpose: it is 128 bytes and the watchdog
 * task only has a 1 KB stack, with the HAL_FLASH_Program() frames and any
 * exception frame stacked on top of it. The pool lock serialises every caller,
 * and no caller is an ISR, so a shared buffer is safe here. */
static laststates_entry_t db_ls_entry;

static int ls_append(uint8_t trigger, uint32_t value)
{
    const int lock_held = laststates_pool_lock();

    /* Serialisation required but unavailable: refuse rather than program an
     * unsynchronised pool (Kilo #21). The caller treats this exactly like a
     * Flash failure. Count the loss here: this writer drives
     * HAL_FLASH_Program() itself and never calls laststates_write(), so
     * without this call a refused boot-fault / boot-OK marker would be
     * invisible in laststates_dropped_records() and ground would have to infer
     * it from a gap - the exact hole the tri-state lock exists to close
     * (Kilo #26). */
    if (lock_held == LASTSTATES_LOCK_FAILED) {
        laststates_note_dropped_record();
        return -1;
    }

    const uint32_t slot = ls_first_free_slot();

    if (slot >= LASTSTATES_MAX_ENTRIES) {
        laststates_pool_unlock(lock_held);
        return -1;
    }
    /* Defensive re-validation, not the mutual-exclusion guarantee: that comes
     * from holding the pool mutex across BOTH the scan above and the whole
     * programming loop below, so when the lock is real there is no window left
     * between them. This check only earns its keep on the paths where the lock
     * is a no-op by design (boot before the scheduler, exception context) and
     * against a scan result corrupted after the fact. */
    if (!ls_slot_is_erased(ls_slot(slot))) {
        laststates_pool_unlock(lock_held);
        return -1;
    }

    laststates_entry_t *const entry = &db_ls_entry;

    memset(entry, 0, sizeof(*entry));
    entry->timestamp  = HAL_GetTick();
    /* Sentinels: a ground decoder must not render this record as a state
     * transition. The bank number and the fault count live in context[]. */
    entry->state_from = 0xFFU;
    entry->state_to   = 0xFFU;
    entry->trigger    = trigger;
    {
        const uint32_t tag  = DB_LS_TAG;
        const uint32_t bank = dual_bank_active_bank();
        memcpy(entry->context, &tag, sizeof(tag));
        memcpy(entry->context + 4, &value, sizeof(value));
        memcpy(entry->context + 8, &bank, sizeof(bank));
    }

    const uint32_t addr = DUAL_BANK_LASTSTATES_BASE + (slot * LASTSTATES_ENTRY_SIZE);
    HAL_StatusTypeDef rc = HAL_OK;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        laststates_pool_unlock(lock_held);
        return -1;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    for (uint32_t off = 0U; off < LASTSTATES_ENTRY_SIZE; off += 8U) {
        uint64_t dword;
        memcpy(&dword, ((const uint8_t *)entry) + off, sizeof(dword));
        rc = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + off, dword);
        if (rc != HAL_OK) {
            break;
        }
    }
    (void)HAL_FLASH_Lock();
    laststates_pool_unlock(lock_held);

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

void dual_bank_handle_boot_fault(void)
{
    dual_bank_mark_boot_fault();

#if defined(DUAL_BANK_FAULT_NO_RESET)
    /* Bench/debug build: keep the CPU in the faulting state for the debugger.
     * The fallback cannot make progress in this configuration. */
    for (;;) {
        __NOP();
    }
#else
    /* Flight behaviour. There is no IWDG in this build, so a fault handler
     * that spins is a permanently silent spacecraft: nothing would ever reset
     * us, dual_bank_init() would never run again, the evidence would never be
     * persisted and the threshold could never be reached. Reset ourselves
     * instead — the RAM scratch survives NVIC_SystemReset(), the next boot
     * writes it to LastStates, and DUAL_BANK_BOOT_FAULT_THRESHOLD failed boots
     * arm the golden-image fallback. A reset loop is recoverable (and visible
     * from ground through the LastStates log); a frozen OBSW is not. */
    __DSB();
    __ISB();
    NVIC_SystemReset();

    for (;;) {          /* NVIC_SystemReset() does not return */
        __NOP();
    }
#endif
}

uint32_t dual_bank_boot_fault_count(void)
{
    return db_fault_count;
}

bool dual_bank_boot_ok_pending(void)
{
    return (db_scratch.ok_pending != 0U);
}

int dual_bank_boot_complete(void)
{
    scratch_init_if_needed();

    /* Only touch Flash when there is evidence to clear — a nominal boot must
     * not consume a pool slot on every power cycle. */
    if ((db_fault_count == 0U) && (db_scratch.fault_count == 0U)) {
        db_scratch.pending    = 0U;
        db_scratch.ok_pending = 0U;
        return 0;
    }

    if (ls_append(TRIGGER_BOOT_OK, db_fault_count) != 0) {
        /* Pool exhausted: the boot-OK marker is NOT recorded, so the persisted
         * fault evidence still reads "this image keeps failing". Keep the
         * counters as they are, flag the outstanding write and report the
         * failure so the caller can retry. dual_bank_init() knows that an
         * exhausted pool cannot be cleared and stops trusting the Flash
         * evidence in that case (see below), so an unwritable marker can never
         * provoke a spurious bank switch. */
        db_scratch.ok_pending = 1U;
        if (db_status == DUAL_BANK_PRIMARY_OK) {
            db_status = DUAL_BANK_DEGRADED;
        }
        return -1;
    }

    db_scratch.fault_count = 0U;
    db_scratch.pending     = 0U;
    db_scratch.ok_pending  = 0U;
    db_fault_count         = 0U;
    return 0;
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

    const uint32_t optr = read_user_option_bytes();

    db_optr       = optr;
    db_bfb2_armed = option_bfb2_enabled(optr);

    if (db_bfb2_armed) {
        /* G5 — BFB2 is already programmed but we are still executing bank 1:
         * either the OBL_LAUNCH reset has not happened yet, or the boot ROM
         * rejected bank 2 and fell back. Reprogramming the same value would
         * burn the option area and could loop resets, so stop here and report
         * the armed state. */
        db_status = DUAL_BANK_FALLBACK_ARMED;
        return DUAL_BANK_SWITCH_ALREADY;
    }
    if (!geometry_is_dual_bank(optr)) {               /* G2 */
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

    db_status     = DUAL_BANK_FALLBACK_ARMED;
    db_bfb2_armed = true;

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

    /* ok_pending is a ONE-SHOT token, honoured only by the boot immediately
     * after the one that raised it (Kilo #21, comment id 3740842364).
     *
     * dual_bank_boot_complete() raises it whenever its ls_append() fails for
     * ANY reason - a transient HAL_FLASH_Unlock() failure, a PGSERR, the
     * "slot not erased" bail-out - not only on a full pool. Left in the warm
     * scratch it could sit there for months and then, on the boot where the
     * image genuinely starts looping and the pool finally fills, zero
     * fault_count from a fossilised "trust me, I booted fine once" token: the
     * golden-image fallback would disarm itself at the precise moment it is
     * the only thing left. Snapshotting and clearing it here bounds its
     * lifetime to exactly one boot, whatever path is taken below. */
    const uint32_t ok_pending_snapshot = db_scratch.ok_pending;
    db_scratch.ok_pending = 0U;

    db_optr        = read_user_option_bytes();
    db_active_bank = dual_bank_active_bank();
    db_bfb2_armed  = option_bfb2_enabled(db_optr);

    /* Fold the evidence: what previous boots persisted, plus anything the
     * fault handlers recorded in RAM since the last successful boot. */
    const uint32_t persisted = ls_count_boot_faults();
    const uint32_t in_ram    = db_scratch.fault_count;
    const bool     pool_full = (ls_first_free_slot() >= LASTSTATES_MAX_ENTRIES);

    if (pool_full) {
        /* Nothing can be appended any more — neither a fault nor the boot-OK
         * marker that clears it. The Flash evidence is therefore frozen and
         * possibly stale, so it may not drive the switch decision.
         *
         * The RAM scratch is not automatically fresher: dual_bank_boot_complete()
         * cannot clear db_scratch.fault_count when its ls_append() fails, it
         * only raises ok_pending. Consuming the one-shot token here is the
         * ESCAPE from the otherwise permanent "pool full + fault_count >=
         * threshold => boot_looping on every warm boot, forever, on a healthy
         * image" trap (W2-2 review): the PREVIOUS boot did prove itself, it
         * just could not say so in Flash. Honour that proof once, and only
         * for the boot that directly follows it. */
        if (ok_pending_snapshot != 0U) {
            db_scratch.fault_count = 0U;
            db_fault_count         = 0U;
        } else {
            /* Fail-safe: the worst case is that a genuinely failing image is
             * not detected across a power cycle, never that a healthy one is
             * thrown away. */
            db_fault_count = in_ram;
        }
        /* `pending` can never be serviced while the pool is full; leaving it
         * set would make every later boot re-read stale evidence. */
        db_scratch.pending = 0U;
    } else if (db_scratch.pending != 0U) {
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

    if (db_bfb2_armed) {
        /* BFB2 programmed, yet the address map still shows bank 1: the switch
         * is armed and pending (or the boot ROM refused bank 2). Report it and
         * do not try to arm it again this boot. */
        db_status = DUAL_BANK_FALLBACK_ARMED;
        (void)dual_bank_verify_golden();   /* keep the health flag current */
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
    } else if (rc == DUAL_BANK_SWITCH_ALREADY) {
        /* switch_to_golden() already latched ON_GOLDEN / FALLBACK_ARMED. */
    } else {
        db_status = DUAL_BANK_DEGRADED;
    }
    return db_status;
}

/* ---------- Accessors ---------- */
dual_bank_status_t dual_bank_get_status(void)      { return db_status; }
bool               dual_bank_golden_valid(void)    { return db_golden_valid; }
uint32_t           dual_bank_get_optr_snapshot(void) { return db_optr; }
bool               dual_bank_bfb2_armed(void)      { return db_bfb2_armed; }
