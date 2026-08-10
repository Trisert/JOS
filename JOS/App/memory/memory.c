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
#ifndef HOST_UNIT_TEST
/* CMSIS-RTOS2 mutex for the LastStates pool lock (W2-2 review, CRITICAL).
 * The host build is single-threaded and stubs the lock out entirely. */
#include "cmsis_os2.h"
#endif
/* bookkeeping mirror scrubbing (W2-5). Core/Inc/seu_mitigation.h on the
 * target; test/fakes/seu_mitigation.h (same signatures, no RTOS) on the host,
 * so the lock/commit calls below stay flight code in both builds. */
#include "seu_mitigation.h"
#include <string.h>
#include <stdint.h>

/* ========== FRAM driver (I2C) ==========
 * Per RED_DES_ElectronicArchitecture_V1:
 *   4x FM24VN10-G on the OBC PCB, on I2C2.
 *
 * Device-select (7-bit) addresses: 0x50, 0x51, 0x52, 0x53 (A0/A1 pins).
 * The driver addresses ONE 16 KB window per device select, using the 16-bit
 * memory-address transfer of HAL_I2C_Mem_Read/Write:
 *
 *   FM24VN_CHIP_SIZE = 16 * 1024 B  = 16 KB  per device select
 *   FRAM_SIZE        = 4 * 16 KB    = 64 KB  usable, and 64 KB is the number
 *                                            the ICD, the cyclic buffer and
 *                                            the unit tests all agree on.
 *
 * DO NOT write `#define FM24VN_CHIP_SIZE 16`: that is a byte count, not a
 * kilobyte count, and it collapses FRAM_SIZE from 65536 to 64 bytes. Every
 * fram_read()/fram_write()/cyclic_buffer_*() bound check below is expressed
 * against FRAM_SIZE, so the whole FRAM would silently shrink to 64 usable
 * bytes and every telemetry record past the first would be rejected with -1
 * (Kilo review of PR #9, finding C1). The _Static_assert on FRAM_SIZE below
 * turns that typo into a compile error instead of a silent loss of storage.
 *
 * FM24VN_CHIP_SIZE is a power of two, so the address decode uses shift/mask
 * instead of division/modulo. This removes the only runtime divisions in the
 * memory module and therefore the divide-by-zero class entirely (review M1).
 */

#define FM24VN_I2C_ADDR_BASE  0x50
#define FM24VN_PAGE_SIZE      16
#define FM24VN_CHIP_SIZE      (16UL * 1024UL)
#define FM24VN_CHIP_SIZE_LOG2 14U    /* 16 KB = 2^14 */
#define FM24VN_NUM_CHIPS      4
#define FRAM_SIZE             (FM24VN_NUM_CHIPS * FM24VN_CHIP_SIZE)

/* FRAM layout (64 KB total):
 *   [0 .. cyclic_buffer_head)   : cyclic science-data buffer (wraps the device)
 *   [top - scrub_pool .. top)   : SEU scrub golden records (W2-5), reserved at
 *                                 the TOP and grown downward; see scrub.h
 *                                 SCRUB_FRAM_BASE. The scrub records are
 *                                 CRC-protected and reject any payload they do
 *                                 not own, so the two regions never corrupt
 *                                 each other even on a head-pointer collision.
 *
 * Compile-time guards: a zero (or non-power-of-two) chip size would make the
 * shift/mask decode below wrong and is the divide-by-zero class M1 guards
 * against; the size/geometry asserts pin the 64 KB total from the ICD so a
 * mistyped literal cannot silently shrink the store (finding C1). */
_Static_assert(FM24VN_CHIP_SIZE > 0U, "FM24VN_CHIP_SIZE must be > 0");
_Static_assert((FM24VN_CHIP_SIZE & (FM24VN_CHIP_SIZE - 1U)) == 0U,
               "FM24VN_CHIP_SIZE must be a power of two");
_Static_assert(FM24VN_CHIP_SIZE == (1UL << FM24VN_CHIP_SIZE_LOG2),
               "FM24VN_CHIP_SIZE_LOG2 must match FM24VN_CHIP_SIZE");
