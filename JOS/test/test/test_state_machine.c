/* ---------------------------------------------------------------------------
 * test_state_machine.c - host unit tests for App/obsw/state_machine.c.
 *
 * The real state_machine.c is linked unmodified. Target-only neighbours are
 * replaced by local doubles: a no-op RTOS (no scheduler on the host),
 * watchdog registration capture (watchdog.c is target-side, not linked) and
 * controllable SRAM2-parity seams (Core/Src/sram2_parity.c needs silicon).
 * LastStates persistence runs against the real memory.c + host Flash model
 * and the boot-CRC confinement gate against the real boot_crc.c with a
 * stamped host image, so the transitions under test traverse the flight code
 * paths. All stimulus goes through the public API (init / request /
 * get_state / beacon accessors / critical-region hook) plus the
 * bms_set_soc_stub() test seam for the SoC-gated CRIT recoveries.
 *
 * Pattern follows PR #74 (test via public API, no statics poked). The task
 * body itself is exercised the same way: state_machine_task_create() is
 * public, and the RTOS double captures the entry point so a test can invoke
 * it on the host. osDelay() escapes via longjmp after N delays (boot needs
 * exactly two, the third ends the first main-loop iteration), behind a
 * SIGALRM hang ceiling shared with the test_comms.c task-loop pattern. The
 * ceiling longjmps back and fails only the hanging test (never _exit: one
 * hung test must not kill the rest of the suite).
 *
 * Refs: ECSS-E-ST-40C 5.5, NASA-STD-8739.8 (unit test evidence).
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "state_machine.h"      /* unit under test -> links state_machine.c */
#include "boot_crc.h"           /* boot_crc_verify(): establish a trusted image */
#include "memory.h"             /* real LastStates pool behind try_transition() */
#include "obsw_types.h"
#include "host_support.h"
#include "seu_mitigation.h"

#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>             /* alarm(), STDERR_FILENO */

/* bms_set_soc_stub() lives in state_machine.c but is not part of the public
 * header (flight code never calls it); declared here for the CRIT-recovery
 * tests below. */
extern void bms_set_soc_stub(uint8_t soc);

/* ---------- Minimal RTOS double (no scheduler on the host) ---------- */
/* Opaque RTOS handles are pointers: back them with pointer-sized, aligned
 * storage so the address-to-handle conversion is well-defined on 64-bit
 * hosts (a uint8_t backing object would invite truncation/alignment bugs
 * the moment anyone dereferences a handle or does arithmetic on one). */
static uintptr_t fake_mutex_obj;
static uintptr_t fake_thread_obj;
static uint32_t fake_tick;

/* Task-body escape hatch (see the task tests at the end of this file).
 * Escape codes: the osDelay() hatch exits the task body normally, the
 * SIGALRM hang handler aborts only the hanging test. */
#define TASK_ESCAPE_DELAY 1
#define TASK_ESCAPE_HANG  2

static volatile sig_atomic_t task_hang_fired;
static jmp_buf        task_escape;
static osThreadFunc_t captured_task;
static void          *captured_arg;
static int            delay_calls;
static int            delay_escape_at = -1;

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    (void)attr;
    return (osMutexId_t)&fake_mutex_obj;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    (void)mutex_id;
    (void)timeout;
    return osOK;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    (void)mutex_id;
    return osOK;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
                         const osThreadAttr_t *attr)
{
    (void)attr;
    /* Capture the entry point: state_machine_task_create() is public, and
     * invoking the captured function runs the real boot sequence + main
     * loop on the host (see the task tests at the end of this file). */
    captured_task = func;
    captured_arg  = argument;
    return (osThreadId_t)&fake_thread_obj;
}

osStatus_t osDelay(uint32_t ticks)
{
    (void)ticks;
    delay_calls++;
    if ((delay_escape_at > 0) && (delay_calls >= delay_escape_at)) {
        longjmp(task_escape, TASK_ESCAPE_DELAY);
    }
    return osOK;
}

uint32_t osKernelGetTickCount(void)
{
    return fake_tick++;
}

/* ---------- Watchdog doubles (watchdog.c is target-side, not linked) ---------- */
static osThreadId_t wdg_handle;
static uint32_t     wdg_period_ms;
static int          wdg_reg_calls;

