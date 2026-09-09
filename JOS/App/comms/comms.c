#include "comms.h"
#include "comms_validate.h"
#include "state_machine.h"
#include "watchdog.h"
#include "cmsis_os.h"
#include "main.h"
#include "sram2_parity.h"   /* SRAM2_CRITICAL_NOINIT placement (W2-3) */
#include <string.h>

/* The RadioLib SX1268 driver lives in App/comms/radiolib_driver.cpp (wired in
   PR #47): lora_*() entry points are declared below with C linkage. */

/* Packet buffer sizes now live in comms.h as COMMS_MAX_PACKET /
   COMMS_BEACON_SIZE — the parity-protected SRAM2 buffers below are sized
   from them, and callers need the same sizes. */

extern SPI_HandleTypeDef hspi1;

/* RadioLib SX1268 driver entry points (C linkage, implemented in
   App/comms/radiolib_driver.cpp). Declared here so comms.c (compiled as C)
   links against the unmangled symbols. */
extern int  lora_init(void);
extern int  lora_tx(const uint8_t *data, size_t len);
extern int  lora_rx(uint8_t *data, size_t *len);
extern int  lora_tx_wait_done(uint32_t timeout_ms);
extern int  lora_start_receive(void);
/* Registers the RX task handle with the driver so the DIO1 ISR wakes this task
   on RX_DONE. Implemented in radiolib_driver.cpp. */
extern void lora_rx_task_register(osThreadId_t handle);

/* LORA_RX_FLAG is defined in comms.h (must match LORA_FLAG_RX_DONE in
   radiolib_driver.cpp — see comment there). */

/* ---------- Packet buffers in SRAM2 (hardware parity) ----------
   Placed in the NOLOAD .sram2_noinit section: the SRAM2 hardware erase run by
   sram2_parity_init() zeroes them at boot with valid parity, so they cost no
   Flash. A single-event upset in a frame being assembled or decoded now
   raises an NMI (recorded + reset) instead of transmitting or executing
   corrupted data. */
static SRAM2_CRITICAL_NOINIT uint8_t comms_beacon_buf[COMMS_BEACON_SIZE];
static SRAM2_CRITICAL_NOINIT uint8_t comms_rx_buf[COMMS_MAX_PACKET];
static SRAM2_CRITICAL_NOINIT uint8_t comms_tx_buf[COMMS_MAX_PACKET];

uint8_t *comms_beacon_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_beacon_buf); }
    return comms_beacon_buf;
}

uint8_t *comms_rx_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_rx_buf); }
    return comms_rx_buf;
}

uint8_t *comms_tx_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_tx_buf); }
    return comms_tx_buf;
}

/* ---------- Stub implementations ---------- */

/* lora_init() is implemented in radiolib_driver.cpp (target build). The host
   unit-test build links a fake from test/fakes/ instead (see B4). */

/* TX sequence counters (defined below; single writer — see note there). */
static comms_tx_stats_t tx_stats;

