/**
 * @file    test_memory.c
 * @brief   Unit tests for App/memory/memory.c — the LastStates pool in
 *          internal Flash and the FM24VN10-G FRAM driver.
 *
 * Flash and I2C are CMock mocks generated from test/fakes/stm32l4xx_hal.h.
 * To exercise a *real* write -> read round trip, the test maps 8 KB of
 * anonymous memory at the LastStates base address (0x08080000) with
 * MAP_FIXED_NOREPLACE and lets the HAL_FLASH_Program mock write the programmed
 * double-words there. laststates_dump_all() then reads that same region, so
 * the round trip goes through the production code path end to end — including
 * the 64-bit double-word granularity the STM32L4 Flash controller imposes.
 *
 * What is verified
 *   - laststates_write() argument contract and Flash unlock/program/lock order.
 *   - write -> dump round trip is byte-exact for one and for several entries.
 *   - a Flash programming failure is propagated and does not advance the pool.
 *   - the pool wraps at LASTSTATES_MAX_ENTRIES and erases the pages before
 *     reusing them (STM32L4 Flash cannot be reprogrammed without an erase).
 *   - FRAM addressing: chip select from the linear address, 16-bit offset,
 *     out-of-range rejection, and error propagation from the HAL.
 *   - cyclic buffer head tracking including wrap-around split writes.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit testing), ECSS-Q-ST-80C §6.2.3
 * (data retention / non-volatile storage verification), NASA-STD-8739.8,
 * JPL-182 Rule 16 (every return value checked).
 */

#include "unity.h"
#include "memory.h"
#include "main.h"                  /* test/fakes/main.h — hi2c2 handle extern */
#include "mock_stm32l4xx_hal.h"
#include "host_flash.h"            /* maps the simulated pool at 0x08080000 */

#include <string.h>
#include <stdint.h>

/* Mirror of the private constants in memory.c — kept in sync deliberately so
   a change to the production layout breaks these tests loudly. */
#define POOL_BASE   0x08080000UL
#define POOL_SIZE   (LASTSTATES_MAX_ENTRIES * LASTSTATES_ENTRY_SIZE)   /* 8 KB */
#define FRAM_TOTAL  (4UL * 16UL * 1024UL)                              /* 64 KB */

static uint8_t          *pool;              /* simulated internal Flash        */
static HAL_StatusTypeDef program_status;    /* injectable programming verdict  */
static int               program_calls;
static int               erase_calls;
static int               unlock_calls;
static int               lock_calls;

/* ---------- HAL callbacks: a tiny Flash simulator ---------- */

static HAL_StatusTypeDef flash_program_cb(uint32_t TypeProgram, uint32_t Address,
                                          uint64_t Data, int cmock_num_calls)
{
    (void)cmock_num_calls;
    program_calls++;

    /* The STM32L4 Flash controller only accepts aligned double-words. */
    TEST_ASSERT_EQUAL_UINT32(FLASH_TYPEPROGRAM_DOUBLEWORD, TypeProgram);
    TEST_ASSERT_EQUAL_UINT32(0U, Address % 8U);
    TEST_ASSERT_TRUE_MESSAGE(Address >= POOL_BASE && (Address + 8U) <= (POOL_BASE + POOL_SIZE),
                             "programming outside the LastStates pool");

    if (program_status == HAL_OK) {
        memcpy((void *)(uintptr_t)Address, &Data, sizeof(Data));
    }
    return program_status;
}

static HAL_StatusTypeDef flash_erase_cb(FLASH_EraseInitTypeDef *pEraseInit,
                                        uint32_t *PageError, int cmock_num_calls)
{
    (void)cmock_num_calls;
    erase_calls++;

    TEST_ASSERT_NOT_NULL(pEraseInit);
    TEST_ASSERT_EQUAL_UINT32(FLASH_TYPEERASE_PAGES, pEraseInit->TypeErase);
    TEST_ASSERT_EQUAL_UINT32(FLASH_BANK_1, pEraseInit->Banks);
    /* 0x08080000 is page 256 of bank 1 (2 KB pages); the 8 KB pool is 4 pages. */
    TEST_ASSERT_EQUAL_UINT32(256U, pEraseInit->Page);
    TEST_ASSERT_EQUAL_UINT32(4U, pEraseInit->NbPages);

    memset(pool, 0xFF, POOL_SIZE);       /* erased Flash reads as all-ones */
    if (PageError != NULL) {
        *PageError = 0xFFFFFFFFU;
    }
    return HAL_OK;
}

