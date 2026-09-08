/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32l4xx_hal_timebase_tim.c
  * @brief   HAL time base based on the hardware TIM6.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"
#include "stm32l4xx_hal_tim.h"

/** @addtogroup STM32L4xx_HAL_TimeBase_TIM
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* T35 (SPF Table 3.18): FreeRTOS owns SysTick, so the HAL 1 ms timebase
   (uwTick / HAL_GetTick() / HAL_Delay()) runs on the spare basic timer TIM6.
   This file overrides the __weak HAL_InitTick() / HAL_SuspendTick() /
   HAL_ResumeTick() hooks from stm32l4xx_hal.c following the STM32CubeMX
   "Timebase Source = TIM6" template, so HAL_Init() picks it up with no call
   site changes in main.c.

   The prescaler/period are derived from the live PCLK1 frequency instead of
   a hardcoded SystemCoreClock value. That matters because HAL_InitTick() runs
   once from HAL_Init() on the 4 MHz MSI reset clock AND again from
   HAL_RCC_OscConfig()/HAL_RCC_ClockConfig() after the switch to the 80 MHz
   PLL: a hardcoded prescaler would leave uwTick running ~20x fast. With the
   divide-down below (1 MHz counter, uwTickFreq reload) the tick stays 1 ms
   across both calls.

   uwTickPrio is stored here because this override replaces the HAL default
   that used to do it: the RCC clock helpers re-invoke
   HAL_InitTick(uwTickPrio), so leaving it "invalid" would pass a bogus IRQ
   priority on the second call. TICK_INT_PRIORITY is 15 (lowest, see
   stm32l4xx_hal_conf.h), the same level SysTick had, so the 1 ms tick never
   preempts the kernel or any peripheral ISR. HAL_IncTick() itself is a single
   32-bit increment with no FreeRTOS calls, so it is safe at any priority. */
/* USER CODE END 0 */
TIM_HandleTypeDef        htim6;
/* USER CODE BEGIN 1 */

/* These tick globals live in stm32l4xx_hal.c; this override maintains them. */
extern __IO uint32_t uwTick;
extern uint32_t uwTickPrio;
extern HAL_TickFreqTypeDef uwTickFreq;

/* USER CODE END 1 */

/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/**
  * @brief  This function configures the TIM6 as a time base source.
  *         The time source is configured to have 1ms time base with a dedicated
  *         Tick interrupt priority.
  * @note   This function is called automatically at the beginning of program after
  *         reset by HAL_Init() or at any time when clock is configured, by
  *         HAL_RCC_OscConfig() and HAL_RCC_ClockConfig().
  * @param  TickPriority: Tick interrupt priority.
  * @retval HAL status
  */
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
  RCC_ClkInitTypeDef    clkconfig;
  uint32_t              uwTimclock;
  uint32_t              uwPrescalerValue;
  uint32_t              uwPeriodValue;
  uint32_t              pFLatency;
  HAL_StatusTypeDef     status = HAL_OK;

  /* Enable TIM6 clock */
  __HAL_RCC_TIM6_CLK_ENABLE();

  /* Get clock configuration */
  HAL_RCC_GetClockConfig(&clkconfig, &pFLatency);

  /* Compute TIM6 clock: on APB1 the timer kernel clock doubles when the APB
     prescaler is different from DIV1 (RM0351 6.2.14). */
  uwTimclock = HAL_RCC_GetPCLK1Freq();
  if (clkconfig.APB1CLKDivider != RCC_HCLK_DIV1)
  {
    uwTimclock *= 2U;
  }

  /* Compute the prescaler value to have TIM6 counter clock equal to 1 MHz */
  uwPrescalerValue = (uwTimclock / 1000000U) - 1U;

  /* Compute the period to reach the configured tick frequency (1 kHz default) */
  uwPeriodValue = (1000000U / (uint32_t)uwTickFreq) - 1U;

  /* Initialize TIM6 */
  htim6.Instance = TIM6;
  htim6.Init.Period = uwPeriodValue;
  htim6.Init.Prescaler = uwPrescalerValue;
  htim6.Init.ClockDivision = 0U;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  status = HAL_TIM_Base_Init(&htim6);
  if (status == HAL_OK)
  {
    /* Start the TIM time base generation in interrupt mode */
    status = HAL_TIM_Base_Start_IT(&htim6);
  }

  /* Return function status */
  if (status == HAL_OK)
  {
    /* Enable the TIM6 global Interrupt */
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);

    /* Configure the TIM6 time base interrupt priority */
    if (TickPriority < (1UL << __NVIC_PRIO_BITS))
    {
      HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0U);
      uwTickPrio = TickPriority;
    }
    else
    {
      status = HAL_ERROR;
    }
  }

  /* Return function status */
  return status;
}

/**
  * @brief  Suspend Tick increment.
  * @note   Disable the tick increment by disabling TIM6 update interrupt.
  * @retval None
  */
void HAL_SuspendTick(void)
{
  /* Disable TIM6 update Interrupt */
  __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

/**
  * @brief  Resume Tick increment.
  * @note   Enable the tick increment by enabling TIM6 update interrupt.
  * @retval None
  */
void HAL_ResumeTick(void)
{
  /* Enable TIM6 Update interrupt */
  __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}

/**
  * @}
  */