int lora_send_chunked(const uint8_t *data, size_t len)
{
    size_t chunk_max = 0U;
    uint8_t *tx = comms_tx_buffer(&chunk_max);

    if ((data == NULL) && (len != 0U)) {
        return -1;
    }
    if (len == 0U) {
        return 0;   /* nothing to stage — NULL/0 is a successful no-op */
    }
    if (chunk_max <= (size_t)COMMS_CHUNK_HDR_LEN) {
        return -1;  /* TX buffer cannot even hold the framing header */
    }

    /* Payload bytes per chunk after the msg + seq + total header. */
    const size_t payload_max = chunk_max - (size_t)COMMS_CHUNK_HDR_LEN;

    /* Bounded: payload_max >= 1, so total >= 1 and <= len. */
    const size_t total = ((len - 1U) / payload_max) + 1U;
    if (total > 255U) {
        return -1;  /* count must fit in the 1-byte total field */
    }

    /* Monotonic message id: consecutive sequences carry consecutive ids so
       ground can distinguish beacons and flag duplicates. Wraps mod 256;
       only consumed here (single writer — the beacon path is one task). */
    static uint8_t tx_msg_id = 0U;
    const uint8_t msg = tx_msg_id++;

    /* Hand each framed chunk to RadioLib. TX is async (startTransmit); wait
       for the DIO1 TX_DONE flag before staging the next chunk so we never
       overwrite the buffer mid-air. 2000 ms covers SF10 @ 125 kHz for the
       largest chunk.
       On ANY radio failure the sequence aborts here: no further chunks go on
       air (the partial message is detectable via its total field), the
       failure is counted, and the caller retries the whole message later. */
    for (size_t seq = 0U; seq < total; seq++) {
        const size_t off = seq * payload_max;   /* no overflow: seq < total <= len */
        size_t n = len - off;
        if (n > payload_max) {
            n = payload_max;
        }
        tx[COMMS_CHUNK_OFF_MSG]   = msg;
        tx[COMMS_CHUNK_OFF_SEQ]   = (uint8_t)seq;
        tx[COMMS_CHUNK_OFF_TOTAL] = (uint8_t)total;
        memcpy(&tx[COMMS_CHUNK_HDR_LEN], data + off, n);
        if (lora_tx(tx, n + (size_t)COMMS_CHUNK_HDR_LEN) != 0) {
            tx_stats.sequences_failed++;
            return -1;
        }
        tx_stats.chunks_sent++;
        if (lora_tx_wait_done(2000U) != 0) {
            tx_stats.sequences_failed++;
            return -1;
        }
    }
    tx_stats.sequences_ok++;
    return 0;
}

/* ---------- TX counters ---------- */

/* TX counters live at file scope above (single writer — lora_send_chunked()
 * runs only on the beacon task, so the increments never race). Same snapshot
 * semantics as the RX counters in comms_validate.c (diagnostics-grade, not
 * flight-critical). */
void comms_tx_get_stats(comms_tx_stats_t *out)
{
    if (out != NULL) {
        out->sequences_ok     = tx_stats.sequences_ok;
        out->sequences_failed = tx_stats.sequences_failed;
        out->chunks_sent      = tx_stats.chunks_sent;
    }
}

/* Compile-time proof for the chunk_max <= HDR guard above: with the fixed
   SRAM2 sizing the TX buffer always holds a header plus payload. If someone
   ever shrinks COMMS_MAX_PACKET to the header size, this — not a runtime
   field failure — tells them. */
_Static_assert(COMMS_MAX_PACKET > COMMS_CHUNK_HDR_LEN,
               "TX buffer must hold the chunk header plus payload");

/* ---------- Telecommand dispatcher (private) ---------- */

/**
 * Execute an already-validated telecommand.
 *
 * Deliberately file-static and suffixed @c _unchecked: it performs NO
 * structural validation of its own, so the only legal caller is
 * comms_rx_handle_frame(), which runs comms_validate_tc() first (length, CRC,
 * opcode whitelist, per-opcode payload size and parameter ranges).
 * Exporting it would make the validation gate bypassable.
 */
static void comms_dispatch_command_unchecked(uint8_t cmd_id,
                                             const uint8_t *payload,
                                             size_t len)
{
    switch (cmd_id) {
    case COMMS_TC_RESET:
        NVIC_SystemReset();
        break;
    case COMMS_TC_EXIT_STATE:
        state_machine_request_transition(STATE_READY, TRIGGER_GROUND_CMD);
        break;
    case COMMS_TC_SET_CONFIG:
        /* TODO: apply config from payload */
        break;
    case COMMS_TC_SEND_DATA:
        /* TODO: read FRAM and send chunked */
        break;
    case COMMS_TC_ACTIVATE_PAYLOAD:
        state_machine_request_transition(STATE_ACTIVE, TRIGGER_GROUND_CMD);
        break;
    case COMMS_TC_SET_BEACON_INTERVAL:
        if ((payload != NULL) && (len >= 4U)) {
            uint32_t interval_ms = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8)  |
                                    (uint32_t)payload[3];
            /* Defence in depth: comms_validate_tc() already range-checked this,
             * re-check here so a future in-file caller cannot bypass the
             * bounds. NASA-PoT #1 / NASA-STD-8739.8. */
            if ((interval_ms == 0UL) ||
                ((interval_ms >= COMMS_TC_BEACON_MIN_MS) &&
                 (interval_ms <= COMMS_TC_BEACON_MAX_MS))) {
                state_machine_set_beacon_interval(interval_ms);
            }
        }
        break;
    default:
        /* Unreachable via comms_rx_handle_frame(): unknown opcodes are
         * rejected by the whitelist. Kept as a defensive no-op. */
        break;
    }
}

