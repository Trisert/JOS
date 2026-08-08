/**
  ******************************************************************************
  * @file    hw_watchdog.c
  * @brief   Independent hardware watchdog (IWDG) - last-resort escalation bound.
  *
  * See Core/Inc/hw_watchdog.h for the rationale (Kilo Code CRITICAL finding on
  * PR #13: every software containment path ends in NVIC_SystemReset(), which is
  * unreachable from the ARMv7-M LOCKUP state).
  *
  * Implemented directly against the CMSIS register definitions rather than via
  * HAL_IWDG: the STM32L4 HAL IWDG driver is not vendored in Drivers/ (only 21
  * of the HAL modules are), and pulling in ~700 lines of driver plus its header
  * to perform five register writes would add far more unexercised code than it
  * removes. The IWDG programming sequence below is the one in RM0351 38.3, and
  * every write is commented against it.
  ******************************************************************************
  */

#include "hw_watchdog.h"

#include "main.h"   /* CMSIS device header: IWDG, IWDG_SR_* */

/* ---------- IWDG key register commands (RM0351 38.4.1) ---------- */
#define IWDG_KEY_RELOAD       0x0000AAAAU  /* refresh the counter          */
#define IWDG_KEY_ENABLE       0x0000CCCCU  /* start the watchdog           */
#define IWDG_KEY_WRITE_ACCESS 0x00005555U  /* unprotect PR / RLR / WINR    */

/* Prescaler /256 - the largest the IWDG offers (IWDG_PR = 0b111). */
#define IWDG_PRESCALER_DIV256 0x00000007U

/* Reload value, 12 bits, maximum. */
#define HW_WATCHDOG_RELOAD    0x00000FFFU

/* Timeout budget.
 *
 * t_out = (prescaler / f_LSI) * (reload + 1)
 *
 * f_LSI is nominally 32 kHz on the STM32L4 and specified as 29.5..34 kHz over
 * the full temperature and VDD range. With /256 and reload 4095:
 *
 *   fastest LSI (34.0 kHz): (256 / 34000) * 4096 = 30.8 s
 *   nominal     (32.0 kHz): (256 / 32000) * 4096 = 32.8 s
 *   slowest     (29.5 kHz): (256 / 29500) * 4096 = 35.5 s
 *
 * The case that matters is the fastest LSI, i.e. ~30.8 s of guaranteed margin
 * before an un-refreshed core is reset. The refresher (watchdog_monitor_task,
 * 500 ms period, osPriorityHigh) has ~60x headroom, so this watchdog cannot
 * fire on a system that is merely busy - only on one that has genuinely
 * stopped executing the OBSW. Bounding a permanent hang at ~31 s is the whole
 * point; tightening it would trade a real recovery guarantee for a
 * spurious-reset risk that watchdog.c already covers in software.
 */

/* Bound on the "prescaler/reload update in progress" poll.
 *
 * Deliberately a plain loop counter, not HAL_GetTick(): hw_watchdog_init()
 * runs early in main() and must stay usable even if SysTick were not yet
 * ticking, and an un-bounded poll here would itself be the kind of silent hang
 * this module exists to eliminate. The registers settle within a few LSI
 * cycles; at 80 MHz this bound is several milliseconds, i.e. orders of
 * magnitude more than required. */
#define IWDG_STATUS_POLL_LIMIT  1000000U

/* Written once by hw_watchdog_init(), read from task and exception context
   (including after a fault), hence volatile. */
static volatile uint8_t s_running = 0U;

void hw_watchdog_init(void)
{
    uint32_t guard;

    /* Idempotent: the IWDG cannot be stopped once started, and re-running the
       sequence on a live watchdog would needlessly re-poll the update flags. */
    if (s_running != 0U) {
        return;
    }

    /* 1. Start the watchdog. This also switches the LSI on automatically, so
          no RCC configuration is required and the watchdog stays independent
          of the system clock tree that SystemClock_Config() sets up. */
    IWDG->KR = IWDG_KEY_ENABLE;

    /* 2. Unprotect PR/RLR (they are write-protected until this key is sent). */
    IWDG->KR = IWDG_KEY_WRITE_ACCESS;

    /* 3. Program prescaler and reload. */
    IWDG->PR  = IWDG_PRESCALER_DIV256;
    IWDG->RLR = HW_WATCHDOG_RELOAD;

    /* 4. Wait for the values to be taken into the LSI clock domain. Bounded:
          see IWDG_STATUS_POLL_LIMIT. */
    guard = IWDG_STATUS_POLL_LIMIT;
    while (((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U) && (guard != 0U)) {
        guard--;
    }

    if (guard == 0U) {
        /* The registers never settled, so the timeout actually in force is
           unknown. The watchdog is running regardless (step 1 is irreversible)
           and will still bound a hang - at the reset-default /4 prescaler,
           roughly 512 ms, which the 500 ms refresher could not reliably meet.
           Report "not running" so the condition is visible rather than
           presenting an unreliable guarantee as a sound one. */
        return;
    }

    /* 5. Refresh once so the counter starts from a full reload. */
    IWDG->KR = IWDG_KEY_RELOAD;

    s_running = 1U;
}

void hw_watchdog_kick(void)
{
    if (s_running != 0U) {
        /* A single store to a write-only key register: no read-modify-write,
           no poll, no HAL_GetTick(). Safe from task, ISR and exception context
           alike, and inherently atomic. */
        IWDG->KR = IWDG_KEY_RELOAD;
    }
}

uint8_t hw_watchdog_is_running(void)
{
    return s_running;
}
