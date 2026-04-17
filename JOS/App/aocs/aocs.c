#include "aocs.h"
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
        /* TODO: poll AOCS MCU via subsystem SPI for attitude data */
        osDelay(pdMS_TO_TICKS(20));
    }
}
