#ifndef DEPLOY_SENSE_H
#define DEPLOY_SENSE_H

#include <stdint.h>
#include <stdbool.h>

#include "obsw_types.h"   /* obw_state_t — INITIALIZATION = STATE_INIT */

/* deploy_sense — DEPLOY_SENSE / LoRa_NRST multiplexed GPIO (PB1).
 *
 * SPF v3 3.7.5.3.1 p.95: ONE OBC GPIO is shared between the antenna
 * deployment switch (DEPLOY_SENSE, COMMS connector pin 19) and the SX1268
 * reset (LoRa_NRST, COMMS connector pin 4). The two roles are mutually
 * exclusive in time:
 *   - LoRa_NRST : output push-pull, idle HIGH (SX1268 reset is active-low).
 *   - DEPLOY_SENSE read: temporarily reconfigure PB1 as input + pull-up,
 *     sample, then IMMEDIATELY restore output HIGH so the line never floats
 *     into a spurious radio reset.
 *
 * OWNERSHIP RULE: call deploy_read_raw() only while the radio is idle
 * (never between lora_tx() and lora_tx_wait_done(), never from the DIO1
 * ISR path). The pin is left as output HIGH on return either way.
 *
 * Switch polarity: the deploy switch pulls PB1 to GND through the harness
 * when the antenna has fired, so DEPLOYED reads 0 (LOW) and STOWED reads 1
 * (HIGH, pull-up). If the harness inverts this, flip DEPLOY_DEPLOYED_LEVEL
 * in one place below.
 */

#define DEPLOY_DEPLOYED_LEVEL 0

/* Configure PB1 as output, idle HIGH (radio NOT in reset). */
void deploy_mux_init(void);

/* Sample the deploy switch (reconfigures PB1, restores output HIGH before
 * returning). Returns the raw level 0/1, negative on bus error. */
int deploy_read_raw(void);

/* 1 when the switch reports DEPLOYED, 0 when STOWED, negative on error. */
int deploy_is_deployed(void);

/* SX1268 reset helpers (same pin, output role). pulse: active-low 200 us
 * (>100 us per SX1268 datasheet). leaves the pin HIGH. */
void deploy_nrst_assert(void);
void deploy_nrst_release(void);
void deploy_nrst_pulse(void);

/* ==========================================================================
 * FDIR-COMM-EL-01 — antenna deployment fault reaction (thermal knife)
 * ==========================================================================
 * Source: RED_FDIR_V2.xlsx, sheet 'COMM', FMEA-COMM-EL-01 /
 * FDIR-COMM-EL-01 (SYS SPF V3 / ANNEXES / ANNEX 09). Document has NO release
 * status and carries "da valutare"/TBD fields elsewhere: treat it as the best
 * available evidence, not an approved baseline (see /root/sp_findings/TROVATO.md).
 *
 * What the document pins down (NOT chosen here):
 *   - entry   : "Thermal knife has been activated but antenna deployment
 *               switch does not detect deployment"
 *   - exit    : "Deployment switch activates - Telecommand received from ground"
 *   - timeout : "If the issue is not resolved within 10 thermal knife
 *               activations, assume failure ... and ignore fault"
 *   - recovery: "Reactivate thermal knife up to 10 times with longer
 *               activation period each repetition, fallback action is to
 *               enter degraded telecommunication operation"
 *   - modes   : "Active only in INITIALIZATION MODE, switches to SAFE MODE
 *               after resolution or timeout"
 *
 * What the document does NOT give, and is therefore a PROJECT CHOICE declared
 * in deploy_fdir_default_config() (see the rationale there):
 *   - how long one thermal-knife activation lasts (activation_ms),
 *   - how much longer each repetition is (activation_step_ms),
 *   - the ceiling on that growth (activation_max_ms),
 *   - the settle/observe gap between two activations (gap_ms).
 * The 10-attempt ceiling is the document's number, not a choice.
 *
 * Operational mapping (JOS states, App/obsw_types.h):
 *   INITIALIZATION MODE -> STATE_INIT
 *   SAFE MODE           -> STATE_CRIT
 * This module only decides; the state transition itself is performed by the
 * caller (state machine) using safe_mode_requested, because App/obsw is
 * outside the touch set of this change. Likewise it does NOT know which load
 * switch fires the knife: the caller installs a driver with
 * deploy_fdir_set_knife_driver(). Until one is installed the module still
 * counts and times the attempts and reports knife_on, so the logic is
 * testable and the hardware seam stays explicit.
 * ========================================================================== */

