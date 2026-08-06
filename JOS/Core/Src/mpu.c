/**
  ******************************************************************************
  * @file    mpu.c
  * @brief   Cortex-M4 Memory Protection Unit (MPU) configuration for RedPill
  *          OBSW — kernel / task isolation and W^X enforcement (W2-1).
  *
  * Hardening card W2-1 (see docs/dev/hardening.md §2.1).
  * Standards: NASA Power of Ten #4, JPL-182, ECSS-E-ST-40C.
  *
  * Region map (ARMv7-M: the highest numbered matching region wins; anything
  * not covered falls back to the default system map because PRIVDEFENA=1 and
  * every thread of this build runs privileged — the kernel uses the plain
  * GCC/ARM_CM4F port, not the FreeRTOS-MPU port):
  *
  *   #0  0x08000000  512 KB  Flash code+rodata   RO,  cacheable, executable
  *   #1  0x20000000  256 KB  SRAM1               RW,  Execute-Never
  *   #2  0x20040000   64 KB  SRAM2 alias         RW,  Execute-Never
  *   #3  0x10000000   64 KB  SRAM2               RW,  Execute-Never
  *   #4  0x40000000  512 MB  Peripherals         RW,  Device, Execute-Never
  *   #5  _estack-8K   32 B   Kernel (MSP) guard  RO,  Execute-Never
  *   #6  (free — reserved for future hardening cards)
  *   #7  <dynamic>    32 B   Running-task guard  RO,  Execute-Never
  *
  * Deliberately *not* covered:
  *   - The LastStates flash pool at 0x08080000 (8 KB) is left on the default
  *     map so HAL_FLASH_Program() can write it; region #0 stops exactly at
  *     0x0807FFFF.
  *   - The SRAM bit-band alias at 0x22000000: a bit-band fetch expands one bit
  *     per word, so it is not a usable code-injection path, and leaving it out
  *     keeps region #6 free.
  *   - The FRAM is an external SPI device, not memory mapped, so it cannot be
  *     fenced by the MPU; it is protected by the driver API instead.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "mpu.h"

#include <stdint.h>

#include "stm32l4xx.h"

/* Private define ------------------------------------------------------------*/

/* Region numbers ------------------------------------------------------------*/
#define MPU_REGION_FLASH            0U
#define MPU_REGION_SRAM1            1U
#define MPU_REGION_SRAM2_ALIAS      2U
#define MPU_REGION_SRAM2            3U
#define MPU_REGION_PERIPH           4U
#define MPU_REGION_KERNEL_GUARD     5U
#define MPU_REGION_TASK_GUARD       7U

/* Address map ---------------------------------------------------------------*/
#define MPU_FLASH_BASE              0x08000000UL   /* 512 KB of application code */
#define MPU_SRAM1_BASE              0x20000000UL   /* 256 KB                     */
#define MPU_SRAM1_END               0x20040000UL
#define MPU_SRAM2_ALIAS_BASE        0x20040000UL   /* 64 KB, contiguous alias    */
#define MPU_SRAM2_ALIAS_END         0x20050000UL
#define MPU_SRAM2_BASE              0x10000000UL   /* 64 KB, parity-protected    */
#define MPU_SRAM2_END               0x10010000UL
#define MPU_PERIPH_BASE             0x40000000UL   /* 512 MB peripheral aperture */

/* RASR SIZE field encoding: log2(size) - 1 ----------------------------------*/
#define MPU_SIZE_32B                4U
#define MPU_SIZE_64KB               15U
#define MPU_SIZE_256KB              17U
#define MPU_SIZE_512KB              18U
#define MPU_SIZE_512MB              28U

/* RASR access permission (AP) field, see ARMv7-M ARM B3.5.8 -----------------*/
#define MPU_AP_RW                   (3UL << MPU_RASR_AP_Pos)  /* priv RW / unpriv RW */
#define MPU_AP_RO                   (6UL << MPU_RASR_AP_Pos)  /* priv RO / unpriv RO */

