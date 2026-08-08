/* ---------------------------------------------------------------------------
 * test_boot_policy.c - unit tests for the boot-CRC *fault policy* in
 *                      App/obsw/boot_crc.c.
 *
 * test_boot_crc.c pins the CRC routine and the verification verdicts; this
 * file pins what the OBC actually DOES with a verdict:
 *
 *   boot_crc_image_trusted()   - which verdicts allow nominal operations;
 *   boot_crc_apply_policy()    - persist the fault in the LastStates pool,
 *                                reset up to BOOT_CRC_MAX_RESET_ATTEMPTS
 *                                times to recover from a transient (SEU)
 *                                corruption, then keep booting *untrusted*
 *                                so ground can re-upload (RedPill has no
 *                                golden image and no IWDG: halting would be
 *                                an unrecoverable brick).
 *
 * Why its own executable: the retry counter and its magic word live in
 * .noinit, i.e. they are process-global static state that survives a warm
 * reset by design. A separate Ceedling test executable gives this file a
 * pristine "cold start" (.noinit reads as zero => magic invalid), which is
 * the initial condition the cold-start branch needs. The tests below then run
 * in source order and each one documents the .noinit state it inherits, the
 * same way successive warm resets inherit it on the spacecraft.
 *
 * The reset itself is captured by support/hal_stubs.c: NVIC_SystemReset()
 * long-jumps back to HOST_EXPECT_NVIC_RESET() instead of returning, so the
 * flight code never executes anything after the reset request - and any
 * *unexpected* reset fails the test loudly.
 *
 * Refs: ECSS-E-ST-40C 5.5 (validation), ECSS-Q-ST-80C 6.3.5 (post-mortem
 *       evidence), NASA-STD-8739.8 (fault detection/recovery evidence).
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "boot_crc.h"
#include "memory.h"
#include "obsw_types.h"
#include "seu_mitigation.h"   /* fakes/: lock/commit bookkeeping stubs */
#include "host_support.h"

#include <stdint.h>
#include <string.h>

/* A stored word that is neither of the two reserved sentinels and does not
 * match the fixture image, i.e. a genuine corruption. */
#define POLICY_BAD_STAMP  (HOST_FW_IMAGE_CRC ^ 0x0000BEEFu)

void setUp(void)
{
    host_flash_reset();       /* erased LastStates pool + zeroed counters */
    seu_stub_reset();
    host_hal_tick_reset();    /* HAL_GetTick() restarts at 0 */
    host_nvic_reset_clear();
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
    laststates_init();
}

void tearDown(void)
{
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
}

static void stamp_and_verify(uint32_t stored_word, boot_crc_status_t expected)
{
    host_fw_crc_stamp(stored_word);
    TEST_ASSERT_EQUAL_INT(expected, boot_crc_verify());
}

/* Check the post-mortem record boot_crc_record_fault() appended to the pool.
 *
 * The layout is part of the ground forensics contract: the transition is
 * reported as OFF -> CRIT with TRIGGER_IMAGE_CRC_FAIL and the context carries
 * {status, expected CRC, computed CRC, region length, attempt number}. A
 * record that lost any of those tells ground nothing about why the OBC came
 * up in safe mode. */
static void assert_fault_record(uint32_t slot, uint32_t timestamp,
                                boot_crc_status_t status,
                                uint32_t expected_word, uint32_t attempt)
{
    laststates_entry_t entry;
    uint32_t           ctx[5];

    memcpy(&entry, host_flash_pool() + (size_t)slot * LASTSTATES_ENTRY_SIZE,
           sizeof(entry));
    memcpy(ctx, entry.context, sizeof(ctx));

    TEST_ASSERT_EQUAL_UINT32(timestamp, entry.timestamp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)STATE_OFF,  entry.state_from);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)STATE_CRIT, entry.state_to);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)TRIGGER_IMAGE_CRC_FAIL, entry.trigger);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)status, ctx[0]);
    TEST_ASSERT_EQUAL_HEX32(expected_word, ctx[1]);            /* stored word */
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC, ctx[2]);        /* computed    */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)HOST_FW_IMAGE_LEN, ctx[3]);
    TEST_ASSERT_EQUAL_UINT32(attempt, ctx[4]);
}

