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
  *   #6  0x08080000    8 KB  LastStates pool     RW,  Execute-Never
  *   #7  <dynamic>    32 B   Running-task guard  RO,  Execute-Never
  *
  * W^X and the LastStates pool: region #0 stops exactly at 0x0807FFFF so that
  * HAL_FLASH_Program() can still write the pool at 0x08080000. Leaving the
  * pool on the default system map would leave it *executable* (PRIVDEFENA=1),
  * and laststates_write() persists a caller-supplied payload — i.e. a
  * write-then-execute primitive in flash. Region #6 closes that hole: the pool
  * is readable and writable, but Execute-Never.
  *
  * Deliberately *not* covered:
  *   - The SRAM bit-band alias at 0x22000000: a bit-band fetch expands one bit
  *     per word, so it is not a usable code-injection path.
  *   - The FRAM is an external SPI device, not memory mapped, so it cannot be
  *     fenced by the MPU; it is protected by the driver API instead.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "mpu.h"

#include <stdint.h>
#include <string.h>

#include "stm32l4xx.h"

#include "memory.h"      /* laststates_write()   */
#include "obsw_types.h"  /* laststates_entry_t   */

/* Private define ------------------------------------------------------------*/

/* Region numbers ------------------------------------------------------------*/
#define MPU_REGION_FLASH            0U
#define MPU_REGION_SRAM1            1U
#define MPU_REGION_SRAM2_ALIAS      2U
#define MPU_REGION_SRAM2            3U
#define MPU_REGION_PERIPH           4U
#define MPU_REGION_KERNEL_GUARD     5U
#define MPU_REGION_LASTSTATES       6U
#define MPU_REGION_TASK_GUARD       7U

/* Address map ---------------------------------------------------------------*/
#define MPU_FLASH_BASE              0x08000000UL   /* 512 KB of application code */
#define MPU_FLASH_SIZE              (512UL * 1024UL)
#define MPU_SRAM1_BASE              0x20000000UL   /* 256 KB                     */
#define MPU_SRAM1_END               0x20040000UL
#define MPU_SRAM2_ALIAS_BASE        0x20040000UL   /* 64 KB, contiguous alias    */
#define MPU_SRAM2_ALIAS_END         0x20050000UL
#define MPU_SRAM2_BASE              0x10000000UL   /* 64 KB, parity-protected    */
#define MPU_SRAM2_END               0x10010000UL
#define MPU_PERIPH_BASE             0x40000000UL   /* 512 MB peripheral aperture */
#define MPU_LASTSTATES_BASE         0x08080000UL   /* 8 KB LastStates flash pool */
#define MPU_LASTSTATES_SIZE         (8UL * 1024UL)

/* RASR SIZE field encoding: log2(size) - 1 ----------------------------------*/
#define MPU_SIZE_32B                4U
#define MPU_SIZE_8KB                12U
#define MPU_SIZE_64KB               15U
#define MPU_SIZE_256KB              17U
#define MPU_SIZE_512KB              18U
#define MPU_SIZE_512MB              28U

/* An MPU region must be aligned to its own size, and the pool must sit exactly
   where the executable flash region stops. Both are asserted at compile time so
   that moving the pool can never silently re-open the W^X hole. */
_Static_assert((MPU_LASTSTATES_BASE % MPU_LASTSTATES_SIZE) == 0UL,
               "LastStates pool must be 8 KB aligned to be an MPU region");
_Static_assert(MPU_LASTSTATES_BASE == (MPU_FLASH_BASE + MPU_FLASH_SIZE),
               "LastStates pool must start where executable region #0 ends");
_Static_assert(((uint32_t)LASTSTATES_MAX_ENTRIES *
                (uint32_t)LASTSTATES_ENTRY_SIZE) == MPU_LASTSTATES_SIZE,
               "LastStates pool size must match MPU region #6 size");

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

/** Marks the staged fault record as valid ("MPUF"). */
#define MPU_FAULT_MAGIC             0x4D505546UL

/* Private types -------------------------------------------------------------*/

/**
  * @brief Reset-persistent MemManage fault record.
  *
  * Lives in .noinit, which the linker places outside .bss so the startup code
  * never zeroes it: the contents survive the NVIC_SystemReset() issued by
  * mpu_memmanage_fault() and are picked up by mpu_fault_log_flush() on the
  * next boot.
  */
