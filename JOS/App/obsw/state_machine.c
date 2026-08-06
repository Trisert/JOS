#include "state_machine.h"
#include "memory.h"        /* laststates_write(): LastStates pool API */
#include "watchdog.h"
#include "boot_crc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "memory.h"         /* laststates_write() prototype */
#include "sram2_parity.h"   /* SRAM2_CRITICAL placement (W2-3) */
#include "seu_mitigation.h" /* redundant snapshot + scrubbing (W2-5) */
#include <string.h>

/* ---------- Private variables ---------- */
static osMutexId_t state_mutex;
static osThreadId_t sm_task_handle;

/* ---------- Critical OBSW state (SRAM2, hardware parity) ----------
   Operational state, beacon override and the last BMS snapshot decide every
   autonomous action of the spacecraft, so they live in the parity-protected
   SRAM2 block: a bit flip here raises an NMI and is contained by
   sram2_parity_nmi_handler() instead of silently steering the mission.
   The .sram2 image is copied from Flash by sram2_parity_init(), which MUST
   run before state_machine_init(); static initialisers below are therefore
   effective exactly as for ordinary .data. */
#define OBSW_STATE_MAGIC   0x4F53574DU   /* "OSWM" */

typedef struct {
    uint32_t     magic;                    /* integrity marker for ground     */
    obw_state_t  current_state;            /* current operational state       */
    uint32_t     beacon_interval_override; /* 0 = per-state default           */
    bms_status_t bms;                      /* latest battery snapshot (stub)  */
} obsw_critical_state_t;

static SRAM2_CRITICAL obsw_critical_state_t obsw_state = {
    .magic                    = OBSW_STATE_MAGIC,
    .current_state            = STATE_OFF,
    .beacon_interval_override = 0U,
    /* Stub BMS - per RED_DES_ElectronicArchitecture_V1:
         BQ76905 on EPS board via local I2C to EPS MCU,
         OBC queries EPS over subsystem SPI.
       Default voltage for 2S Li-ion: 7400 mV nominal. */
    .bms                      = {
        .soc        = 100,
        .temp_c     = 250,   /* 25.0 C */
        .voltage_mv = 7400,
    },
};

static const osThreadAttr_t sm_task_attrs = {
    .name       = "stateMachine",
    .stack_size = 256 * 4,
    .priority   = osPriorityAboveNormal,
};

/* Default battery thresholds */
static const bms_thresholds_t default_thresholds = {
    .b_opok   = 80,
    .b_commok = 60,
    .b_crit   = 40,
    .b_scrit  = 25,
};

static bms_status_t bms_get_status(void)
{
    /* TODO: replace with real subsystem SPI query to EPS MCU */
    return obsw_state.bms;
}

/* For testing: allow overriding SoC from outside */
void bms_set_soc_stub(uint8_t soc)
{
    /* Update and re-snapshot atomically: the scrub task must never observe
       the struct between the write and the commit, or it would "repair" a
       legitimate change back to the previous value (W2-5). */
    seu_mitigation_lock();
    obsw_state.bms.soc = soc;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
}

void *state_machine_critical_region(size_t *len)
{
    if (len != NULL) { *len = sizeof(obsw_state); }
    return &obsw_state;
}

/* ---------- LastStates logging ---------- */
static int laststates_log(uint8_t from, uint8_t to, uint8_t trigger,
                           const uint8_t *ctx, size_t ctx_len)
{
    laststates_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = (uint32_t)osKernelGetTickCount();
    entry.state_from = from;
    entry.state_to   = to;
    entry.trigger    = trigger;
    if (ctx && ctx_len > 0) {
        size_t n = (ctx_len < sizeof(entry.context)) ? ctx_len : sizeof(entry.context);
        memcpy(entry.context, ctx, n);
    }
    int rc = laststates_write(&entry);
    if (rc != 0) {
        /* Flash write failed: the transition cannot be considered safely
           persisted. Signal via the QM fault path (no silent success). */
        return -1;
    }
    return 0;
}

