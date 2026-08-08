#include "comms.h"
#include "state_machine.h"
#include "watchdog.h"
#include "cmsis_os.h"
#include "main.h"
#include <string.h>

/* TODO: include RadioLib headers and implement STM32 HAL wrapper */

#define LORA_MAX_PACKET  64
#define BEACON_SIZE      128

#define CMD_SET_BEACON_INTERVAL  0x06

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

void comms_dispatch_command(uint8_t cmd_id, const uint8_t *payload, size_t len)
{
    switch (cmd_id) {
    case 0x01:  /* RESET */
        NVIC_SystemReset();
        break;
    case 0x02:  /* EXIT_STATE */
        state_machine_request_transition(STATE_READY, TRIGGER_GROUND_CMD);
        break;
    case 0x03:  /* SET_CONFIG */
        /* TODO: apply config from payload */
        break;
    case 0x04:  /* SEND_DATA */
        /* TODO: read FRAM and send chunked */
        break;
    case 0x05:  /* ACTIVATE_PAYLOAD */
        state_machine_request_transition(STATE_ACTIVE, TRIGGER_GROUND_CMD);
        break;
    case CMD_SET_BEACON_INTERVAL:  /* SET_BEACON_INTERVAL */
        /* Uplinked data is untrusted: require the full 4-byte argument and let
           the state machine range-check the value before it can affect the
           beacon cadence or the beacon watchdog period. A rejected or
           malformed command leaves the current cadence untouched. */
        if ((payload != NULL) && (len >= 4u)) {
            uint32_t interval_ms = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8)  |
                                    (uint32_t)payload[3];
            if (state_machine_set_beacon_interval(interval_ms) != 0) {
                /* TODO: emit TC rejection telemetry (out-of-range argument) */
            }
        } else {
            /* TODO: emit TC rejection telemetry (malformed argument) */
        }
        break;
    default:
        break;
    }
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

void lora_rx_task(void *arg)
{
    (void)arg;

    for (;;) {
        watchdog_alive_self();
        /* TODO: enter RX mode, wait for interrupt, decode packet */
        /* TODO: CRC check → decrypt → dispatch command */
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
