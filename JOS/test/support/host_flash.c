/* ---------------------------------------------------------------------------
 * host_flash.c - host emulation of the STM32L4 internal Flash LastStates pool
 *                and of the FM24VN10-G FRAM bank behind hspi2 (SPI2).
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

/* Flash base address, relocatable for the host test build. host_flash.c
 * owns the definition and points it at a malloc'd buffer when mmap at the
 * real address 0x08080000 is unavailable (CI runners, vm.mmap_min_addr). */
uintptr_t flash_base = HOST_FLASH_LASTSTATES_BASE;

/* ---------- emulated devices ---------- */

/* memory.c declares `extern SPI_HandleTypeDef hspi2;`; provide the object. */
SPI_HandleTypeDef hspi2;

/* Port objects for the FRAM chip-selects (fakes/main.h declares them). */
GPIO_TypeDef gpioa_obj = { 0 };
GPIO_TypeDef gpiob_obj = { 1 };
GPIO_TypeDef gpioc_obj = { 2 };

static uint8_t *pool;                       /* mapped at POOL_BASE */
static uint8_t  fram[4 * 16 * 1024];        /* 4 x FM24VN10-G = 64 KB */

static int      flash_unlocked;
static uint32_t erase_count;
static uint32_t program_count;
static uint32_t unlock_count;
static uint32_t lock_count;
static uint32_t last_spi_chip;

/* Per-CS-assertion SPI transaction state (see the FRAM double below). */
#define FRAM_SPI_OP_WREN   0x06u
#define FRAM_SPI_OP_WRITE  0x02u
#define FRAM_SPI_OP_READ   0x03u
#define FRAM_CHIP_SIZE     (16U * 1024U)
#define FRAM_NO_CHIP       0xFFu
static uint8_t  spi_selected = FRAM_NO_CHIP;   /* chip under CS, or FRAM_NO_CHIP */
static uint8_t  spi_op;
static uint16_t spi_addr;
static uint16_t spi_data_len;
static int      spi_hdr_done;
static int      spi_wel[4];

/* Injected program failure (see host_flash_fail_program_after). */
static int      fail_program_armed;
static uint32_t fail_program_countdown;

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
        /* CI runners often forbid fixed maps at 0x08080000 (vm.mmap_min_addr
         * or the address is reserved). Fall back to an anonymous mapping and
         * relocate the pool base so memory.c still finds it. The emulation
         * semantics (erased = 0xFF, program-once, unlock-gated) are unchanged. */
        addr = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (addr == MAP_FAILED) {
            TEST_FAIL_MESSAGE("host_flash: could not allocate the LastStates pool");
            return;
        }
        flash_base = (uintptr_t)addr;
    }

    pool = (uint8_t *)addr;
    memset(pool, 0xFF, POOL_SIZE);          /* erased Flash */
}

/* Map the pool before main() runs. boot_crc.c reaches laststates_write() from
 * boot_crc_apply_policy(), i.e. from test files whose setUp() never calls
 * host_flash_reset(); without this constructor those tests would dereference
 * an unmapped 0x08080000 and segfault instead of failing. */
__attribute__((constructor))
static void host_flash_ctor(void)
{
    host_flash_map_once();
}

/* Byte offset of `addr` inside the emulated pool.
 *
 * The HAL takes a uint32_t address, but on a 64-bit host the pool may sit
 * anywhere (see the fallback above), so the caller's address is the low 32
 * bits of the real one. Subtracting the equally-truncated base in uint32
 * arithmetic is exact modulo 2^32 and therefore recovers the true offset for
 * any pool smaller than 4 GB. */
static uint32_t pool_offset(uint32_t addr)
{
    return addr - (uint32_t)flash_base;
}

/* Absolute Flash page index of the first page of the pool, computed exactly
 * the way memory.c and the tests compute it. */
static uint32_t pool_first_page(void)
{
    return (uint32_t)((flash_base - FLASH_BASE_ADDR) / HOST_FLASH_PAGE_SIZE);
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

    fail_program_armed     = 0;
    fail_program_countdown = 0u;

    last_spi_chip = 0xFFFFFFFFu;

    /* No transaction may span a reset: park every CS and clear the latches. */
    spi_selected = FRAM_NO_CHIP;
    spi_op       = 0u;
    spi_addr     = 0u;
    spi_data_len = 0u;
    spi_hdr_done = 0;
    spi_wel[0] = spi_wel[1] = spi_wel[2] = spi_wel[3] = 0;
}

