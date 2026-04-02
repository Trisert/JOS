#include "memory.h"
#include "main.h"
#include <string.h>

/* ========== FRAM driver (SPI2) ========== */
/* MB85RS4MT: 4 Mbit (512 KB) FRAM, SPI interface */

#define FRAM_WREN   0x06   /* Write Enable */
#define FRAM_WRDI   0x04   /* Write Disable */
#define FRAM_READ   0x03   /* Read */
#define FRAM_WRITE  0x02   /* Write */
#define FRAM_RDID   0x9F   /* Read Device ID */

extern SPI_HandleTypeDef hspi2;

/* FRAM CS pin — adjust to actual GPIO */
#define FRAM_CS_GPIO GPIOB
#define FRAM_CS_PIN  GPIO_PIN_12

static void fram_cs_low(void)  { HAL_GPIO_WritePin(FRAM_CS_GPIO, FRAM_CS_PIN, GPIO_PIN_RESET); }
static void fram_cs_high(void) { HAL_GPIO_WritePin(FRAM_CS_GPIO, FRAM_CS_PIN, GPIO_PIN_SET); }

void fram_init(void)
{
    /* TODO: verify device ID via RDID command */
    fram_cs_high();
}

int fram_read(uint32_t addr, uint8_t *buf, size_t len)
{
    uint8_t cmd[4] = {
        FRAM_READ,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >>  8) & 0xFF),
        (uint8_t)( addr        & 0xFF),
    };

    fram_cs_low();
    /* TODO: use DMA (HAL_SPI_TransmitReceive_DMA) instead of blocking */
    HAL_SPI_Transmit(&hspi2, cmd, 4, 100);
    HAL_SPI_Receive(&hspi2, buf, (uint16_t)len, 1000);
    fram_cs_high();
    return 0;
}

int fram_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    uint8_t cmd[4] = {
        FRAM_WRITE,
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >>  8) & 0xFF),
        (uint8_t)( addr        & 0xFF),
    };

    /* Write enable */
    fram_cs_low();
    uint8_t wren = FRAM_WREN;
    HAL_SPI_Transmit(&hspi2, &wren, 1, 100);
    fram_cs_high();

    fram_cs_low();
    HAL_SPI_Transmit(&hspi2, cmd, 4, 100);
    HAL_SPI_Transmit(&hspi2, (uint8_t *)buf, (uint16_t)len, 1000);
    fram_cs_high();
    return 0;
}

/* ========== Cyclic buffer ========== */
/* 4 MB FRAM used as circular buffer */
#define FRAM_SIZE (4UL * 1024 * 1024)

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