/* ---------- Stub: watchdog kick ---------- */
static void watchdog_kick(void)
{
    /* TODO: HAL_IWDG_Refresh(&hiwdg) when IWDG is configured */
}

/* ---------- State transition logic ---------- */
static int try_transition(obw_state_t target, uint8_t trigger)
{
    bms_status_t bms;
    int ok = 0;

    /* Boot-CRC safe state (docs/dev/hardening.md 2.4): while the running
       image cannot be trusted the OBC is confined to STATE_CRIT — beacon
       only, payloads and nominal ops inhibited — so ground can diagnose and
       re-upload. Only CRIT (and the INIT boot bookkeeping) stay reachable. */
    if ((boot_crc_image_trusted() == 0) &&
        (target != STATE_CRIT) && (target != STATE_INIT)) {
        return -1;
    }

    switch (target) {
    case STATE_INIT:
        /* Only valid from OFF (boot) */
        ok = (obsw_state.current_state == STATE_OFF);
        break;

    case STATE_CRIT:
        /* Any state can enter CRIT on low battery / critical event */
        ok = 1;
        break;

    case STATE_READY:
        /* s1→s3: after boot + antenna deploy success */
        /* s2→s3: battery recovers to b_opok */
        if (obsw_state.current_state == STATE_INIT) {
            ok = 1;  /* assume antenna deploy + self-test passed */
        } else if (obsw_state.current_state == STATE_CRIT) {
            bms = bms_get_status();
            ok = (bms.soc >= default_thresholds.b_opok);
        }
        break;

    case STATE_ACTIVE:
        /* s3→s4: scheduled task / ground command */
        /* s2→s4: ground command + stable battery */
        if (obsw_state.current_state == STATE_READY) {
            ok = 1;
        } else if (obsw_state.current_state == STATE_CRIT) {
            bms = bms_get_status();
            ok = (bms.soc >= default_thresholds.b_opok) &&
                 (trigger == TRIGGER_GROUND_CMD);
        }
        break;

    case STATE_OFF:
        /* Cannot transition to OFF in flight */
        ok = 0;
        break;
    }

    if (!ok) {
        return -1;
    }

    if (laststates_log((uint8_t)obsw_state.current_state, (uint8_t)target, trigger, NULL, 0) != 0) {
        /* LastStates persistence failed (Flash write/erase error). The
           transition still proceeds, but we flag it so the QM fault path
           can record the anomaly instead of silently reporting success. */
        return -1;
    }

    seu_mitigation_lock();
    obsw_state.current_state = target;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    return 0;
}

/* ---------- Autonomous battery check ---------- */
static void check_battery_autonomous(void)
{
    bms_status_t bms = bms_get_status();

    if (bms.soc <= default_thresholds.b_scrit) {
        try_transition(STATE_CRIT, TRIGGER_BATTERY_LOW);
    } else if (obsw_state.current_state == STATE_CRIT &&
               bms.soc >= default_thresholds.b_opok) {
        try_transition(STATE_READY, TRIGGER_BATTERY_OK);
    }
}

