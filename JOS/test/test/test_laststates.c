/* ---------------------------------------------------------------------------
 * test_laststates.c - unit tests for the LastStates pool in App/memory/memory.c
 *
 * The pool is 64 x 128 B of internal Flash at 0x08080000 that records every
 * state-machine transition so a post-anomaly downlink can reconstruct what
 * the spacecraft did before it reset. The module is exercised unmodified
 * against support/host_flash.c, which maps writable pages at that exact
 * address and reproduces NOR-Flash semantics (erased == 0xFF, one program per
 * erase cycle, unlock required).
 *
 * Refs: ECSS-E-ST-40C 5.5, NASA-STD-8739.8 (non-volatile state retention),
 *       RM0351 3.3.7.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "memory.h"
#include "obsw_types.h"
#include "main.h"             /* fakes/main.h: HAL prototypes exercised directly */
#include "seu_mitigation.h"   /* fakes/: lock/commit contract of W2-5 */
#include "host_support.h"

#include <stdint.h>
#include <string.h>

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

void setUp(void)
{
    host_flash_reset();
    seu_stub_reset();
    laststates_init();
}

void tearDown(void)
{
}

/* The on-Flash layout is part of the ICD: 128 B per entry, 64 entries. */
void test_laststates_entry_layout_is_128_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT32(128u, (uint32_t)sizeof(laststates_entry_t));
    TEST_ASSERT_EQUAL_UINT32(128u, (uint32_t)LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_size_t((size_t)LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE,
                             host_flash_pool_size());
}

void test_laststates_init_starts_empty(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());
}

/* Core round trip: what laststates_write() programmed into Flash is exactly
 * what laststates_dump_all() reads back through the absolute pool address. */
void test_laststates_write_then_dump_round_trip(void)
{
    laststates_entry_t entry = make_entry(0x11223344u, STATE_INIT, STATE_READY,
                                          TRIGGER_BOOT, 0xA5u);
    uint8_t out[LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(out);      /* in: capacity, out: bytes written */

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)LASTSTATES_ENTRY_SIZE, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry, out, LASTSTATES_ENTRY_SIZE);
}

/* The bytes must physically land at 0x08080000, not in some shadow buffer. */
void test_laststates_write_lands_at_flash_base_address(void)
{
    laststates_entry_t entry = make_entry(0xCAFEBABEu, STATE_READY, STATE_ACTIVE,
                                          TRIGGER_GROUND_CMD, 0x5Au);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry,
                                  host_flash_pool(), LASTSTATES_ENTRY_SIZE);
}

/* Consecutive entries occupy consecutive 128 B slots, in order. */
void test_laststates_writes_are_sequential(void)
{
    laststates_entry_t a = make_entry(1u, STATE_OFF,  STATE_INIT,  TRIGGER_BOOT, 0x01u);
    laststates_entry_t b = make_entry(2u, STATE_INIT, STATE_READY, TRIGGER_ANTENNA_DONE, 0x02u);
    uint8_t out[2 * LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(out);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&a));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&b));
    TEST_ASSERT_EQUAL_UINT32(2u, laststates_count());

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)(2 * LASTSTATES_ENTRY_SIZE), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&a, out, LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&b,
                                  out + LASTSTATES_ENTRY_SIZE, LASTSTATES_ENTRY_SIZE);
}

void test_laststates_write_rejects_null_entry(void)
{
    TEST_ASSERT_EQUAL_INT(-1, laststates_write(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());
}

void test_laststates_dump_all_rejects_null_arguments(void)
{
    uint8_t out[LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(out);

    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(NULL, &len));
    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(out, NULL));
}

/* *len is the capacity of the caller's buffer on entry. A buffer that cannot
 * hold the whole pool must be refused, not overrun: with a full pool the copy
 * would be 8 KB, which on the downlink path is a caller stack frame. The
 * required size is reported back so the caller can retry with a real buffer.
 *
 * The canary bytes around the buffer pin "nothing was written" rather than
 * just "an error was returned". */
