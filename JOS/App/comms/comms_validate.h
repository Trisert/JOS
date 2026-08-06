#ifndef COMMS_VALIDATE_H
#define COMMS_VALIDATE_H

/**
 * @file    comms_validate.h
 * @brief   Uplink telecommand (TC) validation for the LoRa SX1268 RX path.
 *
 * Every frame received on the uplink MUST pass comms_validate_tc() before it is
 * handed to comms_dispatch_command(). Validation is purely defensive and does
 * not modify the RX state machine.
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
 *   - NASA-STD-8739.8 : command authentication / validation — malformed,
 *     oversized, unknown or out-of-range commands are rejected, never executed.
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

/** Telecommand opcodes (mirrors comms_dispatch_command()). */
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
    COMMS_TC_ERR_PARAM_RANGE    /**< numeric parameter outside min/max bounds */
} comms_tc_result_t;

/** RX acceptance/rejection counters (telemetry + ground diagnostics). */
typedef struct {
    uint32_t accepted;
    uint32_t rejected;
    uint32_t rejected_crc;
    uint32_t rejected_malformed;  /**< too short/long, length mismatch        */
    uint32_t rejected_opcode;
    uint32_t rejected_range;      /**< payload length or parameter range      */
} comms_rx_stats_t;

/**
 * @brief CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).
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

#endif /* COMMS_VALIDATE_H */
