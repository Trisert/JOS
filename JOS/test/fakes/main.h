/**
 * @file    fakes/main.h
 * @brief   Host-test stand-in for Core/Inc/main.h (CubeMX generated).
 *
 * Provides the peripheral handle externs and the Cortex-M intrinsics that
 * JOS/App references. NVIC_SystemReset() is declared as a real function (it is
 * a static inline in CMSIS core_cm4.h) so CMock can mock it and the tests can
 * assert that a RESET telecommand actually resets — without rebooting the
 * test runner.
 */
#ifndef JOS_TEST_FAKE_MAIN_H
#define JOS_TEST_FAKE_MAIN_H

#include "stm32l4xx_hal.h"

/* Peripheral handles — defined in test/support/stubs.c */
extern I2C_HandleTypeDef hi2c2;   /* FRAM bus            */
extern SPI_HandleTypeDef hspi1;   /* SX1268 LoRa radio   */
extern ADC_HandleTypeDef hadc1;   /* BMS measurements    */
extern IWDG_HandleTypeDef hiwdg;  /* independent watchdog */

/* Cortex-M system control */
void NVIC_SystemReset(void);

/* CubeMX error trap */
void Error_Handler(void);

#endif /* JOS_TEST_FAKE_MAIN_H */
