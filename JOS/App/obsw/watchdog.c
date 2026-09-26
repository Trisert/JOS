#include "watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "dual_bank.h"
#include "hw_watchdog.h"

/* Escalation back ends (#36).
 *
 * The Ceedling host build (JOS/test) compiles this file against fakes: its
 * cmsis_os.h declares only the RTOS surface App/ used up to now (no
 * osThreadSuspend()), and Ceedling links a test executable from the headers
 * the *test* file includes, so App/memory/memory.c - and therefore
 * laststates_write() - is not on the link line of test_watchdog.out.
 *
 * Same opt-out idiom as seu_mitigation.h's HOST_UNIT_TEST switch: derive the
 * switch from HOST_UNIT_TEST (defined for every test executable in
 * test/project.yml) instead of requiring a second -D. The *policy* below - which task is
 * flagged, when, and once only - is compiled and exercised on the host; only
 * the two platform back ends (Flash-backed log, scheduler suspend) become
 * no-ops there. Wiring them into the host suite needs test-side scaffolding
 * (osThreadSuspend() in test/fakes/cmsis_os.h, "memory.h" in
 * test/test/test_watchdog.c) and is deliberately left out of this change,
 * which touches flight code only.
 */
#if defined(HOST_UNIT_TEST) && !defined(WDG_NO_ESCALATION_BACKEND)
#define WDG_NO_ESCALATION_BACKEND 1
#endif

#ifndef WDG_NO_ESCALATION_BACKEND
#include "memory.h"      /* laststates_write(): LastStates forensic pool  */
#include "obsw_types.h"  /* laststates_entry_t, TRIGGER_WATCHDOG          */
#include <string.h>
#endif

#ifdef HOST_UNIT_TEST
/* Host double for the pool-holder seam (test/support/rtos_stubs.c): the pool
   lock is stubbed out on the host, so the holder query behind the
   suspend/defer policy is driven by the test instead of by memory.c. Same
   signature as the flight version in memory.h (TaskHandle_t via task.h
   above). */
TaskHandle_t laststates_pool_holder(void);
#endif

/* ---------- Monitored task entry ---------- */
typedef struct {
    osThreadId_t handle;
    uint32_t     expected_period_ms;
    uint32_t     last_tick;
    uint8_t      registered;
    /* 0 until the task has reported liveness at least once. An unarmed entry
       is judged against the start-up grace window instead of 3x its period,
       because its last_tick is only a seed (see watchdog.h). */
    uint8_t      armed;
    /* 0 until last_tick holds a tick sampled *after* osKernelStart(). Tasks
       registered from main() are seeded by the monitor on its first scan. */
    uint8_t      seeded;
    /* 1 once the monitor has escalated this entry (logged it and suspended
       the task). Latched, because a task that stays silent - which a
       suspended one does by construction - must be reported once, not on
       every 500 ms scan until the LastStates pool is full. Cleared by
       watchdog_alive(), so a task ground puts back with osThreadResume() is
       monitored again from its next liveness report. NOT set when the
       escalation is deferred (suspect holds the pool mutex): the next scan
       retries instead, so a transient holder still gets escalated. */
    uint8_t      stalled;
    /* Last stack high-water mark sampled by the monitor scan, in words
       (uxTaskGetStackHighWaterMark() units), not bytes. 0 until the first
       scan has sampled this entry. Read out through
       watchdog_task_stack_hwm() for beacon/housekeeping telemetry. */
    UBaseType_t  stack_hwm_words;
} wdg_entry_t;

static wdg_entry_t wdg_tasks[WDG_MAX_TASKS];
static osMutexId_t wdg_mutex;

/* Escalations deferred because the suspect held the LastStates pool mutex
   (see watchdog_escalate_stalled()). Saturating-free 32-bit counter, zero
   after reset / watchdog_monitor_init(): the deferral leaves no Flash record
   by construction (writing one would wedge on the held mutex), so this
   counter is what makes it visible to ground instead of silent. */
