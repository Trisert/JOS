/**
 * @file    test_comms.c
 * @brief   Unit tests for the uplink telecommand validation gate
 *          (App/comms/comms_validate.c + comms_rx_handle_frame() in
 *          App/comms/comms.c) and for the LoRa RX/beacon task wiring.
 *
 * The SX1268 LoRa layer is stubbed at its two seams:
 *   - frames are injected directly at comms_rx_handle_frame(), which is the
 *     single entry point the RX ISR/task is required to use;
 *   - the SPI handle is an address-only placeholder (support/stubs.c): no
 *     radio and no bus transaction ever happen.
 * The state machine, the watchdog and the RTOS are CMock mocks, so "was this
 * frame dispatched?" is an assertion, not an inference.
 *
 * NVIC_SystemReset() is NOT mocked. It is defined in support/hal_stubs.c,
 * which Ceedling links into every test executable, so a generated mock_main.c
 * would be a duplicate definition. The support double is the stronger seam
 * anyway: unless a test explicitly arms HOST_EXPECT_NVIC_RESET() a reboot
 * request fails the run, and when armed it long-jumps out - reproducing
 * "NVIC_SystemReset() does not return" exactly as on target, which a CMock
 * expectation cannot do.
 *
 * What is verified
 *   - CRC-16/CCITT-FALSE known-answer vector.
 *   - every rejection class (NULL, too short, too long, declared-length
 *     mismatch, CRC error, unknown opcode, illegal payload length,
 *     out-of-range parameter) is rejected AND never dispatched.
 *   - well-formed frames for every whitelisted opcode are accepted and reach
 *     exactly the expected action.
 *   - RX accounting counters classify each verdict correctly.
 *   - lora_rx_task_create()/lora_beacon_task_create() register with the
 *     watchdog monitor, and their loops kick it on every iteration.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit testing), ECSS-E-ST-70-41 (TC
 * acceptance), NASA-STD-8739.8 (command validation before execution),
 * NASA Power of Ten #1/#5 (bounded, fully checked inputs), JPL-182 Rule 16.
 */

#include "unity.h"
#include "comms.h"
#include "comms_validate.h"
#include "sha256.h"                /* wrong-key forgery test only           */
#include "host_support.h"      /* HOST_EXPECT_NVIC_RESET()                    */
#include "mock_state_machine.h"
#include "mock_watchdog.h"     /* also pulls watchdog.h for the WDG_PERIOD_*  */
#include "mock_cmsis_os.h"

#include <string.h>
#include <setjmp.h>
#include <unistd.h>             /* alarm(), STDERR_FILENO, _exit()           */
#include <signal.h>             /* SIGALRM hang ceiling for task-loop tests  */

/* TX call log in support/radiolib_stubs.c (host only): lets the framing tests
   assert the exact on-air chunks (lengths, seq/total headers, reassembly). */
extern size_t          radiolib_stub_tx_count(void);
extern size_t          radiolib_stub_tx_len(size_t i);
extern const uint8_t  *radiolib_stub_tx_data(size_t i);
extern void            radiolib_stub_tx_reset(void);

/* ---------- Frame builder ---------- */

static uint8_t frame_buf[COMMS_TC_MAX_FRAME + 8];

/* Build "opcode | len | payload | CRC16-BE". Returns the frame length. */
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

/* Build authenticated "opcode | len | payload | TAG32-BE | CRC16-BE" sealed
 * with the flight key. Returns the frame length. Misuse (payload beyond the
 * auth budget) fails the test loudly instead of staging a runt frame. */
static size_t build_auth_frame(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT8((uint8_t)COMMS_TC_MAX_AUTH_PAYLOAD, len);
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

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Validate a frame without caring about the out-parameters. */
static comms_tc_result_t validate(const uint8_t *f, size_t len)
{
    uint8_t        opcode  = 0xFFU;
    const uint8_t *payload = (const uint8_t *)1;   /* poison */
    size_t         plen    = 0xDEADU;
    return comms_validate_tc(f, len, &opcode, &payload, &plen);
}

/* Validate an authenticated frame without caring about the out-parameters. */
static comms_tc_result_t validate_auth(const uint8_t *f, size_t len)
{
    uint8_t        opcode  = 0xFFU;
    const uint8_t *payload = (const uint8_t *)1;   /* poison */
    size_t         plen    = 0xDEADU;
    return comms_validate_tc_auth(f, len, &opcode, &payload, &plen);
}

/* host_lora_reset() resets the lora failure-INJECTION state (call counters +
 * armed one-shot failures), not the radio model itself: a leftover armed
 * failure must never leak into the next test. */
void setUp(void)   { memset(frame_buf, 0, sizeof(frame_buf)); host_lora_reset(); }
/* Safety net: the hang ceiling must NEVER survive past its own test case.
   The early-return path inside run_task_until_escape() longjmps into Unity
   before alarm(0) runs, so tearDown() disarms unconditionally — otherwise a
   still-ticking timer could kill an innocent later test with a misleading
   "escape stub never fired" diagnostic. */
void tearDown(void) { alarm(0); }

/* ================= CRC known-answer test ================= */

/* CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
void test_comms_crc16_known_check_string(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1U, comms_crc16_ccitt((const uint8_t *)"123456789", 9U));
}

/* Second known-answer vector over a multi-byte buffer. The expected value
 * 0xB19E was computed from the ORIGINAL bit-per-bit implementation (before the
 * nibble-table rewrite) and pins parity: the tabular version must produce the
 * identical result, not merely a valid CRC. */
void test_comms_crc16_known_multibyte_buffer(void)
{
    static const uint8_t buf[16] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0xAAU, 0xBBU, 0xCCU, 0xDDU,
        0x10U, 0x20U, 0x30U, 0x40U, 0xFFU, 0x7EU, 0x81U, 0x42U
    };
    TEST_ASSERT_EQUAL_HEX16(0xB19EU, comms_crc16_ccitt(buf, 16U));
}

void test_comms_crc16_null_returns_init_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, comms_crc16_ccitt(NULL, 4U));
}

/* ================= structural rejection ================= */

void test_validate_rejects_null_frame(void)
{
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL, validate(NULL, 8U));
}

void test_validate_rejects_null_out_pointers(void)
{
    uint8_t        opcode  = 0U;
    const uint8_t *payload = NULL;
    size_t         plen    = 0U;
    size_t         n       = build_frame(COMMS_TC_RESET, NULL, 0U);

    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc(frame_buf, n, NULL, &payload, &plen));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc(frame_buf, n, &opcode, NULL, &plen));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc(frame_buf, n, &opcode, &payload, NULL));
}

