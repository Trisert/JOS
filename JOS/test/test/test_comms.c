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
 *   - RX accounting counters classify each verdict correctly (including the
 *     PHY bucket for frames the radio drops below the validator).
 *   - chunk framing: msg/seq/total headers, consecutive-message ids, the
 *     255-chunk cap, mid-sequence abort + TX stats, and a ground reassembly
 *     model proving loss/duplicate/mixed-message detection and
 *     reorder-tolerant reassembly.
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
   assert the exact on-air chunks (lengths, msg/seq/total headers, reassembly). */
extern size_t          radiolib_stub_tx_count(void);
extern size_t          radiolib_stub_tx_len(size_t i);
extern const uint8_t  *radiolib_stub_tx_data(size_t i);
extern void            radiolib_stub_tx_reset(void);
/* Fault injection in the same stub: fail the Nth TX call, or fail every
   TX_DONE wait. Armed by the abort-path tests, cleared by _reset(). */
extern void            radiolib_stub_tx_fail_at_index(size_t i);
extern void            radiolib_stub_tx_fail_wait_done(int fail);
/* Direct stub entry points (the past-cap loud-fail test calls lora_tx). */
extern int             lora_tx(const uint8_t *data, size_t len);

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
 * failure must never leak into the next test. The TT&C MAC hook is reset too:
 * a verifier installed by a test must never leak into the next one. */
void setUp(void)   { memset(frame_buf, 0, sizeof(frame_buf)); host_lora_reset();
                     comms_ttc_set_mac_verifier(NULL); }
/* Safety net: the hang ceiling must NEVER survive past its own test case.
   The early-return path inside run_task_until_escape() longjmps into Unity
   before alarm(0) runs, so tearDown() disarms unconditionally — otherwise a
   still-ticking timer could kill an innocent later test with a misleading
   "escape stub never fired" diagnostic. */
void tearDown(void) { alarm(0); comms_ttc_set_mac_verifier(NULL); }

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
    for (int r = COMMS_TC_OK; r <= COMMS_TC_ERR_PHY; r++) {
        TEST_ASSERT_NOT_NULL(comms_tc_result_str((comms_tc_result_t)r));
    }
    TEST_ASSERT_EQUAL_STRING("MAC", comms_tc_result_str(COMMS_TC_ERR_MAC));
    TEST_ASSERT_EQUAL_STRING("PHY", comms_tc_result_str(COMMS_TC_ERR_PHY));
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
    put_be32(p, 60000UL);                    /* 60 s — inside [10 s, 16 min] */
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
   chunks: every radio call carries a 3-byte header (message id, 0-based seq,
   total count) plus up to COMMS_MAX_PACKET - COMMS_CHUNK_HDR_LEN payload
   bytes. The radio itself is a recording stub (support/radiolib_stubs.c), so
   what is verified is the on-air framing contract: per-chunk lengths within
   the LoRa budget, msg/seq/total headers, and byte-exact reassembly. */
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

    /* Headers: one message id shared by both chunks, 0-based seq, total. */
    const uint8_t *c0 = radiolib_stub_tx_data(0);
    const uint8_t *c1 = radiolib_stub_tx_data(1);
    TEST_ASSERT_EQUAL_UINT8(c0[COMMS_CHUNK_OFF_MSG], c1[COMMS_CHUNK_OFF_MSG]);
    TEST_ASSERT_EQUAL_UINT8(0U, c0[COMMS_CHUNK_OFF_SEQ]);
    TEST_ASSERT_EQUAL_UINT8(2U, c0[COMMS_CHUNK_OFF_TOTAL]);
    TEST_ASSERT_EQUAL_UINT8(1U, c1[COMMS_CHUNK_OFF_SEQ]);
    TEST_ASSERT_EQUAL_UINT8(2U, c1[COMMS_CHUNK_OFF_TOTAL]);

    /* Payload slices land after the header, byte-exact. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &radiolib_stub_tx_data(0)[COMMS_CHUNK_HDR_LEN],
                                  payload_max);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[payload_max],
                                  &radiolib_stub_tx_data(1)[COMMS_CHUNK_HDR_LEN], 7);

    /* The staging buffer holds the LAST chunk staged: header + remainder. */
    TEST_ASSERT_EQUAL_UINT8(c1[COMMS_CHUNK_OFF_MSG], tx[COMMS_CHUNK_OFF_MSG]);
    TEST_ASSERT_EQUAL_UINT8(1U, tx[COMMS_CHUNK_OFF_SEQ]);
    TEST_ASSERT_EQUAL_UINT8(2U, tx[COMMS_CHUNK_OFF_TOTAL]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[payload_max], &tx[COMMS_CHUNK_HDR_LEN], 7);
}

/* A payload that fits one chunk still gets its full header (seq 0 of 1),
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
    TEST_ASSERT_EQUAL_UINT8(0U, c[COMMS_CHUNK_OFF_SEQ]);
    TEST_ASSERT_EQUAL_UINT8(1U, c[COMMS_CHUNK_OFF_TOTAL]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &c[COMMS_CHUNK_HDR_LEN], sizeof(payload));
}

/* The 128 B beacon must ship as ceil(128/61) = 3 framed chunks, each within
   the 64 B LoRa budget, reassembling byte-exact — never as one 128 B call
   that violates COMMS_MAX_PACKET. All three chunks share one message id. */
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
    const uint8_t msg = radiolib_stub_tx_data(0)[COMMS_CHUNK_OFF_MSG];
    size_t off = 0U;
    for (size_t i = 0U; i < 3U; i++) {
        const size_t   ln = radiolib_stub_tx_len(i);
        const uint8_t *c  = radiolib_stub_tx_data(i);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(chunk_max, ln);
        TEST_ASSERT_GREATER_THAN_size_t((size_t)COMMS_CHUNK_HDR_LEN, ln);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL_UINT8(msg, c[COMMS_CHUNK_OFF_MSG]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, c[COMMS_CHUNK_OFF_SEQ]);
        TEST_ASSERT_EQUAL_UINT8(3U, c[COMMS_CHUNK_OFF_TOTAL]);
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

/* Consecutive messages carry consecutive ids: ground can tell two beacons
   apart and spot a missing one. (Ids wrap mod 256 — the +1 arithmetic below
   is wraparound-safe by construction.) */
void test_lora_send_chunked_tags_consecutive_messages_with_distinct_ids(void)
{
    uint8_t payload[10];
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0x50U + i);
    }

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, sizeof(payload)));

    TEST_ASSERT_EQUAL_size_t(2U, radiolib_stub_tx_count());
    const uint8_t *a = radiolib_stub_tx_data(0);
    const uint8_t *b = radiolib_stub_tx_data(1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(a[COMMS_CHUNK_OFF_MSG] + 1U),
                            b[COMMS_CHUNK_OFF_MSG]);
}

/* The 1-byte total field caps a message at 255 chunks: anything longer is
   rejected before the first chunk goes on air (and, as a pre-air validation
   failure, is NOT counted as a radio abort in the TX stats). */
