#include "state_machine.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* ---------- Private variables ---------- */
static obw_state_t current_state = STATE_OFF;
static osMutexId_t state_mutex;
static osThreadId_t sm_task_handle;

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

/* ---------- Stub: BMS (to be replaced by real driver) ---------- */
static bms_status_t bms_stub = {
    .soc        = 100,
    .temp_c     = 250,   /* 25.0 C */
    .voltage_mv = 4200,
};

static bms_status_t bms_get_status(void)
{
    /* TODO: replace with real BMS SPI read */
    return bms_stub;
}

/* For testing: allow overriding SoC from outside */
void bms_set_soc_stub(uint8_t soc)
{
    bms_stub.soc = soc;
}

/* ---------- Stub: LastStates logging ---------- */
static void laststates_log(uint8_t from, uint8_t to, uint8_t trigger,
                           const uint8_t *ctx, size_t ctx_len)
{
    /* TODO: replace with real Flash write at 0x08080000 */
    (void)from;
    (void)to;
    (void)trigger;
    (void)ctx;
    (void)ctx_len;
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

    switch (target) {
    case STATE_INIT:
        /* Only valid from OFF (boot) */
        ok = (current_state == STATE_OFF);
        break;

    case STATE_CRIT:
        /* Any state can enter CRIT on low battery / critical event */
        ok = 1;
        break;

    case STATE_READY:
        /* s1→s3: after boot + antenna deploy success */
        /* s2→s3: battery recovers to b_opok */
        if (current_state == STATE_INIT) {
            ok = 1;  /* assume antenna deploy + self-test passed */
        } else if (current_state == STATE_CRIT) {
            bms = bms_get_status();
            ok = (bms.soc >= default_thresholds.b_opok);
        }
        break;

    case STATE_ACTIVE:
        /* s3→s4: scheduled task / ground command */
        /* s2→s4: ground command + stable battery */
        if (current_state == STATE_READY) {
            ok = 1;
        } else if (current_state == STATE_CRIT) {
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

    laststates_log((uint8_t)current_state, (uint8_t)target, trigger, NULL, 0);
    current_state = target;
    return 0;
}

/* ---------- Autonomous battery check ---------- */
static void check_battery_autonomous(void)
{
    bms_status_t bms = bms_get_status();

    if (bms.soc <= default_thresholds.b_scrit) {
        try_transition(STATE_CRIT, TRIGGER_BATTERY_LOW);
    } else if (current_state == STATE_CRIT &&
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
    try_transition(STATE_READY, TRIGGER_ANTENNA_DONE);
    osMutexRelease(state_mutex);

    /* 10 Hz main loop */
    for (;;) {
        watchdog_kick();

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
    current_state = STATE_OFF;
}

osThreadId_t state_machine_task_create(void)
{
    sm_task_handle = osThreadNew(state_machine_task, NULL, &sm_task_attrs);
    return sm_task_handle;
}

obw_state_t state_machine_get_state(void)
{
    obw_state_t s;
    osMutexAcquire(state_mutex, osWaitForever);
    s = current_state;
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
    obw_state_t s = state_machine_get_state();
    switch (s) {
    case STATE_CRIT:   return BEACON_INTERVAL_CRIT;
    case STATE_ACTIVE: return BEACON_INTERVAL_ACTIVE;
    case STATE_READY:  return BEACON_INTERVAL_READY;
    default:           return BEACON_INTERVAL_CRIT;
    }
}
