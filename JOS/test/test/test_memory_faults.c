/* ---------------------------------------------------------------------------
 * test_memory_faults.c - error and recovery paths of App/memory/memory.c.
 *
 * test_laststates.c covers the nominal ring behaviour (append, wrap, dump,
 * FRAM round trips). This file covers what happens when something goes
 * wrong, which is the half that matters in flight:
 *
 *   - an I2C transfer the FM24VN10-G cannot serve (a chip-boundary crossing)
 *     must be reported, not silently truncated;
 *   - the 64 KB FRAM cyclic buffer must wrap correctly, splitting the record
 *     across the end of the bank;
 *   - a reboot must rebuild the ring cursor and the entry count from Flash;
 *   - a second writer of the same pool (Core/Src/dual_bank.c appends 'DBNK'
 *     markers outside laststates_write()) must not wedge the ring;
 *   - a corrupted (SEU) cursor must be clamped, not used as an offset;
 *   - a row that cannot be programmed must fail the write, leave the Flash
 *     controller locked and leave the ring bookkeeping untouched.
 *
 * Everything is exercised against support/host_flash.c, which reproduces NOR
 * semantics (erased == 0xFF, one program per erase cycle, unlock gating) and
 * the FM24VN10-G addressing rules.
 *
 * Refs: ECSS-E-ST-40C 5.5, ECSS-Q-ST-80C 6.3.5, NASA-STD-8739.8, RM0351 3.3.7.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "memory.h"
#include "obsw_types.h"
#include "main.h"             /* fakes/main.h: HAL entry points used directly */
#include "seu_mitigation.h"   /* fakes/: lock/commit contract of W2-5 */
#include "host_support.h"

#include <stdint.h>
#include <string.h>

/* Geometry of the FRAM bank, mirrored from memory.c (4 x FM24VN10-G). */
#define FRAM_CHIP_SIZE   (16u * 1024u)
#define FRAM_TOTAL_SIZE  (4u * FRAM_CHIP_SIZE)

static laststates_entry_t make_entry(uint32_t timestamp, uint8_t from, uint8_t to,
                                     uint8_t trigger, uint8_t fill)
{
    laststates_entry_t e;

    memset(&e, 0, sizeof(e));
    e.timestamp  = timestamp;
    e.state_from = from;
    e.state_to   = to;
    e.trigger    = trigger;
    memset(e.context, fill, sizeof(e.context));
    return e;
}

static laststates_mirror_t *mirror(void)
{
    size_t len = 0u;
    void  *m   = laststates_mirror_region(&len);

    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_size_t(sizeof(laststates_mirror_t), len);
    return (laststates_mirror_t *)m;
}

/* Program one double word straight through the HAL doubles, i.e. WITHOUT
 * going through laststates_write(). This is what a second writer of the pool
 * (Core/Src/dual_bank.c) or a half-completed write interrupted by a reset
 * leaves behind. */
static void program_dword(uint32_t byte_offset, uint64_t value)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    TEST_ASSERT_EQUAL_INT(HAL_OK,
                          HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                            (uint32_t)(flash_base + byte_offset),
                                            value));
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void setUp(void)
{
    host_flash_reset();
    seu_stub_reset();
    laststates_init();
}

void tearDown(void)
{
}

/* =====================================================================
 * FRAM error propagation
 * ===================================================================== */

/* Each FM24VN10-G is its own 16 KB address space: the device does not roll
 * over into the next chip, so a transfer that starts in chip 0 and runs past
 * its last byte is rejected by the part. The driver must return the failure
 * instead of reporting a partial write as success - a silently truncated
 * write is how a telemetry record ends up half-written in FRAM.
 *
 * The chip that was addressed is asserted too, so the failure is the
 * boundary crossing and not a mis-computed device address. */
