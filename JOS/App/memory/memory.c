#include "memory.h"
#include "main.h"
#ifndef HOST_UNIT_TEST
/* Core/Inc is deliberately NOT on the host test include path: it also holds
 * the CubeMX main.h, which would shadow test/fakes/main.h and drag the whole
 * HAL/CMSIS tree into the host build. The only thing this header supplies is
 * the pool geometry, which the host build overrides anyway (see
 * LASTSTATES_FLASH_BASE below). */
#include "dual_bank.h"
#endif
/* bookkeeping mirror scrubbing (W2-5). Core/Inc/seu_mitigation.h on the
 * target; test/fakes/seu_mitigation.h (same signatures, no RTOS) on the host,
 * so the lock/commit calls below stay flight code in both builds. */
#include "seu_mitigation.h"
#include <string.h>
#include <stdint.h>

/* ========== FRAM driver (I2C) ==========
 * Per RED_DES_ElectronicArchitecture_V1:
 *   4x FM24VN10-G (1 Mbit each, I2C interface) = 4 Mbit total.
 *   All four FM24VN10-G devices are on the OBC PCB.
 *
 * I2C addresses: 0x50, 0x51, 0x52, 0x53 (A0/A1 pins select chip).
 * Each chip: 128 Kbit = 16 KB address space.
 * Total: 4 x 16 KB = 64 KB.
 *
 * FM24VN_CHIP_SIZE (16 * 1024 B) is a power of two, so the address decode uses
 * shift/mask instead of division/modulo. This removes the only runtime
 * divisions in the memory module and therefore the divide-by-zero class
 * entirely (review M1).
 */

#define FM24VN_I2C_ADDR_BASE  0x50
#define FM24VN_PAGE_SIZE      16
#define FM24VN_CHIP_SIZE      (16U * 1024U)
#define FM24VN_CHIP_SIZE_LOG2 14U    /* 16 KB = 2^14 */
#define FM24VN_NUM_CHIPS      4
#define FRAM_SIZE             (FM24VN_NUM_CHIPS * FM24VN_CHIP_SIZE)

/* Compile-time guard: a zero (or non-power-of-two) chip size would make the
 * shift/mask decode below wrong and is the divide-by-zero class M1 guards
 * against. */
_Static_assert(FM24VN_CHIP_SIZE > 0U, "FM24VN_CHIP_SIZE must be > 0");
_Static_assert((FM24VN_CHIP_SIZE & (FM24VN_CHIP_SIZE - 1U)) == 0U,
               "FM24VN_CHIP_SIZE must be a power of two");

extern I2C_HandleTypeDef hi2c2;

/* The STM32 HAL I2C entry points take the device address ALREADY shifted left
 * by one (the 8-bit device-select byte, R/W bit clear). The FM24VN10-G parts
 * are 7-bit 0x50..0x53, so the bytes that must reach HAL_I2C_Mem_Read/Write
 * are 0xA0/0xA2/0xA4/0xA6. Passing the raw 7-bit value puts 0x28 on the bus
 * and addresses nothing. */
static uint16_t fram_addr_to_chip(uint32_t addr)
{
    uint16_t addr7 = (uint16_t)((addr >> FM24VN_CHIP_SIZE_LOG2) + FM24VN_I2C_ADDR_BASE);
    return (uint16_t)(addr7 << 1);
}

static uint16_t fram_addr_to_offset(uint32_t addr)
{
    return (uint16_t)(addr & (FM24VN_CHIP_SIZE - 1U));
}

void fram_init(void)
{
    /* TODO: verify each chip responds at its I2C address */
}

int fram_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (addr + len > FRAM_SIZE) return -1;

    uint16_t dev_addr = fram_addr_to_chip(addr);
    uint16_t offset = fram_addr_to_offset(addr);

    if (HAL_I2C_Mem_Read(&hi2c2, dev_addr, offset, I2C_MEMADD_SIZE_16BIT,
                         buf, (uint16_t)len, 1000) != HAL_OK)
        return -1;
    return 0;
}

