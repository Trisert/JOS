#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cmsis_os2.h"
#include "comms_validate.h"
#include "tec.h"          /* single source of truth for the TEC type/task ids */

/* Sizes of the parity-protected comms buffers (SRAM2, see comms.c). */
#define COMMS_MAX_PACKET   64U    /* LoRa payload chunk */

/* LoRa telemetry beacon BUFFER size: 96 B telemetry + 32 B system = 128 B,
 * transmitted in COMMS_MAX_PACKET-sized chunks. NOT the housekeeping (HB)
 * beacon frame of App/obsw/beacon.[ch], which is a different object: a 64-byte
 * (BEACON_HB_LEN) byte-exact housekeeping frame. The two were both called
 * "beacon size" and are not interchangeable, hence the explicit LORA name. */
#define COMMS_LORA_BEACON_SIZE 128U

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
 *   [tail]  6    RS ECC     (RS(255,249) parity, ONLY when the ECC flag is ON)
 *
 * INFO byte decode. The workbook's 'Task details' sheet settles the bit order
 * its own prose leaves open (O3, now closed): column D "TEC bin ID" is the
 * 8-bit byte formed by prefixing the TEC-type Bin ID ('Task types'!C4:C7 —
 * HK=00, DAQ=01, PE=10, DT=11) to the 6-bit task Bin ID (column C, also the
 * task id), and column E "TEC hex ID" is that byte in hex. Worked examples:
 * 'Task details'!E5 (HK task 1, OBC reboot) = 0x01; 'Task details'!E21
 * (HK task 17, TLE) = 0x11; 'Task details'!E55 (HK task 51, Lora link) =
 * 0x33. So bit 1 is the MOST significant bit of the byte:
 *
 *   byte 0: Station ID   1..255              (COMMS_TTC_STATION_MIN/MAX)
 *   byte 1: ECC flag     0x55 = off, 0xAA = on
 *   byte 2: bit 1-2 = TEC type (MSB, 0..3), bit 3-8 = TEC task (0..63)
 *   byte 3: PL length    0..100
 *
 * i.e. the packed byte is (tec_type << 6) | tec_task.
 *
 * Alignment: the packet handed to the radio is ALWAYS a multiple of 16 — the
 * source requires 16*n "for correct interleaving" (!A14) and the RS PARITY
 * element is fixed at 6 bytes (!H54). With ECC off a 16-byte block is 16 data
 * bytes; with ECC on it is a systematic RS(16,10) codeword, i.e. 10 data bytes
 * followed by the 6 parity bytes. The parity therefore rides INSIDE the blocks
 * and an ECC packet is 16*ceil(content/10), NOT 16*n + 6. See assumption A1 in
 * docs/api/ttc-frame.md — the sheet's own sketch draws the parity sparsely and
 * does not disambiguate the order, so the systematic one is flagged for TT&C.
 * Padding is explicitly an application-layer responsibility (source text); it
 * is zero-filled here, and each block's RS parity is computed over that
 * block's data bytes (padding included).
 *
 * MAC SEAM — deliberately opaque. The source only says "hash to validate GS
 * command": it does NOT define the algorithm, the keying, or whether the MAC
 * covers the padding. JOS carries a DIFFERENT scheme on board today
 * (truncated HMAC-SHA256 over opcode|length|payload, see comms_validate.h and
 * sha256.c), and the two are NOT reconciled by this module. build_frame()
 * copies the caller's 4 bytes verbatim (zeros when NULL); parse_frame() hands
 * the same 4 bytes back untouched. No MAC is COMPUTED here; verification is
 * delegated to the pluggable seam comms_ttc_set_mac_verifier(), which the RX
 * entry point calls with those 4 bytes. The default hook REJECTS every frame,
 * so the RX path fails closed until the crypto decision is made. Nothing here
 * removes sha256.c.
 * ====================================================================== */

/** INFO (4) + UNIX TIME (4) + MAC (4). */
#define COMMS_TTC_INFO_LEN      4U
#define COMMS_TTC_TIME_LEN      4U
#define COMMS_TTC_MAC_LEN       4U
#define COMMS_TTC_HDR_LEN       (COMMS_TTC_INFO_LEN + COMMS_TTC_TIME_LEN + COMMS_TTC_MAC_LEN)

/** Interleaving granularity: every TT&C frame length is a multiple of this. */
#define COMMS_TTC_BLOCK         16U

/** DATA bytes carried by one 16-byte block when the ECC flag is ON: the block
 *  is a systematic RS(16,10) codeword, so 10 of its 16 bytes are data. */
#define COMMS_TTC_RS_DATA_LEN   10U

