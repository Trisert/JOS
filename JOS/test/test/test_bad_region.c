/* ---------------------------------------------------------------------------
 * test_bad_region.c - covers the BOOT_CRC_BAD_REGION guard in boot_crc.c.
 *
 * boot_crc_verify() refuses to compute anything when the linker symbols are
 * inconsistent (__fw_crc_start <= __fw_image_start), which would otherwise
 * produce a negative/absurd region length and a meaningless CRC.
 *
 * That layout cannot coexist with the normal one inside a single executable,
 * so this test file is built as its own Ceedling test executable:
 * project.yml adds
 * -DHOST_FW_BAD_REGION for any test executable matching 'test_bad_region',
 * which makes support/stubs.c place __fw_crc_start at the *start* of the
 * fake image.
 *
 * Refs: ECSS-E-ST-40C 5.5, NASA-STD-8739.8 (defensive-branch coverage).
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "boot_crc.h"
#include "host_support.h"

#include <stdint.h>

void setUp(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);   /* a "good" stamp must not help */
}

void tearDown(void)
{
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
}

/* An inverted / empty image region must be reported as BAD_REGION even when
 * the stored CRC word looks plausible. */
void test_boot_crc_verify_rejects_inconsistent_linker_symbols(void)
{
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_BAD_REGION, boot_crc_verify());
}

/* The failure must be latched for telemetry and must zero the region length,
 * so downstream code cannot mistake it for a verified image. */
void test_boot_crc_bad_region_latches_status_and_zero_length(void)
{
    (void)boot_crc_verify();

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_BAD_REGION, boot_crc_get_status());
    TEST_ASSERT_EQUAL_UINT32(0u, boot_crc_get_region_len());
}

/* The guard must return before any CRC is computed: no OK/MISMATCH verdict
 * may be produced from a broken region. */
void test_boot_crc_bad_region_never_reports_ok(void)
{
    boot_crc_status_t status = boot_crc_verify();

    TEST_ASSERT_NOT_EQUAL(BOOT_CRC_OK, status);
    TEST_ASSERT_NOT_EQUAL(BOOT_CRC_MISMATCH, status);
}

/* The pure CRC routine is unaffected by the broken linker layout. */
void test_boot_crc32_still_usable_with_bad_region(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, boot_crc32("123456789", 9));
}