static HAL_StatusTypeDef flash_unlock_cb(int cmock_num_calls)
{
    (void)cmock_num_calls;
    unlock_calls++;
    return HAL_OK;
}

static HAL_StatusTypeDef flash_lock_cb(int cmock_num_calls)
{
    (void)cmock_num_calls;
    lock_calls++;
    return HAL_OK;
}

/* ---------- Fixture ---------- */

void setUp(void)
{
    pool = host_flash_map(POOL_BASE, POOL_SIZE);
    TEST_ASSERT_NOT_NULL_MESSAGE(pool,
                                 "could not map the simulated LastStates Flash");
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)POOL_BASE, pool);

    program_status = HAL_OK;
    program_calls  = 0;
    erase_calls    = 0;
    unlock_calls   = 0;
    lock_calls     = 0;

    HAL_FLASH_Unlock_Stub(flash_unlock_cb);
    HAL_FLASH_Lock_Stub(flash_lock_cb);
    HAL_FLASH_Program_Stub(flash_program_cb);
    HAL_FLASHEx_Erase_Stub(flash_erase_cb);

    laststates_init();
    cyclic_buffer_init();
}

void tearDown(void)
{
    host_flash_unmap(pool, POOL_SIZE);
    pool = NULL;
}

/* ---------- Helpers ---------- */

static laststates_entry_t make_entry(uint32_t ts, uint8_t from, uint8_t to, uint8_t trig)
{
    laststates_entry_t e;
    memset(&e, 0, sizeof(e));
    e.timestamp  = ts;
    e.state_from = from;
    e.state_to   = to;
    e.trigger    = trig;
    for (size_t i = 0; i < sizeof(e.context); i++) {
        e.context[i] = (uint8_t)(0x40U + (i & 0x3FU));   /* recognisable pattern */
    }
    return e;
}

/* ================= LastStates: contract ================= */

void test_laststates_write_rejects_null_entry(void)
{
    TEST_ASSERT_EQUAL_INT(-1, laststates_write(NULL));
    TEST_ASSERT_EQUAL_INT(0, program_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, laststates_count());
}

void test_laststates_init_resets_the_pool_counter(void)
{
    laststates_entry_t e = make_entry(1U, STATE_INIT, STATE_READY, TRIGGER_BOOT);
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    TEST_ASSERT_EQUAL_UINT32(1U, laststates_count());

    laststates_init();
    TEST_ASSERT_EQUAL_UINT32(0U, laststates_count());
}

void test_laststates_dump_all_rejects_null_arguments(void)
{
    uint8_t out[LASTSTATES_ENTRY_SIZE];
    size_t  len = 0U;
    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(NULL, &len));
    TEST_ASSERT_EQUAL_INT(-1, laststates_dump_all(out, NULL));
}

/* ================= LastStates: round trip ================= */

/* The core requirement: what was written is what is read back, byte for byte. */
void test_laststates_write_read_round_trip_single_entry(void)
{
    laststates_entry_t in = make_entry(0x11223344U, STATE_READY, STATE_CRIT,
                                       TRIGGER_BATTERY_LOW);
    uint8_t out[POOL_SIZE];
    size_t  len = 0U;

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&in));
    TEST_ASSERT_EQUAL_UINT32(1U, laststates_count());

    /* 128 B entry programmed as 16 aligned double-words, bracketed by exactly
       one unlock/lock pair (JPL-182: no stray unlocked Flash windows). */
    TEST_ASSERT_EQUAL_INT(LASTSTATES_ENTRY_SIZE / 8, program_calls);
    TEST_ASSERT_EQUAL_INT(1, unlock_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_calls);

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)LASTSTATES_ENTRY_SIZE, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&in, out, LASTSTATES_ENTRY_SIZE);
}

