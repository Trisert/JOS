/**
 * @file    comms_validate.c
 * @brief   Uplink telecommand validation — structure, CRC, opcode whitelist and
 *          per-opcode numeric parameter range checks.
 *
 * Standards: NASA Power of Ten #1 (bounded, overflow-free arithmetic; no
 * unchecked indexing), NASA-STD-8739.8 (command validation/authentication —
 * a malformed or out-of-range telecommand is rejected, never dispatched).
 */

#include "comms_validate.h"

/* ---------- Per-opcode validation table ---------- */

/**
 * One row per accepted telecommand. Opcodes absent from this table are
 * rejected (whitelist, not blacklist).
 *
 * @c has_param selects a single big-endian uint32 parameter located at
 * @c param_off inside the payload; it is range-checked against
 * [@c param_min, @c param_max], with @c allow_zero permitting the reserved
 * "0 = use default" escape value.
 */
typedef struct {
    uint8_t  opcode;
    uint8_t  min_payload;
    uint8_t  max_payload;
    bool     has_param;    /**< payload carries a big-endian uint32 parameter */
    uint8_t  param_off;    /**< byte offset of that parameter in the payload  */
    bool     allow_zero;   /**< accept 0 in addition to [min, max]            */
    uint32_t param_min;
    uint32_t param_max;
} tc_spec_t;

static const tc_spec_t tc_table[] = {
    /* opcode                        min  max  param  off  zero  pmin                    pmax                  */
    { COMMS_TC_RESET,                  0,   0, false,   0, false, 0,                      0                     },
    { COMMS_TC_EXIT_STATE,             0,   0, false,   0, false, 0,                      0                     },
    { COMMS_TC_SET_CONFIG,             1,  32, false,   0, false, 0,                      0                     },
    { COMMS_TC_SEND_DATA,              0,   8, false,   0, false, 0,                      0                     },
    { COMMS_TC_ACTIVATE_PAYLOAD,       0,   0, false,   0, false, 0,                      0                     },
    { COMMS_TC_SET_BEACON_INTERVAL,    4,   4,  true,   0,  true, COMMS_TC_BEACON_MIN_MS, COMMS_TC_BEACON_MAX_MS },
};

#define TC_TABLE_LEN  (sizeof(tc_table) / sizeof(tc_table[0]))

/* Compile-time sanity: the frame budget must fit the LoRa PHY payload. */
_Static_assert(COMMS_TC_MAX_PAYLOAD + COMMS_TC_OVERHEAD == COMMS_TC_MAX_FRAME,
               "TC payload budget inconsistent with max frame size");
_Static_assert(COMMS_TC_MAX_PAYLOAD <= 255U,
               "payload length field is one byte");

/* ---------- Private helpers ---------- */

static const tc_spec_t *tc_lookup(uint8_t opcode)
{
    for (size_t i = 0U; i < TC_TABLE_LEN; i++) {   /* bounded loop, NASA-PoT #2 */
        if (tc_table[i].opcode == opcode) {
            return &tc_table[i];
        }
    }
    return NULL;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |
            (uint32_t)p[3];
}

/* ---------- Public API ---------- */

uint16_t comms_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    if (data == NULL) {
        return crc;
    }
    if (len > COMMS_TC_MAX_FRAME) {   /* hard upper bound on the loop */
        len = COMMS_TC_MAX_FRAME;
    }

    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

