#ifndef BMS_H
#define BMS_H

#include "obsw_types.h"

/* Initialise BMS interface */
void bms_init(void);

/* Get current battery status */
bms_status_t bms_get_status(void);

#endif /* BMS_H */
