#ifndef OBSW_DELAY_H
#define OBSW_DELAY_H

/* ---------------------------------------------------------------------------
 * Overflow-safe RTOS delays.
 *
 * pdMS_TO_TICKS(ms) expands to
 *
 *     ((TickType_t)(((TickType_t)(ms) * configTICK_RATE_HZ) / 1000U))
 *
 * The multiplication is done in TickType_t, which is 32-bit in this build
 * (configUSE_16_BIT_TICKS = 0) with configTICK_RATE_HZ = 1000. Any argument
 * above UINT32_MAX / configTICK_RATE_HZ = 4 294 967 ms (~71.6 min) therefore
 * WRAPS SILENTLY: the caller asks for a long sleep and gets a short one, with
 * no warning from the compiler.
 *
 * This bit the CLOUD payload task: osDelay(pdMS_TO_TICKS(90 * 60 * 1000))
 * computes 5 400 000 * 1000 = 0x1_41DD_7600, truncates to 0x41DD7600, and the
 * task woke every ~18.3 min instead of once per orbit — a 5x over-sampling of
 * the payload and of its FRAM writes.
 *
 * obsw_delay_ms() splits the request into chunks that provably cannot
 * overflow, so a period can be expressed in milliseconds anywhere in the code
 * base without every caller having to re-derive the tick arithmetic.
 *
 * Refs: ECSS-E-ST-40C 5.4 (software design integrity), JPL-182 Rule 14
 *       (check the numeric range of every computation).
 * ------------------------------------------------------------------------- */

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include <stdint.h>

/* Largest millisecond value pdMS_TO_TICKS() can convert without overflowing
   TickType_t. Derived from the actual configuration, not hard-coded, so a
   change of tick rate or tick width re-derives it. */
#define OBSW_DELAY_MAX_SAFE_MS \
    ((uint32_t)((TickType_t)-1 / (TickType_t)configTICK_RATE_HZ))

/* Sleep granularity used to build up long delays. One minute keeps the number
   of wake-ups negligible (90 per orbit for the CLOUD task) while staying two
   orders of magnitude below OBSW_DELAY_MAX_SAFE_MS. */
#define OBSW_DELAY_CHUNK_MS  60000UL

_Static_assert(OBSW_DELAY_CHUNK_MS <= OBSW_DELAY_MAX_SAFE_MS,
               "OBSW_DELAY_CHUNK_MS overflows pdMS_TO_TICKS()");

/* Block the calling task for total_ms milliseconds, whatever its magnitude.
   Zero returns immediately (osDelay(0) is a no-op yield we do not want to
   turn a "no delay" request into). */
static inline void obsw_delay_ms(uint32_t total_ms)
{
    while (total_ms > 0u) {
        uint32_t chunk = (total_ms > (uint32_t)OBSW_DELAY_CHUNK_MS)
                             ? (uint32_t)OBSW_DELAY_CHUNK_MS
                             : total_ms;
        (void)osDelay(pdMS_TO_TICKS(chunk));
        total_ms -= chunk;
    }
}

#endif /* OBSW_DELAY_H */
