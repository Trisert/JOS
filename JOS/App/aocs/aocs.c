#include "aocs.h"
#include "cmsis_os.h"

/* Placeholder — AOCS implementation is handled by the AOCS team */

void aocs_init(void)
{
    /* TODO: initialise IMU (ASM330LHH) and magnetometer (IIS2MDC) */
}

void aocs_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* TODO: B-dot controller / EKF at 50 Hz */
        osDelay(pdMS_TO_TICKS(20));
    }
}