void test_lora_send_chunked_rejects_total_beyond_255(void)
{
    size_t chunk_max = 0U;
    (void)comms_tx_buffer(&chunk_max);
    const size_t payload_max = chunk_max - (size_t)COMMS_CHUNK_HDR_LEN;

    static uint8_t big[256U * 64U];
    const size_t len = 255U * payload_max + 1U;   /* needs 256 chunks */
    TEST_ASSERT_LESS_THAN_size_t(sizeof(big) + 1U, len);

    radiolib_stub_tx_reset();
    comms_tx_stats_t before, after;
    comms_tx_get_stats(&before);
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(big, len));
    TEST_ASSERT_EQUAL_size_t(0U, radiolib_stub_tx_count());
    comms_tx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_ok, after.sequences_ok);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_failed, after.sequences_failed);
    TEST_ASSERT_EQUAL_UINT32(before.chunks_sent, after.chunks_sent);
}

/* Successful sequences are counted: one message ok, chunks added up. */
void test_lora_send_chunked_counts_sequences_and_chunks(void)
{
    size_t chunk_max = 0U;
    (void)comms_tx_buffer(&chunk_max);
    const size_t payload_max = chunk_max - (size_t)COMMS_CHUNK_HDR_LEN;

    uint8_t payload[64];
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)i;
    }

    radiolib_stub_tx_reset();
    comms_tx_stats_t before, after;
    comms_tx_get_stats(&before);
    /* payload_max + 1 payload bytes -> exactly 2 chunks. */
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, payload_max + 1U));
    comms_tx_get_stats(&after);
    TEST_ASSERT_EQUAL_size_t(2U, radiolib_stub_tx_count());
    TEST_ASSERT_EQUAL_UINT32(before.sequences_ok + 1U, after.sequences_ok);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_failed, after.sequences_failed);
    TEST_ASSERT_EQUAL_UINT32(before.chunks_sent + 2U, after.chunks_sent);
}

void test_tx_stats_tolerates_null_out(void)
{
    comms_tx_get_stats(NULL);   /* must not fault */
}

/* A TX failure mid-sequence aborts: chunks 2..3 never go on air, the call
   reports the error, and the abort is counted exactly once. */
void test_lora_send_chunked_aborts_on_mid_sequence_tx_failure(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    for (size_t i = 0U; i < sizeof(beacon); i++) {
        beacon[i] = (uint8_t)(i & 0xFFU);
    }

    radiolib_stub_tx_reset();
    radiolib_stub_tx_fail_at_index(1U);   /* chunk 0 ok, chunk 1 fails */

    comms_tx_stats_t before, after;
    comms_tx_get_stats(&before);
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(1U, radiolib_stub_tx_count());
    comms_tx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_ok, after.sequences_ok);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_failed + 1U, after.sequences_failed);
    TEST_ASSERT_EQUAL_UINT32(before.chunks_sent + 1U, after.chunks_sent);

    radiolib_stub_tx_reset();   /* disarm the fault for later tests */
}

/* Same abort contract when the TX_DONE wait times out instead: the chunk did
   go on air (it counts as sent) but the sequence still stops and fails. */
void test_lora_send_chunked_aborts_on_mid_sequence_wait_failure(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    for (size_t i = 0U; i < sizeof(beacon); i++) {
        beacon[i] = (uint8_t)(0x80U + (i & 0x7FU));
    }

    radiolib_stub_tx_reset();
    radiolib_stub_tx_fail_wait_done(1);

    comms_tx_stats_t before, after;
    comms_tx_get_stats(&before);
    TEST_ASSERT_EQUAL_INT(-1, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(1U, radiolib_stub_tx_count());
    comms_tx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_ok, after.sequences_ok);
    TEST_ASSERT_EQUAL_UINT32(before.sequences_failed + 1U, after.sequences_failed);
    TEST_ASSERT_EQUAL_UINT32(before.chunks_sent + 1U, after.chunks_sent);

    radiolib_stub_tx_reset();   /* disarm the fault for later tests */
}

/* The stub fails loudly past the on-air budget instead of truncating: a full
   budget call is logged, one byte over is rejected and never logged. This
   pins RADIOLIB_STUB_TX_CAP == COMMS_MAX_PACKET behaviourally (the stub
   cannot include comms.h — see its header comment). */
void test_stub_tx_rejects_calls_past_comms_max_packet(void)
{
    uint8_t buf[COMMS_MAX_PACKET + 1U];
    memset(buf, 0xA5U, sizeof(buf));

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_tx(buf, (size_t)COMMS_MAX_PACKET));
    TEST_ASSERT_EQUAL_size_t(1U, radiolib_stub_tx_count());
    TEST_ASSERT_EQUAL_size_t((size_t)COMMS_MAX_PACKET, radiolib_stub_tx_len(0));
    TEST_ASSERT_EQUAL_INT(-1, lora_tx(buf, (size_t)COMMS_MAX_PACKET + 1U));
    TEST_ASSERT_EQUAL_size_t(1U, radiolib_stub_tx_count());
}

/* PHY-level drops (what lora_rx_task() accounts when lora_rx() != 0) land in
   their own bucket: counted, never dispatched, never misfiled as malformed. */
void test_rx_stats_accounts_phy_failures_without_dispatch(void)
{
    comms_rx_stats_t before, after;
    comms_rx_get_stats(&before);

    comms_rx_account(COMMS_TC_ERR_PHY);
    comms_rx_account(COMMS_TC_ERR_PHY);

    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 2U, after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_phy + 2U, after.rejected_phy);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_malformed, after.rejected_malformed);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_crc, after.rejected_crc);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_opcode, after.rejected_opcode);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_range, after.rejected_range);
}

/* ---------- Ground-segment reassembly model ----------
 * The flight side only frames; reassembly happens on the ground. This model
 * proves the on-air bytes carry ENOUGH information: fed with the stub-logged
 * chunks it must reassemble byte-exact, and must refuse a loss, a duplicate,
 * or chunks mixed from two messages. (Reorder needs no refusal — seq drives
 * placement, so any arrival order reassembles.) Returns 0 with *out_len payload bytes in out on
 * success, -1 on any gap, duplicate, or id/total mismatch. */
#define GROUND_REASS_MAX_TOTAL 16U

static int ground_reassemble(const uint8_t *chunks[], const size_t lens[],
                             size_t n, uint8_t *out, size_t out_cap,
                             size_t *out_len)
{
    if ((n == 0U) || (out == NULL) || (out_len == NULL)) {
        return -1;
    }
    const uint8_t msg   = chunks[0][COMMS_CHUNK_OFF_MSG];
    const uint8_t total = chunks[0][COMMS_CHUNK_OFF_TOTAL];
    if ((total == 0U) || (total > GROUND_REASS_MAX_TOTAL) ||
        (n != (size_t)total)) {
        return -1;
    }
    uint8_t seen[GROUND_REASS_MAX_TOTAL]    = { 0U };
    size_t  seq_len[GROUND_REASS_MAX_TOTAL] = { 0U };
    size_t  total_payload = 0U;
    for (size_t i = 0U; i < n; i++) {
        if (chunks[i] == NULL) {
            return -1;
        }
        if ((chunks[i][COMMS_CHUNK_OFF_MSG] != msg) ||
            (chunks[i][COMMS_CHUNK_OFF_TOTAL] != total)) {
            return -1;   /* mixed messages */
        }
        const uint8_t seq = chunks[i][COMMS_CHUNK_OFF_SEQ];
        if ((seq >= total) || (seen[seq] != 0U)) {
            return -1;   /* out of range or duplicate */
        }
        if (lens[i] <= (size_t)COMMS_CHUNK_HDR_LEN) {
            return -1;
        }
        seen[seq]    = 1U;
        seq_len[seq] = lens[i] - (size_t)COMMS_CHUNK_HDR_LEN;
        total_payload += seq_len[seq];
    }
    for (uint8_t s = 0U; s < total; s++) {
        if (seen[s] == 0U) {
            return -1;   /* gap */
        }
    }
    if (total_payload > out_cap) {
        return -1;
    }
    size_t off = 0U;
    for (uint8_t s = 0U; s < total; s++) {
        for (size_t i = 0U; i < n; i++) {
            if (chunks[i][COMMS_CHUNK_OFF_SEQ] == s) {
                memcpy(&out[off], &chunks[i][COMMS_CHUNK_HDR_LEN], seq_len[s]);
                off += seq_len[s];
                break;
            }
        }
    }
    *out_len = off;
    return 0;
}

