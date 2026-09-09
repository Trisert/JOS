#ifndef COMMS_VALIDATE_H
#define COMMS_VALIDATE_H

/**
 * @file    comms_validate.h
 * @brief   Uplink telecommand (TC) validation for the LoRa SX1268 RX path.
 *
 * Every frame received on the uplink MUST pass comms_validate_tc() before it
 * reaches the (file-static) dispatcher in comms.c; the single exported entry
 * point is comms_rx_handle_frame(). Validation is purely defensive and does not
 * modify the RX state machine.
 *
 * SCOPE — structural validation PLUS keyed authentication. The CRC-16/CCITT-FALSE
 * trailer is unkeyed, so it detects corruption and malformed frames only. Any
 * transmitter that knows this frame format can forge a CRC-valid telecommand,
 * and replayed frames are accepted. Authenticated uplink adds a truncated
 * HMAC-SHA256 tag (upstream RedPill design, COMMS.h/makeMAC: HMAC-SHA256 over
 * the header + payload with key {A1,B2,C3,D4}, truncated to 4 bytes
 * big-endian); frames without a valid tag are rejected with COMMS_TC_ERR_MAC
 * and never dispatched. Replay protection (monotonic counter / timestamp) is
 * NOT provided — the JOS frame carries no counter field yet; tracked
 * separately, see the note on COMMS_AUTH_ENFORCE.
 *
 * Authenticated frame layout (big-endian on the wire):
 *
 *   offset  size  field
 *   ------  ----  ---------------------------------------------------------
 *     0      1    opcode      (telecommand id)
 *     1      1    length P    (payload byte count, 0 .. COMMS_TC_MAX_AUTH_PAYLOAD)
 *     2      P    payload
 *    2+P     4    TAG         (truncated HMAC-SHA256 over bytes [0 .. 2+P-1])
 *    6+P     2    CRC-16/CCITT-FALSE over bytes [0 .. 6+P-1], big-endian
 *
 *   total authenticated frame size = P + COMMS_TC_AUTH_OVERHEAD
 *
 * Layout discrimination is exact: a received length of P+8 is authenticated,
 * P+4 is legacy. Both equations cannot hold for the same header byte, so a
 * frame is never parsed both ways.
 *
 * Frame layout (big-endian on the wire):
 *
 *   offset  size  field
 *   ------  ----  ---------------------------------------------------------
 *     0      1    opcode      (telecommand id)
 *     1      1    length N    (payload byte count, 0 .. COMMS_TC_MAX_PAYLOAD)
 *     2      N    payload
 *    2+N     2    CRC-16/CCITT-FALSE over bytes [0 .. 2+N-1], big-endian
 *
 *   total frame size = N + COMMS_TC_OVERHEAD
 *
 * Standards:
 *   - NASA Power of Ten rule #1 / #5 : no unbounded arithmetic, all inputs
 *     bounds-checked before use; fixed upper bounds on every loop.
 *   - NASA-STD-8739.8 : command validation — malformed, oversized, unknown or
 *     out-of-range commands are rejected, never executed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum LoRa PHY payload accepted on the uplink (bytes). */
#define COMMS_TC_MAX_FRAME    64U

/** Header (opcode + length) + trailing CRC-16. */
#define COMMS_TC_HDR_LEN       2U
#define COMMS_TC_CRC_LEN       2U
#define COMMS_TC_OVERHEAD     (COMMS_TC_HDR_LEN + COMMS_TC_CRC_LEN)

/** Smallest legal frame: opcode + length + CRC, zero-byte payload. */
#define COMMS_TC_MIN_FRAME    COMMS_TC_OVERHEAD

/** Largest payload that still fits inside COMMS_TC_MAX_FRAME. */
#define COMMS_TC_MAX_PAYLOAD  (COMMS_TC_MAX_FRAME - COMMS_TC_OVERHEAD)

/* ---------- Authenticated uplink (HMAC-SHA256, truncated) ---------- */

/** Tag length: first 4 bytes of HMAC-SHA256, big-endian (upstream makeMAC). */
#define COMMS_TC_MAC_LEN       4U

