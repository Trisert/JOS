#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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

/* ======================================================================
 * TT&C command frame — conformance to the TTC packets.xlsx specification
 * ======================================================================
 *
 * Source: SharePoint J2050space, TT&C/TTC Operations/TTC packets.xlsx, the
 * current sheet ("TT&C"). The workbook also carries a sheet "(old)" with a
 * different 5-byte-INFO revision; this module implements the CURRENT sheet
 * and documents the difference in docs/api/ttc-frame.md.
 *
 * On-the-wire layout (big-endian):
 *
 *   offset  size  field
 *   ------  ----  -------------------------------------------------------
 *     0      4    INFO       (station id | ECC flag | TEC type+task | PL len)
 *     4      4    UNIX TIME  (seconds since epoch, big-endian)
 *     8      4    MAC        (opaque 32-bit hash — semantics NOT specified)
 *    12      P    PAYLOAD    (0 .. COMMS_TTC_MAX_PL bytes)
 *   ...           PADDING    (zero bytes up to the 16-byte interleaving block)
 *   [tail] 16    RS ECC     (appended ONLY when the ECC flag is ON)
 *
 * INFO byte decode (the source does NOT state bit endianness; bit 1 is
 * taken as the least significant bit of the byte — see the open points in
 * docs/api/ttc-frame.md):
 *
 *   byte 0: Station ID   1..255              (COMMS_TTC_STATION_MIN/MAX)
 *   byte 1: ECC flag     0x55 = off, 0xAA = on
 *   byte 2: bit 1-2 = TEC type (0..3), bit 3-8 = TEC task
 *   byte 3: PL length    0..100
 *
 * Alignment: the frame length handed to the radio MUST be a multiple of 16
 * (the source requires this for interleaving). With ECC on, the 16 trailing
 * Reed-Solomon bytes are appended after the 16-byte-aligned data section, so
 * the whole frame is still a multiple of 16. Padding is explicitly an
 * application-layer responsibility (source text); it is zero-filled here.
 *
 * MAC SEAM — deliberately opaque. The source only says "hash to validate GS
 * command": it does NOT define the algorithm, the keying, or whether the MAC
 * covers the padding. JOS carries a DIFFERENT scheme on board today
 * (truncated HMAC-SHA256 over opcode|length|payload, see comms_validate.h and
 * sha256.c), and the two are NOT reconciled by this module. build_frame()
 * copies the caller's 4 bytes verbatim (zeros when NULL); parse_frame() hands
 * the same 4 bytes back untouched. No MAC is computed or verified here, and
 * nothing here removes sha256.c. Verification is a pluggable hook
 * (comms_ttc_set_mac_verifier) whose default REJECTS every frame, so the RX
 * path fails closed until the crypto decision is made.
 * ====================================================================== */

/** INFO (4) + UNIX TIME (4) + MAC (4). */
#define COMMS_TTC_INFO_LEN      4U
#define COMMS_TTC_TIME_LEN      4U
#define COMMS_TTC_MAC_LEN       4U
#define COMMS_TTC_HDR_LEN       (COMMS_TTC_INFO_LEN + COMMS_TTC_TIME_LEN + COMMS_TTC_MAC_LEN)

/** Interleaving granularity: every TT&C frame length is a multiple of this. */
#define COMMS_TTC_BLOCK         16U

/** Reed-Solomon tail appended when the ECC flag is ON (none when OFF). */
#define COMMS_TTC_ECC_TAIL_LEN  16U

/** Largest payload the field allows (INFO byte 3: 0..100). */
#define COMMS_TTC_MAX_PL        100U

/** Smallest legal frame: one interleaving block (header only, zero payload). */
#define COMMS_TTC_MIN_FRAME     COMMS_TTC_BLOCK

/** Largest legal frame: 7 data blocks (112 B = header + 100 B payload) + ECC tail. */
#define COMMS_TTC_MAX_FRAME     (COMMS_TTC_BLOCK * 8U)

/** ECC flag values (INFO byte 1). */
#define COMMS_TTC_ECC_OFF       0x55U
#define COMMS_TTC_ECC_ON        0xAAU

/** Station ID range (INFO byte 0). 0 is not a legal station. */
#define COMMS_TTC_STATION_MIN   1U
#define COMMS_TTC_STATION_MAX   255U