/* RASR memory type / attribute presets --------------------------------------*/
/* Normal, non-shareable, write-through cacheable (internal flash). */
#define MPU_ATTR_FLASH_MEM          (MPU_RASR_C_Msk)
/* Normal, shareable, write-back cacheable (internal SRAM). */
#define MPU_ATTR_SRAM_MEM           (MPU_RASR_C_Msk | MPU_RASR_B_Msk | MPU_RASR_S_Msk)
/* Device, shareable (peripherals). */
#define MPU_ATTR_DEVICE_MEM         (MPU_RASR_B_Msk | MPU_RASR_S_Msk)

#define MPU_XN                      (MPU_RASR_XN_Msk)

/**
  * Size of the MSP window kept below _estack before the kernel stack guard
  * band trips. The linker only *reserves* _Min_Stack_Size (1 KB), which is far
  * less than what main() + HAL init + nested ISRs actually use, so the guard
  * is placed conservatively low: it exists to stop a runaway kernel stack from
  * reaching .bss / the FreeRTOS heap, not to enforce a tight budget.
  */
#define MPU_KERNEL_STACK_WINDOW     (8U * 1024U)

/** Minimum slack that must remain between the top of .bss/newlib heap and the
  * kernel stack guard for the guard to be installed at all. */
#define MPU_KERNEL_GUARD_MARGIN     (4U * 1024U)

/** Size in bytes of a stack guard band (must match MPU_SIZE_32B). */
#define MPU_GUARD_SIZE              32U

/* Private variables ---------------------------------------------------------*/

/** Set once the MPU has been programmed; keeps the context-switch hook inert
  * if it ever runs before mpu_init(). */
static volatile uint8_t s_mpu_ready = 0U;

/* Linker-provided symbols ---------------------------------------------------*/
extern uint32_t _estack;  /**< Top of RAM / initial MSP. */
extern uint32_t _end;     /**< First byte above .bss (newlib heap start). */

/* Private function prototypes -----------------------------------------------*/
static void mpu_region_set(uint32_t region, uint32_t base, uint32_t size_field,
                           uint32_t attributes);
static void mpu_region_clear(uint32_t region);
static uint32_t mpu_is_sram(uint32_t addr);

/* Private user code ---------------------------------------------------------*/

/**
  * @brief  Program one MPU region and enable it.
  * @param  region     Region number (0..7).
  * @param  base       Region base address, must be aligned to the region size.
  * @param  size_field RASR SIZE encoding, i.e. log2(size in bytes) - 1.
  * @param  attributes RASR attribute bits (XN | AP | TEX/C/B/S).
  */
static void mpu_region_set(uint32_t region, uint32_t base, uint32_t size_field,
                           uint32_t attributes)
{
  MPU->RNR  = region;
  MPU->RBAR = (base & MPU_RBAR_ADDR_Msk);
  MPU->RASR = attributes
              | (size_field << MPU_RASR_SIZE_Pos)
              | MPU_RASR_ENABLE_Msk;
}

/**
  * @brief  Disable one MPU region.
  * @param  region Region number (0..7).
  */
static void mpu_region_clear(uint32_t region)
{
  MPU->RNR  = region;
  MPU->RASR = 0UL;
  MPU->RBAR = 0UL;
}

/**
  * @brief  Tell whether a guard band placed at @p addr would sit in SRAM.
  * @param  addr Candidate guard base address.
  * @retval 1 if the whole band fits inside SRAM1, SRAM2 or the SRAM2 alias.
  */
static uint32_t mpu_is_sram(uint32_t addr)
{
  uint32_t in_sram = 0U;

  if ((addr >= MPU_SRAM1_BASE) && (addr <= (MPU_SRAM1_END - MPU_GUARD_SIZE)))
  {
    in_sram = 1U;
  }
  else if ((addr >= MPU_SRAM2_ALIAS_BASE) &&
           (addr <= (MPU_SRAM2_ALIAS_END - MPU_GUARD_SIZE)))
  {
    in_sram = 1U;
  }
  else if ((addr >= MPU_SRAM2_BASE) && (addr <= (MPU_SRAM2_END - MPU_GUARD_SIZE)))
  {
    in_sram = 1U;
  }
  else
  {
    in_sram = 0U;
  }

  return in_sram;
}

/**
  * @brief  Program and enable the Cortex-M4 MPU. See mpu.h for the policy.
  */
