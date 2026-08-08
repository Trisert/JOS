/* ---------------------------------------------------------------------------
 * test_boot_unstamped_bench.c - the opt-in bench behaviour of the boot-CRC
 *                               unstamped-image policy.
 *
 * The flight/CI build compiles with BOOT_CRC_TRUST_UNSTAMPED = 0: an image
 * whose .fw_crc word is still the 0x00000000 placeholder carries no integrity
 * evidence, is therefore untrusted, and boot_crc_apply_policy() treats it as a
 * fault (pinned in test_boot_policy.c). That is what stops the boot check from
 * being a no-op when the .elf is flashed straight from CubeIDE / OpenOCD.
 *
 * A developer on the bench can opt out explicitly with
 * `make BOOT_CRC_TRUST_UNSTAMPED=1` (the Makefile prints a BENCH-ONLY banner).
 * This executable compiles boot_crc.c with exactly that define - see
 * :defines: :test_boot_unstamped_bench: in project.yml - and pins that the
 * opt-out really does restore the "nothing to check" behaviour, so the escape
 * hatch documented in boot_crc.h is tested rather than asserted.
 *
 * Refs: ECSS-E-ST-40C 5.5 (validation), ECSS-Q-ST-80C 6.3.5.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "boot_crc.h"
#include "memory.h"
#include "obsw_types.h"
#include "seu_mitigation.h"   /* fakes/: lock/commit bookkeeping stubs */
#include "host_support.h"

#include <stdint.h>

void setUp(void)
{
    host_flash_reset();
    seu_stub_reset();
    host_hal_tick_reset();
    host_nvic_reset_clear();
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
    laststates_init();
}

void tearDown(void)
{
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
}

/* The define really is in force in this executable. Without this the two
 * tests below would silently be re-testing the flight policy. */
void test_bench_build_opts_into_trusting_an_unstamped_image(void)
{
    TEST_ASSERT_EQUAL_INT(1, BOOT_CRC_TRUST_UNSTAMPED);
}

/* Bench opt-in: UNSTAMPED is still reported as UNSTAMPED (the verdict never
 * lies about what was found), but it is trusted, so a debugger session on a
 * freshly flashed .elf runs nominal operations. */
void test_bench_unstamped_image_is_trusted(void)
{
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_UNSTAMPED, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(1, boot_crc_image_trusted());
}

/* ... and the fault policy is a no-op for it: no forensic record, no Flash
 * write, no recovery reset, and the retry budget stays armed for a real
 * in-flight corruption. */
void test_bench_unstamped_image_takes_the_no_op_policy_path(void)
{
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_UNSTAMPED, boot_crc_verify());

    boot_crc_apply_policy();

    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_reset_attempts());
    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_program_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_nvic_reset_count());
}

/* A corrupted image is still a fault in a bench build: the opt-in covers the
 * "never stamped" case only, it does not disable the integrity check. */
void test_bench_build_still_rejects_a_corrupted_image(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC ^ 0x0000BEEFu);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(0, boot_crc_image_trusted());
}
