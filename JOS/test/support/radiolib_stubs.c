/* ---------------------------------------------------------------------------
 * support/radiolib_stubs.c - host-unit-test stand-ins for the RadioLib SX1268
 * driver entry points (normally implemented in App/comms/radiolib_driver.cpp,
 * which is C++ and target-only and therefore NOT compiled into the host tests).
 *
 * These stubs mirror the flight prototypes so comms.c (which declares them
 * `extern`) links under Ceedling. Behaviour is minimal: lora_init() reports
 * success, the TX/RX shims return 0, and the flag/task hooks are no-ops. They
 * are intentionally NOT mocked (like fakes/main.h) because Ceedling links the
 * whole :support: set into every test binary.
 *
 * This file is only compiled by the host test build, never by the flight
 * target (the target uses the real radiolib_driver.cpp).
 * ------------------------------------------------------------------------- */

#include <stdint.h>

int lora_init(void)
{
    return 0;
}

int lora_tx(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return 0;
}

int lora_rx(uint8_t *buf, size_t *len)
{
    (void)buf;
    (void)len;
    return 0;
}

int lora_tx_wait_done(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}

int lora_start_receive(void)
{
    return 0;
}

void lora_rx_task_register(void *handle)
{
    (void)handle;
}

void lora_on_dio1_irq(void)
{
    /* No-op on host: the RX task polls in the simulation bridge instead. */
}
