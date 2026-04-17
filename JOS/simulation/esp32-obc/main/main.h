#ifndef MAIN_H_COMPAT
#define MAIN_H_COMPAT

#include "hal_compat.h"

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
ADC_HandleTypeDef hadc1;
TIM_HandleTypeDef htim1;

static inline void SystemClock_Config(void) {}
static inline void MX_GPIO_Init(void) {}
static inline void MX_ADC1_Init(void) { hadc1.Instance = (void *)1; }
static inline void MX_I2C1_Init(void) { hi2c1.Instance = (void *)2; }
static inline void MX_I2C2_Init(void) { hi2c2.Instance = (void *)3; }
static inline void MX_SPI1_Init(void) { hspi1.Instance = (void *)4; }
static inline void MX_SPI2_Init(void) { hspi2.Instance = (void *)5; }
static inline void MX_TIM1_Init(void) { htim1.Instance = (void *)6; }

static inline void NVIC_SystemReset(void)
{
    esp_restart();
}

void esp_restart(void);

#endif /* MAIN_H_COMPAT */
