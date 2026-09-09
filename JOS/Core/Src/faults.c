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
#include "dual_bank.h"      /* dual_bank_mark_boot_fault() (W2-2)            */

#include "FreeRTOS.h"       /* xPortGetFreeHeapSize(), xTaskGetSchedulerState */
#include "task.h"

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

/* Re-entrancy latch for the capture path.
 *
 * fault_capture() dereferences the stacked frame and programs Flash. Either
 * can itself fault (wild SP, wedged Flash controller, a BusFault on the
 * LastStates pool). On ARMv7-M a fault taken while already inside the
 * HardFault handler does not re-enter the handler: the core goes to LOCKUP,
 * where no instruction ever retires again and NVIC_SystemReset() - the entire
 * containment strategy of this module - becomes unreachable. That is a
 * permanent, unobservable hang, i.e. exactly the failure this file exists to
 * prevent.
 *
 * So on re-entry we skip everything that can fault and reset immediately. The
 * evidence from the *first* fault is worth less than a guaranteed reboot, and
 * hw_watchdog (IWDG) covers the residual case where even this reset is not
 * reached. Not reset before NVIC_SystemReset(): it lives in ordinary .bss and
 * is re-zeroed by startup on the way back up. */
static volatile uint32_t s_fault_nesting = 0U;

/* Unrecoverable-but-bounded exit: request a reset and, should the request not
 * take effect, let the IWDG do it. Never returns. */
static void fault_reset_now(void)
{
    NVIC_SystemReset();

    for (;;) {
        /* NVIC_SystemReset() does not return. If it somehow did, the IWDG
           (Core/Src/hw_watchdog.c) is deliberately NOT kicked here, so this
           spin is bounded by the watchdog timeout instead of being forever. */
    }
}

/* Idempotent, and deliberately does NOT write DWT->CYCCNT: the counter reads
 * 0 out of reset and only advances once CYCCNTENA is set, so there is nothing
 * stale to clear - while a reset here would restart the epoch under whichever
 * caller runs second and silently re-order the forensic trail. See
 * fault_timestamp_ms() below. */
void fault_dwt_enable(void)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    }
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/* ---------- Timestamp source for a fault record (finding C5) ----------
 *
 * The records used to be stamped with a bare HAL_GetTick() while nothing in
 * the image ever incremented uwTick: SysTick_Handler() in stm32l4xx_it.c was
 * empty and HAL_IncTick() had no callers, so EVERY record - fault, boot CRC,
 * SEU, dual bank - carried timestamp 0 and the post-mortem trail could not be
 * ordered at all. TIM6 (the HAL timebase, see stm32l4xx_hal_timebase_tim.c)
 * calls HAL_IncTick() while SysTick_Handler() calls only the FreeRTOS tick
 * hook, so HAL_GetTick() is a real millisecond counter.
 *
 * A fault handler still cannot rely on it alone: a fault taken before
 * HAL_Init() (MPU setup, clock configuration, boot-CRC verification) sees
 * uwTick == 0. In that window the DWT cycle counter is used instead.
 *
 * Two things make that fallback actually work (Kilo #26, comment id
 * 3741110984). First, the counter is started by fault_dwt_enable() from
 * mpu_init(), the very first call in main() - the previous version enabled it
 * HERE and divided it by SystemCoreClock/1000 about six cycles later, so it
 * returned 0, the exact value the fallback exists to eliminate, and only
 * produced anything useful when some other module happened to start DWT
 * first. Second, CYCCNT is never reset here: doing so would restart the epoch
 * on every fault and make the stamps unorderable again.
 *
 * The epoch is therefore "ms since mpu_init()" and it wraps every ~54 s at
 * 80 MHz, which is NOT the same clock as HAL_GetTick(). The record says which
 * one it is in fault_record_t.ts_source so ground never has to guess. */