const uint8_t *host_flash_pool(void)      { return pool; }
size_t         host_flash_pool_size(void) { return POOL_SIZE; }
uint32_t host_flash_erase_count(void)     { return erase_count; }
uint32_t host_flash_program_count(void)   { return program_count; }
uint32_t host_flash_unlock_count(void)    { return unlock_count; }
uint32_t host_flash_lock_count(void)      { return lock_count; }
int      host_flash_is_unlocked(void)     { return flash_unlocked; }
uint32_t host_flash_last_spi_chip(void)  { return last_spi_chip; }

void host_flash_fail_program_after(uint32_t successes)
{
    fail_program_armed     = 1;
    fail_program_countdown = successes;
}

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
    uint32_t offset;
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

    offset = pool_offset(Address);
    if (offset > (uint32_t)(POOL_SIZE - 8U)) {
        return HAL_ERROR;                     /* outside the emulated pool */
    }

    memcpy(&current, pool + offset, sizeof(current));
    if (current != ERASED_DWORD) {
        return HAL_ERROR;                     /* PROGERR: row not erased */
    }

    /* Injected controller failure (host_flash_fail_program_after): the real
     * part can raise PROGERR on a perfectly erased row - a worn or damaged
     * cell, or a supply glitch mid-program. There is no other way to reach
     * that path from a test once the module refuses to write into slots that
     * are not fully erased.
     *
     * This block sits BELOW the TypeProgram / alignment / bounds / erased
     * checks on purpose: host_support.h promises "after <successes> successful
     * programs the next one returns HAL_ERROR", so only a program the model
     * would genuinely have carried out may consume the countdown. Decrementing
     * above the checks counted rejected attempts as successes and fired the
     * injection early, blaming the wrong write (Kilo #26). */
    if (fail_program_armed) {
        if (fail_program_countdown == 0U) {
            fail_program_armed = 0;
            return HAL_ERROR;
        }
        fail_program_countdown--;
    }

    memcpy(pool + offset, &Data, sizeof(Data));
    program_count++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    uint32_t i;
    uint32_t first_page;

    if (pEraseInit == NULL || PageError == NULL || pool == NULL) {
        return HAL_ERROR;
    }
    if (!flash_unlocked) {
        return HAL_ERROR;                     /* CR locked: PER/STRT are ignored */
    }
    if (pEraseInit->TypeErase != FLASH_TYPEERASE_PAGES) {
        return HAL_ERROR;
    }

    first_page = pool_first_page();

    for (i = 0U; i < pEraseInit->NbPages; i++) {
        uint32_t page   = pEraseInit->Page + i;
        uint32_t offset = (page - first_page) * HOST_FLASH_PAGE_SIZE;

        if ((page - first_page) >= (uint32_t)(POOL_SIZE / HOST_FLASH_PAGE_SIZE)) {
            /* outside the emulated pool (unsigned wrap covers page < first) */
            *PageError = page;
            return HAL_ERROR;
        }
        memset(pool + offset, 0xFF, HOST_FLASH_PAGE_SIZE);
    }

    *PageError = 0xFFFFFFFFU;                 /* HAL's "no failing page" value */
    erase_count++;
    return HAL_OK;
}

/* ---------- GPIO + HAL SPI (FRAM) ---------- */

/* ---------------------------------------------------------------------------
 * FM24VN10-G SPI framing.
 *
 * One GPIO chip-select per die (see fakes/main.h FRAM_CSx_*): CS0 = PA4,
 * CS1 = PB0, CS2 = PB1, CS3 = PC13. A transaction is everything between CS
 * fall and CS rise:
 *
 *   WREN : 0x06 alone                                           -> latches WEL
 *   WRITE: 0x02 + addr_hi + addr_lo + data bytes                -> needs WEL,
 *            which the part clears when CS rises again
 *   READ : 0x03 + addr_hi + addr_lo, then HAL_SPI_Receive clocks data out
 *
 * Strictness is deliberate, mirroring the old I2C double (which rejected raw
 * 7-bit addresses so a shifted-address defect could not hide):
 *   - a transfer with no CS asserted fails;
 *   - a WRITE without a preceding WREN fails (the part ignores it);
 *   - a single transaction addressing past the end of its 16 KB die fails
 *     (each FM24VN10-G owns only its 16 KB window; the flight driver must
 *     split at chip boundaries, exactly as the cyclic layer does).
 * ------------------------------------------------------------------------- */

