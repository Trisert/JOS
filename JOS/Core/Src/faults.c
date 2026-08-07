/**
  ******************************************************************************
  * @file    faults.c
  * @brief   Cortex-M4 fault containment for the RedPill OBSW.
  *
  * Out of reset the STM32L4 vectors HardFault/MemManage/BusFault/UsageFault to
  * stubs that spin forever, which on orbit means a silent, unobservable hang
  * until the external watchdog fires. This module replaces those stubs with
  * handlers that:
  *
  *   1. capture the stacked core registers (r0-r3, r12, lr, pc, xPSR) plus the
  *      SCB fault status registers (CFSR, HFSR, MMFAR, BFAR, AFSR, SHCSR,
  *      ICSR) into a fault_record_t,
  *   2. persist that record in the LastStates pool (internal Flash) so ground
  *      can post-mortem the failure after the reboot,
  *   3. issue NVIC_SystemReset() to return the OBSW to a known-good state.
  *
  * The same record type is used by the FreeRTOS stack-overflow hook.
  *
  * Standards: NASA-STD-8739.8 (fault containment / no silent failure),
  *            ECSS-E-ST-40C (software integrity, recorded failure context).
  ******************************************************************************
  */

#include "faults.h"

#include "main.h"           /* HAL + CMSIS core (SCB, NVIC_SystemReset)      */
#include "stm32l4xx_it.h"   /* prototypes of the exception handlers          */
#include "memory.h"         /* laststates_write()                            */
#include "obsw_types.h"     /* laststates_entry_t, TRIGGER_*                 */
#include "mpu.h"            /* mpu_memmanage_fault(): MemManage entry (W2-1) */

#include <stddef.h>
#include <string.h>

/* The record must survive the trip through a LastStates entry unchanged. */
_Static_assert(sizeof(fault_record_t) <= sizeof(((laststates_entry_t *)0)->context),
               "fault_record_t does not fit in a LastStates context blob");

/* The naked entry stubs below pass the fault id as an assembler literal. */
_Static_assert(FAULT_ID_HARDFAULT == 0, "HardFault stub literal out of sync");
_Static_assert(FAULT_ID_MEMMANAGE == 1, "MemManage stub literal out of sync");
_Static_assert(FAULT_ID_BUSFAULT == 2, "BusFault stub literal out of sync");
_Static_assert(FAULT_ID_USAGEFAULT == 3, "UsageFault stub literal out of sync");

/* A fault can hit in any operational state, and state_machine_get_state()
   takes a mutex, so it must never be called from an exception handler. The
   state fields of the LastStates entry are therefore marked unknown. */
#define FAULT_STATE_UNKNOWN   0xFFU

/* RAM windows of the STM32L496VGTx: SRAM1 256 KB @ 0x20000000 (with the SRAM2
   alias directly above it) and SRAM2 64 KB @ 0x10000000. Used to sanity-check
   the stack pointer before dereferencing it - a fault caused by a wild SP must
   not trigger a second fault inside the handler (which would lock the core up
   before anything is recorded). */
#define FAULT_SRAM1_BASE      0x20000000U
#define FAULT_SRAM1_END       0x20050000U   /* SRAM1 + SRAM2 alias */
#define FAULT_SRAM2_BASE      0x10000000U
#define FAULT_SRAM2_END       0x10010000U

/* Number of words the core stacks on exception entry (without FP context). */
#define FAULT_FRAME_WORDS     8U

static int fault_frame_is_readable(const uint32_t *frame)
{
    uint32_t addr = (uint32_t)frame;
    uint32_t len  = FAULT_FRAME_WORDS * sizeof(uint32_t);

    if ((frame == NULL) || ((addr & 3U) != 0U)) {
        return 0;
    }
    if ((addr >= FAULT_SRAM1_BASE) && ((addr + len) <= FAULT_SRAM1_END)) {
        return 1;
    }
    if ((addr >= FAULT_SRAM2_BASE) && ((addr + len) <= FAULT_SRAM2_END)) {
        return 1;
    }
    return 0;
}

static void fault_fill_scb(fault_record_t *rec)
{
    rec->cfsr  = SCB->CFSR;
    rec->hfsr  = SCB->HFSR;
    rec->mmfar = SCB->MMFAR;   /* meaningful only if CFSR.MMARVALID */
    rec->bfar  = SCB->BFAR;    /* meaningful only if CFSR.BFARVALID */
    rec->afsr  = SCB->AFSR;
    rec->shcsr = SCB->SHCSR;
    rec->icsr  = SCB->ICSR;
}

/* Serialise the record into the LastStates pool.
   The record is persisted here, in the exception path, because the only
   recovery action is the reset below. To honour the "must not block
   indefinitely" rule, laststates_write() bounds every Flash program/erase with
   the DWT cycle counter (CPU-clock based) rather than HAL_GetTick(): the
   SysTick interrupt cannot preempt a fault handler, so a GetTick()-based Flash
   timeout would never fire and would block forever if the controller stuck.
   The cycle-counter bound always advances, so a stuck controller is reported as
   a write failure and the reset (the real containment) still happens. */
