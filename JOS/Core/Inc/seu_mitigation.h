/**
  ******************************************************************************
  * @file    seu_mitigation.h
  * @brief   Single-Event-Upset mitigation: periodic memory scrubbing (W2-5).
  *
  * SRAM2 hardware parity (W2-3, see sram2_parity.h) *detects* a corrupted byte
  * the moment it is read, but only when it is read: an upset in a structure
  * that is consulted once per orbit stays dormant until the worst moment, and
  * parity cannot correct anything - it can only force a recorded reset.
  *
  * This module adds the missing half of the protection:
  *
  *   1. Snapshot. At init every critical structure is copied into a shadow
  *      image and a CRC-32 of the pair is stored. The three legs of the vote
  *      live at three different addresses - live object, shadow copy and
  *      reference CRC - so a single upset can never take out more than one.
  *
  *   2. Scrub. A low-priority FreeRTOS task re-reads every registered region
  *      once per SEU_SCRUB_INTERVAL_MS, compares live / shadow / CRC and
  *      votes:
  *        - live matches the CRC              -> healthy, shadow refreshed
  *        - shadow matches the CRC            -> live is corrupt, REWRITTEN
  *        - live and shadow agree, CRC does not -> the CRC word flipped, it is
  *                                              recomputed
  *        - nothing agrees                    -> unrecoverable: the event is
  *                                              recorded and, for a region
  *                                              registered with
  *                                              SEU_ESCALATE_SAFE_STATE, the
  *                                              OBSW is contained (recorded
  *                                              reset, then beacon-only safe
  *                                              mode once the reset budget is
  *                                              spent). The corrupt object is
  *                                              never overwritten with the
  *                                              Flash load image: those are
  *                                              compile-time defaults
  *                                              (state OFF, soc 100 %) and
  *                                              restoring them in place would
  *                                              mask a dead battery instead of
  *                                              reporting a fault.
  *      Every non-healthy outcome is recorded in the LastStates pool with the
  *      number of flipped bits, so ground can trend the radiation environment.
  *
  *   3. Touch. Buffers whose content legitimately changes (the comms frames)
  *      cannot be voted on, so they are merely read word by word. On a part
  *      with the SRAM2 parity check enabled, that read is what surfaces a
  *      dormant upset as an NMI now, at a harmless moment, instead of in the
  *      middle of a pass.
  *
  *   4. Count. The parity NMI handler resets the MCU, so an in-RAM counter of
  *      SEU events would not survive its own event. seu_mitigation_nmi_hook()
  *      bumps a saturating counter in an RTC backup register (backup domain,
  *      survives a system reset) before sram2_parity_nmi_handler() records the
  *      context and resets; the next boot notices the increment and writes a
  *      LastStates summary record.
  *
  * Parity vs. voting - which correction is reachable when:
  *   - The shadow pool and the region table live in SRAM1, *not* in the
  *     parity-protected block. With the shadow in SRAM2 and the parity check
  *     enabled, reading a flipped shadow byte would raise an NMI and reboot
  *     the OBSW, so the "shadow was hit, rebuild it" branch could never run:
  *     a free correction would have been turned into a reset. In SRAM1 the
  *     shadow leg and the CRC leg are always correctable by the vote.
  *   - For a live object inside SRAM2 the parity hardware still wins the race
  *     on any odd number of flipped bits in a byte (recorded reset, W2-3).
  *     The vote covers what parity is blind to - an even number of flips
  *     inside one byte - and covers every live object outside SRAM2 (the
  *     LastStates bookkeeping) in full.
  *
  * Ownership contract for SEU_POLICY_GOLDEN regions: the scrubber assumes the
  * live object only changes when its owner says so. Every legitimate write to
  * such a region MUST be followed by seu_mitigation_commit(), and the write
  * plus the commit MUST be atomic with respect to the scrub task - wrap them
  * in seu_mitigation_lock() / seu_mitigation_unlock(). Missing a commit makes
  * the scrubber revert a legitimate update at the next cycle.
  *
  * Standards: NASA-STD-8739.8 (fault tolerance, data integrity, no silent
  *            failure), ECSS-E-ST-40C (recorded failure context),
  *            ECSS-Q-ST-80C Rev.2 (fault detection, isolation, recovery),
  *            NASA Power of Ten #3 (no dynamic allocation - fixed region
  *            table and a statically sized shadow pool).
  ******************************************************************************
  */