/** INFO byte 2 bitfield: bit 1-2 = TEC type, bit 3-8 = TEC task. */
#define COMMS_TTC_TEC_TYPE_MASK 0x03U
#define COMMS_TTC_TEC_TASK_MASK 0xFCU
#define COMMS_TTC_TEC_TYPE_MAX  3U
/* The 6-bit task field can hold 0..63. The source text quotes "0-31" while
 * its own command table uses task ids up to 0x33 (51), so the full 6-bit
 * width is accepted here; the discrepancy is an open point in the doc. */
#define COMMS_TTC_TEC_TASK_MAX  63U

/** TEC task types (source table). NOTE: DT = 4 does not fit the 2-bit type
 *  field the source also specifies — flagged as an open point in the doc. */
#define COMMS_TTC_TEC_HK        1U   /* housekeeping           */
#define COMMS_TTC_TEC_DAQ       2U   /* data acquisition       */
#define COMMS_TTC_TEC_PE        3U   /* payload execution      */
#define COMMS_TTC_TEC_DT        4U   /* data transfer (see note) */

/** Telecommand task ids from the source's HK command table. */
#define COMMS_TTC_TASK_OBC_REBOOT   0x01U
#define COMMS_TTC_TASK_EXIT_STATE   0x02U
#define COMMS_TTC_TASK_VAR_CHANGE   0x03U
#define COMMS_TTC_TASK_SET_TIME     0x04U
#define COMMS_TTC_TASK_EPS_REBOOT   0x08U
#define COMMS_TTC_TASK_ADCS_REBOOT  0x10U
#define COMMS_TTC_TASK_TLE          0x11U
#define COMMS_TTC_TASK_LORA_STATE   0x18U
#define COMMS_TTC_TASK_LORA_CONFIG  0x19U
#define COMMS_TTC_TASK_LORA_PING    0x1AU
#define COMMS_TTC_TASK_ACK          0x31U
#define COMMS_TTC_TASK_NACK         0x32U
#define COMMS_TTC_TASK_LORA_LINK    0x33U

/** TT&C frame codec verdicts. Kept separate from comms_tc_result_t so the
 *  alignment and INFO-field classes stay distinguishable in telemetry;
 *  comms_ttc_to_tc_result() maps them onto the existing RX counters. */
typedef enum {
    COMMS_TTC_OK = 0,           /**< frame is well formed                    */
    COMMS_TTC_ERR_NULL,         /**< NULL frame / INFO / output pointer      */
    COMMS_TTC_ERR_TOO_SHORT,    /**< shorter than one interleaving block     */
    COMMS_TTC_ERR_TOO_LONG,     /**< larger than COMMS_TTC_MAX_FRAME         */
    COMMS_TTC_ERR_ALIGN,        /**< length is not a multiple of 16          */
    COMMS_TTC_ERR_PL_LEN,       /**< PL length > 100 or truncated by padding */
    COMMS_TTC_ERR_STATION_ID,   /**< station id < 1                          */
    COMMS_TTC_ERR_ECC_FLAG,     /**< ECC flag is neither 0x55 nor 0xAA       */
    COMMS_TTC_ERR_TEC_TYPE,     /**< TEC type outside 0..3                   */
    COMMS_TTC_ERR_TEC_TASK,     /**< TEC task outside 0..63                  */
    COMMS_TTC_ERR_MAC,          /**< MAC missing / rejected by the seam      */
    COMMS_TTC_ERR_BUF           /**< caller output buffer too small          */
} comms_ttc_result_t;

/** INFO field, decoded. */
typedef struct {
    uint8_t station_id;   /**< 1..255                                  */
    uint8_t ecc_flag;     /**< COMMS_TTC_ECC_OFF / COMMS_TTC_ECC_ON    */
    uint8_t tec_type;     /**< 0..3 (TEC type field)                   */
    uint8_t tec_task;     /**< 0..63 (TEC task field)                  */
    uint8_t pl_len;       /**< 0..100                                  */
} comms_ttc_info_t;

/** A parsed TT&C frame. Pointers alias the caller's buffer (no copy). */
typedef struct {
    comms_ttc_info_t info;
    uint32_t         unix_time;
    const uint8_t   *mac;        /**< pointer to the 4 opaque MAC bytes    */
    const uint8_t   *payload;    /**< NULL when pl_len == 0                */
    size_t           frame_len;  /**< total on-air length (with ECC tail)  */
    size_t           data_len;   /**< frame_len minus the ECC tail         */
} comms_ttc_frame_t;

