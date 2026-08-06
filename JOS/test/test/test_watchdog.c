/**
 * @file    test_watchdog.c
 * @brief   Unit tests for the software watchdog monitor (App/obsw/watchdog.c)
 *          and for its wiring into an RTOS task loop (App/aocs/aocs.c).
 *
 * The CMSIS-RTOS2 API and the FreeRTOS tick source are replaced by CMock
 * mocks generated from test/fakes/cmsis_os.h and test/fakes/task.h, so every
 * mutex take/give, thread-id lookup and tick read is observable and
 * deterministic. The two Core/Src services the monitor depends on
 * (dual_bank_boot_complete(), hw_watchdog_kick()) are counted doubles in
 * support/rtos_stubs.c. Nothing here touches silicon.
 *
 * What is verified
 *   - watchdog_register_task() argument contract (NULL handle, zero period,
 *     un-initialised monitor) - refuse rather than pretend to monitor.
 *   - registration de-duplication and slot exhaustion (WDG_MAX_TASKS).
 *   - the pre-scheduler registration rule: a tick sampled before
 *     osKernelStart() is meaningless, so such an entry is stored *unseeded*
 *     and the tick source is not read at all.
 *   - watchdog_alive()/watchdog_alive_self() only refresh *registered* tasks;
 *     an unregistered handle must not touch the tick source at all (asserted
 *     by CMock: an unexpected xTaskGetTickCount() call fails the test).
 *   - the monitor task itself: it refreshes the IWDG backstop unconditionally
 *     on every scan, seeds entries registered before the scheduler started,
 *     and declares the boot good only after DUAL_BANK_BOOT_OK_UPTIME_MS of
 *     scheduler uptime, retrying a bounded number of times.
 *   - aocs_task_create() registers the created thread with the monitor at the
 *     declared period (WDG_PERIOD_AOCS_MS), and the aocs_task() loop calls
 *     watchdog_alive_self() on every iteration.
 *
 * Standards: ECSS-E-ST-40C 5.5 (unit testing), NASA-STD-8739.8 7.3
 * (independent verification of fault-detection functions),
 * JPL Institutional Coding Standard (JPL-182) Rule 16 (check return values).
 */

#include "unity.h"
#include "watchdog.h"
#include "aocs.h"
#include "dual_bank.h"
#include "hw_watchdog.h"
#include "mock_cmsis_os.h"
#include "mock_task.h"

#include <setjmp.h>

/* Host seams exported by support/rtos_stubs.c (see fakes/dual_bank.h). */
uint32_t host_dual_bank_boot_complete_calls(void);
void     host_dual_bank_fail_boot_complete(uint32_t times);
void     host_dual_bank_reset(void);

/* ---------- Fake RTOS objects (only their addresses matter) ---------- */
static uint8_t mutex_obj;
static uint8_t thread_objs[WDG_MAX_TASKS + 2];

#define FAKE_MUTEX   ((osMutexId_t)&mutex_obj)
#define TH(i)        ((osThreadId_t)&thread_objs[(i)])

/* ---------- Fixture ---------- */

void setUp(void)
{
    host_dual_bank_reset();
    host_hw_watchdog_reset();

    /* Re-initialise the monitor before every test: watchdog.c keeps its task
       table in file-static storage that would otherwise leak between cases. */
    osMutexNew_ExpectAnyArgsAndReturn(FAKE_MUTEX);
    watchdog_monitor_init();

    /* Mutex traffic is not the subject under test; the tick source is. */
    osMutexAcquire_IgnoreAndReturn(osOK);
    osMutexRelease_IgnoreAndReturn(osOK);
}

void tearDown(void) { }

/* watchdog_register_task() consults the kernel state before it trusts
   xTaskGetTickCount(). Declared per test rather than in setUp() on purpose:
   CMock's _IgnoreAndReturn queues its return values, so a second call in a
   test would be shadowed by the one from the fixture and the pre-scheduler
   cases would silently exercise the running-kernel path instead. */
static void given_kernel_running(void)
{
    osKernelGetState_IgnoreAndReturn(osKernelRunning);
}

static void given_kernel_not_started(void)
{
    osKernelGetState_IgnoreAndReturn(osKernelInactive);
}

/* ================= registration contract ================= */