int watchdog_register_task(osThreadId_t handle, uint32_t expected_period_ms)
{
    wdg_handle      = handle;
    wdg_period_ms   = expected_period_ms;
    wdg_reg_calls++;
    return 0;
}

void watchdog_alive_self(void)
{
}

/* ---------- SRAM2 doubles (Core/Src/sram2_parity.c needs silicon) ---------- */
static int sim_boot_fault;
static int boot_fault_acks;

int sram2_parity_boot_fault(void)
{
    return sim_boot_fault;
}

void sram2_parity_boot_fault_ack(void)
{
    boot_fault_acks++;
}

int sram2_restore_from_image(void *obj, size_t len)
{
    (void)obj;
    (void)len;
    return 0;
}

/* ---------- Fixture ---------- */
void setUp(void)
{
    host_flash_reset();
    seu_stub_reset();
    fake_tick      = 0u;
    sim_boot_fault = 0;
    boot_fault_acks = 0;
    wdg_reg_calls  = 0;
    wdg_handle     = NULL;
    wdg_period_ms  = 0u;
    captured_task  = NULL;
    captured_arg   = NULL;
    delay_calls    = 0;
    delay_escape_at = -1;

    laststates_pool_lock_set_result_for_test(LASTSTATES_LOCK_NOT_NEEDED);
    laststates_init();

    /* A stamped image, so the boot-CRC confinement gate lets the OBSW reach
     * READY/ACTIVE (otherwise every nominal transition is refused). */
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());

    state_machine_init();
    TEST_ASSERT_EQUAL_INT(STATE_OFF, (int)state_machine_get_state());
}

void tearDown(void)
{
    alarm(0);   /* the hang ceiling must never survive its own test case */
}

/* ---------- Task body (captured entry point) ----------
 *
 * These run LAST in this file: the parity tests latch the in-run
 * confinement flag, which no public API clears (by design — a
 * memory-integrity finding is not something a test teardown may walk out
 * of, any more than a battery recovery may). */

/* Hang ceiling for the task tests: if the osDelay() escape never fires the
 * task loops forever, so SIGALRM aborts just that test via longjmp instead
 * of spinning to CI's job timeout (same pattern as test_comms.c, but scoped
 * to the failing test — never _exit()). */
#define TASK_HANG_SECS 30

static void task_hang_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nERROR: state-machine task test exceeded 30 s - "
                       "osDelay() escape never fired?\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    /* Fail just this test: longjmp back to run_task_until_delay(), which
     * reports via TEST_FAIL_MESSAGE. Never _exit() here — one hung test
     * must not kill the rest of the suite. */
    task_hang_fired = 1;
    longjmp(task_escape, TASK_ESCAPE_HANG);
}

/* Create the task (public API, captures the entry point) and run its body
 * until the delay_escape_at-th osDelay(). Boot needs exactly two delays
 * (100 ms settle + 500 ms init work); the third ends the first main-loop
 * iteration, after check_battery_autonomous() ran. */
static void run_task_until_delay(int escape_at)
{
    osThreadId_t h = state_machine_task_create();
    int esc;

    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_NOT_NULL(captured_task);

    delay_calls     = 0;
    delay_escape_at = escape_at;
    task_hang_fired = 0;
    (void)signal(SIGALRM, task_hang_handler);
    alarm(TASK_HANG_SECS);

    esc = setjmp(task_escape);
    if (esc == 0) {
        captured_task(captured_arg);
        TEST_FAIL_MESSAGE("state-machine task returned - it must not exit");
    }

    alarm(0);
    delay_escape_at = -1;
    if ((esc == TASK_ESCAPE_HANG) || (task_hang_fired != 0)) {
        TEST_FAIL_MESSAGE("state-machine task hung - osDelay() escape never "
                          "fired (hang ceiling)");
    }
    TEST_ASSERT_EQUAL_INT(escape_at, delay_calls);
}

/* Walk the nominal boot path s0->s1->s3. */
static void boot_to_ready(void)
{
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_INIT,
                                                              TRIGGER_BOOT));
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                      TRIGGER_ANTENNA_DONE));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* Walk to s4 (payload ops) via a ground command. */
static void boot_to_active(void)
{
    boot_to_ready();
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* ACTIVE can still always reach CRIT on battery low. */
void test_active_to_crit_on_battery_low(void)
{
    boot_to_active();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_CRIT,
                                                       TRIGGER_BATTERY_LOW));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}

