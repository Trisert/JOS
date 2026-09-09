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

/* Failure injection for the lora_send_chunked() error paths in comms.c
 * (radio refuses the chunk / TX_DONE never arrives). Call-counted, so a test
 * can fail the Nth upcoming call and prove the staging loop aborts
 * mid-transfer, not just at its first iteration. Default is success, so
 * every existing test is unaffected. Reset by host_lora_reset() (call from
 * setUp). */
static int tx_calls;
static int tx_fail_at   = -1;   /* 1-based lora_tx() call number to fail */
static int wait_calls;
static int wait_fail_at = -1;   /* 1-based lora_tx_wait_done() call to fail */

int  lora_tx(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    tx_calls++;
    if ((tx_fail_at > 0) && (tx_calls == tx_fail_at)) {
        tx_fail_at = -1;   /* fail once */
        return -1;
    }
    return 0;
}
int  lora_rx(uint8_t *buf, size_t *len)       { (void)buf; if (len) *len = 0U; return 0; }
int  lora_tx_wait_done(uint32_t timeout_ms)
{
    (void)timeout_ms;
    wait_calls++;
    if ((wait_fail_at > 0) && (wait_calls == wait_fail_at)) {
        wait_fail_at = -1;   /* fail once */
        return -1;
    }
    return 0;
}

/* Arm a one-shot failure on a RELATIVE call count: n = 1 fails the very next
 * lora_tx()/lora_tx_wait_done() call ("next call", not absolute call #n);
 * n = 2 the one after that. n <= 0 disarms. */
void host_lora_fail_tx_on_call(int n)
{
    tx_fail_at = (n > 0) ? tx_calls + n : -1;
}
void host_lora_fail_wait_on_call(int n)
{
    wait_fail_at = (n > 0) ? wait_calls + n : -1;
}
void host_lora_reset(void)
{
    tx_calls = 0;   tx_fail_at   = -1;
    wait_calls = 0; wait_fail_at = -1;
}
int  lora_start_receive(void)                  { return 0; }
void lora_rx_task_register(void *handle)       { (void)handle; }
void lora_on_dio1_irq(void)                    { }