void test_fram_write_reports_a_transfer_crossing_a_chip_boundary(void)
{
    const uint8_t payload[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };

    fram_init();

    TEST_ASSERT_EQUAL_INT(-1, fram_write(FRAM_CHIP_SIZE - 4u, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_HEX16(0xA0u, host_flash_last_i2c_addr());  /* chip 0, shifted */
}

void test_fram_read_reports_a_transfer_crossing_a_chip_boundary(void)
{
    uint8_t buf[8];

    fram_init();
    memset(buf, 0xC3, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(-1, fram_read(FRAM_CHIP_SIZE - 4u, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX16(0xA0u, host_flash_last_i2c_addr());
    TEST_ASSERT_EQUAL_HEX8(0xC3u, buf[0]);   /* nothing was handed back */
}

/* =====================================================================
 * Cyclic buffer wrap-around
 * ===================================================================== */

/* The 64 KB FRAM bank is a ring: a record that does not fit in the tail is
 * split, the remainder goes to offset 0 and the head follows it. Losing the
 * split (or wrapping the head without writing the remainder) silently drops
 * the oldest half of every record written at the end of the bank. */
void test_cyclic_buffer_write_wraps_and_splits_the_record(void)
{
    static uint8_t chunk[4096];
    const uint8_t  record[16] = {
        0xA0u, 0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA6u, 0xA7u,
        0xB0u, 0xB1u, 0xB2u, 0xB3u, 0xB4u, 0xB5u, 0xB6u, 0xB7u,
    };
    uint8_t  tail[8];
    uint8_t  head[8];
    uint32_t i;

    memset(chunk, 0x5Au, sizeof(chunk));

    fram_init();
    cyclic_buffer_init();

    /* Fill the bank up to 8 bytes short of the end. */
    for (i = 0u; i < 15u; i++) {
        TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(chunk, sizeof(chunk)));
    }
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(chunk, sizeof(chunk) - 8u));
    TEST_ASSERT_EQUAL_UINT32(FRAM_TOTAL_SIZE - 8u, cyclic_buffer_head());

    /* 16 B into an 8 B tail: 8 bytes at the end, 8 bytes back at offset 0. */
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(record, sizeof(record)));
    TEST_ASSERT_EQUAL_UINT32(8u, cyclic_buffer_head());

    memset(tail, 0, sizeof(tail));
    memset(head, 0, sizeof(head));
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_read(FRAM_TOTAL_SIZE - 8u, tail, sizeof(tail)));
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_read(0u, head, sizeof(head)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(record, tail, sizeof(tail));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(record + 8, head, sizeof(head));
}

/* A read that would run past the end of the bank is refused rather than
 * wrapped: the caller asked for a contiguous range that does not exist. */
void test_cyclic_buffer_read_rejects_a_range_past_the_end(void)
{
    uint8_t buf[8];

    fram_init();
    cyclic_buffer_init();

    TEST_ASSERT_EQUAL_INT(-1, cyclic_buffer_read(FRAM_TOTAL_SIZE - 4u, buf, sizeof(buf)));
}

/* =====================================================================
 * LastStates: recovery after a reboot
 * ===================================================================== */

/* laststates_init() runs on every boot and must re-derive the cursor and the
 * count from Flash. If it restarted at slot 0 the next write would fail on a
 * non-erased row (PROGERR) and the forensic trail would be dead exactly when
 * it is needed - after the reset that is being investigated. */
void test_laststates_init_recovers_cursor_and_count_after_reboot(void)
{
    laststates_entry_t appended = make_entry(99u, STATE_READY, STATE_ACTIVE,
                                             TRIGGER_TASK_COMPLETE, 0x99u);
    uint32_t i;

    for (i = 0u; i < 3u; i++) {
        laststates_entry_t e = make_entry(i, STATE_INIT, STATE_READY,
                                          TRIGGER_BOOT, (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }

    laststates_init();                     /* the reboot */

    TEST_ASSERT_EQUAL_UINT32(3u, mirror()->idx);
    TEST_ASSERT_EQUAL_UINT32(3u, mirror()->count);
    TEST_ASSERT_EQUAL_HEX32(LASTSTATES_MIRROR_MAGIC, mirror()->magic);
    TEST_ASSERT_EQUAL_UINT32(3u, laststates_count());

    /* The next record appends after the recovered trail, not over it. */
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&appended));
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&appended,
                                  host_flash_pool() + 3u * LASTSTATES_ENTRY_SIZE,
                                  LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT32(4u, laststates_count());
}

/* The pool has a second writer: Core/Src/dual_bank.c appends boot-fault /
 * boot-OK markers ('DBNK') outside laststates_write(). When one of those
 * landed in the slot the cached cursor points at, the cursor must be
 * re-derived from Flash; programming the occupied slot would fail and, since
 * the cursor never advances, every later write would fail too. */
void test_laststates_write_resyncs_when_a_second_writer_took_the_slot(void)
{
    laststates_entry_t entry = make_entry(0x1234u, STATE_READY, STATE_CRIT,
                                          TRIGGER_BOOT_FAULT, 0x42u);

    /* 'DBNK' marker dropped into slot 0 behind the module's back. */
    program_dword(0u, 0x4B4E42440000000EULL);
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);   /* cursor is now stale */

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));

    /* Re-derived to the first free slot, and the marker is intact. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry,
                                  host_flash_pool() + LASTSTATES_ENTRY_SIZE,
                                  LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT32(2u, mirror()->idx);
    TEST_ASSERT_EQUAL_UINT32(2u, laststates_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());   /* no page recycled */
}

/* The write cursor is SEU-scrubbed bookkeeping, but the scrubber only votes
 * between passes: a flip that lands just before a write must not be used as a
 * slot index (it would compute an address outside the pool). The module
 * clamps it back into the ring instead. */
void test_laststates_write_clamps_an_out_of_range_cursor(void)
{
    laststates_entry_t entry = make_entry(0x5150u, STATE_ACTIVE, STATE_CRIT,
                                          TRIGGER_SEU_SCRUB, 0x5Au);

    mirror()->idx = (uint32_t)LASTSTATES_MAX_ENTRIES + 7u;   /* bit flip */

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));

    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry,
                                  host_flash_pool(), LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT32(1u, mirror()->idx);
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());
}

/* A slot whose first double word still reads erased while a later row is
 * already programmed (a write interrupted by a reset, or an SEU) cannot be
 * completed: the second row raises PROGERR. The write must report the
 * failure, re-lock the Flash controller and leave the ring bookkeeping
 * untouched - advancing the cursor over a half-written record would hide it
 * from the post-mortem dump and skip a slot forever. */
void test_laststates_write_fails_cleanly_on_a_partially_programmed_slot(void)
{
    laststates_entry_t entry = make_entry(0x7777u, STATE_READY, STATE_CRIT,
                                          TRIGGER_FAULT, 0x77u);
    int commits_before;

    /* Row 8 of slot 0 is already programmed; row 0 still reads erased, so the
     * module sees a free slot and starts writing into it. */
    program_dword(64u, 0x0011223344556677ULL);
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);

    commits_before = seu_stub_commit_count();

    TEST_ASSERT_EQUAL_INT(-1, laststates_write(&entry));

    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_UINT32(host_flash_unlock_count(), host_flash_lock_count());
    TEST_ASSERT_EQUAL_INT(commits_before, seu_stub_commit_count());  /* no advance */
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);
}