/* ---------- Uplink validation gate ---------- */

/**
 * Validate a raw uplink frame and dispatch it only if it is well formed,
 * CRC-clean, HMAC-authenticated, of a known opcode and with in-range
 * parameters. Every rejection is counted (comms_rx_get_stats) and never
 * reaches the dispatcher.
 *
 * Scope: structural validation (framing, CRC-16 integrity, opcode whitelist,
 * parameter ranges) PLUS keyed authentication. The CRC is unkeyed, so it
 * detects corruption and malformed frames only; the truncated HMAC-SHA256 tag
 * (upstream RedPill makeMAC design) authenticates the sender. Layout
 * discrimination is by exact length: len == P+8 selects the authenticated
 * validator, anything else the legacy one. With COMMS_AUTH_ENFORCE=1 (flight
 * default) a structurally valid but untagged legacy frame is rejected with
 * COMMS_TC_ERR_MAC; with COMMS_AUTH_ENFORCE=0 (bench/migration only) legacy
 * CRC-only frames are still dispatched. Either way a bad tag is rejected
 * before the dispatcher runs (verify-then-dispatch).
 *
 * Standards: NASA-PoT #1 (bounds-checked, no overflow), NASA-STD-8739.8
 * (command validation before execution).
 */
comms_tc_result_t comms_rx_handle_frame(const uint8_t *frame, size_t len)
{
    uint8_t        opcode      = 0U;
    const uint8_t *payload     = NULL;
    size_t         payload_len = 0U;
    comms_tc_result_t result;

    if (comms_frame_is_auth_layout(frame, len)) {
        result = comms_validate_tc_auth(frame, len,
                                        &opcode, &payload, &payload_len);
    } else {
        result = comms_validate_tc(frame, len, &opcode, &payload, &payload_len);
#if COMMS_AUTH_ENFORCE
        /* Structurally valid but untagged: well-formed legacy frames parse
         * cleanly yet carry no authentication — reject, do NOT dispatch.
         * Malformed frames keep their structural verdict for telemetry. */
        if (result == COMMS_TC_OK) {
            result = COMMS_TC_ERR_MAC;
        }
#endif
    }

    comms_rx_account(result);

    if (result != COMMS_TC_OK) {
        return result;   /* rejected — do NOT dispatch */
    }

    comms_dispatch_command_unchecked(opcode, payload, payload_len);
    return COMMS_TC_OK;
}

/* ---------- Beacon TX task ---------- */

static const osThreadAttr_t beacon_attrs = {
    .name       = "loraBeacon",
    .stack_size = 256 * 4,
    .priority   = osPriorityBelowNormal,
};