int fram_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (addr + len > FRAM_SIZE) return -1;

    uint16_t dev_addr = fram_addr_to_chip(addr);
    uint16_t offset = fram_addr_to_offset(addr);

    if (HAL_I2C_Mem_Write(&hi2c2, dev_addr, offset, I2C_MEMADD_SIZE_16BIT,
                          (uint8_t *)buf, (uint16_t)len, 1000) != HAL_OK)
        return -1;
    return 0;
}

/* ========== Cyclic buffer ========== */
/* 4x FM24VN10-G = 64 KB FRAM used as circular buffer */

static uint32_t cb_head = 0;   /* next write position */

void cyclic_buffer_init(void)
{
    /* TODO: read persisted head from a known FRAM address */
    cb_head = 0;
}

int cyclic_buffer_write(const uint8_t *data, size_t len)
{
    if (len == 0) return 0;

    /* Handle wrap-around */
    if (cb_head + len > FRAM_SIZE) {
        uint32_t first = FRAM_SIZE - cb_head;
        fram_write(cb_head, data, first);
        fram_write(0, data + first, len - first);
        cb_head = len - first;
    } else {
        fram_write(cb_head, data, len);
        cb_head += len;
    }
    return 0;
}

int cyclic_buffer_read(uint32_t offset, uint8_t *buf, size_t len)
{
    if (offset + len > FRAM_SIZE) return -1;
    return fram_read(offset, buf, len);
}

uint32_t cyclic_buffer_head(void)
{
    return cb_head;
}

/* ========== LastStates pool ========== */
/* 64 entries x 128 B = 8 KB in internal Flash, starting at 0x08080000.
 *
 * The pool is a ring buffer of fixed 128 B records. STM32L4 internal Flash can
 * only be programmed once per bit until the 2 KB page holding the slot is
 * erased again, so a full page (16 slots) is erased when the write cursor
 * wraps back onto still-valid data. Erasing only that ONE page recycles the
 * OLDEST page and therefore never overwrites the newest valid record
 * (NASA-STD-8739.8 fault containment / ECSS-E-ST-80C post-mortem integrity).
 *
 * Because a subsequent write may land in a page whose other slots were just
 * erased, the pool is not always a contiguous run of valid records. Post-mortem
 * readback (laststates_dump_all / laststates_count) therefore scans the WHOLE
 * pool for valid slots rather than trusting a cached count.
 */

#ifdef HOST_UNIT_TEST
/* Host unit-test build: the pool cannot live at a hard-coded Flash address in
 * a PC process, so test/support/host_flash.c owns the base and publishes it
 * here. uintptr_t, never uint32_t: truncating a 64-bit mapping to 32 bits
 * produced a wild pointer (segfault) instead of a test failure. On the target
 * uintptr_t IS uint32_t, so the flight code below is unchanged. */
extern uintptr_t flash_base;
#define LASTSTATES_FLASH_BASE  (flash_base)
#else
/* Single source of truth for the pool base: Core/Inc/dual_bank.h. The
   dual-bank fallback validates the golden-image slot against this address
   (gate G1) and appends boot-fault markers to the same pool, so the two must
   never drift apart. Relocating the pool means changing
   DUAL_BANK_LASTSTATES_BASE, the LASTSTATES region in STM32L496VGTX_FLASH.ld
   and the ground forensics tooling together — see the warning in dual_bank.h
   (0x080FE000 is NOT a valid target). */
#define LASTSTATES_FLASH_BASE  ((uintptr_t)DUAL_BANK_LASTSTATES_BASE)
#endif
#define LASTSTATES_FLASH_END   (LASTSTATES_FLASH_BASE + \
                                (uintptr_t)LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)
#define LS_PAGE_SHIFT          11U   /* STM32L4 Flash page = 2 KB = 2^11 (HAL FLASH_PAGE_SIZE) */

/* A freshly-erased Flash double-word reads as all ones; a programmed slot is
 * never all-ones (the trigger byte is always a small enum value), so testing
 * the first double-word reliably distinguishes a free slot from a valid one. */
#define SLOT_ERASED_DWORD      0xFFFFFFFFFFFFFFFFULL

/* Compile-time guards tying the ring to the dual-bank pool description, so the
   two writers of this pool (here and Core/Src/dual_bank.c) can never drift
   apart or straddle the bank boundary (W2-2 review C3). */