static uint32_t wdg_holder_deferral_count;

/* Deadline helper: 3x the declared period, saturating instead of wrapping.
   WDG_PERIOD_CLOUD_MS is already 5 400 000 ms, so the multiplication is worth
   guarding before someone declares a bigger one. */
static uint32_t wdg_silence_limit_ms(uint32_t period_ms)
{
    return (period_ms > (UINT32_MAX / 3u)) ? UINT32_MAX : (period_ms * 3u);
}

/* A tick sampled before the scheduler runs is meaningless (xTaskGetTickCount()
   reads 0 and stays there), so registrations from main() are marked unseeded
   and the monitor stamps them with the first real tick it sees. */
static uint8_t wdg_kernel_is_running(void)
{
    return (osKernelGetState() == osKernelRunning) ? 1u : 0u;
}

/* ---------- Public functions ---------- */

/* Consecutive deferrals of the SAME suspect (reset when another task stalls
   or an escalation is carried out). */
static osThreadId_t wdg_deferred_suspect;
static uint32_t     wdg_consecutive_deferrals;

void watchdog_monitor_init(void)
{
    const osMutexAttr_t mtx_attrs = {
        .name      = "wdgMtx",
        .attr_bits = osMutexPrioInherit,
    };
    wdg_mutex = osMutexNew(&mtx_attrs);

    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        wdg_tasks[i].registered = 0;
        wdg_tasks[i].armed      = 0;
        wdg_tasks[i].seeded     = 0;
        wdg_tasks[i].stalled    = 0;
        wdg_tasks[i].stack_hwm_words = (UBaseType_t)0;
    }
    wdg_holder_deferral_count = 0u;
    wdg_deferred_suspect      = NULL;
    wdg_consecutive_deferrals = 0u;
}

int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms)
{
    if ((wdg_mutex == NULL) || (handle == NULL) || (expected_period_ms == 0u)) {
        /* Not initialised yet, or nonsense arguments: refuse rather than
           silently pretend the task is monitored. */
        return -1;
    }

    osMutexAcquire(wdg_mutex, osWaitForever);
    const uint8_t running = wdg_kernel_is_running();
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            /* Already registered (e.g. task re-created): refresh in place. */
            wdg_tasks[i].expected_period_ms = expected_period_ms;
            wdg_tasks[i].last_tick          = running ? xTaskGetTickCount() : 0u;
            wdg_tasks[i].seeded             = running;
            /* Re-registration is not a liveness report: a task that
               re-declares its period gets a fresh grace window rather than
               inheriting the previous armed state. */
            wdg_tasks[i].armed              = 0u;
            /* A re-created task is a fresh deadline, not the stalled one
               that was escalated: clear the latch or a second stall of the
               same handle would never be reported. */
            wdg_tasks[i].stalled            = 0u;
            osMutexRelease(wdg_mutex);
            return 0;
        }
    }
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (!wdg_tasks[i].registered) {
            wdg_tasks[i].handle            = handle;
            wdg_tasks[i].expected_period_ms = expected_period_ms;
            wdg_tasks[i].last_tick          = running ? xTaskGetTickCount() : 0u;
            wdg_tasks[i].seeded             = running;
            wdg_tasks[i].armed              = 0u;
            wdg_tasks[i].stalled            = 0u;
            wdg_tasks[i].registered         = 1;
            osMutexRelease(wdg_mutex);
            return 0;
        }
    }
    osMutexRelease(wdg_mutex);
    return -1;  /* no slots */
}

/* `handle` is only compared against the registered handles, never dereferenced
 * or reassigned, so it is declared const (cppcheck constParameter). The
 * top-level const does not change the function type, so the prototype in
 * watchdog.h stays ABI-compatible. */
