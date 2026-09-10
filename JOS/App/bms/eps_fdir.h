#ifndef EPS_FDIR_H
#define EPS_FDIR_H

#include <stdint.h>
#include <stdbool.h>

/* ===========================================================================
 * eps_fdir — FDIR detectors for the EPS charger IC and battery monitor IC.
 *
 * Source of the behaviour implemented here: RED_FDIR_V2.xlsx, sheet 'EPS'
 * (SYS SPF V3 / ANNEXES / ANNEX 09), cross-checked against SW_DATA_TYPES.xlsx
 * (sheet 'Data Types'), which places V_BAT, BAT_SOC, BAT_TEMP, I_BAT_CHG and
 * I_BAT_DSG on CHG_I2C at address 0x26 — i.e. the charger IC is read over I2C,
 * there is no separate "battery monitor MCU" the OBC queries over SPI.
 *
 * FDIR-EPS-EL-03  item "Charger"
 *   feared event  : charger stops charging the battery
 *   entry         : charging current is 0 mA for more than 60 min
 *   exit          : charger reset
 *   monitoring    : charging current read over I2C from the charger IC
 *   recovery      : EPS resets the charging IC
 *   modes         : active in all modes
 *
 * FDIR-EPS-EL-05  item "Battery monitor"
 *   feared event  : battery monitor stops communicating with the EPS uC
 *   entry         : battery monitor does not respond to the EPS uC query
 *   timeout       : does not respond for more than 10 s
 *   exit          : battery monitor resumes communication
 *   monitoring    : the messages sent by the battery monitor
 *   recovery      : EPS resets the battery monitor IC
 *
 * FDIR-EPS-EL-01 / FDIR-EPS-EL-04  item "EPS uC" / "uC Watchdog"
 *   The redundancy pair around the OBC<->EPS hardware heartbeat: the OBC can
 *   reset the EPS uC through the NRST pin "after TBC ms from the last HB".
 *   The sheet writes TBC for EVERY one of those windows and no other delivered
 *   document gives a number, so the heartbeat window is NOT defaulted to a
 *   guess — see EPS_FDIR_HB_TIMEOUT_UNARMED below.
 *
 * DOCUMENT STATUS: both spreadsheets are subteam working documents with no
 * release status (they still carry "TBC"/"da valutare" fields), so they are
 * the best available evidence, not an approved baseline. Every value used by
 * this module is either quoted from the sheet (marked "doc") or declared here
 * as a PROJECT choice (marked "project"); nothing is silently invented.
 *
 * WHAT THIS MODULE DOES NOT DO: it does not talk to any IC. It is pure
 * detection/recovery-decision logic driven by an input snapshot, and its
 * output is a request that the caller executes. That keeps it host-testable
 * (tests/) and keeps the I2C/NRST register-level transactions — which are NOT
 * specified anywhere in the delivered documentation — out of it. Exactly the
 * same trade bms.c already makes for the EPS SPI frame (see the pinned seam
 * in bms.c).
 * ========================================================================= */

/* --- Timeouts -------------------------------------------------------------- */

/* FDIR-EPS-EL-03 (doc): "0 mA for more than 60 min". */
#define EPS_FDIR_CHARGER_TIMEOUT_MS (60UL * 60UL * 1000UL)   /* 3 600 000 ms */

/* FDIR-EPS-EL-05 (doc): "does not respond ... for more than 10 s". */
#define EPS_FDIR_BATT_MON_TIMEOUT_MS (10UL * 1000UL)         /*    10 000 ms */

/* FDIR-EPS-EL-01/-04 heartbeat window (project, forced by the document).
 * RED_FDIR_V2.xlsx states the OBC-resets-EPS-via-NRST window as "TBC ms from
 * the last HB" and gives no value in the EPS sheet, the OBC sheet, or any
 * other delivered document. Picking a number here would be inventing a
 * requirement on a hardware reset line, so the default is UNARMED (0) and the
 * heartbeat detector stays silent until a caller supplies the confirmed value
 * via eps_fdir_config_t.eps_hb_timeout_ms. Unarmed is the conservative side
 * of "we do not reset the EPS on a timer we cannot justify". */
#define EPS_FDIR_HB_TIMEOUT_UNARMED 0UL

/* Any configured window must fit the modular elapsed-time test below: with a
 * 32-bit millisecond counter a window of 2^31 ms (24.8 days) is already
 * ambiguous against (now - start). eps_fdir_init() rejects a config that
 * exceeds this instead of silently wrapping. */
#define EPS_FDIR_TIMEOUT_MAX_MS 0x7FFFFFFFUL

/* --- Types ----------------------------------------------------------------- */

/* Which IC the caller must reset. EPS_FDIR_TARGET_EPS means "reset the EPS uC
 * through the OBC->EPS NRST line" (FDIR-EPS-EL-04), not an EPS-internal IC. */
typedef enum {
    EPS_FDIR_TARGET_CHARGER = 0,   /* charger IC on CHG_I2C @ 0x26 */
    EPS_FDIR_TARGET_BATT_MONITOR,  /* battery monitor IC             */
    EPS_FDIR_TARGET_EPS            /* EPS uC, via NRST from the OBC  */
} eps_fdir_target_t;