_Static_assert(FRAM_SIZE == (64UL * 1024UL),
               "FRAM_SIZE must be 64 KB (4 x 16 KB) per RED_DES_ElectronicArchitecture_V1");

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
 *
 * "Valid" means WRITTEN TO COMPLETION, not merely "not erased": a reset in the
 * middle of the 16 double-word programming sequence leaves a torn record whose
 * timestamp/trigger bytes are present but whose payload is still 0xFF. Such a
 * slot is neither free (it cannot be re-programmed) nor reportable, so
 * slot_is_erased()/slot_is_complete() below distinguish the three states and
 * the readback path only ever returns complete records (finding C4).
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

/* A freshly-erased Flash double-word reads as all ones. */
#define SLOT_ERASED_DWORD      0xFFFFFFFFFFFFFFFFULL

/* Number of 64-bit programming units in one record. The STM32L4 Flash word is
 * a double word, so this is also the number of individually-committed writes
 * laststates_write() performs, i.e. the granularity at which a reset can tear
 * a record in half. */
#define LASTSTATES_ENTRY_DWORDS  ((uint32_t)(LASTSTATES_ENTRY_SIZE / 8U))
_Static_assert((LASTSTATES_ENTRY_SIZE % 8U) == 0U,
               "LastStates entry must be a whole number of Flash double words");

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

static const volatile uint64_t *slot_dwords(uint32_t idx)
{
    return (const volatile uint64_t *)(LASTSTATES_FLASH_BASE +
                                       (uintptr_t)idx * LASTSTATES_ENTRY_SIZE);
}

/* TRUE only when the WHOLE slot is still erased, i.e. it can be programmed.
 *
 * The previous implementation tested the first double word only. That is not
 * enough in either direction: a record torn by a reset mid-write (dword 0
 * programmed, tail still 0xFF) was reported as a complete, valid record, and
 * the ring bookkeeping could hand a partially-programmed slot back to
 * flash_write_row(), where re-programming a non-erased double word fails with
 * PROGERR and kills every later write (Kilo review of PR #9, finding C4).
 * Scanning all 16 double words costs 16 Flash reads on a free slot and exits
 * on the first word for an occupied one, which is the common case. */
static int slot_is_erased(uint32_t idx)
{
    const volatile uint64_t *d = slot_dwords(idx);

    for (uint32_t i = 0U; i < LASTSTATES_ENTRY_DWORDS; i++) {
        if (d[i] != SLOT_ERASED_DWORD) {
            return 0;
        }
    }
    return 1;
}

/* TRUE when the slot holds a record that was written to completion.
 *
 * flash_write_row() programs strictly in ascending address order and each
 * double word is committed by the Flash controller before the next one is
 * started, so the LAST double word can only be programmed after every earlier
 * one. Seeing it programmed therefore proves the full 128 B landed; seeing it
 * erased while dword 0 is programmed is exactly the torn-write signature of a
 * reset (or a bounded-wait timeout) in the middle of laststates_write().
 * Torn records are skipped by laststates_count()/laststates_dump_all(), so
 * ground never reconstructs a timeline from a half-record whose timestamp and
 * trigger bytes are meaningless.
 *
 * The last double word covers context[110..115] plus the two structure
 * padding bytes, and every writer of this pool (laststates_write() callers,
 * Core/Src/dual_bank.c, Core/Src/sram2_parity.c, App/obsw/boot_crc.c)
 * memset()s the entry to zero first, so a complete record can never read back
 * as all-ones there.
 *
 * KNOWN LIMITATION (documented deliberately, not an oversight): this is a
 * write-COMPLETION check, not an integrity check. The 128 B entry layout is a
 * frozen ground ICD with no CRC field, so a single-event upset that flips a
 * bit inside an otherwise complete record is still reported as valid. Adding a
 * per-record CRC requires an ICD change (a CRC over bytes 0..123 stored in the
 * currently-unused tail) and is tracked as a separate work package; the SEU
 * scrubber (W2-5) covers the RAM bookkeeping mirror only, never the Flash
 * records. */
