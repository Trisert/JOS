#ifndef RADIO_OWNERSHIP_H
#define RADIO_OWNERSHIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded task-side arbitration. A failed acquisition is a refusal, not an
 * invitation to touch the radio or SPI peripheral without ownership. */
/* CMSIS-RTOS2 mutex timeouts are ticks. The flight FreeRTOS configuration is
 * fixed at 1 kHz, so this is the documented 10 ms bounded refusal window. */
#define RADIO_OWNERSHIP_TIMEOUT_TICKS 10U

/* Physical SPI1 ownership is deliberately separate from radio-operation
 * ownership: the former is held only for one peripheral transaction. */
int  lora_spi_bus_acquire(uint32_t timeout_ticks);
void lora_spi_bus_release(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_OWNERSHIP_H */
