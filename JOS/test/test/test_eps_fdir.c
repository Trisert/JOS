/**
 * @file    test_eps_fdir.c
 * @brief   Unit tests for the EPS charger / battery-monitor FDIR detectors
 *          (App/bms/eps_fdir.c, RED_FDIR_V2.xlsx sheet 'EPS').
 *
 * The module is pure decision logic: it is fed a state snapshot (charging
 * current and whether that I2C read completed, whether the battery monitor
 * answered, the current tick) and returns reset REQUESTS the caller executes.
 * The tests therefore drive it with synthetic snapshots and assert on the
 * requests; no HAL, no clock, no IC.
 *
 * Numeric literals are spelled out rather than expressed through the module's
 * own macros, so a test cannot pass just because the constant it is asserting
 * on was edited in step with it. The values pinned here are the ones written
 * in the document: 60 min = 3 600 000 ms and 10 s = 10 000 ms.
 *
 * What is verified
 *   - defaults: 60 min / 10 s from the sheet, heartbeat UNARMED (TBC).
 *   - 0 mA below the 60 min window -> no request; past it -> request.
 *   - the boundary is "MORE than 60 min" (exactly 60 min still does not fire).
 *   - charging current back above 0 restarts the window (counter reset).
 *   - a FAILED charger read is not "0 mA" and raises nothing.
 *   - battery monitor mute below 10 s -> no request; past it -> request;
 *     answering again clears the request and re-arms the window.
 *   - the tick counter wrapping through 0 is handled (unsigned modular
 *     difference), on both windows.
 *   - an unarmed channel never fires; the heartbeat channel fires only once a
 *     caller supplies the TBC window.
 *   - FDIR-EPS-EL-04 fires the NRST request at 1xTBC from the last heartbeat
 *     (one window, not two), and the "EPS never heard since boot" path has the
 *     SAME latency — the two paths of one channel must agree.
 *   - ack restarts the window instead of letting the request hammer the IC,
 *     and the request is a level: it stays set while the fault holds.
 *   - NULL / out-of-range inputs are rejected without side effects.
 */

#include "unity.h"

#include "eps_fdir.h"

#define T60MIN 3600000UL     /* 60 min, RED_FDIR_V2.xlsx 'EPS' FDIR-EPS-EL-03 */
#define T10S      10000UL    /* 10 s,   RED_FDIR_V2.xlsx 'EPS' FDIR-EPS-EL-05 */

/* FDIR-EPS-EL-04 window: the document writes "TBC ms from the last HB" and
 * gives no value, so the test supplies one. The assertions below are written
 * in terms of THIS literal, never the module's own macro, so the latency they
 * pin down is the one written here and not whatever the module happens to be
 * configured with: 1xTBC from the last heartbeat, not 2xTBC. */
#define THB 5000UL

static eps_fdir_t        ctx;
static eps_fdir_config_t cfg;

/* One periodic snapshot: no charger fault, monitor answering. */
static eps_fdir_requests_t step(uint32_t now_ms, int16_t ma, bool chg_valid, bool bm_ok)
{
    const eps_fdir_input_t in = {
        .now_ms                 = now_ms,
        .chg_current_ma         = ma,
        .chg_sample_valid       = chg_valid,
        .batt_monitor_responsive = bm_ok,
    };
    return eps_fdir_update(&ctx, &in);
}

void setUp(void)
{
    eps_fdir_config_default(&cfg);
    TEST_ASSERT_EQUAL_INT(0, eps_fdir_init(&ctx, &cfg));
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------- defaults */

void test_defaults_are_the_document_values(void)
{
    eps_fdir_config_t d = { 0 };

    eps_fdir_config_default(&d);

    /* Literals, not the module's macros: these are the numbers in the sheet. */
    TEST_ASSERT_EQUAL_UINT32(3600000UL, d.charger_timeout_ms);       /* 60 min */
    TEST_ASSERT_EQUAL_UINT32(10000UL,   d.batt_monitor_timeout_ms);  /* 10 s   */
    /* TBC in the document: unarmed, not guessed. */
    TEST_ASSERT_EQUAL_UINT32(0UL, d.eps_hb_timeout_ms);

    eps_fdir_config_default(NULL);   /* must not fault */
}

/* --------------------------------------------------- charger (EL-03) 0 mA */

void test_charger_zero_current_below_window_no_request(void)
{
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);   /* episode starts */

    TEST_ASSERT_FALSE(step(T60MIN - 1UL, 0, true, true).charger_reset);
}

void test_charger_zero_current_exactly_at_window_no_request(void)
{
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);

    /* The entry condition is "MORE than 60 min", so the window boundary
       itself is still inside the specification. */
    TEST_ASSERT_FALSE(step(T60MIN, 0, true, true).charger_reset);
}

