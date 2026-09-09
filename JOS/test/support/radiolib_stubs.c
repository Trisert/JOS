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

/* ---------- TX call log (framing tests, host only) ----------
 * Records every lora_tx() payload (up to 64 B per call — the flight
 * COMMS_MAX_PACKET budget) so chunk-framing tests can assert per-chunk
 * lengths, seq/total headers and reassembly. Flight code never calls these;
 * they are linked into every test binary but inert unless used. */
#define RADIOLIB_STUB_TX_LOG_MAX 300U
static uint8_t stub_tx_data[RADIOLIB_STUB_TX_LOG_MAX][64];
static size_t  stub_tx_len[RADIOLIB_STUB_TX_LOG_MAX];
static size_t  stub_tx_n = 0U;

int lora_tx(const uint8_t *data, size_t len)
{
    if ((stub_tx_n < (size_t)RADIOLIB_STUB_TX_LOG_MAX) && (data != NULL)) {
        size_t copy = (len < 64U) ? len : 64U;
        for (size_t i = 0U; i < copy; i++) {
            stub_tx_data[stub_tx_n][i] = data[i];
        }
        stub_tx_len[stub_tx_n] = len;   /* record the FULL claimed length */
        stub_tx_n++;
    }
    (void)data; (void)len; return 0;
}
int  lora_rx(uint8_t *buf, size_t *len)       { (void)buf; if (len) *len = 0U; return 0; }
int  lora_tx_wait_done(uint32_t timeout_ms)    { (void)timeout_ms; return 0; }

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
void radiolib_stub_tx_reset(void)              { stub_tx_n = 0U; }
