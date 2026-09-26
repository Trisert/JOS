#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "cmsis_os.h"
#include "obsw_types.h"   /* BEACON_INTERVAL_* */
#include <stdint.h>
#include <stddef.h>

/* Maximum number of tasks that can be monitored.
   Four tasks are monitored today (defaultTask, stateMachine, loraBeacon,
   loraRX). clear/cloud/aocs register inside their *_task_create() helpers,
   which main() does not call yet (payload/AOCS bring-up pending) — they are
   covered the moment they are enabled. The monitor task itself is
   deliberately not monitored. See docs/dev/hardening.md 3.1. */
#define WDG_MAX_TASKS 12

/* Nominal loop periods (ms) declared by each monitored task at registration.
   The monitor flags a task once it has been silent for more than 3x its
   declared period (see watchdog_monitor_task()). Kept here so the periods
   used across App/ are visible in one place. */
#define WDG_PERIOD_DEFAULT_TASK_MS   1000u
#define WDG_PERIOD_STATE_MACHINE_MS   100u
#define WDG_PERIOD_LORA_RX_MS         100u
/* Bootstrap period for the beacon task only: the slowest cadence the beacon
   is ever allowed to run at (see BEACON_INTERVAL_MAX). lora_beacon_task()
   re-registers itself with the cadence actually in force on every change, so
   this value only bounds the very first monitoring window. */
#define WDG_PERIOD_LORA_BEACON_MS   BEACON_INTERVAL_MAX
#define WDG_PERIOD_CLEAR_MS          1000u
#define WDG_PERIOD_CLOUD_MS         (90UL * 60UL * 1000UL) /* once per orbit */
#define WDG_PERIOD_AOCS_MS             20u

/* Monitor scan period (ms). */
#define WDG_MONITOR_PERIOD_MS         500u

/* Start-up grace window (ms).
 *
 * Tasks are registered from main() *before* osKernelStart(), where
 * xTaskGetTickCount() still reads 0, and several of them do bring-up work
 * (peripheral settling, init delays) before their first watchdog_alive()
 * call. Seeding last_tick at registration therefore made the very first
 * monitor scan report every task as hung (elapsed 500 ms > 3 x 100 ms for the
 * state machine) on every single boot.
 *
 * Instead an entry stays *unarmed* until the task reports liveness for the
 * first time; while unarmed it is judged against max(3 x period, this grace
 * window), measured from the first real post-kernel-start tick. Long enough
 * for the slowest bring-up path in the tree (state_machine_task: 100 ms +
 * 500 ms of init work), short enough that a task which never starts is still
 * caught within a few seconds. */
#define WDG_STARTUP_GRACE_MS         5000u

/* Initialise the watchdog monitor */
void watchdog_monitor_init(void);

/* Register a task for monitoring (call during task init) */
int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms);

/* Create the watchdog monitor task */
osThreadId_t watchdog_task_create(void);

/* Called by monitored tasks to signal liveness */
void watchdog_alive(const osThreadId_t handle);

/* Convenience wrapper: signal liveness for the calling task */
void watchdog_alive_self(void);

/* Record/defer gate for a stalled task (see watchdog.c): returns 1 when the
   escalation record may be written for `suspect` now, 0 when it must be
   deferred because `suspect` currently holds the LastStates pool mutex
   (`pool_holder` as read by laststates_pool_holder(), NULL when the mutex is
   free) - writing the record would wedge the monitor on that mutex. The name
   predates the reset policy (the monitor used to suspend the task). */
int watchdog_suspend_allowed(osThreadId_t suspect, osThreadId_t pool_holder);

/* ---------- Escalation of a stalled task ----------
   A task silent for more than 3x its declared period is escalated: the
   monitor writes a TRIGGER_WATCHDOG LastStates record and resets the OBC
   (after dual_bank_mark_boot_fault(), so a task that hangs on every boot
   arms the golden-image fallback). When the stalled task holds the
   LastStates pool mutex the record cannot be written (it would wedge the
   monitor): the escalation is deferred and retried on the next scan, at most
   WDG_MAX_HOLDER_DEFERRALS times in a row, after which the OBC resets
   without the record. 60 scans x 500 ms = 30 s: two orders of magnitude
   above the documented nominal pool hold (~80 ms, see state_machine.c
   try_transition()), yet a bounded wait for a task that hung inside the
   pool lock. */
#define WDG_MAX_HOLDER_DEFERRALS   60u

#define WDG_ACTION_NONE               0  /* nothing to escalate (NULL suspect) */
#define WDG_ACTION_DEFER              1  /* suspect holds the pool mutex: retry */
#define WDG_ACTION_RECORD_AND_RESET   2  /* record TRIGGER_WATCHDOG, reset OBC  */
#define WDG_ACTION_RESET_UNRECORDED   3  /* deferral budget spent: reset only   */

/* Pure decision behind the escalation (host-tested): what to do with
   `suspect` given the current pool-mutex holder and how many times in a row
   its escalation has already been deferred. */
int watchdog_escalation_action(osThreadId_t suspect, osThreadId_t pool_holder,
                               uint32_t consecutive_deferrals);

#ifdef HOST_UNIT_TEST
/* Host-only observation of the last escalation (the record and the reset are
   compiled out on the host): the WDG_ACTION_* taken, and how many resets the
   monitor would have performed. */
int      watchdog_last_escalation_action(void);
uint32_t watchdog_escalation_resets(void);
#endif

/* Escalations deferred by the policy above (suspect held the pool mutex).
   The deferral leaves no Flash record by construction - writing one would
   wedge the monitor on the held mutex - so this saturating-free 32-bit
   counter (zero after reset / watchdog_monitor_init()) is what makes
   deferrals visible to ground instead of silent. */
uint32_t watchdog_holder_deferrals(void);

/* Stack high-water mark of a monitored task, in words
   (uxTaskGetStackHighWaterMark() units, not bytes), as last sampled by the
   monitor scan. Returns 0 and fills `*hwm_words`, or -1 for a NULL argument
   or a handle that is not registered. 0 in `*hwm_words` means "no scan has
   sampled this entry yet". Housekeeping/beacon telemetry reads stack health
   through here instead of touching the kernel directly. */
int watchdog_task_stack_hwm(osThreadId_t handle, UBaseType_t *hwm_words);

#endif /* WATCHDOG_H */
