/* deploy_sense.c — DEPLOY_SENSE / LoRa_NRST mux on PB1 (see header),
 * plus the FDIR-COMM-EL-01 antenna-deployment fault reaction. */

#include "deploy_sense.h"
#include "main.h"

#include <stddef.h>   /* NULL */

#ifndef HOST_UNIT_TEST
#include "stm32l4xx_hal.h"

/* Blocking microsecond spin (same technique as the RadioLib HAL).
 * Host double below: instant no-op. */
static void mux_delay_us(uint32_t us)
{
    volatile uint32_t n = (SystemCoreClock / 1000000u) * us / 4u;
    while (n-- != 0u) {
        __NOP();
    }
}
#else
static void mux_delay_us(uint32_t us) { (void)us; }
#endif

/* Idle state of the mux: output HIGH = radio NOT in reset. */
static void mux_as_output_high(void)
{
    GPIO_InitTypeDef cfg = {0};
    cfg.Pin   = DEPLOY_SENSE_Pin;
    cfg.Mode  = GPIO_MODE_OUTPUT_PP;
    cfg.Pull  = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DEPLOY_SENSE_GPIO_Port, &cfg);
    HAL_GPIO_WritePin(DEPLOY_SENSE_GPIO_Port, DEPLOY_SENSE_Pin, GPIO_PIN_SET);
}

void deploy_mux_init(void)
{
    mux_as_output_high();
}

int deploy_read_raw(void)
{
    GPIO_InitTypeDef cfg = {0};
    int level;

    cfg.Pin   = DEPLOY_SENSE_Pin;
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_PULLUP;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DEPLOY_SENSE_GPIO_Port, &cfg);
    mux_delay_us(10u);              /* pull-up settle */
    level = (HAL_GPIO_ReadPin(DEPLOY_SENSE_GPIO_Port, DEPLOY_SENSE_Pin)
             == GPIO_PIN_SET) ? 1 : 0;
    /* Restore the reset role BEFORE returning: the line must never float. */
    mux_as_output_high();
    return level;
}

int deploy_is_deployed(void)
{
    int raw = deploy_read_raw();
    if (raw < 0) {
        return raw;
    }
    return (raw == DEPLOY_DEPLOYED_LEVEL) ? 1 : 0;
}

void deploy_nrst_assert(void)
{
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_RESET);
}

void deploy_nrst_release(void)
{
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_SET);
}

void deploy_nrst_pulse(void)
{
    deploy_nrst_assert();
    mux_delay_us(200u);             /* SX1268 needs >100 us */
    deploy_nrst_release();
}

/* ==========================================================================
 * FDIR-COMM-EL-01 — antenna deployment (thermal knife reactivation)
 *
 * Defaults: the numbers RED_FDIR_V2.xlsx does not give, chosen to be
 * conservative on both energy and time and stated here so they are reviewable:
 *
 *   max_attempts = 10    <- DOCUMENT (not a choice): "up to 10 thermal knife
 *                           activations" then "assume failure ... and ignore
 *                           fault".
 *   activation_ms = 5000 <- CHOICE. A thermal knife cuts a nylon wire by
 *                           heating a nichrome element; a few seconds is the
 *                           usual order of magnitude, and 5 s is short enough
 *                           that a failed attempt wastes little battery on a
 *                           PocketQube. The document gives no number.
 *   activation_step_ms = 5000 <- CHOICE. "longer activation period each
 *                           repetition" is in the document, the amount is not:
 *                           linear growth of one base period per repetition
 *                           gives 5,10,...,50 s, a bounded total of 275 s of
 *                           knife-on time, and reaches a plausible "this wire
 *                           is not going to give" energy without a runaway
 *                           schedule.
 *   activation_max_ms = 60000 <- CHOICE. Ceiling so a future change to
 *                           max_attempts cannot grow the period without
 *                           bound; inert with the defaults above (attempt 10
 *                           ends at 50 s) but reachable via configure().
 *   gap_ms = 5000 <- CHOICE. Time with the knife OFF to let the switch settle
 *                           and be sampled. Not in the document.
 * ========================================================================== */