#ifndef SEU_MITIGATION_H
#define SEU_MITIGATION_H

#include <stdint.h>
#include <stddef.h>

#include "cmsis_os2.h"   /* osThreadId_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Build-time configuration ---------- */

/** Scrub period. 5 minutes is a compromise between the LEO upset rate (hours
 *  to days per device for this SRAM size) and the cost of the pass. */
#ifndef SEU_SCRUB_INTERVAL_MS
#define SEU_SCRUB_INTERVAL_MS   (5UL * 60UL * 1000UL)
#endif

/** Maximum number of registered regions (static table, no allocation). */
#ifndef SEU_MAX_REGIONS
#define SEU_MAX_REGIONS         8U
#endif

/** Bytes reserved for all shadow copies together (bump-allocated at init). */
#ifndef SEU_SHADOW_POOL_BYTES
#define SEU_SHADOW_POOL_BYTES   512U
#endif

/** Largest SEU_POLICY_GOLDEN region. Sizes the two staging buffers and is
 *  re-checked on every use, so a flipped length field can never overrun
 *  them. */
#ifndef SEU_MAX_REGION_BYTES
#define SEU_MAX_REGION_BYTES    256U
#endif

/** Largest SEU_POLICY_TOUCH region. Touch regions are only read, but a
 *  flipped length must not turn that read into a walk off the end of RAM. */
#ifndef SEU_MAX_TOUCH_BYTES
#define SEU_MAX_TOUCH_BYTES     1024U
#endif

/** How many times an unrecoverable corruption of a safety-critical region may
 *  answer with a reset before the OBSW stops rebooting and stays in the
 *  beacon-only safe mode instead. Bounds the damage of a hard-failed cell,
 *  which would otherwise reboot the spacecraft forever. */
#ifndef SEU_ESCALATION_RESET_LIMIT
#define SEU_ESCALATION_RESET_LIMIT  3U
#endif

/** Keep the cumulative SEU counter in an RTC backup register (survives the
 *  reset performed by the parity NMI handler). Set to 0 to drop the
 *  dependency on the backup domain. */
#ifndef SEU_USE_BACKUP_REGISTER
#define SEU_USE_BACKUP_REGISTER 1
#endif

/** RTC backup registers holding the SEU counter, the value already reported
 *  to ground and the escalation (reset) budget. Highest indices, to stay
 *  clear of the low registers commonly used for boot flags. */
#ifndef SEU_BKP_COUNT_INDEX
#define SEU_BKP_COUNT_INDEX     31U
#endif
#ifndef SEU_BKP_ACK_INDEX
#define SEU_BKP_ACK_INDEX       30U
#endif
#ifndef SEU_BKP_ESCALATION_INDEX
#define SEU_BKP_ESCALATION_INDEX 29U
#endif

/** Stack of the scrub task, in bytes (CMSIS-RTOS v2 convention). */
#ifndef SEU_SCRUB_TASK_STACK
#define SEU_SCRUB_TASK_STACK    (384U * 4U)
#endif

/* ---------- Regions ---------- */

/** Identifier of a protected region; also the index used by ground. */
typedef enum {
    SEU_REGION_OBSW_STATE   = 0,  /* state machine critical struct  (SRAM2) */
    SEU_REGION_LASTSTATES   = 1,  /* LastStates pool bookkeeping    (SRAM1) */
    SEU_REGION_COMMS_BEACON = 2,  /* beacon staging buffer          (SRAM2) */
    SEU_REGION_COMMS_RX     = 3,  /* uplink decode buffer           (SRAM2) */
    SEU_REGION_COMMS_TX     = 4,  /* downlink chunk buffer          (SRAM2) */
    SEU_REGION_ID_COUNT     = 5,
} seu_region_id_t;