static void fill_beacon_pattern(uint8_t *beacon, size_t len, uint8_t base)
{
    for (size_t i = 0U; i < len; i++) {
        beacon[i] = (uint8_t)(base + (i & 0xFFU));
    }
}

/* A lost chunk is detectable: 2 of 3 chunks (n != total) refuse to reassemble. */
void test_ground_model_detects_lost_chunk(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    fill_beacon_pattern(beacon, sizeof(beacon), 0U);

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(3U, radiolib_stub_tx_count());

    const uint8_t *got[2] = { radiolib_stub_tx_data(0), radiolib_stub_tx_data(2) };
    const size_t   lens[2] = { radiolib_stub_tx_len(0), radiolib_stub_tx_len(2) };
    uint8_t out[COMMS_BEACON_SIZE];
    size_t  out_len = 0U;
    TEST_ASSERT_EQUAL_INT(-1, ground_reassemble(got, lens, 2U, out, sizeof(out), &out_len));
}

/* Reordered chunks still reassemble byte-exact: placement follows seq, not
   arrival order. */
void test_ground_model_reassembles_reordered_chunks(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    fill_beacon_pattern(beacon, sizeof(beacon), 0x20U);

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(3U, radiolib_stub_tx_count());

    /* arrival order 2, 0, 1 */
    const uint8_t *got[3] = {
        radiolib_stub_tx_data(2), radiolib_stub_tx_data(0), radiolib_stub_tx_data(1)
    };
    const size_t lens[3] = {
        radiolib_stub_tx_len(2), radiolib_stub_tx_len(0), radiolib_stub_tx_len(1)
    };
    uint8_t out[COMMS_BEACON_SIZE];
    size_t  out_len = 0U;
    TEST_ASSERT_EQUAL_INT(0, ground_reassemble(got, lens, 3U, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(beacon), out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(beacon, out, sizeof(beacon));
}

/* A duplicate chunk (same msg + seq twice, one seq missing) is refused. */
void test_ground_model_detects_duplicate_chunk(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    fill_beacon_pattern(beacon, sizeof(beacon), 0x40U);

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(3U, radiolib_stub_tx_count());

    const uint8_t *got[3] = {
        radiolib_stub_tx_data(0), radiolib_stub_tx_data(1), radiolib_stub_tx_data(1)
    };
    const size_t lens[3] = {
        radiolib_stub_tx_len(0), radiolib_stub_tx_len(1), radiolib_stub_tx_len(1)
    };
    uint8_t out[COMMS_BEACON_SIZE];
    size_t  out_len = 0U;
    TEST_ASSERT_EQUAL_INT(-1, ground_reassemble(got, lens, 3U, out, sizeof(out), &out_len));
}

/* Chunks from two consecutive messages never merge: the id mismatch refuses,
   and the two ids are consecutive. */
void test_ground_model_refuses_chunks_from_two_messages(void)
{
    uint8_t beacon[COMMS_BEACON_SIZE];
    fill_beacon_pattern(beacon, sizeof(beacon), 0x60U);

    radiolib_stub_tx_reset();
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(beacon, sizeof(beacon)));
    TEST_ASSERT_EQUAL_size_t(6U, radiolib_stub_tx_count());

    const uint8_t *first  = radiolib_stub_tx_data(0);
    const uint8_t *second = radiolib_stub_tx_data(3);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(first[COMMS_CHUNK_OFF_MSG] + 1U),
                            second[COMMS_CHUNK_OFF_MSG]);

    const uint8_t *got[2] = { first, second };
    const size_t   lens[2] = { radiolib_stub_tx_len(0), radiolib_stub_tx_len(3) };
    uint8_t out[COMMS_BEACON_SIZE];
    size_t  out_len = 0U;
    TEST_ASSERT_EQUAL_INT(-1, ground_reassemble(got, lens, 2U, out, sizeof(out), &out_len));
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
   Three loop iterations therefore log 9 radio calls with msg ids stepping by
   one per beacon, cycling seq 0,1,2 and total 3, every call within the 64 B
   budget. */
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
    const uint8_t base = radiolib_stub_tx_data(0)[COMMS_CHUNK_OFF_MSG];
    for (size_t i = 0U; i < 9U; i++) {
        const size_t   ln = radiolib_stub_tx_len(i);
        const uint8_t *c  = radiolib_stub_tx_data(i);
        TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)COMMS_MAX_PACKET, ln);
        TEST_ASSERT_GREATER_THAN_size_t((size_t)COMMS_CHUNK_HDR_LEN, ln);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(base + i / 3U), c[COMMS_CHUNK_OFF_MSG]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % 3U), c[COMMS_CHUNK_OFF_SEQ]);
        TEST_ASSERT_EQUAL_UINT8(3U, c[COMMS_CHUNK_OFF_TOTAL]);
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

/* ==================================================================== */
/* TT&C command frame codec (W1)                                        */
/*                                                                      */
/* Every expected byte/number below is a LITERAL, never the COMMS_TTC_* */
/* macro the production code uses — a wrong constant in comms.h must    */
/* not be able to make a test agree with a bug.                         */
/* ==================================================================== */

/* Independent scratch buffer: a TT&C frame reaches COMMS_TTC_MAX_FRAME (192 B
   with ECC on: 12 content blocks), larger than the legacy frame_buf
   (COMMS_TC_MAX_FRAME + 8). */
static uint8_t ttc_buf[COMMS_TTC_MAX_FRAME + 8U];

static void ttc_info_init(comms_ttc_info_t *info, uint8_t station, uint8_t ecc,
                          uint8_t type, uint8_t task, uint8_t pl_len)
{
    info->station_id = station;
    info->ecc_flag   = ecc;
    info->tec_type   = type;
    info->tec_task   = task;
    info->pl_len     = pl_len;
}

/* Build a minimal valid frame (no payload). Holds the ECC flag caller-supplied
   so the tests can exercise both tail rules. */
static size_t ttc_build_header_only(uint8_t ecc)
{
    comms_ttc_info_t info;
    size_t           n = 0U;

    ttc_info_init(&info, 1U, ecc, 0U, 0x01U, 0U);   /* HK, OBC reboot */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));
    return n;
}

/* ---- INFO bitfield ---- */

