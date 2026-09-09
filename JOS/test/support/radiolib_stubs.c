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

/* Unified host lora stub: TX call log + two fault-injection schemes.
 * - TX log (framing tests): records every lora_tx() payload up to
 *   RADIOLIB_STUB_TX_CAP per call (the flight COMMS_MAX_PACKET budget) so
 *   chunk-framing tests can assert per-chunk lengths, msg/seq/total headers
 *   and reassembly. Past the budget: fail loudly, never truncate.
 * - host_lora_fail_tx/wait_on_call(n): fail the Nth upcoming call (relative
 *   count, n=1 = very next call). Reset by host_lora_reset() (call from
 *   setUp) so an armed failure never leaks into the next test.
 * - radiolib_stub_tx_fail_at_index(i)/fail_wait_done: fail TX call index i
 *   (0-based) or every TX_DONE wait. Reset by radiolib_stub_tx_reset().
 * A failed call is NOT logged — it never went on air.
 *
 * RADIOLIB_STUB_TX_CAP must equal COMMS_MAX_PACKET. The stub deliberately
 * does NOT include comms.h (support/ files must stay free of App headers —
 * see project.yml), so the equality is pinned by the host test
 * test_stub_tx_cap_matches_comms_max_packet instead of a _Static_assert. */
#define RADIOLIB_STUB_TX_CAP 64U
#define RADIOLIB_STUB_TX_LOG_MAX 300U
static uint8_t stub_tx_data[RADIOLIB_STUB_TX_LOG_MAX][RADIOLIB_STUB_TX_CAP];
static size_t  stub_tx_len[RADIOLIB_STUB_TX_LOG_MAX];
static size_t  stub_tx_n = 0U;
static int     tx_calls;
static int     tx_fail_at   = -1;   /* 1-based lora_tx() call number to fail */
static int     wait_calls;
static int     wait_fail_at = -1;   /* 1-based lora_tx_wait_done() call to fail */
static size_t  stub_tx_fail_at = (size_t)-1;
static int     stub_wait_fail  = 0;

void radiolib_stub_tx_fail_at_index(size_t i) { stub_tx_fail_at = i; }
void radiolib_stub_tx_fail_wait_done(int fail) { stub_wait_fail = fail; }

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
    stub_tx_n = 0U; stub_tx_fail_at = (size_t)-1; stub_wait_fail = 0;
}

int lora_tx(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return -1;
    }
    if (len > (size_t)RADIOLIB_STUB_TX_CAP) {
        return -1;   /* past the on-air budget: fail loudly, never truncate */
    }
    tx_calls++;
    if ((tx_fail_at > 0) && (tx_calls == tx_fail_at)) {
        tx_fail_at = -1;   /* fail once */
        return -1;
    }
    if (stub_tx_n == stub_tx_fail_at) {
        return -1;   /* injected mid-sequence failure */
    }
    if (stub_tx_n < (size_t)RADIOLIB_STUB_TX_LOG_MAX) {
        for (size_t i = 0U; i < len; i++) {
            stub_tx_data[stub_tx_n][i] = data[i];
        }
        stub_tx_len[stub_tx_n] = len;
        stub_tx_n++;
    }
    return 0;
}
int  lora_rx(uint8_t *buf, size_t *len)       { (void)buf; if (len) *len = 0U; return 0; }
int  lora_start_receive(void)                  { return 0; }
void lora_rx_task_register(void *handle)       { (void)handle; }
void lora_on_dio1_irq(void)                    { }
int  lora_tx_wait_done(uint32_t timeout_ms)
{
    (void)timeout_ms;
    wait_calls++;
    if ((wait_fail_at > 0) && (wait_calls == wait_fail_at)) {
        wait_fail_at = -1;   /* fail once */
        return -1;
    }
    return stub_wait_fail ? -1 : 0;
}

/* Log accessors for the framing tests (inert unless a test calls them). */
size_t radiolib_stub_tx_count(void)            { return stub_tx_n; }
size_t radiolib_stub_tx_len(size_t i)
{
    return (i < stub_tx_n) ? stub_tx_len[i] : 0U;
}
const uint8_t *radiolib_stub_tx_data(size_t i)
{
    return (i < stub_tx_n) ? stub_tx_data[i] : (const uint8_t *)0;
}
void radiolib_stub_tx_reset(void)
{
    stub_tx_n      = 0U;
    stub_tx_fail_at = (size_t)-1;
    stub_wait_fail  = 0;
}
