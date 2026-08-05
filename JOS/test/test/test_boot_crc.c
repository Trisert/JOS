/* Test suite for boot_crc32() — firmware image integrity check.
 * Ceedling + Unity. Pure function, no mocks required.
 * References: ECSS-E-ST-40C §5.4 (integrity), NASA-STD-8739.8 (verification).
 */
#include "unity.h"
#include "boot_crc.h"

/* Known-answer test vectors for CRC-32 (IEEE 802.3, poly 0xEDB88320,
 * reflected, init/xorout 0xFFFFFFFF) — identical to zlib.crc32. */
static const char *kTestStr = "123456789";   /* canonical CRC-32 check string */

void setUp(void) { }
void tearDown(void) { }

/* The single most important vector: the official CRC-32 check value. */
void test_boot_crc32_known_check_string(void) {
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, boot_crc32(kTestStr, 9));
}

/* Empty input must yield 0x00000000 (init XOR final = 0xFFFFFFFF ^ 0xFFFFFFFF). */
void test_boot_crc32_empty(void) {
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32("", 0));
}

/* NULL pointer is an explicit contract in boot_crc.c: returns 0. */
void test_boot_crc32_null_ptr_returns_zero(void) {
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, boot_crc32(NULL, 10));
}

/* Single byte 0x00 → 0xD202EF8D (standard table value). */
void test_boot_crc32_single_zero_byte(void) {
    uint8_t b = 0x00;
    TEST_ASSERT_EQUAL_HEX32(0xd202ef8du, boot_crc32(&b, 1));
}

/* Single byte 0xFF → 0xFF000000 (standard table value). */
void test_boot_crc32_single_0xff_byte(void) {
    uint8_t b = 0xFF;
    TEST_ASSERT_EQUAL_HEX32(0xff000000u, boot_crc32(&b, 1));
}

/* Longer ASCII string, cross-checked against the industry reference vector. */
void test_boot_crc32_longer_string(void) {
    const char *s = "The quick brown fox jumps over the lazy dog";
    TEST_ASSERT_EQUAL_HEX32(0x414fa339u, boot_crc32(s, 43));
}

/* Two identical buffers must produce identical CRC (determinism). */
void test_boot_crc32_deterministic(void) {
    uint8_t buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 7 + 3);
    uint32_t a = boot_crc32(buf, 32);
    uint32_t b = boot_crc32(buf, 32);
    TEST_ASSERT_EQUAL_HEX32(a, b);
}
