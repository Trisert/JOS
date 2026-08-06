#include "boot_crc.h"

/* ---------------------------------------------------------------------------
 * Linker-provided image bounds (STM32L496VGTX_FLASH.ld):
 *   __fw_image_start = ORIGIN(FLASH)         (0x08000000)
 *   __fw_crc_start   = start of .fw_crc      (last loaded section)
 * Declared as arrays so the symbol *address* is the value we need.
 * ------------------------------------------------------------------------- */
extern const uint8_t __fw_image_start[];
extern const uint8_t __fw_crc_start[];

/* ---------------------------------------------------------------------------
 * Stored (expected) CRC.
 *
 * Emitted into the .fw_crc section, i.e. the last 4 bytes of the binary
 * image, and left at the placeholder value until `tools/fw_crc_stamp.py`
 * patches the built .bin/.hex. `used` + KEEP() in the linker script prevent
 * -ffunction-sections/--gc-sections from dropping it.
 * ------------------------------------------------------------------------- */
__attribute__((section(".fw_crc"), used))
const volatile uint32_t fw_crc_stored = BOOT_CRC_UNSTAMPED_VALUE;

/* ---------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3), reflected, poly 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF — bit-identical to zlib.crc32().
 *
 * Nibble-wise table (16 entries, 64 B of .rodata) instead of the usual 1 KiB
 * byte table: ~4x faster than the bitwise loop while keeping the Flash cost
 * negligible on a 512 KB image budget.
 * ------------------------------------------------------------------------- */
static const uint32_t crc32_nibble_table[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (p == NULL) {
        return 0u;
    }

    while (len-- > 0u) {
        crc ^= (uint32_t)(*p++);
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
    }

    return crc ^ 0xFFFFFFFFu;
}

/* ---------- Latched result (readable by telemetry / beacon) ---------- */
static boot_crc_status_t crc_status     = BOOT_CRC_UNSTAMPED;
static uint32_t          crc_computed   = 0u;
static uint32_t          crc_expected   = BOOT_CRC_UNSTAMPED_VALUE;
static uint32_t          crc_region_len = 0u;

/* boot_crc_verify() depends on the firmware linker symbols __fw_image_start /
 * __fw_crc_start (defined in STM32L496VGTX_FLASH.ld). Those do not exist in a
 * host build, so compile the body only for the firmware target. The checksum
 * primitive boot_crc32() is link-independent and is what the unit tests and
 * the scrubber (W2-5) rely on. */
#ifndef BOOT_CRC_HOST_BUILD
boot_crc_status_t boot_crc_verify(void)
{
    const uint8_t *start = __fw_image_start;
    const uint8_t *end   = __fw_crc_start;

    if (end <= start) {
        /* Linker layout broken: cannot verify anything. */
        crc_status     = BOOT_CRC_BAD_REGION;
        crc_region_len = 0u;
        return crc_status;
    }

    crc_region_len = (uint32_t)(end - start);
    crc_computed   = boot_crc32(start, (size_t)crc_region_len);
    crc_expected   = fw_crc_stored;

    if (crc_expected == BOOT_CRC_UNSTAMPED_VALUE) {
        /* Image was never stamped (debug build flashed straight from the
         * .elf). Nothing to compare against — report, do not fail. */
        crc_status = BOOT_CRC_UNSTAMPED;
    } else if (crc_computed == crc_expected) {
        crc_status = BOOT_CRC_OK;
    } else {
        crc_status = BOOT_CRC_MISMATCH;
    }

    return crc_status;
}
#endif /* BOOT_CRC_HOST_BUILD */

boot_crc_status_t boot_crc_get_status(void)     { return crc_status; }
uint32_t          boot_crc_get_computed(void)   { return crc_computed; }
uint32_t          boot_crc_get_expected(void)   { return crc_expected; }
uint32_t          boot_crc_get_region_len(void) { return crc_region_len; }