void test_ttc_info_bitfield_pack_unpack_literals(void)
{
    uint8_t type = 0xEEU;
    uint8_t task = 0xEEU;

    /* INFO byte 2 = (TEC type << 6) | TEC task: the TEC type occupies the two
       HIGH bits (bit 1-2 read MSB-first) and the task the six LOW bits (bit 3-8).
       Source: TTC packets.xlsx 'Task types'!C4:C7 ("Bin ID": HK=00, DAQ=01,
       PE=10, DT=11) composed with the 6-bit task Bin ID into 'Task details'
       column D ("TEC bin ID") / column E ("TEC hex ID"). Worked examples from
       the sheet: 'Task details'!E5 (HK task 1, OBC reboot) = 0x01;
       'Task details'!E21 (HK task 17, TLE) = 0x11; 'Task details'!E55
       (HK task 51, Lora link) = 0x33. */
    TEST_ASSERT_EQUAL_HEX8(0x00U, comms_ttc_info_pack(0U, 0U));
    TEST_ASSERT_EQUAL_HEX8(0x01U, comms_ttc_info_pack(0U, 0x01U));   /* HK, OBC reboot */
    TEST_ASSERT_EQUAL_HEX8(0x11U, comms_ttc_info_pack(0U, 0x11U));   /* HK, TLE        */
    TEST_ASSERT_EQUAL_HEX8(0x33U, comms_ttc_info_pack(0U, 0x33U));   /* HK, Lora link  */
    TEST_ASSERT_EQUAL_HEX8(0x40U, comms_ttc_info_pack(1U, 0U));      /* DAQ=01 << 6    */
    TEST_ASSERT_EQUAL_HEX8(0xC0U, comms_ttc_info_pack(3U, 0U));      /* DT=11  << 6    */
    TEST_ASSERT_EQUAL_HEX8(0xFFU, comms_ttc_info_pack(3U, 63U));

    comms_ttc_info_unpack(0x11U, &type, &task);
    TEST_ASSERT_EQUAL_UINT8(0U, type);
    TEST_ASSERT_EQUAL_UINT8(0x11U, task);

    comms_ttc_info_unpack(0x33U, &type, &task);
    TEST_ASSERT_EQUAL_UINT8(0U, type);
    TEST_ASSERT_EQUAL_UINT8(0x33U, task);

    comms_ttc_info_unpack(0xFFU, &type, &task);
    TEST_ASSERT_EQUAL_UINT8(3U, type);
    TEST_ASSERT_EQUAL_UINT8(63U, task);

    /* NULL output pointers are ignored, not dereferenced. */
    comms_ttc_info_unpack(0x00U, NULL, NULL);
}

/* ---- 16*n padding rule ---- */

void test_ttc_padded_len_rounds_to_16(void)
{
    TEST_ASSERT_EQUAL_size_t(0U,   comms_ttc_padded_len(0U, false));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(1U, false));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(15U, false));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(16U, false));
    TEST_ASSERT_EQUAL_size_t(32U,  comms_ttc_padded_len(17U, false));
    TEST_ASSERT_EQUAL_size_t(112U, comms_ttc_padded_len(112U, false));
    /* ECC ON: the 6 RS parity bytes ('Packet structure'!H54) live INSIDE each
       16-byte block, so a block carries 10 DATA bytes + 6 parity and the
       packet is STILL 16*n (!A14) - it is not 16*n + 6. */
    TEST_ASSERT_EQUAL_size_t(0U,   comms_ttc_padded_len(0U, true));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(1U, true));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(10U, true));
    TEST_ASSERT_EQUAL_size_t(32U,  comms_ttc_padded_len(11U, true));
    TEST_ASSERT_EQUAL_size_t(32U,  comms_ttc_padded_len(12U, true));   /* header only */
    TEST_ASSERT_EQUAL_size_t(32U,  comms_ttc_padded_len(16U, true));
    TEST_ASSERT_EQUAL_size_t(16U,  comms_ttc_padded_len(12U, false));
    TEST_ASSERT_EQUAL_size_t(48U,  comms_ttc_padded_len(30U, true));
    TEST_ASSERT_EQUAL_size_t(64U,  comms_ttc_padded_len(31U, true));
    TEST_ASSERT_EQUAL_size_t(192U, comms_ttc_padded_len(112U, true));  /* 12 blocks */

    /* The invariant !A14 asks for, in BOTH modes: the total is 16*n. */
    for (size_t c = 0U; c <= 120U; c++) {
        TEST_ASSERT_EQUAL_size_t(0U, comms_ttc_padded_len(c, false) % 16U);
        TEST_ASSERT_EQUAL_size_t(0U, comms_ttc_padded_len(c, true) % 16U);
        /* The parity costs capacity, never a tail: ECC ON is never shorter. */
        TEST_ASSERT_TRUE(comms_ttc_padded_len(c, true) >=
                         comms_ttc_padded_len(c, false));
    }
}

/* ---- Reed-Solomon parity known-answer vectors ---- */

/* KAT provenance (docs/api/ttc-frame.md §2): the parity below is produced
   bit-for-bit by BOTH (a) python `reedsolo` RSCodec(6) with its defaults
   (fcr=0, prim=0x11D, generator=2 => a shortened RS(255,249) code) and
   (b) an independent GF(256) polynomial-division encoder written for this
   review. Two implementations agreeing is what makes the vector a KAT
   rather than a snapshot of one encoder. */
void test_rs_ecc_known_answer_literals(void)
{
    uint8_t parity[6];
    const uint8_t kat_zero[6] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
    const uint8_t kat_a[6]    = { 0x0EU, 0x7BU, 0xA9U, 0x55U, 0xD2U, 0x5AU };
    const uint8_t kat_c[6]    = { 0x04U, 0x04U, 0x19U, 0xD6U, 0x2AU, 0xE4U };

    /* Block of ten zero data bytes -> the all-zero codeword (reedsolo: 00*6). */
    const uint8_t data_zero[10] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(data_zero, sizeof(data_zero), parity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat_zero, parity, 6U);

    /* 10 data bytes 00..09 -> 6 parity symbols: this is the RS(16,10) codeword
       that fills one 16-byte interleaving block. */
    const uint8_t data_a[10] = { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U };
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(data_a, sizeof(data_a), parity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat_a, parity, 6U);

    /* 10 data bytes 10..19 -> 6 parity symbols (a second block vector). */
    uint8_t data_c[10];
    for (size_t i = 0U; i < sizeof(data_c); i++) {
        data_c[i] = (uint8_t)(10U + i);
    }
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(data_c, sizeof(data_c), parity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat_c, parity, 6U);

    /* The encoder bound is COMMS_TTC_RS_MAX_DATA (249), so a longer message is
       still legal; this vector cross-checks the generator against reedsolo on
       a multi-symbol input. */
    uint8_t       data_b[16];
    const uint8_t kat_b[6] = { 0x19U, 0xC6U, 0x88U, 0x16U, 0xC8U, 0x89U };
    for (size_t i = 0U; i < sizeof(data_b); i++) {
        data_b[i] = (uint8_t)i;
    }
    comms_ttc_rs_ecc_encode(data_b, sizeof(data_b), parity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat_b, parity, 6U);

    /* Linearity check: the all-zero codeword has all-zero parity (a real
       RS encoder must return this, and it proves the tail is computed, not
       left at the 0xEE poison). */
    uint8_t zeros[32];
    memset(zeros, 0, sizeof(zeros));
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(zeros, sizeof(zeros), parity);
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00U, parity[i]);
    }

    /* Defensive contract: NULL data / oversize len -> parity zeroed, no
       uninitialised bytes. */
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(NULL, 10U, parity);
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00U, parity[i]);
    }
    memset(parity, 0xEEU, sizeof(parity));
    comms_ttc_rs_ecc_encode(data_a, sizeof(data_a) + 260U, parity);
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00U, parity[i]);
    }

    /* NULL parity pointer: must return without touching memory. */
    comms_ttc_rs_ecc_encode(data_a, sizeof(data_a), NULL);
}