comms_tc_result_t comms_validate_tc(const uint8_t   *frame,
                                    size_t           len,
                                    uint8_t         *out_opcode,
                                    const uint8_t  **out_payload,
                                    size_t          *out_len)
{
    if ((frame == NULL) || (out_opcode == NULL) ||
        (out_payload == NULL) || (out_len == NULL)) {
        return COMMS_TC_ERR_NULL;
    }

    /* (c) reject malformed / oversized frames before touching their content */
    if (len < (size_t)COMMS_TC_MIN_FRAME) {
        return COMMS_TC_ERR_TOO_SHORT;
    }
    if (len > (size_t)COMMS_TC_MAX_FRAME) {
        return COMMS_TC_ERR_TOO_LONG;
    }

    const uint8_t opcode      = frame[0];
    const size_t  payload_len = (size_t)frame[1];

    /* Declared length must match the received length exactly. Both operands are
     * already bounded (<= 64 and <= 255), so the addition cannot overflow. */
    if ((payload_len + (size_t)COMMS_TC_OVERHEAD) != len) {
        return COMMS_TC_ERR_LEN_MISMATCH;
    }
    if (payload_len > (size_t)COMMS_TC_MAX_PAYLOAD) {
        return COMMS_TC_ERR_TOO_LONG;
    }

    /* (b) integrity: CRC-16/CCITT over header + payload, big-endian trailer */
    const size_t   crc_off = COMMS_TC_HDR_LEN + payload_len;
    const uint16_t rx_crc  = (uint16_t)(((uint16_t)frame[crc_off] << 8) |
                                         (uint16_t)frame[crc_off + 1U]);
    if (comms_crc16_ccitt(frame, crc_off) != rx_crc) {
        return COMMS_TC_ERR_CRC;
    }

    /* Opcode whitelist */
    const tc_spec_t *spec = tc_lookup(opcode);
    if (spec == NULL) {
        return COMMS_TC_ERR_OPCODE;
    }

    /* Per-opcode payload size contract */
    if ((payload_len < (size_t)spec->min_payload) ||
        (payload_len > (size_t)spec->max_payload)) {
        return COMMS_TC_ERR_PAYLOAD_LEN;
    }

    /* (a) numeric parameter range check */
    if (spec->has_param) {
        const size_t need = (size_t)spec->param_off + 4U;
        if (need > payload_len) {
            return COMMS_TC_ERR_PAYLOAD_LEN;
        }
        const uint32_t value = be32(&frame[COMMS_TC_HDR_LEN + spec->param_off]);
        const bool     zero_ok = spec->allow_zero && (value == 0UL);
        if (!zero_ok &&
            ((value < spec->param_min) || (value > spec->param_max))) {
            return COMMS_TC_ERR_PARAM_RANGE;
        }
    }

    *out_opcode  = opcode;
    *out_payload = (payload_len > 0U) ? &frame[COMMS_TC_HDR_LEN] : NULL;
    *out_len     = payload_len;
    return COMMS_TC_OK;
}

const char *comms_tc_result_str(comms_tc_result_t result)
{
    switch (result) {
    case COMMS_TC_OK:               return "OK";
    case COMMS_TC_ERR_NULL:         return "NULL";
    case COMMS_TC_ERR_TOO_SHORT:    return "TOO_SHORT";
    case COMMS_TC_ERR_TOO_LONG:     return "TOO_LONG";
    case COMMS_TC_ERR_LEN_MISMATCH: return "LEN_MISMATCH";
    case COMMS_TC_ERR_CRC:          return "CRC";
    case COMMS_TC_ERR_OPCODE:       return "BAD_OPCODE";
    case COMMS_TC_ERR_PAYLOAD_LEN:  return "BAD_PAYLOAD_LEN";
    case COMMS_TC_ERR_PARAM_RANGE:  return "PARAM_RANGE";
    default:                        return "UNKNOWN";
    }
}

/* ---------- Counters ---------- */

static comms_rx_stats_t rx_stats;

void comms_rx_account(comms_tc_result_t result)
{
    switch (result) {
    case COMMS_TC_OK:
        rx_stats.accepted++;
        return;
    case COMMS_TC_ERR_CRC:
        rx_stats.rejected_crc++;
        break;
    case COMMS_TC_ERR_OPCODE:
        rx_stats.rejected_opcode++;
        break;
    case COMMS_TC_ERR_PAYLOAD_LEN:
    case COMMS_TC_ERR_PARAM_RANGE:
        rx_stats.rejected_range++;
        break;
    default:
        rx_stats.rejected_malformed++;
        break;
    }
    rx_stats.rejected++;
}

void comms_rx_get_stats(comms_rx_stats_t *out)
{
    if (out != NULL) {
        *out = rx_stats;
    }
}