void test_charger_zero_current_past_window_requests_reset(void)
{
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);

    TEST_ASSERT_TRUE(step(T60MIN + 1UL, 0, true, true).charger_reset);

    /* Level, not pulse: still asserted while the fault holds. */
    TEST_ASSERT_TRUE(step(T60MIN + 2UL, 0, true, true).charger_reset);
}

void test_charger_current_above_zero_resets_counter(void)
{
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);

    /* Charging again: the episode is over well before the window. */
    TEST_ASSERT_FALSE(step(T60MIN - 100UL, 250, true, true).charger_reset);

    /* A new 0 mA episode starts here, so the previous 59 min 59.9 s of
       silence must NOT count towards it. */
    TEST_ASSERT_FALSE(step(T60MIN - 99UL, 0, true, true).charger_reset);
    TEST_ASSERT_FALSE(step(T60MIN - 99UL + T60MIN, 0, true, true).charger_reset);
    TEST_ASSERT_TRUE(step(T60MIN - 99UL + T60MIN + 1UL, 0, true, true).charger_reset);
}

void test_charger_failed_read_is_not_zero_current(void)
{
    /* 90 min with the charger I2C read never completing: no evidence of
       "0 mA", so no reset request. */
    TEST_ASSERT_FALSE(step(0U, 0, false, true).charger_reset);
    TEST_ASSERT_FALSE(step(90UL * 60UL * 1000UL, 0, false, true).charger_reset);

    /* ...and a failed read breaks an episode in progress: the full window is
       required again afterwards. */
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);
    TEST_ASSERT_FALSE(step(T60MIN - 1UL, 0, false, true).charger_reset);
    TEST_ASSERT_FALSE(step(2UL * T60MIN - 1UL, 0, true, true).charger_reset);  /* new episode */
    TEST_ASSERT_FALSE(step(3UL * T60MIN - 1UL, 0, true, true).charger_reset);  /* exactly the window */
    TEST_ASSERT_TRUE(step(3UL * T60MIN, 0, true, true).charger_reset);         /* past it */
}

/* ------------------------------------------------- battery monitor (EL-05) */

void test_batt_monitor_mute_below_window_no_request(void)
{
    TEST_ASSERT_FALSE(step(0U, 250, true, false).batt_monitor_reset);

    TEST_ASSERT_FALSE(step(T10S - 1UL, 250, true, false).batt_monitor_reset);
    /* "more than 10 s" */
    TEST_ASSERT_FALSE(step(T10S, 250, true, false).batt_monitor_reset);
}

void test_batt_monitor_mute_past_window_requests_reset(void)
{
    TEST_ASSERT_FALSE(step(0U, 250, true, false).batt_monitor_reset);

    TEST_ASSERT_TRUE(step(T10S + 1UL, 250, true, false).batt_monitor_reset);
}

