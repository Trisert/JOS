/**
 * @file    comms_validate.c
 * @brief   Uplink telecommand validation — structure, CRC, opcode whitelist and
 *          per-opcode numeric parameter range checks.
 *
 * Scope: STRUCTURAL validation only. The CRC-16/CCITT-FALSE trailer is unkeyed,
 * so it detects corrupted/malformed frames but does NOT authenticate the sender
 * and offers no replay protection — any transmitter that knows the frame format
 * can produce a CRC-valid telecommand. Authentication would require a keyed MAC
 * plus a monotonic counter / rolling code.
 *
 * Standards: NASA Power of Ten #1 (bounded, overflow-free arithmetic; no
 * unchecked indexing), NASA-STD-8739.8 (command validation — a malformed or
 * out-of-range telecommand is rejected, never dispatched).
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

/* Nibble-wise CRC-16/CCITT-FALSE table (16 entries, 32 B of .rodata) — the
 * MSB-first twin of the reflected crc32_nibble_table in App/obsw/boot_crc.c.
 * table[n] = 4 CRC iterations applied to (n << 12) with poly 0x1021, so two
 * table lookups per byte replace the 8-iteration inner bit loop while staying
 * bit-for-bit identical to the original SHIFT-AND implementation above. */
static const uint16_t crc16_nibble_table[16] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u,
    0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu,
    0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
};

uint16_t comms_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    /* Defensive contract (documented in the header): a NULL buffer yields the
     * CRC init value 0xFFFF and an over-long request is clamped to the frame
     * budget, so the loop bound is always finite (NASA-PoT #2). Callers inside
     * this module never rely on either path — comms_validate_tc() bounds `len`
     * to COMMS_TC_MAX_FRAME before calling. */
    if (data == NULL) {
        return crc;
    }
    if (len > (size_t)COMMS_TC_MAX_FRAME) {   /* hard upper bound on the loop */
        len = (size_t)COMMS_TC_MAX_FRAME;
    }

    /* MSB-first nibble table (App/obsw/boot_crc.c analogue). Two table
     * lookups per byte replace the 8-iteration inner bit loop and stay
     * bit-for-bit identical to the original SHIFT-AND implementation (poly
     * 0x1021, init 0xFFFF, no final XOR). table[n] is 4 CRC iterations of
     * (n << 12), so each (crc << 4) ^ table[top_nibble] folds in one nibble. */
    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        crc = (uint16_t)((uint16_t)(crc << 4) ^ crc16_nibble_table[(crc >> 12) & 0x000FU]);
        crc = (uint16_t)((uint16_t)(crc << 4) ^ crc16_nibble_table[(crc >> 12) & 0x000FU]);
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

    /* Declared payload must fit the frame budget. Checked BEFORE the
     * length-match test so this branch is reachable (MISRA C:2025 Rule 2.1 —
     * no dead code): e.g. a 4-byte frame declaring 200 payload bytes lands
     * here rather than in the mismatch case. */
    if (payload_len > (size_t)COMMS_TC_MAX_PAYLOAD) {
        return COMMS_TC_ERR_TOO_LONG;
    }

    /* Declared length must match the received length exactly. Both operands are
     * already bounded (<= 60 and <= 64), so the addition cannot overflow. */
    if ((payload_len + (size_t)COMMS_TC_OVERHEAD) != len) {
        return COMMS_TC_ERR_LEN_MISMATCH;
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

/*
 * Single-writer counters: comms_rx_account() is called only from the LoRa RX
 * path (one task / one simulation bridge task), so the read-modify-write
 * increments never race with each other. Readers (telemetry) take a struct
 * snapshot via comms_rx_get_stats(); on Cortex-M4 that copy is not atomic, so a
 * snapshot taken while the RX task is mid-update may be one increment stale in
 * one field. That is acceptable for diagnostics counters; if these ever become
 * flight-critical they must be moved behind a critical section or made
 * per-field atomics.
 */
static volatile comms_rx_stats_t rx_stats;

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
        out->accepted           = rx_stats.accepted;
        out->rejected           = rx_stats.rejected;
        out->rejected_crc       = rx_stats.rejected_crc;
        out->rejected_malformed = rx_stats.rejected_malformed;
        out->rejected_opcode    = rx_stats.rejected_opcode;
        out->rejected_range     = rx_stats.rejected_range;
    }
}
