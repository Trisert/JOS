#ifndef FAULTS_H
#define FAULTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Fault record ----------
 *
 * Every fault path (Cortex-M4 exception or FreeRTOS stack-overflow hook)
 * serialises this record into the `context` blob of a LastStates entry before
 * the MCU is reset, so the failure can be reconstructed from ground after the
 * reboot (NASA-STD-8739.8 fault containment, ECSS-E-ST-40C integrity).
 */

/* "FLT1" - record format marker, bump the last digit on layout changes. */
#define FAULT_RECORD_MAGIC   0x464C5431U

/* Bytes reserved for the offending task name (FreeRTOS names are short). */
#define FAULT_TASK_NAME_LEN  16U

/* Which fault path produced the record. Values are also encoded as literals
   in the naked exception entry stubs in faults.c (static-asserted there). */
typedef enum {
    FAULT_ID_HARDFAULT      = 0,
    FAULT_ID_MEMMANAGE      = 1,
    FAULT_ID_BUSFAULT       = 2,
    FAULT_ID_USAGEFAULT     = 3,
    FAULT_ID_STACK_OVERFLOW = 4,
    /* HardFault that was *derived* from a failure to stack the exception
       frame (HFSR.FORCED && CFSR.MSTKERR). The canonical producer is a task
       stack overflow: the offending push faults against the read-only MPU
       guard band at the bottom of the task stack, and exception entry to
       MemManage then tries to stack the frame at PSP-32, i.e. back inside the
       band that just faulted, so the MemManage entry itself faults and
       escalates. Recorded under its own id because the generic HardFault code
       would hide the single most likely in-flight failure mode behind the
       least specific label. Never encoded in an entry stub - fault_capture()
       derives it from HFSR/CFSR. */
    FAULT_ID_HARDFAULT_STACKING = 5,
} fault_id_t;

typedef struct {
    uint32_t magic;       /* FAULT_RECORD_MAGIC                              */
    uint32_t fault_id;    /* fault_id_t                                      */
    uint32_t exc_return;  /* EXC_RETURN (LR on exception entry), 0 if n/a    */
    /* Registers stacked by the core on exception entry (0 when not stacked) */
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;
    /* System Control Block fault status registers */
    uint32_t cfsr;        /* Configurable Fault Status Register              */
    uint32_t hfsr;        /* HardFault Status Register                       */
    uint32_t mmfar;       /* valid only when CFSR.MMARVALID is set           */
    uint32_t bfar;        /* valid only when CFSR.BFARVALID is set           */
    uint32_t afsr;        /* Auxiliary Fault Status Register                 */
    uint32_t shcsr;       /* System Handler Control and State Register       */
    uint32_t icsr;        /* Interrupt Control and State Register            */
    char     task[FAULT_TASK_NAME_LEN]; /* offending task, "" when unknown   */
} fault_record_t;

/* ---------- Public API ---------- */

/* Enable the MemManage, BusFault and UsageFault handlers (they are disabled
   out of reset, which escalates every such fault to HardFault).
   Call once during boot, before the scheduler starts. */
void fault_handlers_init(void);

/* Record a fault and reset the MCU. Called from the naked exception stubs
   with the stacked exception frame; not intended for direct use. */
void fault_capture(const uint32_t *frame, uint32_t fault_id, uint32_t exc_return);

/* Record a FreeRTOS stack overflow (task name may be NULL) and reset the MCU.
   Called from vApplicationStackOverflowHook(). Does not return. */
void fault_log_stack_overflow(const char *task_name);

#ifdef __cplusplus
}
#endif

#endif /* FAULTS_H */