/** How the scrubber treats a region. */
typedef enum {
    /** Content is stable between explicit commits: vote and rewrite. */
    SEU_POLICY_GOLDEN = 0,
    /** Content changes freely: only read it, so the parity hardware gets a
     *  chance to report a dormant upset. No comparison, no rewrite. */
    SEU_POLICY_TOUCH  = 1,
} seu_policy_t;

/** What to do when a region is corrupted beyond repair (no two legs agree). */
typedef enum {
    /** Record the loss and leave the live object alone. Correct for data
     *  whose history is worth more than its consistency (the LastStates
     *  bookkeeping) and for buffers that are rewritten anyway. */
    SEU_ESCALATE_NONE       = 0,
    /** The object steers the spacecraft and cannot be trusted any more:
     *  contain it. A recorded NVIC_SystemReset() re-establishes the boot-path
     *  invariants, up to SEU_ESCALATION_RESET_LIMIT times across the mission
     *  (counter in the backup domain); after that the OBSW is forced into the
     *  beacon-only STATE_CRIT instead of rebooting again. */
    SEU_ESCALATE_SAFE_STATE = 1,
} seu_escalation_t;

/** Outcome of scrubbing one region. */
typedef enum {
    SEU_RESULT_HEALTHY      = 0,
    SEU_RESULT_TOUCHED      = 1,  /* SEU_POLICY_TOUCH region read back      */
    SEU_RESULT_REPAIRED     = 2,  /* live object rewritten from the shadow  */
    SEU_RESULT_CRC_REFRESH  = 3,  /* both copies agreed, the CRC word flipped */
    SEU_RESULT_UNRECOVERED  = 4,  /* no two copies agree                    */
    SEU_RESULT_INVALID      = 5,  /* the region descriptor itself is corrupt */
} seu_scrub_result_t;

/* ---------- LastStates records ---------- */

/** Marker so ground can find an SEU record inside a LastStates context blob. */
#define SEU_RECORD_MAGIC        0x53455552U   /* "SEUR" */

/** Event ids used in seu_record_t.event_id. */
enum {
    SEU_EVENT_SCRUB_REPAIR    = 0U,  /* corrupted live object rewritten     */
    SEU_EVENT_SCRUB_CRC       = 1U,  /* reference CRC recomputed            */
    SEU_EVENT_SCRUB_FAILED    = 2U,  /* unrecoverable mismatch              */
    SEU_EVENT_PARITY_HISTORY  = 3U,  /* boot after one or more parity NMIs  */
    SEU_EVENT_SHADOW_REPAIR   = 4U,  /* shadow copy rebuilt from the live   */
    SEU_EVENT_REGION_INVALID  = 5U,  /* region descriptor failed validation */
    SEU_EVENT_REGISTER_FAILED = 6U,  /* region / task could not be created  */
    SEU_EVENT_SAFE_STATE      = 7U,  /* containment action taken (C1)       */
};

/** Cumulative counters, readable over telemetry. */
typedef struct {
    uint32_t scrub_cycles;         /* completed scrub passes                */
    uint32_t regions_checked;      /* region checks performed               */
    uint32_t mismatches;           /* regions found corrupted               */
    uint32_t repairs;              /* live objects rewritten from the shadow */
    uint32_t shadow_repairs;       /* shadow copies rebuilt from the live   */
    uint32_t crc_refreshes;        /* reference CRC words repaired          */
    uint32_t unrecoverable;        /* mismatches with no trustworthy copy   */
    uint32_t escalations;          /* containment actions (reset / STATE_CRIT) */
    uint32_t region_faults;        /* region descriptors rejected as corrupt */
    uint32_t registration_failures;/* regions / tasks that could not be set up */
    uint32_t bit_errors;           /* total flipped bits observed           */
    uint32_t parity_events_boot;   /* sram2_parity_error_count() this boot  */
    uint32_t parity_events_total;  /* persistent counter (backup register)  */
    uint32_t parity_status;        /* sram2_parity_status_t at the last pass */
    uint32_t last_scrub_tick;      /* HAL_GetTick() of the last pass        */
} seu_stats_t;