void watchdog_alive(const osThreadId_t handle)
{
    if ((wdg_mutex == NULL) || (handle == NULL)) {
        return;
    }

    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            wdg_tasks[i].last_tick = xTaskGetTickCount();
            /* First real report from the task: leave the grace window and
               start judging it against its declared period. */
            wdg_tasks[i].seeded    = 1u;
            wdg_tasks[i].armed     = 1u;
            /* The task is talking again (ground resumed it, or it was never
               really dead): re-arm the escalation so a later stall is
               reported instead of being swallowed by the latch. */
            wdg_tasks[i].stalled   = 0u;
            break;
        }
    }
    osMutexRelease(wdg_mutex);
}

void watchdog_alive_self(void)
{
    watchdog_alive(osThreadGetId());
}

/* ---------- Monitor task ---------- */

/* Boot-success declaration (W2-2, dual-bank fallback).
 *
 * The boot-fault window must stay open until the system has proved it can
 * actually run, not merely reach osKernelStart(): the faults that matter
 * (task creation, driver bring-up, first payload access) happen after the
 * scheduler starts. The monitor task therefore declares the boot good only
 * once DUAL_BANK_BOOT_OK_UPTIME_MS of scheduler uptime has elapsed, and
 * retries a bounded number of times if the marker cannot be persisted
 * (LastStates pool exhausted) instead of dropping the failure on the floor. */
#define WDG_BOOT_OK_MAX_RETRIES  3U

static void watchdog_declare_boot_ok_if_due(uint32_t uptime_ms)
{
    static uint8_t boot_ok_done    = 0U;
    static uint8_t boot_ok_retries = 0U;

    if (boot_ok_done) {
        return;
    }
    if (uptime_ms < DUAL_BANK_BOOT_OK_UPTIME_MS) {
        return;
    }

    if (dual_bank_boot_complete() == 0) {
        boot_ok_done = 1U;
        return;
    }

    /* Could not persist the boot-OK marker. dual_bank keeps the pending flag
       and stops trusting the frozen Flash evidence, so the failure is safe —
       but do not retry for ever. */
    boot_ok_retries++;
    if (boot_ok_retries >= WDG_BOOT_OK_MAX_RETRIES) {
        boot_ok_done = 1U;
    }
}

/* ---------- Suspend/defer policy ---------- */

/* Whether the monitor may suspend `suspect` given the current pool holder.
 *
 * NEVER suspend the task that holds the LastStates pool mutex: suspending a
 * mutex holder wedges every later laststates_write() - including the
 * monitor's own escalation record - on an osWaitForever acquire its holder
 * can no longer release. Skipping the suspend alone is not enough either: the
 * escalation record is written AFTER the suspend, so it would wedge the
 * monitor on the same held mutex. The only safe action is to defer the whole
 * escalation (no suspend, no Flash write) and retry on the next scan, when
 * the holder has normally released the mutex long ago.
 *
 * In the CMSIS-RTOS2 FreeRTOS wrapper an osThreadId_t and a TaskHandle_t are
 * the same TCB pointer, so the suspect is compared directly against the
 * holder read by laststates_pool_holder() (xQueueGetMutexHolder(),
 * INCLUDE_xQueueGetMutexHolder = 1 in FreeRTOSConfig.h).
 *
 * Returns 1 when the suspect may be suspended now, 0 when the escalation
 * must be deferred. Compiled in both builds so the host suite exercises the
 * exact flight decision (see watchdog.h). */
int watchdog_suspend_allowed(osThreadId_t suspect, osThreadId_t pool_holder)
{
    if (suspect == NULL) {
        return 0;
    }
    if (pool_holder == NULL) {
        return 1;
    }
    return ((TaskHandle_t)suspect != (TaskHandle_t)pool_holder) ? 1 : 0;
}

uint32_t watchdog_holder_deferrals(void)
{
    return wdg_holder_deferral_count;
}