/** Header (opcode + length) + tag + trailing CRC-16. */
#define COMMS_TC_AUTH_OVERHEAD (COMMS_TC_HDR_LEN + COMMS_TC_MAC_LEN + COMMS_TC_CRC_LEN)

/** Smallest legal authenticated frame: opcode + length + tag + CRC. */
#define COMMS_TC_MIN_AUTH_FRAME COMMS_TC_AUTH_OVERHEAD

/** Largest payload that still fits inside COMMS_TC_MAX_FRAME with a tag. */
#define COMMS_TC_MAX_AUTH_PAYLOAD (COMMS_TC_MAX_FRAME - COMMS_TC_AUTH_OVERHEAD)

/**
 * Uplink HMAC key (4 bytes, upstream RedPill SECRET_KEY = A1 B2 C3 D4).
 * Per-mission override with -DCOMMS_AUTH_KEY0=.. -DCOMMS_AUTH_KEY3=.. ; the
 * ground station must use the same key or every uplink is rejected.
 */
#ifndef COMMS_AUTH_KEY0
#define COMMS_AUTH_KEY0 0xA1U
#endif
#ifndef COMMS_AUTH_KEY1
#define COMMS_AUTH_KEY1 0xB2U
#endif
#ifndef COMMS_AUTH_KEY2
#define COMMS_AUTH_KEY2 0xC3U
#endif
#ifndef COMMS_AUTH_KEY3
#define COMMS_AUTH_KEY3 0xD4U
#endif

/**
 * Authentication enforcement (SPF v3 §3.6.6.3 / §3.7: uplink authentication
 * required). 1 = comms_rx_handle_frame() rejects any frame without a valid
 * tag (legacy CRC-only frames fail with COMMS_TC_ERR_MAC); 0 = legacy
 * CRC-only frames are still accepted (bench / migration only — never flight).
 * Override with -DCOMMS_AUTH_ENFORCE=0.
 */
#ifndef COMMS_AUTH_ENFORCE
#define COMMS_AUTH_ENFORCE 1
#endif

/** Telecommand opcodes (mirrors the dispatcher in comms.c). */
#define COMMS_TC_RESET                0x01U
#define COMMS_TC_EXIT_STATE           0x02U
#define COMMS_TC_SET_CONFIG           0x03U
#define COMMS_TC_SEND_DATA            0x04U
#define COMMS_TC_ACTIVATE_PAYLOAD     0x05U
#define COMMS_TC_SET_BEACON_INTERVAL  0x06U

/** Accepted beacon interval bounds (ms). 0 is the "use per-state default" escape. */
#define COMMS_TC_BEACON_MIN_MS        1000UL      /*  1 s  */
#define COMMS_TC_BEACON_MAX_MS        3600000UL   /*  1 h  */

/** Validation verdicts. Only COMMS_TC_OK may be dispatched. */
typedef enum {
    COMMS_TC_OK = 0,            /**< frame is well formed and in range        */
    COMMS_TC_ERR_NULL,          /**< NULL buffer / NULL output pointer        */
    COMMS_TC_ERR_TOO_SHORT,     /**< shorter than the minimum frame           */
    COMMS_TC_ERR_TOO_LONG,      /**< larger than COMMS_TC_MAX_FRAME           */
    COMMS_TC_ERR_LEN_MISMATCH,  /**< header length != actual frame length     */
    COMMS_TC_ERR_CRC,           /**< CRC-16/CCITT mismatch                    */
    COMMS_TC_ERR_OPCODE,        /**< opcode not in the whitelist table        */
    COMMS_TC_ERR_PAYLOAD_LEN,   /**< payload length illegal for this opcode   */
    COMMS_TC_ERR_PARAM_RANGE,   /**< numeric parameter outside min/max bounds */
    COMMS_TC_ERR_MAC,           /**< missing or invalid HMAC tag (appended last
                                     so all existing verdict values are stable) */
} comms_tc_result_t;

