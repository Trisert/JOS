/* ---------------------------------------------------------------------------
 * test_boot_crc.c - unit tests for App/obsw/boot_crc.c
 *
 * Two layers are covered:
 *
 *   1. boot_crc32() as a pure function, against published CRC-32 (IEEE 802.3
 *      / zlib) known-answer vectors. A CRC whose polynomial, reflection or
 *      final XOR silently disagrees with the ground-segment implementation is
 *      worse than no CRC at all, so the check value 0xCBF43926 for "123456789"
 *      is asserted explicitly.
 *
 *   2. boot_crc_verify() and its four latching accessors, driven over the
 *      host doubles in support/stubs.c. All three reachable outcomes
 *      (UNSTAMPED, OK, MISMATCH) are exercised; BOOT_CRC_BAD_REGION needs a
 *      different linker-symbol layout and lives in test_bad_region.c.
 *
 * Refs: ECSS-E-ST-40C 5.5 (validation), NASA-STD-8739.8 (fault detection
 *       evidence), JPL-182 Rule 31.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "boot_crc.h"
#include "host_support.h"

#include <stdint.h>
#include <stddef.h>

/* boot_crc.c's fake image, provided by support/stubs.c. */
extern const uint8_t __fw_image_start[];

void setUp(void)
{
    /* Every test starts from an unstamped image, like a fresh debug build. */
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
}

void tearDown(void)
{
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
}

/* =====================================================================
 * boot_crc32() - known-answer vectors
 * ===================================================================== */

/* The canonical CRC-32 check value: CRC of the ASCII string "123456789". */
void test_boot_crc32_matches_canonical_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, boot_crc32("123456789", 9));
}

/* Empty input: init 0xFFFFFFFF XOR final 0xFFFFFFFF == 0. */
void test_boot_crc32_of_empty_input_is_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32("", 0));
}

/* Documented contract in boot_crc.c: a NULL buffer returns 0 rather than
 * dereferencing. Reached before the length is even looked at. */
void test_boot_crc32_null_pointer_returns_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32(NULL, 10));
}

void test_boot_crc32_single_zero_byte(void)
{
    const uint8_t b = 0x00u;
    TEST_ASSERT_EQUAL_HEX32(0xD202EF8Du, boot_crc32(&b, 1));
}

void test_boot_crc32_single_ones_byte(void)
{
    const uint8_t b = 0xFFu;
    TEST_ASSERT_EQUAL_HEX32(0xFF000000u, boot_crc32(&b, 1));
}

void test_boot_crc32_long_ascii_vector(void)
{
    const char *s = "The quick brown fox jumps over the lazy dog";
    TEST_ASSERT_EQUAL_HEX32(0x414FA339u, boot_crc32(s, 43));
}

/* The nibble-table implementation must be order sensitive: a CRC that ignores
 * byte order would not detect a swapped uplink fragment. */