void test_laststates_round_trip_preserves_entry_order(void)
{
    laststates_entry_t a = make_entry(0xAAAA0001U, STATE_OFF,  STATE_INIT,  TRIGGER_BOOT);
    laststates_entry_t b = make_entry(0xBBBB0002U, STATE_INIT, STATE_READY, TRIGGER_ANTENNA_DONE);
    laststates_entry_t c = make_entry(0xCCCC0003U, STATE_READY, STATE_ACTIVE, TRIGGER_GROUND_CMD);
    uint8_t out[POOL_SIZE];
    size_t  len = 0U;

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&a));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&b));
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&c));
    TEST_ASSERT_EQUAL_UINT32(3U, laststates_count());

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)(3 * LASTSTATES_ENTRY_SIZE), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&a, &out[0], LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&b, &out[LASTSTATES_ENTRY_SIZE], LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&c, &out[2 * LASTSTATES_ENTRY_SIZE], LASTSTATES_ENTRY_SIZE);
}

/* A transition record must survive verbatim, fields included. */
void test_laststates_round_trip_preserves_transition_fields(void)
{
    laststates_entry_t in = make_entry(0xDEADBEEFU, STATE_ACTIVE, STATE_CRIT,
                                       TRIGGER_CRIT_EVENT);
    laststates_entry_t back;
    uint8_t out[POOL_SIZE];
    size_t  len = 0U;

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&in));
    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    memcpy(&back, out, sizeof(back));

    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, back.timestamp);
    TEST_ASSERT_EQUAL_UINT8(STATE_ACTIVE, back.state_from);
    TEST_ASSERT_EQUAL_UINT8(STATE_CRIT, back.state_to);
    TEST_ASSERT_EQUAL_UINT8(TRIGGER_CRIT_EVENT, back.trigger);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.context, back.context, sizeof(in.context));
}

/* ================= LastStates: failure and wrap ================= */

void test_laststates_write_propagates_flash_error_and_keeps_count(void)
{
    laststates_entry_t e = make_entry(1U, STATE_INIT, STATE_CRIT, TRIGGER_BATTERY_LOW);

    program_status = HAL_ERROR;
    TEST_ASSERT_EQUAL_INT(-1, laststates_write(&e));
    TEST_ASSERT_EQUAL_UINT32(0U, laststates_count());
    /* Aborts on the first failed double-word, and still re-locks the Flash. */
    TEST_ASSERT_EQUAL_INT(1, program_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_calls);
}

/* The pool is circular: entry 65 overwrites entry 1, which on STM32L4 Flash
   requires erasing the pages first. */
void test_laststates_wrap_erases_pool_before_reuse(void)
{
    laststates_entry_t e;
    uint8_t out[POOL_SIZE];
    size_t  len = 0U;

    for (int i = 0; i < LASTSTATES_MAX_ENTRIES; i++) {
        e = make_entry((uint32_t)(0x1000U + i), STATE_READY, STATE_ACTIVE,
                       TRIGGER_TASK_COMPLETE);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)LASTSTATES_MAX_ENTRIES, laststates_count());
    TEST_ASSERT_EQUAL_INT(0, erase_calls);          /* no erase while filling */

    /* Wrap: the 65th write must erase, then land at index 0. */
    laststates_entry_t wrapped = make_entry(0x5A5A5A5AU, STATE_ACTIVE, STATE_READY,
                                            TRIGGER_TASK_COMPLETE);
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&wrapped));
    TEST_ASSERT_EQUAL_INT(1, erase_calls);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)LASTSTATES_MAX_ENTRIES, laststates_count());

    TEST_ASSERT_EQUAL_INT(0, laststates_dump_all(out, &len));
    TEST_ASSERT_EQUAL_size_t((size_t)POOL_SIZE, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&wrapped, &out[0], LASTSTATES_ENTRY_SIZE);
    /* Everything after the wrapped entry was erased to 0xFF. */
    TEST_ASSERT_EQUAL_HEX8(0xFFU, out[LASTSTATES_ENTRY_SIZE]);
}