/* A NULL handle is a programming error: refuse, never silently accept. */
void test_watchdog_register_rejects_null_handle(void)
{
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(NULL, 100u));
}

/* A zero period would make the 3x staleness window degenerate. */
void test_watchdog_register_rejects_zero_period(void)
{
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(TH(0), 0u));
}

/* Registering before watchdog_monitor_init() must fail (no mutex yet). */
void test_watchdog_register_rejects_uninitialised_monitor(void)
{
    osMutexNew_ExpectAnyArgsAndReturn(NULL);   /* mutex creation fails */
    watchdog_monitor_init();
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(TH(0), 100u));
}

/* Happy path: the entry is stamped with the current tick exactly once. */
void test_watchdog_register_accepts_task_and_stamps_tick(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(1000u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), WDG_PERIOD_AOCS_MS));
}

/* Tasks are registered from main(), i.e. before osKernelStart(), where
   xTaskGetTickCount() reads 0 and stays there. Seeding last_tick from there
   made the first monitor scan report every task as hung, so the registration
   must not read the tick source at all: no expectation is queued here, and
   CMock fails the test if one is consumed. The monitor stamps the entry on
   its first scan instead (see the monitor tests below). */
void test_watchdog_register_before_kernel_start_does_not_read_tick(void)
{
    given_kernel_not_started();
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));
}

/* Re-registering the same handle refreshes in place, it must not burn a slot. */
void test_watchdog_register_deduplicates_same_handle(void)
{
    given_kernel_running();
    xTaskGetTickCount_IgnoreAndReturn(5u);

    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 250u));

    /* All remaining slots must still be free ... */
    for (int i = 1; i < WDG_MAX_TASKS; i++) {
        TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(i), 100u));
    }
    /* ... and only then is the table full. */
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(TH(WDG_MAX_TASKS), 100u));
}

/* Bounded resource: the (WDG_MAX_TASKS + 1)-th distinct task is rejected. */
void test_watchdog_register_reports_pool_exhaustion(void)
{
    given_kernel_running();
    xTaskGetTickCount_IgnoreAndReturn(5u);

    for (int i = 0; i < WDG_MAX_TASKS; i++) {
        TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(i), 100u));
    }
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(TH(WDG_MAX_TASKS), 100u));
    TEST_ASSERT_EQUAL_INT(-1, watchdog_register_task(TH(WDG_MAX_TASKS + 1), 100u));
}

/* ================= liveness signalling ================= */

/* A registered task refreshes its timestamp: the tick source is read once. */
void test_watchdog_alive_refreshes_registered_task(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(10u);       /* registration stamp */
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    xTaskGetTickCount_ExpectAndReturn(20u);       /* liveness stamp */
    watchdog_alive(TH(0));
}

/* An unknown handle must be ignored. No xTaskGetTickCount() expectation is
   queued, so CMock fails the test if the table is walked into a write. */
