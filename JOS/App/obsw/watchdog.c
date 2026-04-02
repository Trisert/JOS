#include "watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* ---------- Monitored task entry ---------- */
typedef struct {
    osThreadId_t handle;
    uint32_t     expected_period_ms;
    uint32_t     last_tick;
    uint8_t      registered;
} wdg_entry_t;

static wdg_entry_t wdg_tasks[WDG_MAX_TASKS];
static osMutexId_t wdg_mutex;

/* ---------- Public functions ---------- */

void watchdog_monitor_init(void)
{
    const osMutexAttr_t mtx_attrs = {
        .name      = "wdgMtx",
        .attr_bits = osMutexPrioInherit,
    };
    wdg_mutex = osMutexNew(&mtx_attrs);

    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        wdg_tasks[i].registered = 0;
    }
}

int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms)
{
    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (!wdg_tasks[i].registered) {
            wdg_tasks[i].handle            = handle;
            wdg_tasks[i].expected_period_ms = expected_period_ms;
            wdg_tasks[i].last_tick          = xTaskGetTickCount();
            wdg_tasks[i].registered         = 1;
            osMutexRelease(wdg_mutex);
            return 0;
        }
    }
    osMutexRelease(wdg_mutex);
    return -1;  /* no slots */
}

void watchdog_alive(osThreadId_t handle)
{
    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            wdg_tasks[i].last_tick = xTaskGetTickCount();
            break;
        }
    }
    osMutexRelease(wdg_mutex);
}

/* ---------- Monitor task ---------- */
static void watchdog_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t now = xTaskGetTickCount();

        osMutexAcquire(wdg_mutex, osWaitForever);
        for (int i = 0; i < WDG_MAX_TASKS; i++) {
            if (!wdg_tasks[i].registered) continue;

            uint32_t elapsed = (now - wdg_tasks[i].last_tick)
                               * portTICK_PERIOD_MS;
            /* Allow 3x the expected period before flagging */
            if (elapsed > wdg_tasks[i].expected_period_ms * 3) {
                /* TODO: log anomaly, optionally suspend/delete task */
            }
        }
        osMutexRelease(wdg_mutex);

        /* Check every 500 ms */
        osDelay(pdMS_TO_TICKS(500));
    }
}

osThreadId_t watchdog_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "watchdog",
        .stack_size = 128 * 4,
        .priority   = osPriorityHigh,
    };
    return osThreadNew(watchdog_monitor_task, NULL, &attrs);
}