int watchdog_task_stack_hwm(osThreadId_t handle, UBaseType_t *hwm_words)
{
    int rc = -1;

    if ((handle == NULL) || (hwm_words == NULL)) {
        return -1;
    }

    osMutexAcquire(wdg_mutex, osWaitForever);
    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        if (wdg_tasks[i].registered && wdg_tasks[i].handle == handle) {
            *hwm_words = wdg_tasks[i].stack_hwm_words;
            rc = 0;
            break;
        }
    }
    osMutexRelease(wdg_mutex);
    return rc;
}

/* Reaction to a task that missed its liveness deadline.
 *
 * Policy: RECORD, then RESET the OBC. (It used to be "suspend, then record",
 * which left the spacecraft with no recovery path at all: the IWDG/STWD100
 * keep being kicked by this task unconditionally, and "ground can put it back
 * with osThreadResume()" was not true - no telecommand does that, and if the
 * stalled task was loraRX the command could not even be received. A
 * suspended RX task meant a deaf satellite for the rest of the mission, a
 * suspended stateMachine meant no autonomous battery protection.)
 *
 *  - Record through laststates_write() with TRIGGER_WATCHDOG, so the anomaly
 *    reaches ground over the existing LastStates downlink after the reset.
 *    state_from/state_to are 0xFF: this is a fault record, not a state
 *    transition — same convention as mpu_fault_log_flush().
 *
 *  - dual_bank_mark_boot_fault() before the reset, exactly like the fault
 *    handlers (Core/Src/faults.c): a task that hangs on every boot is then a
 *    boot loop the golden-image fallback can see (DUAL_BANK_BOOT_FAULT_
 *    THRESHOLD), while an occasional hang after the boot was declared good
 *    is cleared by the next boot-OK marker.
 *
 *  - The record is only written when the stalled task does NOT hold the
 *    LastStates pool mutex (watchdog_suspend_allowed()): writing it would
 *    wedge this task on the held mutex. Such an escalation is deferred and
 *    retried on the next scan (the holder normally releases in bounded
 *    time), but only WDG_MAX_HOLDER_DEFERRALS times in a row - a task that
 *    hangs INSIDE the pool lock would otherwise be deferred for ever. After
 *    that the OBC resets without the record.
 *
 * Called with wdg_mutex RELEASED. Holding the monitor mutex across a Flash
 * program and a reset would block every watchdog_alive() caller.
 *
 * Returns the action taken (watchdog_escalation_action()). On the host the
 * record and the reset are compiled out; the decision is exercised in full.
 */
#define WDG_ESCALATE_HANDLED   0
#define WDG_ESCALATE_DEFERRED  1

#ifdef HOST_UNIT_TEST
static int wdg_last_action = WDG_ACTION_NONE;
static uint32_t wdg_resets_requested;

int watchdog_last_escalation_action(void) { return wdg_last_action; }
uint32_t watchdog_escalation_resets(void) { return wdg_resets_requested; }
#endif

int watchdog_escalation_action(osThreadId_t suspect, osThreadId_t pool_holder,
                               uint32_t consecutive_deferrals)
{
    if (suspect == NULL) {
        return WDG_ACTION_NONE;
    }
    if (watchdog_suspend_allowed(suspect, pool_holder) != 0) {
        return WDG_ACTION_RECORD_AND_RESET;
    }
    if (consecutive_deferrals < WDG_MAX_HOLDER_DEFERRALS) {
        return WDG_ACTION_DEFER;
    }
    return WDG_ACTION_RESET_UNRECORDED;
}

