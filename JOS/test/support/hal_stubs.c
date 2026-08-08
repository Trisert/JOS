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
 * double records the request and longjmp-free aborts the test run with a
 * readable message unless a test has explicitly armed it (no test currently
 * exercises the reset path; boot_crc_apply_policy() only reaches it after the
 * attempt counter saturates).
 *
 * Refs: NASA-STD-8739.8 (test doubles must be explicit, never silent).
 * ------------------------------------------------------------------------- */
#include "main.h"        /* fakes/main.h */
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

void NVIC_SystemReset(void)
{
    /* On target this never returns. Failing loudly beats silently continuing
     * into code the flight software believes is unreachable. */
    TEST_FAIL_MESSAGE("NVIC_SystemReset() called: the code under test asked "
                      "for a reboot");
}
