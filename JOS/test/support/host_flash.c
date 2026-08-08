/* ---------------------------------------------------------------------------
 * host_flash.c - host emulation of the STM32L4 internal Flash LastStates pool
 *                and of the FM24VN10-G FRAM bank behind hi2c2.
 *
 * Why a memory mapping and not a plain array:
 *
 *   App/memory/memory.c addresses the LastStates pool by absolute address.
 *   laststates_dump_all() does, literally,
 *
 *       memcpy(out, (const uint8_t *)LASTSTATES_FLASH_BASE, *len);
 *
 *   with LASTSTATES_FLASH_BASE == 0x08080000. No amount of link-time
 *   substitution redirects that; the only faithful way to test the module
 *   unmodified is to put real, writable pages at 0x08080000 in the test
 *   process. mmap(MAP_FIXED_NOREPLACE) does exactly that and, unlike plain
 *   MAP_FIXED, refuses instead of silently unmapping something else.
 *
 * Flash semantics that are reproduced, because memory.c depends on them:
 *
 *   - erased state is all-ones (0xFF);
 *   - programming is 64-bit aligned and 64-bit wide (FLASH_TYPEPROGRAM_DOUBLEWORD);
 *   - a double-word may only be programmed once per erase cycle: programming
 *     over a non-erased row fails (the real part raises PROGERR). This is what
 *     makes the erase-before-wrap path in laststates_write() observable.
 *   - programming *and* page erase both require the Flash to be unlocked:
 *     PG and PER/STRT live in FLASH_CR, which is write-protected until the
 *     KEY1/KEY2 sequence has been written (RM0351 3.3.5). The double enforces
 *     the same precondition on both operations, so a missing HAL_FLASH_Unlock()
 *     around either one shows up as a test failure instead of passing silently.
 *
 * Refs: RM0351 3.3.7 (Flash program/erase), ECSS-E-ST-40C 5.5.
 * ------------------------------------------------------------------------- */
#include "host_support.h"
#include "main.h"        /* fakes/main.h */
#include "obsw_types.h"  /* LASTSTATES_MAX_ENTRIES / LASTSTATES_ENTRY_SIZE */
#include "unity.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define FLASH_BASE_ADDR    0x08000000UL
#define POOL_BASE          HOST_FLASH_LASTSTATES_BASE
#define POOL_SIZE          ((size_t)LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)
#define ERASED_DWORD       0xFFFFFFFFFFFFFFFFULL

/* ---------- emulated devices ---------- */

/* memory.c declares `extern I2C_HandleTypeDef hi2c2;`; provide the object. */
I2C_HandleTypeDef hi2c2;

static uint8_t *pool;                       /* mapped at POOL_BASE */
static uint8_t  fram[4 * 16 * 1024];        /* 4 x FM24VN10-G = 64 KB */

static int      flash_unlocked;
static uint32_t erase_count;
static uint32_t program_count;
static uint32_t unlock_count;
static uint32_t lock_count;
static uint16_t last_i2c_dev_addr;

/* ---------- fixture control ---------- */