/* ---------- Main task loop ---------- */
static void state_machine_task(void *arg)
{
    (void)arg;

    /* Boot sequence: s0 → s1 → s3 */
    osDelay(pdMS_TO_TICKS(100));   /* let peripherals settle */

    osMutexAcquire(state_mutex, osWaitForever);
    try_transition(STATE_INIT, TRIGGER_BOOT);
    osMutexRelease(state_mutex);

    /* TODO: antenna deployment sequence + self-tests here */
    osDelay(pdMS_TO_TICKS(500));   /* placeholder for init work */

    osMutexAcquire(state_mutex, osWaitForever);
    if (boot_crc_image_trusted() != 0) {
        try_transition(STATE_READY, TRIGGER_ANTENNA_DONE);
    } else {
        /* Image integrity fault survived the reset budget: enter the safe
           state instead of nominal ops (beacon-only, payloads inhibited). */
        try_transition(STATE_CRIT, TRIGGER_IMAGE_CRC_FAIL);
    }
    osMutexRelease(state_mutex);

    /* 10 Hz main loop */
    for (;;) {
        watchdog_kick();
        watchdog_alive_self();

        osMutexAcquire(state_mutex, osWaitForever);
        check_battery_autonomous();
        osMutexRelease(state_mutex);

        osDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------- Public functions ---------- */

void state_machine_init(void)
{
    const osMutexAttr_t mtx_attrs = {
        .name      = "stateMtx",
        .attr_bits = osMutexPrioInherit,
    };
    state_mutex = osMutexNew(&mtx_attrs);

    /* sram2_parity_init() copies the .sram2 image out of Flash before the
       application starts. A wrong magic means that copy never happened (or
       the block is corrupted), so restore the compile-time defaults rather
       than running the mission on undefined data. */
    if (obsw_state.magic != OBSW_STATE_MAGIC) {
        (void)sram2_restore_from_image(&obsw_state, sizeof(obsw_state));
    }

    obsw_state.current_state            = STATE_OFF;
    obsw_state.beacon_interval_override = 0U;

    /* seu_mitigation_init() runs after this function and takes the first
       snapshot; the commit here is a no-op before that point and keeps the
       shadow in step if the state machine is ever re-initialised. */
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
}

osThreadId_t state_machine_task_create(void)
{
    sm_task_handle = osThreadNew(state_machine_task, NULL, &sm_task_attrs);
    if (sm_task_handle != NULL) {
        (void)watchdog_register_task(sm_task_handle, WDG_PERIOD_STATE_MACHINE_MS);
    }
    return sm_task_handle;
}

obw_state_t state_machine_get_state(void)
{
    obw_state_t s;
    osMutexAcquire(state_mutex, osWaitForever);
    s = obsw_state.current_state;
    osMutexRelease(state_mutex);
    return s;
}

int state_machine_request_transition(obw_state_t target, uint8_t trigger)
{
    int rc;
    osMutexAcquire(state_mutex, osWaitForever);
    rc = try_transition(target, trigger);
    osMutexRelease(state_mutex);
    return rc;
}

uint32_t state_machine_get_beacon_interval(void)
{
    uint32_t interval;
    osMutexAcquire(state_mutex, osWaitForever);
    if (obsw_state.beacon_interval_override != 0) {
        interval = obsw_state.beacon_interval_override;
    } else {
        obw_state_t s = obsw_state.current_state;
        switch (s) {
        case STATE_CRIT:   interval = BEACON_INTERVAL_CRIT;   break;
        case STATE_ACTIVE: interval = BEACON_INTERVAL_ACTIVE;  break;
        case STATE_READY:  interval = BEACON_INTERVAL_READY;   break;
        default:           interval = BEACON_INTERVAL_CRIT;     break;
        }
    }
    osMutexRelease(state_mutex);
    return interval;
}

int state_machine_set_beacon_interval(uint32_t interval_ms)
{
    /* Range check before the value can reach the beacon task or the watchdog.
       0 is the documented "clear the override / revert to per-state default"
       encoding. Any other value must be within the certified band: an
       interval longer than BEACON_INTERVAL_MAX would out-run the beacon
       watchdog period and permanently flag a healthy task, and one shorter
       than BEACON_INTERVAL_MIN would violate the RF duty-cycle budget.
       Out-of-range requests are rejected, leaving the current cadence intact
       (fail-safe: an erroneous or corrupted uplink cannot silence the beacon). */
    if ((interval_ms != 0u) &&
        ((interval_ms < BEACON_INTERVAL_MIN) || (interval_ms > BEACON_INTERVAL_MAX))) {
        return -1;
    }

    osMutexAcquire(state_mutex, osWaitForever);
    seu_mitigation_lock();
    obsw_state.beacon_interval_override = interval_ms;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    osMutexRelease(state_mutex);
    return 0;
}
