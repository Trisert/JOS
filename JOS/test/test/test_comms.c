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
#include "host_support.h"      /* HOST_EXPECT_NVIC_RESET()                    */
#include "mock_state_machine.h"
#include "mock_watchdog.h"     /* also pulls watchdog.h for the WDG_PERIOD_*  */
#include "mock_cmsis_os.h"

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

/* A valid RESET telecommand must actually reboot the OBC. The frame is
   accounted as accepted *before* the dispatcher runs, so the reset counter and
   the RX statistics are both checked: the long jump out of NVIC_SystemReset()
   means comms_rx_handle_frame() never returns, exactly as on target. */
void test_rx_gate_dispatches_reset(void)
{
    comms_rx_stats_t before, after;
    comms_rx_get_stats(&before);

    const uint32_t resets_before = host_nvic_reset_count();
    size_t         n             = build_frame(COMMS_TC_RESET, NULL, 0U);

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
    size_t n = build_frame(COMMS_TC_SET_BEACON_INTERVAL, p, 4U);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_rx_gate_dispatches_zero_beacon_interval_escape(void)
{
    uint8_t p[4];
    put_be32(p, 0UL);
    state_machine_set_beacon_interval_ExpectAndReturn(0UL, 0);
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

/* lora_send_chunked() stages the payload through the SRAM2 TX buffer in
   chunk_max-sized pieces. The radio itself is still a stub, so what is
   verified is the bounds contract: a NULL buffer with a non-zero length is
   refused, and a payload larger than one chunk is walked without running off
   the end of the staging buffer (the last chunk must be the remainder). */
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
    TEST_ASSERT_GREATER_THAN_size_t(0U, chunk_max);

    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }

    /* One-and-a-bit chunks: exercises both the full-chunk and the remainder
       iteration of the staging loop. */
    const size_t len = chunk_max + 7U;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(payload), len);
    TEST_ASSERT_EQUAL_INT(0, lora_send_chunked(payload, len));

    /* The buffer holds the LAST chunk staged, i.e. the 7-byte remainder. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[chunk_max], tx, 7);
}

/* ================= task loops ================= */

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

/* Run `task` until the third osDelay() and assert it never returned. */
static void run_task_iterations(void (*task)(void *))
{
    delay_calls = 0;
    osDelay_Stub(osDelay_escape_cb);

    if (setjmp(loop_escape) == 0) {
        task(NULL);
        TEST_FAIL_MESSAGE("RTOS task loop returned - it must not exit");
    }
    TEST_ASSERT_EQUAL_INT(3, delay_calls);
}

/* lora_rx_task() now signals liveness every iteration, mirroring the other
 * monitored tasks. The loop runs three iterations before the escape stub
 * longjmps out, so queue three expectations. lora_rx_task_create() also calls
 * osThreadGetId() once to register the RX task handle with the driver ISR
 * hook (B3), and each iteration blocks on osThreadFlagsWait() (also B3), so
 * expect those calls at the start / per iteration. */
void test_lora_rx_task_loop_delays_at_the_registered_period(void)
{
    osThreadGetId_ExpectAndReturn(RX_TH);
    osThreadFlagsWait_ExpectAndReturn(LORA_RX_FLAG, LORA_RX_FLAG);
    osThreadFlagsWait_ExpectAndReturn(LORA_RX_FLAG, LORA_RX_FLAG);
    osThreadFlagsWait_ExpectAndReturn(LORA_RX_FLAG, LORA_RX_FLAG);
    osDelay_Stub(osDelay_escape_cb);
    watchdog_alive_self_Expect();
    watchdog_alive_self_Expect();
    watchdog_alive_self_Expect();
    run_task_iterations(lora_rx_task);
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
