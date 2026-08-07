#include "memory.h"
#include "main.h"
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
 * FM24VN_CHIP_SIZE (16 B) is a power of two, so the address decode uses
 * shift/mask instead of division/modulo. This removes the only runtime
 * divisions in the memory module and therefore the divide-by-zero class
 * entirely (review M1).
 */

#define FM24VN_I2C_ADDR_BASE  0x50
#define FM24VN_PAGE_SIZE      16
#define FM24VN_CHIP_SIZE      16
#define FM24VN_CHIP_SIZE_LOG2 4U     /* 16 B = 2^4 */
#define FM24VN_NUM_CHIPS      4
#define FRAM_SIZE             (FM24VN_NUM_CHIPS * FM24VN_CHIP_SIZE)

/* Compile-time guard: a zero (or non-power-of-two) chip size would make the
 * shift/mask decode below wrong and is the divide-by-zero class M1 guards
 * against. */
_Static_assert(FM24VN_CHIP_SIZE > 0U, "FM24VN_CHIP_SIZE must be > 0");
_Static_assert((FM24VN_CHIP_SIZE & (FM24VN_CHIP_SIZE - 1U)) == 0U,
               "FM24VN_CHIP_SIZE must be a power of two");

extern I2C_HandleTypeDef hi2c2;

static uint8_t fram_addr_to_chip(uint32_t addr)
{
    return (uint8_t)((addr >> FM24VN_CHIP_SIZE_LOG2) + FM24VN_I2C_ADDR_BASE);
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

    uint8_t dev_addr = fram_addr_to_chip(addr);
    uint16_t offset = fram_addr_to_offset(addr);

    if (HAL_I2C_Mem_Read(&hi2c2, dev_addr, offset, I2C_MEMADD_SIZE_16BIT,
                         buf, (uint16_t)len, 1000) != HAL_OK)
        return -1;
    return 0;
}

int fram_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (addr + len > FRAM_SIZE) return -1;

    uint8_t dev_addr = fram_addr_to_chip(addr);
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
 * Reviewed-and-fixed pass: C1 (readback scan) + C2 (DWT bounded wait) + M1 (div0 guard) applied.
 *
 * Because a subsequent write may land in a page whose other slots were just
 * erased, the pool is not always a contiguous run of valid records. Post-mortem
 * readback (laststates_dump_all / laststates_count) therefore scans the WHOLE
 * pool for valid slots rather than trusting a cached count.
 */

#define LASTSTATES_FLASH_BASE  0x08080000U
#define LASTSTATES_FLASH_END   (LASTSTATES_FLASH_BASE + LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)
#define LS_PAGE_SHIFT          11U   /* STM32L4 Flash page = 2 KB = 2^11 (HAL FLASH_PAGE_SIZE) */

/* A freshly-erased Flash double-word reads as all ones; a programmed slot is
 * never all-ones (the trigger byte is always 0..9), so testing the first
 * double-word reliably distinguishes a free slot from a valid one. */
#define SLOT_ERASED_DWORD      0xFFFFFFFFFFFFFFFFULL

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

static uint32_t ls_idx = 0;  /* next write index (circular) */

/* ---------- DWT cycle counter (bounded Flash waits in the fault path) ----------
 * The fault handlers run with interrupts masked, so HAL_GetTick() never
 * advances there and a GetTick()-based Flash timeout would block forever if
 * the controller stuck. Every Flash operation below is instead bounded by the
 * DWT cycle counter, which is CPU-clock based and always runs, so the handler
 * can never block indefinitely (review C2).
 */
#define FLASH_DWORD_TIMEOUT_CYCLES   0x20000U   /* ~1.6 ms @ 80 MHz, >> 100 us nominal */
#define FLASH_PAGE_TIMEOUT_CYCLES    0x400000U  /* ~52 ms @ 80 MHz, >> 22 ms nominal   */

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