#ifndef HOST_UNIT_TEST
/* Pool-geometry guards. Skipped on the host, where LASTSTATES_FLASH_BASE is a
 * relocatable variable rather than a constant expression; the geometry they
 * protect is a property of the linker script, which the host build does not
 * use. */
_Static_assert((LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)
               <= DUAL_BANK_LASTSTATES_SIZE,
               "LastStates ring does not fit the reserved pool");
_Static_assert(((LASTSTATES_FLASH_BASE - DUAL_BANK_FLASH_BASE)
                % DUAL_BANK_PAGE_SIZE) == 0U,
               "LastStates pool must start on a Flash page boundary");
_Static_assert(((LASTSTATES_FLASH_BASE - DUAL_BANK_FLASH_BASE) / DUAL_BANK_BANK_SIZE)
               == (((LASTSTATES_FLASH_END - 1U) - DUAL_BANK_FLASH_BASE) / DUAL_BANK_BANK_SIZE),
               "LastStates pool must not straddle the bank boundary");
#endif /* !HOST_UNIT_TEST — pool-geometry guards */

/* Pool bookkeeping, mirrored and scrubbed by seu_mitigation.c (W2-5). Kept
   in one structure with a magic marker so the scrubber has something to vote
   on and ground can recognise it in a dump. `idx` is THE write cursor of the
   ring; `count` tracks how many valid records the last scan found plus the
   records written since. */
static laststates_mirror_t ls_mirror = {
    .magic = LASTSTATES_MIRROR_MAGIC,
    .count = 0U,   /* number of entries written */
    .idx   = 0U,   /* next write index (circular) */
};

/* Published to seu_mitigation_register_region() so the scrubber can vote on
   the ring bookkeeping. Defined for both builds: it touches no target-only
   register state, and memory.h declares it unconditionally. */
void *laststates_mirror_region(size_t *len)
{
    if (len != NULL) { *len = sizeof(ls_mirror); }
    return &ls_mirror;
}

/* Compile-time guards against the divide-by-zero class in this module (M1). */
_Static_assert(LASTSTATES_MAX_ENTRIES > 0U, "LASTSTATES_MAX_ENTRIES must be > 0");
_Static_assert((LASTSTATES_MAX_ENTRIES & (LASTSTATES_MAX_ENTRIES - 1U)) == 0U,
               "LASTSTATES_MAX_ENTRIES must be a power of two (ring mask)");
_Static_assert(LASTSTATES_ENTRY_SIZE > 0U,  "LASTSTATES_ENTRY_SIZE must be > 0");
_Static_assert(FLASH_PAGE_SIZE > 0U,        "FLASH_PAGE_SIZE must be > 0");
_Static_assert((LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE) % FLASH_PAGE_SIZE == 0U,
               "LastStates pool must be a whole number of Flash pages");
_Static_assert((FLASH_PAGE_SIZE & (FLASH_PAGE_SIZE - 1U)) == 0U,
               "FLASH_PAGE_SIZE must be a power of two (page mask)");

/* ---------- DWT cycle counter (bounded Flash waits in the fault path) ----------
 * The fault handlers run with interrupts masked, so HAL_GetTick() never
 * advances there and a GetTick()-based Flash timeout would block forever if
 * the controller stuck. Every Flash operation below is instead bounded by the
 * DWT cycle counter, which is CPU-clock based and always runs, so the handler
 * can never block indefinitely (review C2).
 */
#define FLASH_DWORD_TIMEOUT_CYCLES   0x20000U   /* ~1.6 ms @ 80 MHz, >> 100 us nominal */
#define FLASH_PAGE_TIMEOUT_CYCLES    0x400000U  /* ~52 ms @ 80 MHz, >> 22 ms nominal   */

static int slot_is_erased(uint32_t idx)
{
    const uint64_t *d = (const uint64_t *)(LASTSTATES_FLASH_BASE +
                                           (uintptr_t)idx * LASTSTATES_ENTRY_SIZE);
    return (*d == SLOT_ERASED_DWORD) ? 1 : 0;
}