void mpu_init(void)
{
  uint32_t guard_base;
  uint32_t region;

  s_mpu_ready = 0U;

  /* Make sure every outstanding memory access completes under the old
     configuration before the MPU is touched. */
  __DMB();

  /* Disable the MPU and wipe every region so no stale configuration (e.g. a
     warm reset from a bootloader) survives. */
  MPU->CTRL = 0UL;
  for (region = 0U; region < 8U; region++)
  {
    mpu_region_clear(region);
  }

  /* #0 Application code + rodata in flash: read-only, executable.
        Stops wild pointers from corrupting the image; the LastStates pool at
        0x08080000 is intentionally left outside this region so that
        HAL_FLASH_Program() keeps working. */
  mpu_region_set(MPU_REGION_FLASH, MPU_FLASH_BASE, MPU_SIZE_512KB,
                 MPU_AP_RO | MPU_ATTR_FLASH_MEM);

  /* #1 SRAM1: read/write, never executable (W^X). */
  mpu_region_set(MPU_REGION_SRAM1, MPU_SRAM1_BASE, MPU_SIZE_256KB,
                 MPU_AP_RW | MPU_ATTR_SRAM_MEM | MPU_XN);

  /* #2 SRAM2 seen through its 0x20040000 alias: read/write, never executable. */
  mpu_region_set(MPU_REGION_SRAM2_ALIAS, MPU_SRAM2_ALIAS_BASE, MPU_SIZE_64KB,
                 MPU_AP_RW | MPU_ATTR_SRAM_MEM | MPU_XN);

  /* #3 SRAM2 at its native 0x10000000 address: read/write, never executable. */
  mpu_region_set(MPU_REGION_SRAM2, MPU_SRAM2_BASE, MPU_SIZE_64KB,
                 MPU_AP_RW | MPU_ATTR_SRAM_MEM | MPU_XN);

  /* #4 Peripheral aperture: Device memory, never executable. */
  mpu_region_set(MPU_REGION_PERIPH, MPU_PERIPH_BASE, MPU_SIZE_512MB,
                 MPU_AP_RW | MPU_ATTR_DEVICE_MEM | MPU_XN);

  /* #5 Kernel (MSP) stack guard band. Installed only if it stays clear of the
        top of .bss / the newlib heap, so a large static footprint can never
        turn this hardening measure into a boot failure. */
  guard_base = (uint32_t)&_estack - MPU_KERNEL_STACK_WINDOW;
  if (guard_base >= ((uint32_t)&_end + MPU_KERNEL_GUARD_MARGIN))
  {
    mpu_region_set(MPU_REGION_KERNEL_GUARD, guard_base & ~(MPU_GUARD_SIZE - 1U),
                   MPU_SIZE_32B, MPU_AP_RO | MPU_ATTR_SRAM_MEM | MPU_XN);
  }

  /* #7 is programmed on every context switch by mpu_task_stack_guard_set(). */

  /* Report MPU violations as MemManage faults rather than letting them
     escalate directly to HardFault (ECSS-E-ST-40C fault containment). */
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;

  /* Enable the MPU, keeping the default system map as a background region for
     privileged accesses (PRIVDEFENA) and leaving it disabled for NMI/HardFault
     so a fault handler can always run. */
  MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;

  __DSB();
  __ISB();

  s_mpu_ready = 1U;
}

/**
  * @brief  Move the per-task stack guard onto the stack of the running task.
  * @param  stack_base Lowest address of the task stack (TCB pxStack).
  */
void mpu_task_stack_guard_set(const void *stack_base)
{
  uint32_t base;

  if (s_mpu_ready == 0U)
  {
    return;
  }

  /* Task stacks come from the FreeRTOS heap and are only 8-byte aligned, while
     an MPU region must be aligned to its own size. Round up into the stack so
     the band always lies inside the task's own memory. */
  base = ((uint32_t)stack_base + (MPU_GUARD_SIZE - 1U)) & ~(MPU_GUARD_SIZE - 1U);

  if (mpu_is_sram(base) != 0U)
  {
    mpu_region_set(MPU_REGION_TASK_GUARD, base, MPU_SIZE_32B,
                   MPU_AP_RO | MPU_ATTR_SRAM_MEM | MPU_XN);
  }
  else
  {
    mpu_region_clear(MPU_REGION_TASK_GUARD);
  }

  __DSB();
  __ISB();
}