void test_validate_rejects_runt_frames(void)
{
    (void)build_frame(COMMS_TC_RESET, NULL, 0U);
    for (size_t len = 0U; len < COMMS_TC_MIN_FRAME; len++) {
        TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_SHORT, validate(frame_buf, len));
    }
}

void test_validate_rejects_oversized_frame(void)
{
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate(frame_buf, (size_t)COMMS_TC_MAX_FRAME + 1U));
}

/* Declared payload length must match the number of bytes actually received —
   the classic truncation / injection vector. */
void test_validate_rejects_length_mismatch(void)
{
    size_t n = build_frame(COMMS_TC_SET_CONFIG, (const uint8_t *)"\x01\x02\x03\x04", 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, validate(frame_buf, n));

    frame_buf[1] = 3U;                       /* lie about the payload size */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH, validate(frame_buf, n));

    frame_buf[1] = 5U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH, validate(frame_buf, n));
}

/* A declared payload larger than the frame budget is TOO_LONG, not a length
   mismatch: comms_validate_tc() checks the declared size against
   COMMS_TC_MAX_PAYLOAD *before* comparing it with the received length, so the
   two rejection classes stay distinguishable in telemetry and neither branch
   is unreachable (MISRA C:2025 Rule 2.1). This test pins that ordering — the
   frame below would otherwise also satisfy the mismatch test. */
void test_validate_rejects_declared_payload_beyond_budget(void)
{
    uint8_t big[COMMS_TC_MAX_FRAME];
    memset(big, 0xA5, sizeof(big));
    (void)build_frame(COMMS_TC_SET_CONFIG, big, (uint8_t)COMMS_TC_MAX_PAYLOAD);

    /* Header claims 250 B of payload - far beyond the 60 B budget. */
    frame_buf[1] = 250U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate(frame_buf, (size_t)COMMS_TC_MAX_FRAME));

    /* Guard band: one byte over the budget is still TOO_LONG ... */
    frame_buf[1] = (uint8_t)(COMMS_TC_MAX_PAYLOAD + 1U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate(frame_buf, (size_t)COMMS_TC_MAX_FRAME));

    /* ... and exactly at the budget it becomes a plain length mismatch,
       because the received frame is one byte shorter than declared. */
    frame_buf[1] = (uint8_t)COMMS_TC_MAX_PAYLOAD;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH,
                          validate(frame_buf, (size_t)COMMS_TC_MAX_FRAME - 1U));
}

/* ================= integrity ================= */

void test_validate_rejects_corrupted_crc(void)
{
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[n - 1] ^= 0xFFU;               /* single-bit-plane corruption */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, validate(frame_buf, n));
}

void test_validate_rejects_corrupted_payload_byte(void)
{
    uint8_t payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t  n = build_frame(COMMS_TC_SET_CONFIG, payload, 4U);
    frame_buf[3] ^= 0x01U;                   /* flip one payload bit */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, validate(frame_buf, n));
}

/* ================= whitelist and ranges ================= */

void test_validate_rejects_unknown_opcode(void)
{
    size_t n = build_frame(0x7FU, NULL, 0U);   /* not in the TC table */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, validate(frame_buf, n));
}

void test_validate_rejects_illegal_payload_length_for_opcode(void)
{
    /* RESET takes no payload. */
    uint8_t junk[2] = { 0x00, 0x01 };
    size_t  n = build_frame(COMMS_TC_RESET, junk, 2U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, validate(frame_buf, n));

    /* SET_CONFIG needs at least one byte. */
    n = build_frame(COMMS_TC_SET_CONFIG, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, validate(frame_buf, n));
}

void test_validate_rejects_out_of_range_beacon_interval(void)
{
    uint8_t p[4];

    put_be32(p, COMMS_TC_BEACON_MIN_MS - 1UL);          /* 999 ms — too fast */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, COMMS_TC_BEACON_MAX_MS + 1UL);          /* > 1 h — too slow */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, 0xFFFFFFFFUL);                          /* saturated */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
}

void test_validate_accepts_beacon_interval_bounds_and_zero_escape(void)
{
    uint8_t p[4];

    put_be32(p, COMMS_TC_BEACON_MIN_MS);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, COMMS_TC_BEACON_MAX_MS);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, 0UL);                       /* reserved "use per-state default" */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
}

/* On acceptance the out-parameters describe the payload slice exactly. */
void test_validate_reports_payload_slice_on_accept(void)
{
    uint8_t        payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t        opcode     = 0U;
    const uint8_t *out        = NULL;
    size_t         out_len    = 0U;

    size_t n = build_frame(COMMS_TC_SEND_DATA, payload, 8U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          comms_validate_tc(frame_buf, n, &opcode, &out, &out_len));
    TEST_ASSERT_EQUAL_HEX8(COMMS_TC_SEND_DATA, opcode);
    TEST_ASSERT_EQUAL_size_t(8U, out_len);
    TEST_ASSERT_EQUAL_PTR(&frame_buf[COMMS_TC_HDR_LEN], out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, 8);
}

void test_validate_reports_null_payload_for_empty_command(void)
{
    uint8_t        opcode  = 0U;
    const uint8_t *out     = (const uint8_t *)1;
    size_t         out_len = 99U;

    size_t n = build_frame(COMMS_TC_ACTIVATE_PAYLOAD, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          comms_validate_tc(frame_buf, n, &opcode, &out, &out_len));
    TEST_ASSERT_EQUAL_HEX8(COMMS_TC_ACTIVATE_PAYLOAD, opcode);
    TEST_ASSERT_EQUAL_size_t(0U, out_len);
    TEST_ASSERT_NULL(out);
}

void test_result_strings_are_never_null(void)
{
    for (int r = COMMS_TC_OK; r <= COMMS_TC_ERR_MAC; r++) {
        TEST_ASSERT_NOT_NULL(comms_tc_result_str((comms_tc_result_t)r));
    }
    TEST_ASSERT_EQUAL_STRING("MAC", comms_tc_result_str(COMMS_TC_ERR_MAC));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", comms_tc_result_str((comms_tc_result_t)999));
}

/* ================= RX gate: reject must never dispatch ================= */

/* No state_machine_* / NVIC_SystemReset expectations are queued in these
   tests, so CMock fails if a rejected frame reaches the dispatcher. */

