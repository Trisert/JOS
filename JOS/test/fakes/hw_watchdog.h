/**
 * @file    fakes/hw_watchdog.h
 * @brief   Host-test stand-in for Core/Inc/hw_watchdog.h (IWDG backstop, #24).
 *
 * App/obsw/watchdog.c refreshes the independent hardware watchdog at the top
 * of every monitor scan. The real implementation writes IWDG->KR through
 * CMSIS; on the host it is a counter in support/rtos_stubs.c so a test can
 * assert the kick happened without touching silicon.
 */
#ifndef JOS_TEST_FAKE_HW_WATCHDOG_H
#define JOS_TEST_FAKE_HW_WATCHDOG_H

#include <stdint.h>

void hw_watchdog_kick(void);

/* Host-only observation seam (not part of the flight API). */
uint32_t host_hw_watchdog_kick_count(void);
void     host_hw_watchdog_reset(void);

#endif /* JOS_TEST_FAKE_HW_WATCHDOG_H */
