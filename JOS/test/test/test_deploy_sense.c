/**
 * @file    test_deploy_sense.c
 * @brief   Unit tests for the DEPLOY_SENSE / LoRa_NRST mux on PB1
 *          (App/comms/deploy_sense.c, SPF v3 3.7.5.3.1 p.95) and for the
 *          FDIR-COMM-EL-01 antenna-deployment reaction (RED_FDIR_V2.xlsx,
 *          sheet 'COMM', FMEA-COMM-EL-01).
 *
 * The PB1 GPIO is doubled by support/hal_stubs.c: HAL_GPIO_Init records the
 * last mode/pull per pin, HAL_GPIO_WritePin drives an ODR latch, and
 * host_gpio_force_input() overrides what a pin reads as input (the
 * deploy-switch level). That is exactly the surface the mux sequences, so
 * "the radio line is never left floating" is an assertion, not an inference.
 *
 * What is verified
 *   - mux init leaves PB1 as output HIGH (radio NOT in reset).
 *   - a deploy read samples the switch level AND restores output HIGH.
 *   - deployed = LOW, stowed = HIGH (DEPLOY_DEPLOYED_LEVEL).
 *   - the SX1268 reset helpers drive the same pin (assert LOW, release HIGH,
 *     pulse ends HIGH).
 *   - FDIR: at most 10 activations (never 11), growing period per activation,
 *     no activation outside INITIALIZATION, SAFE MODE requested on resolution
 *     and on timeout, degraded telecom on timeout.
 *   - FDIR knife cannot be left energised (the hazard this module exists to
 *     prevent): swapping or clearing the driver cuts through the OLD driver
 *     first, re-init cuts before forgetting the handle, the ON phase honours
 *     the mode, the cut does not depend on the caller's step() cadence, and
 *     knife_on only reports a line that was actually commanded.
 */

#include "unity.h"
#include "deploy_sense.h"
#include "host_support.h"
#include "main.h"       /* fakes/main.h on the host: GPIO mode/pull constants */
#include "obsw_types.h" /* obw_state_t: STATE_INIT is INITIALIZATION MODE */

#define PB1 1

#define SW_DEPLOYED 0   /* switch pulls PB1 to GND once the antenna fires */
#define SW_STOWED   1

/* ---- knife-driver recorder (deploy_fdir_set_knife_driver) -------------- */
static int  knife_on_calls;
static int  knife_off_calls;
static int  knife_last_level;
static void knife_recorder(bool on)
{
    if (on) {
        knife_on_calls++;
    } else {
        knife_off_calls++;
    }
    knife_last_level = on ? 1 : 0;
}

/* Second recorder: proves which driver gets the command when they are
 * swapped while the knife is energised. */
static int  knife2_on_calls;
static void knife_recorder2(bool on)
{
    if (on) {
        knife2_on_calls++;
    }
}

static const deploy_fdir_status_t *fdir;

void setUp(void)
{
    host_gpio_reset();
    host_gpio_force_input(PB1, -1);   /* PB1 follows ODR unless forced */
    deploy_fdir_init();
    deploy_fdir_set_knife_driver(knife_recorder);
    knife_on_calls  = 0;
    knife_off_calls = 0;
    knife_last_level = -1;
    knife2_on_calls = 0;
    fdir = deploy_fdir_status();
}

void tearDown(void)
{
    host_gpio_force_input(PB1, -1);
    deploy_fdir_set_knife_driver(NULL);
}

/* Mux init must idle HIGH as push-pull output: the SX1268 reset is
 * active-low, so anything else holds the radio in reset from boot. */
