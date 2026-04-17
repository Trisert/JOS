#ifndef CMSIS_OS_H_COMPAT
#define CMSIS_OS_H_COMPAT

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef TaskHandle_t osThreadId_t;
typedef SemaphoreHandle_t osMutexId_t;
typedef QueueHandle_t osMessageQueueId_t;
typedef TimerHandle_t osTimerId_t;
typedef uint32_t osPriority_t;

#define osPriorityLow          (1)
#define osPriorityBelowNormal  (2)
#define osPriorityNormal       (3)
#define osPriorityAboveNormal  (4)
#define osPriorityHigh         (5)
#define osPriorityRealtime     (6)
#define osPriorityError        (0x7FFFFFFF)

#define osWaitForever          portMAX_DELAY
#define osKernelInitialize()   (0)
#define osKernelStart()        vTaskStartScheduler()

typedef struct {
    const char *name;
    uint32_t    stack_size;
    osPriority_t priority;
    uint32_t    attr_bits;
    void       *cb_mem;
    uint32_t    cb_size;
    void       *tz_module;
} osThreadAttr_t;

#define osThreadPrioInherit    (1U << 0)

typedef struct {
    const char *name;
    uint32_t    attr_bits;
} osMutexAttr_t;

static inline osThreadId_t osThreadNew(void (*func)(void *), void *arg, const osThreadAttr_t *attr)
{
    TaskHandle_t handle;
    UBaseType_t prio = tskIDLE_PRIORITY + 2;
    if (attr && attr->priority > 0) {
        prio = (UBaseType_t)attr->priority;
    }
    uint32_t stack = (attr && attr->stack_size) ? attr->stack_size : 4096;
    BaseType_t rc = xTaskCreate(func, (attr && attr->name) ? attr->name : "task",
                                stack / sizeof(StackType_t), arg, prio, &handle);
    (void)rc;
    return handle;
}

static inline osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    (void)attr;
    return xSemaphoreCreateMutex();
}

static inline int osMutexAcquire(osMutexId_t id, uint32_t timeout)
{
    return (xSemaphoreTake(id, timeout) == pdTRUE) ? 0 : -1;
}

static inline int osMutexRelease(osMutexId_t id)
{
    return (xSemaphoreGive(id) == pdTRUE) ? 0 : -1;
}

static inline uint32_t osDelay(uint32_t ticks)
{
    vTaskDelay(ticks);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* CMSIS_OS_H_COMPAT */
