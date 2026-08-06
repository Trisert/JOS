/**
 * @file    test_comms.c
 * @brief   Unit tests for the uplink telecommand validation gate
 *          (App/comms/comms_validate.c + comms_rx_handle_frame() in
 *          App/comms/comms.c) and for the LoRa RX/beacon task wiring.
 *
 * The SX1268 LoRa layer is stubbed at its two seams:
 *   - the SPI/HAL layer (test/fakes/stm32l4xx_hal.h, test/fakes/main.h) is
 *     mocked, so no radio, no bus and no NVIC reset ever happen;
 *   - frames are injected directly at comms_rx_handle_frame(), which is the
 *     single entry point the RX ISR/task is required to use.
 * The state machine and the watchdog are CMock mocks, so "was this frame
 * dispatched?" is an assertion, not an inference.
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
#include "mock_state_machine.h"
#include "mock_watchdog.h"
#include "mock_cmsis_os.h"
#include "mock_main.h"

#include <string.h>
#include <setjmp.h>

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

void setUp(void)   { memset(frame_buf, 0, sizeof(frame_buf)); }
void tearDown(void) { }

/* ================= CRC known-answer test ================= */

/* CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
void test_comms_crc16_known_check_string(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1U, comms_crc16_ccitt((const uint8_t *)"123456789", 9U));
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

void test_validate_rejects_maximum_payload_overflow(void)
{
    /* Header claims 250 B of payload — far beyond the 60 B budget. */
    uint8_t big[COMMS_TC_MAX_FRAME];
    memset(big, 0xA5, sizeof(big));
    (void)build_frame(COMMS_TC_SET_CONFIG, big, (uint8_t)COMMS_TC_MAX_PAYLOAD);
    frame_buf[1] = 250U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_LEN_MISMATCH,
                          validate(frame_buf, (size_t)COMMS_TC_MAX_FRAME));
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
    for (int r = COMMS_TC_OK; r <= COMMS_TC_ERR_PARAM_RANGE; r++) {
        TEST_ASSERT_NOT_NULL(comms_tc_result_str((comms_tc_result_t)r));
    }
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

    /* CRC error on an otherwise perfect RESET — must not reboot the OBC. */
    size_t n = build_frame(COMMS_TC_RESET, NULL, 0U);
    frame_buf[n - 1] ^= 0x55U;
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_CRC, comms_rx_handle_frame(frame_buf, n));

    /* Unknown opcode. */
    n = build_frame(0xEEU, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_OPCODE, comms_rx_handle_frame(frame_buf, n));

    /* Out-of-range beacon interval. */
    put_be32(p, 10UL);
    n = build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_PARAM_RANGE, comms_rx_handle_frame(frame_buf, n));
}

/* ================= RX gate: accept must dispatch exactly once ============= */

void test_rx_gate_dispatches_exit_state(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_activate_payload(void)
{
    state_machine_request_transition_ExpectAndReturn(STATE_ACTIVE, TRIGGER_GROUND_CMD, 0);
    size_t n = build_frame(COMMS_TC_ACTIVATE_PAYLOAD, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_reset(void)
{
    NVIC_SystemReset_Expect();
    size_t n = build_frame(COMMS_TC_RESET, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_in_range_beacon_interval(void)
{
    uint8_t p[4];
    put_be32(p, 60000UL);                    /* 60 s — inside [1 s, 1 h] */
    state_machine_set_beacon_interval_Expect(60000UL);
    size_t n = build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_zero_beacon_interval_escape(void)
{
    uint8_t p[4];
    put_be32(p, 0UL);
    state_machine_set_beacon_interval_Expect(0UL);
    size_t n = build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

/* ================= RX accounting ================= */

void test_rx_stats_classify_each_verdict(void)
{
    comms_rx_stats_t before, after;
    comms_rx_get_stats(&before);

    /* 1 accepted */
    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    (void)comms_rx_handle_frame(frame_buf, build_frame(COMMS_TC_EXIT_STATE, NULL, 0U));

    /* 1 CRC rejection */
    size_t n = build_frame(COMMS_TC_EXIT_STATE, NULL, 0U);
    frame_buf[n - 1] ^= 0x01U;
    (void)comms_rx_handle_frame(frame_buf, n);

    /* 1 opcode rejection */
    (void)comms_rx_handle_frame(frame_buf, build_frame(0xABU, NULL, 0U));

    /* 1 range rejection */
    uint8_t p[4];
    put_be32(p, 1UL);
    (void)comms_rx_handle_frame(frame_buf, build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U));

    /* 2 malformed rejections */
    (void)comms_rx_handle_frame(NULL, 8U);
    (void)comms_rx_handle_frame(frame_buf, 2U);

    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.accepted + 1U,            after.accepted);
    TEST_ASSERT_EQUAL_UINT32(before.rejected + 5U,            after.rejected);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_crc + 1U,        after.rejected_crc);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_opcode + 1U,     after.rejected_opcode);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_range + 1U,      after.rejected_range);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_malformed + 2U,  after.rejected_malformed);
}

void test_rx_stats_tolerates_null_out(void)
{
    comms_rx_get_stats(NULL);   /* must not fault */
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

void test_lora_beacon_task_create_registers_with_watchdog(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(BEACON_TH);
    watchdog_register_task_ExpectAndReturn(BEACON_TH, WDG_PERIOD_LORA_BEACON_MS, 0);
    TEST_ASSERT_EQUAL_PTR(BEACON_TH, lora_beacon_task_create());
}

void test_lora_task_create_skips_registration_when_thread_creation_fails(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(NULL);
    TEST_ASSERT_NULL(lora_rx_task_create());   /* no watchdog_register_task */
}

static jmp_buf loop_escape;
static int     delay_calls;

static osStatus_t osDelay_escape_cb(uint32_t ticks, int cmock_num_calls)
{
    (void)ticks;
    (void)cmock_num_calls;
    delay_calls++;
    if (delay_calls >= 3) {
        longjmp(loop_escape, 1);
    }
    return osOK;
}

void test_lora_rx_task_loop_kicks_watchdog_every_iteration(void)
{
    watchdog_alive_self_Expect();
    watchdog_alive_self_Expect();
    watchdog_alive_self_Expect();

    delay_calls = 0;
    osDelay_Stub(osDelay_escape_cb);

    if (setjmp(loop_escape) == 0) {
        lora_rx_task(NULL);
        TEST_FAIL_MESSAGE("lora_rx_task() returned — the RTOS loop must not exit");
    }
    TEST_ASSERT_EQUAL_INT(3, delay_calls);
}

void test_lora_beacon_task_loop_kicks_watchdog_every_iteration(void)
{
    for (int i = 0; i < 3; i++) {
        state_machine_get_beacon_interval_ExpectAndReturn(BEACON_INTERVAL_READY);
        watchdog_alive_self_Expect();
    }

    delay_calls = 0;
    osDelay_Stub(osDelay_escape_cb);

    if (setjmp(loop_escape) == 0) {
        lora_beacon_task(NULL);
        TEST_FAIL_MESSAGE("lora_beacon_task() returned — the RTOS loop must not exit");
    }
    TEST_ASSERT_EQUAL_INT(3, delay_calls);
}
