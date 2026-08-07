#include "comms.h"
#include "comms_validate.h"
#include "state_machine.h"
#include "watchdog.h"
#include "cmsis_os.h"
#include "main.h"
#include <string.h>

/* TODO: include RadioLib headers and implement STM32 HAL wrapper */

#define LORA_MAX_PACKET  64
#define BEACON_SIZE      128

extern SPI_HandleTypeDef hspi1;

/* ---------- Stub implementations ---------- */

int lora_init(void)
{
    /* TODO: configure SX1268 via RadioLib — SF10, BW125, CR4/8, 433 MHz */
    return 0;
}

int lora_send_chunked(const uint8_t *data, size_t len)
{
    /* TODO: fragment into 64-byte chunks with seq numbers, TX each */
    (void)data;
    (void)len;
    return 0;
}

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
 * CRC-clean, of a known opcode and with in-range parameters. Every rejection
 * is counted (comms_rx_get_stats) and never reaches the dispatcher.
 *
 * Scope: this is *structural* validation (framing, CRC-16 integrity, opcode
 * whitelist, parameter ranges). The CRC is unkeyed, so it detects corruption
 * and malformed frames — it does NOT authenticate the sender and gives no
 * replay protection. A keyed MAC + rolling counter would be needed for that.
 *
 * Standards: NASA-PoT #1 (bounds-checked, no overflow), NASA-STD-8739.8
 * (command validation before execution).
 */
comms_tc_result_t comms_rx_handle_frame(const uint8_t *frame, size_t len)
{
    uint8_t        opcode      = 0U;
    const uint8_t *payload     = NULL;
    size_t         payload_len = 0U;

    comms_tc_result_t result =
        comms_validate_tc(frame, len, &opcode, &payload, &payload_len);

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
        /* TODO: build beacon packet (96 B telemetry + 32 B sys) */
        /* TODO: lora_tx(beacon_buf, BEACON_SIZE); */

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
 * NOT YET WIRED: the SX1268 driver is still a stub, so nothing calls
 * comms_rx_handle_frame() on the flight target yet — the gate is dormant on
 * hardware and only exercised by the ESP32 simulation bridge and host tests.
 * When the radio driver lands, the ONLY permitted path from PHY payload to
 * dispatcher is comms_rx_handle_frame().
 */
void lora_rx_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* TODO: enter RX mode, wait for the DIO1 IRQ, read the PHY payload into
         *       rx_buf / rx_len, then gate it through:
         *           comms_tc_result_t r = comms_rx_handle_frame(rx_buf, rx_len);
         *           if (r != COMMS_TC_OK) { log comms_tc_result_str(r); }
         *       which performs structure + CRC + opcode + range validation and
         *       dispatches only valid telecommands. The rejection reason must be
         *       logged/telemetered, not discarded. */
        osDelay(pdMS_TO_TICKS(100));
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