static uint32_t fault_timestamp_ms(uint32_t *source)
{
    uint32_t tick = HAL_GetTick();
    uint32_t cycles_per_ms;

    if (tick != 0U) {
        *source = (uint32_t)FAULT_TS_SOURCE_TICK;
        return tick;
    }

    /* Pre-tick: derive the timestamp from the cycle counter. Enabling it here
       is a last-resort backstop for a fault that somehow precedes mpu_init();
       it does NOT reset CYCCNT, so an already-running counter keeps its
       epoch. */
    fault_dwt_enable();

    cycles_per_ms = SystemCoreClock / 1000U;
    if (cycles_per_ms == 0U) {          /* never on this part; no div-by-zero */
        *source = (uint32_t)FAULT_TS_SOURCE_NONE;
        return 0U;
    }

    *source = (uint32_t)FAULT_TS_SOURCE_DWT;
    return DWT->CYCCNT / cycles_per_ms;
}

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
static void fault_persist(fault_record_t *rec, uint8_t trigger)
{
    laststates_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = fault_timestamp_ms(&rec->ts_source);
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

    /* Second fault while handling the first: reset, do not recurse into
       LOCKUP. See s_fault_nesting above. */
    if (s_fault_nesting != 0U) {
        fault_reset_now();
    }
    s_fault_nesting = 1U;

    /* Dual-bank fallback evidence (W2-2) FIRST: RAM-only, ISR-safe, no Flash
       and no HAL, so it cannot block or fault. It must be recorded before the
       Flash write below, which may legitimately fail on a corrupted image -
       exactly the case the golden-image fallback exists for. */
    dual_bank_mark_boot_fault();

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

    /* Distinguish the escalated stack-overflow path from a generic HardFault.
       HFSR.FORCED says this HardFault was escalated from a configurable fault;
       CFSR.MSTKERR says the escalation happened because the core could not
       stack the exception frame - which is what a task overflowing into its
       read-only MPU guard band produces. Both bits are already in `rec`, so
       the raw registers stay available to ground either way. */
    if ((fault_id == (uint32_t)FAULT_ID_HARDFAULT) &&
        ((rec.hfsr & SCB_HFSR_FORCED_Msk) != 0UL) &&
        ((rec.cfsr & SCB_CFSR_MSTKERR_Msk) != 0UL)) {
        rec.fault_id = (uint32_t)FAULT_ID_HARDFAULT_STACKING;
    }

    fault_persist(&rec, TRIGGER_FAULT);

    fault_reset_now();
}

void fault_log_stack_overflow(const char *task_name)
{
    fault_record_t rec;

    /* Same latch as fault_capture(): the FreeRTOS overflow hook also programs
       Flash, and it can be reached from a task whose stack is already gone. */
    if (s_fault_nesting != 0U) {
        fault_reset_now();
    }
    s_fault_nesting = 1U;

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

    fault_reset_now();
}

/* ---------- Deferred malloc-failure record (vApplicationMallocFailedHook) ---
 *
 * The hook runs in task context after pvPortMalloc() failed, i.e. with the
 * heap exhausted. laststates_write() is unusable here: it takes the pool
 * mutex with osWaitForever (deadlock if the scheduler is suspended or the
 * holder can never run to release it) and programs/erases internal Flash
 * (millisecond-scale, with failure modes of its own) — all while the
 * allocator that just failed is the only thing between this hook and the
 * rest of the system. So this path never touches the mutex, the Flash
 * controller, or the frame pointer: it stages the two heap watermarks in a
 * reset-persistent .noinit slot (same contract as mpu.c's MemManage staging)
 * and resets. fault_malloc_flush() commits the slot to LastStates at task
 * level on the next boot, where blocking is legal. The wire layout of the
 * committed entry is unchanged (fault_record_t with r0 = free heap,
 * r1 = min-ever-free under TRIGGER_MALLOC_FAILED), so the ground decoder is
 * untouched.
 *
 * Fields that need an exception frame or a live tick base (exc_return,
 * stacked registers, SCB snapshot, ts_source) stay zero/NONE: there is no
 * frame here and the stamp clock dies with this boot. */

#define FAULT_MALLOC_MAGIC      0x464D414CUL   /* "FMAL" */

