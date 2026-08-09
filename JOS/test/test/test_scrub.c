/* Host unit tests for App/obsw/scrub.c (W2-5 SEU scrubbing).
 *
 * scrub.c is compiled with SCRUB_NO_RTOS, so the RTOS task and the real
 * fram_read()/fram_write() (HAL/I2C) are excluded. Instead we inject an
 * in-memory FRAM model through scrub_bind_fram() and use it to inject
 * single/multi-bit upsets and FRAM corruption, exercising every branch:
 * register, write-through sync, periodic refresh repair, boot init, and the
 * golden-record CRC guard that must REJECT a corrupt backup rather than
 * restore wrong data.
 *
 * Standards: NASA-STD-8739.8 (fault tolerance, no silent data corruption),
 *            ECSS-E-ST-40C §5.4 (data integrity).
 */

#include "unity.h"
#include "scrub.h"
#include "boot_crc.h"
/* boot_crc.c is linked in for boot_crc32(); its fault path calls
   laststates_write(), so memory.c has to be linked too. Ceedling derives the
   link set from the headers a test includes, hence this include (same reason
   test_boot_crc.c/test_bad_region.c include it). */
#include "memory.h"
#include <string.h>

/* ---------- Fake FRAM model ---------- */
static uint8_t g_fram[65536];
static int     g_read_rc;     /* force transport failure */
static int     g_write_rc;

static int fake_read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (g_read_rc != 0) return g_read_rc;
    TEST_ASSERT_TRUE(addr + len <= sizeof(g_fram));
    memcpy(buf, g_fram + addr, len);
    return 0;
}

static int fake_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    if (g_write_rc != 0) return g_write_rc;
    TEST_ASSERT_TRUE(addr + len <= sizeof(g_fram));
    memcpy(g_fram + addr, buf, len);
    return 0;
}

/* ---------- Test fixture ---------- */
typedef struct {
    uint32_t magic;
    uint32_t counter;
    uint16_t flags;
} critical_t;

static critical_t g_crit;

void setUp(void)
{
    scrub_reset();
    memset(g_fram, 0x00, sizeof(g_fram));
    g_read_rc  = 0;
    g_write_rc = 0;
    memset(&g_crit, 0, sizeof(g_crit));
    g_crit.magic   = 0x12345678u;
    g_crit.counter = 42u;
    g_crit.flags   = 0x00FFu;
    scrub_bind_fram(fake_read, fake_write);
}

void tearDown(void) { }

/* ---------- Tests ---------- */

void test_register_and_sync_writes_crc_protected_record(void)
{
    TEST_ASSERT_EQUAL(SCRUB_OK, scrub_register(&g_crit, sizeof(g_crit),
                                               SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL(SCRUB_OK, scrub_sync(SCRUB_REGION_OBSW_STATE));

    /* The golden copy in FRAM must equal the live struct exactly. */
    scrub_record_t rec;
    memcpy(&rec, g_fram + SCRUB_SLOT_ADDR(SCRUB_REGION_OBSW_STATE),
           sizeof(rec));
    TEST_ASSERT_EQUAL_HEX32(0x53435552u,
                            ((uint32_t)rec.magic[0] << 24) |
                            ((uint32_t)rec.magic[1] << 16) |
                            ((uint32_t)rec.magic[2] << 8)  |
                             (uint32_t)rec.magic[3]);
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, g_crit.magic);
    TEST_ASSERT_EQUAL_UINT(42u, g_crit.counter);
}

void test_refresh_repairs_corrupted_live_ram(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    /* Simulate a single-event upset in live RAM. */
    g_crit.counter = 0xDEADBEEFu;

    TEST_ASSERT_EQUAL(SCRUB_REPAIRED, scrub_refresh(SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL_UINT(42u, g_crit.counter);
    TEST_ASSERT_EQUAL_UINT(1u, scrub_repair_count());
}

void test_refresh_no_change_when_healthy(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    TEST_ASSERT_EQUAL(SCRUB_NO_CHANGE, scrub_refresh(SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL_UINT(0u, scrub_repair_count());
}

void test_tick_repairs_only_corrupted_regions(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    g_crit.flags = 0x0000u;   /* corrupt one field */

    TEST_ASSERT_EQUAL_UINT(1u, scrub_tick());
    TEST_ASSERT_EQUAL_UINT(0x00FFu, g_crit.flags);
}

void test_init_restores_from_fram_on_boot(void)
{
    /* First register+sync to lay down a golden copy. */
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    /* Now pretend the live struct was corrupted before init ran. */
    g_crit.counter = 0u;
    g_crit.flags   = 0u;

    TEST_ASSERT_EQUAL(SCRUB_OK, scrub_init());
    TEST_ASSERT_EQUAL_UINT(42u, g_crit.counter);
    TEST_ASSERT_EQUAL_UINT(0x00FFu, g_crit.flags);
}

void test_refresh_rejects_corrupt_fram_record(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    /* Flip a bit in the stored golden payload (FRAM bit upset). */
    uint32_t slot = SCRUB_SLOT_ADDR(SCRUB_REGION_OBSW_STATE);
    g_fram[slot + 20] ^= 0x01u;         /* payload region */

    g_crit.counter = 0xDEADBEEFu;       /* also corrupt live RAM */

    /* The scrubber must NOT restore wrong data: it rejects the golden copy. */
    TEST_ASSERT_EQUAL(SCRUB_ERR_CRC, scrub_refresh(SCRUB_REGION_OBSW_STATE));
    /* Live RAM must remain untouched (not silently overwritten with garbage). */
    TEST_ASSERT_EQUAL_UINT(0xDEADBEEFu, g_crit.counter);
    TEST_ASSERT_EQUAL_UINT(0u, scrub_repair_count());
}

void test_double_bit_live_corruption_still_repaired_from_good_fram(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    scrub_sync(SCRUB_REGION_OBSW_STATE);

    /* Two flipped bits in live RAM (SRAM2 parity would miss this; scrub fixes it). */
    g_crit.magic   ^= 0x01000000u;
    g_crit.counter ^= 0x0000FFFFu;

    TEST_ASSERT_EQUAL(SCRUB_REPAIRED, scrub_refresh(SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, g_crit.magic);
    TEST_ASSERT_EQUAL_UINT(42u, g_crit.counter);
}

void test_fram_transport_failure_is_reported(void)
{
    scrub_register(&g_crit, sizeof(g_crit), SCRUB_REGION_OBSW_STATE);
    g_read_rc = -1;
    TEST_ASSERT_EQUAL(SCRUB_ERR_FRAM, scrub_refresh(SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL_UINT(1u, scrub_fram_error_count());

    g_read_rc = 0;
    g_write_rc = -1;
    TEST_ASSERT_EQUAL(SCRUB_ERR_FRAM, scrub_sync(SCRUB_REGION_OBSW_STATE));
}

void test_register_invalid_args(void)
{
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID,
                      scrub_register(NULL, 10u, SCRUB_REGION_OBSW_STATE));
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID,
                      scrub_register(&g_crit, 0u, SCRUB_REGION_OBSW_STATE));
    /* Region too large for the fixed slot. */
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID,
                      scrub_register(&g_crit, SCRUB_MAX_REGION_SIZE + 1u, 0u));
    /* Region id out of range. */
    TEST_ASSERT_EQUAL(SCRUB_ERR_INVALID,
                      scrub_register(&g_crit, 4u, SCRUB_MAX_REGIONS));
}