/* =====================================================================
 * boot_crc_image_trusted()
 * ===================================================================== */

/* With the flight policy (BOOT_CRC_TRUST_UNSTAMPED = 0, the default) only a
 * verified image may be trusted. MISMATCH and ERASED are obvious faults;
 * UNSTAMPED is one too, because an image flashed straight from the .elf
 * (CubeIDE / OpenOCD, or `make crc-stamp` skipped) carries no integrity
 * evidence at all - trusting it turns the whole boot check into a no-op on
 * exactly the path most likely to be used by hand.
 * test_boot_unstamped_bench.c pins the opposite, opt-in bench behaviour. */
void test_boot_crc_image_trusted_only_for_a_verified_image(void)
{
    stamp_and_verify(HOST_FW_IMAGE_CRC, BOOT_CRC_OK);
    TEST_ASSERT_EQUAL_INT(1, boot_crc_image_trusted());

    stamp_and_verify(BOOT_CRC_UNSTAMPED_VALUE, BOOT_CRC_UNSTAMPED);
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());

    stamp_and_verify(POLICY_BAD_STAMP, BOOT_CRC_MISMATCH);
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());

    stamp_and_verify(BOOT_CRC_ERASED_VALUE, BOOT_CRC_ERASED);
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());
}

/* =====================================================================
 * boot_crc_apply_policy()
 * ===================================================================== */

/* The full recovery campaign of a corrupted image, from the cold start to the
 * exhausted retry budget. Runs first in this executable on purpose: .noinit
 * still reads as zero here, so the magic word is invalid and the cold-start
 * branch (initialise the budget) is the one taken.
 *
 * Every boot records its own evidence *before* the reset, so ground gets one
 * LastStates entry per attempt - including the last one, where the OBC gives
 * up and continues booting in safe mode instead of reset-looping forever. */
void test_boot_crc_apply_policy_resets_until_the_budget_is_exhausted(void)
{
    stamp_and_verify(POLICY_BAD_STAMP, BOOT_CRC_MISMATCH);

    /* Cold start: nothing has been counted yet. */
    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_reset_attempts());

    /* Boot 1: fault recorded (attempt 0), reboot requested. */
    HOST_EXPECT_NVIC_RESET(boot_crc_apply_policy());
    TEST_ASSERT_EQUAL_UINT32(1u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());
    assert_fault_record(0u, 0u, BOOT_CRC_MISMATCH, POLICY_BAD_STAMP, 0u);

    /* Boot 2 (warm: the magic word is now valid, the budget is NOT reset). */
    HOST_EXPECT_NVIC_RESET(boot_crc_apply_policy());
    TEST_ASSERT_EQUAL_UINT32(2u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(2u, laststates_count());
    assert_fault_record(1u, 1u, BOOT_CRC_MISMATCH, POLICY_BAD_STAMP, 1u);

    TEST_ASSERT_EQUAL_UINT32(BOOT_CRC_MAX_RESET_ATTEMPTS, host_nvic_reset_count());

    /* Boot 3: budget exhausted. The fault is still recorded, but the OBC must
     * NOT reset again - hal_stubs.c fails this test if it does - and must keep
     * running with the image untrusted (state machine -> STATE_CRIT). */
    boot_crc_apply_policy();
    TEST_ASSERT_EQUAL_UINT32(BOOT_CRC_MAX_RESET_ATTEMPTS, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(BOOT_CRC_MAX_RESET_ATTEMPTS, host_nvic_reset_count());
    TEST_ASSERT_EQUAL_UINT32(3u, laststates_count());
    assert_fault_record(2u, 2u, BOOT_CRC_MISMATCH, POLICY_BAD_STAMP, 2u);
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());
}

