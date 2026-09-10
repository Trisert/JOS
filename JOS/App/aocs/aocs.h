#ifndef AOCS_H
#define AOCS_H

#include <stdint.h>
#include "cmsis_os.h"

/* ---------------------------------------------------------------------------
 * OBC-side AOCS contract (SPF V3 §3.3; subsystem data dictionary
 * SW_DATA_TYPES.xlsx, sheet "Data Types").
 *
 * ARCHITECTURE - the AOCS control law does NOT run on the OBC, and an earlier
 * revision of docs/api/aocs.md claimed otherwise. The satellite carries a
 * separate AOCS board with its own STM32L496VGT3; B-dot and the
 * nadir-pointing EKF execute there. On the subsystem SPI the OBC is MASTER
 * and the AOCS is SLAVE, so this module polls and reads. See
 * docs/api/aocs.md for the full contract and the blocker list.
 *
 * TWO DIFFERENT THINGS - DO NOT CONFLATE THEM:
 *
 *   1. AOCS_STATUS  - the SUBSYSTEM STATE. 2-bit field in the housekeeping
 *                     beacon STATUS byte (bits 3..4, 1-indexed), read by AOCS
 *                     on the INTERNAL bus. FOUR values: OFF, DET, POINTING,
 *                     FAULT (SW_DATA_TYPES.xlsx, row "AOCS_STATUS").
 *   2. T_AOCS_MODE  - a TELEMETRY parameter of the SPF operational database:
 *                     telemetry position 20, 1 B, Integer, TWO values (0/1).
 *                     It is a lossy projection of the state, NOT the state.
 *
 * An earlier revision of this header modelled the state as the 2-valued
 * T_AOCS_MODE field (0 = Detumbling, 1 = Nadir-Pointing). That is a
 * simplification the subsystem data dictionary does not support: an AOCS
 * FAULT was not representable. The 4-valued state now lives in
 * aocs_state_t; the 2-valued field is kept, separate and clearly named.
 *
 * Only values the delivered documentation actually fixes are defined here.
 * The SPI frame format, register map, baud/CS wiring, polling rate and
 * scaling are NOT specified, so no driver is written against them.
 * ------------------------------------------------------------------------- */

/* --- AOCS_STATUS: the subsystem state (4 values) --------------------------
 *
 * Source: SW_DATA_TYPES.xlsx, sheet "Data Types", row "AOCS_STATUS":
 *   Type=Digital, Size available/requirement = 2 bit, Read By=AOCS,
 *   Read BUS=INTERNAL, HB?=Yes, Notes="OFF, DET, POINTING, FAULT".
 * The sheet "HB" places the two bits in the beacon STATUS byte at bit 3 and
 * bit 4 (1-indexed), next to BAT_STATUS/LINES_FAULT and OBC_STATUS.
 *
 * ASSUMPTION, declared and not hidden: the xlsx lists the four names in the
 * order OFF, DET, POINTING, FAULT but gives no explicit numeric codes. The
 * enumerator values below follow that listing order (0..3). If the AOCS team
 * assigns different codes, only this enum changes; the telemetry conversion
 * below is written against the NAMES, not the numbers.
 */
#define AOCS_STATUS_BITS            2U
#define AOCS_STATUS_HB_BIT_FIRST    3U
#define AOCS_STATUS_HB_BIT_LAST     4U

typedef enum {
    AOCS_STATE_OFF      = 0,  /* AOCS unpowered / not executing   */
    AOCS_STATE_DET      = 1,  /* Detumbling (B-dot)               */
    AOCS_STATE_POINTING = 2,  /* Nadir-pointing (EKF)             */
    AOCS_STATE_FAULT    = 3,  /* AOCS declared FAULT              */
} aocs_state_t;

/* --- T_AOCS_MODE: the 1-byte telemetry projection (2 values) -------------
 *
 * Source: SPF operational database, "TELEMETRY PARAMETERS", T_AOCS_MODE,
 * telemetry position 20: 1 B, Integer, 0 = Detumbling, 1 = Nadir-Pointing.
 * The SPF also asks ground to alert if detumbling persists longer than
 * expected; that is a ground-side check, not an OBC one.
 *
 * The field is 1 byte (2 states used), so it can only carry the two values
 * below. It has NO DEFINED ENCODING for OFF or FAULT - a gap in the source
 * documents, not something to invent. The FOUR-valued AOCS_STATUS above is the
 * correct channel for OFF/FAULT; see aocs_state_to_tlm_mode().
 */
