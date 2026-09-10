#include "eps_fdir.h"

#include <string.h>

/* eps_fdir — detection/recovery decisions for the EPS charger and battery
 * monitor (FDIR-EPS-EL-03 / -05), plus the OBC side of the OBC<->EPS
 * heartbeat redundancy (FDIR-EPS-EL-04). See eps_fdir.h for the source
 * document, the exact quoted conditions and what is a project choice.
 *
 * No HAL, no I2C, no GPIO: the caller samples the ICs and executes the
 * resets. This file is therefore host-compilable and unit-tested.
 */

/* --- Robust elapsed-time test ---------------------------------------------
 *
 * Every window here is compared as
 *     (uint32_t)(now_ms - start_ms) > window_ms
 * which is exact modulo 2^32: unsigned subtraction is well defined even when
 * now_ms < start_ms because the tick counter rolled over, and the difference
 * is the true number of milliseconds elapsed as long as it is below 2^32
 * (a 32-bit ms counter wraps after 49.7 days, so the only way to lose meaning
 * is to ask a window longer than the counter itself). No overflow, no
 * signed comparison, no "now < start -> clamp to 0" hack: the modular
 * difference already handles the wrap, and eps_fdir_init() rejects windows
 * above EPS_FDIR_TIMEOUT_MAX_MS (< 2^31) so a wrap can never be confused with
 * a legitimate long silence.
 */
static bool eps_fdir_elapsed_gt(uint32_t now_ms, uint32_t start_ms, uint32_t window_ms)
{
    return ((uint32_t)(now_ms - start_ms)) > window_ms;
}

/* Advance one IC detector (charger, battery monitor).
 *
 * `in_fault` is the instantaneous verdict on the monitored condition;
 * `window_ms == 0` leaves the channel disarmed (never requests a reset).
 * The episode logic is deliberately one-shot: the first tick past the window
 * latches the request, and no further request is raised until either the
 * condition clears (exit condition) or the caller acknowledges the reset.
 * RED_FDIR_V2.xlsx specifies no retry count for either -EL-03 or -EL-05, so
 * none is invented.
 *
 * The heartbeat channel (-EL-04) does NOT use this helper: its window is
 * anchored to the last heartbeat, not to the moment its verdict flips. See
 * eps_fdir_update(). */
static void eps_fdir_channel_step(eps_fdir_channel_t *ch, uint32_t now_ms,
                                  uint32_t window_ms, bool in_fault,
                                  bool *request)
{
    if (window_ms == 0UL) {          /* unarmed channel (TBC / disabled) */
        ch->in_fault       = false;
        ch->latched        = false;
        ch->fault_start_ms = 0U;
        return;
    }

    if (!in_fault) {                 /* exit condition: clear everything */
        ch->in_fault       = false;
        ch->latched        = false;
        ch->fault_start_ms = 0U;
        return;
    }

    if (!ch->in_fault) {             /* entry condition: start a new window */
        ch->in_fault       = true;
        ch->fault_start_ms = now_ms;
        return;
    }

    if (ch->latched) {               /* already requested: hold the level */
        *request = true;
        return;
    }

    if (eps_fdir_elapsed_gt(now_ms, ch->fault_start_ms, window_ms)) {
        ch->latched = true;
        *request    = true;
    }
}

void eps_fdir_config_default(eps_fdir_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->charger_timeout_ms      = EPS_FDIR_CHARGER_TIMEOUT_MS;
    cfg->batt_monitor_timeout_ms = EPS_FDIR_BATT_MON_TIMEOUT_MS;
    cfg->eps_hb_timeout_ms       = EPS_FDIR_HB_TIMEOUT_UNARMED;
}

static bool eps_fdir_window_valid(uint32_t window_ms)
{
    return (window_ms <= EPS_FDIR_TIMEOUT_MAX_MS);
}

int eps_fdir_init(eps_fdir_t *ctx, const eps_fdir_config_t *cfg)
{
    if ((ctx == NULL) || (cfg == NULL)) {
        return -1;
    }
    if (!eps_fdir_window_valid(cfg->charger_timeout_ms) ||
        !eps_fdir_window_valid(cfg->batt_monitor_timeout_ms) ||
        !eps_fdir_window_valid(cfg->eps_hb_timeout_ms)) {
        return -1;
    }

    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = *cfg;
    return 0;
}

void eps_fdir_note_eps_heartbeat(eps_fdir_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    /* Re-anchor the heartbeat window on the heartbeat that just arrived. The
       next eps_fdir_update() recomputes the verdict from this tick and, being
       no longer silent, drops any latched request. */
    ctx->hb_ref_ms = now_ms;
}

