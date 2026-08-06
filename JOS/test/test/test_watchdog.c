/**
 * @file    test_watchdog.c
 * @brief   Unit tests for the software watchdog monitor (App/obsw/watchdog.c)
 *          and for its wiring into an RTOS task loop (App/aocs/aocs.c).
 *
 * The CMSIS-RTOS2 API and the FreeRTOS tick source are replaced by CMock
 * mocks generated from test/fakes/cmsis_os.h and test/fakes/task.h, so every
 * mutex take/give, thread-id lookup and tick read is observable and
 * deterministic. Nothing here touches silicon.
 *
 * What is verified
 *   - watchdog_register_task() argument contract (NULL handle, zero period,
 *     un-initialised monitor) — refuse rather than pretend to monitor.
 *   - registration de-duplication and slot exhaustion (WDG_MAX_TASKS).
 *   - watchdog_alive()/watchdog_alive_self() only refresh *registered* tasks;
 *     an unregistered handle must not touch the tick source at all (asserted
 *     by CMock: an unexpected xTaskGetTickCount() call fails the test).
 *   - aocs_task_create() registers the created thread with the monitor at the
 *     declared period (WDG_PERIOD_AOCS_MS) — PR #10 wiring.
 *   - the aocs_task() loop calls watchdog_alive_self() on every iteration.
 *
 * The hardware IWDG (HAL_IWDG_Refresh) is not yet enabled in the flight build
 * — see the TODO in App/obsw/state_machine.c. Its mock seam exists in
 * test/fakes/stm32l4xx_hal.h so the refresh call can be asserted as soon as
 * CubeMX generates the hiwdg handle.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit testing), NASA-STD-8739.8 §7.3
 * (independent verification of fault-detection functions),
 * JPL Institutional Coding Standard (JPL-182) Rule 16 (check return values).
 */

#include "unity.h"
#include "watchdog.h"
#include "aocs.h"
#include "mock_cmsis_os.h"
#include "mock_task.h"

#include <setjmp.h>

/* ---------- Fake RTOS objects (only their addresses matter) ---------- */
static uint8_t mutex_obj;
static uint8_t thread_objs[WDG_MAX_TASKS + 2];

#define FAKE_MUTEX   ((osMutexId_t)&mutex_obj)
#define TH(i)        ((osThreadId_t)&thread_objs[(i)])

/* ---------- Fixture ---------- */

void setUp(void)
{
    /* Re-initialise the monitor before every test: watchdog.c keeps its task
       table in file-static storage that would otherwise leak between cases. */
    osMutexNew_ExpectAnyArgsAndReturn(FAKE_MUTEX);
    watchdog_monitor_init();

    /* Mutex traffic is not the subject under test; the tick source is. */
    osMutexAcquire_IgnoreAndReturn(osOK);
    osMutexRelease_IgnoreAndReturn(osOK);
}

void tearDown(void) { }

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
    xTaskGetTickCount_ExpectAndReturn(1000u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), WDG_PERIOD_AOCS_MS));
}

/* Re-registering the same handle refreshes in place, it must not burn a slot. */
void test_watchdog_register_deduplicates_same_handle(void)
{
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
    xTaskGetTickCount_ExpectAndReturn(10u);       /* registration stamp */
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), 100u));

    xTaskGetTickCount_ExpectAndReturn(20u);       /* liveness stamp */
    watchdog_alive(TH(0));
}

/* An unknown handle must be ignored. No xTaskGetTickCount() expectation is
   queued, so CMock fails the test if the table is walked into a write. */
void test_watchdog_alive_ignores_unregistered_handle(void)
{
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

/* ================= PR #10 wiring: tasks register + kick ================= */

/* aocs_task_create() must hand the new thread to the watchdog monitor.
   The registration is observed twice over: the tick stamp taken during
   registration, and the fact that a later liveness signal for that handle is
   accepted (it would be silently dropped for an unmonitored task). */
void test_aocs_task_create_registers_task_with_watchdog(void)
{
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

/* --- the task loop kicks the watchdog on every iteration --- */

static jmp_buf loop_escape;
static int     delay_calls;

static osStatus_t osDelay_escape_cb(uint32_t ticks, int cmock_num_calls)
{
    (void)cmock_num_calls;
    /* 20 ms nominal AOCS polling period at a 1 kHz tick. */
    TEST_ASSERT_EQUAL_UINT32(WDG_PERIOD_AOCS_MS, ticks);

    delay_calls++;
    if (delay_calls >= 3) {
        longjmp(loop_escape, 1);   /* break out of the infinite RTOS loop */
    }
    return osOK;
}

void test_aocs_task_loop_signals_watchdog_every_iteration(void)
{
    xTaskGetTickCount_ExpectAndReturn(1u);
    TEST_ASSERT_EQUAL_INT(0, watchdog_register_task(TH(0), WDG_PERIOD_AOCS_MS));

    /* Three iterations, each one must call watchdog_alive_self() exactly once
       (osThreadGetId + tick stamp). A missing or extra kick fails here. */
    for (int i = 0; i < 3; i++) {
        osThreadGetId_ExpectAndReturn(TH(0));
        xTaskGetTickCount_ExpectAndReturn((uint32_t)(10 + i));
    }

    delay_calls = 0;
    osDelay_Stub(osDelay_escape_cb);

    if (setjmp(loop_escape) == 0) {
        aocs_task(NULL);
        TEST_FAIL_MESSAGE("aocs_task() returned — the RTOS loop must not exit");
    }

    TEST_ASSERT_EQUAL_INT(3, delay_calls);
}