void test_rx_gate_drops_malformed_frames_without_dispatching(void)
{
    uint8_t p[4];

    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,      comms_rx_handle_frame(NULL, 8U));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_SHORT, comms_rx_handle_frame(frame_buf, 3U));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          comms_rx_handle_frame(frame_buf, (size_t)COMMS_TC_MAX_FRAME + 1U));

    /* CRC error on an otherwise perfectly sealed RESET — must not reboot. */
    size_t n = build_auth_frame(COMMS_TC_RESET, NULL, 0U);
    frame_buf[n - 1] ^= 0x55U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, comms_rx_handle_frame(frame_buf, n));

    /* Bad tag on a CRC-valid RESET — must not reboot either. */
    n = build_auth_frame(COMMS_TC_RESET, NULL, 0U);
    frame_buf[2] ^= 0x01U;                       /* flip one tag bit ... */
    /* ... and repair the CRC over the tampered bytes so only the tag fails. */
    {
        const uint16_t crc = comms_crc16_ccitt(frame_buf, n - 2U);
        frame_buf[n - 2]   = (uint8_t)(crc >> 8);
        frame_buf[n - 1]   = (uint8_t)(crc & 0xFFU);
    }
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));

    /* Unknown opcode with a VALID tag (proves the tag covers any byte). */
    n = build_auth_frame(0xEEU, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, comms_rx_handle_frame(frame_buf, n));

    /* Out-of-range beacon interval, sealed. */
    put_be32(p, 10UL);
    n = build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE, comms_rx_handle_frame(frame_buf, n));
}

/* Enforcement (COMMS_AUTH_ENFORCE=1, the flight default compiled into this
 * binary): a structurally perfect legacy CRC-only frame carries no tag and
 * must be rejected with COMMS_TC_ERR_MAC, never dispatched. No
 * state_machine_* / NVIC_SystemReset expectations are queued, so CMock fails
 * if the frame reaches the dispatcher. */
void test_rx_gate_rejects_untagged_legacy_frame_when_enforced(void)
{
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, validate(frame_buf, n));  /* well formed */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));

    n = build_frame(COMMS_TC_RESET, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));
}

/* Malformed legacy frames keep their structural verdict under enforcement —
 * the OK->MAC mapping applies only to frames that parsed cleanly. */
void test_rx_gate_keeps_structural_verdict_for_malformed_legacy(void)
{
    size_t n = build_frame(COMMS_TC_RESET, NULL, 0U);
    frame_buf[n - 1] ^= 0x55U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, comms_rx_handle_frame(frame_buf, n));

    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_SHORT,
                          comms_rx_handle_frame(frame_buf, 2U));
}

/* ================= RX gate: accept must dispatch exactly once ============= */

void test_rx_gate_dispatches_exit_state(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_activate_payload(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    size_t n = build_auth_frame(COMMS_TC_ACTIVATE_PAYLOAD, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* A valid RESET telecommand must actually reboot the OBC. The frame is
   accounted as accepted *before* the dispatcher runs, so the reset counter and
   the RX statistics are both checked: the long jump out of NVIC_SystemReset()
   means comms_rx_handle_frame() never returns, exactly as on target. */
void test_rx_gate_dispatches_reset(void)
{
    comms_rx_stats_t before, after;
    comms_rx_get_stats(&before);

    const uint32_t resets_before = host_nvic_reset_count();
    size_t         n             = build_auth_frame(COMMS_TC_RESET, NULL, 0U);

    HOST_EXPECT_NVIC_RESET(comms_rx_handle_frame(frame_buf, n));

    TEST_ASSERT_EQUAL_UINT32(resets_before + 1U, host_nvic_reset_count());
    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U, after.accepted);
}

void test_rx_gate_dispatches_in_range_beacon_interval(void)
{
    uint8_t p[4];
    put_be32(p, 60000UL);                    /* 60 s — inside [1 s, 1 h] */
    state_machine_set_beacon_interval_ExpectAndReturn(60000UL, 0);
    size_t n = build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_zero_beacon_interval_escape(void)
{
    uint8_t p[4];
    put_be32(p, 0UL);
    state_machine_set_beacon_interval_ExpectAndReturn(0UL, 0);
    size_t n = build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* ================= RX accounting ================= */

void test_rx_stats_classify_each_verdict(void)
{
    comms_rx_stats_t before, after;
    comms_rx_get_stats(&before);

    /* 1 accepted (sealed EXIT_STATE) */
    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    (void)comms_rx_handle_frame(frame_buf, build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U));

    /* 1 CRC rejection (sealed frame, corrupted trailer) */
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[n - 1] ^= 0x01U;
    (void)comms_rx_handle_frame(frame_buf, n);

    /* 1 MAC rejection (sealed frame, corrupted tag, repaired CRC) */
    n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[3] ^= 0x01U;
    {
        const uint16_t crc = comms_crc16_ccitt(frame_buf, n - 2U);
        frame_buf[n - 2]   = (uint8_t)(crc >> 8);
        frame_buf[n - 1]   = (uint8_t)(crc & 0xFFU);
    }
    (void)comms_rx_handle_frame(frame_buf, n);

    /* 1 opcode rejection (sealed frame, unknown opcode) */
    (void)comms_rx_handle_frame(frame_buf, build_auth_frame(0xABU, NULL, 0U));

    /* 1 range rejection (sealed frame, out-of-range parameter) */
    uint8_t p[4];
    put_be32(p, 1UL);
    (void)comms_rx_handle_frame(frame_buf, build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U));

    /* 1 untagged-legacy rejection (well formed, no tag — enforced) */
    (void)comms_rx_handle_frame(frame_buf, build_frame(COMMS_TC_EXIT_STATE, NULL, 0U));

    /* 2 malformed rejections */
    (void)comms_rx_handle_frame(NULL, 8U);
    (void)comms_rx_handle_frame(frame_buf, 2U);

    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U,            after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 7U,            after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_crc + 1U,        after.rejected_crc);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_mac + 2U,        after.rejected_mac);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_opcode + 1U,     after.rejected_opcode);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_range + 1U,      after.rejected_range);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_malformed + 2U,  after.rejected_malformed);
}

void test_rx_stats_tolerates_null_out(void)
{
    comms_rx_get_stats(NULL);   /* must not fault */
}

/* ================= uplink authentication (HMAC-SHA256) ================= */

/* Known-answer vectors for comms_auth_tag(), computed independently with
 * Python hashlib (hmac.new(key, data, sha256)) — NOT with the C code under
 * test. Key = A1 B2 C3 D4 (upstream RedPill SECRET_KEY). */
static uint32_t auth_tag_or_fail(const uint8_t *data, size_t len)
{
    uint32_t tag = 0U;

    TEST_ASSERT_TRUE(comms_auth_tag(data, len, &tag));
    return tag;
}