void test_laststates_dump_all_refuses_undersized_buffer(void)
{
    struct {
        uint8_t guard_lo[16];
        uint8_t out[2 * LASTSTATES_ENTRY_SIZE];
        uint8_t guard_hi[16];
    } framed;

    size_t   len;
    uint32_t i;

    memset(&framed, 0x00, sizeof(framed));

    for (i = 0u; i < (uint32_t)LASTSTATES_MAX_ENTRIES; i++) {
        laststates_entry_t e = make_entry(i, STATE_READY, STATE_ACTIVE,
                                          TRIGGER_TASK_COMPLETE, (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }

    len = sizeof(framed.out);       /* 256 B for an 8 KB pool */
    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(framed.out, &len));

    /* Required size reported back ... */
    TEST_ASSERT_EQUAL_size_t((size_t)LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE, len);

    /* ... and not one byte was copied anywhere. */
    for (i = 0u; i < (uint32_t)sizeof(framed.out); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00u, framed.out[i]);
    }
    for (i = 0u; i < (uint32_t)sizeof(framed.guard_lo); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00u, framed.guard_lo[i]);
        TEST_ASSERT_EQUAL_HEX8(0x00u, framed.guard_hi[i]);
    }
}

/* An exactly-sized buffer is accepted and *len comes back as the byte count. */
void test_laststates_dump_all_accepts_exactly_sized_buffer(void)
{
    uint8_t out[2 * LASTSTATES_ENTRY_SIZE];
    size_t  len;

    laststates_entry_t a = make_entry(11u, STATE_OFF,  STATE_INIT,  TRIGGER_BOOT, 0x11u);
    laststates_entry_t b = make_entry(22u, STATE_INIT, STATE_READY, TRIGGER_BOOT, 0x22u);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&a));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&b));

    len = sizeof(out);
    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)(2 * LASTSTATES_ENTRY_SIZE), len);
}

/* Boundary of the capacity check: one byte short of the required size must
 * still be refused with nothing copied. An off-by-one here (>= instead of >)
 * is exactly the overrun the size parameter was introduced to prevent, and a
 * test that only ever passes an oversized buffer cannot see it. */
void test_laststates_dump_all_refuses_a_buffer_one_byte_short(void)
{
    uint8_t out[3 * LASTSTATES_ENTRY_SIZE];
    size_t  len;
    uint32_t i;

    laststates_entry_t a = make_entry(1u, STATE_OFF,   STATE_INIT,  TRIGGER_BOOT, 0xA1u);
    laststates_entry_t b = make_entry(2u, STATE_INIT,  STATE_READY, TRIGGER_BOOT, 0xB2u);
    laststates_entry_t c = make_entry(3u, STATE_READY, STATE_ACTIVE, TRIGGER_BOOT, 0xC3u);

    memset(out, 0x00, sizeof(out));

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&a));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&b));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&c));

    len = sizeof(out) - 1u;                       /* one byte short */
    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)(3 * LASTSTATES_ENTRY_SIZE), len);
    for (i = 0u; i < (uint32_t)sizeof(out); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00u, out[i]);    /* nothing copied */
    }

    len = sizeof(out);                            /* exactly enough */
    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)(3 * LASTSTATES_ENTRY_SIZE), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&c,
                                  out + (2 * LASTSTATES_ENTRY_SIZE),
                                  LASTSTATES_ENTRY_SIZE);
}

/* An empty pool needs no space at all, so even a zero-capacity buffer is a
 * legal (and side-effect free) request. */
void test_laststates_dump_all_on_empty_pool_writes_nothing(void)
{
    uint8_t out[LASTSTATES_ENTRY_SIZE];
    size_t  len = 0u;

    memset(out, 0xC3, sizeof(out));

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_HEX8(0xC3u, out[0]);
}

/* Flash must be unlocked to program and re-locked afterwards; leaving the
 * Flash controller unlocked is a latent corruption hazard under SEU. */
void test_laststates_write_leaves_flash_locked(void)
{
    laststates_entry_t entry = make_entry(7u, STATE_READY, STATE_CRIT,
                                          TRIGGER_BATTERY_LOW, 0x77u);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));
    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_UINT32(host_flash_unlock_count(), host_flash_lock_count());
    /* 128 B written as 16 double-words. */
    TEST_ASSERT_EQUAL_UINT32(16u, host_flash_program_count());
}

/* Filling the pool must not erase anything: all 64 slots are still virgin. */
void test_laststates_fills_pool_without_erasing(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)LASTSTATES_MAX_ENTRIES; i++) {
        laststates_entry_t e = make_entry(i, STATE_READY, STATE_ACTIVE,
                                          TRIGGER_TASK_COMPLETE, (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }

    TEST_ASSERT_EQUAL_UINT32((uint32_t)LASTSTATES_MAX_ENTRIES, laststates_count());
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());
}

/* Wrapping past the last slot must erase before reusing slot 0. Without the
 * erase the program would fail on a non-erased row (PROGERR), silently losing
 * the transition record -- this is the regression this test pins down.
 *
 * Only the ONE page the slot belongs to is recycled (16 slots of 128 B in a
 * 2 KB page), never the whole pool: erasing the pool would throw away the 48
 * newer records the post-mortem downlink exists to recover. */