eps_fdir_requests_t eps_fdir_update(eps_fdir_t *ctx, const eps_fdir_input_t *in)
{
    eps_fdir_requests_t out = { false, false, false };

    if ((ctx == NULL) || (in == NULL)) {
        return out;
    }

    /* FDIR-EPS-EL-03 — "charging current is 0 mA for more than 60 min".
       Only a COMPLETED read can assert "0 mA". A failed I2C transaction is no
       information, not evidence of a charger fault: it opens no episode and
       breaks any episode in progress, so a dead CHG_I2C bus cannot by itself
       trigger a charger reset (a link fault is a different failure mode, and
       resetting the charger IC would not fix it). I_BAT_CHG is a charge-only
       quantity, so a non-positive reading is treated as "not charging". */
    eps_fdir_channel_step(&ctx->charger, in->now_ms, ctx->cfg.charger_timeout_ms,
                          (in->chg_sample_valid && (in->chg_current_ma <= 0)),
                          &out.charger_reset);

    /* FDIR-EPS-EL-05 — "does not respond to the EPS uC query for more than
       10 s". The condition is binary by construction: the caller knows
       whether the query was answered. */
    eps_fdir_channel_step(&ctx->batt_monitor, in->now_ms,
                          ctx->cfg.batt_monitor_timeout_ms,
                          !in->batt_monitor_responsive,
                          &out.batt_monitor_reset);

    /* FDIR-EPS-EL-04 — "OBS resets the EPS uC via the NRST pin after TBC ms
       from the last heartbeat".

       This channel deliberately does NOT go through eps_fdir_channel_step().
       The IC channels measure how long their condition has been true, but the
       heartbeat requirement pins the reset to the LAST HEARTBEAT itself, so
       the window is measured from `hb_ref_ms` (that heartbeat, or the boot
       tick while the EPS has never been heard from) and the request is raised
       on the first tick past TBC. Handing the step function a "silent" verdict
       would open a second window when the verdict flips and delay NRST to
       2xTBC — a double count of one window — while the never-heard path,
       anchored to boot, would still fire at 1xTBC: the two paths of this one
       channel would disagree with each other. Both now ask for the reset at
       exactly TBC, the latency written in the document.

       The request is still a level: it stays asserted for as long as the
       silence lasts, and eps_fdir_ack_reset() gives an executed NRST a fresh
       window before another request.

       Only evaluated when the window is armed (nonzero); with the TBC default
       the channel stays silent. */
    if (ctx->cfg.eps_hb_timeout_ms != EPS_FDIR_HB_TIMEOUT_UNARMED) {
        if (eps_fdir_elapsed_gt(in->now_ms, ctx->hb_ref_ms,
                                ctx->cfg.eps_hb_timeout_ms)) {
            ctx->eps_hb.in_fault = true;
            ctx->eps_hb.latched  = true;
            out.eps_reset        = true;
        } else {
            ctx->eps_hb.in_fault = false;
            ctx->eps_hb.latched  = false;
        }
    } else {
        ctx->eps_hb.in_fault = false;
        ctx->eps_hb.latched  = false;
    }

    return out;
}

void eps_fdir_ack_reset(eps_fdir_t *ctx, eps_fdir_target_t target, uint32_t now_ms)
{
    eps_fdir_channel_t *ch;

    if (ctx == NULL) {
        return;
    }

    if (target == EPS_FDIR_TARGET_EPS) {
        /* FDIR-EPS-EL-04: unlike the IC channels, this window is anchored to
           the last heartbeat (or to boot), not to the tick the fault verdict
           flipped. Ack only moves the anchor when a request was actually
           outstanding: the caller has pulsed NRST and the EPS uC must get a
           full window to boot and resume its heartbeat before another request
           is raised. Acking a healthy heartbeat channel is a no-op — the
           anchor stays on the last heartbeat. */
        if (ctx->eps_hb.latched) {
            ctx->hb_ref_ms = now_ms;
        }
        ctx->eps_hb.latched  = false;
        ctx->eps_hb.in_fault = false;
        return;
    }

    switch (target) {
    case EPS_FDIR_TARGET_CHARGER:      ch = &ctx->charger;      break;
    case EPS_FDIR_TARGET_BATT_MONITOR: ch = &ctx->batt_monitor; break;
    default:                           return;                  /* not a target */
    }

    ch->latched = false;
    if (ch->in_fault) {
        ch->fault_start_ms = now_ms;   /* restart the window for this episode */
    } else {
        ch->fault_start_ms = 0U;
    }
}
