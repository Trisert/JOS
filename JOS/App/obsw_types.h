#ifndef OBW_TYPES_H
#define OBW_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ---------- Operational States ---------- */
typedef enum {
    STATE_OFF   = 0,   /* s0: system off, kill switches active */
    STATE_INIT  = 1,   /* s1: boot, antenna deployment, self-tests */
    STATE_CRIT  = 2,   /* s2: critical/safe mode, beacon only */
    STATE_READY = 3,   /* s3: idle, uplink listening, housekeeping */
    STATE_ACTIVE = 4,  /* s4: payload ops, PDT, scheduled tasks */
} obw_state_t;

#define OBW_STATE_COUNT 5

/* ---------- Battery SoC thresholds (percent) ---------- */
typedef struct {
    uint8_t b_opok;    /* ~80%: normal ops allowed */
    uint8_t b_commok;  /* intermediate: PDT ok, payloads suspended */
    uint8_t b_crit;    /* low: finish current PDT, then s2 */
    uint8_t b_scrit;   /* ~25%: abort PDT immediately, enter s2 */
} bms_thresholds_t;

/* ---------- LastStates entry ---------- */
#define LASTSTATES_ENTRY_SIZE 128
#define LASTSTATES_MAX_ENTRIES 64

typedef struct {
    uint32_t timestamp;          /* system tick at transition */
    uint8_t  state_from;         /* previous state */
    uint8_t  state_to;           /* new state */
    uint8_t  trigger;            /* what caused the transition */
    uint8_t  reserved[3];
    uint8_t  context[116];       /* free-form context data */
} laststates_entry_t;

/* State transition triggers */
enum {
    TRIGGER_BOOT            = 0,
    TRIGGER_BATTERY_LOW     = 1,
    TRIGGER_BATTERY_OK      = 2,
    TRIGGER_CRIT_EVENT      = 3,
    TRIGGER_GROUND_CMD      = 4,
    TRIGGER_TASK_COMPLETE   = 5,
    TRIGGER_ANTENNA_DONE    = 6,
    TRIGGER_WATCHDOG        = 7,
    TRIGGER_FAULT           = 8,  /* Cortex-M4 fault exception (see faults.h) */
    TRIGGER_STACK_OVERFLOW  = 9,  /* FreeRTOS stack overflow hook             */
};

/* ---------- BMS interface (stub for now) ---------- */
typedef struct {
    uint8_t soc;                 /* state of charge 0-100% */
    int16_t temp_c;              /* battery temperature (0.1 C units) */
    uint16_t voltage_mv;         /* battery voltage in mV */
} bms_status_t;

/* ---------- Beacon intervals per state (ms) ---------- */
#define BEACON_INTERVAL_CRIT    (16UL * 60 * 1000)  /* 16 min */
#define BEACON_INTERVAL_READY   ( 4UL * 60 * 1000)  /*  4 min */
#define BEACON_INTERVAL_ACTIVE  ( 1UL * 60 * 1000)  /*  1 min default */

/* Bounds accepted for a ground-commanded beacon interval override
   (CMD_SET_BEACON_INTERVAL). Values outside this range are rejected, never
   clamped silently, so an out-of-range telecommand is visible as a failure
   instead of quietly changing the cadence.

   - MIN protects the RF duty cycle and the TX chain from a command that would
     make the beacon task hammer the radio.
   - MAX is the slowest cadence the beacon watchdog is dimensioned for; it
     bounds the worst-case liveness detection time (3 x MAX) and prevents an
     uplinked interval from out-running the watchdog and permanently flagging
     a healthy task. */
#define BEACON_INTERVAL_MIN     (10UL * 1000)       /* 10 s  */
#define BEACON_INTERVAL_MAX     BEACON_INTERVAL_CRIT /* 16 min */

#endif /* OBW_TYPES_H */
