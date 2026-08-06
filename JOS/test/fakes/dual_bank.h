/**
 * @file    fakes/dual_bank.h
 * @brief   Host-test stand-in for Core/Inc/dual_bank.h (W2-2).
 *
 * App/obsw/watchdog.c declares the boot good once the scheduler has been up
 * for DUAL_BANK_BOOT_OK_UPTIME_MS, by calling dual_bank_boot_complete().
 * The real implementation lives in Core/Src/dual_bank.c, which programs Flash
 * and reads the RCC reset flags - target only.
 *
 * Only the two symbols watchdog.c actually references are declared. The
 * value below MUST track Core/Inc/dual_bank.h: a divergence would make
 * test_watchdog assert against a deadline the flight build does not use.
 * The double lives in support/rtos_stubs.c.
 */
#ifndef JOS_TEST_FAKE_DUAL_BANK_H
#define JOS_TEST_FAKE_DUAL_BANK_H

/* Keep in sync with Core/Inc/dual_bank.h. */
#define DUAL_BANK_BOOT_OK_UPTIME_MS     5000U

/* 0 on success (boot-OK marker persisted), non-zero when it could not be. */
int dual_bank_boot_complete(void);

#endif /* JOS_TEST_FAKE_DUAL_BANK_H */
