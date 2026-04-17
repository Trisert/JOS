#include "comms.h"
#include "state_machine.h"
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
    /* TODO: configure SX1268 via RadioLib — SF10, BW125, CR4/8, 436 MHz */
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
    (void)payload;
    (void)len;

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
        if (len >= 4) {
            uint32_t interval_ms = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8)  |
                                    (uint32_t)payload[3];
            state_machine_set_beacon_interval(interval_ms);
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

    for (;;) {
        uint32_t interval = state_machine_get_beacon_interval();
        /* TODO: build beacon packet (96 B telemetry + 32 B sys) */
        /* TODO: lora_tx(beacon_buf, BEACON_SIZE); */

        osDelay(pdMS_TO_TICKS(interval));
    }
}

osThreadId_t lora_beacon_task_create(void)
{
    return osThreadNew(lora_beacon_task, NULL, &beacon_attrs);
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
        /* TODO: enter RX mode, wait for interrupt, decode packet */
        /* TODO: CRC check → decrypt → dispatch command */
        osDelay(pdMS_TO_TICKS(100));
    }
}

osThreadId_t lora_rx_task_create(void)
{
    return osThreadNew(lora_rx_task, NULL, &rx_attrs);
}