typedef struct {
    uint8_t  max_attempts;        /* activations before declaring failure */
    uint32_t activation_ms;       /* period of attempt #1                  */
    uint32_t activation_step_ms;  /* added to the period of each next one  */
    uint32_t activation_max_ms;   /* hard ceiling on the period            */
    uint32_t gap_ms;              /* knife-off settle/observe time         */
} deploy_fdir_config_t;

typedef enum {
    DEPLOY_FDIR_IDLE = 0,    /* not started (or not in INITIALIZATION) */
    DEPLOY_FDIR_RETRYING,    /* activations in progress                */
    DEPLOY_FDIR_DEPLOYED,    /* resolved by the deployment switch      */
    DEPLOY_FDIR_FAILED,      /* 10 attempts exhausted, fault ignored   */
    DEPLOY_FDIR_ABORTED      /* terminated by ground / leaving INIT    */
} deploy_fdir_result_t;

typedef struct {
    deploy_fdir_result_t result;
    uint8_t  attempts;               /* activations started, 0..max_attempts */
    bool     knife_on;               /* knife energised right now            */
    bool     degraded_telecom;       /* fallback: degraded TC operation      */
    bool     safe_mode_requested;    /* caller must enter STATE_CRIT         */
    uint32_t current_activation_ms;  /* period of the attempt in progress    */
} deploy_fdir_status_t;

/* Knife hardware seam. Called with true to energise, false to cut. The
 * integration installs this; with none installed the decision logic and the
 * status/telemetry counters keep working (that is what the host tests use). */
typedef void (*deploy_knife_driver_t)(bool on);

/* Reset to defaults and clear all state. Call once at boot (with
 * deploy_mux_init()). */
void deploy_fdir_init(void);

/* The declared defaults, with rationale (see the .c file). */
const deploy_fdir_config_t *deploy_fdir_default_config(void);

/* Override the configuration. Rejected (returns false, config unchanged) if
 * max_attempts is 0 or activation_ms is 0: a retry budget of zero or a
 * zero-length activation would silently disable the reaction. */
bool deploy_fdir_configure(const deploy_fdir_config_t *cfg);

/* Install the knife driver (NULL to detach). */
void deploy_fdir_set_knife_driver(deploy_knife_driver_t drv);

/* The configuration in force (never NULL). */
const deploy_fdir_config_t *deploy_fdir_config(void);

/* Period of the 1-based attempt `attempt_index`, with the growth applied and
 * capped. Attempt 0 and out-of-range indices are treated as attempt 1. */
uint32_t deploy_fdir_activation_ms(uint8_t attempt_index);

/* Advance the FDIR. `mode` is the current operational state, `now_ms` a
 * monotonic millisecond tick. Called periodically (100 ms cadence in flight):
 * new activations only start in STATE_INIT. Idempotent after it ends. */
void deploy_fdir_step(obw_state_t mode, uint32_t now_ms);

/* Exit condition "Telecommand received from ground": stop retrying. The TC
 * carries the target mode, so this does NOT force SAFE MODE. */
void deploy_fdir_ground_command(void);

/* Current status (never NULL). */
const deploy_fdir_status_t *deploy_fdir_status(void);

#endif /* DEPLOY_SENSE_H */
