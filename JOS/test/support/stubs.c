/* Test support: provide the linker-provided image bounds that boot_crc.c
 * references via extern. In the real firmware these come from the linker
 * script (STM32L496VGTX_FLASH.ld); in the host test we define them as
 * simple static arrays so boot_crc_verify() links and can be exercised. */
#include <stdint.h>

/* Placed in .data so they have a real address the linker can resolve. */
uint8_t __fw_image_start[1024] = {0};
uint8_t __fw_crc_start[4] = {0};