#ifdef HOST_UNIT_TEST
/* ---------------------------------------------------------------------------
 * Host unit-test build.
 *
 * The Cortex-M register model (FLASH->CR, DWT, CoreDebug) does not exist on a
 * PC, so the three primitives below are expressed through the HAL entry points
 * that test/support/host_flash.c emulates with real NOR semantics: unlock
 * gating (RM0351 3.3.5), one program per erase cycle and 0xFF after erase.
 * The *callers* (laststates_init/write/dump_all, the ring and page-recycling
 * logic, every bounds guard) are the unmodified flight code - which is the
 * part the tests exist to pin down.
 * ------------------------------------------------------------------------- */
static void dwt_cyccnt_enable(void)
{
    /* No cycle counter on the host; the HAL doubles return immediately. */
}

static int flash_write_dword_bounded(uintptr_t addr, uint64_t data)
{
    return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              (uint32_t)addr, data) == HAL_OK) ? 0 : -1;
}

static int flash_erase_page_bounded(uintptr_t addr)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t               page_error = 0U;
    HAL_StatusTypeDef      st;

    /* Same bounds guard (B3) as the target path. */
    if (addr < LASTSTATES_FLASH_BASE || addr >= LASTSTATES_FLASH_END) {
        return -1;
    }

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks     = FLASH_BANK_1;
    erase_init.Page      = (uint32_t)((addr - 0x08000000UL) / FLASH_PAGE_SIZE);
    erase_init.NbPages   = 1U;

    HAL_FLASH_Unlock();
    st = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? 0 : -1;
}
#else  /* target build: direct register access, cycle-counter bounded */
static void dwt_cyccnt_enable(void)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    }
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        DWT->CYCCNT = 0U;
        DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/* Program one 64-bit double word with a hard, cycle-counted bound. Never relies
 * on HAL_GetTick(); returns 0 on success, -1 on timeout/error. Mirrors the HAL
 * double-word program sequence (set PG, write the two words, poll BSY). */
static int flash_write_dword_bounded(uintptr_t addr, uint64_t data)
{
    uint32_t start = DWT->CYCCNT;

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    SET_BIT(FLASH->CR, FLASH_CR_PG);
    *(__IO uint32_t *)(uintptr_t)addr       = (uint32_t)data;
    __ISB();
    *(__IO uint32_t *)(uintptr_t)(addr + 4U) = (uint32_t)(data >> 32U);

    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY)) {
        if ((DWT->CYCCNT - start) >= FLASH_DWORD_TIMEOUT_CYCLES) {
            CLEAR_BIT(FLASH->CR, FLASH_CR_PG);
            return -1;
        }
    }
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG);

    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_SR_ERRORS)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SR_ERRORS);
        return -1;
    }
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_EOP)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP);
    }
    return 0;
}

/* Erase the single 2 KB Flash page that contains [addr, addr+FLASH_PAGE_SIZE)
 * with a cycle-counted bound. Correctly handles the dual-bank layout of the
 * STM32L496: the LastStates pool sits at 0x08080000, i.e. in bank 2. Returns
 * 0 on success, -1 on timeout/error. */
static int flash_erase_page_bounded(uintptr_t addr)
{
    dwt_cyccnt_enable();
    uint32_t start = DWT->CYCCNT;

    /* Bounds guard (B3): only ever erase a page inside the LastStates pool.
     * Refusing anything else prevents an errant call from wiping the vector
     * table or any other Flash region. */
    if (addr < LASTSTATES_FLASH_BASE || addr >= LASTSTATES_FLASH_END) {
        return -1;
    }

    uint32_t page_abs = (addr - 0x08000000U) >> LS_PAGE_SHIFT;
    uint32_t banks = FLASH_BANK_1;
    uint32_t pnb   = page_abs;

    if ((FLASH->OPTR & FLASH_OPTR_DUALBANK) != 0U) {
        if (page_abs >= 256U) {
            banks = FLASH_BANK_2;
            pnb   = page_abs - 256U;
        }
    }

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    /* Mirror HAL FLASH_PageErase(): select bank via BKER, page via PNB, then PER+STRT. */
    if (banks == FLASH_BANK_2) {
        SET_BIT(FLASH->CR, FLASH_CR_BKER);
    } else {
        CLEAR_BIT(FLASH->CR, FLASH_CR_BKER);
    }
    MODIFY_REG(FLASH->CR, FLASH_CR_PNB, ((pnb & 0xFFU) << FLASH_CR_PNB_Pos));
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    SET_BIT(FLASH->CR, FLASH_CR_STRT);

    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY)) {
        if ((DWT->CYCCNT - start) >= FLASH_PAGE_TIMEOUT_CYCLES) {
            CLEAR_BIT(FLASH->CR, (FLASH_CR_PER | FLASH_CR_PNB));
            HAL_FLASH_Lock();
            return -1;
        }
    }
    CLEAR_BIT(FLASH->CR, (FLASH_CR_PER | FLASH_CR_PNB));

    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_SR_ERRORS)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SR_ERRORS);
        HAL_FLASH_Lock();
        return -1;
    }
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_EOP)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP);
    }

    HAL_FLASH_Lock();
    return 0;
}
#endif /* HOST_UNIT_TEST */