void test_auth_tag_known_answer_vectors(void)
{
    static const uint8_t exit_state[2] = { 0x02U, 0x00U };
    static const uint8_t reset[2]      = { 0x01U, 0x00U };
    static const uint8_t beacon[6]     = { 0x06U, 0x04U, 0x00U, 0x00U, 0xEAU, 0x60U };

    TEST_ASSERT_EQUAL_HEX32(0xD7D5199CU, auth_tag_or_fail(exit_state, sizeof(exit_state)));
    TEST_ASSERT_EQUAL_HEX32(0x2413C884U, auth_tag_or_fail(reset, sizeof(reset)));
    TEST_ASSERT_EQUAL_HEX32(0x9FB4A7A7U, auth_tag_or_fail(beacon, sizeof(beacon)));
}

/* Multi-block tag input: a 58-byte header+payload slice forces the inner
 * SHA-256 through two compression blocks (64-byte ipad + 58 bytes of data).
 * Expected value from Python hmac.new(key, data, sha256) — first 4 bytes. */
void test_auth_tag_multiblock_known_answer(void)
{
    uint8_t slice[58];
    size_t i;

    slice[0] = 0x03U;
    slice[1] = 0x20U;
    for (i = 2U; i < sizeof(slice); i++) {
        slice[i] = (uint8_t)(i - 2U);
    }
    TEST_ASSERT_EQUAL_HEX32(0xD43AF134U, auth_tag_or_fail(slice, sizeof(slice)));
}

/* FIPS 180-4 multi-block SHA-256 vectors, computed with Python hashlib
 * (independent of the C code). The 56-byte message spans two blocks after
 * padding; the 112-byte message spans two blocks before padding. Both
 * exercise the 64-bit bit-length accumulation across compression blocks. */
void test_sha256_multiblock_known_answer(void)
{
    static const char msg56[]  = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char msg112[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                                 "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const uint8_t exp56[32] = {
        0x24U, 0x8DU, 0x6AU, 0x61U, 0xD2U, 0x06U, 0x38U, 0xB8U,
        0xE5U, 0xC0U, 0x26U, 0x93U, 0x0CU, 0x3EU, 0x60U, 0x39U,
        0xA3U, 0x3CU, 0xE4U, 0x59U, 0x64U, 0xFFU, 0x21U, 0x67U,
        0xF6U, 0xECU, 0xEDU, 0xD4U, 0x19U, 0xDBU, 0x06U, 0xC1U
    };
    static const uint8_t exp112[32] = {
        0xCFU, 0x5BU, 0x16U, 0xA7U, 0x78U, 0xAFU, 0x83U, 0x80U,
        0x03U, 0x6CU, 0xE5U, 0x9EU, 0x7BU, 0x04U, 0x92U, 0x37U,
        0x0BU, 0x24U, 0x9BU, 0x11U, 0xE8U, 0xF0U, 0x7AU, 0x51U,
        0xAFU, 0xACU, 0x45U, 0x03U, 0x7AU, 0xFEU, 0xE9U, 0xD1U
    };
    SHA256_CTX ctx;
    uint8_t digest[32];

    TEST_ASSERT_EQUAL_size_t(56U, sizeof(msg56) - 1U);
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)msg56, sizeof(msg56) - 1U);
    sha256_final(&ctx, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp56, digest, sizeof(digest));

    TEST_ASSERT_EQUAL_size_t(112U, sizeof(msg112) - 1U);
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)msg112, sizeof(msg112) - 1U);
    sha256_final(&ctx, digest);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp112, digest, sizeof(digest));
}

/* Every 32-bit value is a legitimate tag, so NULL must be rejected
 * explicitly — never mapped to a return value that collides with a real
 * tag. The out-parameter is left untouched on rejection. */
void test_auth_tag_null_is_rejected(void)
{
    static const uint8_t sample[2] = { 0x02U, 0x00U };
    uint32_t tag = 0xA5A5A5A5U;

    TEST_ASSERT_FALSE(comms_auth_tag(NULL, sizeof(sample), &tag));
    TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5U, tag);

    TEST_ASSERT_FALSE(comms_auth_tag(sample, sizeof(sample), NULL));

    TEST_ASSERT_FALSE(comms_auth_tag(NULL, 0U, &tag));
    TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5U, tag);
}

void test_auth_layout_discriminator(void)
{
    size_t n;

    n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_TRUE(comms_frame_is_auth_layout(frame_buf, n));

    n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(frame_buf, n));

    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(NULL, 8U));
    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(frame_buf, 0U));
    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(frame_buf, 1U));
    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(frame_buf, (size_t)COMMS_TC_MAX_FRAME + 1U));

    /* Declared payload beyond the auth budget is never an auth layout. */
    (void)build_frame(COMMS_TC_SET_CONFIG, NULL, 0U);
    frame_buf[1] = (uint8_t)(COMMS_TC_MAX_AUTH_PAYLOAD + 1U);
    TEST_ASSERT_FALSE(comms_frame_is_auth_layout(frame_buf, (size_t)COMMS_TC_MAX_FRAME));
}

void test_validate_auth_accepts_sealed_frame_and_reports_slice(void)
{
    uint8_t        payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t        opcode     = 0U;
    const uint8_t *out        = NULL;
    size_t         out_len    = 0U;

    size_t n = build_auth_frame(COMMS_TC_SEND_DATA, payload, 8U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          comms_validate_tc_auth(frame_buf, n, &opcode, &out, &out_len));
    TEST_ASSERT_EQUAL_HEX8(COMMS_TC_SEND_DATA, opcode);
    TEST_ASSERT_EQUAL_size_t(8U, out_len);
    /* Payload slice excludes the 4 tag bytes that follow it on the wire. */
    TEST_ASSERT_EQUAL_PTR(&frame_buf[COMMS_TC_HDR_LEN], out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, 8);
}

void test_validate_auth_accepts_empty_command_with_null_payload(void)
{
    uint8_t        opcode  = 0U;
    const uint8_t *out     = (const uint8_t *)1;
    size_t         out_len = 99U;

    size_t n = build_auth_frame(COMMS_TC_ACTIVATE_PAYLOAD, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          comms_validate_tc_auth(frame_buf, n, &opcode, &out, &out_len));
    TEST_ASSERT_EQUAL_HEX8(COMMS_TC_ACTIVATE_PAYLOAD, opcode);
    TEST_ASSERT_EQUAL_size_t(0U, out_len);
    TEST_ASSERT_NULL(out);
}

