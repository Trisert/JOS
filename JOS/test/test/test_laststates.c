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
#include "main.h"        /* fakes/main.h: HAL prototypes exercised directly */
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

/* Wrapping past the last slot must erase the pool first. Without the erase the
 * program would fail on a non-erased row (PROGERR), silently losing the
 * transition record -- this is the regression this test pins down. */
void test_laststates_wrap_erases_pool_before_reuse(void)
{
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
    TEST_ASSERT_EQUAL_UINT32((uint32_t)LASTSTATES_MAX_ENTRIES, laststates_count());
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
                                            HOST_FLASH_LASTSTATES_BASE,
                                            0x0123456789ABCDEFULL));

    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    TEST_ASSERT_EQUAL_INT(HAL_OK,
                          HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                            HOST_FLASH_LASTSTATES_BASE,
                                            0x0123456789ABCDEFULL));
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void test_hal_flash_erase_requires_unlock(void)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t               page_error = 0u;
    const uint32_t         first_page =
        (uint32_t)((HOST_FLASH_LASTSTATES_BASE - 0x08000000UL) / HOST_FLASH_PAGE_SIZE);

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