static int slot_is_complete(uint32_t idx)
{
    const volatile uint64_t *d = slot_dwords(idx);

    return ((d[0] != SLOT_ERASED_DWORD) &&
            (d[LASTSTATES_ENTRY_DWORDS - 1U] != SLOT_ERASED_DWORD)) ? 1 : 0;
}

/* Erase bounds guard (finding C2), shared by the host and the target erase
 * paths so the two can never diverge.
 *
 * A page erase is the single most destructive operation this module can
 * perform: the target path drives FLASH->CR directly, so a corrupted cursor
 * or a bogus caller address would happily erase the vector table, the active
 * application image or the dual-bank golden image. Accept an address only if
 * it is page-aligned AND the whole 2 KB page it starts is inside the
 * LastStates pool [LASTSTATES_FLASH_BASE, LASTSTATES_FLASH_END). */
static int flash_page_in_pool(uintptr_t addr)
{
    const uintptr_t page_mask = ((uintptr_t)1U << LS_PAGE_SHIFT) - 1U;

    if (addr < LASTSTATES_FLASH_BASE)  { return 0; }
    if (addr >= LASTSTATES_FLASH_END)  { return 0; }
    /* Alignment is checked RELATIVE to the pool base (which is itself page
     * aligned: _Static_assert above on the target, mmap granularity on the
     * host). Together with the pool being a whole number of pages - also
     * static-asserted - an aligned address below the end always has its whole
     * page inside the pool, so no separate end-overlap test is needed. */
    if (((addr - LASTSTATES_FLASH_BASE) & page_mask) != 0U) { return 0; }
    return 1;
}

#ifdef HOST_UNIT_TEST
/* Test-only window onto the guard above. The guard is what stands between a
 * corrupted ring cursor and an erased vector table, so it is pinned down by a
 * direct unit test instead of only being reached through laststates_write(). */
