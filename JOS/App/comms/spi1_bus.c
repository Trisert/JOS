/*
 * spi1_bus.c — shared SPI1 bus arbitration, see spi1_bus.h.
 */
#include "spi1_bus.h"

static osMutexId_t s_spi1_mutex = NULL;

void spi1_bus_init(void)
{
    /* First call wins; later calls (second client init, spiBegin) are no-ops.
     * Prio-inherit: radio + ADC tasks run at different priorities. */
    static const osMutexAttr_t attr = {
        .name      = "spi1_bus",
        .attr_bits = (uint32_t)(osMutexPrioInherit | osMutexRobust),
        .cb_mem    = NULL,
        .cb_size   = 0U,
    };

    if (s_spi1_mutex == NULL) {
        s_spi1_mutex = osMutexNew(&attr);
    }
    /* On creation failure s_spi1_mutex stays NULL and spi1_bus_lock() refuses:
     * an explicit miss beats silently unsynchronised bus traffic. */
}

bool spi1_bus_lock(uint32_t timeout_ms)
{
    if (s_spi1_mutex == NULL) {
        return false;
    }
    return (osMutexAcquire(s_spi1_mutex, timeout_ms) == osOK);
}

void spi1_bus_unlock(void)
{
    if (s_spi1_mutex != NULL) {
        (void)osMutexRelease(s_spi1_mutex);
    }
}
