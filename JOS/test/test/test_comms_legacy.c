/**
 * @file    test_comms_legacy.c
 * @brief   Relaxed-mode RX gate (COMMS_AUTH_ENFORCE=0, set per-test in
 *          project.yml): legacy CRC-only frames are still dispatched while
 *          sealed frames keep verifying. Bench/migration coverage for the
 *          #else branch the enforced suite (test_comms.c) never executes.
 *
 * This binary compiles App/comms with COMMS_AUTH_ENFORCE=0; every other
 * test binary uses the flight default (1). The two modes never mix in one
 * executable.
 */

#include "unity.h"
#include "comms.h"
#include "comms_validate.h"
#include "tec.h"                    /* comms.c's TT&C path calls tec_dispatch() */
#include "sha256.h"                /* sealed-frame builder uses the tag API   */
#include "mock_state_machine.h"
#include "mock_watchdog.h"     /* link seam only (no expectations queued)     */
#include "mock_cmsis_os.h"     /* link seam only (no expectations queued)     */

#include <string.h>

/* ---------- Frame builders (mirrors test_comms.c) ---------- */

static uint8_t frame_buf[COMMS_TC_MAX_FRAME + 8];

/* Legacy "opcode | len | payload | CRC16-BE". */
static size_t build_frame(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
    frame_buf[0] = opcode;
    frame_buf[1] = len;
    if ((payload != NULL) && (len > 0U)) {
        memcpy(&frame_buf[COMMS_TC_HDR_LEN], payload, len);
    }
    const size_t   crc_off = (size_t)COMMS_TC_HDR_LEN + len;
    const uint16_t crc     = comms_crc16_ccitt(frame_buf, crc_off);
    frame_buf[crc_off]     = (uint8_t)(crc >> 8);
    frame_buf[crc_off + 1] = (uint8_t)(crc & 0xFFU);
    return crc_off + COMMS_TC_CRC_LEN;
}

/* Authenticated "opcode | len | payload | TAG32-BE | CRC16-BE". */
static size_t build_auth_frame(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
    frame_buf[0] = opcode;
    frame_buf[1] = len;
    if ((payload != NULL) && (len > 0U)) {
        memcpy(&frame_buf[COMMS_TC_HDR_LEN], payload, len);
    }
    const size_t tag_off = (size_t)COMMS_TC_HDR_LEN + len;
    uint32_t     tag     = 0U;

    TEST_ASSERT_TRUE(comms_auth_tag(frame_buf, tag_off, &tag));
    frame_buf[tag_off]     = (uint8_t)(tag >> 24);
    frame_buf[tag_off + 1] = (uint8_t)(tag >> 16);
    frame_buf[tag_off + 2] = (uint8_t)(tag >> 8);
    frame_buf[tag_off + 3] = (uint8_t)tag;
    const size_t   crc_off = tag_off + (size_t)COMMS_TC_MAC_LEN;
    const uint16_t crc     = comms_crc16_ccitt(frame_buf, crc_off);
    frame_buf[crc_off]     = (uint8_t)(crc >> 8);
    frame_buf[crc_off + 1] = (uint8_t)(crc & 0xFFU);
    return crc_off + COMMS_TC_CRC_LEN;
}

void setUp(void)   { memset(frame_buf, 0, sizeof(frame_buf)); }
void tearDown(void) { }

/* A well-formed legacy frame dispatches when enforcement is relaxed. */
void test_relaxed_gate_dispatches_legacy_frame(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* A sealed frame still verifies and dispatches when relaxed. */
void test_relaxed_gate_dispatches_sealed_frame(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    size_t n = build_auth_frame(COMMS_TC_ACTIVATE_PAYLOAD, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* A bad tag is rejected even when relaxed — relaxing enforcement waives the
 * tag requirement for legacy layouts only, never for sealed ones. No mock
 * expectations queued: dispatch must not happen. */
void test_relaxed_gate_still_rejects_bad_tag(void)
{
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[2] ^= 0x01U;
    const uint16_t crc = comms_crc16_ccitt(frame_buf, n - (size_t)COMMS_TC_CRC_LEN);
    frame_buf[n - 2]   = (uint8_t)(crc >> 8);
    frame_buf[n - 1]   = (uint8_t)(crc & 0xFFU);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));
}

/* Structural verdicts are unchanged when relaxed. */
void test_relaxed_gate_keeps_structural_verdicts(void)
{
    size_t n = build_frame(0xEEU, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, comms_rx_handle_frame(frame_buf, n));

    n = build_frame(COMMS_TC_RESET, NULL, 0U);
    frame_buf[n - 1] ^= 0x55U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, comms_rx_handle_frame(frame_buf, n));
}