static void host_flash_map_once(void)
{
    void *addr;

    if (pool != NULL) {
        return;
    }

    addr = mmap((void *)POOL_BASE, POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (addr == MAP_FAILED || addr != (void *)POOL_BASE) {
        TEST_FAIL_MESSAGE("host_flash: could not map the LastStates pool at "
                          "0x08080000 (address already in use, or "
                          "vm.mmap_min_addr too high)");
        return; /* not reached; TEST_FAIL_MESSAGE long-jumps out */
    }

    pool = (uint8_t *)addr;
}

void host_flash_reset(void)
{
    host_flash_map_once();

    memset(pool, 0xFF, POOL_SIZE);   /* erased Flash */
    memset(fram, 0x00, sizeof(fram));

    flash_unlocked = 0;
    erase_count    = 0u;
    program_count  = 0u;
    unlock_count   = 0u;
    lock_count     = 0u;

    last_i2c_dev_addr = 0xFFFFu;
}

const uint8_t *host_flash_pool(void)      { return pool; }
size_t         host_flash_pool_size(void) { return POOL_SIZE; }
uint32_t host_flash_erase_count(void)     { return erase_count; }
uint32_t host_flash_program_count(void)   { return program_count; }
uint32_t host_flash_unlock_count(void)    { return unlock_count; }
uint32_t host_flash_lock_count(void)      { return lock_count; }
int      host_flash_is_unlocked(void)     { return flash_unlocked; }
uint16_t host_flash_last_i2c_addr(void)   { return last_i2c_dev_addr; }

/* ---------- HAL Flash ---------- */

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    unlock_count++;
    flash_unlocked = 1;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    lock_count++;
    flash_unlocked = 0;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    size_t   offset;
    uint64_t current;

    if (pool == NULL || !flash_unlocked) {
        return HAL_ERROR;                     /* CR locked */
    }
    if (TypeProgram != FLASH_TYPEPROGRAM_DOUBLEWORD) {
        return HAL_ERROR;
    }
    if ((Address % 8U) != 0U) {
        return HAL_ERROR;                     /* PGAERR: misaligned */
    }
    if (Address < POOL_BASE || (Address + 8U) > (POOL_BASE + POOL_SIZE)) {
        return HAL_ERROR;                     /* outside the emulated pool */
    }

    offset = (size_t)(Address - POOL_BASE);
    memcpy(&current, pool + offset, sizeof(current));
    if (current != ERASED_DWORD) {
        return HAL_ERROR;                     /* PROGERR: row not erased */
    }

    memcpy(pool + offset, &Data, sizeof(Data));
    program_count++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    uint32_t i;

    if (pEraseInit == NULL || PageError == NULL || pool == NULL) {
        return HAL_ERROR;
    }
    if (!flash_unlocked) {
        return HAL_ERROR;                     /* CR locked: PER/STRT are ignored */
    }
    if (pEraseInit->TypeErase != FLASH_TYPEERASE_PAGES) {
        return HAL_ERROR;
    }

    for (i = 0U; i < pEraseInit->NbPages; i++) {
        uint32_t page_addr = FLASH_BASE_ADDR +
                             (pEraseInit->Page + i) * HOST_FLASH_PAGE_SIZE;

        if (page_addr < POOL_BASE ||
            (page_addr + HOST_FLASH_PAGE_SIZE) > (POOL_BASE + POOL_SIZE)) {
            *PageError = page_addr;           /* outside the emulated pool */
            return HAL_ERROR;
        }
        memset(pool + (page_addr - POOL_BASE), 0xFF, HOST_FLASH_PAGE_SIZE);
    }

    *PageError = 0xFFFFFFFFU;                 /* HAL's "no failing page" value */
    erase_count++;
    return HAL_OK;
}

/* ---------- HAL I2C (FRAM) ---------- */

/* ---------------------------------------------------------------------------
 * FM24VN10-G device addressing.
 *
 * The device select byte is 1 0 1 0 A2 A1 A0 R/W, i.e. 7-bit addresses
 * 0x50..0x53 for the four chips on the OBC. Every STM32 HAL I2C entry point
 * takes the address *already shifted left by one* (the 8-bit form), so the
 * legal values arriving here are 0xA0, 0xA2, 0xA4 and 0xA6 with the R/W bit
 * clear.
 *
 * The unshifted 7-bit values are rejected on purpose: passing 0x50 to the real
 * HAL puts 0x28 on the bus and talks to nothing (or to the wrong device).
 * Accepting both forms here would hide exactly that class of defect.
 * ------------------------------------------------------------------------- */
#define FRAM_I2C_ADDR_FIRST  0xA0u          /* 7-bit 0x50 << 1 */
#define FRAM_I2C_ADDR_LAST   0xA6u          /* 7-bit 0x53 << 1 */
#define FRAM_CHIP_SIZE       (16U * 1024U)

static int fram_index(uint16_t dev_addr, uint16_t mem_addr, uint16_t size, size_t *out)
{
    size_t chip;

    last_i2c_dev_addr = dev_addr;

    if ((dev_addr & 0x01u) != 0u) {
        return -1;                            /* R/W bit must be clear */
    }
    if (dev_addr < FRAM_I2C_ADDR_FIRST || dev_addr > FRAM_I2C_ADDR_LAST) {
        return -1;
    }

    chip = (size_t)((dev_addr - FRAM_I2C_ADDR_FIRST) >> 1);

    if (((size_t)mem_addr + size) > FRAM_CHIP_SIZE) {
        return -1;                            /* would cross a chip boundary */
    }

    *out = (chip * FRAM_CHIP_SIZE) + mem_addr;
    return 0;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize,
                                   uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    size_t index;

    (void)Timeout;
    if (hi2c == NULL || pData == NULL || MemAddSize != I2C_MEMADD_SIZE_16BIT) {
        return HAL_ERROR;
    }
    if (fram_index(DevAddress, MemAddress, Size, &index) != 0) {
        return HAL_ERROR;
    }

    memcpy(pData, &fram[index], Size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    size_t index;

    (void)Timeout;
    if (hi2c == NULL || pData == NULL || MemAddSize != I2C_MEMADD_SIZE_16BIT) {
        return HAL_ERROR;
    }
    if (fram_index(DevAddress, MemAddress, Size, &index) != 0) {
        return HAL_ERROR;
    }

    memcpy(&fram[index], pData, Size);
    return HAL_OK;
}