/* ---- TX build: literal byte layout ---- */

void test_ttc_build_frame_literal_bytes_no_ecc(void)
{
    comms_ttc_info_t info;
    const uint8_t    mac[4]     = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    const uint8_t    payload[5] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U };
    size_t           n          = 0U;

    ttc_info_init(&info, 42U, 0x55U, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info,
                              0x11223344UL, mac, payload, sizeof(payload), &n));

    TEST_ASSERT_EQUAL_size_t(32U, n);              /* 17 data B -> 32 */
    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);

    TEST_ASSERT_EQUAL_HEX8(0x2AU, ttc_buf[0]);     /* station id 42        */
    TEST_ASSERT_EQUAL_HEX8(0x55U, ttc_buf[1]);     /* ECC off              */
    TEST_ASSERT_EQUAL_HEX8(0x11U, ttc_buf[2]);     /* HK << 6 | task 0x11  */
    TEST_ASSERT_EQUAL_HEX8(0x05U, ttc_buf[3]);     /* PL length            */
    TEST_ASSERT_EQUAL_HEX8(0x11U, ttc_buf[4]);     /* UNIX time BE         */
    TEST_ASSERT_EQUAL_HEX8(0x22U, ttc_buf[5]);
    TEST_ASSERT_EQUAL_HEX8(0x33U, ttc_buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0x44U, ttc_buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0xDEU, ttc_buf[8]);     /* MAC opaque, verbatim */
    TEST_ASSERT_EQUAL_HEX8(0xADU, ttc_buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0xBEU, ttc_buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0xEFU, ttc_buf[11]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &ttc_buf[12], 5U);

    /* Zero padding from the end of the payload to the block boundary. */
    for (size_t i = 17U; i < 32U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[i]);
    }
}

void test_ttc_build_frame_null_mac_is_zeroed(void)
{
    comms_ttc_info_t info;
    const uint8_t    payload[5] = { 1U, 2U, 3U, 4U, 5U };
    size_t           n          = 0U;

    ttc_info_init(&info, 9U, 0x55U, 1U, 0x08U, 5U);   /* DAQ */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, sizeof(payload), &n));

    TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[11]);
    /* DAQ Bin ID 01 << 6 | task 0x08 = 0x48 ('Task types'!C5=01). */
    TEST_ASSERT_EQUAL_HEX8(0x48U, ttc_buf[2]);
}

/* ---- ECC block rule: 16*n with the parity INSIDE the blocks ---- */

/* On-air offset of content byte @p i of an ECC-ON frame: block (i / 10), slot
   (i % 10). The 6 parity bytes of every block occupy slots 10..15. Literals on
   purpose (see the section header: tests never reuse the macros). */
static size_t ttc_ecc_onair(size_t i)
{
    return ((i / 10U) * 16U) + (i % 10U);
}

/* 'Packet structure'!H54 fixes the RS PARITY element at 6 bytes; !A14 requires
   the packet to be 16*n ("correct interleaving"). Both hold together when the
   parity rides INSIDE each 16-byte block: the block is a systematic RS(16,10)
   codeword, so the frame is STILL 16*n - never 16*n + 6.
   KAT provenance: python-reedsolo RSCodec(6) with its defaults (fcr=0,
   prim=0x11D, generator=2) and an independent GF(256) encoder; both agree
   byte-for-byte on this whole frame (docs/api/ttc-frame.md). */
void test_ttc_build_frame_ecc_on_keeps_16n_blocks(void)
{
    comms_ttc_info_t info;
    const uint8_t    mac[4]     = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    const uint8_t    payload[5] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U };
    const uint8_t    kat[32]    = {
        /* content: 2A AA 11 05 11 22 33 44 DE AD | BE EF 01 02 03 04 05 + 3 pad */
        0x2AU, 0xAAU, 0x11U, 0x05U, 0x11U, 0x22U, 0x33U, 0x44U, 0xDEU, 0xADU,
        0xB6U, 0xF8U, 0x52U, 0x10U, 0x1EU, 0xB1U,   /* block 0 parity */
        0xBEU, 0xEFU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x00U, 0x00U, 0x00U,
        0x3AU, 0xC8U, 0x17U, 0x76U, 0x04U, 0xC7U    /* block 1 parity */
    };
    size_t n = 0U;

    ttc_info_init(&info, 42U, 0xAAU, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info,
                              0x11223344UL, mac, payload, sizeof(payload), &n));

    TEST_ASSERT_EQUAL_size_t(32U, n);          /* 17 content B -> 2 blocks */
    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);     /* the 16*n rule, ECC ON    */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat, ttc_buf, 32U);

    /* The three tail slots of block 1 are the mandated zero padding
       ('Packet structure'!J44/J50): content bytes 17..19 -> on-air 23..25. */
    TEST_ASSERT_EQUAL_UINT8(0x00U, ttc_buf[ttc_ecc_onair(17U)]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, ttc_buf[ttc_ecc_onair(18U)]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, ttc_buf[ttc_ecc_onair(19U)]);

    /* The same content with ECC OFF is also 16*n: the parity costs DATA
       capacity (10 of 16 bytes per block), it is not an appended tail. */
    ttc_info_init(&info, 42U, 0x55U, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info,
                              0x11223344UL, mac, payload, sizeof(payload), &n));
    TEST_ASSERT_EQUAL_size_t(32U, n);
    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);
    for (size_t i = 17U; i < 32U; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00U, ttc_buf[i]);   /* zero padding, ECC OFF */
    }
}

/* Header-only ECC-ON frame: 12 content bytes -> ceil(12/10) = 2 blocks -> 32 B
   (not 22). block 0 = 01 AA 01 00 00 00 00 00 00 00 -> parity 82 BB 7F 0F B5 56
   (station 1, HK|OBC reboot, PL 0, unix 0, zero MAC); block 1 is ten zero data
   bytes -> all-zero parity. */
void test_ttc_build_frame_ecc_header_only_matches_kat(void)
{
    const uint8_t kat[32] = {
        0x01U, 0xAAU, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x82U, 0xBBU, 0x7FU, 0x0FU, 0xB5U, 0x56U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    size_t n = ttc_build_header_only(0xAAU);

    TEST_ASSERT_EQUAL_size_t(32U, n);
    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kat, ttc_buf, 32U);
}

void test_ttc_build_frame_zero_payload_aligns_to_blocks(void)
{
    TEST_ASSERT_EQUAL_size_t(16U, ttc_build_header_only(0x55U));  /* 1 block  */
    TEST_ASSERT_EQUAL_size_t(32U, ttc_build_header_only(0xAAU));  /* 2 blocks */
}

/* ---- RX parse: round trip ---- */