typedef struct
{
    uint32_t magic;       /* FAULT_MALLOC_MAGIC when a record is staged */
    uint32_t free_heap;   /* xPortGetFreeHeapSize() at failure time     */
    uint32_t min_ever;    /* xPortGetMinimumEverFreeHeapSize() ditto    */
    uint32_t chk;         /* XOR of the fields above, for validation    */
} fault_malloc_stage_t;

/* Reset-persistent by construction: .noinit is NOLOAD, so the startup code
   never zeroes it and the contents survive the NVIC_SystemReset() below. */
static fault_malloc_stage_t s_malloc_stage __attribute__((section(".noinit")));

static uint32_t fault_malloc_checksum(const fault_malloc_stage_t *s)
{
    return s->magic ^ s->free_heap ^ s->min_ever;
}

void fault_log_malloc_failed(void)
{
    /* Dedicated latch, not s_fault_nesting: that guards the exception path,
       this guards the heap-exhaustion path. A second failure while staging
       the first resets instead of recursing — with no heap left, even the
       staging stores must not be re-entered. Lives in .bss, re-zeroed by
       startup on the way back up. */
    static volatile uint32_t s_malloc_nesting = 0U;

    if (s_malloc_nesting != 0U) {
        fault_reset_now();
    }
    s_malloc_nesting = 1U;

    /* Plain variable reads only: no lock, no Flash, no frame-pointer chase.
       The heap watermarks are safe to sample even with the scheduler
       suspended, and __builtin_return_address() is deliberately NOT used —
       under optimisation the hook's caller frame is not guaranteed to exist,
       so it can return garbage or fault. */
    s_malloc_stage.magic     = FAULT_MALLOC_MAGIC;
    s_malloc_stage.free_heap = (uint32_t)xPortGetFreeHeapSize();
    s_malloc_stage.min_ever  = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    s_malloc_stage.chk       = fault_malloc_checksum(&s_malloc_stage);

    /* Make sure the slot has left the write buffer before the core resets. */
    __DSB();

    fault_reset_now();
}

/* Commit a staged malloc-failure record to the LastStates pool. Task-level
   boot context only (called from main(), next to mpu_fault_log_flush()):
   laststates_write() may block on the pool mutex and program Flash here.
   Returns 1 persisted, 0 nothing staged, -1 staged but the write failed. */
int fault_malloc_flush(void)
{
    fault_record_t rec;
    laststates_entry_t entry;
    int rc;

    if ((s_malloc_stage.magic != FAULT_MALLOC_MAGIC) ||
        (s_malloc_stage.chk != fault_malloc_checksum(&s_malloc_stage))) {
        /* Nothing staged, or the slot did not survive (cold boot with random
           SRAM contents). Clear it so a stale pattern cannot be replayed. */
        s_malloc_stage.magic = 0U;
        return 0;
    }

    memset(&rec, 0, sizeof(rec));
    rec.magic    = FAULT_RECORD_MAGIC;
    rec.fault_id = (uint32_t)FAULT_ID_MALLOC_FAILED;
    /* Same positional layout as before the deferral: r0 = free heap,
       r1 = min-ever-free; the ground decoder reads them positionally and
       they carry no register meaning (no exception frame was stacked).
       ts_source stays NONE: the fault predates this boot's tick base,
       exactly like the MPU flush. */
    rec.r0 = s_malloc_stage.free_heap;
    rec.r1 = s_malloc_stage.min_ever;

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = 0U;
    entry.state_from = FAULT_STATE_UNKNOWN;
    entry.state_to   = FAULT_STATE_UNKNOWN;
    entry.trigger    = TRIGGER_MALLOC_FAILED;
    memcpy(entry.context, &rec, sizeof(rec));

    rc = laststates_write(&entry);

    /* One entry per failure: drop the slot whether or not the write worked,
       so a boot loop cannot keep re-filling the pool with the same event. */
    s_malloc_stage.magic = 0U;
    s_malloc_stage.chk   = 0U;

    return (rc == 0) ? 1 : -1;
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