static int flash_write_row(uintptr_t addr, const uint8_t *data, size_t len)
{
    if (len == 0U || (len % 8U) != 0U) return -1;
    if (addr < LASTSTATES_FLASH_BASE || (addr + len) > LASTSTATES_FLASH_END) return -1;

    dwt_cyccnt_enable();

    HAL_FLASH_Unlock();
    uint32_t off = 0U;
    int rc = 0;
    while (off < len) {
        uint64_t dword = 0U;
        memcpy(&dword, data + off, 8);
        if (flash_write_dword_bounded(addr + off, dword) != 0) {
            rc = -1;
            break;
        }
        off += 8U;
    }
    HAL_FLASH_Lock();
    return rc;
}

/* Re-derive the write cursor from Flash. The pool is always written in order,
 * so the first erased slot is the next free one.
 *
 * This is also what makes the pool safe to SHARE with a second writer: the
 * dual-bank fallback (Core/Src/dual_bank.c) appends boot-fault / boot-OK
 * markers (tagged 'DBNK') outside laststates_write(), both before
 * laststates_init() runs and later from a task. Re-deriving the cursor means
 * those appends are simply picked up here instead of colliding with a stale
 * in-RAM index (W2-2 review C2). Returns the number of occupied slots.
 */
static uint32_t laststates_resync(void)
{
    uint32_t idx = 0U;

    while (idx < LASTSTATES_MAX_ENTRIES) {
        if (slot_is_erased(idx)) {
            break;
        }
        idx++;
    }
    /* A completely full pool wraps to slot 0; the next write recycles the
     * oldest page there. */
    ls_mirror.idx = (idx >= LASTSTATES_MAX_ENTRIES) ? 0U : idx;
    return idx;
}

void laststates_init(void)
{
    dwt_cyccnt_enable();

    /* Scan the pool for the first free (erased) slot. That slot is where the
     * next record must be appended so post-mortem readback can recover the
     * existing trail after the reboot (review C1). If the pool is completely
     * full we wrap to index 0 and the next write will recycle the oldest page.
     */
    uint32_t idx   = 0U;
    uint32_t valid = 0U;
    while (idx < LASTSTATES_MAX_ENTRIES) {
        if (slot_is_erased(idx)) {
            break;
        }
        idx++;
    }
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            valid++;
        }
    }

    /* Publish the scanned cursor through the SEU-scrubbed bookkeeping mirror
       and take its snapshot, so the scrub task votes on the real state from
       the first cycle (W2-5). The commit is a no-op until
       seu_mitigation_init() has registered the region. */
    seu_mitigation_lock();
    ls_mirror.magic = LASTSTATES_MIRROR_MAGIC;
    ls_mirror.idx   = (idx < LASTSTATES_MAX_ENTRIES) ? idx : 0U;
    ls_mirror.count = valid;
    (void)seu_mitigation_commit(SEU_REGION_LASTSTATES);
    seu_mitigation_unlock();
}