/**
 * @brief Total padded frame length for @p content_len data bytes.
 *
 * Rounds @p content_len up to the next multiple of COMMS_TTC_BLOCK, then
 * adds COMMS_TTC_ECC_TAIL_LEN when @p ecc_on. The result is always a
 * multiple of 16 (the data section and the ECC tail are each 16-aligned).
 * Pure helper; returns 0 for a content length of 0.
 */
size_t comms_ttc_padded_len(size_t content_len, bool ecc_on);

/** @brief Pack TEC type (bit 1-2) and TEC task (bit 3-8) into INFO byte 2. */
uint8_t comms_ttc_info_pack(uint8_t tec_type, uint8_t tec_task);

/** @brief Unpack INFO byte 2. NULL output pointers are ignored. */
void comms_ttc_info_unpack(uint8_t byte2, uint8_t *tec_type, uint8_t *tec_task);

/**
 * @brief Build a TT&C command frame into @p out (TX path).
 *
 * Writes INFO, UNIX TIME, the opaque MAC, the payload, zero padding to the
 * 16-byte block boundary and (when the ECC flag is ON) a zeroed 16-byte ECC
 * tail reserved for the RS layer. @p info->pl_len must equal @p pl_len.
 * @p mac may be NULL (field written as four zero bytes).
 *
 * On COMMS_TTC_OK, @p *out_len receives the frame length (a multiple of 16).
 * On COMMS_TTC_ERR_BUF, @p *out_len receives the length that would be needed.
 * Verdict codes carry the same meaning as comms_ttc_result_str().
 */
comms_ttc_result_t comms_ttc_build_frame(uint8_t                  *out,
                                         size_t                    cap,
                                         const comms_ttc_info_t   *info,
                                         uint32_t                  unix_time,
                                         const uint8_t            *mac,
                                         const uint8_t            *payload,
                                         size_t                    pl_len,
                                         size_t                   *out_len);

/**
 * @brief Parse and validate a received TT&C frame (RX path).
 *
 * Checks, in order: NULL, length bounds (one block .. COMMS_TTC_MAX_FRAME),
 * 16-byte alignment, INFO field ranges (station id, ECC flag, TEC type,
 * TEC task, PL length) and that the declared PL length actually fits the
 * data section. On COMMS_TTC_OK the out-struct points into @p frame.
 * The MAC is copied out as an opaque pointer ONLY — never verified here
 * (verification is the comms_ttc_mac_verify() seam).
 */
comms_ttc_result_t comms_ttc_parse_frame(const uint8_t     *frame,
                                         size_t             len,
                                         comms_ttc_frame_t *out);

/** @brief Human-readable verdict, for logging. Never returns NULL. */
const char *comms_ttc_result_str(comms_ttc_result_t result);

/** @brief Map a TT&C verdict onto the existing RX counter classes. */
comms_tc_result_t comms_ttc_to_tc_result(comms_ttc_result_t result);

/** @brief True when (frame, len) can only be a spec TT&C frame.
 *
 * Discriminator, cheap and collision-free against the legacy/uplink-auth
 * layouts: length is a non-zero multiple of 16 within [MIN, MAX] AND INFO
 * byte 1 carries an ECC flag value (0x55/0xAA). A legacy frame's byte 1 is
 * its payload-length (0..60), so it can never match. NULL-safe.
 */
bool comms_frame_is_ttc_layout(const uint8_t *frame, size_t len);

/**
 * @brief Validate + dispatch a TT&C command frame (verify-then-dispatch).
 *
 * Parses the frame, runs the MAC seam, accounts the verdict in the existing
 * RX counters and dispatches by TEC type/task on success. Until the MAC
 * semantics are defined the seam rejects everything, so this entry point
 * parses but never executes a command (fail closed). The legacy
 * comms_rx_handle_frame() behaviour is preserved for non-TT&C layouts.
 */
comms_tc_result_t comms_rx_handle_ttc_frame(const uint8_t *frame, size_t len);

/** MAC verdict hook: true only when the 4 opaque bytes are acceptable.
 *  @p mac points at the 4 bytes inside @p frame; @p len is the full frame. */
typedef bool (*comms_ttc_mac_verify_fn)(const uint8_t *frame, size_t len,
                                        const uint8_t mac[4]);

/** @brief Install the MAC verifier (NULL restores the fail-closed default). */
void comms_ttc_set_mac_verifier(comms_ttc_mac_verify_fn fn);

/** @brief Current MAC verdict: calls the hook when set, else rejects. */
bool comms_ttc_mac_verify(const uint8_t *frame, size_t len,
                          const uint8_t mac[4]);

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
