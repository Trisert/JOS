#include "comms.h"
#include "state_machine.h"
#include "cmsis_os.h"
#include "main.h"
#include "sram2_parity.h"   /* SRAM2_CRITICAL_NOINIT placement (W2-3) */
#include <string.h>

/* TODO: include RadioLib headers and implement STM32 HAL wrapper */

#define CMD_SET_BEACON_INTERVAL  0x06

extern SPI_HandleTypeDef hspi1;

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

int lora_init(void)
{
    /* TODO: configure SX1268 via RadioLib — SF10, BW125, CR4/8, 433 MHz */
    return 0;
}

int lora_send_chunked(const uint8_t *data, size_t len)
{
    size_t chunk_max = 0U;
    uint8_t *tx = comms_tx_buffer(&chunk_max);
    size_t off = 0U;

    if ((data == NULL) && (len != 0U)) {
        return -1;
    }

    /* TODO: add sequence numbers + CRC and hand each chunk to RadioLib.
       The staging buffer already lives in parity-protected SRAM2. */
    while (off < len) {
        size_t n = ((len - off) < chunk_max) ? (len - off) : chunk_max;
        memcpy(tx, data + off, n);
        /* TODO: lora_tx(tx, n); */
        off += n;
    }
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
        size_t beacon_len = 0U;
        uint8_t *beacon = comms_beacon_buffer(&beacon_len);

        /* TODO: build beacon packet (96 B telemetry + 32 B sys) in `beacon` */
        (void)beacon;
        (void)beacon_len;
        /* TODO: lora_tx(beacon, beacon_len); */

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
        size_t rx_len = 0U;
        uint8_t *rx = comms_rx_buffer(&rx_len);

        /* TODO: enter RX mode, wait for interrupt, decode packet into `rx` */
        (void)rx;
        (void)rx_len;
        /* TODO: CRC check → decrypt → dispatch command */
        osDelay(pdMS_TO_TICKS(100));
    }
}

osThreadId_t lora_rx_task_create(void)
{
    return osThreadNew(lora_rx_task, NULL, &rx_attrs);
}