/** Post-mortem record written to the LastStates pool (fits the 116 B blob). */
typedef struct {
    uint32_t magic;                /* SEU_RECORD_MAGIC                      */
    uint32_t event_id;             /* SEU_EVENT_*                           */
    uint32_t region_id;            /* seu_region_id_t                       */
    uint32_t region_addr;          /* first byte of the live object         */
    uint32_t region_len;           /* size of the live object               */
    uint32_t byte_errors;          /* bytes differing live vs shadow        */
    uint32_t bit_errors;           /* bits differing live vs shadow         */
    uint32_t first_bad_offset;     /* offset of the first differing byte    */
    uint32_t crc_live;             /* CRC-32 of the live object             */
    uint32_t crc_shadow;           /* CRC-32 of the shadow copy             */
    uint32_t crc_reference;        /* CRC-32 stored at the last commit      */
    seu_stats_t stats;             /* counters at the time of the event     */
} seu_record_t;

/* ---------- API ---------- */

/**
  * @brief  Snapshot the critical structures and arm the scrubber.
  * @note   MUST run after sram2_parity_init() (which erases SRAM2) and after
  *         the owners of the critical structures are initialised
  *         (state_machine_init(), laststates_init(), lora_init()), and before
  *         osKernelStart(). Registers the built-in regions and enables access
  *         to the backup-domain SEU counter. A registration that fails is
  *         counted in seu_stats_t.registration_failures and recorded in the
  *         LastStates pool - never silently ignored.
  */
void seu_mitigation_init(void);

/** @brief Create the low-priority scrub task. @retval thread id, NULL on failure
  *        (the failure is also counted and recorded in LastStates). */
osThreadId_t seu_scrub_task_create(void);

/**
  * @brief  Register a memory region with the scrubber.
  * @param  id         region identifier (also the telemetry index)
  * @param  addr       first byte of the live object (must be in SRAM1/SRAM2)
  * @param  len        size in bytes; <= SEU_MAX_REGION_BYTES for a golden
  *                    region (and <= the free space in the shadow pool),
  *                    <= SEU_MAX_TOUCH_BYTES for a touch region
  * @param  policy     SEU_POLICY_GOLDEN or SEU_POLICY_TOUCH
  * @param  escalation what to do if the region is ever corrupted beyond
  *                    repair (SEU_ESCALATE_NONE for a touch region)
  * @retval 0 on success, -1 on bad arguments or an exhausted table / pool.
  */
int seu_mitigation_register_region(seu_region_id_t id, void *addr, size_t len,
                                   seu_policy_t policy);

/**
  * @brief  Re-take the snapshot of a region after a legitimate update.
  * @retval 0 on success, -1 if the region is unknown, not initialised or its
  *         descriptor fails the bounds check.
  * @note   Cheap (a memcpy plus a CRC over a few tens of bytes) and safe to
  *         call before the RTOS starts. Call it inside the same
  *         seu_mitigation_lock() section as the update itself.
  */
int seu_mitigation_commit(seu_region_id_t id);

/**
  * @brief  Run one scrub pass over every registered region.
  * @retval number of regions found corrupted (0 = all healthy), -1 if the
  *         module is not initialised.
  * @note   Exposed so a telecommand or a ground-triggered self-test can force
  *         a pass without waiting for the periodic task.
  */
int seu_mitigation_scrub_once(void);

/** @brief Copy the cumulative counters out (telemetry). */
void seu_mitigation_get_stats(seu_stats_t *out);

/** @brief Total SEU events seen since power-on (scrub repairs + parity NMIs). */
uint32_t seu_mitigation_event_count(void);

/**
  * @brief  Interrupt-safe mutual exclusion against the scrub task.
  * @note   Implemented with PRIMASK, not with taskENTER_CRITICAL(), so it is
  *         also legal on the NMI path (where the FreeRTOS critical-section
  *         assertion would spin forever). Nesting is counted.
  */
void seu_mitigation_lock(void);
void seu_mitigation_unlock(void);

/**
  * @brief  NMI back end: count the SEU before the OBSW is reset.
  * @note   Called from NMI_Handler() ahead of sram2_parity_nmi_handler(),
  *         which never returns. Touches only an RTC backup register and a
  *         RAM counter - no HAL, no Flash, no logging.
  */
void seu_mitigation_nmi_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* SEU_MITIGATION_H */