void test_validate_auth_rejects_null_and_runt_and_oversize(void)
{
    uint8_t        opcode  = 0U;
    const uint8_t *payload = NULL;
    size_t         plen    = 0U;
    size_t         n       = build_auth_frame(COMMS_TC_RESET, NULL, 0U);

    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc_auth(NULL, n, &opcode, &payload, &plen));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc_auth(frame_buf, n, NULL, &payload, &plen));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc_auth(frame_buf, n, &opcode, NULL, &plen));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_NULL,
                          comms_validate_tc_auth(frame_buf, n, &opcode, &payload, NULL));

    for (size_t len = 0U; len < (size_t)COMMS_TC_MIN_AUTH_FRAME; len++) {
        TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_SHORT, validate_auth(frame_buf, len));
    }
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate_auth(frame_buf, (size_t)COMMS_TC_MAX_FRAME + 1U));
}

void test_validate_auth_rejects_declared_payload_beyond_budget(void)
{
    (void)build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);

    frame_buf[1] = 250U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate_auth(frame_buf, (size_t)COMMS_TC_MAX_FRAME));

    frame_buf[1] = (uint8_t)(COMMS_TC_MAX_AUTH_PAYLOAD + 1U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_LONG,
                          validate_auth(frame_buf, (size_t)COMMS_TC_MAX_FRAME));
}

void test_validate_auth_rejects_length_mismatch(void)
{
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);

    frame_buf[1] = 1U;                       /* lie about the payload size */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH, validate_auth(frame_buf, n));
}

/* A legacy CRC-only frame is not an authenticated frame: a 0-payload legacy
 * frame (4 B) is below the authenticated minimum (8 B); a longer legacy
 * frame never satisfies P+8 either. Either way the auth validator rejects
 * it — never OK. */
void test_validate_auth_rejects_legacy_frame(void)
{
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, validate(frame_buf, n));  /* legacy view */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_TOO_SHORT, validate_auth(frame_buf, n));

    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    n = build_frame(COMMS_TC_SEND_DATA, payload, 8U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, validate(frame_buf, n));  /* legacy view */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH, validate_auth(frame_buf, n));
}

void test_validate_auth_rejects_corrupted_crc(void)
{
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[n - 1] ^= 0xFFU;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, validate_auth(frame_buf, n));
}

/* Repair the CRC over tampered bytes so the ONLY failing check is the tag. */
static void repair_crc(size_t n)
{
    const uint16_t crc = comms_crc16_ccitt(frame_buf, n - (size_t)COMMS_TC_CRC_LEN);
    frame_buf[n - 2]   = (uint8_t)(crc >> 8);
    frame_buf[n - 1]   = (uint8_t)(crc & 0xFFU);
}

