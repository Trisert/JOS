/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32l4xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "main.h"
#include "stm32l4xx_it.h"
#include "FreeRTOS.h"
#include "task.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dual_bank.h"
#include "sram2_parity.h"
#include "seu_mitigation.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Fault handlers (HardFault/MemManage/BusFault/UsageFault) live in
   Core/Src/faults.c - see the note further down in this file. */
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */
  /* NMI is NOT a synonym for "the software did something dumb" on an
     STM32L4. It is raised by at least four distinct sources, and they need
     different treatment (W2-2 review):

       - SRAM2 parity error         (SYSCFG_CFGR2.SPF, W2-3)
       - Flash double-bit ECC error (FLASH_ECCR.ECCD, RM0351 3.3)
       - RCC clock security system  (HSE failure)
       - an early-boot software fault (the dual-bank case)

     Feeding a parity or ECC NMI into dual_bank_mark_boot_fault() fabricates
     exactly the evidence that arms the golden-image switch: boot -> pool scan
     -> ECC NMI -> "boot fault" -> reset -> boot ... i.e. a self-sustaining
     reset loop that swaps banks on evidence it manufactured itself. So
     discriminate the source FIRST. Each branch below records its own
     evidence and resets; none of them returns, so the spin further down is
     only reachable in the -DDUAL_BANK_FAULT_NO_RESET bench build. */

  /* Count the upset in the RTC backup domain first: every branch below
     records the context and resets, so an in-RAM counter would not survive
     its own event (W2-5). */
  seu_mitigation_nmi_hook();

  if (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_PE) != 0U)
  {
    /* SRAM2 parity error: records the failure context in the LastStates pool,
       clears SPF and resets the OBSW (W2-3). */
    sram2_parity_nmi_handler();
  }
  else if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD) != 0U)
  {
    /* Flash double-bit ECC error on a read: a memory defect, not a boot
       fault. Clear the latch so a transient cannot re-storm, record it
       through the generic NMI record (SRAM2_EVENT_OTHER_NMI) and reset —
       deliberately WITHOUT touching the dual-bank boot-fault counter. */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
    sram2_parity_nmi_handler();
  }
  else if (__HAL_RCC_GET_IT(RCC_IT_CSS) != RESET)
  {
    /* RCC clock security system: the HSE crystal/oscillator died. That is a
       CLOCK fault, not a boot fault, and no bank switch can repair an
       oscillator - routing it into dual_bank_handle_boot_fault() would count
       DUAL_BANK_BOOT_FAULT_THRESHOLD of them and then swap to the golden
       image on self-manufactured evidence, i.e. the very loop this
       discrimination exists to kill (Kilo #21, comment id 3740842361).
       Handing it to sram2_parity_nmi_handler() was still wrong, though: that
       module hard-codes TRIGGER_SRAM2_PARITY on the entry and bumps
       sram2_parity_events, which seu_mitigation.c publishes as
       parity_events_boot - so the record landed filed under "SRAM2 parity"
       with an inflated RAM-health counter and ground diagnosed memory
       degradation for an HSE failure. It also reset on every pass, and a
       PERMANENT crystal failure has no reset budget, so it would reboot
       forever and program the LastStates trail away (Kilo #26, comment
       id 3741110986).

       sram2_clock_fault_nmi_handler() records under TRIGGER_CLOCK_FAULT with
       its own bounded budget, touches none of the parity counters and RETURNS:
       the hardware has already switched the system clock to HSI16, so the
       OBSW keeps running on the fallback instead of looping through reset. */
    sram2_clock_fault_nmi_handler();
    return;
  }
  else
  {
    /* Early-boot software fault: nothing else claimed this NMI, so the only
       remaining source is software that failed before the OBSW was up - the
       one case a golden-image switch can plausibly repair. Recording is
       RAM-only and ISR-safe (no Flash, no HAL, no blocking); the reset is
       issued there rather than leaving it to the IWDG backstop
       (Core/Src/hw_watchdog.c, ~31 s, added in W2 #24), so the fallback
       threshold is reached promptly instead of after half a minute of
       silence per boot. See Core/Inc/dual_bank.h. */
    dual_bank_handle_boot_fault();
  }
  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
    /* Only reached with -DDUAL_BANK_FAULT_NO_RESET (bench debugging). */
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/*
  * HardFault_Handler, MemManage_Handler, BusFault_Handler and
  * UsageFault_Handler are intentionally NOT defined here. The CubeMX stubs for
  * them were `while (1)` traps, i.e. a silent hang on orbit. The flight
  * implementations live in Core/Src/faults.c: they record the stacked core
  * registers and the SCB fault status registers in the LastStates pool and
  * then reset the MCU.
  *
  * The dual-bank boot-fault hook (W2-2) therefore lives in faults.c too:
  * fault_capture() calls dual_bank_mark_boot_fault() (RAM-only, ISR-safe)
  * before persisting the fault record and issuing NVIC_SystemReset(), so the
  * boot-fault counter still advances and the golden-image threshold is
  * reachable. Only NMI_Handler, which faults.c does not own, calls
  * dual_bank_handle_boot_fault() (record + reset) directly above.
  *
  * If this file is regenerated by CubeMX the stubs reappear and the link fails
  * with "multiple definition of `HardFault_Handler'" - delete them again
  * instead of removing the handlers in faults.c.
  */

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
  /* The generated body of this handler was EMPTY, which broke two things at
     once (Kilo review of PR #9, finding C5):

       1. HAL_IncTick() was never called, so uwTick stayed 0 forever.
          HAL_GetTick() therefore returned 0 to every caller, which means
          every LastStates / fault / boot-CRC / SEU record was stamped with
          timestamp 0 and ground could not order the post-mortem trail at
          all. Any HAL_Delay() or HAL *_Timeout path would also have hung.
       2. xPortSysTickHandler() was never called either. FreeRTOSConfig.h
          sets USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 1, which switches
          OFF the SysTick_Handler that cmsis_os2.c would otherwise provide,
          so this function is the ONLY tick source of the RTOS: without it
          vTaskDelay()/osDelay() never return and time slicing never runs.

     The scheduler-state guard is required because SysTick is already running
     between HAL_Init() and osKernelStart(): calling the FreeRTOS tick hook
     before the scheduler exists corrupts the (not yet initialised) task
     lists. */
  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
#if (INCLUDE_xTaskGetSchedulerState == 1)
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
#endif /* INCLUDE_xTaskGetSchedulerState */
    xPortSysTickHandler();
#if (INCLUDE_xTaskGetSchedulerState == 1)
  }
#endif /* INCLUDE_xTaskGetSchedulerState */
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32L4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32l4xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