static int watchdog_escalate_stalled(osThreadId_t handle,
                                     uint32_t     elapsed_ms,
                                     uint32_t     limit_ms,
                                     uint32_t     period_ms)
{
    if (handle != wdg_deferred_suspect) {
        wdg_deferred_suspect      = handle;
        wdg_consecutive_deferrals = 0u;
    }

    /* Holder query FIRST, in both builds: laststates_pool_holder() is a
       non-blocking xQueueGetMutexHolder() read (flight) or the host double
       (test). */
    const int action = watchdog_escalation_action(
        handle, (osThreadId_t)laststates_pool_holder(),
        wdg_consecutive_deferrals);

#ifdef HOST_UNIT_TEST
    wdg_last_action = action;
#endif

    if (action == WDG_ACTION_NONE) {
        return WDG_ESCALATE_HANDLED;
    }
    if (action == WDG_ACTION_DEFER) {
        wdg_holder_deferral_count++;
        wdg_consecutive_deferrals++;
        return WDG_ESCALATE_DEFERRED;
    }

    wdg_deferred_suspect      = NULL;
    wdg_consecutive_deferrals = 0u;

#ifdef WDG_NO_ESCALATION_BACKEND
    (void)elapsed_ms;
    (void)limit_ms;
    (void)period_ms;
#ifdef HOST_UNIT_TEST
    wdg_resets_requested++;
#endif
    return WDG_ESCALATE_HANDLED;
#else
    if (action == WDG_ACTION_RECORD_AND_RESET) {
        laststates_entry_t entry;
        uint32_t           ctx[6];

        (void)memset(&entry, 0, sizeof(entry));
        entry.timestamp  = (uint32_t)osKernelGetTickCount();
        entry.state_from = 0xFFU;   /* not a transition: fault record */
        entry.state_to   = 0xFFU;
        entry.trigger    = (uint8_t)TRIGGER_WATCHDOG;

        /* Marker so ground can find a watchdog record inside a context blob,
           the way dual_bank tags its entries 'DBNK'. */
        ctx[0] = 0x474F4457UL;                  /* 'WDOG' (little-endian)  */
        ctx[1] = (uint32_t)(uintptr_t)handle;   /* which task went silent  */
        ctx[2] = elapsed_ms;                    /* how long it was silent  */
        ctx[3] = limit_ms;                      /* the deadline it blew    */
        ctx[4] = period_ms;                     /* the period it declared  */
        ctx[5] = (uint32_t)action;              /* WDG_ACTION_* taken      */
        (void)memcpy(entry.context, ctx, sizeof(ctx));

        /* Best effort: the reset below is the containment, the record the
           evidence. laststates_write() counts its own drops. */
        (void)laststates_write(&entry);
    }

    dual_bank_mark_boot_fault();
    __DSB();
    NVIC_SystemReset();
    return WDG_ESCALATE_HANDLED;   /* not reached */
#endif
}