void test_mux_init_idle_high(void)
{
    deploy_mux_init();
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* A stowed switch (pull-up HIGH) reads 1, and the pin comes back as
 * output HIGH — the reset role is restored before returning. */
void test_read_stowed_restores_output_high(void)
{
    int level;

    deploy_mux_init();
    host_gpio_force_input(PB1, 1);
    level = deploy_read_raw();

    TEST_ASSERT_EQUAL(1, level);
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* A fired switch pulls PB1 to GND: reads 0 even though the idle ODR is 1. */
void test_read_deployed_switch_low(void)
{
    int level;

    deploy_mux_init();
    host_gpio_force_input(PB1, 0);
    level = deploy_read_raw();

    TEST_ASSERT_EQUAL(0, level);
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* While sampling, the pin really passes through input+pull-up (not an
 * output fighting the switch): the stub latches the transient mode even
 * though the pin is restored to output before the call returns. */
void test_read_samples_as_input_pullup(void)
{
    deploy_mux_init();
    TEST_ASSERT_EQUAL(0, host_gpio_saw_input_pullup(PB1));

    host_gpio_force_input(PB1, 0);
    TEST_ASSERT_EQUAL(0, deploy_read_raw());

    TEST_ASSERT_EQUAL(1, host_gpio_saw_input_pullup(PB1));
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* Polarity: DEPLOYED reads LOW, STOWED reads HIGH. */
void test_is_deployed_polarity(void)
{
    deploy_mux_init();

    host_gpio_force_input(PB1, 0);
    TEST_ASSERT_EQUAL(1, deploy_is_deployed());

    host_gpio_force_input(PB1, 1);
    TEST_ASSERT_EQUAL(0, deploy_is_deployed());
}

/* Reset helpers share the same pin: assert pulls LOW, release drives HIGH,
 * pulse (>100 us per SX1268 datasheet) ends HIGH. */
void test_nrst_helpers(void)
{
    deploy_mux_init();

    deploy_nrst_assert();
    TEST_ASSERT_EQUAL(0, host_gpio_odr(PB1));

    deploy_nrst_release();
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));

    deploy_nrst_assert();
    deploy_nrst_pulse();
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
}

/* ==========================================================================
 * FDIR-COMM-EL-01
 * ========================================================================== */

#define FDIR_STEP_MS   100u
/* Worst case with the defaults: 275 s of knife-on + 10 x 5 s gap = 325 s. */
#define FDIR_LIMIT_MS  400000u

static bool fdir_ended(void)
{
    return (fdir->result == DEPLOY_FDIR_DEPLOYED) ||
           (fdir->result == DEPLOY_FDIR_FAILED)   ||
           (fdir->result == DEPLOY_FDIR_ABORTED);
}

/* Run the reaction from `first_ms` to the end (or the limit) in FDIR_STEP_MS
 * slices, all in INITIALIZATION. Returns the tick at which it terminated. */
static uint32_t fdir_run_in_init(uint32_t first_ms)
{
    uint32_t t;

    for (t = first_ms; (t <= FDIR_LIMIT_MS) && !fdir_ended(); t += FDIR_STEP_MS) {
        deploy_fdir_step(STATE_INIT, t);
    }
    return t;
}

/* The declared defaults: the 10-attempt ceiling comes from the document, the
 * timing knobs are declared project choices and must be coherent (growth is
 * positive, ceiling above the first period, non-zero gap). */
void test_fdir_defaults_are_declared_and_coherent(void)
{
    const deploy_fdir_config_t *cfg = deploy_fdir_default_config();

    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL_UINT8(10u, cfg->max_attempts);   /* document */
    TEST_ASSERT_GREATER_THAN_UINT32(0u, cfg->activation_ms);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, cfg->activation_step_ms);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, cfg->gap_ms);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(cfg->activation_ms, cfg->activation_max_ms);
}

/* "up to 10 ... activations": the counter stops at exactly 10, never 11, and
 * the timeout declares the fault ignored and asks for SAFE MODE with degraded
 * telecommunication as the fallback. */
void test_fdir_stops_at_10_attempts_not_11(void)
{
    uint32_t t;

    host_gpio_force_input(PB1, SW_STOWED);
    t = fdir_run_in_init(0u);
    TEST_ASSERT_TRUE(fdir_ended());
    TEST_ASSERT_TRUE(t <= FDIR_LIMIT_MS);

    TEST_ASSERT_EQUAL_UINT8(10u, fdir->attempts);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_FAILED, fdir->result);
    TEST_ASSERT_TRUE(fdir->degraded_telecom);
    TEST_ASSERT_TRUE(fdir->safe_mode_requested);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(10, knife_on_calls);   /* one per activation */
    TEST_ASSERT_EQUAL_INT(0, knife_last_level);  /* ends with the knife cut */

    /* Terminal: further calls (even far in the future) change nothing. */
    deploy_fdir_step(STATE_INIT, FDIR_LIMIT_MS + 1000000u);
    TEST_ASSERT_EQUAL_UINT8(10u, fdir->attempts);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_FAILED, fdir->result);
    TEST_ASSERT_EQUAL_INT(10, knife_on_calls);
}