/** RX acceptance/rejection counters (telemetry + ground diagnostics). */
typedef struct {
    uint32_t accepted;
    uint32_t rejected;
    uint32_t rejected_crc;
    uint32_t rejected_malformed;  /**< too short/long, length mismatch        */
    uint32_t rejected_opcode;
    uint32_t rejected_range;      /**< payload length or parameter range      */
    uint32_t rejected_mac;        /**< missing or invalid HMAC tag            */
} comms_rx_stats_t;

/**
 * @brief CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).
 *
 * Defensive contract: @p data == NULL returns the init value 0xFFFF (which will
 * not match a real frame trailer), and @p len is clamped to COMMS_TC_MAX_FRAME
 * so the loop is always bounded. Callers must not use it as a general-purpose
 * CRC over buffers larger than one uplink frame.
 */
uint16_t comms_crc16_ccitt(const uint8_t *data, size_t len);

/**
 * @brief Validate a raw uplink frame: structure, CRC, opcode, parameter ranges.
 *
 * @param[in]  frame        raw received bytes (may be NULL — rejected)
 * @param[in]  len          number of bytes in @p frame
 * @param[out] out_opcode   opcode, written only on COMMS_TC_OK
 * @param[out] out_payload  pointer into @p frame, written only on COMMS_TC_OK
 * @param[out] out_len      payload byte count, written only on COMMS_TC_OK
 *
 * @return COMMS_TC_OK when the frame may be dispatched, otherwise the reason.
 *
 * @note Pure function: no side effects, no allocation, no blocking.
 */
comms_tc_result_t comms_validate_tc(const uint8_t   *frame,
                                    size_t           len,
                                    uint8_t         *out_opcode,
                                    const uint8_t  **out_payload,
                                    size_t          *out_len);

/** @brief Human-readable verdict, for logging. Never returns NULL. */
const char *comms_tc_result_str(comms_tc_result_t result);

/** @brief Snapshot of the RX validation counters. Safe with @p out == NULL. */
void comms_rx_get_stats(comms_rx_stats_t *out);

/** @brief Update counters from a verdict (called by the RX path). */
void comms_rx_account(comms_tc_result_t result);

/* ---------- Authenticated uplink ---------- */

/**
 * @brief Compute the uplink authentication tag for a header+payload slice.
 *
 * Truncated HMAC-SHA256 (upstream makeMAC design): HMAC-SHA256 with the
 * COMMS_AUTH_KEY* key over @p data[0..len-1], first 4 bytes big-endian.
 * Ground stations call this over opcode|length|payload to seal a frame;
 * the flight side recomputes it inside comms_validate_tc_auth().
 *
 * Defensive contract: @p data == NULL returns 0, and @p len is clamped to
 * COMMS_TC_MAX_FRAME so the hash loop is always bounded. (The validator
 * rejects NULL frames before any tag comparison, so 0 is never accepted.)
 */
uint32_t comms_auth_tag(const uint8_t *data, size_t len);

/**
 * @brief True when (frame, len) has the authenticated layout (len == P + 8).
 *
 * NULL-safe (NULL returns false). Reads frame[1] only when len >= 2, so it
 * is safe on runt frames. Used by comms_rx_handle_frame() to pick the
 * validator before touching frame content.
 */
bool comms_frame_is_auth_layout(const uint8_t *frame, size_t len);

/**
 * @brief Validate an authenticated uplink frame: structure, CRC, HMAC tag,
 * opcode whitelist, parameter ranges — verify-then-dispatch.
 *
 * Check order is deliberate: framing first (bounds before content), then
 * CRC (cheap corruption filter), then the HMAC tag (authentication before
 * any opcode parsing, so unauthenticated senders learn nothing about the
 * whitelist), then opcode and parameter ranges. A bad tag returns
 * COMMS_TC_ERR_MAC and the frame must never be dispatched.
 *
 * Out-parameter contract identical to comms_validate_tc().
 */
comms_tc_result_t comms_validate_tc_auth(const uint8_t   *frame,
                                         size_t           len,
                                         uint8_t         *out_opcode,
                                         const uint8_t  **out_payload,
                                         size_t          *out_len);

#endif /* COMMS_VALIDATE_H */
