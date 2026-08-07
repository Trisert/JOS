/* ---------------------------------------------------------------------------
 * host_support.h - interface of the host-side test doubles.
 *
 * Two independent groups live here:
 *
 *   1. Firmware-image doubles (stubs.c)   - stand in for the linker-provided
 *      symbols that App/obsw/boot_crc.c references.
 *   2. Flash / FRAM doubles (host_flash.c) - emulate the STM32L4 internal
 *      Flash pool that App/memory/memory.c writes LastStates entries into,
 *      plus the FM24VN10 FRAM behind HAL_I2C_Mem_*.
 *
 * Only the test files and the support files include this header; no flight
 * source is aware of it.
 * ------------------------------------------------------------------------- */
#ifndef HOST_SUPPORT_H
#define HOST_SUPPORT_H

#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
 * Firmware image doubles (support/stubs.c)
 * ========================================================================== */

/* Length in bytes of the fake firmware image spanned by
 * [__fw_image_start, __fw_crc_start). Kept small and fully deterministic so
 * the expected CRC-32 can be a hard-coded known-answer value. */
#define HOST_FW_IMAGE_LEN 64u

/* CRC-32 (IEEE 802.3 / zlib) of the 64 bytes defined in stubs.c.
 * Independently produced with Python: zlib.crc32(image) -> 0x00DEB784. */
#define HOST_FW_IMAGE_CRC 0x00DEB784u

/* Emulate what tools/fw_crc_stamp.py does post-build: patch the 32-bit word
 * that boot_crc.c publishes in the .fw_crc section. */
void host_fw_crc_stamp(uint32_t value);

/* Read back the stamped word (0xFFFFFFFF when unstamped). */
uint32_t host_fw_crc_stamped_value(void);

/* ==========================================================================
 * Internal-Flash / FRAM doubles (support/host_flash.c)
 * ========================================================================== */

/* memory.c hard-codes the LastStates pool at this absolute address and even
 * memcpy()s straight from it in laststates_dump_all(). The host double
 * therefore has to place real, writable memory at exactly that address --
 * see host_flash_reset(). */
#define HOST_FLASH_LASTSTATES_BASE 0x08080000UL
#define HOST_FLASH_PAGE_SIZE       2048u   /* STM32L4 bank-1 page size */

/* Map (once) and return the LastStates pool to the erased state (all 0xFF),
 * clear the FRAM contents and zero all call counters. Call from setUp().
 * Aborts the test with a readable message if the fixed mapping is refused. */
void host_flash_reset(void);

/* Direct access to the emulated pool, for assertions that bypass memory.c. */
const uint8_t *host_flash_pool(void);
size_t         host_flash_pool_size(void);

/* Call counters, so tests can assert on the erase-before-rewrite protocol. */
uint32_t host_flash_erase_count(void);
uint32_t host_flash_program_count(void);
uint32_t host_flash_unlock_count(void);
uint32_t host_flash_lock_count(void);

/* TRUE while HAL_FLASH_Unlock() has not been balanced by HAL_FLASH_Lock(). */
int host_flash_is_unlocked(void);

#endif /* HOST_SUPPORT_H */
