#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "cmsis_os.h"
#include <stdint.h>

/* Maximum number of tasks that can be monitored */
#define WDG_MAX_TASKS 8

/* Initialise the watchdog monitor */
void watchdog_monitor_init(void);

/* Register a task for monitoring (call during task init) */
int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms);

/* Create the watchdog monitor task */
osThreadId_t watchdog_task_create(void);

/* Called by monitored tasks to signal liveness */
void watchdog_alive(osThreadId_t handle);

#endif /* WATCHDOG_H */
