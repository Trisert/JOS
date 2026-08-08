/* ---------------------------------------------------------------------------
 * hal_stubs.c - host definitions for the CMSIS/HAL entry points that the
 *               modules under test call but that have no meaning on a PC.
 *
 * Only symbols that are *referenced* by host-compiled flight code live here:
 *
 *   App/obsw/boot_crc.c -> HAL_GetTick()        (timestamp of the CRC-fail
 *                                                LastStates record)
 *                       -> NVIC_SystemReset()   (reboot after too many
 *                                                consecutive CRC failures)
 *
 * NVIC_SystemReset() must not return on target. Returning here would let the
 * test continue past a point the flight code considers unreachable, so the
 * double either
 *
 *   - aborts the test run with a readable message (default: nothing asked for
 *     a reboot, so a reboot is a defect), or
 *   - longjmp()s straight back to the HOST_EXPECT_NVIC_RESET() call site when
 *     a test has explicitly armed the capture. That reproduces "does not
 *     return" faithfully: no statement after NVIC_SystemReset() is executed,
 *     which is exactly what the retry-budget logic in boot_crc_apply_policy()
 *     relies on.
 *
 * Refs: NASA-STD-8739.8 (test doubles must be explicit, never silent).
 * ------------------------------------------------------------------------- */
#include "main.h"        /* fakes/main.h */
#include "host_support.h"
#include "unity.h"

#include <stdint.h>

/* Monotonic millisecond tick. Deterministic (no wall clock) so a test that
 * asserts on a recorded timestamp can predict it: the counter advances by one
 * per call and is reset by host_hal_tick_reset(). */
static uint32_t host_tick;

uint32_t HAL_GetTick(void)
{
    return host_tick++;
}

void host_hal_tick_reset(void)
{
    host_tick = 0u;
}

/* ---------------------------------------------------------------------------
 * NVIC_SystemReset() capture
 * ------------------------------------------------------------------------- */
jmp_buf host_nvic_reset_jmp;

static int      nvic_reset_armed;
static uint32_t nvic_reset_requests;

void host_nvic_reset_arm(void)
{
    nvic_reset_armed = 1;
}

void host_nvic_reset_disarm(void)
{
    nvic_reset_armed = 0;
}

uint32_t host_nvic_reset_count(void)
{
    return nvic_reset_requests;
}

void host_nvic_reset_clear(void)
{
    nvic_reset_armed    = 0;
    nvic_reset_requests = 0u;
}

void NVIC_SystemReset(void)
{
    nvic_reset_requests++;

    if (nvic_reset_armed) {
        /* Armed by HOST_EXPECT_NVIC_RESET(): emulate the reboot by never
         * returning to the caller. */
        nvic_reset_armed = 0;
        longjmp(host_nvic_reset_jmp, 1);
    }

    /* On target this never returns. Failing loudly beats silently continuing
     * into code the flight software believes is unreachable. */
    TEST_FAIL_MESSAGE("NVIC_SystemReset() called: the code under test asked "
                      "for a reboot");
}