/* A healthy boot arms the counter for a *future* in-flight corruption and
 * touches nothing else: no reset, no Flash write, no forensic record. The
 * budget inherited from the previous test (2/2, exhausted) must be cleared,
 * otherwise a single historical corruption would permanently disable the
 * recovery reset. */
void test_boot_crc_apply_policy_on_trusted_image_rearms_the_budget(void)
{
    TEST_ASSERT_EQUAL_UINT32(BOOT_CRC_MAX_RESET_ATTEMPTS,
                             boot_crc_get_reset_attempts());

    stamp_and_verify(HOST_FW_IMAGE_CRC, BOOT_CRC_OK);
    boot_crc_apply_policy();

    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_program_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_nvic_reset_count());
}

/* An unstamped image is a fault verdict under the flight policy: it is
 * recorded with its own status (not collapsed into MISMATCH) and it spends
 * the recovery budget like any other untrusted image. This is the test that
 * would have failed while boot_crc_image_trusted() returned 1 for UNSTAMPED,
 * i.e. while the CubeIDE/OpenOCD flashing path silently bypassed the check. */
void test_boot_crc_apply_policy_on_unstamped_image_is_a_fault(void)
{
    stamp_and_verify(BOOT_CRC_UNSTAMPED_VALUE, BOOT_CRC_UNSTAMPED);
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());

    HOST_EXPECT_NVIC_RESET(boot_crc_apply_policy());

    TEST_ASSERT_EQUAL_UINT32(1u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(1u, host_nvic_reset_count());
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());
    assert_fault_record(0u, 0u, BOOT_CRC_UNSTAMPED, BOOT_CRC_UNSTAMPED_VALUE, 0u);

    /* This executable shares one .noinit across its tests, the way successive
     * warm resets share it on the spacecraft. Hand the next test the clean
     * cold-start budget it documents as its precondition. */
    stamp_and_verify(HOST_FW_IMAGE_CRC, BOOT_CRC_OK);
    boot_crc_apply_policy();
    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_reset_attempts());
}

/* Proof that the re-arm above is real and not just a cleared accessor: after
 * the clean boot the OBC must be willing to spend the recovery budget again.
 * ERASED (0xFFFFFFFF, the image tail was never programmed) is used here so the
 * policy is pinned for a second fault verdict, and the record must carry that
 * status rather than collapsing it into MISMATCH. */
void test_boot_crc_apply_policy_reset_budget_is_usable_again_after_clean_boot(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_reset_attempts());

    stamp_and_verify(BOOT_CRC_ERASED_VALUE, BOOT_CRC_ERASED);

    HOST_EXPECT_NVIC_RESET(boot_crc_apply_policy());

    TEST_ASSERT_EQUAL_UINT32(1u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(1u, host_nvic_reset_count());
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());
    assert_fault_record(0u, 0u, BOOT_CRC_ERASED, BOOT_CRC_ERASED_VALUE, 0u);
}

/* The forensic record goes through laststates_write(), so it must honour the
 * SEU ownership contract of the ring bookkeeping (W2-5): commit inside a
 * balanced lock. A fault path that skipped the commit would have the scrubber
 * rewind the cursor and the next boot would overwrite this very record. */
void test_boot_crc_fault_record_commits_the_seu_mirror_under_lock(void)
{
    const int commits_after_init = seu_stub_commit_count();   /* from setUp() */

    stamp_and_verify(POLICY_BAD_STAMP, BOOT_CRC_MISMATCH);

    HOST_EXPECT_NVIC_RESET(boot_crc_apply_policy());

    TEST_ASSERT_EQUAL_INT(commits_after_init + 1, seu_stub_commit_count());
    TEST_ASSERT_EQUAL_INT((int)SEU_REGION_LASTSTATES, seu_stub_last_commit_region());
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());
    TEST_ASSERT_FALSE(host_flash_is_unlocked());
}
