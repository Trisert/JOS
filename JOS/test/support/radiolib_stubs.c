/* ---------------------------------------------------------------------------
 * support/radiolib_stubs.c - host-unit-test stand-ins for the RadioLib SX1268
 * driver entry points (normally implemented in App/comms/radiolib_driver.cpp
 * for the flight target). These let comms.c link under Ceedling without the
 * real RadioLib HAL.
 *
 * Only the symbols comms.c / the test harness actually call are provided.
 *
 * Self-contained type handling: Ceedling's embedded-simulation toolchain may
 * not expose <stdint.h>/<stddef.h> in support/ files, so fall back to local
 * typedefs when the system headers are unavailable.
 * ------------------------------------------------------------------------- */

#if defined(__has_include)
  #if __has_include(<stdint.h>)
    #include <stdint.h>
    #define HAVE_STDINT 1
  #endif
  #if __has_include(<stddef.h>)
    #include <stddef.h>
    #define HAVE_STDDEF 1
  #endif
#else
  #include <stdint.h>
  #include <stddef.h>
  #define HAVE_STDINT 1
  #define HAVE_STDDEF 1
#endif

#ifndef HAVE_STDINT
  typedef unsigned char uint8_t;
#endif
#ifndef HAVE_STDDEF
  typedef unsigned long size_t;
#endif

int  lora_init(void)                          { return 0; }
int  lora_tx(const uint8_t *data, size_t len) { (void)data; (void)len; return 0; }
int  lora_rx(uint8_t *buf, size_t *len)       { (void)buf; if (len) *len = 0U; return 0; }
int  lora_tx_wait_done(uint32_t timeout_ms)    { (void)timeout_ms; return 0; }
int  lora_start_receive(void)                  { return 0; }
void lora_rx_task_register(void *handle)       { (void)handle; }
void lora_on_dio1_irq(void)                    { }
