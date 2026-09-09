/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

extern TIM_HandleTypeDef htim6;

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* OBC V2.0 netlist assignments (SPF v3 3.7.5.3.1 p.95):
 *   TEMP_GPIO_IN  = 4x TMP1827 SDQ on ONE 1-wire bus -> PB2 (pin 37)
 *   DEPLOY_SENSE / LoRa_NRST multiplexed on ONE GPIO -> PB1
 * PB4 is NJTRST (excluded), PA4 is FRAM CS, NRST is the MCU hardware reset
 * (not a GPIO — do not touch). */
#define TEMP_1WIRE_GPIO_Port   GPIOB
#define TEMP_1WIRE_Pin         GPIO_PIN_2
#define DEPLOY_SENSE_GPIO_Port GPIOB
#define DEPLOY_SENSE_Pin       GPIO_PIN_1
/* LoRa_NRST shares the PB1 GPIO with DEPLOY_SENSE: output for the SX1268
 * reset (active low) vs input + pull-up for the deploy-switch read.
 * Never both at once — see App/comms/deploy_sense.h ownership rule. */
#define LoRa_NRST_GPIO_Port    GPIOB
#define LoRa_NRST_Pin          GPIO_PIN_1

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
