#include "memory.h"
#include "main.h"
#include <string.h>

/* ========== FRAM driver (I2C) ========== */
/* Per RED_DES_ElectronicArchitecture_V1:
 *   4x FM24VN10-G (1 Mbit each, I2C interface) = 4 Mbit total.
 *   All four FM24VN10-G devices are on the OBC PCB.
 *
 * I2C addresses: 0x50, 0x51, 0x52, 0x53 (A0/A1 pins select chip).
 * Each chip: 128 Kbit = 16 KB address space.
 * Total: 4 x 16 KB = 64 KB.
 */

#define FM24VN_I2C_ADDR_BASE  0x50
#define FM24VN_PAGE_SIZE      16
#define FM24VN_CHIP_SIZE      (16UL * 1024)
#define FM24VN_NUM_CHIPS      4
#define FRAM_SIZE             (FM24VN_NUM_CHIPS * FM24VN_CHIP_SIZE)

extern I2C_HandleTypeDef hi2c2;

static uint8_t fram_addr_to_chip(uint32_t addr)
{
    return (uint8_t)((addr / FM24VN_CHIP_SIZE) + FM24VN_I2C_ADDR_BASE);
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

static int flash_write_row(uint32_t addr, const uint8_t *data, size_t len)
{
    /* STM32L4 Flash programming: unlock, write double-word (8 bytes), lock */
    /* TODO: implement with HAL_FLASH_Unlock/Program/Lock */
    (void)addr;
    (void)data;
    (void)len;
    return 0;
}

int laststates_write(const laststates_entry_t *entry)
{
    if (!entry) return -1;

    uint32_t addr = LASTSTATES_FLASH_BASE + ls_idx * LASTSTATES_ENTRY_SIZE;
    int rc = flash_write_row(addr, (const uint8_t *)entry, LASTSTATES_ENTRY_SIZE);
    if (rc != 0) return rc;

    ls_idx = (ls_idx + 1) % LASTSTATES_MAX_ENTRIES;
    if (ls_count < LASTSTATES_MAX_ENTRIES) ls_count++;
    return 0;
}

int laststates_dump_all(uint8_t *out, size_t *len)
{
    if (!out || !len) return -1;
    *len = ls_count * LASTSTATES_ENTRY_SIZE;
    memcpy(out, (const uint8_t *)LASTSTATES_FLASH_BASE, *len);
    return 0;
}

uint32_t laststates_count(void)
{
    return ls_count;
}
