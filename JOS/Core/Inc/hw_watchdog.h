/**
  ******************************************************************************
  * @file    hw_watchdog.h
  * @brief   Independent hardware watchdog (IWDG) - last-resort escalation bound.
  *
  * Rationale (Kilo Code review of PR #13, CRITICAL finding on the MPU/fault
  * path). Every software containment path in this OBSW ends in
  * NVIC_SystemReset(). That instruction is only reachable while the core is
  * still executing:
  *
  *   - a fault taken while the core is entering a fault handler of the same or
  *     lower priority escalates to HardFault, and a fault taken inside the
  *     HardFault handler puts an ARMv7-M core into the LOCKUP state. In LOCKUP
  *     no instruction retires ever again, so no amount of software fault
  *     containment can issue a reset;
  *   - App/obsw/watchdog.c is a *software* FreeRTOS task. It cannot run while
  *     the core is parked in an exception handler, spinning in Error_Handler()
  *     (which additionally does __disable_irq()), or in LOCKUP.
  *
  * Until this module existed there was no hardware watchdog anywhere in the
  * tree (HAL_IWDG_MODULE_ENABLED was commented out), so any of the above was a
  * permanent, unobservable hang - in orbit, where nobody can reach the reset
  * button. The IWDG runs off the LSI, is independent of the system clock, of
  * the core's execution state and of PRIMASK, and cannot be stopped once
  * started. It is the only mechanism that recovers from LOCKUP.
  *
  * It is deliberately a *backstop*, not a scheduling watchdog: the timeout is
  * ~32 s while the refresher runs every 500 ms, so it can never fire on a
  * merely busy system. Detecting a late task remains watchdog.c's job.
  *
  * Standards: NASA-STD-8739.8 (fault containment / no silent failure),
  *            ECSS-Q-ST-80C 6.2.6 (fault tolerance, bounded recovery).
  ******************************************************************************
  */

#ifndef HW_WATCHDOG_H
#define HW_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Start the independent watchdog.
  *
  * Has no clock or peripheral prerequisites: starting the IWDG switches the
  * LSI on by itself, so this may be called as early as desired and the
  * remaining boot sequence is then already covered.
  *
  * Starting the IWDG is irreversible by design: there is no hw_watchdog_stop().
  * Every long-running boot step must therefore call hw_watchdog_kick().
  *
  * Idempotent: a second call is a no-op.
  */
void hw_watchdog_init(void);

/**
  * @brief  Refresh the watchdog counter.
  *
  * Reduces to a single register write (IWDG->KR = reload key), so it is safe
  * from task, ISR and exception context alike, and is a no-op when the
  * watchdog was never started.
  */
void hw_watchdog_kick(void);

/**
  * @brief  Report whether the watchdog is armed.
  * @retval 1 when the watchdog is armed with the intended ~31 s timeout,
  *         0 when the prescaler/reload programming did not take effect.
  *
  * A 0 here means the LOCKUP/hang bound described above is NOT in place; the
  * caller should surface it as a housekeeping/telemetry point rather than
  * assume the backstop exists.
  */
uint8_t hw_watchdog_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_WATCHDOG_H */