void test_validate_auth_rejects_tampered_payload(void)
{
    uint8_t payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t  n = build_auth_frame(COMMS_TC_SET_CONFIG, payload, 4U);
    frame_buf[3] ^= 0x01U;                   /* flip one payload bit */
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

void test_validate_auth_rejects_tampered_tag(void)
{
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[2] ^= 0x80U;                   /* flip the top tag bit */
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

void test_validate_auth_rejects_tampered_opcode(void)
{
    size_t n = build_auth_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[0] = COMMS_TC_RESET;           /* opcode covered by the tag */
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

/* A tag sealed with any other key must fail — proves the verifier actually
 * keys the HMAC and does not accept a self-consistent forgery. */
void test_validate_auth_rejects_wrong_key(void)
{
    static const uint8_t wrong_key[4] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    uint8_t  mac[32];
    uint8_t  p[4];
    size_t   n;

    put_be32(p, 60000UL);
    n = build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);

    /* Re-seal the header slice with the wrong key, then repair the CRC so
     * the tag is the only failing check. */
    hmac_sha256(wrong_key, sizeof(wrong_key), frame_buf,
                (size_t)COMMS_TC_HDR_LEN + 4U, mac);
    frame_buf[6] = mac[0];
    frame_buf[7] = mac[1];
    frame_buf[8] = mac[2];
    frame_buf[9] = mac[3];
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

/* Authentication runs BEFORE opcode parsing: a bad tag reports MAC even when
 * the opcode is also unknown, while a good tag over an unknown opcode
 * reports OPCODE. Unauthenticated senders learn nothing about the whitelist. */
void test_validate_auth_checks_tag_before_opcode(void)
{
    size_t n = build_auth_frame(0xEEU, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, validate_auth(frame_buf, n));

    frame_buf[2] ^= 0x01U;
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

void test_validate_auth_enforces_opcode_and_range_after_tag(void)
{
    uint8_t p[4];

    /* Valid tag, illegal size for RESET. */
    uint8_t junk[2] = { 0x00, 0x01 };
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN,
                          validate_auth(frame_buf,
                                        build_auth_frame(COMMS_TC_RESET, junk, 2U)));

    /* Valid tag, out-of-range beacon interval. */
    put_be32(p, COMMS_TC_BEACON_MIN_MS - 1UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate_auth(frame_buf,
                                        build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    /* Valid tag, in-range beacon interval. */
    put_be32(p, 60000UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate_auth(frame_buf,
                                        build_auth_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
}

/* Largest payload any whitelisted opcode accepts (SET_CONFIG, 32 B) seals
 * and verifies — exercises the multi-block HMAC path on the flight side. */
void test_validate_auth_accepts_largest_opcode_payload(void)
{
    uint8_t payload[32];
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate_auth(frame_buf,
                                        build_auth_frame(COMMS_TC_SET_CONFIG,
                                                         payload, (uint8_t)sizeof(payload))));
}

/* A full-budget authenticated frame (56 B) passes framing, CRC and tag, then
 * fails the per-opcode size contract — no opcode accepts that much. A
 * tampered tag on the same frame fails earlier with MAC. */
void test_validate_auth_full_budget_frame_fails_opcode_size(void)
{
    uint8_t payload[COMMS_TC_MAX_AUTH_PAYLOAD];
    memset(payload, 0xA5, sizeof(payload));

    size_t n = build_auth_frame(COMMS_TC_SET_CONFIG, payload, (uint8_t)sizeof(payload));
    TEST_ASSERT_EQUAL_size_t((size_t)COMMS_TC_MAX_FRAME, n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, validate_auth(frame_buf, n));

    frame_buf[2] ^= 0x01U;
    repair_crc(n);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, validate_auth(frame_buf, n));
}

/* ================= LoRa task wiring (watchdog mocked) ================= */

static uint8_t rx_thread_obj;
static uint8_t beacon_thread_obj;
#define RX_TH      ((osThreadId_t)&rx_thread_obj)
#define BEACON_TH  ((osThreadId_t)&beacon_thread_obj)

void test_lora_rx_task_create_registers_with_watchdog(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(RX_TH);
    watchdog_register_task_ExpectAndReturn(RX_TH, WDG_PERIOD_LORA_RX_MS, 0);
    TEST_ASSERT_EQUAL_PTR(RX_TH, lora_rx_task_create());
}

/* The beacon cadence is state-dependent and ground-commandable, so creation
   bootstraps the monitor with the SLOWEST permitted cadence: registering the
   nominal period here would false-flag the task the first time ground slows
   the beacon down. The task narrows the period itself on its first
   iteration (see the loop test below). */
void test_lora_beacon_task_create_registers_worst_case_period(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(BEACON_TH);
    watchdog_register_task_ExpectAndReturn(BEACON_TH, WDG_PERIOD_LORA_BEACON_MS, 0);
    TEST_ASSERT_EQUAL_PTR(BEACON_TH, lora_beacon_task_create());
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_MAX, WDG_PERIOD_LORA_BEACON_MS);
}

void test_lora_task_create_skips_registration_when_thread_creation_fails(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(NULL);
    TEST_ASSERT_NULL(lora_rx_task_create());   /* no watchdog_register_task */

    osThreadNew_ExpectAnyArgsAndReturn(NULL);
    TEST_ASSERT_NULL(lora_beacon_task_create());
}

/* ================= packet buffers ================= */

/* The three staging buffers live in parity-protected SRAM2 on target. What is
   testable on the host is the accessor contract: a non-NULL buffer, the size
   advertised in comms.h, and tolerance of a NULL out-parameter. */
void test_comms_buffers_report_their_declared_capacity(void)
{
    size_t   len = 0U;
    uint8_t *p;

    p = comms_beacon_buffer(&len);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_size_t(COMMS_BEACON_SIZE, len);

    p = comms_rx_buffer(&len);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_size_t(COMMS_MAX_PACKET, len);

    p = comms_tx_buffer(&len);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_size_t(COMMS_MAX_PACKET, len);

    /* NULL length pointer must not fault. */
    TEST_ASSERT_NOT_NULL(comms_beacon_buffer(NULL));
    TEST_ASSERT_NOT_NULL(comms_rx_buffer(NULL));
    TEST_ASSERT_NOT_NULL(comms_tx_buffer(NULL));
}

void test_lora_init_reports_success(void)
{
    TEST_ASSERT_EQUAL_INT(0, lora_init());
}

/* lora_send_chunked() stages the payload through the SRAM2 TX buffer in framed
   chunks: every radio call carries a 2-byte header (0-based seq, total count)
   plus up to COMMS_MAX_PACKET - COMMS_CHUNK_HDR_LEN payload bytes. The radio
   itself is a recording stub (support/radiolib_stubs.c), so what is verified
   is the on-air framing contract: per-chunk lengths within the LoRa budget,
   seq/total headers, and byte-exact reassembly. */
void test_lora_send_chunked_rejects_null_with_nonzero_length(void)
{
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(NULL, 16U));
}

void test_lora_send_chunked_accepts_null_with_zero_length(void)
{
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(NULL, 0U));
}

void test_lora_send_chunked_stages_multi_chunk_payload(void)
{
    size_t         chunk_max = 0U;
    const uint8_t *tx        = comms_tx_buffer(&chunk_max);
    uint8_t        payload[3U * 64U];

    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_GREATER_THAN_size_t((size_t)COMMS_CHUNK_HDR_LEN, chunk_max);

    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }

    radiolib_stub_tx_reset();

    /* One full framed chunk plus a 7-byte remainder: exercises both the
       full-chunk and the remainder iteration of the staging loop. */
    const size_t payload_max = chunk_max - (size_t)COMMS_CHUNK_HDR_LEN;
    const size_t len = payload_max + 7U;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(payload), len);
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, len));

    /* Two radio calls, each within the LoRa payload budget. */
    TEST_ASSERT_EQUAL_size_t(2U, radiolib_stub_tx_count());
    TEST_ASSERT_EQUAL_size_t(chunk_max, radiolib_stub_tx_len(0));
    TEST_ASSERT_EQUAL_size_t(7U + (size_t)COMMS_CHUNK_HDR_LEN,
                             radiolib_stub_tx_len(1));

    /* Headers: 0-based seq, total count. */
    TEST_ASSERT_EQUAL_UINT8(0U, radiolib_stub_tx_data(0)[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, radiolib_stub_tx_data(0)[1]);
    TEST_ASSERT_EQUAL_UINT8(1U, radiolib_stub_tx_data(1)[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, radiolib_stub_tx_data(1)[1]);

    /* Payload slices land after the header, byte-exact. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &radiolib_stub_tx_data(0)[COMMS_CHUNK_HDR_LEN],
                                  payload_max);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[payload_max],
                                  &radiolib_stub_tx_data(1)[COMMS_CHUNK_HDR_LEN], 7);

    /* The staging buffer holds the LAST chunk staged: header + remainder. */
    TEST_ASSERT_EQUAL_UINT8(1U, tx[0]);
    TEST_ASSERT_EQUAL_UINT8(2U, tx[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[payload_max], &tx[COMMS_CHUNK_HDR_LEN], 7);
}

/* A payload that fits one chunk still gets its seq/total header (seq 0 of 1),
   so the ground segment sees one framing format, never two. */
void test_lora_send_chunked_single_chunk_carries_seq_zero_total_one(void)
{
    uint8_t payload[10];
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xA0U + i);
    }

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, sizeof(payload)));

    TEST_ASSERT_EQUAL_size_t(1U, radiolib_stub_tx_count());
    TEST_ASSERT_EQUAL_size_t(sizeof(payload) + (size_t)COMMS_CHUNK_HDR_LEN,
                             radiolib_stub_tx_len(0));
    const uint8_t *c = radiolib_stub_tx_data(0);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(0U, c[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, c[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &c[COMMS_CHUNK_HDR_LEN], sizeof(payload));
}

/* The 128 B beacon must ship as ceil(128/62) = 3 framed chunks, each within
   the 64 B LoRa budget, reassembling byte-exact — never as one 128 B call
   that violates COMMS_MAX_PACKET. */
void test_lora_send_chunked_fragments_128B_beacon_within_budget(void)
{
    size_t chunk_max = 0U;
    (void)comms_tx_buffer(&chunk_max);

    uint8_t beacon[COMMS_BEACON_SIZE];
    for (size_t i = 0U; i < sizeof(beacon); i++) {
        beacon[i] = (uint8_t)(i & 0xFFU);
    }

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));

    TEST_ASSERT_EQUAL_size_t(3U, radiolib_stub_tx_count());
    size_t off = 0U;
    for (size_t i = 0U; i < 3U; i++) {
        const size_t   ln = radiolib_stub_tx_len(i);
        const uint8_t *c  = radiolib_stub_tx_data(i);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(chunk_max, ln);
        TEST_ASSERT_GREATER_THAN_size_t((size_t)COMMS_CHUNK_HDR_LEN, ln);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, c[0]);
        TEST_ASSERT_EQUAL_UINT8(3U, c[1]);
        const size_t n = ln - (size_t)COMMS_CHUNK_HDR_LEN;
        TEST_ASSERT_EQUAL_UINT8_ARRAY(&beacon[off], &c[COMMS_CHUNK_HDR_LEN], n);
        off += n;
    }
    TEST_ASSERT_EQUAL_size_t(sizeof(beacon), off);
}