void test_batt_monitor_answer_clears_request_and_rearms(void)
{
    TEST_ASSERT_FALSE(step(0U, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_TRUE(step(T10S + 1UL, 250, true, false).batt_monitor_reset);

    /* Exit condition: the monitor resumes communication. */
    TEST_ASSERT_FALSE(step(T10S + 2UL, 250, true, true).batt_monitor_reset);

    /* Mute again -> a fresh 10 s window, not an immediate re-trigger. */
    TEST_ASSERT_FALSE(step(T10S + 3UL, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_FALSE(step(2UL * T10S + 3UL, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_TRUE(step(2UL * T10S + 4UL, 250, true, false).batt_monitor_reset);
}

/* ------------------------------------------------------------ tick wrap */

void test_tick_counter_wrap_is_handled(void)
{
    /* Episode starts 1 000 000 ms before the 32-bit counter rolls over. */
    const uint32_t start = 0xFFFFFFFFUL - 1000000UL;

    TEST_ASSERT_FALSE(step(start, 0, true, true).charger_reset);

    /* Still short of the window, on the far side of the wrap:
       2 600 000 - (2^32 - 1 000 001) == 3 600 000 exactly. */
    TEST_ASSERT_FALSE(step(2599999UL, 0, true, true).charger_reset);

    /* One more millisecond across the rollover == 3 600 001 elapsed. */
    TEST_ASSERT_TRUE(step(2600000UL, 0, true, true).charger_reset);

    /* Same modular arithmetic on the 10 s window: the episode starts 5 001 ms
       before the counter rolls over. */
    const uint32_t bm_start = 0xFFFFFFFFUL - 5000UL;
    TEST_ASSERT_FALSE(step(bm_start, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_FALSE(step(4999UL, 250, true, false).batt_monitor_reset);  /* 10 000 elapsed */
    TEST_ASSERT_TRUE(step(5000UL, 250, true, false).batt_monitor_reset);   /* 10 001 elapsed */
}

/* ------------------------------------------------------------ ack / rearm */

void test_ack_restarts_window_without_hammering(void)
{
    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);
    TEST_ASSERT_TRUE(step(T60MIN + 1UL, 0, true, true).charger_reset);

    /* The caller performed the reset; the fault condition has not cleared
       yet (charging takes time to come back). No new request is raised
       until a full window has been observed since the reset. */
    eps_fdir_ack_reset(&ctx, EPS_FDIR_TARGET_CHARGER, T60MIN + 1UL);
    TEST_ASSERT_FALSE(step(T60MIN + 1UL, 0, true, true).charger_reset);
    TEST_ASSERT_FALSE(step(T60MIN + 1UL + T60MIN, 0, true, true).charger_reset);
    TEST_ASSERT_TRUE(step(T60MIN + 1UL + T60MIN + 1UL, 0, true, true).charger_reset);
}

void test_ack_clears_latch_of_a_recovered_channel(void)
{
    /* Battery monitor recovered before the caller acknowledged: the exit
       condition alone clears the request, and ack on a healthy channel is a
       no-op that must not arm anything. */
    TEST_ASSERT_FALSE(step(0U, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_TRUE(step(T10S + 1UL, 250, true, false).batt_monitor_reset);
    TEST_ASSERT_FALSE(step(T10S + 2UL, 250, true, true).batt_monitor_reset);

    eps_fdir_ack_reset(&ctx, EPS_FDIR_TARGET_BATT_MONITOR, T10S + 2UL);
    TEST_ASSERT_FALSE(step(T10S + 3UL, 250, true, true).batt_monitor_reset);
}

void test_ack_rejects_null_and_unknown_target(void)
{
    eps_fdir_ack_reset(NULL, EPS_FDIR_TARGET_CHARGER, 1000U);

    TEST_ASSERT_FALSE(step(0U, 0, true, true).charger_reset);
    TEST_ASSERT_TRUE(step(T60MIN + 1UL, 0, true, true).charger_reset);

    /* Unknown target: ignored, the pending request is untouched. */
    eps_fdir_ack_reset(&ctx, (eps_fdir_target_t) 42, T60MIN + 2UL);
    TEST_ASSERT_TRUE(step(T60MIN + 3UL, 0, true, true).charger_reset);
}

/* ------------------------------------------------------ unarmed / heartbeat */

void test_unarmed_channels_never_fire(void)
{
    eps_fdir_config_t off = { 0, 0, 0 };     /* every window disarmed */

    TEST_ASSERT_EQUAL_INT(0, eps_fdir_init(&ctx, &off));
    for (uint32_t t = 0; t < 10UL; t++) {
        eps_fdir_requests_t r = step(t * T60MIN, 0, true, false);
        TEST_ASSERT_FALSE(r.charger_reset);
        TEST_ASSERT_FALSE(r.batt_monitor_reset);
        TEST_ASSERT_FALSE(r.eps_reset);
    }
}

void test_heartbeat_unarmed_is_silent(void)
{
    /* TBC in the document: with the default config an EPS that never sends
       a heartbeat must NOT be reset on an invented timeout. */
    TEST_ASSERT_FALSE(step(90UL * 60UL * 1000UL, 250, true, true).eps_reset);
}

/* FDIR-EPS-EL-04, silence AFTER a heartbeat that was actually received:
 * "OBS resets the EPS uC via the NRST pin after TBC ms from the last
 * heartbeat". ONE timeout after the last HB must be enough — a second window
 * opened on top of the first one would push NRST out to 2xTBC and leave the
 * EPS uC running as a dead sensor for a full extra TBC. */
void test_heartbeat_resets_at_one_timeout_from_last_hb(void)
{
    eps_fdir_config_t hb = { T60MIN, T10S, THB };   /* window = project input */

    TEST_ASSERT_EQUAL_INT(0, eps_fdir_init(&ctx, &hb));

    eps_fdir_note_eps_heartbeat(&ctx, 1000U);

    TEST_ASSERT_FALSE(step(1000U, 250, true, true).eps_reset);         /* HB just came */
    TEST_ASSERT_FALSE(step(1000UL + THB, 250, true, true).eps_reset);  /* exactly TBC: not "more than" */
    TEST_ASSERT_TRUE(step(1000UL + THB + 1UL, 250, true, true).eps_reset); /* 1xTBC: NRST due */

    /* Level, not pulse: asserted for as long as the silence lasts. */
    TEST_ASSERT_TRUE(step(1000UL + THB + 2UL, 250, true, true).eps_reset);

    /* A heartbeat arrives: the EPS is alive, the request drops. */
    eps_fdir_note_eps_heartbeat(&ctx, 7000U);
    TEST_ASSERT_FALSE(step(7000U, 250, true, true).eps_reset);

    /* ...and the next silence is again ONE timeout after THAT heartbeat. */
    TEST_ASSERT_FALSE(step(7000UL + THB, 250, true, true).eps_reset);
    TEST_ASSERT_TRUE(step(7000UL + THB + 1UL, 250, true, true).eps_reset);

    eps_fdir_note_eps_heartbeat(NULL, 0U);   /* must not fault */
}

/* FDIR-EPS-EL-04, the other path of the SAME channel: an EPS that has never
 * sent a heartbeat since boot. Its reference is the boot tick, and it must
 * request NRST with exactly the same latency as the "went silent" path —
 * TBC. The two paths disagreeing (one at TBC, one at 2xTBC) is the bug this
 * test pins down. */
void test_heartbeat_never_seen_resets_at_one_timeout_from_boot(void)
{
    eps_fdir_config_t hb = { T60MIN, T10S, THB };

    TEST_ASSERT_EQUAL_INT(0, eps_fdir_init(&ctx, &hb));

    TEST_ASSERT_FALSE(step(0U, 250, true, true).eps_reset);         /* boot, nothing heard */
    TEST_ASSERT_FALSE(step(THB, 250, true, true).eps_reset);        /* exactly TBC */
    TEST_ASSERT_TRUE(step(THB + 1UL, 250, true, true).eps_reset);   /* 1xTBC: NRST due */
    TEST_ASSERT_TRUE(step(THB + 2UL, 250, true, true).eps_reset);   /* still silent */
}

/* Ack on the heartbeat channel: the caller has pulsed NRST, so the EPS uC is
 * granted a full fresh window to boot and resume its heartbeat before another
 * request is raised (same "no hammering" rule as the IC channels). */
void test_heartbeat_ack_restarts_one_window(void)
{
    eps_fdir_config_t hb = { T60MIN, T10S, THB };

    TEST_ASSERT_EQUAL_INT(0, eps_fdir_init(&ctx, &hb));
    eps_fdir_note_eps_heartbeat(&ctx, 0U);
    TEST_ASSERT_TRUE(step(THB + 1UL, 250, true, true).eps_reset);

    eps_fdir_ack_reset(&ctx, EPS_FDIR_TARGET_EPS, THB + 1UL);
    TEST_ASSERT_FALSE(step(THB + 1UL, 250, true, true).eps_reset);
    TEST_ASSERT_FALSE(step(2UL * THB + 1UL, 250, true, true).eps_reset);       /* one window after the NRST */
    TEST_ASSERT_TRUE(step(2UL * THB + 2UL, 250, true, true).eps_reset);        /* ...then due again */
}

/* ------------------------------------------------------- input validation */

void test_update_with_null_returns_no_request(void)
{
    eps_fdir_requests_t r = eps_fdir_update(NULL, NULL);

    TEST_ASSERT_FALSE(r.charger_reset);
    TEST_ASSERT_FALSE(r.batt_monitor_reset);
    TEST_ASSERT_FALSE(r.eps_reset);

    r = eps_fdir_update(&ctx, NULL);
    TEST_ASSERT_FALSE(r.charger_reset);
}

void test_init_rejects_null_and_oversized_windows(void)
{
    eps_fdir_config_t bad = { 0, 0, 0 };

    TEST_ASSERT_EQUAL_INT(-1, eps_fdir_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(-1, eps_fdir_init(&ctx, NULL));

    /* A window that does not fit the 32-bit modular difference: a wrap would
       become indistinguishable from a legitimate long silence. */
    bad.charger_timeout_ms = EPS_FDIR_TIMEOUT_MAX_MS + 1UL;
    TEST_ASSERT_EQUAL_INT(-1, eps_fdir_init(&ctx, &bad));

    bad.charger_timeout_ms      = T60MIN;
    bad.batt_monitor_timeout_ms = EPS_FDIR_TIMEOUT_MAX_MS + 1UL;
    TEST_ASSERT_EQUAL_INT(-1, eps_fdir_init(&ctx, &bad));

    bad.batt_monitor_timeout_ms = T10S;
    bad.eps_hb_timeout_ms       = EPS_FDIR_TIMEOUT_MAX_MS + 1UL;
    TEST_ASSERT_EQUAL_INT(-1, eps_fdir_init(&ctx, &bad));
}

void test_detectors_are_independent(void)
{
    /* A charger fault must not raise a battery-monitor or EPS request, and
       vice versa. */
    eps_fdir_requests_t r = step(0U, 0, true, false);

    TEST_ASSERT_FALSE(r.charger_reset);
    TEST_ASSERT_FALSE(r.batt_monitor_reset);

    r = step(T60MIN + 1UL, 0, true, true);
    TEST_ASSERT_TRUE(r.charger_reset);
    TEST_ASSERT_FALSE(r.batt_monitor_reset);
    TEST_ASSERT_FALSE(r.eps_reset);
}