/* The s3->s4 entry path: READY reaches ACTIVE on a ground command. */
void test_ready_to_active_accepted(void)
{
    boot_to_ready();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* ACTIVE has no nominal exit except the safe state: READY is refused and the
 * OBSW stays in ACTIVE. */
void test_active_to_ready_refused(void)
{
    boot_to_active();

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                          TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* Task creation still registers the state machine with the watchdog at its
 * declared 100 ms period (WDG_PERIOD_STATE_MACHINE_MS). */
void test_task_create_registers_with_watchdog(void)
{
    osThreadId_t h = state_machine_task_create();

    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(1, wdg_reg_calls);
    TEST_ASSERT_EQUAL_PTR(h, wdg_handle);
    TEST_ASSERT_EQUAL_UINT32(100u, wdg_period_ms);
}

/* ---------- INIT / OFF target rules ---------- */

/* INIT is boot bookkeeping only: reachable from OFF, refused anywhere else. */
void test_init_from_non_off_is_refused(void)
{
    boot_to_ready();

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_INIT,
                                                               TRIGGER_BOOT));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* OFF can never be a transition target in flight. */
void test_off_target_is_refused(void)
{
    boot_to_ready();

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_OFF,
                                                      TRIGGER_BATTERY_LOW));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* ---------- CRIT recovery paths ---------- */

/* Walk to CRIT (safe mode) from an active payload operation. */
static void boot_to_crit(void)
{
    boot_to_active();
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_CRIT,
                                                      TRIGGER_BATTERY_LOW));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}

/* s2->s3: battery recovered (default stub SoC 100 >= b_opok 80). */
void test_crit_recovery_to_ready_when_soc_ok(void)
{
    boot_to_crit();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* s2->s3 stays refused while the battery is still low, and opens once the
 * SoC recovers. Also exercises bms_set_soc_stub(). */
void test_crit_recovery_to_ready_refused_when_soc_low(void)
{
    boot_to_crit();

    bms_set_soc_stub(50u);
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());

    bms_set_soc_stub(100u);
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* s2->s4: a ground command resumes ops once the battery is stable. */
void test_crit_to_active_on_ground_cmd_when_soc_ok(void)
{
    boot_to_crit();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* s2->s4 needs BOTH a stable battery AND the ground-command trigger. */
void test_crit_to_active_refused_when_soc_low_or_wrong_trigger(void)
{
    boot_to_crit();

    bms_set_soc_stub(10u);
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());

    bms_set_soc_stub(100u);
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_ACTIVE,
                                                          TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}

/* ---------- Confinement gates ---------- */

/* While the running image is untrusted, nominal targets are refused; only
 * the INIT boot bookkeeping and the CRIT safe state stay reachable. */
void test_untrusted_image_confines_to_crit_and_init(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC ^ 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());

    /* Nominal ops refused from OFF. */
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                      TRIGGER_ANTENNA_DONE));
    TEST_ASSERT_EQUAL_INT(STATE_OFF, (int)state_machine_get_state());

    /* Boot bookkeeping still allowed ... */
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_INIT,
                                                              TRIGGER_BOOT));
    /* ... but boot cannot proceed to nominal ops either. */
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                      TRIGGER_ANTENNA_DONE));
    TEST_ASSERT_EQUAL_INT(STATE_INIT, (int)state_machine_get_state());

    /* Safe state always reachable. */
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_CRIT,
                                                 TRIGGER_IMAGE_CRC_FAIL));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());

    /* Re-trust the image: the confined OBSW recovers nominally. */
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* A Flash failure under the LastStates write refuses the transition WITHOUT
 * moving state: the record is the evidence, not a side effect. */
void test_transition_fails_when_laststates_persistence_fails(void)
{
    boot_to_ready();

    host_flash_fail_program_after(0u);
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
}

/* ---------- Beacon cadence ---------- */

/* Per-state default beacon intervals, including the fail-safe default arm. */
void test_beacon_interval_defaults_per_state(void)
{
    /* OFF hits the default arm (fail-safe: beacon like CRIT). */
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_CRIT,
                             state_machine_get_beacon_interval());

    boot_to_ready();
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_READY,
                             state_machine_get_beacon_interval());

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_ACTIVE,
                             state_machine_get_beacon_interval());

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_CRIT,
                                                      TRIGGER_BATTERY_LOW));
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_CRIT,
                             state_machine_get_beacon_interval());
}

