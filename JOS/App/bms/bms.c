#include "bms.h"

/* Stub BMS — returns fixed values until real EPS SPI driver is ready */

static bms_status_t bms_status = {
    .soc        = 100,
    .temp_c     = 250,
    .voltage_mv = 4200,
};

void bms_init(void)
{
    /* TODO: init SPI to EPS STM32L1 */
}

bms_status_t bms_get_status(void)
{
    /* TODO: read from BQ27441 via EPS SPI */
    return bms_status;
}
