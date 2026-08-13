/* ---------------------------------------------------------------------------
 * rtos_stubs.c - host doubles for the two Core/Src services that
 *                App/obsw/watchdog.c depends on (W2-6).
 *
 *   Core/Src/dual_bank.c  -> dual_bank_boot_complete()  (programs Flash, reads
 *                            the RCC reset flags)
 *   Core/Src/hw_watchdog.c-> hw_watchdog_kick()         (writes IWDG->KR)
 *
 * Neither module is host-compilable, and neither is on the :source: path, so
 * without these definitions test_watchdog.out does not link.
 *
 * They are counters rather than empty bodies on purpose: "the monitor task
 * refreshed the IWDG" and "the boot-OK marker was declared" are behaviours a
 * test must be able to assert, and NASA-STD-8739.8 requires test doubles to be
 * explicit rather than silent no-ops.
 *
 * Ceedling links every :support: file into every test executable, so nothing
 * here may reference a symbol owned by one specific module.
 * ------------------------------------------------------------------------- */
#include "dual_bank.h"      /* fakes/dual_bank.h   */
#include "hw_watchdog.h"    /* fakes/hw_watchdog.h */

#include <stdint.h>

/* ---------- IWDG backstop ---------- */

static uint32_t hw_wdg_kicks;

void hw_watchdog_kick(void)
{
    hw_wdg_kicks++;
}

uint32_t host_hw_watchdog_kick_count(void)
{
    return hw_wdg_kicks;
}

void host_hw_watchdog_reset(void)
{
    hw_wdg_kicks = 0u;
}

/* ---------- Dual-bank boot-OK marker ----------
 * Returns 0 (persisted) by default. A test that wants to exercise the bounded
 * retry path in watchdog_declare_boot_ok_if_due() arms a failure count first.
 */
static uint32_t boot_complete_calls;
static uint32_t boot_complete_failures_left;

int dual_bank_boot_complete(void)
{
    boot_complete_calls++;

    if (boot_complete_failures_left > 0u) {
        boot_complete_failures_left--;
        return -1;
    }
    return 0;
}

uint32_t host_dual_bank_boot_complete_calls(void)
{
    return boot_complete_calls;
}

void host_dual_bank_fail_boot_complete(uint32_t times)
{
    boot_complete_failures_left = times;
}

void host_dual_bank_reset(void)
{
    boot_complete_calls         = 0u;
    boot_complete_failures_left = 0u;
}

/* ---------- Thread flags (CMSIS-RTOS2 host double) ----------
 * On the flight target these are provided by the FreeRTOS CMSIS wrapper. The
 * host test build links these no-op stand-ins so comms.c (which parks the RX
 * task on osThreadFlagsWait) compiles and links; the unit tests do not drive
 * real RTOS scheduling, so returning "no flags" / osOK is sufficient. */
#include "cmsis_os.h"

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    (void)flags; (void)options; (void)timeout;
    return 0u;
}

osStatus_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
    (void)thread_id; (void)flags;
    return osOK;
}