typedef struct
{
  uint32_t magic;  /**< MPU_FAULT_MAGIC when a record is staged. */
  uint32_t cfsr;   /**< SCB->CFSR at fault time.                 */
  uint32_t mmfar;  /**< Faulting address (valid if MMARVALID).   */
  uint32_t pc;     /**< Stacked return address.                  */
  uint32_t lr;     /**< Stacked link register.                   */
  uint32_t psr;    /**< Stacked xPSR.                            */
  uint32_t sp;     /**< Exception stack frame pointer.           */
  uint32_t chk;    /**< XOR of the fields above, for validation. */
} mpu_fault_record_t;

/* Private variables ---------------------------------------------------------*/

/** Set once the MPU has been programmed; keeps the context-switch hook inert
  * if it ever runs before mpu_init(). */
static volatile uint8_t s_mpu_ready = 0U;

/** Set when region #5 (kernel stack guard) was actually installed, so the
  * skip is observable instead of silent. */
static volatile uint8_t s_kernel_guard_installed = 0U;

/** Staged fault record. Not zero-initialised on purpose: see .noinit in
  * STM32L496VGTX_FLASH.ld. */
static mpu_fault_record_t s_fault_record __attribute__((section(".noinit")));

/* Linker-provided symbols ---------------------------------------------------*/
extern uint32_t _estack;  /**< Top of RAM / initial MSP. */
extern uint32_t _end;     /**< First byte above .bss (newlib heap start). */

/* Private function prototypes -----------------------------------------------*/
static void mpu_region_set(uint32_t region, uint32_t base, uint32_t size_field,
                           uint32_t attributes);
static void mpu_region_clear(uint32_t region);
static uint32_t mpu_is_sram(uint32_t addr);
static uint32_t mpu_fault_checksum(const mpu_fault_record_t *rec);

/* Private user code ---------------------------------------------------------*/

/**
  * @brief  Program one MPU region and enable it.
  *
  * The region number is written through RBAR's VALID/REGION fields rather than
  * relying on a previously latched RNR, so the operation does not depend on
  * global MPU state and cannot be corrupted by a preempting caller. RASR is
  * cleared first: while the base address changes, the old size/attributes must
  * not stay live over the new base.
  *
  * @param  region     Region number (0..7).
  * @param  base       Region base address, must be aligned to the region size.
  * @param  size_field RASR SIZE encoding, i.e. log2(size in bytes) - 1.
  * @param  attributes RASR attribute bits (XN | AP | TEX/C/B/S).
  */
