#ifndef AOCS_H
#define AOCS_H

#include <stdint.h>
#include "cmsis_os.h"

/* Placeholder — AOCS is developed by a separate team */

void aocs_init(void);
void aocs_task(void *arg);

/* Create the AOCS polling task and register it with the watchdog monitor.
   Not called from main() yet — the subsystem SPI driver is still a stub. */
osThreadId_t aocs_task_create(void);

#endif /* AOCS_H */
