/* ---------------------------------------------------------------------------
 * test_state_machine.c - host unit tests for App/obsw/state_machine.c.
 *
 * Covers the SPF v3 Table 3.21 s4->s3 transition: when a scheduled task or
 * payload operation finishes, the OBSW returns from STATE_ACTIVE to
 * STATE_READY on TRIGGER_TASK_COMPLETE. Before the fix, try_transition()
 * had no ACTIVE branch under the STATE_READY target, so the nominal
 * end-of-task path was refused (-1) and the OBSW was stuck in ACTIVE until
 * a battery event or a ground command moved it.
 *
 * The real state_machine.c is linked unmodified. Target-only neighbours are
 * replaced by local doubles: a no-op RTOS (no scheduler on the host),
 * watchdog registration capture (watchdog.c is not host-compilable) and
 * controllable SRAM2-parity seams (Core/Src/sram2_parity.c needs silicon).
 * LastStates persistence runs against the real memory.c + host Flash model,
 * the FRAM write-through against the real scrub.c + an in-memory FRAM, and
 * the boot-CRC confinement gate against the real boot_crc.c with a stamped
 * host image, so the transitions under test traverse the flight code paths.
 *
 * Refs: ECSS-E-ST-40C 5.5, NASA-STD-8739.8 (unit test evidence),
 *       SPF v3 Table 3.21 (s4->s3 on task completion).
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "state_machine.h"      /* unit under test -> links state_machine.c */
#include "scrub.h"              /* real write-through target (fake FRAM below) */
#include "boot_crc.h"           /* boot_crc_verify(): establish a trusted image */
#include "memory.h"             /* real LastStates pool behind try_transition() */
#include "obsw_types.h"
#include "host_support.h"
#include "seu_mitigation.h"

#include <stdint.h>
#include <string.h>

/* NOTE: TRIGGER_TASK_COMPLETE already exists in obsw_types.h (= 5, part of
 * the ground telemetry contract, never renumbered). No enum change needed;
 * this typedef fails the build if the value ever drifts. */
typedef char trigger_task_complete_value_is_5[(TRIGGER_TASK_COMPLETE == 5) ? 1 : -1];

/* bms_set_soc_stub() lives in state_machine.c but is not part of the public
 * header (flight code never calls it); declared here for the CRIT-recovery
 * tests below. */
extern void bms_set_soc_stub(uint8_t soc);

/* ---------- Minimal RTOS double (no scheduler on the host) ---------- */
static uint8_t  fake_mutex_obj;
static uint8_t  fake_thread_obj;
static uint32_t fake_tick;

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
    (void)func;
    (void)argument;
    (void)attr;
    return (osThreadId_t)&fake_thread_obj;
}

osStatus_t osDelay(uint32_t ticks)
{
    (void)ticks;
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

/* ---------- In-memory FRAM for the scrub write-through (cf. test_scrub.c) --- */
static uint8_t g_fram[65536];

static int fake_fram_read(uint32_t addr, uint8_t *buf, size_t len)
{
    TEST_ASSERT_TRUE(addr + len <= sizeof(g_fram));
    memcpy(buf, g_fram + addr, len);
    return 0;
}

static int fake_fram_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    TEST_ASSERT_TRUE(addr + len <= sizeof(g_fram));
    memcpy(g_fram + addr, buf, len);
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

    memset(g_fram, 0x00, sizeof(g_fram));
    scrub_reset();
    scrub_bind_fram(fake_fram_read, fake_fram_write);

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

/* SPF Table 3.21: s4->s3 on task completion. */
void test_task_complete_returns_active_to_ready(void)
{
    uint8_t trail[4 * LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(trail);
    const laststates_entry_t *last;

    boot_to_active();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                   TRIGGER_TASK_COMPLETE));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());

    /* The completion must be on the forensic trail with from/to/trigger. */
    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(trail, &len));
    TEST_ASSERT_TRUE(len >= (size_t)LASTSTATES_ENTRY_SIZE);
    last = (const laststates_entry_t *)(trail + len - LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT8(STATE_ACTIVE, last->state_from);
    TEST_ASSERT_EQUAL_UINT8(STATE_READY, last->state_to);
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_TASK_COMPLETE, last->trigger);
}

/* s4->s3 is gated on the completion trigger: any other trigger is refused
 * and the OBSW stays in ACTIVE (cf. the s2->s4 GROUND_CMD gate). */
void test_active_to_ready_rejects_wrong_trigger(void)
{
    boot_to_active();

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                        TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());

    TEST_ASSERT_EQUAL_INT(-1, state_machine_request_transition(STATE_READY,
                                                          TRIGGER_BATTERY_OK));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* The s3->s4 entry path is unchanged by the new return branch. */
void test_ready_to_active_still_accepted(void)
{
    boot_to_ready();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
}

/* Safety regression: ACTIVE can still always reach CRIT on battery low. */
void test_active_to_crit_on_battery_low(void)
{
    boot_to_active();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_CRIT,
                                                       TRIGGER_BATTERY_LOW));
    TEST_ASSERT_EQUAL_INT(STATE_CRIT, (int)state_machine_get_state());
}

/* A full task cycle returns to READY and can start the next task. */
void test_task_cycle_can_repeat(void)
{
    boot_to_active();

    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                   TRIGGER_TASK_COMPLETE));
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_ACTIVE,
                                                       TRIGGER_GROUND_CMD));
    TEST_ASSERT_EQUAL_INT(STATE_ACTIVE, (int)state_machine_get_state());
    TEST_ASSERT_EQUAL_INT(0, state_machine_request_transition(STATE_READY,
                                                   TRIGGER_TASK_COMPLETE));
    TEST_ASSERT_EQUAL_INT(STATE_READY, (int)state_machine_get_state());
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
                                                   TRIGGER_TASK_COMPLETE));
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

/* ---------- Scrub write-through seams (same binary, same FRAM model) --- */

/* Fresh boot, golden copy never synced: scrub_init() reports the missing
 * backup instead of restoring garbage. */
void test_scrub_init_reports_empty_fram(void)
{
    TEST_ASSERT_EQUAL(SCRUB_ERR_MAGIC, scrub_init());
}

/* Unknown or unregistered region ids are rejected, never touching FRAM. */
void test_scrub_rejects_unknown_region(void)
{
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID, scrub_sync(SCRUB_MAX_REGIONS));
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID, scrub_sync(1u));
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID, scrub_refresh(SCRUB_MAX_REGIONS));
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID, scrub_refresh(1u));
}

/* An unbound FRAM transport is reported, not dereferenced. */
void test_scrub_reports_unbound_fram_backend(void)
{
    scrub_bind_fram(NULL, NULL);
    TEST_ASSERT_EQUAL(SCRUB_ERR_FRAM,
                      scrub_sync(SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL(SCRUB_ERR_FRAM,
                      scrub_refresh(SCRUB_REGION_OBSW_STATE));
    scrub_bind_fram(fake_fram_read, fake_fram_write);
}
