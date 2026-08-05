#ifndef BOOT_CRC_H
#define BOOT_CRC_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Firmware image integrity check (boot-time CRC32)
 *
 * Rationale: ECSS-E-ST-40C §5.4 (software integrity) and NASA-STD-8739.8
 * (fault detection) require the flight image to be verified before it is
 * trusted. SEU/latch-up or a partially completed uplink can corrupt Flash;
 * a corrupted image must be detected and reported, not silently executed.
 *
 * Implementation: pure-software CRC-32 (IEEE 802.3, reflected, poly
 * 0xEDB88320 — identical to zlib.crc32) over the whole loaded image, from
 * the start of Flash up to (but excluding) the stored CRC word:
 *
 *     region = [ __fw_image_start , __fw_crc_start )
 *
 * No HAL / peripheral CRC unit is used, so the check runs before any clock,
 * peripheral or RTOS dependency and cannot be defeated by a mis-configured
 * CRC peripheral.
 *
 * The expected CRC lives in the dedicated `.fw_crc` linker section, which is
 * the last loaded section of the image (see STM32L496VGTX_FLASH.ld). It is
 * written post-build by `tools/fw_crc_stamp.py`. An unstamped image keeps the
 * placeholder 0xFFFFFFFF and reports BOOT_CRC_UNSTAMPED (see below).
 * ------------------------------------------------------------------------- */

/* Value present in the image when it has not been stamped post-build. */
#define BOOT_CRC_UNSTAMPED_VALUE  0xFFFFFFFFu

typedef enum {
    BOOT_CRC_OK         = 0,  /* stored CRC matches the computed CRC       */
    BOOT_CRC_MISMATCH   = 1,  /* image corrupted (or wrongly stamped)      */
    BOOT_CRC_UNSTAMPED  = 2,  /* placeholder present: debug/unstamped build*/
    BOOT_CRC_BAD_REGION = 3,  /* linker symbols inconsistent               */
} boot_crc_status_t;

/* Generic software CRC-32 (IEEE 802.3, reflected). Exposed so the same
 * routine can be reused for uplink/downlink payload checks. */
uint32_t boot_crc32(const void *data, size_t len);

/* Verify the firmware image. Must be called from main() before
 * osKernelStart(). Latches the result for later telemetry. */
boot_crc_status_t boot_crc_verify(void);

/* Accessors for the latched result (valid after boot_crc_verify()). */
boot_crc_status_t boot_crc_get_status(void);
uint32_t          boot_crc_get_computed(void);
uint32_t          boot_crc_get_expected(void);
uint32_t          boot_crc_get_region_len(void);

#endif /* BOOT_CRC_H */