/* "longer activation period each repetition": every consecutive activation is
 * strictly longer than the previous one. Observed from the real run, not from
 * the helper alone. */
void test_fdir_period_grows_between_consecutive_attempts(void)
{
    uint32_t periods[10];
    uint8_t  prev_attempts = 0;
    uint8_t  i;
    uint32_t t;

    host_gpio_force_input(PB1, SW_STOWED);
    for (i = 0; i < 10u; i++) {
        periods[i] = 0u;
    }

    for (t = 0u; (t <= FDIR_LIMIT_MS) && !fdir_ended(); t += FDIR_STEP_MS) {
        deploy_fdir_step(STATE_INIT, t);
        if (fdir->attempts != prev_attempts) {
            TEST_ASSERT_TRUE(fdir->attempts <= 10u);
            periods[fdir->attempts - 1u] = fdir->current_activation_ms;
            prev_attempts = fdir->attempts;
        }
    }

    TEST_ASSERT_EQUAL_UINT8(10u, prev_attempts);
    for (i = 1u; i < 10u; i++) {
        TEST_ASSERT_GREATER_THAN_UINT32(periods[i - 1u], periods[i]);
    }
    /* And the periods are the declared schedule, not incidental values. */
    for (i = 0u; i < 10u; i++) {
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(i + 1u) * 5000u, periods[i]);
    }
}

/* A resolved deployment (the switch closes) requests SAFE MODE, clears the
 * degraded flag, and cuts the knife. */
void test_fdir_deployed_requests_safe_mode(void)
{
    host_gpio_force_input(PB1, SW_DEPLOYED);   /* switch already closed */
    deploy_fdir_step(STATE_INIT, 0u);

    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_DEPLOYED, fdir->result);
    TEST_ASSERT_TRUE(fdir->safe_mode_requested);
    TEST_ASSERT_FALSE(fdir->degraded_telecom);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);

    deploy_fdir_step(STATE_INIT, 1000000u);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_DEPLOYED, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);
}

/* The reaction is "active only in INITIALIZATION MODE": outside STATE_INIT it
 * must never fire the knife nor count an activation. */
void test_fdir_inactive_outside_initialization(void)
{
    const obw_state_t others[] = { STATE_OFF, STATE_CRIT, STATE_READY, STATE_ACTIVE };
    uint32_t i;
    uint32_t t;

    host_gpio_force_input(PB1, SW_STOWED);
    for (i = 0u; i < (sizeof(others) / sizeof(others[0])); i++) {
        for (t = 0u; t < 100000u; t += FDIR_STEP_MS) {
            deploy_fdir_step(others[i], t);
        }
    }

    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_IDLE, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(0u, fdir->attempts);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_FALSE(fdir->safe_mode_requested);
    TEST_ASSERT_EQUAL_INT(0, knife_on_calls);
}

/* Started in INITIALIZATION, but the mode leaves it without a deploy reading:
 * the reaction stops where it is (no further activation) and does not force
 * SAFE MODE — the mode change came from elsewhere (ground). */
void test_fdir_aborts_when_leaving_initialization(void)
{
    uint32_t t;

    host_gpio_force_input(PB1, SW_STOWED);

    /* Two activations start (attempt 1 at t=0, attempt 2 at t=10000). */
    for (t = 0u; t <= 10000u; t += FDIR_STEP_MS) {
        deploy_fdir_step(STATE_INIT, t);
    }
    TEST_ASSERT_EQUAL_UINT8(2u, fdir->attempts);

    for (t = 10100u; (t <= FDIR_LIMIT_MS) && !fdir_ended(); t += FDIR_STEP_MS) {
        deploy_fdir_step(STATE_READY, t);
    }

    TEST_ASSERT_TRUE(fdir_ended());
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(2u, fdir->attempts);   /* no third activation */
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_FALSE(fdir->safe_mode_requested);
    TEST_ASSERT_EQUAL_INT(2, knife_on_calls);
}

/* Exit condition "Telecommand received from ground" stops the retries. */
void test_fdir_ground_command_stops_retrying(void)
{
    host_gpio_force_input(PB1, SW_STOWED);
    deploy_fdir_step(STATE_INIT, 0u);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_RETRYING, fdir->result);

    deploy_fdir_ground_command();

    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);

    deploy_fdir_step(STATE_INIT, FDIR_LIMIT_MS);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);
}