void test_ttc_build_parse_round_trip(void)
{
    comms_ttc_info_t  info;
    comms_ttc_frame_t f;
    const uint8_t     mac[4]     = { 0xA0U, 0xA1U, 0xA2U, 0xA3U };
    const uint8_t     payload[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    size_t            n          = 0U;

    ttc_info_init(&info, 77U, 0x55U, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info,
                              0xDEADBEEFUL, mac, payload, sizeof(payload), &n));

    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK, comms_ttc_parse_frame(ttc_buf, n, &f));
    TEST_ASSERT_EQUAL_UINT8(77U, f.info.station_id);
    TEST_ASSERT_EQUAL_HEX8(0x55U, f.info.ecc_flag);
    TEST_ASSERT_EQUAL_UINT8(0U, f.info.tec_type);
    TEST_ASSERT_EQUAL_UINT8(0x11U, f.info.tec_task);
    TEST_ASSERT_EQUAL_UINT8(5U, f.info.pl_len);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFUL, f.unix_time);
    TEST_ASSERT_EQUAL_PTR(&ttc_buf[8], f.mac);          /* opaque, aliased */
    TEST_ASSERT_EQUAL_PTR(&ttc_buf[12], f.payload);
    TEST_ASSERT_EQUAL_size_t(32U, f.frame_len);
    TEST_ASSERT_EQUAL_size_t(32U, f.data_len);
    TEST_ASSERT_EQUAL_HEX8(0xA0U, f.mac[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA3U, f.mac[3]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, 5U);
}

/* ECC-ON round trip: the header and payload are de-interleaved out of the
   16-byte blocks (10 data + 6 parity each), and data_len reports the DATA
   capacity the blocks carry (2 blocks -> 20 B), not the on-air total. */
void test_ttc_parse_ecc_on_frame_deinterleaves(void)
{
    comms_ttc_frame_t f;
    const uint8_t     payload[5] = { 1U, 2U, 3U, 4U, 5U };
    const uint8_t     mac[4]     = { 0xA0U, 0xA1U, 0xA2U, 0xA3U };
    comms_ttc_info_t  info;
    size_t            n = 0U;

    ttc_info_init(&info, 4U, 0xAAU, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, mac,
                              payload, sizeof(payload), &n));

    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);
    TEST_ASSERT_EQUAL_size_t(32U, n);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK, comms_ttc_parse_frame(ttc_buf, n, &f));
    TEST_ASSERT_EQUAL_size_t(32U, f.frame_len);
    TEST_ASSERT_EQUAL_size_t(20U, f.data_len);          /* 2 blocks x 10 data */
    TEST_ASSERT_EQUAL_UINT8(4U, f.info.station_id);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, f.info.ecc_flag);
    TEST_ASSERT_EQUAL_UINT8(5U, f.info.pl_len);
    TEST_ASSERT_NOT_NULL(f.mac);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mac, f.mac, 4U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, 5U);
}

/* ---- RX parse: rejection classes ---- */

void test_ttc_parse_rejects_null_and_runt_and_oversize(void)
{
    comms_ttc_frame_t f;
    size_t            n = ttc_build_header_only(0x55U);

    TEST_ASSERT_EQUAL_size_t(16U, n);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_NULL, comms_ttc_parse_frame(NULL, n, &f));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_NULL, comms_ttc_parse_frame(ttc_buf, n, NULL));

    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TOO_SHORT, comms_ttc_parse_frame(ttc_buf, 0U, &f));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TOO_SHORT, comms_ttc_parse_frame(ttc_buf, 15U, &f));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TOO_LONG, comms_ttc_parse_frame(ttc_buf, 208U, &f));

    /* 17 is within [16,128] but not an interleaving block count. */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_ALIGN, comms_ttc_parse_frame(ttc_buf, 17U, &f));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TOO_SHORT, comms_ttc_parse_frame(ttc_buf, 1U, &f));
}

/* Canonical length: the frame length must equal exactly
   comms_ttc_padded_len(HDR + PL, ecc). A frame that declares 5 payload bytes
   but is only one 16-byte block long is non-canonical (comms.c:319 accepted
   it before this fix) and must be rejected, not silently truncated. */
void test_ttc_parse_rejects_noncanonical_length(void)
{
    comms_ttc_info_t  info;
    comms_ttc_frame_t f;
    const uint8_t     payload[5] = { 1U, 2U, 3U, 4U, 5U };
    size_t            n          = 0U;

    ttc_info_init(&info, 8U, 0x55U, 0U, 0x11U, 5U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, sizeof(payload), &n));
    TEST_ASSERT_EQUAL_size_t(32U, n);

    /* Declares PL 5 (canonical 32 B) but handed as 16 B. */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_LEN_MISMATCH,
                          comms_ttc_parse_frame(ttc_buf, 16U, &f));
    /* The canonical length parses. */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK, comms_ttc_parse_frame(ttc_buf, 32U, &f));

    /* Header-only frame (PL 0, canonical 16 B) padded out to 32 B: the extra
       block is not a legal canonical length either. */
    size_t n0 = ttc_build_header_only(0x55U);
    TEST_ASSERT_EQUAL_size_t(16U, n0);
    memset(&ttc_buf[n0], 0, 16U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_LEN_MISMATCH,
                          comms_ttc_parse_frame(ttc_buf, 32U, &f));

    /* Same rule with ECC: a header-only ECC frame is 32 B (two RS(16,10)
       blocks - the 12 content bytes do not fit one 10-byte data field). 16 B
       with the ECC flag set is 16-aligned but not canonical, and neither is
       the 48 B over-pad. */
    n0 = ttc_build_header_only(0xAAU);
    TEST_ASSERT_EQUAL_size_t(32U, n0);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_LEN_MISMATCH,
                          comms_ttc_parse_frame(ttc_buf, 16U, &f));
    memset(&ttc_buf[n0], 0, 16U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_LEN_MISMATCH,
                          comms_ttc_parse_frame(ttc_buf, 48U, &f));
}

void test_ttc_parse_rejects_pl_len_field_beyond_max(void)
{
    comms_ttc_frame_t f;
    size_t            n = ttc_build_header_only(0x55U);

    TEST_ASSERT_EQUAL_size_t(16U, n);
    ttc_buf[3] = 101U;                       /* one over the 0..100 field */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_PL_LEN, comms_ttc_parse_frame(ttc_buf, n, &f));

    ttc_buf[3] = 255U;
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_PL_LEN, comms_ttc_parse_frame(ttc_buf, n, &f));
}

void test_ttc_parse_rejects_bad_info_fields(void)
{
    comms_ttc_frame_t f;
    size_t            n = ttc_build_header_only(0x55U);

    ttc_buf[0] = 0U;   /* station id 0 is not legal */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_STATION_ID, comms_ttc_parse_frame(ttc_buf, n, &f));

    ttc_buf[0] = 1U;
    ttc_buf[1] = 0x00U; /* neither 0x55 nor 0xAA */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_ECC_FLAG, comms_ttc_parse_frame(ttc_buf, n, &f));
}

/* ---- TX validation ---- */

void test_ttc_build_rejects_pl_len_beyond_max(void)
{
    comms_ttc_info_t info;
    uint8_t          payload[101];
    size_t           n = 0U;

    memset(payload, 0xA5U, sizeof(payload));
    ttc_info_init(&info, 1U, 0x55U, 1U, 0x01U, 101U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_PL_LEN,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, 101U, &n));

    /* INFO and the payload length argument must agree. */
    ttc_info_init(&info, 1U, 0x55U, 1U, 0x01U, 101U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_PL_LEN,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, 100U, &n));
}

