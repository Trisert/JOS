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
 * a corrupted image must be detected, recorded and acted upon — never
 * silently executed.
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
 * written post-build by `make crc-stamp` (tools/fw_crc_stamp.py), which is a
 * prerequisite of `make all` and is re-run and re-checked in CI, so a shipped
 * artefact is never unstamped.
 *
 * Sentinel values (see docs/dev/hardening.md §2.4):
 *   0x00000000  BOOT_CRC_UNSTAMPED_VALUE — deliberate "not stamped yet"
 *               placeholder. Deliberately NOT the erased-Flash pattern: a
 *               word that has decayed/erased to 0xFFFFFFFF must not be able
 *               to downgrade a corrupted image to "nothing to check".
 *   0xFFFFFFFF  BOOT_CRC_ERASED_VALUE — erased Flash. Treated as a fault
 *               (BOOT_CRC_ERASED), i.e. the image tail was never programmed
 *               or has been corrupted.
 * Any other value is a real stamp and must match the computed CRC.
 * ------------------------------------------------------------------------- */

/* Value stored in .fw_crc by the compiler, before the image is stamped. */
#define BOOT_CRC_UNSTAMPED_VALUE  0x00000000u

/* Erased-Flash pattern: never a valid stamp, always a fault. */
#define BOOT_CRC_ERASED_VALUE     0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * Fault policy
 *
 * BOOT_CRC_FATAL selects what happens when the image cannot be trusted. It is
 * defined to 1 by the build system (Makefile C_DEFS) and defaults to 1 here so
 * that no build can accidentally end up with the fault path compiled out.
 *
 *   BOOT_CRC_FATAL = 1 (flight default)
 *     1. The fault is recorded in the LastStates pool (internal Flash).
 *     2. The OBC performs NVIC_SystemReset(), up to
 *        BOOT_CRC_MAX_RESET_ATTEMPTS times, to recover from a transient
 *        (SEU-induced) corruption of the Flash read path. The attempt counter
 *        lives in the .noinit RAM region and therefore survives the warm reset.
 *     3. When the retry budget is exhausted the OBC continues to boot but the
 *        image stays *untrusted*: state_machine confines the satellite to
 *        STATE_CRIT (beacon-only, payloads inhibited) so ground can diagnose
 *        and re-upload. RedPill carries no golden image / bootloader, and the
 *        IWDG is not configured, so halting in Error_Handler() (a
 *        __disable_irq(); while(1)) would be an unrecoverable brick — that is
 *        explicitly *not* the safe state.
 *
 *   BOOT_CRC_FATAL = 0 (bench builds only)
 *     The fault is still recorded and the image is still marked untrusted
 *     (safe mode), but no reset is attempted, so a debugger session survives.
 * ------------------------------------------------------------------------- */
#ifndef BOOT_CRC_FATAL
#define BOOT_CRC_FATAL 1
#endif

#ifndef BOOT_CRC_MAX_RESET_ATTEMPTS
#define BOOT_CRC_MAX_RESET_ATTEMPTS 2u
#endif

typedef enum {
    BOOT_CRC_OK         = 0,  /* stored CRC matches the computed CRC        */
    BOOT_CRC_MISMATCH   = 1,  /* image corrupted (or wrongly stamped)       */
    BOOT_CRC_UNSTAMPED  = 2,  /* 0x00000000 placeholder: unstamped build    */
    BOOT_CRC_BAD_REGION = 3,  /* linker symbols inconsistent                */
    BOOT_CRC_ERASED     = 4,  /* 0xFFFFFFFF: image tail erased/corrupted    */
} boot_crc_status_t;

/* Generic software CRC-32 (IEEE 802.3, reflected). Exposed so the same
 * routine can be reused for uplink/downlink payload checks, and so the host
 * self-test (tools/crc_selftest.c, `make crc-selftest`) can prove the flight
 * routine agrees with the zlib-based stamping tool. */
uint32_t boot_crc32(const void *data, size_t len);

#if !defined(BOOT_CRC_HOST_BUILD)

/* Verify the firmware image. Must be called from main() before
 * osKernelStart(). Latches the result for later telemetry. */
boot_crc_status_t boot_crc_verify(void);

/* Apply the fault policy for the latched result. Must be called after
 * laststates_init() (the fault is persisted there) and before
 * osKernelStart(). May not return: it resets the MCU while the retry budget
 * lasts. See "Fault policy" above. */
void boot_crc_apply_policy(void);

/* 1 when the running image may be trusted for nominal operations
 * (BOOT_CRC_OK or an unstamped bench build), 0 when the OBC must stay in the
 * safe state. Consulted by the state machine on every transition. */
int boot_crc_image_trusted(void);

/* Accessors for the latched result (valid after boot_crc_verify()). */
boot_crc_status_t boot_crc_get_status(void);
uint32_t          boot_crc_get_computed(void);
uint32_t          boot_crc_get_expected(void);
uint32_t          boot_crc_get_region_len(void);

/* Number of integrity-driven resets performed since the last clean boot
 * (survives warm reset; for beacon telemetry / post-mortem). */
uint32_t          boot_crc_get_reset_attempts(void);

#endif /* !BOOT_CRC_HOST_BUILD */

#endif /* BOOT_CRC_H */
