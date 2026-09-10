#ifndef AOCS_H
#define AOCS_H

#include <stdint.h>
#include "cmsis_os.h"

/* ---------------------------------------------------------------------------
 * OBC-side AOCS contract (SPF V3 §3.3).
 *
 * ARCHITECTURE - the AOCS control law does NOT run on the OBC, and an earlier
 * revision of docs/api/aocs.md claimed otherwise. The satellite carries a
 * separate AOCS board with its own STM32L496VGT3; B-dot and the
 * nadir-pointing EKF execute there. On the subsystem SPI the OBC is MASTER
 * and the AOCS is SLAVE, so this module polls and reads. See
 * docs/api/aocs.md for the full contract and the blocker list.
 *
 * Only values the SPF actually fixes are defined here. The SPI frame format,
 * register map, baud/CS wiring, polling rate and scaling are NOT specified in
 * the delivered documentation, so no driver is written against them.
 * ------------------------------------------------------------------------- */

/* T_AOCS_MODE - "Current AOCS mode". Telemetry position 20, 1 B, Integer.
 * The SPF also asks ground to alert if detumbling persists longer than
 * expected; that is a ground-side check, not an OBC one. */
#define AOCS_TLM_MODE_POS          20U
#define AOCS_TLM_MODE_LEN          1U
#define AOCS_MODE_DETUMBLING       0U
#define AOCS_MODE_NADIR_POINTING   1U

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