/** Parity bytes inside each 16-byte block when the ECC flag is ON.
 *  'Packet structure'!H54: the RS PARITY element is "6 bytes". The encoder
 *  lives in comms.c (single TU) — see comms_ttc_rs_ecc_encode() and
 *  docs/api/ttc-frame.md for the field/generator citation and for the
 *  block-geometry assumption (A1: RS(16,10) per 16-byte block, so the packet
 *  stays 16*n as 'Packet structure'!A14 requires — there is NO parity tail). */
#define COMMS_TTC_RS_PARITY_LEN 6U

/** RS parity symbols per codeword (one parity byte per block slot). */
#define COMMS_TTC_RS_NSYM       COMMS_TTC_RS_PARITY_LEN

/** Largest DATA section one RS encode accepts (GF(256) code-length bound). */
#define COMMS_TTC_RS_MAX_DATA   (255U - COMMS_TTC_RS_NSYM)

/** Largest payload the field allows (INFO byte 3: 0..100). */
#define COMMS_TTC_MAX_PL        100U

/** Smallest legal frame: one interleaving block (header only, zero payload). */
#define COMMS_TTC_MIN_FRAME     COMMS_TTC_BLOCK

/** Largest legal frame, a whole number of 16-byte blocks. The worst case is
 *  ECC ON with a 100-byte payload: 112 content bytes / 10 data bytes per
 *  codeword = 12 blocks = 192 B (ECC OFF needs only 7 blocks = 112 B). */
#define COMMS_TTC_MAX_FRAME     (COMMS_TTC_BLOCK * 12U)

/** ECC flag values (INFO byte 1). */
#define COMMS_TTC_ECC_OFF       0x55U
#define COMMS_TTC_ECC_ON        0xAAU

/** Station ID range (INFO byte 0). 0 is not a legal station. */
#define COMMS_TTC_STATION_MIN   1U
#define COMMS_TTC_STATION_MAX   255U

/** INFO byte 2 bitfield, MSB-first: bit 1-2 = TEC type, bit 3-8 = TEC task. */
#define COMMS_TTC_TEC_TYPE_MASK 0xC0U
#define COMMS_TTC_TEC_TASK_MASK 0x3FU
#define COMMS_TTC_TEC_TYPE_MAX  3U
/* The 6-bit task field can hold 0..63. The source prose quotes "0-31" while
 * its own command table uses task ids up to 0x33 (51, 6 bits) — see open
 * point O5. The full 6-bit width is accepted here. */
#define COMMS_TTC_TEC_TASK_MAX  63U

/* SINGLE SOURCE OF TRUTH — the TEC type and task ids are OWNED by tec.h
 * (TEC_WIRE_TYPE_* and TEC_TASK_HK_*); the COMMS_TTC_* names below are thin
 * aliases, never a second definition of the same value. They are kept because
 * the frame codec and docs/api/ttc-frame.md speak in COMMS_TTC_* terms and
 * because a rename there would silently change what "the task id" means.
 * Changing a value means changing it in tec.h, once. */

/** TEC task types: the 2-bit Bin IDs from 'Task types'!C4:C7 (NOT the
 *  human-facing Name IDs 1..4 in column B, which are tec.h's tec_type_t).
 *  The packed INFO byte is (COMMS_TTC_TEC_* << 6) | task, so the wire value —
 *  not tec_type_t — is what belongs here; see tec.h "DO NOT use the enum value
 *  as a wire value". Source: tec.h TEC_WIRE_TYPE_*. */
#define COMMS_TTC_TEC_HK        TEC_WIRE_TYPE_HK    /* Bin ID 00 — housekeeping      */
#define COMMS_TTC_TEC_DAQ       TEC_WIRE_TYPE_DAQ   /* Bin ID 01 — data acquisition  */
#define COMMS_TTC_TEC_PE        TEC_WIRE_TYPE_PE    /* Bin ID 10 — payload execution */
#define COMMS_TTC_TEC_DT        TEC_WIRE_TYPE_DT    /* Bin ID 11 — data transfer     */

/** Telecommand task ids from the source's HK command table. Source: tec.h
 *  TEC_TASK_HK_* (one row per defined spec task). */