static int slot_is_erased(uint32_t idx)
{
    const uint64_t *d = (const uint64_t *)(LASTSTATES_FLASH_BASE + idx * LASTSTATES_ENTRY_SIZE);
    return (*d == SLOT_ERASED_DWORD) ? 1 : 0;
}

/* Program one 64-bit double word with a hard, cycle-counted bound. Never relies
 * on HAL_GetTick(); returns 0 on success, -1 on timeout/error. Mirrors the HAL
 * double-word program sequence (set PG, write the two words, poll BSY). */
static int flash_write_dword_bounded(uint32_t addr, uint64_t data)
{
    uint32_t start = DWT->CYCCNT;

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    SET_BIT(FLASH->CR, FLASH_CR_PG);
    *(__IO uint32_t *)addr       = (uint32_t)data;
    __ISB();
    *(__IO uint32_t *)(addr + 4U) = (uint32_t)(data >> 32U);

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
static int flash_erase_page_bounded(uint32_t addr)
{
    uint32_t start = DWT->CYCCNT;

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

static int flash_write_row(uint32_t addr, const uint8_t *data, size_t len)
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

void laststates_init(void)
{
    dwt_cyccnt_enable();

    /* Scan the pool for the first free (erased) slot. That slot is where the
     * next record must be appended so post-mortem readback can recover the
     * existing trail after the reboot (review C1). If the pool is completely
     * full we wrap to index 0 and the next write will recycle the oldest page.
     */
    uint32_t idx = 0U;
    while (idx < LASTSTATES_MAX_ENTRIES) {
        if (slot_is_erased(idx)) {
            break;
        }
        idx++;
    }
    ls_idx = idx;
}

int laststates_write(const laststates_entry_t *entry)
{
    if (entry == NULL) return -1;

    /* Bounds guard: ls_idx is always in range after init, but never trust a
       cached index against corruption. */
    if (ls_idx >= LASTSTATES_MAX_ENTRIES) {
        ls_idx = 0U;
    }

    uint32_t addr = LASTSTATES_FLASH_BASE + ls_idx * LASTSTATES_ENTRY_SIZE;

    /* If the target slot still holds a valid (programmed) record, the ring has
     * wrapped: recycle the OLDEST page it belongs to. Erasing only that page
     * preserves every newer page, so the newest valid record is never lost
       (review C1). */
    if (!slot_is_erased(ls_idx)) {
        uint32_t page_addr = addr & ~(((uint32_t)1U << LS_PAGE_SHIFT) - 1U);
        if (flash_erase_page_bounded(page_addr) != 0) {
            return -1;
        }
    }

    if (flash_write_row(addr, (const uint8_t *)entry, LASTSTATES_ENTRY_SIZE) != 0) {
        return -1;
    }

    ls_idx = (ls_idx + 1U) & (LASTSTATES_MAX_ENTRIES - 1U);
    return 0;
}

int laststates_dump_all(uint8_t *out, size_t *len)
{
    if (out == NULL || len == NULL) return -1;

    /* Scan the whole pool: because page recycling can leave gaps, valid
     * records are not necessarily a contiguous prefix. Copy every valid slot
     * in index order so ground can reconstruct the trail by timestamp. */
    uint32_t valid = 0U;
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            memcpy(out + valid * LASTSTATES_ENTRY_SIZE,
                   (const uint8_t *)(LASTSTATES_FLASH_BASE + i * LASTSTATES_ENTRY_SIZE),
                   LASTSTATES_ENTRY_SIZE);
            valid++;
        }
    }
    *len = valid * LASTSTATES_ENTRY_SIZE;
    return 0;
}

uint32_t laststates_count(void)
{
    uint32_t valid = 0U;
    for (uint32_t i = 0U; i < LASTSTATES_MAX_ENTRIES; i++) {
        if (!slot_is_erased(i)) {
            valid++;
        }
    }
    return valid;
}