static int fram_cs_decode(GPIO_TypeDef *port, uint16_t pin, uint8_t *chip)
{
    if (port == FRAM_CS0_GPIO_Port && pin == FRAM_CS0_Pin)      { *chip = 0u; return 0; }
    if (port == FRAM_CS1_GPIO_Port && pin == FRAM_CS1_Pin)      { *chip = 1u; return 0; }
    if (port == FRAM_CS2_GPIO_Port && pin == FRAM_CS2_Pin)      { *chip = 2u; return 0; }
    if (port == FRAM_CS3_GPIO_Port && pin == FRAM_CS3_Pin)      { *chip = 3u; return 0; }
    return -1;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    uint8_t chip;

    if (fram_cs_decode(GPIOx, GPIO_Pin, &chip) != 0) {
        return;   /* not a FRAM CS line: nothing emulated behind it */
    }
    if (PinState == GPIO_PIN_RESET) {
        /* CS fall opens a transaction. */
        spi_selected = chip;
        spi_op       = 0u;
        spi_addr     = 0u;
        spi_data_len = 0u;
        spi_hdr_done = 0;
        last_spi_chip = chip;
    } else {
        /* CS rise closes it; a completed WRITE consumes the WEL. */
        if (spi_selected == chip && spi_hdr_done && spi_op == FRAM_SPI_OP_WRITE) {
            spi_wel[chip] = 0;
        }
        if (spi_selected == chip) {
            spi_selected = FRAM_NO_CHIP;
        }
    }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    (void)GPIOx;
    (void)GPIO_Pin;
    return GPIO_PIN_SET;   /* CS lines idle high; nothing else is modelled */
}

/* Parse the 3-byte header (opcode + 16-bit address) of a READ/WRITE
 * transaction. Returns 0 on success, -1 when the frame is not one. */
static int spi_parse_header(const uint8_t *pData, uint16_t size)
{
    if (size < 3u) {
        return -1;
    }
    if (pData[0] != FRAM_SPI_OP_READ && pData[0] != FRAM_SPI_OP_WRITE) {
        return -1;
    }
    spi_op       = pData[0];
    spi_addr     = (uint16_t)(((uint16_t)pData[1] << 8) | pData[2]);
    spi_data_len = 0u;
    spi_hdr_done = 1;
    return 0;
}

/* Payload bounds of the open transaction against its 16 KB die. */
static int spi_payload_fits(uint16_t extra)
{
    return ((uint32_t)spi_addr + spi_data_len + extra) <= FRAM_CHIP_SIZE;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, const uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout)
{
    (void)Timeout;

    if (hspi == NULL || (Size != 0u && pData == NULL)) {
        return HAL_ERROR;
    }
    if (spi_selected == FRAM_NO_CHIP) {
        return HAL_ERROR;                     /* clocking with no CS asserted */
    }
    if (Size == 0u) {
        return HAL_OK;
    }
    if (!spi_hdr_done) {
        /* First frame of the transaction: standalone WREN or READ/WRITE
         * header (the header may arrive with the first payload bytes in one
         * frame, so fall through to the payload path below). */
        if (Size == 1u && pData[0] == FRAM_SPI_OP_WREN) {
            spi_wel[spi_selected] = 1;
            spi_op       = FRAM_SPI_OP_WREN;
            spi_hdr_done = 1;
            return HAL_OK;
        }
        if (spi_parse_header(pData, Size) != 0) {
            return HAL_ERROR;
        }
        if (spi_op == FRAM_SPI_OP_WRITE && !spi_wel[spi_selected]) {
            return HAL_ERROR;                 /* WRITE without WREN */
        }
        pData += 3u;
        Size  -= 3u;
    } else if (spi_op != FRAM_SPI_OP_WRITE) {
        return HAL_ERROR;                     /* data phase of a READ/WREN */
    }
    if (Size == 0u) {
        return HAL_OK;
    }
    if (spi_op != FRAM_SPI_OP_WRITE || !spi_payload_fits(Size)) {
        return HAL_ERROR;                     /* would cross a chip boundary */
    }

    memcpy(&fram[(size_t)spi_selected * FRAM_CHIP_SIZE + spi_addr + spi_data_len],
           pData, Size);
    spi_data_len += Size;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *pData,
                                  uint16_t Size, uint32_t Timeout)
{
    (void)Timeout;

    if (hspi == NULL || (Size != 0u && pData == NULL)) {
        return HAL_ERROR;
    }
    if (spi_selected == FRAM_NO_CHIP) {
        return HAL_ERROR;                     /* clocking with no CS asserted */
    }
    if (Size == 0u) {
        return HAL_OK;
    }
    if (!spi_hdr_done || spi_op != FRAM_SPI_OP_READ) {
        return HAL_ERROR;                     /* read with no READ header */
    }
    if (!spi_payload_fits(Size)) {
        return HAL_ERROR;                     /* would cross a chip boundary */
    }

    memcpy(pData,
           &fram[(size_t)spi_selected * FRAM_CHIP_SIZE + spi_addr + spi_data_len],
           Size);
    spi_data_len += Size;
    return HAL_OK;
}
