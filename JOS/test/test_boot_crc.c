/* Host unit tests for App/obsw/boot_crc.c (W2-1 CRC-32, reused by W2-5).
 * Build with Ceedling: ceedling test:test_boot_crc
 *
 * Standards: NASA-STD-8739.8 (fault detection), ECSS-E-ST-40C §5.4 (integrity).
 */

#include "unity.h"
#include "boot_crc.h"

/* IEEE 802.3 CRC-32 check value for the ASCII string "123456789". */
static const char *CHECK_STRING = "123456789";
#define CRC32_CHECK_VALUE  0xCBF43926u

void setUp(void)   { /* no shared state to reset */ }
void tearDown(void) { /* no shared state to reset */ }

void test_crc32_known_vector(void)
{
    TEST_ASSERT_EQUAL_HEX32(CRC32_CHECK_VALUE,
                            boot_crc32(CHECK_STRING, 9u));
}

void test_crc32_empty_is_identity(void)
{
    /* Empty input: init XOR final = 0xFFFFFFFF ^ 0xFFFFFFFF = 0. */
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32(NULL, 0u));
}

void test_crc32_null_ptr_returns_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32(NULL, 100u));
}

void test_crc32_changes_with_one_bit_flip(void)
{
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(i * 7 + 3);

    uint32_t base = boot_crc32(buf, sizeof(buf));
    buf[3] ^= 0x40u;                       /* flip one bit in the payload */
    uint32_t flipped = boot_crc32(buf, sizeof(buf));

    TEST_ASSERT_NOT_EQUAL_HEX32(base, flipped);
    /* A single bit flip must change the CRC (otherwise it cannot detect SEU). */
    TEST_ASSERT_TRUE(flipped != 0u);
}

void test_crc32_is_order_independent_of_pointer(void)
{
    /* Recomputing identical content yields identical CRC. */
    uint8_t a[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t b[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_HEX32(boot_crc32(a, 4), boot_crc32(b, 4));
}