#define COMMS_TTC_TASK_OBC_REBOOT   TEC_TASK_HK_OBC_REBOOT
#define COMMS_TTC_TASK_EXIT_STATE   TEC_TASK_HK_EXIT_STATE
#define COMMS_TTC_TASK_VAR_CHANGE   TEC_TASK_HK_VAR_CHANGE
#define COMMS_TTC_TASK_SET_TIME     TEC_TASK_HK_SET_TIME
#define COMMS_TTC_TASK_EPS_REBOOT   TEC_TASK_HK_EPS_REBOOT
#define COMMS_TTC_TASK_ADCS_REBOOT  TEC_TASK_HK_ADCS_REBOOT
#define COMMS_TTC_TASK_TLE          TEC_TASK_HK_TLE
#define COMMS_TTC_TASK_LORA_STATE   TEC_TASK_HK_LORA_STATE
#define COMMS_TTC_TASK_LORA_CONFIG  TEC_TASK_HK_LORA_CONFIG
#define COMMS_TTC_TASK_LORA_PING    TEC_TASK_HK_LORA_PING
#define COMMS_TTC_TASK_ACK          TEC_TASK_HK_ACK
#define COMMS_TTC_TASK_NACK         TEC_TASK_HK_NACK
#define COMMS_TTC_TASK_LORA_LINK    TEC_TASK_HK_LORA_LINK

/* The alias expansion above is the contract, and a silently drifting alias is
 * worse than a duplicated literal: these pin it at compile time against the
 * literals of the source table so a change in tec.h that moves a value is a
 * build error here, not a runtime surprise. */
_Static_assert(TEC_WIRE_TYPE_HK == 0U && TEC_WIRE_TYPE_DAQ == 1U &&
               TEC_WIRE_TYPE_PE == 2U && TEC_WIRE_TYPE_DT == 3U,
               "TEC wire Bin IDs must stay 00/01/10/11");
_Static_assert(TEC_TASK_HK_OBC_REBOOT  == 0x01U && TEC_TASK_HK_EXIT_STATE == 0x02U &&
               TEC_TASK_HK_VAR_CHANGE  == 0x03U && TEC_TASK_HK_SET_TIME   == 0x04U &&
               TEC_TASK_HK_EPS_REBOOT  == 0x08U && TEC_TASK_HK_ADCS_REBOOT == 0x10U &&
               TEC_TASK_HK_TLE         == 0x11U && TEC_TASK_HK_LORA_STATE  == 0x18U &&
               TEC_TASK_HK_LORA_CONFIG == 0x19U && TEC_TASK_HK_LORA_PING   == 0x1AU &&
               TEC_TASK_HK_ACK         == 0x31U && TEC_TASK_HK_NACK        == 0x32U &&
               TEC_TASK_HK_LORA_LINK   == 0x33U,
               "TEC task ids must keep the HK command-table values");

/** TT&C frame codec verdicts. Kept separate from comms_tc_result_t so the
 *  alignment and INFO-field classes stay distinguishable in telemetry;
 *  comms_ttc_to_tc_result() maps them onto the existing RX counters. */
