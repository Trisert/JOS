#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include <stddef.h>
#include "cmsis_os2.h"
#include "comms_validate.h"

/* Sizes of the parity-protected comms buffers (SRAM2, see comms.c). */
#define COMMS_MAX_PACKET   64U    /* LoRa payload chunk */
#define COMMS_BEACON_SIZE  128U   /* 96 B telemetry + 32 B system */

/* Chunk framing header (see lora_send_chunked): byte 0 = message id, byte 1 =
   0-based sequence number, byte 2 = total chunk count. Payload per chunk is
   therefore COMMS_MAX_PACKET - COMMS_CHUNK_HDR_LEN; the 128 B beacon ships as
   3 chunks. The message id is a monotonic per-sequence counter: consecutive
   beacons carry consecutive ids, so the ground segment can tell two beacons
   apart, spot a missing message, and flag a duplicate (same id + seq seen
   twice, e.g. a re-driven TX_DONE). Wraps mod 256 — duplicates are only
   unambiguous within a 256-message window, which is plenty at beacon rates. */
#define COMMS_CHUNK_HDR_LEN 3U
#define COMMS_CHUNK_OFF_MSG   0U
#define COMMS_CHUNK_OFF_SEQ   1U
#define COMMS_CHUNK_OFF_TOTAL 2U

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

/* Send data in chunks of at most COMMS_MAX_PACKET bytes.
 *
 * Every chunk carries a COMMS_CHUNK_HDR_LEN-byte header (byte 0 = message id,
 * byte 1 = 0-based sequence number, byte 2 = total chunk count) so the
 * ground segment can tell consecutive messages apart, detect a
 * lost/duplicated/reordered chunk and reassemble the payload. Returns 0 on
 * success, -1 on NULL/length errors, an undersized TX buffer, a count that
 * would not fit in the 1-byte total field, or a radio error.
 *
 * Failure semantics: the sequence aborts at the first failed chunk (no
 * further chunks go on air — a partial message is always detectable via its
 * total field) and the failure is counted in the TX stats below. The beacon
 * task retries the whole message at the next interval.
 *
 * Blocking: one lora_tx_wait_done() (up to 2000 ms) per chunk. The 128 B
 * beacon is 3 chunks, so a beacon TX blocks this task for up to ~6 s. That
 * is covered by the watchdog with huge margin — the beacon task is monitored
 * against its 1..16 min cadence (flagged only after 3x the period, i.e. no
 * earlier than 3 min), so no mid-sequence kick is needed. */
int lora_send_chunked(const uint8_t *data, size_t len);

/* TX sequence counters (telemetry + ground diagnostics). A failed sequence
 * means the message was truncated on air — always cross-check chunks_sent
 * against the per-message total field on the ground. */
typedef struct {
    uint32_t sequences_ok;      /**< fully transmitted messages            */
    uint32_t sequences_failed;  /**< aborted mid-sequence (radio error)    */
    uint32_t chunks_sent;       /**< chunks handed to the radio, all time  */
} comms_tx_stats_t;

/* Snapshot of the TX counters. Safe with @p out == NULL. */
void comms_tx_get_stats(comms_tx_stats_t *out);

/* Validate a raw uplink frame and dispatch it only when it is well formed,
 * CRC-clean, HMAC-authenticated, of a whitelisted opcode and with in-range
 * parameters.
 *
 * This is the ONLY exported entry point for received telecommands: the
 * dispatcher itself is file-static inside comms.c, so no caller can reach an
 * opcode without passing validation first. Layout discrimination is by exact
 * length (len == P+8 selects the authenticated validator); with
 * COMMS_AUTH_ENFORCE=1 (flight default) untagged legacy frames are rejected
 * with COMMS_TC_ERR_MAC, with =0 (bench only) they are still dispatched.
 *
 * NOTE: the CRC is unkeyed (CRC-16/CCITT-FALSE) — it is a corruption check,
 * not authentication. Authentication is the truncated HMAC-SHA256 tag; there
 * is still no replay protection (no counter field yet).
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