int laststates_erase_addr_allowed(uintptr_t addr)
{
    return flash_page_in_pool(addr);
}
#endif

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

    /* Exactly the same bounds guard as the target path (B3 / finding C2). */
    if (!flash_page_in_pool(addr)) {
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

/* Is the cycle counter actually counting?
 *
 * DWT_CYCCNT is an OPTIONAL Cortex-M4 feature and can additionally be held
 * disabled by a debug probe or by DWT->CTRL being write-ignored. If it never
 * advances, a `(DWT->CYCCNT - start) >= budget` loop can never terminate -
 * exactly the infinite spin in a fault handler that the bounded waits exist to
 * prevent. Reading it twice around a data-synchronisation barrier costs a
 * handful of cycles and tells the wait loop below to fall back to an iteration
 * budget instead. */
static int dwt_cyccnt_running(void)
{
    uint32_t t0 = DWT->CYCCNT;

    __DSB();
    __ISB();

    return (DWT->CYCCNT != t0) ? 1 : 0;
}

/* Poll FLASH_SR.BSY with a bound that ALWAYS terminates.
 *
 * Primary bound: the DWT cycle counter (CPU-clock based, runs with interrupts
 * masked, unlike HAL_GetTick()). Fallback bound, used when the cycle counter
 * is not implemented/enabled: a plain iteration budget. One iteration is at
 * least a few CPU cycles, so using the same number as the cycle budget only
 * ever makes the fallback timeout LONGER in wall-clock terms, never shorter -
 * it is a liveness guarantee, not a precise timeout.
 *
 * The iteration budget is counted and checked UNCONDITIONALLY, not only on the
 * fallback branch: dwt_cyccnt_running() is a photograph taken before the loop,
 * and CYCCNTENA can be cleared *while* we spin (a debug probe attaching, any
 * other agent writing DWT->CTRL). CYCCNT would then freeze at `start`,
 * (DWT->CYCCNT - start) would stay 0 and this loop - which exists precisely to
 * stop a fault handler spinning forever - would never exit (Kilo #26). With
 * both checks in place the cycle bound still decides the normal timeout (one
 * iteration costs several cycles, so it always trips first) and the spin count
 * is a strictly later backstop that no register write can disable.
 *
 * Returns 0 when BSY cleared in time, -1 on timeout (the caller owns the
 * register clean-up). */
static int flash_wait_bsy_bounded(uint32_t budget)
{
    const int      have_cyccnt = dwt_cyccnt_running();
    const uint32_t start       = DWT->CYCCNT;
    uint32_t       spins       = 0U;

    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY)) {
        /* cppcheck cannot model a memory-mapped register: it folds every read
           of the volatile DWT->CYCCNT to the same value, concludes
           dwt_cyccnt_running() always returns 0 (knownConditionTrueFalse) and
           then reduces the cycle test to `budget <= 0` on an unsigned type
           (unsignedLessThanZero). Both are artefacts of that folding, not
           defects — on the target CYCCNT increments every CPU cycle. Narrow,
           per-line suppressions; no check id is disabled anywhere else. */
        /* Scope: these are the only two inline suppressions in this file, they
           are attached to the single line below (not the function, not the
           file), and each names exactly one id. The repo policy they sit under
           is not "first-party code carries no suppressions" -- it is that a
           suppression must be per-line, must name one id, must carry a written
           justification, and must stay rare. JOS/Core/Src/sram2_parity.c is
           the only other first-party file that holds any (four, each justified
           at its own site); adding one anywhere else is a review decision, not
           a routine fix.

           Staleness: these cannot be retired automatically. The gate passes
           `--suppress=unmatchedSuppression` on the cppcheck command line
           (JOS/Makefile, pinned to cppcheck 2.13.0) -- that is a BUILD-LEVEL
           bookkeeping flag, NOT a per-line directive in this file. It is
           intentional, not an oversight: the multi-configuration analysis
           reports a suppression as unmatched in every configuration where the
           guarded line is preprocessed out, so without it the build would fail
           for reasons that have nothing to do with the code. The accepted cost
           is that a suppression which has become unnecessary goes quiet instead
           of loud, which makes detecting staleness a MANUAL, bump-time duty:

             on every CPPCHECK_VERSION bump in JOS/Makefile the reviewer must
             delete the two `cppcheck-suppress` lines below, re-run
             `make -C JOS cppcheck`, and restore them only if those two
             findings actually come back. The same re-verification applies to
             the four suppressions in sram2_parity.c. A bump that skips this
             has not been reviewed. Runtime re-verification requires the pinned
             cppcheck 2.13.0 (installed by CI; presence is not assumed locally). */
        /* cppcheck-suppress knownConditionTrueFalse */
        /* cppcheck-suppress unsignedLessThanZero */
        if ((have_cyccnt != 0) && ((DWT->CYCCNT - start) >= budget)) {
            return -1;
        }
        spins++;
        if (spins >= budget) {
            return -1;   /* cycle counter absent, or silenced mid-loop */
        }
    }
    return 0;
}

/* Program one 64-bit double word with a hard, cycle-counted bound. Never relies
 * on HAL_GetTick(); returns 0 on success, -1 on timeout/error. Mirrors the HAL
 * double-word program sequence (set PG, write the two words, poll BSY). */
