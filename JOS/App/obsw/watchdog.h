#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "cmsis_os.h"
#include "obsw_types.h"   /* BEACON_INTERVAL_* */
#include <stdint.h>
#include <stddef.h>

/* Maximum number of tasks that can be monitored.
   Currently 7 tasks register (defaultTask, stateMachine, loraBeacon, loraRX,
   clear, cloud, aocs); the monitor task itself is deliberately not monitored. */
#define WDG_MAX_TASKS 12

/* Nominal loop periods (ms) declared by each monitored task at registration.
   The monitor flags a task once it has been silent for more than 3x its
   declared period (see watchdog_monitor_task()). Kept here so the periods
   used across App/ are visible in one place. */
#define WDG_PERIOD_DEFAULT_TASK_MS   1000u
#define WDG_PERIOD_STATE_MACHINE_MS   100u
#define WDG_PERIOD_LORA_RX_MS         100u
#define WDG_PERIOD_LORA_BEACON_MS   BEACON_INTERVAL_CRIT   /* slowest beacon */
#define WDG_PERIOD_CLEAR_MS          1000u
#define WDG_PERIOD_CLOUD_MS         (90UL * 60UL * 1000UL) /* once per orbit */
#define WDG_PERIOD_AOCS_MS             20u

/* Initialise the watchdog monitor */
void watchdog_monitor_init(void);

/* Register a task for monitoring (call during task init) */
int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms);

/* Create the watchdog monitor task */
osThreadId_t watchdog_task_create(void);

/* Called by monitored tasks to signal liveness */
void watchdog_alive(osThreadId_t handle);

/* Convenience wrapper: signal liveness for the calling task */
void watchdog_alive_self(void);

#endif /* WATCHDOG_H */