/* The activation schedule is that of the configuration in force, including the
 * ceiling: a config change moves the schedule, it is not ignored. */
void test_fdir_activation_schedule_follows_config(void)
{
    deploy_fdir_config_t cfg = *deploy_fdir_default_config();

    TEST_ASSERT_EQUAL_UINT32(5000u, deploy_fdir_activation_ms(1u));
    TEST_ASSERT_EQUAL_UINT32(50000u, deploy_fdir_activation_ms(10u));
    TEST_ASSERT_EQUAL_UINT32(deploy_fdir_activation_ms(1u), deploy_fdir_activation_ms(0u));

    cfg.activation_ms      = 1000u;
    cfg.activation_step_ms = 1000u;
    cfg.activation_max_ms  = 8000u;
    TEST_ASSERT_TRUE(deploy_fdir_configure(&cfg));
    TEST_ASSERT_EQUAL_UINT32(1000u, deploy_fdir_activation_ms(1u));
    TEST_ASSERT_EQUAL_UINT32(5000u, deploy_fdir_activation_ms(5u));
    TEST_ASSERT_EQUAL_UINT32(8000u, deploy_fdir_activation_ms(8u));
    TEST_ASSERT_EQUAL_UINT32(8000u, deploy_fdir_activation_ms(20u));  /* capped */
}

/* A retry budget of zero or a zero-length activation would silently disable
 * the reaction: refused, configuration unchanged. */
void test_fdir_invalid_config_rejected(void)
{
    deploy_fdir_config_t cfg = *deploy_fdir_default_config();

    TEST_ASSERT_FALSE(deploy_fdir_configure(NULL));

    cfg.max_attempts = 0u;
    TEST_ASSERT_FALSE(deploy_fdir_configure(&cfg));

    cfg = *deploy_fdir_default_config();
    cfg.activation_ms = 0u;
    TEST_ASSERT_FALSE(deploy_fdir_configure(&cfg));

    TEST_ASSERT_EQUAL_UINT8(10u, deploy_fdir_config()->max_attempts);
}

/* ==========================================================================
 * Knife de-energising hazards (CodeRabbit HIGH on PR #88)
 *
 * The knife is a load switch: an actuator left energised burns the battery and
 * destroys the mechanism. Every path that can lose track of the line must cut
 * it first. Each test below fails on the pre-fix code.
 * ========================================================================== */

/* HAZARD (a) — deploy_fdir_set_knife_driver() replaced/cleared the driver
 * without ever de-energising through it. The outgoing driver never received
 * `false`, and detaching (NULL) removed the only off-path (knife_drive() does
 * `if (s_drv != NULL)`): the line stayed latched ON for the whole mission. */
void test_fdir_set_driver_cuts_old_knife(void)
{
    host_gpio_force_input(PB1, SW_STOWED);

    deploy_fdir_step(STATE_INIT, 0u);          /* attempt 1 energises the line */
    TEST_ASSERT_TRUE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);
    TEST_ASSERT_EQUAL_INT(0, knife_off_calls);

    /* Swap while energised: the OLD driver must be told to cut, the new one
     * must not be used for a line it never energised. */
    deploy_fdir_set_knife_driver(knife_recorder2);
    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);
    TEST_ASSERT_EQUAL_INT(0, knife2_on_calls);
    TEST_ASSERT_FALSE(fdir->knife_on);

    /* Detach entirely while energised: NULL is the worst case — without this
     * cut there is no off-path left at all. */
    deploy_fdir_init();                        /* clears state, line already cold */
    deploy_fdir_set_knife_driver(knife_recorder);
    deploy_fdir_step(STATE_INIT, 0u);          /* attempt 1 energises again */
    TEST_ASSERT_TRUE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(2, knife_on_calls);

    knife_off_calls = 0;
    deploy_fdir_set_knife_driver(NULL);
    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);  /* the live driver cut the line */
    TEST_ASSERT_FALSE(fdir->knife_on);

    /* With no driver installed nothing can re-energise, and knife_on says so. */
    deploy_fdir_step(STATE_INIT, 60000u);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(2, knife_on_calls);
}

/* HAZARD (b) — deploy_fdir_init() cleared s_drv and s_st.knife_on without
 * driving the line OFF. A warm reboot / watchdog re-init orphaned the
 * actuator while telemetry reported knife_on = false on a hot line. */
