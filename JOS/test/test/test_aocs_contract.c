/* ---------------------------------------------------------------------------
 * test_aocs_contract.c — pins the OBC-side AOCS telemetry contract.
 *
 * WHY THIS EXISTS: the values below are a WIRE CONTRACT with the ground
 * segment. T_AOCS_MODE and T_ANGULAR_RATE are addressed by position and
 * length inside the telemetry frame (SPF operational database), so an
 * innocent edit to aocs.h — renumbering, "tidying" a length, collapsing the
 * three rate thresholds into one — would silently change what ground parses.
 * Nothing else in the build would notice.
 *
 * These assertions deliberately restate the literals rather than referencing
 * the macros on both sides: comparing a macro to itself proves nothing.
 *
 * The AOCS DRIVER is not tested here because it does not exist: the SPI
 * frame format is not specified in the delivered documentation (see the
 * blocker list in docs/api/aocs.md). This file pins only what the SPF fixes.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "aocs.h"
/* aocs.c is compiled alongside (Ceedling links the source matching the header
 * it is asked about), so its dependencies must resolve too: watchdog.h gives
 * CMock a mock for watchdog_alive_self()/watchdog_register_task(), and
 * mock_cmsis_os.h covers osDelay()/osThreadNew(). Same pairing as
 * test_watchdog.c, which also links aocs.c. */
#include "watchdog.h"
#include "mock_cmsis_os.h"
#include "mock_task.h"      /* xTaskGetTickCount()/uxTaskGetStackHighWaterMark() */

void setUp(void)   { }
void tearDown(void) { }

/* T_AOCS_MODE — telemetry position 20, 1 B. */
void test_aocs_mode_telemetry_position_and_length(void)
{
    TEST_ASSERT_EQUAL_UINT32(20U, AOCS_TLM_MODE_POS);
    TEST_ASSERT_EQUAL_UINT32(1U,  AOCS_TLM_MODE_LEN);
}

/* Mode codes are the documented values, not an enum that can be reordered. */
void test_aocs_mode_codes_are_the_specified_values(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, AOCS_MODE_DETUMBLING);
    TEST_ASSERT_EQUAL_UINT32(1U, AOCS_MODE_NADIR_POINTING);
}

/* T_ANGULAR_RATE — position 21, 6 B = 3 axes x 2 B, signed. The internal
 * arithmetic of the length must hold, or a consumer walking the buffer with
 * AOCS_TLM_RATE_AXES / AOCS_TLM_RATE_BYTES_AXIS would read out of bounds. */
void test_aocs_rate_telemetry_shape(void)
{
    TEST_ASSERT_EQUAL_UINT32(21U, AOCS_TLM_RATE_POS);
    TEST_ASSERT_EQUAL_UINT32(6U,  AOCS_TLM_RATE_LEN);
    TEST_ASSERT_EQUAL_UINT32(3U,  AOCS_TLM_RATE_AXES);
    TEST_ASSERT_EQUAL_UINT32(2U,  AOCS_TLM_RATE_BYTES_AXIS);
    TEST_ASSERT_EQUAL_UINT32(AOCS_TLM_RATE_LEN,
                             AOCS_TLM_RATE_AXES * AOCS_TLM_RATE_BYTES_AXIS);
}

/* The two telemetry entries must not overlap: MODE is 1 B at position 20, so
 * RATE at position 21 is exactly contiguous and must stay so. */
void test_aocs_mode_and_rate_do_not_overlap(void)
{
    TEST_ASSERT_EQUAL_UINT32(AOCS_TLM_MODE_POS + AOCS_TLM_MODE_LEN,
                             AOCS_TLM_RATE_POS);
}

/* Three distinct thresholds with distinct roles. Collapsing them (e.g. using
 * the 5 deg/s handover value as the critical-event threshold) would change
 * when the OBSW declares a critical event, which is a ground-visible
 * behaviour change — hence three separate assertions. */
void test_aocs_rate_thresholds_keep_their_distinct_roles(void)
{
    TEST_ASSERT_EQUAL_UINT32(10U, AOCS_TUMBLE_RATE_XY_DPS);
    TEST_ASSERT_EQUAL_UINT32(20U, AOCS_TUMBLE_RATE_Z_DPS);
    TEST_ASSERT_EQUAL_UINT32(10U, AOCS_RATE_CRIT_DPS);
    TEST_ASSERT_EQUAL_UINT32(5U,  AOCS_DETUMBLE_HANDOVER_DPS);
}
