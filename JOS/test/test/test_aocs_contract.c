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
 * TWO DIFFERENT THINGS are pinned here, and the tests keep them apart:
 *   - AOCS_STATUS, the 4-valued SUBSYSTEM STATE from SW_DATA_TYPES.xlsx
 *     (OFF, DET, POINTING, FAULT, 2 bit in the beacon STATUS byte);
 *     the source names the states but does not assign numeric wire codes.
 *   - T_AOCS_MODE, the 2-valued TELEMETRY field from the SPF (0/1), plus
 *     the explicit 4->2 projection that maps the two OFF/FAULT states down.
 *
 * These assertions deliberately restate the SPF literals rather than
 * referencing the macros on both sides: comparing a macro to itself proves
 * nothing. AOCS_STATUS tests below check only source-pinned width/state
 * coverage and keep the current enum order visibly provisional.
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

/* Mode codes of the T_AOCS_MODE TELEMETRY field. This field stays 2-valued:
 * the SPF does not define more, and it is NOT where the 4-state subsystem
 * state lives (that is AOCS_STATUS, pinned below). */
void test_aocs_mode_telemetry_codes_are_two_valued(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, AOCS_MODE_DETUMBLING);
    TEST_ASSERT_EQUAL_UINT32(1U, AOCS_MODE_NADIR_POINTING);
}

/* AOCS_STATUS is a FOUR-valued state, not the 2-valued telemetry field. The
 * source names the four states and assigns a 2-bit field, but does not assign
 * numeric wire codes. Keep this test about coverage/range rather than making
 * the current 0..3 declaration look like a released encoding. */
void test_aocs_status_has_four_distinct_in_range_states(void)
{
    TEST_ASSERT_TRUE(AOCS_STATE_OFF < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_DET < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_POINTING < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_FAULT < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_OFF, AOCS_STATE_DET);
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_OFF, AOCS_STATE_POINTING);
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_OFF, AOCS_STATE_FAULT);
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_DET, AOCS_STATE_POINTING);
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_DET, AOCS_STATE_FAULT);
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_POINTING, AOCS_STATE_FAULT);
}

/* AOCS_STATUS is exactly 2 bits (SW_DATA_TYPES.xlsx), so four distinct
 * implementation states must fit in that field. Numeric wire codes remain
 * unassigned by the source set. */
void test_aocs_status_is_two_bits_and_covers_all_four_states(void)
{
    TEST_ASSERT_EQUAL_UINT32(2U, AOCS_STATUS_BITS);
    TEST_ASSERT_TRUE(AOCS_STATE_OFF < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_DET < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_POINTING < (1U << AOCS_STATUS_BITS));
    TEST_ASSERT_TRUE(AOCS_STATE_FAULT < (1U << AOCS_STATUS_BITS));
}

/* The two bits occupy beacon STATUS bit 3..4, 1-indexed, and the declared
 * range must span exactly AOCS_STATUS_BITS positions. */
void test_aocs_status_hb_bit_range_matches_width(void)
{
    TEST_ASSERT_EQUAL_UINT32(3U, AOCS_STATUS_HB_BIT_FIRST);
    TEST_ASSERT_EQUAL_UINT32(4U, AOCS_STATUS_HB_BIT_LAST);
    TEST_ASSERT_EQUAL_UINT32(AOCS_STATUS_BITS,
                             AOCS_STATUS_HB_BIT_LAST - AOCS_STATUS_HB_BIT_FIRST + 1U);
}

/* The 4->2 projection, pinned with literal inputs and literal expected
 * outputs so it cannot be "verified" against its own macros. OFF and FAULT
 * project to 0: see the conservative-encoding note in aocs.h — value 1
 * asserts "under control" and must never be emitted for a state that is not
 * POINTING. */
void test_aocs_state_to_tlm_mode_projection_table(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode(AOCS_STATE_OFF));
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode(AOCS_STATE_DET));
    TEST_ASSERT_EQUAL_UINT32(1U, aocs_state_to_tlm_mode(AOCS_STATE_POINTING));
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode(AOCS_STATE_FAULT));
}

/* Explicitly: FAULT and OFF are never reported as Nadir-Pointing. If this
 * ever fails, ground is being told the satellite is stabilised when it is
 * not — the exact failure mode this correction exists to prevent. */
void test_aocs_fault_and_off_never_report_nadir_pointing(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode(AOCS_STATE_FAULT));
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode(AOCS_STATE_OFF));
    TEST_ASSERT_NOT_EQUAL_UINT32(1U, aocs_state_to_tlm_mode(AOCS_STATE_FAULT));
    TEST_ASSERT_NOT_EQUAL_UINT32(1U, aocs_state_to_tlm_mode(AOCS_STATE_OFF));
}

/* The inverse projection is lossy by construction: the field carries two
 * values, so it can only ever decode to DET or POINTING. OFF and FAULT are
 * NOT recoverable from T_AOCS_MODE (read AOCS_STATUS instead). */
void test_aocs_tlm_mode_to_state_is_two_valued_only(void)
{
    TEST_ASSERT_EQUAL_UINT32(AOCS_STATE_DET, aocs_tlm_mode_to_state(0U));
    TEST_ASSERT_EQUAL_UINT32(AOCS_STATE_POINTING, aocs_tlm_mode_to_state(1U));
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_FAULT, aocs_tlm_mode_to_state(0U));
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_FAULT, aocs_tlm_mode_to_state(1U));
}

/* Out-of-range state values. aocs_state_t names four states in a 2-bit
 * field; any value at or above that field's range is unknown. That is exactly
 * what the `default:` arm of the switch is for — and until this test it was
 * never exercised (guaranteed by the code, not demonstrated by the tests).
 * An unknown state must fall back to DETUMBLING (0), never to Nadir-Pointing:
 * reporting a state we do not recognise as "under control" is the failure this
 * whole correction exists to prevent. */
void test_aocs_state_to_tlm_mode_out_of_range_falls_back_to_detumbling(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode((aocs_state_t)4));
    TEST_ASSERT_EQUAL_UINT32(0U, aocs_state_to_tlm_mode((aocs_state_t)255));
    TEST_ASSERT_NOT_EQUAL_UINT32(1U, aocs_state_to_tlm_mode((aocs_state_t)4));
    TEST_ASSERT_NOT_EQUAL_UINT32(1U, aocs_state_to_tlm_mode((aocs_state_t)255));
}

/* Out-of-range mode bytes. T_AOCS_MODE is 1 byte but defines only two codes
 * (0/1); any other byte (2..255) is undefined by the SPF. The inverse
 * projection must decode it to DET (1), not POINTING: an unrecognised mode
 * byte must not be read as "the satellite is under control". This is the
 * implicit `else` arm of the ternary, untested until now. */
void test_aocs_tlm_mode_to_state_out_of_range_falls_back_to_det(void)
{
    TEST_ASSERT_EQUAL_UINT32(AOCS_STATE_DET, aocs_tlm_mode_to_state(2U));
    TEST_ASSERT_EQUAL_UINT32(AOCS_STATE_DET, aocs_tlm_mode_to_state(255U));
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_POINTING, aocs_tlm_mode_to_state(2U));
    TEST_ASSERT_NOT_EQUAL_UINT32(AOCS_STATE_POINTING, aocs_tlm_mode_to_state(255U));
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