void test_laststates_wrap_erases_pool_before_reuse(void)
{
    const uint32_t slots_per_page = HOST_FLASH_PAGE_SIZE / LASTSTATES_ENTRY_SIZE;
    laststates_entry_t wrapped;
    uint32_t i;

    for (i = 0u; i < (uint32_t)LASTSTATES_MAX_ENTRIES; i++) {
        laststates_entry_t e = make_entry(i, STATE_READY, STATE_ACTIVE,
                                          TRIGGER_TASK_COMPLETE, (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());

    wrapped = make_entry(0xDEADBEEFu, STATE_ACTIVE, STATE_CRIT,
                         TRIGGER_CRIT_EVENT, 0xEEu);
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&wrapped));

    TEST_ASSERT_EQUAL_UINT32(1u, host_flash_erase_count());
    /* One page recycled (16 slots gone) and slot 0 immediately rewritten. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)LASTSTATES_MAX_ENTRIES - slots_per_page + 1u,
                             laststates_count());
    /* The wrapped entry is back at slot 0 ... */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&wrapped,
                                  host_flash_pool(), LASTSTATES_ENTRY_SIZE);
    /* ... and slot 1 was erased by the page erase. */
    TEST_ASSERT_EQUAL_HEX8(0xFFu, host_flash_pool()[LASTSTATES_ENTRY_SIZE]);
}

/* =====================================================================
 * FRAM / cyclic buffer (same HAL doubles, I2C side)
 * ===================================================================== */