void lora_beacon_task(void *arg)
{
    (void)arg;
    uint32_t registered_period_ms = 0u;

    for (;;) {
        uint32_t interval = state_machine_get_beacon_interval();

        /* The beacon cadence is state-dependent (1..16 min) and can also be
           retargeted from ground via CMD_SET_BEACON_INTERVAL. Registering the
           worst case once at creation would make the monitor blind for up to
           3 x 16 min even when the task is supposed to run every minute.
           Re-registering the *same* handle takes the duplicate-refresh path in
           watchdog_register_task(): the existing slot is updated in place with
           the new period and its last_tick is reset, so the monitor always
           tracks the cadence actually in force and the period change itself
           cannot false-flag the task. */
        if (interval != registered_period_ms) {
            if (watchdog_register_task(osThreadGetId(), interval) == 0) {
                registered_period_ms = interval;
            }
        }

        watchdog_alive_self();

        size_t beacon_len = 0U;
        const uint8_t *beacon = comms_beacon_buffer(&beacon_len);

        /* Build beacon packet (96 B telemetry + 32 B sys) in `beacon`.
           TODO: full telemetry encoding. For now transmit the staging buffer
           as-is so the link is exercised end-to-end. The 128 B beacon does
           NOT fit one LoRa payload (COMMS_MAX_PACKET = 64): fragment it via
           lora_send_chunked(), which frames every chunk with a msg + seq +
           total header (3 chunks on the air).
           A failed sequence is already counted in the TX stats by
           lora_send_chunked() — the whole message is retried here at the next
           interval, never resumed mid-sequence (ground detects the truncation
           via the total field).
           Blocking note: a full beacon holds this task for up to ~6 s
           (3 x 2 s TX_DONE waits). The watchdog judges this task against its
           1..16 min cadence and flags it only after 3x the period (>= 3 min),
           so the block is two orders of magnitude inside the monitor window. */
        if (beacon_len > 0U) {
            if (lora_send_chunked(beacon, beacon_len) != 0) {
                /* counted — retry the whole message next interval */
            }
        }

        osDelay(pdMS_TO_TICKS(interval));
    }
}

osThreadId_t lora_beacon_task_create(void)
{
    osThreadId_t handle = osThreadNew(lora_beacon_task, NULL, &beacon_attrs);
    if (handle != NULL) {
        /* Bootstrap with the slowest permitted cadence so the task is covered
           from the first tick; the task itself narrows the period to the
           cadence actually in force on its first iteration (duplicate-refresh). */
        (void)watchdog_register_task(handle, WDG_PERIOD_LORA_BEACON_MS);
    }
    return handle;
}

/* ---------- RX task ---------- */

static const osThreadAttr_t rx_attrs = {
    .name       = "loraRX",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/*
 * NOW WIRED: the SX1268 driver (radiolib_driver.cpp) lands the PHY payload into
 * `rx` via the DIO1 IRQ. The ONLY permitted path from PHY payload to dispatcher
 * is comms_rx_handle_frame(), which performs structure + CRC + HMAC tag +
 * opcode + range
 * validation and dispatches only valid telecommands. A rejection reason must be
 * logged/telemetered, never discarded.
 */
void lora_rx_task(void *arg)
{
    (void)arg;

    /* Tell the driver which task to wake on RX_DONE. */
    lora_rx_task_register(osThreadGetId());

    /* Arm continuous RX so the DIO1 IRQ fires on the next downlink. */
    (void)lora_start_receive();

    for (;;) {
        /* Block on the DIO1 RX_DONE flag (with timeout) and kick the watchdog
           inside the loop so the blocking wait never arms a false 'hung' flag.
           The 100 ms poll granularity keeps the monitor happy. */
        uint32_t flags = osThreadFlagsWait(LORA_RX_FLAG, osFlagsWaitAny, 100U);
        watchdog_alive_self();

        if (flags == LORA_RX_FLAG) {
            size_t rx_len = 0U;
            uint8_t *rx = comms_rx_buffer(&rx_len);

            if (lora_rx(rx, &rx_len) == 0) {
                comms_tc_result_t r = comms_rx_handle_frame(rx, rx_len);
                if (r != COMMS_TC_OK) {
                    /* Rejection reason must be visible, not silently dropped.
                       Route it to the telemetry/log sink once one exists; for
                       now surface the human-readable reason via the existing
                       comms_tc_result_str(). */
                    const char *why = comms_tc_result_str(r);
                    (void)why;  /* TODO: forward `why` to telemetry/log sink */
                }
            } else {
                /* PHY-level reject (oversize payload or radio read error):
                   the frame never reached the validator, so account it here —
                   otherwise the drop is a blind spot in the RX stats. */
                comms_rx_account(COMMS_TC_ERR_PHY);
            }

            /* Re-arm RX for the next frame. */
            (void)lora_start_receive();
        }
    }
}

osThreadId_t lora_rx_task_create(void)
{
    osThreadId_t handle = osThreadNew(lora_rx_task, NULL, &rx_attrs);
    if (handle != NULL) {
        (void)watchdog_register_task(handle, WDG_PERIOD_LORA_RX_MS);
    }
    return handle;
}
