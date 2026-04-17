#include "bms.h"

/* Stub BMS — returns fixed values until real subsystem SPI driver is ready.
 * Per RED_DES_ElectronicArchitecture_V1:
 *   BQ76905 is on the EPS board, connected to the EPS MCU via local I2C.
 *   The OBC queries the EPS over the subsystem SPI bus for battery telemetry
 *   (voltage, current, temperature, SoC, SoH).
 */

static bms_status_t bms_status = {
    .soc        = 100,
    .temp_c     = 250,
    .voltage_mv = 7400,
};

void bms_init(void)
{
    /* TODO: init subsystem SPI master to EPS STM32L496 (SPI slave) */
}

bms_status_t bms_get_status(void)
{
    /* TODO: query EPS MCU over subsystem SPI for BQ76905 telemetry */
    return bms_status;
}