typedef enum {
    FDIR_PH_IDLE = 0,   /* never started                                      */
    FDIR_PH_ON,         /* knife energised                                    */
    FDIR_PH_GAP,        /* knife off, observing the deployment switch         */
    FDIR_PH_END         /* terminal (DEPLOYED / FAILED / ABORTED)             */
} fdir_phase_t;

static const deploy_fdir_config_t deploy_fdir_defaults = {
    10u,        /* max_attempts       */
    5000u,      /* activation_ms      */
    5000u,      /* activation_step_ms */
    60000u,     /* activation_max_ms  */
    5000u       /* gap_ms             */
};

static deploy_fdir_config_t   s_cfg;
static deploy_fdir_status_t   s_st;
static fdir_phase_t           s_phase;
static uint32_t               s_phase_ms;   /* tick the current phase began */
static deploy_knife_driver_t  s_drv;

const deploy_fdir_config_t *deploy_fdir_default_config(void)
{
    return &deploy_fdir_defaults;
}

/* Drive the knife line and keep telemetry truthful (defined below). Declared
 * here because the driver swap and init() must cut through the OLD driver. */
static void knife_drive(bool on);

bool deploy_fdir_configure(const deploy_fdir_config_t *cfg)
{
    if ((cfg == NULL) || (cfg->max_attempts == 0u) || (cfg->activation_ms == 0u)) {
        return false;
    }
    s_cfg = *cfg;
    /* A ceiling below the first period would make the schedule shrink. */
    if ((s_cfg.activation_max_ms != 0u) &&
        (s_cfg.activation_max_ms < s_cfg.activation_ms)) {
        s_cfg.activation_max_ms = s_cfg.activation_ms;
    }
    return true;
}

void deploy_fdir_set_knife_driver(deploy_knife_driver_t drv)
{
    /* Cut through the OUTGOING driver before dropping the handle. Otherwise a
     * knife it energised could never be turned off again: the replacement was
     * never told the line is on, and detaching (NULL) removes the only
     * off-path, latching the knife ON for the whole mission. An OFF command
     * on an already-cold line is harmless; a missing one is not. */
    knife_drive(false);
    s_drv = drv;
}

void deploy_fdir_init(void)
{
    /* A re-init (watchdog, warm reboot) must not orphan an energised knife:
     * cut it through the driver in force BEFORE forgetting it, or the actuator
     * stays hot while telemetry reports knife_on = false. At cold boot no
     * driver is installed yet, so this calls nothing and touches no GPIO. */
    knife_drive(false);

    s_cfg        = deploy_fdir_defaults;
    s_drv        = NULL;
    s_phase      = FDIR_PH_IDLE;
    s_phase_ms   = 0u;
    s_st.result                = DEPLOY_FDIR_IDLE;
    s_st.attempts              = 0u;
    s_st.knife_on              = false;
    s_st.degraded_telecom      = false;
    s_st.safe_mode_requested   = false;
    s_st.current_activation_ms = 0u;
}

static uint32_t activation_for(const deploy_fdir_config_t *cfg, uint32_t attempt_1based)
{
    uint32_t n  = (attempt_1based > 0u) ? (attempt_1based - 1u) : 0u;
    uint32_t ms = cfg->activation_ms;

    if (cfg->activation_step_ms != 0u) {
        if (n > ((UINT32_MAX - ms) / cfg->activation_step_ms)) {
            ms = UINT32_MAX;                 /* overflow: let the cap decide */
        } else {
            ms += n * cfg->activation_step_ms;
        }
    }
    if ((cfg->activation_max_ms != 0u) && (ms > cfg->activation_max_ms)) {
        ms = cfg->activation_max_ms;
    }
    return ms;
}

uint32_t deploy_fdir_activation_ms(uint8_t attempt_index)
{
    uint32_t idx = (attempt_index == 0u) ? 1u : (uint32_t)attempt_index;
    return activation_for(&s_cfg, idx);
}

static void knife_drive(bool on)
{
    if (s_drv == NULL) {
        /* Nothing was commanded to the hardware: do not let telemetry claim
         * the knife is energised on a line that was never driven. */
        return;
    }
    s_drv(on);
    s_st.knife_on = on;
}