typedef enum {
    COMMS_TTC_OK = 0,           /**< frame is well formed                    */
    COMMS_TTC_ERR_NULL,         /**< NULL frame / INFO / output pointer      */
    COMMS_TTC_ERR_TOO_SHORT,    /**< shorter than one interleaving block     */
    COMMS_TTC_ERR_TOO_LONG,     /**< larger than COMMS_TTC_MAX_FRAME         */
    COMMS_TTC_ERR_ALIGN,        /**< data section is not a multiple of 16   */
    COMMS_TTC_ERR_LEN_MISMATCH, /**< len != comms_ttc_padded_len(HDR+PL,ecc) */
    COMMS_TTC_ERR_PL_LEN,       /**< PL length > 100 or truncated by padding */
    COMMS_TTC_ERR_STATION_ID,   /**< station id < 1                          */
    COMMS_TTC_ERR_ECC_FLAG,     /**< ECC flag is neither 0x55 nor 0xAA       */
    COMMS_TTC_ERR_TEC_TYPE,     /**< TEC type outside 0..3                   */
    COMMS_TTC_ERR_TEC_TASK,     /**< TEC task outside 0..63                  */
    COMMS_TTC_ERR_UNSUPPORTED,  /**< TEC type/task is not an implemented cmd */
    COMMS_TTC_ERR_PAYLOAD,      /**< command payload short/incoherent        */
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

/** A parsed TT&C frame.
 *
 *  With the ECC flag OFF the @c mac and @c payload pointers alias the
 *  caller's buffer (no copy). With ECC ON every block interleaves 10 data
 *  with 6 parity bytes, so the fields are not contiguous on air: the parser
 *  de-interleaves them into a module-internal buffer and the pointers stay
 *  valid until the next comms_ttc_parse_frame() call (the RX path is
 *  single-threaded). */
typedef struct {
    comms_ttc_info_t info;
    uint32_t         unix_time;
    const uint8_t   *mac;        /**< pointer to the 4 opaque MAC bytes    */
    const uint8_t   *payload;    /**< NULL when pl_len == 0                */
    size_t           frame_len;  /**< total on-air length (always 16*n)    */
    size_t           data_len;   /**< DATA bytes the frame carries: frame_len
                                      (ECC off) or blocks*10 (ECC on)        */
} comms_ttc_frame_t;

/**
 * @brief Total padded frame length for @p content_len data bytes.
 *
 * The on-air packet is ALWAYS a whole number of 16-byte blocks (16*n,
 * 'Packet structure'!A14) — with the ECC flag on and off alike. Only the DATA
 * capacity of a block changes with ECC: a block holds COMMS_TTC_BLOCK (16)
 * data bytes with ECC off, and COMMS_TTC_RS_DATA_LEN (10) data bytes + the 6
 * parity bytes with ECC on. So this is
 * @c 16 * ceil(content_len / 16) (ECC off) or
 * @c 16 * ceil(content_len / 10) (ECC on) — never 16*n + 6.
 * Pure helper; returns 0 for a content length of 0.
 */
size_t comms_ttc_padded_len(size_t content_len, bool ecc_on);

/** @brief Pack TEC type (bits 1-2, MSB) and TEC task (bits 3-8) into INFO byte 2.
 *  The byte is (tec_type << 6) | tec_task. */
uint8_t comms_ttc_info_pack(uint8_t tec_type, uint8_t tec_task);

/**
 * @brief Reed-Solomon (255,249) shortened encoder over GF(256), 6 parity
 *        symbols — the RS PARITY element ('Packet structure'!H54).
 *
 * Computes @c data(x) * x^6 mod g(x) with g(x) = prod_{i=0..5}(x - alpha^i),
 * over GF(256) with primitive polynomial 0x11D (x^8+x^4+x^3+x^2+1) and
 * alpha = 2. These are the standard RS(255,249) field/generator parameters
 * (python-reedsolo defaults; Wicker & Bhargava 1994, ch. 5) and are NOT
 * invented here. Encoding is systematic: the 6 parity symbols follow the
 * data bytes of the codeword. The codec calls it once per 16-byte block with
 * a 10-byte data field, so the parity lands in the block's 6 parity slots
 * (COMMS_TTC_RS_DATA_LEN + COMMS_TTC_RS_PARITY_LEN == COMMS_TTC_BLOCK).
 * Known-answer vectors validated against reedsolo and an independent
 * implementation are pinned in test/test_comms.c.
 *
 * @param[in]  data    DATA section bytes (non-NULL, len <= COMMS_TTC_RS_MAX_DATA)
 * @param[in]  len     data length
 * @param[out] parity  COMMS_TTC_RS_NSYM bytes, always fully written; zeroed
 *                     on a NULL/oversize call (no uninitialised bytes on air)
 */
void comms_ttc_rs_ecc_encode(const uint8_t *data, size_t len,
                             uint8_t parity[COMMS_TTC_RS_NSYM]);

/** @brief Unpack INFO byte 2. NULL output pointers are ignored. */
void comms_ttc_info_unpack(uint8_t byte2, uint8_t *tec_type, uint8_t *tec_task);

/**
 * @brief Build a TT&C command frame into @p out (TX path).
 *
 * Writes INFO, UNIX TIME, the opaque MAC, the payload, zero padding to the
 * 16-byte block boundary and (when the ECC flag is ON) a real 6-byte
 * Reed-Solomon parity inside each block, computed over that block's 10 data
 * bytes. @p info->pl_len must equal @p pl_len. @p mac may be NULL (field
 * written as four zero bytes).
 *
 * On COMMS_TTC_OK, @p *out_len receives the frame length (always 16*n: 16*n
 * data-only blocks with ECC off, 16*n RS(16,10) codewords with ECC on). On
 * COMMS_TTC_ERR_BUF, @p *out_len receives the length that
 * would be needed. Verdict codes carry the same meaning as
 * comms_ttc_result_str().
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
 * INFO field ranges (station id, ECC flag, PL length), the 16*n alignment
 * (both geometries), the CANONICAL length (len must equal
 * comms_ttc_padded_len(HDR + PL, ecc) — anything else is
 * COMMS_TTC_ERR_LEN_MISMATCH) and that the declared PL length actually fits
 * the data section. With ECC off the out-struct points into @p frame; with
 * ECC on the header/payload are de-interleaved into a module-internal buffer
 * (see comms_ttc_frame_t).
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
 * Parses the frame, runs the MAC seam, dispatches by TEC type/task on success
 * and accounts the FINAL verdict in the existing RX counters. Unsupported TEC
 * types/tasks and short/incoherent command payloads are rejected
 * (COMMS_TTC_ERR_UNSUPPORTED / COMMS_TTC_ERR_PAYLOAD) and counted as
 * rejections — never as accepted. Until the MAC semantics are defined the
 * seam rejects everything, so this entry point parses but never executes a
 * command (fail closed). The legacy comms_rx_handle_frame() behaviour is
 * preserved for non-TT&C layouts.
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