static int flash_write_dword_bounded(uintptr_t addr, uint64_t data)
{
    /* Enable the counter here as well as in the callers: the bound must be a
     * property of THIS function, not of the call path that reached it. */
    dwt_cyccnt_enable();

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    SET_BIT(FLASH->CR, FLASH_CR_PG);
    *(__IO uint32_t *)(uintptr_t)addr       = (uint32_t)data;
    __ISB();
    *(__IO uint32_t *)(uintptr_t)(addr + 4U) = (uint32_t)(data >> 32U);

    if (flash_wait_bsy_bounded(FLASH_DWORD_TIMEOUT_CYCLES) != 0) {
        CLEAR_BIT(FLASH->CR, FLASH_CR_PG);
        return -1;
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

    /* Bounds guard (B3): only ever erase a page inside the LastStates pool.
     * Refusing anything else prevents an errant call from wiping the vector
     * table, the golden image or any other Flash region. The guard is checked
     * BEFORE the Flash controller is unlocked, so a rejected address never
     * even leaves the controller unlocked. */
    if (!flash_page_in_pool(addr)) {
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

    if (flash_wait_bsy_bounded(FLASH_PAGE_TIMEOUT_CYCLES) != 0) {
        CLEAR_BIT(FLASH->CR, (FLASH_CR_PER | FLASH_CR_PNB));
        HAL_FLASH_Lock();
        return -1;
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

/* ---------------------------------------------------------------------------
 * LastStates pool lock (W2-2 review, CRITICAL: Flash write race).
 *
 * See memory.h for the full rationale and the meaning of the three return
 * values. Short version: laststates_write() and dual_bank.c:ls_append() both
 * run "pick the first erased slot -> unlock -> program -> lock" on the SAME
 * pool, from tasks of different priority, with configUSE_PREEMPTION == 1. One
 * mutex owns that sequence in both writers.
 *
 * The mutex is created in laststates_init(), which main() calls before
 * osKernelInitialize(): osMutexNew() only refuses to run from an ISR, so
 * creating it there is legal and removes any lazy-creation race between the
 * two writers.
 * ------------------------------------------------------------------------- */

/* Degraded-path telemetry. Compiled in BOTH builds so the host tests can see
 * the refusal happen (Kilo #21: "at minimum, bump a counter that reaches the
 * LastStates/telemetry stream so ground can see the pool went unsynchronised").
 */
static volatile uint32_t ls_lock_failures   = 0U;
static volatile uint32_t ls_dropped_records = 0U;

uint32_t laststates_lock_failures(void)   { return ls_lock_failures; }
uint32_t laststates_dropped_records(void) { return ls_dropped_records; }

/* Every writer of this pool - laststates_write() here and
 * Core/Src/dual_bank.c:ls_append(), which drives HAL_FLASH_Program() itself
 * and never routes through laststates_write() - must call this when it
 * refuses a record because serialisation was unavailable. Without it the
 * dual-bank boot-fault / boot-OK markers were dropped silently and the
 * tri-state lock was invisible from the ground (Kilo #26). */
void laststates_note_dropped_record(void)
{
    ls_dropped_records++;
}

#ifndef HOST_UNIT_TEST

static osMutexId_t ls_pool_mutex = NULL;

/* Set when laststates_init() ran but osMutexNew() handed back NULL (FreeRTOS
 * heap exhausted). Distinguishes "the mutex does not exist yet, boot is still
 * single-threaded" (safe: no lock needed) from "the mutex will never exist"
 * (unsafe: two tasks can now race, so every write must be refused). */
static volatile uint8_t ls_pool_lock_degraded = 0U;

static void laststates_pool_lock_create(void)
{
    static const osMutexAttr_t attr = {
        .name      = "lsPool",
        .attr_bits = osMutexPrioInherit,   /* the writers span 3 priorities */
        .cb_mem    = NULL,
        .cb_size   = 0U,
    };

    if (ls_pool_mutex == NULL) {
        ls_pool_mutex = osMutexNew(&attr);
    }
    if (ls_pool_mutex == NULL) {
        /* Nobody checked this return before; a NULL mutex silently degraded
         * every later lock into a no-op, i.e. exactly the unsynchronised pool
         * this module exists to prevent (Kilo #21). Latch it and fail loud. */
        ls_pool_lock_degraded = 1U;
        ls_lock_failures++;
    }
}

/* Exception-context preparation for a Flash write.
 *
 * The fault / MPU / parity handlers log through laststates_write() and then
 * reset, so they can neither block on the mutex nor let the preempted task
 * resume. Returning "no lock needed" and programming anyway is not safe on its
 * own: the preempted task may be sitting between the two 32-bit stores of a
 * double-word program (PG set, BSY not yet asserted) or between PER+PNB and
 * STRT. Programming on top of either is a programming-sequence error, and the
 * record we most wanted to read is then filed under dropped_records.
 *
 * So: wait for BSY with the same cycle-counted bound the writers use (never
 * HAL_GetTick(): SysTick cannot preempt an NMI), then sanitise FLASH->CR
 * before handing the controller over. If the controller is still busy when the
 * bound expires it is wedged - refuse the write rather than queue behind an
 * operation that may never end. */
static int laststates_flash_isr_prepare(void)
{
    dwt_cyccnt_enable();

    if (flash_wait_bsy_bounded(FLASH_PAGE_TIMEOUT_CYCLES) != 0) {
        ls_lock_failures++;
        return LASTSTATES_LOCK_FAILED;
    }

    /* BSY low proves the controller is idle *now*, not that FLASH->CR is
     * clean. Clear every operation-select bit plus the page number, then the
     * error latches, so the write below starts from a known state. */
    CLEAR_BIT(FLASH->CR, (FLASH_CR_PG | FLASH_CR_FSTPG | FLASH_CR_PER |
                          FLASH_CR_MER1 | FLASH_CR_MER2 | FLASH_CR_PNB));
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    __DSB();

    return LASTSTATES_LOCK_NOT_NEEDED;
}

int laststates_pool_lock(void)
{
    if (__get_IPSR() != 0U) {
        /* fault / parity NMI path: log and reset, never block */
        return laststates_flash_isr_prepare();
    }
    /* "Is serialisation even possible?" comes BEFORE "can I serialise?".
     * main() calls laststates_init() at main.c:172 and osKernelInitialize()
     * only at main.c:212, so the whole window in between is provably
     * single-threaded - and it carries exactly the boot forensics
     * (sram2_parity_persist_boot_records(), mpu_fault_log_flush(),
     * boot_crc_apply_policy()). Testing the degraded latch first refused those
     * writes wholesale even though no mutex was needed, contradicting the
     * contract in memory.h (Kilo #26). Concurrency starts with the scheduler,
     * so the latch is only meaningful once it is running. */
    if (osKernelGetState() != osKernelRunning) {
        return LASTSTATES_LOCK_NOT_NEEDED;   /* boot is single-threaded */
    }
    if (ls_pool_lock_degraded != 0U) {
        /* The mutex could not be created: serialisation is required (two
         * tasks write this pool) and permanently unavailable. Fail safe. */
        ls_lock_failures++;
        return LASTSTATES_LOCK_FAILED;
    }
    if (ls_pool_mutex == NULL) {
        /* Scheduler running with no mutex and no degraded latch means
         * laststates_init() never ran: both writers can be scheduled, so this
         * is "required and unavailable" too - never a silent fail-open. */
        ls_lock_failures++;
        return LASTSTATES_LOCK_FAILED;
    }
    if (osMutexAcquire(ls_pool_mutex, osWaitForever) != osOK) {
        ls_lock_failures++;
        return LASTSTATES_LOCK_FAILED;
    }
    return LASTSTATES_LOCK_HELD;
}

void laststates_pool_unlock(int held)
{
    if (held == LASTSTATES_LOCK_HELD) {
        (void)osMutexRelease(ls_pool_mutex);
    }
}

#else  /* host unit-test build: single-threaded, no RTOS */

/* Forced result, so the LASTSTATES_LOCK_FAILED refusal path is reachable from
 * a test (there is no CMSIS-RTOS2 and no __get_IPSR() on the host). */
static int ls_host_lock_result = LASTSTATES_LOCK_NOT_NEEDED;

void laststates_pool_lock_set_result_for_test(int result)
{
    ls_host_lock_result = result;
}

static void laststates_pool_lock_create(void) { }
int  laststates_pool_lock(void)          { return ls_host_lock_result; }
void laststates_pool_unlock(int held)    { (void)held; }

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

    /* Create the pool mutex before anything can write: main() calls us before
     * osKernelInitialize(), and dual_bank.c's writer only starts once the
     * watchdog task runs, so the mutex always exists by the time two writers
     * can actually race (W2-2 review, CRITICAL). */
    laststates_pool_lock_create();

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
        if (slot_is_complete(i)) {
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

    /* Take the pool lock for the WHOLE select-slot / erase / program sequence:
       dual_bank.c:ls_append() runs the same sequence from a higher-priority
       task, and interleaving the two corrupts the pool (W2-2 review,
       CRITICAL). Every exit path below must release it. */
    const int lock_held = laststates_pool_lock();

    /* Serialisation required but unavailable (mutex creation or acquire
       failed, or the exception path found the Flash controller wedged): refuse
       WITHOUT touching Flash. Programming an unsynchronised pool risks a torn
       128-byte entry, which destroys forensic history that is already there -
       strictly worse than losing this one record. Count the loss so ground can
       see it instead of inferring it from a gap (Kilo #21). */
    if (lock_held == LASTSTATES_LOCK_FAILED) {
        ls_dropped_records++;
        return -1;
    }

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
            laststates_pool_unlock(lock_held);
            return -1;
        }
    }

    if (flash_write_row(addr, (const uint8_t *)entry, LASTSTATES_ENTRY_SIZE) != 0) {
        laststates_pool_unlock(lock_held);
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
    laststates_pool_unlock(lock_held);
    return 0;
}

/* Pool lock for a READER.
 *
 * Same mutual exclusion as laststates_pool_lock(), minus the exception-path
 * Flash-controller take-over: a reader programs nothing, so sanitising
 * FLASH->CR on its behalf would corrupt an in-flight write instead of
 * protecting one. From an exception there is also nothing to exclude - the
 * handler resets - so the read simply runs unlocked and relies on the hard
 * copy bound below. */
static int laststates_reader_lock(void)
{
#ifndef HOST_UNIT_TEST
    if (__get_IPSR() != 0U) {
        return LASTSTATES_LOCK_NOT_NEEDED;
    }
#endif
    return laststates_pool_lock();
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
    uint32_t valid    = 0U;

    /* Hold the pool lock across BOTH passes. laststates_write() and
     * dual_bank.c:ls_append() seal a record with a single double-word program
     * (see slot_is_complete()), so without the lock a record can become
     * complete between the counting pass and the copying pass and the memcpy
     * would run one entry past the capacity that was just checked (Kilo #26).
     * A failed lock is NOT fatal for a reader - it cannot corrupt Flash - so
     * the dump proceeds, and the `valid < needed` bound below keeps it inside
     * the caller's buffer in every case. */
    const int lock_held = laststates_reader_lock();

    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (slot_is_complete(i)) {
            needed++;
        }
    }
    if ((size_t)needed * LASTSTATES_ENTRY_SIZE > capacity) {
        *len = (size_t)needed * LASTSTATES_ENTRY_SIZE;   /* required, nothing copied */
        laststates_pool_unlock(lock_held);
        return -1;
    }

    /* Scan the whole pool: because page recycling can leave gaps, valid
     * records are not necessarily a contiguous prefix. Copy every valid slot
     * in index order so ground can reconstruct the trail by timestamp. */
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (valid >= needed) {
            break;      /* hard bound: never copy past the checked capacity */
        }
        if (slot_is_complete(i)) {
            memcpy(out + valid * LASTSTATES_ENTRY_SIZE,
                   (const uint8_t *)(LASTSTATES_FLASH_BASE + (uintptr_t)i * LASTSTATES_ENTRY_SIZE),
                   LASTSTATES_ENTRY_SIZE);
            valid++;
        }
    }
    *len = valid * LASTSTATES_ENTRY_SIZE;
    laststates_pool_unlock(lock_held);
    return 0;
}

uint32_t laststates_count(void)
{
    /* Authoritative count comes from Flash, not from the cached mirror: page
       recycling leaves gaps and a cached counter can itself be upset. */
    uint32_t valid = 0U;
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (slot_is_complete(i)) {
            valid++;
        }
    }
    return valid;
}