static void mpu_region_set(uint32_t region, uint32_t base, uint32_t size_field,
                           uint32_t attributes)
{
  MPU->RNR  = region;
  MPU->RASR = 0UL;
  MPU->RBAR = (base & MPU_RBAR_ADDR_Msk)
              | MPU_RBAR_VALID_Msk
              | (region & MPU_RBAR_REGION_Msk);
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
  MPU->RBAR = MPU_RBAR_VALID_Msk | (region & MPU_RBAR_REGION_Msk);
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
  * @brief  XOR checksum over the payload of a staged fault record.
  * @param  rec Record to checksum.
  * @retval Checksum value that must equal rec->chk for the record to be valid.
  */
static uint32_t mpu_fault_checksum(const mpu_fault_record_t *rec)
{
  return rec->magic ^ rec->cfsr ^ rec->mmfar ^ rec->pc ^ rec->lr ^
         rec->psr ^ rec->sp;
}

/**
  * @brief  Program and enable the Cortex-M4 MPU. See mpu.h for the policy.
  */
void mpu_init(void)
{
  uint32_t guard_base;
  uint32_t region;

  s_mpu_ready = 0U;
  s_kernel_guard_installed = 0U;

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
        Stops wild pointers from corrupting the image. The region stops at
        0x0807FFFF so HAL_FLASH_Program() can write the LastStates pool; the
        pool itself is fenced Execute-Never by region #6 below. */
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
        turn this hardening measure into a boot failure. The outcome is latched
        in s_kernel_guard_installed so the skip is reportable. */
  guard_base = (uint32_t)&_estack - MPU_KERNEL_STACK_WINDOW;
  if (guard_base >= ((uint32_t)&_end + MPU_KERNEL_GUARD_MARGIN))
  {
    mpu_region_set(MPU_REGION_KERNEL_GUARD, guard_base & ~(MPU_GUARD_SIZE - 1U),
                   MPU_SIZE_32B, MPU_AP_RO | MPU_ATTR_SRAM_MEM | MPU_XN);
    s_kernel_guard_installed = 1U;
  }

  /* #6 LastStates flash pool: writable (HAL_FLASH_Program) but Execute-Never,
        so persisted payload bytes can never be fetched as instructions. */
  mpu_region_set(MPU_REGION_LASTSTATES, MPU_LASTSTATES_BASE, MPU_SIZE_8KB,
                 MPU_AP_RW | MPU_ATTR_FLASH_MEM | MPU_XN);

  /* #7 is programmed on every context switch by mpu_task_stack_guard_set(). */

  /* Report MPU violations as MemManage faults rather than letting them
     escalate directly to HardFault (ECSS-E-ST-40C fault containment).
     MemManage_Handler() stages the fault and resets; it never spins. */
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
  * @brief  Tell whether the kernel (MSP) stack guard band was installed.
  * @retval 1 when region #5 is active, 0 when it was skipped.
  */
uint8_t mpu_kernel_guard_installed(void)
{
  return s_kernel_guard_installed;
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

/**
  * @brief  MemManage fault entry point: stage the fault, then reset.
  *
  * Bounded by construction — a handful of stores, a DSB and a reset request.
  * Nothing here can block: no flash programming (HAL_FLASH_Program()'s
  * completion poll is bounded only by HAL_GetTick(), and SysTick cannot
  * preempt this handler, so its timeout could never expire) and no spin loop.
  *
  * @param  frame Exception stack frame (MSP or PSP, per EXC_RETURN bit 2).
  */
void mpu_memmanage_fault(const uint32_t *frame)
{
  s_fault_record.magic = MPU_FAULT_MAGIC;
  s_fault_record.cfsr  = SCB->CFSR;
  s_fault_record.mmfar = ((SCB->CFSR & SCB_CFSR_MMARVALID_Msk) != 0UL)
                         ? SCB->MMFAR : 0xFFFFFFFFUL;
  s_fault_record.sp    = (uint32_t)frame;

  /* The stacked frame is only trustworthy if the fault did not happen while
     stacking it (CFSR MSTKERR); guard the dereference either way. */
  if (frame != NULL)
  {
    s_fault_record.pc  = frame[6];
    s_fault_record.lr  = frame[5];
    s_fault_record.psr = frame[7];
  }
  else
  {
    s_fault_record.pc  = 0UL;
    s_fault_record.lr  = 0UL;
    s_fault_record.psr = 0UL;
  }

  s_fault_record.chk = mpu_fault_checksum(&s_fault_record);

  /* Make sure the record has left the write buffer before the core resets. */
  __DSB();

  NVIC_SystemReset();

  /* NVIC_SystemReset() does not return. */
  for (;;)
  {
  }
}

/**
  * @brief  Commit a staged MemManage fault record to the LastStates pool.
  * @retval 1 persisted, 0 nothing staged, -1 staged but the flash write failed.
  */
int mpu_fault_log_flush(void)
{
  laststates_entry_t entry;
  uint32_t words[6];
  int rc;

  if ((s_fault_record.magic != MPU_FAULT_MAGIC) ||
      (s_fault_record.chk != mpu_fault_checksum(&s_fault_record)))
  {
    /* Nothing staged, or the record did not survive (e.g. cold boot with
       random SRAM contents). Clear it so a stale pattern cannot be replayed. */
    s_fault_record.magic = 0UL;
    return 0;
  }

  (void)memset(&entry, 0, sizeof(entry));
  entry.timestamp  = 0U;    /* the fault predates this boot's tick base */
  entry.state_from = 0xFFU; /* unknown: the fault aborted the previous run */
  entry.state_to   = 0xFFU;
  entry.trigger    = (uint8_t)TRIGGER_MPU_FAULT;

  words[0] = s_fault_record.cfsr;
  words[1] = s_fault_record.mmfar;
  words[2] = s_fault_record.pc;
  words[3] = s_fault_record.lr;
  words[4] = s_fault_record.psr;
  words[5] = s_fault_record.sp;
  (void)memcpy(entry.context, words, sizeof(words));

  rc = laststates_write(&entry);

  /* One entry per fault: drop the record whether or not the write worked, so a
     boot loop cannot keep re-filling the pool with the same event. */
  s_fault_record.magic = 0UL;
  s_fault_record.chk   = 0UL;

  return (rc == 0) ? 1 : -1;
}