int laststates_write(const laststates_entry_t *entry)
{
    if (entry == NULL) return -1;

    /* Bounds guard: the cursor is always in range after init, but never trust
       a cached index against corruption. */
    if (ls_mirror.idx >= LASTSTATES_MAX_ENTRIES) {
        ls_mirror.idx = 0U;
    }

    /* The pool has a second writer: Core/Src/dual_bank.c appends boot-fault
       and boot-OK markers (tagged 'DBNK') outside laststates_write(), both
       before laststates_init() runs and later from a task. Programming a slot
       that is no longer erased fails on STM32L4 and, because the cursor never
       advanced, every later write would fail too — the forensic log would be
       silently dead. So whenever the target slot has moved under us, re-derive
       the cursor from Flash before deciding anything (W2-2 review C2). Only if
       the pool is genuinely full does the ring wrap and recycle a page. */
    uint32_t ls_idx = ls_mirror.idx;  /* local cursor = SEU mirror cursor */
    if (!slot_is_erased(ls_idx)) {
        (void)laststates_resync();
        ls_idx = ls_mirror.idx;  /* resync may have moved the cursor */
    }

    uintptr_t addr = (uintptr_t)LASTSTATES_FLASH_BASE + (uintptr_t)ls_idx * LASTSTATES_ENTRY_SIZE;
    ls_mirror.idx = ls_idx;  /* keep the SEU scrubber mirror in sync (W2-5) */

    /* If the target slot STILL holds a valid (programmed) record after the
     * resync, the ring really has wrapped: recycle the OLDEST page it belongs
     * to. Erasing only that page preserves every newer page, so the newest
     * valid record is never lost (review C1). Note this can drop dual-bank
     * boot-fault evidence — fail-safe by design: a lost counter can only
     * inhibit a fallback, never trigger one. */
    if (!slot_is_erased(ls_idx)) {
        uintptr_t page_addr = addr & ~(((uintptr_t)1U << LS_PAGE_SHIFT) - 1U);
        if (flash_erase_page_bounded(page_addr) != 0) {
            return -1;
        }
    }

    if (flash_write_row(addr, (const uint8_t *)entry, LASTSTATES_ENTRY_SIZE) != 0) {
        return -1;
    }

    /* Advance the bookkeeping and re-take its snapshot in one atomic step, so
       the scrub task can never see the pair half-updated and mistake a
       legitimate advance for a bit flip (W2-5). The lock is PRIMASK based, so
       this is also safe on the parity-NMI path, which logs through here. */
    seu_mitigation_lock();
    ls_mirror.idx = (ls_mirror.idx + 1U) & (LASTSTATES_MAX_ENTRIES - 1U);
    if (ls_mirror.count < LASTSTATES_MAX_ENTRIES) { ls_mirror.count++; }
    (void)seu_mitigation_commit(SEU_REGION_LASTSTATES);
    seu_mitigation_unlock();
    return 0;
}

int laststates_dump_all(uint8_t *out, size_t *len)
{
    if (out == NULL || len == NULL) return -1;

    /* *len is the caller's buffer CAPACITY on entry. A buffer that cannot hold
     * every valid record is refused instead of overrun: with a full pool the
     * copy is 8 KB, which on the downlink path is a caller stack frame. The
     * required size is reported back so the caller can retry (review B3 /
     * NASA-STD-8739.8 buffer-overrun class). */
    size_t   capacity = *len;
    uint32_t needed   = 0U;

    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            needed++;
        }
    }
    if ((size_t)needed * LASTSTATES_ENTRY_SIZE > capacity) {
        *len = (size_t)needed * LASTSTATES_ENTRY_SIZE;   /* required, nothing copied */
        return -1;
    }

    /* Scan the whole pool: because page recycling can leave gaps, valid
     * records are not necessarily a contiguous prefix. Copy every valid slot
     * in index order so ground can reconstruct the trail by timestamp. */
    uint32_t valid = 0U;
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            memcpy(out + valid * LASTSTATES_ENTRY_SIZE,
                   (const uint8_t *)(LASTSTATES_FLASH_BASE + (uintptr_t)i * LASTSTATES_ENTRY_SIZE),
                   LASTSTATES_ENTRY_SIZE);
            valid++;
        }
    }
    *len = valid * LASTSTATES_ENTRY_SIZE;
    return 0;
}

uint32_t laststates_count(void)
{
    /* Authoritative count comes from Flash, not from the cached mirror: page
       recycling leaves gaps and a cached counter can itself be upset. */
    uint32_t valid = 0U;
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            valid++;
        }
    }
    return valid;
}
