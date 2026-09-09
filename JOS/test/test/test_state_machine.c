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
