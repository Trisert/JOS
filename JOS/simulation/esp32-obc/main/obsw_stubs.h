#include <string.h>
#include "cmsis_os.h"

osThreadId_t lora_beacon_task_create(void);
osThreadId_t lora_rx_task_create(void);

void lora_beacon_task_create(void) {}
void lora_rx_task_create(void) {}
void cloud_task_create(void) {}
void clear_task_create(void) {}
void bms_init(void) {}
void fram_init(void) {}
void cyclic_buffer_init(void) {}
void laststates_init(void) {}
int lora_init(void) { return 0; }
void state_machine_init(void) {}
osThreadId_t state_machine_task_create(void) { return NULL; }
osThreadId_t watchdog_task_create(void) { return NULL; }
void watchdog_monitor_init(void) {}
int lora_send_chunked(const void *d, unsigned long l) { (void)d; (void)l; return 0; }
