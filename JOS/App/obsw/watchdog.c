#include "watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "dual_bank.h"
#include "hw_watchdog.h"

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
    if ((wdg_mutex == NULL) || (handle == NULL) || (expected_period_ms == 0u)) {
        /* Not initialised yet, or nonsense arguments: refuse rather than
           silently pretend the task is monitored. */
        return -1;
    }

    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            /* Already registered (e.g. task re-created): refresh in place. */
            wdg_tasks[i].expected_period_ms = expected_period_ms;
            wdg_tasks[i].last_tick          = xTaskGetTickCount();
            osMutexRelease(wdg_mutex);
            return 0;
        }
    }
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

/* `handle` is only compared against the registered handles, never dereferenced
 * or reassigned, so it is declared const (cppcheck constParameter). The
 * top-level const does not change the function type, so the prototype in
 * watchdog.h stays ABI-compatible. */
void watchdog_alive(const osThreadId_t handle)
{
    if ((wdg_mutex == NULL) || (handle == NULL)) {
        return;
    }

    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            wdg_tasks[i].last_tick = xTaskGetTickCount();
            break;
        }
    }
    osMutexRelease(wdg_mutex);
}

void watchdog_alive_self(void)
{
    watchdog_alive(osThreadGetId());
}

/* ---------- Monitor task ---------- */

/* Boot-success declaration (W2-2, dual-bank fallback).
 *
 * The boot-fault window must stay open until the system has proved it can
 * actually run, not merely reach osKernelStart(): the faults that matter
 * (task creation, driver bring-up, first payload access) happen after the
 * scheduler starts. The monitor task therefore declares the boot good only
 * once DUAL_BANK_BOOT_OK_UPTIME_MS of scheduler uptime has elapsed, and
 * retries a bounded number of times if the marker cannot be persisted
 * (LastStates pool exhausted) instead of dropping the failure on the floor. */
#define WDG_BOOT_OK_MAX_RETRIES  3U

static void watchdog_declare_boot_ok_if_due(uint32_t uptime_ms)
{
    static uint8_t boot_ok_done    = 0U;
    static uint8_t boot_ok_retries = 0U;

    if (boot_ok_done) {
        return;
    }
    if (uptime_ms < DUAL_BANK_BOOT_OK_UPTIME_MS) {
        return;
    }

    if (dual_bank_boot_complete() == 0) {
        boot_ok_done = 1U;
        return;
    }

    /* Could not persist the boot-OK marker. dual_bank keeps the pending flag
       and stops trusting the frozen Flash evidence, so the failure is safe —
       but do not retry for ever. */
    boot_ok_retries++;
    if (boot_ok_retries >= WDG_BOOT_OK_MAX_RETRIES) {
        boot_ok_done = 1U;
    }
}

static void watchdog_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t now = xTaskGetTickCount();

        /* Refresh the independent hardware watchdog. This task is the single
           owner of the refresh once the scheduler is running: it is
           osPriorityHigh and runs every 500 ms against a ~31 s IWDG timeout,
           so the backstop only fires when the OBSW has genuinely stopped
           executing (LOCKUP, a spin in an exception handler, a hang in
           Error_Handler) rather than when it is merely busy.

           Deliberately unconditional and at the top of the loop: making the
           kick depend on the task-liveness scan below would couple the
           hardware backstop to the software monitor's own correctness, and a
           bug there would turn into a reset storm. Escalating a late task is
           the scan's job (see the TODO below), not the IWDG's. */
        hw_watchdog_kick();

        watchdog_declare_boot_ok_if_due(now * portTICK_PERIOD_MS);

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