#define AOCS_TLM_MODE_POS          20U
#define AOCS_TLM_MODE_LEN          1U
#define AOCS_MODE_DETUMBLING       0U
#define AOCS_MODE_NADIR_POINTING   1U

/* --- Internal 4-state <-> 2-value telemetry projection --------------------
 *
 * T_AOCS_MODE is LOSSY: OFF and FAULT have no code in that field. These
 * helpers make the loss explicit instead of letting it happen by accident.
 *
 * Conservative encoding - DECLARED here because the source documents leave it
 * open: both OFF and FAULT project to AOCS_MODE_DETUMBLING (0). Rationale:
 * value 1 asserts "Nadir-Pointing", i.e. "the satellite is under control".
 * Emitting 1 while the AOCS is off or faulted would tell ground to stand
 * down when it must not. Projecting down to 0 never asserts a control
 * success that has not happened; the OFF/FAULT condition itself is reported
 * through AOCS_STATUS (2-bit), which is where the four states belong.
 * This is the choice declared in the PR body; it is reversible in one place.
 */
static inline uint8_t aocs_state_to_tlm_mode(aocs_state_t state)
{
    switch (state) {
    case AOCS_STATE_POINTING:
        return (uint8_t)AOCS_MODE_NADIR_POINTING;
    case AOCS_STATE_DET:
    case AOCS_STATE_OFF:
    case AOCS_STATE_FAULT:
    default:
        return (uint8_t)AOCS_MODE_DETUMBLING;
    }
}

/* Inverse projection. LOSSY AND NOT INVERTIBLE: the field carries only two
 * values, so a decoded mode can only ever be DET or POINTING. OFF and FAULT
 * are NOT recoverable from T_AOCS_MODE - read AOCS_STATUS for those. */
static inline aocs_state_t aocs_tlm_mode_to_state(uint8_t mode)
{
    return (mode == (uint8_t)AOCS_MODE_NADIR_POINTING)
               ? AOCS_STATE_POINTING
               : AOCS_STATE_DET;
}

/* T_ANGULAR_RATE - "Body angular rate (IMU ASM330LHHXTR), 3-axis", deg/s.
 * Telemetry position 21, 6 B total = 3 axes x 2 B, signed. Raw -> deg/s via
 * the IMU datasheet scale factor. */
#define AOCS_TLM_RATE_POS          21U
#define AOCS_TLM_RATE_LEN          6U
#define AOCS_TLM_RATE_AXES         3U
#define AOCS_TLM_RATE_BYTES_AXIS   2U

/* Rate thresholds. Each has a DIFFERENT role in the SPF - they are not
 * interchangeable, so they are not collapsed into one constant:
 *   TUMBLE   (SPF §1.5, LEOP "tumbling" definition): X/Y above 10 deg/s, Z
 *            above 20 deg/s. NOTE: §3.3.3 states the same predicate with
 *            "and" instead of "or" - an SPF self-contradiction recorded in
 *            docs/api/aocs.md, not resolved here.
 *   CRIT     (SPF operational database, T_ANGULAR_RATE): critical event if
 *            |omega| exceeds 10 deg/s on ANY axis.
 *   HANDOVER (SPF §3.6.1 notes): B-dot stays active in s3/s4 until the rate
 *            is below 5 deg/s on all axes, then the EKF takes over. */
#define AOCS_TUMBLE_RATE_XY_DPS    10U
#define AOCS_TUMBLE_RATE_Z_DPS     20U
#define AOCS_RATE_CRIT_DPS         10U
#define AOCS_DETUMBLE_HANDOVER_DPS 5U

void aocs_init(void);
void aocs_task(void *arg);

/* Create the AOCS polling task and register it with the watchdog monitor.
   Not called from main() yet — the subsystem SPI driver is still a stub. */
osThreadId_t aocs_task_create(void);

#endif /* AOCS_H */