void test_boot_crc32_is_order_sensitive(void)
{
    const uint8_t a[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    const uint8_t b[4] = { 0xEFu, 0xBEu, 0xADu, 0xDEu };

    TEST_ASSERT_NOT_EQUAL(boot_crc32(a, sizeof(a)), boot_crc32(b, sizeof(b)));
}

/* A single flipped bit must change the CRC (basic error-detection property). */
void test_boot_crc32_detects_single_bit_flip(void)
{
    uint8_t  buf[32];
    uint32_t clean;
    int      i;

    for (i = 0; i < 32; i++) {
        buf[i] = (uint8_t)(i * 7 + 3);
    }
    clean = boot_crc32(buf, sizeof(buf));

    buf[17] ^= 0x01u;
    TEST_ASSERT_NOT_EQUAL(clean, boot_crc32(buf, sizeof(buf)));
}

/* Known-answer vector over a binary (non-ASCII) buffer, so the byte path is
 * pinned for values above 0x7F as well. buf[i] = i * 7 + 3, i in [0, 32);
 * both check values come from Python zlib.crc32(), an independent
 * implementation. The flipped-bit variant is a second known answer, not just
 * "different from the clean one": a corrupted block must hash to exactly this
 * value, which also catches an implementation that merely scrambles input. */
void test_boot_crc32_binary_ramp_known_answer(void)
{
    uint8_t buf[32];
    int     i;

    for (i = 0; i < 32; i++) {
        buf[i] = (uint8_t)(i * 7 + 3);
    }

    TEST_ASSERT_EQUAL_HEX32(0xA10E8695u, boot_crc32(buf, sizeof(buf)));

    buf[17] ^= 0x01u;
    TEST_ASSERT_EQUAL_HEX32(0x18F55D7Du, boot_crc32(buf, sizeof(buf)));
}

/* A truncated read of the same buffer must not produce the full-length CRC:
 * length is part of the message, so short reads are detectable. */
void test_boot_crc32_is_length_sensitive(void)
{
    uint8_t buf[32];
    int     i;

    for (i = 0; i < 32; i++) {
        buf[i] = (uint8_t)(i * 7 + 3);
    }

    TEST_ASSERT_NOT_EQUAL(0xA10E8695u, boot_crc32(buf, sizeof(buf) - 1u));
}

/* Cross-check the host fixture itself: the expected image CRC baked into
 * host_support.h was produced by Python's zlib.crc32, i.e. an independent
 * implementation. If this fails, boot_crc32 and zlib disagree. */
void test_boot_crc32_of_fixture_image_matches_zlib(void)
{
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC,
                            boot_crc32(__fw_image_start, HOST_FW_IMAGE_LEN));
}

/* =====================================================================
 * boot_crc_verify() - image verification outcomes
 * ===================================================================== */

/* An image that was flashed straight from the .elf keeps the 0xFFFFFFFF
 * placeholder. That must be reported, not treated as corruption. */
void test_boot_crc_verify_reports_unstamped_placeholder(void)
{
    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_UNSTAMPED, boot_crc_verify());
    TEST_ASSERT_EQUAL_HEX32(BOOT_CRC_UNSTAMPED_VALUE, boot_crc_get_expected());
}

/* Positive path: a correctly stamped image verifies OK. */
void test_boot_crc_verify_accepts_correctly_stamped_image(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC, boot_crc_get_computed());
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC, boot_crc_get_expected());
}

/* Negative path: one wrong bit in the stored word must be flagged. */
void test_boot_crc_verify_rejects_corrupted_image(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC ^ 0x00000001u);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
    TEST_ASSERT_NOT_EQUAL(boot_crc_get_expected(), boot_crc_get_computed());
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC, boot_crc_get_computed());
}

/* A stored word that happens to be zero is still a mismatch, not "unstamped":
 * only 0xFFFFFFFF means unstamped. */
void test_boot_crc_verify_treats_zero_stored_word_as_mismatch(void)
{
    host_fw_crc_stamp(0x00000000u);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
}

/* =====================================================================
 * Latching accessors
 * ===================================================================== */

/* The region length is derived from the linker symbols; it must equal the
 * size of the fixture image and be latched for telemetry. */
void test_boot_crc_get_region_len_matches_linker_symbols(void)
{
    (void)boot_crc_verify();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)HOST_FW_IMAGE_LEN, boot_crc_get_region_len());
}

/* boot_crc_get_status() must return the same value boot_crc_verify() did,
 * for every outcome; telemetry reads the accessor, not the return value. */
void test_boot_crc_get_status_latches_each_outcome(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_get_status());

    host_fw_crc_stamp(HOST_FW_IMAGE_CRC ^ 0xFFFFu);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_MISMATCH, boot_crc_get_status());

    host_fw_crc_stamp(BOOT_CRC_UNSTAMPED_VALUE);
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_UNSTAMPED, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_UNSTAMPED, boot_crc_get_status());
}

/* Verification has no side effects on the image, so it is repeatable. */
void test_boot_crc_verify_is_idempotent(void)
{
    host_fw_crc_stamp(HOST_FW_IMAGE_CRC);

    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());
    TEST_ASSERT_EQUAL_INT(BOOT_CRC_OK, boot_crc_verify());
    TEST_ASSERT_EQUAL_HEX32(HOST_FW_IMAGE_CRC, boot_crc_get_computed());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)HOST_FW_IMAGE_LEN, boot_crc_get_region_len());
}