void test_ttc_build_rejects_bad_info_and_null(void)
{
    comms_ttc_info_t info;
    size_t           n = 0U;

    ttc_info_init(&info, 1U, 0x55U, 1U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_NULL,
        comms_ttc_build_frame(NULL, 32U, &info, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_NULL,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), NULL, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_NULL,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 3U, &n));

    ttc_info_init(&info, 0U, 0x55U, 1U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_STATION_ID,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));

    ttc_info_init(&info, 1U, 0x11U, 1U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_ECC_FLAG,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));

    ttc_info_init(&info, 1U, 0x55U, 4U, 0x01U, 0U);   /* type field is 2 bits */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TEC_TYPE,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));

    ttc_info_init(&info, 1U, 0x55U, 1U, 64U, 0U);     /* task field is 6 bits */
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_TEC_TASK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));
}

void test_ttc_build_reports_needed_length_when_buffer_too_small(void)
{
    comms_ttc_info_t info;
    uint8_t          small[8];
    size_t           n = 0U;

    ttc_info_init(&info, 1U, 0x55U, 1U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_ERR_BUF,
        comms_ttc_build_frame(small, sizeof(small), &info, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_size_t(16U, n);      /* the caller learns the size to alloc */
}

/* ---- layout discriminator ---- */

void test_ttc_layout_discriminator(void)
{
    size_t n;

    n = ttc_build_header_only(0x55U);
    TEST_ASSERT_TRUE(comms_frame_is_ttc_layout(ttc_buf, n));
    n = ttc_build_header_only(0xAAU);
    TEST_ASSERT_TRUE(comms_frame_is_ttc_layout(ttc_buf, n));

    /* Legacy CRC-only frame: byte 1 is its payload length, never 0x55/0xAA. */
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    n = build_frame(COMMS_TC_SEND_DATA, payload, 8U);
    TEST_ASSERT_FALSE(comms_frame_is_ttc_layout(frame_buf, n));

    /* Authenticated frame is 16 B but byte 1 = 8 (payload length). */
    uint8_t auth_payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    n = build_auth_frame(COMMS_TC_SEND_DATA, auth_payload, 8U);
    TEST_ASSERT_EQUAL_size_t(16U, n);
    TEST_ASSERT_FALSE(comms_frame_is_ttc_layout(frame_buf, n));

    TEST_ASSERT_FALSE(comms_frame_is_ttc_layout(NULL, 16U));
    TEST_ASSERT_FALSE(comms_frame_is_ttc_layout(frame_buf, 0U));
}

/* ---- result strings and mapping ---- */

void test_ttc_result_strings_are_never_null(void)
{
    for (int r = COMMS_TTC_OK; r <= COMMS_TTC_ERR_BUF; r++) {
        TEST_ASSERT_NOT_NULL(comms_ttc_result_str((comms_ttc_result_t)r));
    }
    TEST_ASSERT_EQUAL_STRING("ALIGN", comms_ttc_result_str(COMMS_TTC_ERR_ALIGN));
    TEST_ASSERT_EQUAL_STRING("LEN_MISMATCH", comms_ttc_result_str(COMMS_TTC_ERR_LEN_MISMATCH));
    TEST_ASSERT_EQUAL_STRING("UNSUPPORTED", comms_ttc_result_str(COMMS_TTC_ERR_UNSUPPORTED));
    TEST_ASSERT_EQUAL_STRING("PAYLOAD", comms_ttc_result_str(COMMS_TTC_ERR_PAYLOAD));
    TEST_ASSERT_EQUAL_STRING("MAC", comms_ttc_result_str(COMMS_TTC_ERR_MAC));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", comms_ttc_result_str((comms_ttc_result_t)999));

    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_ttc_to_tc_result(COMMS_TTC_OK));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_ttc_to_tc_result(COMMS_TTC_ERR_MAC));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_STATION_ID));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_PL_LEN));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_PAYLOAD));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_UNSUPPORTED));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_ALIGN));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH,
                          comms_ttc_to_tc_result(COMMS_TTC_ERR_LEN_MISMATCH));
}

/* ---- MAC seam ---- */

static int     ttc_mac_seen_calls;
static int     ttc_mac_seen_accept;
static uint8_t ttc_mac_seen_bytes[4];

static bool ttc_mac_recording(const uint8_t *frame, size_t len,
                              const uint8_t mac[4])
{
    (void)len;
    ttc_mac_seen_calls++;
    /* On an ECC-OFF frame the MAC pointer must alias the 4 bytes at offset 8
       of the on-air buffer (no copy anywhere on that path). */
    TEST_ASSERT_EQUAL_PTR(&frame[8], mac);
    return ttc_mac_seen_accept != 0;
}

/* Same recorder, but it captures the 4 MAC bytes instead of asserting the
   alias: an ECC-ON frame interleaves the MAC with parity, so the seam is
   handed the de-interleaved copy, not a pointer into the frame. */
static bool ttc_mac_recording_capture(const uint8_t *frame, size_t len,
                                      const uint8_t mac[4])
{
    (void)frame;
    (void)len;
    ttc_mac_seen_calls++;
    memcpy(ttc_mac_seen_bytes, mac, 4U);
    return ttc_mac_seen_accept != 0;
}

void test_ttc_mac_seam_is_fail_closed_by_default(void)
{
    const uint8_t mac[4] = { 1U, 2U, 3U, 4U };

    /* No verifier installed -> every frame is rejected (fail closed). */
    TEST_ASSERT_FALSE(comms_ttc_mac_verify(ttc_buf, 16U, mac));

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;
    /* mac must alias the frame bytes at offset 8 (asserted in the callback). */
    TEST_ASSERT_TRUE(comms_ttc_mac_verify(ttc_buf, 16U, &ttc_buf[8]));
    TEST_ASSERT_EQUAL_INT(1, ttc_mac_seen_calls);

    ttc_mac_seen_accept = 0;
    TEST_ASSERT_FALSE(comms_ttc_mac_verify(ttc_buf, 16U, &ttc_buf[8]));
}

/* ---- RX entry: parse, account, dispatch ---- */

/* With no MAC verifier the frame is parsed but NEVER dispatched (no
   NVIC/state-machine expectations queued -> CMock fails if it is). */
void test_rx_ttc_fails_closed_without_mac_verifier(void)
{
    comms_rx_stats_t before, after;
    size_t           n = ttc_build_header_only(0x55U);

    comms_rx_get_stats(&before);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_ttc_frame(ttc_buf, n));
    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 1U, after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_mac + 1U, after.rejected_mac);
}

/* The legacy entry point routes a TT&C-layout frame to the TT&C path: same
   fail-closed verdict, never to the opcode validator. */
void test_rx_gate_routes_ttc_layout_frames(void)
{
    size_t n = ttc_build_header_only(0x55U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(ttc_buf, n));
}

/* A valid MAC and a valid frame reach the dispatcher: TEC HK task 0x01 is
   OBC reboot and must actually reset. */
void test_rx_ttc_dispatches_obc_reboot(void)
{
    comms_ttc_info_t info;
    size_t           n = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    ttc_info_init(&info, 5U, 0x55U, 0U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));

    const uint32_t resets_before = host_nvic_reset_count();
    HOST_EXPECT_NVIC_RESET(comms_rx_handle_ttc_frame(ttc_buf, n));
    TEST_ASSERT_EQUAL_UINT32(resets_before + 1U, host_nvic_reset_count());
    TEST_ASSERT_EQUAL_INT(1, ttc_mac_seen_calls);
}