/* Reset requests produced by one eps_fdir_update() call.
 *
 * These are LEVELS, not pulses: a request stays asserted for as long as the
 * fault condition holds (or until the caller acknowledges it), so a caller
 * that polls slower than the detector cannot miss it. The caller executes the
 * reset and then calls eps_fdir_ack_reset(). */
typedef struct {
    bool charger_reset;        /* FDIR-EPS-EL-03 -> reset the charger IC      */
    bool batt_monitor_reset;   /* FDIR-EPS-EL-05 -> reset the battery monitor */
    bool eps_reset;            /* FDIR-EPS-EL-04 -> reset the EPS uC via NRST */
} eps_fdir_requests_t;

/* Detection windows, in milliseconds of the caller's monotonic tick.
 * 0 = UNARMED: that channel never requests a reset. Charge and battery-monitor
 * windows come from eps_fdir_config_default(); the heartbeat window must be
 * supplied explicitly (TBC — see EPS_FDIR_HB_TIMEOUT_UNARMED). */
typedef struct {
    uint32_t charger_timeout_ms;
    uint32_t batt_monitor_timeout_ms;
    uint32_t eps_hb_timeout_ms;
} eps_fdir_config_t;

/* One periodic state snapshot, as sampled by the caller.
 *
 * now_ms is the caller's monotonic millisecond tick (HAL_GetTick()). It may
 * be called at any rate and it may be any value: the wrap through zero is
 * handled. It must only ever be non-decreasing — a tick that goes BACKWARDS
 * is indistinguishable from a very large forward jump modulo 2^32, and would
 * look like an elapsed window.
 *
 * chg_current_ma / chg_sample_valid: I_BAT_CHG from the charger IC. The valid
 *   flag matters — a failed I2C read is NOT "0 mA" and must never be counted
 *   as evidence of a charger fault (see the note in eps_fdir.c).
 * batt_monitor_responsive: true when the battery monitor answered the last
 *   query; false when it did not. */
typedef struct {
    uint32_t now_ms;
    int16_t  chg_current_ma;
    bool     chg_sample_valid;
    bool     batt_monitor_responsive;
} eps_fdir_input_t;

/* Per-channel detector state. Public so the caller can place eps_fdir_t in
 * its own storage; treat the fields as private. */
typedef struct {
    uint32_t fault_start_ms;   /* tick when the current episode began   */
    bool     in_fault;         /* condition currently holds             */
    bool     latched;          /* reset requested, not yet cleared      */
} eps_fdir_channel_t;

typedef struct {
    eps_fdir_config_t   cfg;
    eps_fdir_channel_t  charger;
    eps_fdir_channel_t  batt_monitor;
    eps_fdir_channel_t  eps_hb;
    bool                hb_seen;      /* an EPS heartbeat was ever noted */
    uint32_t            last_hb_ms;   /* tick of the last EPS heartbeat  */
} eps_fdir_t;

/* --- API ------------------------------------------------------------------- */

/* Fill cfg with the document-derived defaults:
 *   charger      = EPS_FDIR_CHARGER_TIMEOUT_MS  (60 min, doc)
 *   batt monitor = EPS_FDIR_BATT_MON_TIMEOUT_MS (10 s,   doc)
 *   EPS heartbeat= EPS_FDIR_HB_TIMEOUT_UNARMED  (TBC,    project) */
void eps_fdir_config_default(eps_fdir_config_t *cfg);

/* Load a configuration and clear all detector state.
 * Returns 0 on success, -1 when ctx or cfg is NULL or a window exceeds
 * EPS_FDIR_TIMEOUT_MAX_MS. Call once before the first eps_fdir_update(). */
int eps_fdir_init(eps_fdir_t *ctx, const eps_fdir_config_t *cfg);

/* Record that an EPS heartbeat arrived at now_ms (FDIR-EPS-EL-01/-04).
 * Call it from the heartbeat receive path, not from a poll; a heartbeat is an
 * event, not a level. */
void eps_fdir_note_eps_heartbeat(eps_fdir_t *ctx, uint32_t now_ms);

/* Advance every detector with one snapshot and return the outstanding reset
 * requests. Safe to call at any rate; the module is rate-independent (windows
 * are measured between ticks, not counted in calls). A NULL ctx or in returns
 * "no request", never a fabricated fault. */
eps_fdir_requests_t eps_fdir_update(eps_fdir_t *ctx, const eps_fdir_input_t *in);

/* Tell the module that the caller has executed the reset for `target`, at
 * now_ms. If the fault condition still holds, the detection window is
 * restarted from now_ms — the reset must be given a full window to work
 * before another request is raised, and no retry policy is specified by the
 * document (unlike FDIR-COMM-EL-01, which does specify one). */
void eps_fdir_ack_reset(eps_fdir_t *ctx, eps_fdir_target_t target, uint32_t now_ms);

#endif /* EPS_FDIR_H */
