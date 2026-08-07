#include "memory.h"
#include "main.h"
#include <string.h>
#include <stdint.h>

/* ========== FRAM driver (I2C) ========== */
/* Per RED_DES_ElectronicArchitecture_V1:
 *   4x FM24VN10-G (1 Mbit each, I2C interface) = 4 Mbit total.
 *   All four FM24VN10-G devices are on the OBC PCB.
 *
 * Device select byte: 1 0 1 0 A2 A1 A0 R/W, i.e. 7-bit addresses
 * 0x50, 0x51, 0x52, 0x53 (A0/A1 pins select the chip).
 *
 * The STM32 HAL I2C API takes the device address *already shifted left by
 * one* (the 8-bit form including the R/W bit position), so the values handed
 * to HAL_I2C_Mem_Read/Write must be 0xA0, 0xA2, 0xA4, 0xA6 -- not the raw
 * 7-bit ones. Passing 0x50 puts 0x28 on the bus and addresses nothing.
 *
 * Each chip: 128 Kbit = 16 KB address space.
 * Total: 4 x 16 KB = 64 KB.
 */

#define FM24VN_I2C_ADDR_7BIT  0x50            /* A2..A0 = 000 for the first chip */
#define FM24VN_I2C_ADDR_BASE  (FM24VN_I2C_ADDR_7BIT << 1)   /* 0xA0, HAL 8-bit form */
#define FM24VN_I2C_ADDR_STEP  (1u << 1)       /* one 7-bit address per chip */
#define FM24VN_PAGE_SIZE      16
#define FM24VN_CHIP_SIZE      (16UL * 1024)
#define FM24VN_NUM_CHIPS      4
#define FRAM_SIZE             (FM24VN_NUM_CHIPS * FM24VN_CHIP_SIZE)

extern I2C_HandleTypeDef hi2c2;

static uint8_t fram_addr_to_chip(uint32_t addr)
{
    return (uint8_t)(FM24VN_I2C_ADDR_BASE +
                     (addr / FM24VN_CHIP_SIZE) * FM24VN_I2C_ADDR_STEP);
}

static uint16_t fram_addr_to_offset(uint32_t addr)
{
    return (uint16_t)(addr % FM24VN_CHIP_SIZE);
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
/* 64 entries x 128 B = 8 KB in internal Flash, starting at 0x08080000 */

#define LASTSTATES_FLASH_BASE  0x08080000U
#define LASTSTATES_FLASH_END   (LASTSTATES_FLASH_BASE + LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)

static uint32_t ls_count = 0;  /* number of entries written */
static uint32_t ls_idx   = 0;  /* next write index (circular) */

void laststates_init(void)
{
    /* TODO: scan Flash to find how many entries are valid */
    ls_count = 0;
    ls_idx = 0;
}

/* Write one 64-bit double-word to internal Flash at the given address.
   Caller must ensure the target sector was erased and FLASH is unlocked. */
static HAL_StatusTypeDef flash_write_dword(uint32_t addr, uint64_t data)
{
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, data);
}

/* Erase the Flash pages that contain the LastStates pool.
   STM32L4 internal Flash is erased per 2 KB page (not per sector). A
   double-word can only be programmed once per bit until the page is erased
   again, so the pool (8 KB = 4 pages) must be erased before reuse. */
static int flash_erase_pool(void)
{
    /* STM32L4 bank-1 pages are 2 KB; LastStates pool is 8 KB @ 0x08080000. */
    uint32_t first_page = (LASTSTATES_FLASH_BASE - 0x08000000U) / (2U * 1024U);
    uint32_t page_count  = LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE / (2U * 1024U);

    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0U;

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks     = FLASH_BANK_1;
    erase_init.Page      = first_page;
    erase_init.NbPages   = page_count;

    HAL_StatusTypeDef rc = HAL_OK;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    rc = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();

    return (rc == HAL_OK) ? 0 : -1;
}

static int flash_write_row(uint32_t addr, const uint8_t *data, size_t len)
{
    if (len == 0 || (len % 8U) != 0) return -1;
    if (addr < LASTSTATES_FLASH_BASE || (addr + len) > LASTSTATES_FLASH_END) return -1;

    HAL_StatusTypeDef rc = HAL_OK;
    uint32_t off = 0;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    while (off < len) {
        uint64_t dword = 0;
        memcpy(&dword, data + off, 8);
        rc = flash_write_dword(addr + off, dword);
        if (rc != HAL_OK) {
            break;
        }
        off += 8;
    }
    HAL_FLASH_Lock();

    return (rc == HAL_OK) ? 0 : -1;
}

int laststates_write(const laststates_entry_t *entry)
{
    if (!entry) return -1;

    /* When the pool is full and we are about to wrap to index 0, the target
       sector still holds the oldest (now overwritten) entry. STM32L4 Flash
       must be erased per sector before it can be reprogrammed, so erase the
       LastStates sector first. */
    if (ls_idx == 0 && ls_count >= LASTSTATES_MAX_ENTRIES) {
        if (flash_erase_pool() != 0) {
            return -1;
        }
    }

    uint32_t addr = LASTSTATES_FLASH_BASE + ls_idx * LASTSTATES_ENTRY_SIZE;
    int rc = flash_write_row(addr, (const uint8_t *)entry, LASTSTATES_ENTRY_SIZE);
    if (rc != 0) return rc;

    ls_idx = (ls_idx + 1) % LASTSTATES_MAX_ENTRIES;
    if (ls_count < LASTSTATES_MAX_ENTRIES) ls_count++;
    return 0;
}

/* Serialise the whole pool into the caller's buffer.
 *
 * *len is IN/OUT: on entry it is the capacity of `out` in bytes, on a
 * successful return it holds the number of bytes written. The pool can hold
 * LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE = 8 KB, so a caller that
 * sized `out` for a couple of entries must not be handed the whole pool:
 * a too-small buffer is refused (-1) with *len set to the size required. */
int laststates_dump_all(uint8_t *out, size_t *len)
{
    size_t needed;

    if (!out || !len) return -1;

    needed = (size_t)ls_count * LASTSTATES_ENTRY_SIZE;

    if (*len < needed) {
        *len = needed;          /* tell the caller how much it would take */
        return -1;
    }

    memcpy(out, (const uint8_t *)LASTSTATES_FLASH_BASE, needed);
    *len = needed;
    return 0;
}

uint32_t laststates_count(void)
{
    return ls_count;
}
