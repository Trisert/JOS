#include "aocs.h"
#include "watchdog.h"
#include "cmsis_os.h"

/* Per RED_DES_ElectronicArchitecture_V1:
 *   IMU (ASM330LHH) and MAG (IIS2MDC) are on the AOCS board,
 *   connected to the AOCS MCU via local I2C.
 *   The OBC reads attitude data by polling the AOCS over the
 *   subsystem SPI bus (OBC master, AOCS slave).
 *   This file is a placeholder — actual AOCS firmware runs on a
 *   separate STM32L496VGT3 on the AOCS board.
 */

void aocs_init(void)
{
    /* No-op on the OBC: sensors are on the AOCS board */
}

void aocs_task(void *arg)
{
    (void)arg;

    for (;;) {
        watchdog_alive_self();
        /* TODO: poll AOCS MCU via subsystem SPI for attitude data */
        osDelay(pdMS_TO_TICKS(20));
    }
}

osThreadId_t aocs_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "aocs",
        .stack_size = 256 * 4,
        .priority   = osPriorityNormal,
    };

    /* Not started from main() yet: the subsystem SPI link to the AOCS MCU is
       still a stub. Kept here so the task is watchdog-monitored the moment it
       is enabled (docs/dev/hardening.md §3.1). */
    osThreadId_t handle = osThreadNew(aocs_task, NULL, &attrs);
    if (handle != NULL) {
        (void)watchdog_register_task(handle, WDG_PERIOD_AOCS_MS);
    }
    return handle;
}
