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

#include <stdint.h>

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
  *   - The LastStates flash pool (0x08080000, 8 KB) is read/write but
  *     Execute-Never, so a persisted payload can never be fetched as code.
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
  * @brief  Tell whether the kernel (MSP) stack guard band was installed.
  *
  * mpu_init() skips region #5 when the guard would land inside .bss or the
  * newlib heap, so a large static footprint can never turn this hardening
  * measure into a boot failure. The skip must not be silent: callers can use
  * this to raise a housekeeping flag / telemetry point instead.
  *
  * @retval 1 the guard band is active, 0 it was skipped (or mpu_init() has
  *         not run yet).
  */
uint8_t mpu_kernel_guard_installed(void);

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

/**
  * @brief  MemManage fault entry point: stage the fault, then reset.
  *
  * Called from MemManage_Handler(). Captures the fault status registers and
  * the exception stack frame into a reset-persistent RAM record, then issues
  * NVIC_SystemReset(). It never returns and never spins: an MPU violation
  * must not turn silent corruption into a silent hang (ECSS-E-ST-40C fault
  * containment).
  *
  * The record is *not* written to flash here on purpose. Programming the
  * LastStates pool means HAL_FLASH_Program(), whose completion poll is only
  * bounded by HAL_GetTick() — and SysTick cannot preempt this handler, so the
  * timeout can never expire. Persisting is therefore deferred to
  * mpu_fault_log_flush() at task level on the next boot.
  *
  * @param  frame Exception stack frame (MSP or PSP, selected from EXC_RETURN).
  */
void mpu_memmanage_fault(const uint32_t *frame);

/**
  * @brief  Commit a staged MemManage fault record to the LastStates pool.
  *
  * Call once at boot, after laststates_init(), from task/thread context. If
  * the previous run died on an MPU violation, one LastStates entry tagged
  * TRIGGER_MPU_FAULT is written with CFSR / MMFAR / PC / LR / xPSR / SP.
  *
  * @retval 1 a record was found and persisted, 0 nothing was staged,
  *         -1 the record was staged but could not be written to flash.
  */
int mpu_fault_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* MPU_H */