/* Zero length stages nothing and touches the radio not at all. */
void test_lora_send_chunked_empty_payload_sends_nothing(void)
{
    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(frame_buf, 0U));
    TEST_ASSERT_EQUAL_size_t(0U, radiolib_stub_tx_count());
}

/* The validator band must equal the state-machine band [10 s, 16 min]: the
   certified edges pass, one millisecond outside fails on both sides. */
void test_validate_beacon_interval_matches_state_machine_band(void)
{
    TEST_ASSERT_EQUAL_UINT32(10000UL, (uint32_t)COMMS_TC_BEACON_MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(960000UL, (uint32_t)COMMS_TC_BEACON_MAX_MS);

    uint8_t p[4];

    put_be32(p, 10000UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
    put_be32(p, 960000UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, 9999UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
    put_be32(p, 960001UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
}

/* Regression: the legacy [1 s, 1 h] edges the validator used to accept must
   now fail — 1 s hammers the TX chain below the duty-cycle floor, 1 h
   out-runs the beacon watchdog ceiling. */
void test_validate_rejects_legacy_1s_1h_band_edges(void)
{
    uint8_t p[4];

    put_be32(p, 1000UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));

    put_be32(p, 3600000UL);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          validate(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U)));
}

/* The RX staging buffer, the validator budget and the driver reject threshold
   must agree: a PHY payload that fits the buffer fits validation, so the
   driver's oversize reject (radiolib_driver.cpp, no silent truncation) fires
   exactly where comms_validate_tc() reports TOO_LONG — no gap between them. */
void test_rx_buffer_matches_max_frame_budget(void)
{
    size_t rx_len = 0U;
    TEST_ASSERT_NOT_NULL(comms_rx_buffer(&rx_len));
    TEST_ASSERT_EQUAL_size_t((size_t)COMMS_TC_MAX_FRAME, rx_len);
    TEST_ASSERT_EQUAL_size_t((size_t)COMMS_MAX_PACKET, rx_len);
}

/* A radio refusal mid-staging aborts the transfer: the chunk that failed
 * is reported, not retried, not skipped. */
void test_lora_send_chunked_reports_radio_tx_failure(void)
{
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    host_lora_fail_tx_on_call(1);
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(payload, sizeof(payload)));
}

/* TX_DONE never arriving (DIO1 timeout) aborts the transfer the same way. */
void test_lora_send_chunked_reports_tx_done_timeout(void)
{
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    host_lora_fail_wait_on_call(1);
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(payload, sizeof(payload)));
}

/* A failure on the SECOND chunk still aborts: the first chunk went out,
 * but the transfer as a whole did not complete. */
void test_lora_send_chunked_reports_failure_on_later_chunk(void)
{
    size_t         chunk_max = 0U;
    const uint8_t *tx        = comms_tx_buffer(&chunk_max);
    static uint8_t payload[3U * 64U];
    size_t         i;

    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_GREATER_THAN_size_t(0U, chunk_max);
    for (i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }

    host_lora_fail_wait_on_call(2);   /* first chunk OK, second TX_DONE times out */
    TEST_ASSERT_EQUAL_INT(-1,
                          lora_send_chunked(payload, chunk_max + 7U));
}