void test_fram_write_read_round_trip(void)
{
    const uint8_t payload[8] = { 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x01u, 0x02u, 0x03u, 0x04u };
    uint8_t       readback[8];

    fram_init();
    TEST_ASSERT_EQUAL_INT(0, fram_write(0x100u, payload, sizeof(payload)));

    memset(readback, 0, sizeof(readback));
    TEST_ASSERT_EQUAL_INT(0, fram_read(0x100u, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, readback, sizeof(payload));
}

/* Access beyond the 64 KB FRAM bank must be refused, not wrapped silently. */
void test_fram_rejects_out_of_range_access(void)
{
    uint8_t buf[4] = { 0 };

    TEST_ASSERT_EQUAL_INT(-1, fram_write(64u * 1024u, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, fram_read(64u * 1024u, buf, sizeof(buf)));
}

/* The STM32 HAL I2C API takes the device address already shifted left by one.
 * The four FM24VN10-G parts are 7-bit 0x50..0x53, so the bytes that must reach
 * HAL_I2C_Mem_Read/Write are 0xA0, 0xA2, 0xA4, 0xA6. Handing the HAL the raw
 * 7-bit value would address 0x28 on the real bus, and no host double is
 * allowed to paper over that. */
void test_fram_uses_shifted_i2c_device_addresses(void)
{
    const uint16_t expected[4] = { 0xA0u, 0xA2u, 0xA4u, 0xA6u };
    uint8_t        byte        = 0x5Au;
    uint32_t       chip;

    fram_init();

    for (chip = 0u; chip < 4u; chip++) {
        uint32_t addr = chip * 16u * 1024u;

        TEST_ASSERT_EQUAL_INT(0, fram_write(addr, &byte, 1u));
        TEST_ASSERT_EQUAL_HEX16(expected[chip], host_flash_last_i2c_addr());

        TEST_ASSERT_EQUAL_INT(0, fram_read(addr, &byte, 1u));
        TEST_ASSERT_EQUAL_HEX16(expected[chip], host_flash_last_i2c_addr());
    }
}

/* Each chip is a separate 16 KB address space: byte 0 of chip 1 must not alias
 * byte 0 of chip 0. This only holds if the chip-select arithmetic and the
 * address shift agree. */
void test_fram_chips_do_not_alias_each_other(void)
{
    const uint8_t marker0 = 0x11u;
    const uint8_t marker1 = 0x22u;
    uint8_t       readback = 0u;

    fram_init();

    TEST_ASSERT_EQUAL_INT(0, fram_write(0u, &marker0, 1u));
    TEST_ASSERT_EQUAL_INT(0, fram_write(16u * 1024u, &marker1, 1u));

    TEST_ASSERT_EQUAL_INT(0, fram_read(0u, &readback, 1u));
    TEST_ASSERT_EQUAL_HEX8(marker0, readback);

    TEST_ASSERT_EQUAL_INT(0, fram_read(16u * 1024u, &readback, 1u));
    TEST_ASSERT_EQUAL_HEX8(marker1, readback);
}

/* =====================================================================
 * Flash controller preconditions (RM0351 3.3.5: PG and PER both need the
 * KEY1/KEY2 unlock). The doubles enforce them symmetrically, so a module that
 * forgets HAL_FLASH_Unlock() around either operation fails here.
 * ===================================================================== */

void test_hal_flash_program_requires_unlock(void)
{
    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_INT(HAL_ERROR,
                          HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                            (uint32_t)flash_base,
                                            0x0123456789ABCDEFULL));

    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    TEST_ASSERT_EQUAL_INT(HAL_OK,
                          HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                            (uint32_t)flash_base,
                                            0x0123456789ABCDEFULL));
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void test_hal_flash_erase_requires_unlock(void)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t               page_error = 0u;
    const uint32_t         first_page =
        (uint32_t)((flash_base - 0x08000000UL) / HOST_FLASH_PAGE_SIZE);

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks     = FLASH_BANK_1;
    erase_init.Page      = first_page;
    erase_init.NbPages   = 1u;

    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_INT(HAL_ERROR, HAL_FLASHEx_Erase(&erase_init, &page_error));
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());

    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASHEx_Erase(&erase_init, &page_error));
    TEST_ASSERT_EQUAL_UINT32(1u, host_flash_erase_count());
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void test_cyclic_buffer_write_advances_head_and_reads_back(void)
{
    const uint8_t record[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t       readback[16];

    fram_init();
    cyclic_buffer_init();
    TEST_ASSERT_EQUAL_UINT32(0u, cyclic_buffer_head());

    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(record, sizeof(record)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(record), cyclic_buffer_head());

    memset(readback, 0, sizeof(readback));
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_read(0u, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(record, readback, sizeof(record));
}

void test_cyclic_buffer_zero_length_write_is_a_no_op(void)
{
    const uint8_t dummy = 0xAAu;

    fram_init();
    cyclic_buffer_init();

    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(&dummy, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cyclic_buffer_head());
}

/* ---------------------------------------------------------------------------
 * SEU ownership contract (W2-5)
 *
 * Core/Inc/seu_mitigation.h requires every legitimate update of a
 * SEU_POLICY_GOLDEN region to happen inside seu_mitigation_lock()/unlock()
 * and to be followed by seu_mitigation_commit(); a missing commit makes the
 * scrubber revert the update at the next pass (i.e. it would silently rewind
 * the LastStates write cursor). support/seu_stubs.c counts the calls so the
 * contract is asserted here rather than assumed.
 * ------------------------------------------------------------------------- */
void test_laststates_updates_commit_the_seu_mirror_under_lock(void)
{
    laststates_entry_t entry = make_entry(0xDEADBEEFu, STATE_READY, STATE_CRIT,
                                          TRIGGER_FAULT, 0x77u);

    /* setUp() ran laststates_init(), which resyncs the mirror from Flash. */
    TEST_ASSERT_EQUAL_INT(1, seu_stub_commit_count());
    TEST_ASSERT_EQUAL_INT((int)SEU_REGION_LASTSTATES, seu_stub_last_commit_region());
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());   /* balanced lock/unlock */

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));

    TEST_ASSERT_EQUAL_INT(2, seu_stub_commit_count());
    TEST_ASSERT_EQUAL_INT((int)SEU_REGION_LASTSTATES, seu_stub_last_commit_region());
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());
}

/* The region handed to seu_mitigation_register_region() must be the live
 * bookkeeping itself: right size, right magic, and a cursor/count that track
 * the writes. A stale or bogus region would make the scrubber vote on the
 * wrong bytes. */
void test_laststates_mirror_region_tracks_the_ring_bookkeeping(void)
{
    laststates_entry_t entry = make_entry(7u, STATE_INIT, STATE_READY,
                                          TRIGGER_BOOT, 0x11u);
    size_t len = 0u;
    const laststates_mirror_t *m = laststates_mirror_region(&len);

    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_size_t(sizeof(laststates_mirror_t), len);
    TEST_ASSERT_EQUAL_HEX32(LASTSTATES_MIRROR_MAGIC, m->magic);
    TEST_ASSERT_EQUAL_UINT32(0u, m->idx);
    TEST_ASSERT_EQUAL_UINT32(0u, m->count);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));
    TEST_ASSERT_EQUAL_UINT32(1u, m->idx);      /* cursor advanced by one slot */
    TEST_ASSERT_EQUAL_UINT32(laststates_count(), m->count);
}