static void start_attempt(uint32_t now_ms)
{
    s_st.attempts++;
    s_st.result = DEPLOY_FDIR_RETRYING;
    s_st.current_activation_ms =
        activation_for(&s_cfg, (uint32_t)s_st.attempts);
    s_phase    = FDIR_PH_ON;
    s_phase_ms = now_ms;
    knife_drive(true);
}

static void finish(deploy_fdir_result_t result, bool degraded, bool safe_mode,
                   uint32_t now_ms)
{
    knife_drive(false);
    s_st.result              = result;
    s_st.degraded_telecom    = degraded;
    s_st.safe_mode_requested = safe_mode;
    s_phase                  = FDIR_PH_END;
    s_phase_ms               = now_ms;
}

/* One phase boundary crossed at `now_ms`; returns false when the FDIR has
 * stopped and there is nothing left to do. Bounded: at most max_attempts
 * transitions per call. */
static bool advance(obw_state_t mode, uint32_t now_ms)
{
    switch (s_phase) {
    case FDIR_PH_IDLE:
        if (mode != STATE_INIT) {
            return false;                   /* active only in INITIALIZATION */
        }
        start_attempt(now_ms);
        return true;

    case FDIR_PH_ON:
        /* The reaction is scoped to INITIALIZATION: if the mode has already
         * left it, cut NOW instead of holding the line hot until the end of
         * the window (up to activation_max_ms, 50 s with the defaults). This
         * is what makes the cut independent of the caller's cadence: the first
         * step in a non-INIT mode closes the reaction as ABORTED. */
        if (mode != STATE_INIT) {
            finish(DEPLOY_FDIR_ABORTED, false, false, now_ms);
            return false;
        }
        /* The switch may close while the knife is still hot. */
        if (deploy_is_deployed() == 1) {
            finish(DEPLOY_FDIR_DEPLOYED, false, true, now_ms);
            return false;
        }
        if ((uint32_t)(now_ms - s_phase_ms) >= s_st.current_activation_ms) {
            knife_drive(false);
            s_phase    = FDIR_PH_GAP;
            s_phase_ms = now_ms;
            return true;
        }
        return false;

    case FDIR_PH_GAP:
        if (deploy_is_deployed() == 1) {
            finish(DEPLOY_FDIR_DEPLOYED, false, true, now_ms);
            return false;
        }
        if ((uint32_t)(now_ms - s_phase_ms) < s_cfg.gap_ms) {
            return false;
        }
        /* Leaving INITIALIZATION without a deploy reading or the 10-attempt
         * timeout means the reaction no longer applies (the FDIR is scoped
         * to INITIALIZATION): stop cleanly instead of firing a further
         * activation outside its mode. */
        if (mode != STATE_INIT) {
            finish(DEPLOY_FDIR_ABORTED, false, false, now_ms);
            return false;
        }
        if (s_st.attempts >= s_cfg.max_attempts) {
            /* Timeout: assume the switch or the mechanism failed and ignore
             * the fault, no further mitigation exists. Fall back to degraded
             * telecommunication operation and hand over to SAFE MODE. */
            finish(DEPLOY_FDIR_FAILED, true, true, now_ms);
            return false;
        }
        start_attempt(now_ms);
        return true;

    case FDIR_PH_END:
    default:
        return false;
    }
}

void deploy_fdir_step(obw_state_t mode, uint32_t now_ms)
{
    /* Consume every elapsed phase so a late/coarse call does not lose an
     * activation; the loop is bounded by max_attempts. */
    while (advance(mode, now_ms)) {
        /* keep going */
    }
}

void deploy_fdir_ground_command(void)
{
    if (s_phase == FDIR_PH_END) {
        return;
    }
    /* `s_phase_ms` is only a bookkeeping stamp here: no tick is available at
     * this seam, and a finished FDIR never reads it again. */
    finish(DEPLOY_FDIR_ABORTED, false, false, s_phase_ms);
}

const deploy_fdir_status_t *deploy_fdir_status(void)
{
    return &s_st;
}

const deploy_fdir_config_t *deploy_fdir_config(void)
{
    return &s_cfg;
}