/* SET_CONFIG and SEND_DATA are accepted by the gate (validation only). Handlers are TODO (no-op), so no mock expectations are queued. Frames are sealed: ENFORCE=1 rejects legacy CRC-only frames with ERR_MAC. */
void test_rx_gate_dispatches_set_config_and_send_data(void)
{
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t  n;

    n = build_auth_frame(COMMS_TC_SET_CONFIG, payload, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));

    n = build_auth_frame(COMMS_TC_SEND_DATA, payload, 8U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* ================= task loops ================= */

static jmp_buf loop_escape;
static int     delay_calls;

/* Iterations every task-loop test must complete before escaping. Single
   source of truth shared by the escape stubs AND the helpers below, so a
   stub threshold can never drift from the expected count. */
#define TASK_LOOP_ITERS 3

/* Hang ceiling for the task-loop tests (seconds). The #50 failure mode was
   an escape stub that NEVER fired, so the loop spun until CI's 6 h job
   timeout. alarm() makes that scenario fail loudly in seconds: the SIGALRM
   handler aborts with a diagnostic naming the likely cause. POSIX hosts
   only (alarm/signal) — same portability class as the Ceedling host
   harness itself (Linux/macOS CI + dev shells). */
#define TASK_LOOP_HANG_SECS 30
/* Two-level indirection is REQUIRED: a single-level #x would suppress
   expansion and print the literal macro name in the diagnostic instead
   of 30 (verified: Kilo caught exactly that on the first attempt). */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define TASK_LOOP_HANG_SECS_STR STRINGIFY(TASK_LOOP_HANG_SECS)

static void task_loop_hang_handler(int sig)
{
    (void)sig;
    /* Stringify so the message shows the real value (30), not the macro
       name — a diagnostic you have to resolve by hand is half a diagnostic. */
    const char msg[] = "\nERROR: task-loop test exceeded "
        TASK_LOOP_HANG_SECS_STR
        " s - escape stub never fired? (PR #50 failure mode: stub counting "
        "a call the loop never makes)\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    _exit(2);
}

static osStatus_t osDelay_escape_cb(uint32_t ticks, int cmock_num_calls)
{
    (void)ticks;
    (void)cmock_num_calls;
    delay_calls++;
    if (delay_calls >= TASK_LOOP_ITERS) {
        longjmp(loop_escape, 1);
    }
    return osOK;
}

/* Shared escape scaffold for the RTOS task-loop tests — owns the WHOLE
   escape contract:
     - arms a SIGALRM hang ceiling, so the #50 never-fires scenario fails
       in seconds instead of spinning to CI's 6 h job timeout;
     - the setjmp/longjmp ceremony ("a task loop must never return" —
       TEST_FAIL_MESSAGE catches an early return);
     - reset + exact-count assert of the caller's per-iteration counter.
   The counter is a file-scope static, so it stays determinate across the
   longjmp (C11 7.13.2.1 exempts objects of static storage duration) — no
   volatile needed. The counted call differs by design per caller (osDelay
   for the delay-paced beacon loop, watchdog_alive_self() kick for the
   event-driven RX loop); if a stub stops firing, the alarm fires instead
   and names the failure mode. */
static void run_task_until_escape(void (*task)(void *), int *counter)
{
    *counter = 0;
    (void)signal(SIGALRM, task_loop_hang_handler);
    alarm(TASK_LOOP_HANG_SECS);

    if (setjmp(loop_escape) == 0) {
        task(NULL);
        /* Early return: Unity longjmps from TEST_FAIL_MESSAGE, so the
           disarm below would be skipped - tearDown() covers that path. */
        TEST_FAIL_MESSAGE("RTOS task loop returned - it must not exit");
    }

    alarm(0); /* escaped in time - disarm the hang ceiling */

    /* If the escape stub fired late or early, the count is wrong here. */
    TEST_ASSERT_EQUAL_INT(TASK_LOOP_ITERS, *counter);
}

/* Run the beacon task until the third osDelay(); assert it never returned. */
static void run_task_iterations(void (*task)(void *))
{
    osDelay_Stub(osDelay_escape_cb);

    run_task_until_escape(task, &delay_calls);
}

/* lora_rx_task() is EVENT-DRIVEN: every iteration blocks on
   osThreadFlagsWait() (100 ms poll granularity) and kicks the watchdog —
   it NEVER calls osDelay(). An escape hatch counting osDelay() calls
   therefore never fires and the test spins forever (CI 6 h timeout, both
   x86_64 and aarch64). Escape by counting watchdog_alive_self() calls
   instead: exactly one kick per loop iteration. */
static int alive_calls;

static void alive_escape_cb(int cmock_num_calls)
{
    (void)cmock_num_calls;
    alive_calls++;
    if (alive_calls >= TASK_LOOP_ITERS) {
        longjmp(loop_escape, 1);
    }
}

/* lora_rx_task() registers its handle, then loops forever on the DIO1
   RX_DONE flag, kicking the watchdog once per iteration. */
void test_lora_rx_task_loops_forever_kicking_watchdog_each_iteration(void)
{
    osThreadGetId_ExpectAndReturn(RX_TH);
    /* Return the RX_DONE flag every iteration so the loop body runs. */
    osThreadFlagsWait_IgnoreAndReturn(LORA_RX_FLAG);
    watchdog_alive_self_Stub(alive_escape_cb);

    run_task_until_escape(lora_rx_task, &alive_calls);
}

/* First iteration: the task narrows its monitored period from the bootstrap
   worst case to the cadence actually in force (duplicate-refresh path in
   watchdog_register_task()). Later iterations must NOT re-register while the
   cadence is unchanged - a needless re-registration resets the monitor's
   grace window on every beacon and would mask a genuinely hung task. */
void test_lora_beacon_task_loop_registers_actual_cadence_once(void)
{
    for (int i = 0; i < 3; i++) {
        state_machine_get_beacon_interval_ExpectAndReturn(BEACON_INTERVAL_READY);
        if (i == 0) {
            osThreadGetId_ExpectAndReturn(BEACON_TH);
            watchdog_register_task_ExpectAndReturn(BEACON_TH,
                                                   BEACON_INTERVAL_READY, 0);
        }
        watchdog_alive_self_Expect();
    }

    run_task_iterations(lora_beacon_task);
}

/* End-to-end framing proof: one beacon iteration emits the 128 B beacon as 3
   framed chunks (never a single 128 B lora_tx violating COMMS_MAX_PACKET).
   Three loop iterations therefore log 9 radio calls with cycling seq 0,1,2
   and total 3, every call within the 64 B budget. */
void test_lora_beacon_task_fragments_beacon_into_framed_chunks(void)
{
    radiolib_stub_tx_reset();

    for (int i = 0; i < 3; i++) {
        state_machine_get_beacon_interval_ExpectAndReturn(BEACON_INTERVAL_READY);
        if (i == 0) {
            osThreadGetId_ExpectAndReturn(BEACON_TH);
            watchdog_register_task_ExpectAndReturn(BEACON_TH,
                                                   BEACON_INTERVAL_READY, 0);
        }
        watchdog_alive_self_Expect();
    }

    run_task_iterations(lora_beacon_task);

    TEST_ASSERT_EQUAL_size_t(9U, radiolib_stub_tx_count());
    for (size_t i = 0U; i < 9U; i++) {
        const size_t   ln = radiolib_stub_tx_len(i);
        const uint8_t *c  = radiolib_stub_tx_data(i);
        TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)COMMS_MAX_PACKET, ln);
        TEST_ASSERT_GREATER_THAN_size_t((size_t)COMMS_CHUNK_HDR_LEN, ln);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % 3U), c[0]);
        TEST_ASSERT_EQUAL_UINT8(3U, c[1]);
    }
}

/* A cadence change commanded from ground must be re-declared to the monitor,
   otherwise it keeps judging the beacon against the previous period. */
void test_lora_beacon_task_loop_reregisters_when_cadence_changes(void)
{
    const uint32_t intervals[3] = {
        BEACON_INTERVAL_READY,
        BEACON_INTERVAL_READY,      /* unchanged -> no re-registration */
        BEACON_INTERVAL_ACTIVE      /* ground slows/speeds the beacon */
    };

    for (int i = 0; i < 3; i++) {
        state_machine_get_beacon_interval_ExpectAndReturn(intervals[i]);
        if ((i == 0) || (intervals[i] != intervals[i - 1])) {
            osThreadGetId_ExpectAndReturn(BEACON_TH);
            watchdog_register_task_ExpectAndReturn(BEACON_TH, intervals[i], 0);
        }
        watchdog_alive_self_Expect();
    }

    run_task_iterations(lora_beacon_task);
}

/* If the monitor refuses the registration the task must keep retrying on the
   next iteration rather than latching a period it never actually declared. */
void test_lora_beacon_task_loop_retries_failed_registration(void)
{
    for (int i = 0; i < 3; i++) {
        state_machine_get_beacon_interval_ExpectAndReturn(BEACON_INTERVAL_READY);
        osThreadGetId_ExpectAndReturn(BEACON_TH);
        watchdog_register_task_ExpectAndReturn(BEACON_TH,
                                               BEACON_INTERVAL_READY, -1);
        watchdog_alive_self_Expect();
    }

    run_task_iterations(lora_beacon_task);
}
