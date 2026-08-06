/**
  ******************************************************************************
  * @file    mpu.h
  * @brief   Cortex-M4 Memory Protection Unit (MPU) configuration for RedPill
  *          OBSW — kernel / task isolation and W^X enforcement (W2-1).
  *
  * Hardening card W2-1 (see docs/dev/hardening.md §2.1).
  * Standards: NASA Power of Ten #4, JPL-182, ECSS-E-ST-40C.
  ******************************************************************************
  */

#ifndef MPU_H
#define MPU_H

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Program and enable the Cortex-M4 MPU.
  *
  * Must be called as early as possible in main(), before any RTOS object is
  * created and well before osKernelStart(). Idempotent: it fully re-programs
  * the region set on every call.
  *
  * Enforced policy (privileged background map left enabled, PRIVDEFENA=1):
  *   - Flash code/rodata (0x08000000, 512 KB) is read-only + executable.
  *   - All SRAM (SRAM1, SRAM2 and its 0x20040000 alias) is read/write but
  *     Execute-Never: no code can be executed from RAM.
  *   - The peripheral aperture is Device + Execute-Never.
  *   - A read-only guard band is placed below the kernel (MSP) stack so a
  *     kernel stack overflow faults instead of silently corrupting .bss/heap.
  *
  * @note MemManage faults are enabled so violations are reported through
  *       MemManage_Handler() instead of escalating straight to HardFault.
  */
void mpu_init(void);

/**
  * @brief  Move the per-task stack guard region onto the given task stack.
  *
  * Called from traceTASK_SWITCHED_IN() (see FreeRTOSConfig.h) on every context
  * switch, so the running task always has a read-only guard band at the low
  * (overflow) end of its own stack. A task that overruns its stack therefore
  * traps on the offending write instead of corrupting the neighbouring task
  * stack, a TCB or the kernel heap.
  *
  * The band is read-only rather than no-access on purpose: FreeRTOS'
  * configCHECK_FOR_STACK_OVERFLOW==2 pattern check *reads* the bottom of the
  * stack, and those reads must keep working.
  *
  * @param  stack_base Lowest address of the task stack (TCB pxStack). Ignored
  *                    if it does not point into SRAM or if mpu_init() has not
  *                    run yet.
  */
void mpu_task_stack_guard_set(const void *stack_base);

#ifdef __cplusplus
}
#endif

#endif /* MPU_H */
