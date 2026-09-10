#ifndef AOCS_STATUS_H
#define AOCS_STATUS_H

/* ---------------------------------------------------------------------------
 * AOCS_STATUS - the subsystem state (4 values). SINGLE DEFINITION.
 *
 * This is the one and only definition of aocs_state_t. It lives in its own
 * header, ahead of aocs.h, for one concrete reason: aocs.h pulls in
 * "cmsis_os.h" (it declares the AOCS polling task), and a consumer that only
 * needs the STATE - the housekeeping beacon frame, App/obsw/beacon.[ch] - must
 * not acquire a HAL/RTOS dependency to name four values. aocs.h includes this
 * header, so anything that already includes aocs.h keeps seeing aocs_state_t
 * unchanged; beacon.h includes it too, so the two can never drift apart.
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
 * assigns different codes, only this enum changes; the telemetry conversion in
 * aocs.h is written against the NAMES, not the numbers.
 * ------------------------------------------------------------------------- */

#define AOCS_STATUS_BITS            2U
#define AOCS_STATUS_HB_BIT_FIRST    3U
#define AOCS_STATUS_HB_BIT_LAST     4U

typedef enum {
    AOCS_STATE_OFF      = 0,  /* AOCS unpowered / not executing   */
    AOCS_STATE_DET      = 1,  /* Detumbling (B-dot)               */
    AOCS_STATE_POINTING = 2,  /* Nadir-pointing (EKF)             */
    AOCS_STATE_FAULT    = 3,  /* AOCS declared FAULT              */
} aocs_state_t;

#endif /* AOCS_STATUS_H */