/* =====================================================================
 * Torn (partially programmed) records - Kilo review of PR #9, finding C4
 *
 * A 128 B record is programmed as 16 separate double words. A reset (or a
 * bounded-wait timeout) between them leaves a slot whose first double word is
 * programmed and whose tail is still erased. Testing only double word 0 - the
 * previous implementation - reported such a half-record as a perfectly valid
 * entry, so ground would reconstruct the anomaly timeline from a record whose
 * payload is 0xFF filler.
 * ===================================================================== */

/* Program one double word straight through the HAL doubles, bypassing
 * laststates_write(), to build the torn-slot fixture. */
static void program_raw_dword(uint32_t byte_offset, uint64_t value)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    TEST_ASSERT_EQUAL_INT(HAL_OK,
                          HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                            (uint32_t)flash_base + byte_offset,
                                            value));
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void test_laststates_torn_record_is_not_reported_as_valid(void)
{
    uint8_t out[LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(out);

    program_raw_dword(0u, 0x0000000102030405ULL);   /* only dword 0 of slot 0 */
    laststates_init();                              /* rescan after the reset */

    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());

    memset(out, 0xC3, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_HEX8(0xC3u, out[0]);          /* nothing copied out */
}

/* Only the LAST double word missing is still a torn record: the completeness
 * rule is "the tail is programmed", not "something is programmed". */
void test_laststates_record_missing_only_its_tail_is_skipped(void)
{
    uint32_t d;

    for (d = 0u; d < (LASTSTATES_ENTRY_SIZE / 8u) - 1u; d++) {
        program_raw_dword(d * 8u, 0x1122334455667788ULL);
    }
    laststates_init();

    TEST_ASSERT_EQUAL_UINT32(0u, laststates_count());
}

/* The torn slot must not be handed back to the programmer either: re-writing
 * a non-erased double word fails with PROGERR on the real part and would kill
 * every later write. The cursor skips it and the next record lands in slot 1,
 * with no page erase (the newer records must survive). */
void test_laststates_write_skips_a_torn_slot(void)
{
    laststates_entry_t e = make_entry(0x55u, STATE_READY, STATE_ACTIVE,
                                      TRIGGER_BOOT, 0x5Au);

    program_raw_dword(0u, 0x0000000102030405ULL);
    laststates_init();

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));

    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&e,
                                  host_flash_pool() + LASTSTATES_ENTRY_SIZE,
                                  LASTSTATES_ENTRY_SIZE);
}

/* A complete record is still reported next to a torn one. */
void test_laststates_dump_returns_only_the_complete_record(void)
{
    laststates_entry_t e = make_entry(0x99u, STATE_INIT, STATE_READY,
                                      TRIGGER_BOOT, 0x42u);
    uint8_t out[2 * LASTSTATES_ENTRY_SIZE];
    size_t  len = sizeof(out);

    program_raw_dword(0u, 0x0000000102030405ULL);
    laststates_init();
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)LASTSTATES_ENTRY_SIZE, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&e, out, LASTSTATES_ENTRY_SIZE);
}

/* =====================================================================
 * Flash page-erase bounds guard - Kilo review of PR #9, finding C2
 *
 * The target path drives FLASH->CR directly, so an out-of-pool address would
 * erase 2 KB of whatever it points at: the vector table, the running image or
 * the dual-bank golden image. Only page-aligned addresses inside
 * [pool_base, pool_end) may be erased.
 * ===================================================================== */
void test_laststates_erase_guard_only_accepts_pool_pages(void)
{
    const uintptr_t base = flash_base;
    const uintptr_t end  = base + (uintptr_t)LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE;

    /* first and last page of the pool */
    TEST_ASSERT_TRUE(laststates_erase_addr_allowed(base));
    TEST_ASSERT_TRUE(laststates_erase_addr_allowed(end - HOST_FLASH_PAGE_SIZE));

    /* one page below the pool, and the first page past it */
    TEST_ASSERT_FALSE(laststates_erase_addr_allowed(base - HOST_FLASH_PAGE_SIZE));
    TEST_ASSERT_FALSE(laststates_erase_addr_allowed(end));

    /* inside the pool but not on a page boundary */
    TEST_ASSERT_FALSE(laststates_erase_addr_allowed(base + LASTSTATES_ENTRY_SIZE));

    /* the vector table, i.e. the worst case this guard exists for */
    TEST_ASSERT_FALSE(laststates_erase_addr_allowed((uintptr_t)0x08000000UL));
}
