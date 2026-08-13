#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include <stddef.h>
#include "cmsis_os2.h"
#include "comms_validate.h"

/* Sizes of the parity-protected comms buffers (SRAM2, see comms.c). */
#define COMMS_MAX_PACKET   64U    /* LoRa payload chunk */
#define COMMS_BEACON_SIZE  128U   /* 96 B telemetry + 32 B system */

/* RX task wake flag (DIO1 RX_DONE). Must match LORA_FLAG_RX_DONE in
   radiolib_driver.cpp so the ISR and the task agree on the bit. */
#define LORA_RX_FLAG 0x02U

/* Initialise LoRa transceiver (SX1268 on SPI1) */
int lora_init(void);

/* Beacon TX task — sends beacon at state-dependent interval */
void lora_beacon_task(void *arg);
osThreadId_t lora_beacon_task_create(void);
osThreadId_t lora_rx_task_create(void);

/* RX task — continuous uplink listening, command dispatch */
void lora_rx_task(void *arg);

/* Send data in 64-byte chunks */
int lora_send_chunked(const uint8_t *data, size_t len);

/* Validate a raw uplink frame and dispatch it only when it is well formed,
 * CRC-clean, of a whitelisted opcode and with in-range parameters.
 *
 * This is the ONLY exported entry point for received telecommands: the
 * dispatcher itself is file-static inside comms.c, so no caller can reach an
 * opcode without passing structural validation first.
 *
 * NOTE: the CRC is unkeyed (CRC-16/CCITT-FALSE) — this is *structural*
 * validation against corruption and malformed frames, NOT authentication.
 * It provides no protection against a deliberately forged or replayed uplink.
 *
 * Returns COMMS_TC_OK when dispatched, otherwise the rejection reason. */
comms_tc_result_t comms_rx_handle_frame(const uint8_t *frame, size_t len);

/* ---------- Parity-protected packet buffers (SRAM2) ----------
   The beacon, RX and TX buffers hold the only copy of a telemetry frame or of
   an uplinked telecommand while it is being assembled or decoded, so they are
   allocated in the SRAM2 block whose hardware parity turns a bit flip into an
   NMI instead of a corrupted command (W2-3, NASA-STD-8739.8 data integrity).
   Each accessor returns the buffer base and, when len is non-NULL, its size. */
uint8_t *comms_beacon_buffer(size_t *len);
uint8_t *comms_rx_buffer(size_t *len);
uint8_t *comms_tx_buffer(size_t *len);

#endif /* COMMS_H */