static void watchdog_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t now = xTaskGetTickCount();
        /* Filled in by the scan below when it flags a task; acted on after
           the mutex is released (see watchdog_escalate_stalled()). */
        osThreadId_t stalled_handle  = NULL;
        int          stalled_idx     = -1;
        uint32_t     stalled_elapsed = 0u;
        uint32_t     stalled_limit   = 0u;
        uint32_t     stalled_period  = 0u;

        /* Refresh the independent hardware watchdog. This task is the single
           owner of the refresh once the scheduler is running: it is
           osPriorityHigh and runs every 500 ms against a ~31 s IWDG timeout,
           so the backstop only fires when the OBSW has genuinely stopped
           executing (LOCKUP, a spin in an exception handler, a hang in
           Error_Handler) rather than when it is merely busy.

           Deliberately unconditional and at the top of the loop: making the
           kick depend on the task-liveness scan below would couple the
           hardware backstop to the software monitor's own correctness, and a
           bug there would turn into a reset storm. Escalating a late task is
           the scan's job (watchdog_escalate_stalled() below), not the
           IWDG's. */
        hw_watchdog_kick();

        watchdog_declare_boot_ok_if_due(now * portTICK_PERIOD_MS);

        osMutexAcquire(wdg_mutex, osWaitForever);
        for (int i = 0; i < WDG_MAX_TASKS; i++) {
            if (!wdg_tasks[i].registered) continue;

            /* Stack high-water mark telemetry (INCLUDE_uxTaskGetStackHighWaterMark
               = 1 in FreeRTOSConfig.h). Sampled for every registered entry on
               every scan: the call only reads the watermark pattern down from
               the stack top, never blocks, and at 12 x <=1 KB per 500 ms scan
               its cost is negligible. In the CMSIS-RTOS2 FreeRTOS wrapper an
               osThreadId_t IS the task handle, hence the cast. */
            wdg_tasks[i].stack_hwm_words =
                uxTaskGetStackHighWaterMark((TaskHandle_t)wdg_tasks[i].handle);

            /* Registered before osKernelStart(): last_tick is not a real
               tick. Stamp it with the first tick the monitor actually
               observes and give the task its grace window from here, instead
               of reporting a hang that only measures the boot sequence. */
            if (!wdg_tasks[i].seeded) {
                wdg_tasks[i].last_tick = now;
                wdg_tasks[i].seeded    = 1u;
                continue;
            }

            uint32_t elapsed = (now - wdg_tasks[i].last_tick)
                               * portTICK_PERIOD_MS;

            /* Armed tasks (they have reported at least once) are held to 3x
               their declared period. A task that has not reported yet is
               still in bring-up, so it gets the longer of that limit and the
               start-up grace window. */
            uint32_t limit = wdg_silence_limit_ms(wdg_tasks[i].expected_period_ms);
            if ((!wdg_tasks[i].armed) && (limit < WDG_STARTUP_GRACE_MS)) {
                limit = WDG_STARTUP_GRACE_MS;
            }

            if (elapsed > limit) {
                /* Flag here, act outside the lock. At most one escalation
                   per scan: handling one costs a Flash write, and two tasks
                   blowing their deadline inside the same 500 ms scan is
                   already a system-level event. The second one is left
                   unlatched on purpose, so the next scan picks it up rather
                   than swallowing it. */
                if ((!wdg_tasks[i].stalled) && (stalled_handle == NULL)) {
                    wdg_tasks[i].stalled = 1u;
                    stalled_handle  = wdg_tasks[i].handle;
                    stalled_idx     = i;
                    stalled_elapsed = elapsed;
                    stalled_limit   = limit;
                    stalled_period  = wdg_tasks[i].expected_period_ms;
                }
            }
        }
        osMutexRelease(wdg_mutex);

        if (stalled_handle != NULL) {
            if (watchdog_escalate_stalled(stalled_handle, stalled_elapsed,
                                          stalled_limit, stalled_period)
                == WDG_ESCALATE_DEFERRED) {
                /* The suspect holds the LastStates pool mutex: nothing was
                   written and no reset was taken. Clear the latch so the next
                   scan retries (a transient Flash-write holder releases the
                   mutex in bounded time); after WDG_MAX_HOLDER_DEFERRALS
                   retries the OBC resets without the record. The deferral
                   itself is counted inside watchdog_escalate_stalled(). */
                osMutexAcquire(wdg_mutex, osWaitForever);
                wdg_tasks[stalled_idx].stalled = 0u;
                osMutexRelease(wdg_mutex);
            }
        }

        /* Check every WDG_MONITOR_PERIOD_MS */
        osDelay(pdMS_TO_TICKS(WDG_MONITOR_PERIOD_MS));
    }
}

osThreadId_t watchdog_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "watchdog",
        /* 1 KB, not 512 B: this task calls dual_bank_boot_complete() ->
           ls_append(), whose Flash-programming path stacks HAL frames and can
           take an exception frame on top (W2-2 review): 32 B baseline
           Cortex-M frame, 104 B extended frame with FP state (+72 B, saved
           via lazy stacking; see FreeRTOSConfig.h). No task uses the FPU
           today, so the extended frame is not currently reachable — the
           1 KiB margin stays conservative regardless. The 128-byte LastStates
           entry itself is no longer a local (see dual_bank.c), but the margin
           is still needed. */
        .stack_size = 256 * 4,
        .priority   = osPriorityHigh,
    };
    return osThreadNew(watchdog_monitor_task, NULL, &attrs);
}