static void fault_persist(const fault_record_t *rec, uint8_t trigger)
{
    laststates_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = HAL_GetTick();
    entry.state_from = FAULT_STATE_UNKNOWN;
    entry.state_to   = FAULT_STATE_UNKNOWN;
    entry.trigger    = trigger;
    memcpy(entry.context, rec, sizeof(*rec));

    /* Best effort: if the Flash write fails there is no alternative sink and
       the reset must happen regardless, so the status is deliberately
       discarded here (the reset itself is the containment action). */
    (void)laststates_write(&entry);
}

void fault_handlers_init(void)
{
    /* MemManage, BusFault and UsageFault are disabled out of reset; without
       this every such fault escalates to HardFault and the precise cause is
       only recoverable from HFSR.FORCED + CFSR. */
    SCB->SHCSR |= (SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_BUSFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);
    __DSB();
    __ISB();
}

void fault_capture(const uint32_t *frame, uint32_t fault_id, uint32_t exc_return)
{
    fault_record_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.magic      = FAULT_RECORD_MAGIC;
    rec.fault_id   = fault_id;
    rec.exc_return = exc_return;

    if (fault_frame_is_readable(frame)) {
        rec.r0   = frame[0];
        rec.r1   = frame[1];
        rec.r2   = frame[2];
        rec.r3   = frame[3];
        rec.r12  = frame[4];
        rec.lr   = frame[5];
        rec.pc   = frame[6];
        rec.xpsr = frame[7];
    }

    fault_fill_scb(&rec);
    fault_persist(&rec, TRIGGER_FAULT);

    NVIC_SystemReset();

    for (;;) {
        /* NVIC_SystemReset() does not return; guard against a failed reset. */
    }
}

void fault_log_stack_overflow(const char *task_name)
{
    fault_record_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.magic    = FAULT_RECORD_MAGIC;
    rec.fault_id = (uint32_t)FAULT_ID_STACK_OVERFLOW;
    rec.lr       = (uint32_t)__builtin_return_address(0);

    if (task_name != NULL) {
        /* Manual bounded copy: rec.task is already zeroed, so the name stays
           NUL-terminated even when it is truncated. */
        for (uint32_t i = 0U; (i < (FAULT_TASK_NAME_LEN - 1U)) && (task_name[i] != '\0'); i++) {
            rec.task[i] = task_name[i];
        }
    }

    fault_fill_scb(&rec);
    fault_persist(&rec, TRIGGER_STACK_OVERFLOW);

    NVIC_SystemReset();

    for (;;) {
        /* NVIC_SystemReset() does not return; guard against a failed reset. */
    }
}

/* ---------- Exception entry stubs ----------
 *
 * Each handler must be naked: the C prologue would push registers onto the
 * same stack that holds the exception frame, so the frame pointer has to be
 * taken before any compiler-generated code runs. EXC_RETURN bit 2 selects the
 * stack that was in use when the fault was taken (0 = MSP, 1 = PSP).
 *
 * These are strong definitions of the vector-table symbols declared weak in
 * Core/Startup/startup_stm32l496vgtx.s; the CubeMX `while (1)` stubs were
 * removed from Core/Src/stm32l4xx_it.c so that these take their place.
 */
#define FAULT_ENTRY_STUB(handler, id_literal)               \
    __attribute__((naked)) void handler(void)               \
    {                                                       \
        __asm volatile (                                    \
            ".syntax unified            \n"                 \
            "tst    lr, #4              \n"                 \
            "ite    eq                  \n"                 \
            "mrseq  r0, msp             \n"                 \
            "mrsne  r0, psp             \n"                 \
            "movs   r1, #" #id_literal "\n"                 \
            "mov    r2, lr              \n"                 \
            "b      fault_capture       \n"                 \
        );                                                  \
    }

FAULT_ENTRY_STUB(HardFault_Handler,  0)
FAULT_ENTRY_STUB(BusFault_Handler,   2)
FAULT_ENTRY_STUB(UsageFault_Handler, 3)

/* MemManage is the MPU's own fault (W2-1) and does NOT go through
   fault_capture(): that path programs Flash from handler context, whose HAL
   completion poll is bounded only by HAL_GetTick() — and SysTick cannot preempt
   an exception handler, so the timeout could never expire. mpu_memmanage_fault()
   instead stages the record (CFSR/MMFAR/PC/LR/xPSR/SP) in reset-persistent
   .noinit, resets, and the record is committed to LastStates at task level by
   mpu_fault_log_flush() early in the next boot. Same naked/EXC_RETURN contract
   as the stubs above; it never returns and never spins. */
__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile (
        ".syntax unified            \n"
        "tst    lr, #4              \n"
        "ite    eq                  \n"
        "mrseq  r0, msp             \n"
        "mrsne  r0, psp             \n"
        "b      mpu_memmanage_fault \n"
    );
}
