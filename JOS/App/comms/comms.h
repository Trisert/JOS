#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include <stddef.h>

/* Initialise LoRa transceiver (SX1268 on SPI1) */
int lora_init(void);

/* Beacon TX task — sends beacon at state-dependent interval */
void lora_beacon_task(void *arg);

/* RX task — continuous uplink listening, command dispatch */
void lora_rx_task(void *arg);

/* Send data in 64-byte chunks */
int lora_send_chunked(const uint8_t *data, size_t len);

/* Dispatch a decoded telecommand */
void comms_dispatch_command(uint8_t cmd_id, const uint8_t *payload, size_t len);

#endif /* COMMS_H */