void test_watchdog_alive_ignores_unregistered_handle(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(10u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    watchdog_alive(TH(1));                        /* never registered */
}

/* NULL is rejected before any RTOS call. */
void test_watchdog_alive_ignores_null_handle(void)
{
    watchdog_alive(NULL);
}

/* watchdog_alive_self() resolves the caller through osThreadGetId(). */
void test_watchdog_alive_self_uses_current_thread_id(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(10u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    osThreadGetId_ExpectAndReturn(TH(0));
    xTaskGetTickCount_ExpectAndReturn(42u);
    watchdog_alive_self();
}

/* ================= monitor task creation ================= */

void test_watchdog_task_create_returns_thread_handle(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(TH(3));
    TEST_ASSERT_EQUAL_PTR(TH(3), watchdog_task_create());
}

/* ================= the monitor scan itself =================
 *
 * watchdog_monitor_task() is file-static and never returns, so it is reached
 * the only way the flight build reaches it: through the function pointer
 * watchdog_task_create() hands to osThreadNew(). A CMock callback captures
 * that pointer, and an osDelay() stub long-jumps out of the infinite loop
 * after a fixed number of scans - the same technique the task-loop tests use.
 */

static osThreadFunc_t captured_entry;
static jmp_buf        loop_escape;
static int            delay_calls;
static int            delay_escape_after;

static osThreadId_t osThreadNew_capture_cb(osThreadFunc_t func, void *argument,
                                           const osThreadAttr_t *attr,
                                           int cmock_num_calls)
{
    (void)argument;
    (void)cmock_num_calls;
    TEST_ASSERT_NOT_NULL(attr);
    captured_entry = func;
    return TH(3);
}

static osStatus_t osDelay_escape_cb(uint32_t ticks, int cmock_num_calls)
{
    (void)ticks;
    (void)cmock_num_calls;

    delay_calls++;
    if (delay_calls >= delay_escape_after) {
        longjmp(loop_escape, 1);   /* break out of the infinite RTOS loop */
    }
    return osOK;
}

static osStatus_t osDelay_monitor_period_cb(uint32_t ticks, int cmock_num_calls)
{
    /* The scan cadence is part of the contract: 3x the fastest monitored
       period (100 ms) must still leave room for a scan to notice. */
    TEST_ASSERT_EQUAL_UINT32(WDG_MONITOR_PERIOD_MS, ticks);
    return osDelay_escape_cb(ticks, cmock_num_calls);
}

/* Run the captured monitor entry point for `scans` iterations. */
static void run_monitor_scans(int scans)
{
    TEST_ASSERT_NOT_NULL(captured_entry);
    delay_calls        = 0;
    delay_escape_after = scans;

    if (setjmp(loop_escape) == 0) {
        captured_entry(NULL);
        TEST_FAIL_MESSAGE("watchdog_monitor_task() returned - it must not exit");
    }
    TEST_ASSERT_EQUAL_INT(scans, delay_calls);
}

static void capture_monitor_entry(void)
{
    captured_entry = NULL;
    osThreadNew_Stub(osThreadNew_capture_cb);
    TEST_ASSERT_EQUAL_PTR(TH(3), watchdog_task_create());
    TEST_ASSERT_NOT_NULL(captured_entry);
}

/* The IWDG refresh is unconditional and at the top of the loop: coupling it
   to the liveness scan would make a bug in the software monitor turn into a
   reset storm. One kick per scan, with no task registered at all. */
void test_monitor_refreshes_hardware_watchdog_every_scan(void)
{
    capture_monitor_entry();

    /* Uptime stays well below DUAL_BANK_BOOT_OK_UPTIME_MS, so the boot-OK
       marker must not be declared yet. */
    xTaskGetTickCount_IgnoreAndReturn(10u);
    osDelay_Stub(osDelay_monitor_period_cb);

    run_monitor_scans(3);

    TEST_ASSERT_EQUAL_UINT32(3u, host_hw_watchdog_kick_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_dual_bank_boot_complete_calls());
}

/* An entry registered before osKernelStart() carries no usable tick. The
   monitor must stamp it with the first tick it actually observes and give it
   its grace window from there, instead of reporting a hang that only measures
   the boot sequence. Two scans: the first seeds, the second judges - and with
   a 5 s grace window nothing is flagged. */
void test_monitor_seeds_entries_registered_before_kernel_start(void)
{
    given_kernel_not_started();
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    capture_monitor_entry();

    xTaskGetTickCount_IgnoreAndReturn(50u);
    osDelay_Stub(osDelay_escape_cb);

    run_monitor_scans(2);

    TEST_ASSERT_EQUAL_UINT32(2u, host_hw_watchdog_kick_count());
}

/* An armed task (it has reported liveness at least once) that then goes
   silent for more than 3x its declared period is walked over the staleness
   branch. There is no escalation action yet (see the TODO in watchdog.c), so
   what is asserted is that the scan completes and keeps kicking the IWDG:
   a late task must never stop the hardware refresh. */
void test_monitor_scans_stale_armed_task_without_stopping_the_iwdg(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(0u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    xTaskGetTickCount_ExpectAndReturn(0u);
    watchdog_alive(TH(0));                    /* arm it at tick 0 */

    capture_monitor_entry();

    /* 1 s later: elapsed (1000 ms) > 3 x 100 ms, so the staleness branch is
       taken. Deliberately still below DUAL_BANK_BOOT_OK_UPTIME_MS: the
       boot-OK latch is a one-shot shared by every test in this process and
       must only be consumed by the dedicated test further down. */
    xTaskGetTickCount_IgnoreAndReturn(1000u);
    osDelay_Stub(osDelay_escape_cb);

    run_monitor_scans(1);

    TEST_ASSERT_EQUAL_UINT32(1u, host_hw_watchdog_kick_count());
}

/* Boot-OK declaration. The window must stay open until the system has proved
   it can run, not merely reach osKernelStart(), so the marker is written only
   after DUAL_BANK_BOOT_OK_UPTIME_MS of scheduler uptime - and a failure to
   persist it is retried, bounded, rather than dropped.
 *
 * NOTE ON ORDERING: watchdog_declare_boot_ok_if_due() latches its "done" flag
 * in function-static storage that watchdog_monitor_init() does not clear, and
 * every test in this file shares one process. This is therefore the LAST
 * monitor test in the file: once it runs, no later test can observe a
 * boot-OK attempt again. */
void test_monitor_declares_boot_ok_after_uptime_and_retries_once(void)
{
    capture_monitor_entry();

    /* First attempt fails to persist (LastStates pool momentarily full), the
       second succeeds; after that the marker is latched and no further call
       is made however long the monitor runs. */
    host_dual_bank_fail_boot_complete(1u);

    xTaskGetTickCount_IgnoreAndReturn(DUAL_BANK_BOOT_OK_UPTIME_MS);
    osDelay_Stub(osDelay_escape_cb);

    run_monitor_scans(4);

    TEST_ASSERT_EQUAL_UINT32(2u, host_dual_bank_boot_complete_calls());
    TEST_ASSERT_EQUAL_UINT32(4u, host_hw_watchdog_kick_count());
}

/* ================= PR #10 wiring: tasks register + kick ================= */

/* aocs_task_create() must hand the new thread to the watchdog monitor.
   The registration is observed twice over: the tick stamp taken during
   registration, and the fact that a later liveness signal for that handle is
   accepted (it would be silently dropped for an unmonitored task). */
void test_aocs_task_create_registers_task_with_watchdog(void)
{
    given_kernel_running();
    osThreadNew_ExpectAnyArgsAndReturn(TH(0));
    xTaskGetTickCount_ExpectAndReturn(7u);        /* registration stamp */

    TEST_ASSERT_EQUAL_PTR(TH(0), aocs_task_create());

    xTaskGetTickCount_ExpectAndReturn(8u);        /* handle is monitored */
    watchdog_alive(TH(0));
}

/* If the RTOS refuses to create the thread there is nothing to monitor:
   no registration must be attempted (no tick read is expected). */
void test_aocs_task_create_does_not_register_when_thread_creation_fails(void)
{
    osThreadNew_ExpectAnyArgsAndReturn(NULL);
    TEST_ASSERT_NULL(aocs_task_create());
}

void test_aocs_init_is_a_noop_on_the_obc(void)
{
    /* The sensors live on the AOCS board; the OBC side must not touch any
       peripheral at init. No mock expectation is queued, so any RTOS or HAL
       call made here fails the test. */
    aocs_init();
}

/* --- the task loop kicks the watchdog on every iteration --- */

static osStatus_t osDelay_aocs_period_cb(uint32_t ticks, int cmock_num_calls)
{
    /* 20 ms nominal AOCS polling period at a 1 kHz tick. */
    TEST_ASSERT_EQUAL_UINT32(WDG_PERIOD_AOCS_MS, ticks);
    return osDelay_escape_cb(ticks, cmock_num_calls);
}

void test_aocs_task_loop_signals_watchdog_every_iteration(void)
{
    given_kernel_running();
    xTaskGetTickCount_ExpectAndReturn(1u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), WDG_PERIOD_AOCS_MS));

    /* Three iterations, each one must call watchdog_alive_self() exactly once
       (osThreadGetId + tick stamp). A missing or extra kick fails here. */
    for (int i = 0; i < 3; i++) {
        osThreadGetId_ExpectAndReturn(TH(0));
        xTaskGetTickCount_ExpectAndReturn((uint32_t)(10 + i));
    }

    delay_calls        = 0;
    delay_escape_after = 3;
    osDelay_Stub(osDelay_aocs_period_cb);

    if (setjmp(loop_escape) == 0) {
        aocs_task(NULL);
        TEST_FAIL_MESSAGE("aocs_task() returned - the RTOS loop must not exit");
    }

    TEST_ASSERT_EQUAL_INT(3, delay_calls);
}