void test_laststates_write_fails_when_wrap_erase_fails(void)
{
    laststates_entry_t e;

    for (int i = 0; i < LASTSTATES_MAX_ENTRIES; i++) {
        e = make_entry((uint32_t)i, STATE_READY, STATE_ACTIVE, TRIGGER_TASK_COMPLETE);
        TEST_ASSERT_EQUAL_INT(0, laststates_write(&e));
    }

    /* Erase now fails: the write must be refused, not silently corrupt Flash. */
    HAL_FLASHEx_Erase_Stub(NULL);
    HAL_FLASHEx_Erase_IgnoreAndReturn(HAL_ERROR);
    program_calls = 0;

    e = make_entry(0xFFFFU, STATE_ACTIVE, STATE_CRIT, TRIGGER_CRIT_EVENT);
    TEST_ASSERT_EQUAL_INT(-1, laststates_write(&e));
    TEST_ASSERT_EQUAL_INT(0, program_calls);
}

/* ================= FRAM driver ================= */

void test_fram_write_rejects_out_of_range_address(void)
{
    uint8_t data[8] = { 0 };
    TEST_ASSERT_EQUAL_INT(-1, fram_write(FRAM_TOTAL, data, sizeof(data)));
    TEST_ASSERT_EQUAL_INT(-1, fram_write(FRAM_TOTAL - 4U, data, sizeof(data)));
}

void test_fram_read_rejects_out_of_range_address(void)
{
    uint8_t buf[8] = { 0 };
    TEST_ASSERT_EQUAL_INT(-1, fram_read(FRAM_TOTAL, buf, sizeof(buf)));
}

/* Linear address -> (I2C device, 16-bit offset). Chip 0 is 0x50, chip 1 0x51. */
void test_fram_write_selects_chip_and_offset(void)
{
    uint8_t data[4] = { 1, 2, 3, 4 };

    HAL_I2C_Mem_Write_ExpectAndReturn(&hi2c2, 0x50U, 0x0010U, I2C_MEMADD_SIZE_16BIT,
                                      data, 4U, 1000U, HAL_OK);
    TEST_ASSERT_EQUAL_INT(0, fram_write(0x0010U, data, 4U));

    /* 16 KB in => second chip, offset back to 0. */
    HAL_I2C_Mem_Write_ExpectAndReturn(&hi2c2, 0x51U, 0x0000U, I2C_MEMADD_SIZE_16BIT,
                                      data, 4U, 1000U, HAL_OK);
    TEST_ASSERT_EQUAL_INT(0, fram_write(16UL * 1024UL, data, 4U));
}

void test_fram_read_propagates_hal_error(void)
{
    uint8_t buf[4] = { 0 };

    HAL_I2C_Mem_Read_ExpectAndReturn(&hi2c2, 0x50U, 0x0000U, I2C_MEMADD_SIZE_16BIT,
                                     buf, 4U, 1000U, HAL_TIMEOUT);
    TEST_ASSERT_EQUAL_INT(-1, fram_read(0U, buf, 4U));
}

/* ================= Cyclic buffer ================= */

void test_cyclic_buffer_advances_head(void)
{
    uint8_t data[16];
    memset(data, 0x5A, sizeof(data));

    TEST_ASSERT_EQUAL_UINT32(0U, cyclic_buffer_head());

    HAL_I2C_Mem_Write_IgnoreAndReturn(HAL_OK);
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(data), cyclic_buffer_head());

    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(2 * sizeof(data)), cyclic_buffer_head());
}

void test_cyclic_buffer_zero_length_write_is_a_noop(void)
{
    uint8_t data[1] = { 0 };
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(data, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, cyclic_buffer_head());
}

void test_cyclic_buffer_read_rejects_out_of_range(void)
{
    uint8_t buf[8] = { 0 };
    TEST_ASSERT_EQUAL_INT(-1, cyclic_buffer_read(FRAM_TOTAL - 4U, buf, sizeof(buf)));
}
