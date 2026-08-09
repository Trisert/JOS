/**
 * @file    fakes/cmsis_os.h
 * @brief   Host-test stand-in for the CMSIS-RTOS2 API (FreeRTOS wrapper).
 *
 * The flight build resolves cmsis_os.h from Middlewares/Third_Party/FreeRTOS.
 * That header drags in the whole FreeRTOS kernel and the Cortex-M port layer,
 * neither of which can be compiled on the x86/aarch64 test host. This fake
 * declares exactly the API surface JOS/App uses, so CMock can generate
 * mock_cmsis_os.c from it and the tests can drive the RTOS deterministically.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit testing on a representative host with
 * simulated interfaces), NASA-STD-8739.8 §7 (software verification).
 *
 * NOTE: keep the declarations here signature-compatible with CMSIS-RTOS2.
 *       Any divergence would make the tests verify something the flight
 *       software does not do.
 */
#ifndef JOS_TEST_FAKE_CMSIS_OS_H
#define JOS_TEST_FAKE_CMSIS_OS_H

#include <stdint.h>
#include <stddef.h>

/* The real CMSIS-RTOS2 FreeRTOS wrapper pulls in the kernel headers, so App/
 * code reaches macros such as pdMS_TO_TICKS through cmsis_os.h alone. Mirror
 * that here, otherwise modules that rely on the transitive include break. */
#include "FreeRTOS.h"

/* ---------- Status / priority ---------- */
typedef enum {
    osOK            =  0,
    osError         = -1,
    osErrorTimeout  = -2,
    osErrorResource = -3,
    osErrorParameter= -4
} osStatus_t;

typedef int32_t osPriority_t;

#define osPriorityNone          ((osPriority_t) 0)
#define osPriorityIdle          ((osPriority_t) 1)
#define osPriorityLow           ((osPriority_t) 8)
#define osPriorityBelowNormal   ((osPriority_t)16)
#define osPriorityNormal        ((osPriority_t)24)
#define osPriorityAboveNormal   ((osPriority_t)32)
#define osPriorityHigh          ((osPriority_t)40)
#define osPriorityRealtime      ((osPriority_t)48)

#define osWaitForever           0xFFFFFFFFU
#define osFlagsWaitAny          0x00000000U

/* ---------- Object ids ---------- */
typedef void *osThreadId_t;
typedef void *osMutexId_t;
typedef void *osSemaphoreId_t;
typedef void *osMessageQueueId_t;

typedef void (*osThreadFunc_t)(void *argument);

/* ---------- Attribute structures (CMSIS-RTOS2 layout) ---------- */
typedef struct {
    const char *name;
    uint32_t    attr_bits;
    void       *cb_mem;
    uint32_t    cb_size;
    void       *stack_mem;
    uint32_t    stack_size;
    osPriority_t priority;
    uint32_t    tz_module;
    uint32_t    reserved;
} osThreadAttr_t;

typedef struct {
    const char *name;
    uint32_t    attr_bits;
    void       *cb_mem;
    uint32_t    cb_size;
} osMutexAttr_t;

#define osMutexRecursive        0x00000001U
#define osMutexPrioInherit      0x00000002U
#define osMutexRobust           0x00000008U

/* ---------- Kernel ---------- */
/* App/obsw/watchdog.c asks the kernel whether the scheduler is running before
   it trusts xTaskGetTickCount(): a tick sampled from main() (pre-osKernelStart)
   reads 0 and stays there, so registrations made there must be re-seeded by
   the monitor. Mocking this is what lets a test drive both sides of that
   branch without a scheduler. */
typedef enum {
    osKernelInactive    = 0,
    osKernelReady       = 1,
    osKernelRunning     = 2,
    osKernelLocked      = 3,
    osKernelSuspended   = 4,
    osKernelError       = -1
} osKernelState_t;

osStatus_t      osKernelStart(void);
uint32_t        osKernelGetTickCount(void);
osKernelState_t osKernelGetState(void);

/* ---------- Threads ---------- */
osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr);
osThreadId_t osThreadGetId(void);
osStatus_t   osDelay(uint32_t ticks);

/* ---------- Mutexes ---------- */
osMutexId_t osMutexNew(const osMutexAttr_t *attr);
osStatus_t  osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);
osStatus_t  osMutexRelease(osMutexId_t mutex_id);

#endif /* JOS_TEST_FAKE_CMSIS_OS_H */