void test_fdir_init_cuts_knife_before_forget(void)
{
    host_gpio_force_input(PB1, SW_STOWED);

    deploy_fdir_step(STATE_INIT, 0u);
    TEST_ASSERT_TRUE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);

    deploy_fdir_init();

    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);   /* cut through the live driver */
    TEST_ASSERT_FALSE(fdir->knife_on);           /* telemetry matches the line  */
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_IDLE, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(0u, fdir->attempts);

    /* A second re-init with the line already known OFF must not touch it. */
    deploy_fdir_init();
    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);

    /* A cold init with no driver installed calls nothing (boot order safe). */
    TEST_ASSERT_FALSE(fdir->knife_on);
}

/* HAZARD (c) — the FDIR_PH_ON phase never tested the mode: leaving
 * STATE_INIT mid-activation kept the knife hot until the end of the window
 * (up to activation_max_ms, 50 s with the defaults) instead of cutting at
 * once as ABORTED. */
void test_fdir_on_cuts_when_mode_leaves_init(void)
{
    host_gpio_force_input(PB1, SW_STOWED);

    deploy_fdir_step(STATE_INIT, 0u);            /* attempt 1, knife ON */
    TEST_ASSERT_TRUE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_RETRYING, fdir->result);

    /* 100 ms into a 5000 ms window the mode leaves INITIALIZATION. */
    deploy_fdir_step(STATE_READY, 100u);

    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_FALSE(fdir->safe_mode_requested);  /* the mode change was external */
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);
    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);

    /* Terminal: later steps in any mode never re-energise the line. */
    deploy_fdir_step(STATE_INIT, 200u);
    deploy_fdir_step(STATE_READY, 500000u);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);
    TEST_ASSERT_FALSE(fdir->knife_on);
}

/* HAZARD (d) — every off-path was reachable only from deploy_fdir_step() and
 * deploy_fdir_ground_command(). With the old header contract (step() "new
 * activations only start in STATE_INIT") a caller could stop calling step()
 * outside INITIALIZATION and the knife stayed on forever. The cut must not
 * depend on the caller driving a fine cadence: the first step in a non-INIT
 * mode cuts, even if it arrives long after the window. */
void test_fdir_cut_is_independent_of_step_cadence(void)
{
    host_gpio_force_input(PB1, SW_STOWED);

    deploy_fdir_step(STATE_INIT, 0u);            /* knife ON */
    TEST_ASSERT_TRUE(fdir->knife_on);

    /* Coarse caller: one single step 200 s later, in a non-INIT mode. */
    deploy_fdir_step(STATE_READY, 200000u);

    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_FALSE(fdir->knife_on);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);   /* no extra activation */
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);
    TEST_ASSERT_EQUAL_INT(1, knife_off_calls);

    /* Returning to INITIALIZATION does not resurrect a finished reaction. */
    deploy_fdir_step(STATE_INIT, 200100u);
    deploy_fdir_step(STATE_INIT, 400000u);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_ABORTED, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts);
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);
    TEST_ASSERT_FALSE(fdir->knife_on);
}

/* HAZARD (e) — knife_drive() set s_st.knife_on even when the call never
 * reached the hardware (no driver installed). knife_on is telemetry and must
 * report the line actually commanded, or the ground sees "knife on" on a
 * module that has no line driven at all. */
void test_fdir_knife_on_reflects_commanded_line(void)
{
    host_gpio_force_input(PB1, SW_STOWED);

    deploy_fdir_set_knife_driver(NULL);          /* no line to drive */

    deploy_fdir_step(STATE_INIT, 0u);
    TEST_ASSERT_EQUAL_INT(DEPLOY_FDIR_RETRYING, fdir->result);
    TEST_ASSERT_EQUAL_UINT8(1u, fdir->attempts); /* the logic still counts */
    TEST_ASSERT_FALSE(fdir->knife_on);           /* but nothing was commanded */

    deploy_fdir_step(STATE_INIT, 5000u);         /* window ends: still nothing */
    TEST_ASSERT_FALSE(fdir->knife_on);

    /* With the driver installed again the same sequence does report ON. */
    deploy_fdir_set_knife_driver(knife_recorder);
    deploy_fdir_step(STATE_INIT, 15000u);        /* attempt 2 starts */
    TEST_ASSERT_TRUE(fdir->knife_on);
    TEST_ASSERT_EQUAL_INT(1, knife_on_calls);
}
