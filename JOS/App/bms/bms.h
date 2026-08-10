#ifndef BMS_H
#define BMS_H

#include <stdbool.h>

#include "obsw_types.h"

/* Initialise BMS interface (brings up the subsystem SPI master to the EPS) */
void bms_init(void);

/* True once the subsystem SPI master to the EPS STM32L496 is initialised and
   ready. False means bms_get_status() is serving boot defaults, not EPS
   telemetry. */
bool bms_spi_ready(void);

/* Get current battery status */
bms_status_t bms_get_status(void);

#endif /* BMS_H */