/* OBC reboot declares PL = 0 ('Task details'!G5); a frame that smuggles a
   payload is rejected as incoherent and must NOT reset. */
void test_rx_ttc_obc_reboot_rejects_payload(void)
{
    comms_ttc_info_t info;
    const uint8_t    payload[1] = { 0xFFU };
    size_t           n          = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    ttc_info_init(&info, 5U, 0x55U, 0U, 0x01U, 1U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, sizeof(payload), &n));

    /* No HOST_EXPECT_NVIC_RESET armed: a reset here would fail the run. */
    const uint32_t resets_before = host_nvic_reset_count();
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));
    TEST_ASSERT_EQUAL_UINT32(resets_before, host_nvic_reset_count());
}

/* Exit state ('HK tasks'!D13:D14 — payload byte 1 "State old", byte 2 "State
   new"): the handler must request the NEW state carried in the payload, not a
   hardcoded STATE_READY (the comms.c:407 defect). */
void test_rx_ttc_dispatches_exit_state_new_state(void)
{
    comms_ttc_info_t info;
    const uint8_t    payload[2] = { 3U, 4U };   /* STATE_READY -> STATE_ACTIVE */
    size_t           n          = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    ttc_info_init(&info, 5U, 0x55U, 0U, 0x02U, 2U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL,
                              payload, sizeof(payload), &n));

    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_ttc_frame(ttc_buf, n));
}

/* End-to-end on an ECC-ON frame: the payload is de-interleaved out of the
   16-byte blocks before dispatch, so Exit state still applies the REQUESTED
   new state, and the MAC seam sees the de-interleaved 4 MAC bytes. */
void test_rx_ttc_ecc_on_frame_dispatches_exit_state(void)
{
    comms_ttc_info_t info;
    const uint8_t    payload[2] = { 3U, 4U };   /* STATE_READY -> STATE_ACTIVE */
    const uint8_t    mac[4]     = { 0xA0U, 0xA1U, 0xA2U, 0xA3U };
    size_t           n          = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording_capture);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;
    memset(ttc_mac_seen_bytes, 0, sizeof(ttc_mac_seen_bytes));

    ttc_info_init(&info, 5U, 0xAAU, 0U, 0x02U, 2U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, mac,
                              payload, sizeof(payload), &n));
    TEST_ASSERT_EQUAL_size_t(0U, n % 16U);

    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_ttc_frame(ttc_buf, n));
    TEST_ASSERT_EQUAL_INT(1, ttc_mac_seen_calls);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mac, ttc_mac_seen_bytes, 4U);
}

/* Exit-state payload validation (comms.c:407): empty / 1-byte payloads, an
   out-of-range old or new state, and a no-op old == new pair are all rejected
   — never dispatched, never counted accepted. */
void test_rx_ttc_exit_state_rejects_bad_payload(void)
{
    comms_rx_stats_t before, after;
    comms_ttc_info_t info;
    size_t           n = 0U;
    const uint8_t    good[2]    = { 3U, 4U };
    const uint8_t    bad_new[2] = { 3U, 9U };   /* new state 9 > STATE_ACTIVE */
    const uint8_t    bad_old[2] = { 7U, 4U };   /* old state 7 > STATE_ACTIVE */
    const uint8_t    same[2]    = { 3U, 3U };   /* no transition requested   */

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    comms_rx_get_stats(&before);

    /* Empty payload: too short. */
    ttc_info_init(&info, 5U, 0x55U, 0U, 0x02U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));

    /* 1-byte payload: too short. */
    ttc_info_init(&info, 5U, 0x55U, 0U, 0x02U, 1U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, good, 1U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));

    /* 2-byte payloads that are out of range or inconsistent. */
    ttc_info_init(&info, 5U, 0x55U, 0U, 0x02U, 2U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, bad_new, 2U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));

    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, bad_old, 2U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));

    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, same, 2U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PAYLOAD_LEN, comms_rx_handle_ttc_frame(ttc_buf, n));

    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 5U, after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_range + 5U, after.rejected_range);
}

/* Unsupported commands must be REJECTED, not counted as accepted (the
   comms.c:412 defect). A non-HK TEC type and an unimplemented HK task both
   return COMMS_TC_ERR_OPCODE and take no action. */
void test_rx_ttc_unsupported_type_and_task_are_rejected(void)
{
    comms_rx_stats_t before, after;
    comms_ttc_info_t info;
    size_t           n = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    comms_rx_get_stats(&before);

    /* DAQ (Bin ID 01, 'Task types'!C5) is not a command carrier. */
    ttc_info_init(&info, 5U, 0x55U, 1U, 0x01U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, comms_rx_handle_ttc_frame(ttc_buf, n));

    /* HK task 0x11 (TLE, 'Task details'!E21) is not implemented here. */
    ttc_info_init(&info, 5U, 0x55U, 0U, 0x11U, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0UL, NULL, NULL, 0U, &n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, comms_rx_handle_ttc_frame(ttc_buf, n));

    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 2U, after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_opcode + 2U, after.rejected_opcode);
}

/* Freshness / anti-replay is deliberately NOT implemented (system-level
   decision; open point O2 in docs/api/ttc-frame.md). The UNIX time is parsed
   but never compared, so the SAME frame is dispatched twice. This test PINS
   the current behaviour and must be replaced when replay protection lands —
   it is the executable record of the gap. */
void test_rx_ttc_replay_is_accepted_today(void)
{
    comms_rx_stats_t before, after;
    comms_ttc_info_t info;
    const uint8_t    payload[2] = { 3U, 4U };
    size_t           n          = 0U;

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_calls  = 0;
    ttc_mac_seen_accept = 1;

    ttc_info_init(&info, 5U, 0x55U, 0U, 0x02U, 2U);
    TEST_ASSERT_EQUAL_INT(COMMS_TTC_OK,
        comms_ttc_build_frame(ttc_buf, sizeof(ttc_buf), &info, 0x12345678UL, NULL,
                              payload, sizeof(payload), &n));

    comms_rx_get_stats(&before);
    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_ttc_frame(ttc_buf, n));
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_ttc_frame(ttc_buf, n));
    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 2U, after.accepted);
}

/* Rejected TT&C frames are accounted in the existing counter classes. */
void test_rx_ttc_accounts_rejections(void)
{
    comms_rx_stats_t before, after;
    size_t           n = ttc_build_header_only(0x55U);

    comms_ttc_set_mac_verifier(ttc_mac_recording);
    ttc_mac_seen_accept = 0;

    comms_rx_get_stats(&before);
    /* MAC rejection (verifier rejects). */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_ttc_frame(ttc_buf, n));
    /* Structural rejection: not a block count. */
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH,
                          comms_rx_handle_ttc_frame(ttc_buf, 17U));
    /* Bad INFO field: parameter range. */
    ttc_buf[0] = 0U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE,
                          comms_rx_handle_ttc_frame(ttc_buf, n));
    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 3U, after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_mac + 1U, after.rejected_mac);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_malformed + 1U, after.rejected_malformed);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_range + 1U, after.rejected_range);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
}