/* Ground-commanded override: accepted in-band, 0 clears it, out-of-band is
 * rejected with the cadence left intact (fail-safe). */
void test_beacon_interval_override_accept_reject(void)
{
    boot_to_ready();

    TEST_ASSERT_EQUAL_INT(0, state_machine_set_beacon_interval(60000u));
    TEST_ASSERT_EQUAL_UINT32(60000u, state_machine_get_beacon_interval());

    TEST_ASSERT_EQUAL_INT(0, state_machine_set_beacon_interval(0u));
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_READY,
                             state_machine_get_beacon_interval());

    TEST_ASSERT_EQUAL_INT(-1, state_machine_set_beacon_interval(
                                  (uint32_t)(BEACON_INTERVAL_MIN - 1u)));
    TEST_ASSERT_EQUAL_INT(-1, state_machine_set_beacon_interval(
                                  (uint32_t)(BEACON_INTERVAL_MAX + 1u)));
    TEST_ASSERT_EQUAL_UINT32(BEACON_INTERVAL_READY,
                             state_machine_get_beacon_interval());
}

/* The SEU-scrub hook publishes the critical struct extent; a NULL len is
 * accepted (size unknown, address still valid). */
void test_critical_region_reports_extent(void)
{
    size_t len = 0u;
    void  *p   = state_machine_critical_region(&len);

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_NOT_NULL(state_machine_critical_region(NULL));
}

/* ---------- Task body: boot sequence + autonomous loop ----------
 * (run last — see the note above run_task_until_delay). */

/* Nominal boot through the real task: s0->s1->s3, then one quiet main-loop
 * iteration (healthy battery, nothing to do). */
void test_task_boot_reaches_ready_and_idles(void)
{
    bms_set_soc_stub(100u);
    run_task_until_delay(3);

    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
    TEST_ASSERT_EQUAL_INT(0, boot_fault_acks);
}

/* The autonomous loop contains on low battery without any ground command:
 * boot reaches READY (s1->s3 checks no SoC), the first iteration sees
 * SoC <= b_scrit and drives s3->s2 by itself. */
void test_task_loop_drives_crit_on_battery_low(void)
{
    bms_set_soc_stub(10u);
    run_task_until_delay(3);

    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());

    bms_set_soc_stub(100u);   /* no leakage into later tests */
}

/* An untrusted image survives into the task boot: INIT bookkeeping still
 * commits, nominal ops stay refused, and the task contains to CRIT under
 * the image-CRC trigger (no parity ack — that latch was never the cause). */
void test_task_boot_untrusted_image_enters_crit(void)
{
    bms_set_soc_stub(100u);
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC ^ 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());

    run_task_until_delay(3);

    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
    TEST_ASSERT_EQUAL_INT(0, boot_fault_acks);
}

/* SRAM2 reports a parity finding: the task latches it, contains to CRIT
 * under the SRAM2 trigger with the record landed (ack released), and the
 * first loop iteration cannot recover — the latch outlives the ack. */
void test_task_boot_parity_fault_latches_and_confines(void)
{
    bms_set_soc_stub(100u);
    sim_boot_fault = 1;
    run_task_until_delay(3);

    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
    TEST_ASSERT_EQUAL_INT(1, boot_fault_acks);

    /* Confinement is in force for every later caller: nominal targets stay
     * refused while CRIT (and INIT bookkeeping) remain reachable. */
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}

/* Fail-closed latch (roast-8): when the LastStates write fails under the
 * safe-state entry, CRIT is forced by hand with NO record — and the
 * reset-stable finding must NOT be acked, or the next boot would come up
 * nominal with no trace of either the finding or the containment. */
void test_task_boot_parity_fault_without_record_keeps_latch(void)
{
    bms_set_soc_stub(100u);
    sim_boot_fault = 1;
    /* Refuse every LastStates write without touching Flash (robust against
     * laststates_write()'s program pattern: the single-shot flash-failure
     * injection would be consumed by the INIT attempt and let the CRIT
     * record land, which is the opposite of this test). */
    laststates_pool_lock_set_result_for_test(LASTSTATES_LOCK_FAILED);
    run_task_until_delay(3);

    /* Containment stands (forced), evidence missing (nothing acked). */
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
    TEST_ASSERT_EQUAL_INT(0, boot_fault_acks);

    /* ... and the confinement still holds afterwards. */
    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}
