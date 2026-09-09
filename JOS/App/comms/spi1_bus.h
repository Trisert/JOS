/*
 * spi1_bus.h — shared SPI1 bus arbitration (radio SX1268 + CLOUD MAX11128 ADC).
 *
 * SPI1 is a single physical bus (PA5/PA6/PA7, SPF pag 77) with two chip-selects:
 * the SX1268 LoRa transceiver (driven from App/comms/radiolib_hal.cpp) and the
 * CLOUD payload MAX11128 ADC (driven from Core/Src/MAX11128.c). Both clients
 * run in FreeRTOS tasks, so every multi-phase bus transaction must hold this
 * mutex from the first CS assert to the last CS de-assert; otherwise a radio
 * burst interleaved inside an ADC frame (or vice versa) corrupts both frames
 * with no HAL error to catch it.
 *
 * Priority inheritance is on (writers span several task priorities); the lock
 * call always takes an explicit finite timeout — never osWaitForever from a
 * task that must stay schedulable. Callers treat "lock refused" as a transient
 * bus-busy and return a best-effort miss (0 / untouched buffer), exactly like
 * a HAL timeout.
 *
 * Host-testable: only <stdint.h>, <stdbool.h> and cmsis_os2.h (faked on host).
 */
#ifndef SPI1_BUS_H
#define SPI1_BUS_H

#include <stdint.h>
#include <stdbool.h>

#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default bound for acquiring the bus: well above one worst-case DMA frame
 * (~50 ms for a full 64 KiB chunk at ~10 Mbit/s) but finite, so a wedged
 * holder can never park a task forever. */
#define SPI1_BUS_LOCK_TIMEOUT_MS  (100U)

/* Idempotent: creates the mutex once. Safe to call from every client init and
 * from spiBegin(); the first call wins, later calls are no-ops. If creation
 * fails (heap exhausted) the lock calls below refuse, they never pass a NULL
 * handle into the RTOS. */
void spi1_bus_init(void);

/* Acquire the bus, waiting at most timeout_ms. Returns true on success.
 * timeout_ms must be finite (never osWaitForever) from task context. */
bool spi1_bus_lock(uint32_t timeout_ms);

/* Release the bus. No-op when the mutex was never created. */
void spi1_bus_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* SPI1_BUS_H */
