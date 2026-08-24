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

/* Program a WHOLE 128 B record without going through laststates_write(), the
 * way Core/Src/dual_bank.c ls_append() does it: HAL_FLASH_Unlock() once, then
 * all 16 double words in ascending order. A single double word is NOT a
 * second-writer record - it is a torn write, which memory.c deliberately
 * treats as unusable (see slot_is_complete()). */
static void program_full_record(uint32_t slot, const laststates_entry_t *entry)
{
    const uint8_t *bytes = (const uint8_t *)entry;
    uint32_t       off;

    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Unlock());
    for (off = 0u; off < (uint32_t)LASTSTATES_ENTRY_SIZE; off += 8u) {
        uint64_t dword;

        memcpy(&dword, bytes + off, sizeof(dword));
        TEST_ASSERT_EQUAL_INT(HAL_OK,
                              HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                                (uint32_t)(flash_base +
                                                           slot * LASTSTATES_ENTRY_SIZE + off),
                                                dword));
    }
    TEST_ASSERT_EQUAL_INT(HAL_OK, HAL_FLASH_Lock());
}

void setUp(void)
{
    host_flash_reset();
    seu_stub_reset();
    laststates_pool_lock_set_result_for_test(LASTSTATES_LOCK_NOT_NEEDED);
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
    laststates_entry_t marker = make_entry(0x0Eu, STATE_READY, STATE_READY,
                                           TRIGGER_BOOT_OK, 0x00u);

    /* 'DBNK' marker dropped into slot 0 behind the module's back. It is a
     * COMPLETE 128 B record, exactly like the one dual_bank.c ls_append()
     * programs - a lone double word would be a torn write, which the ring now
     * refuses to report (finding C4). */
    {
        const uint32_t tag = 0x4B4E4244u;   /* 'D','B','N','K' little endian */
        memcpy(marker.context, &tag, sizeof(tag));
    }
    program_full_record(0u, &marker);
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);   /* cursor is now stale */

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));

    /* Re-derived to the first free slot, and the marker is intact. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry,
                                  host_flash_pool() + LASTSTATES_ENTRY_SIZE,
                                  LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&marker,
                                  host_flash_pool(), LASTSTATES_ENTRY_SIZE);
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
 * already programmed (a write interrupted by a reset, or an SEU) is DAMAGED:
 * it can neither be completed (re-programming a non-erased row raises
 * PROGERR) nor reported (its header is 0xFF filler).
 *
 * Before the finding-C4 fix, slot_is_erased() looked at double word 0 only,
 * so the module happily started writing into such a slot, hit PROGERR on the
 * damaged row and returned -1 WITHOUT advancing the cursor - which means the
 * very next write, and every write after it, failed in exactly the same place.
 * One damaged slot killed the forensic log permanently, in flight, silently.
 *
 * The contract is now: the damaged slot is skipped, the record lands in the
 * next free slot, the Flash controller is left locked, and the damaged slot
 * is not counted or dumped. Logging survives the damage. */
void test_laststates_write_skips_a_partially_programmed_slot(void)
{
    laststates_entry_t entry = make_entry(0x7777u, STATE_READY, STATE_CRIT,
                                          TRIGGER_FAULT, 0x77u);

    /* Row 8 of slot 0 is already programmed; row 0 still reads erased, so the
     * old dword-0-only test would have seen a free slot here. */
    program_dword(64u, 0x0011223344556677ULL);
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);

    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));

    /* Landed in slot 1, no page recycled, nothing lost. */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)&entry,
                                  host_flash_pool() + LASTSTATES_ENTRY_SIZE,
                                  LASTSTATES_ENTRY_SIZE);
    TEST_ASSERT_EQUAL_UINT32(2u, mirror()->idx);
    TEST_ASSERT_EQUAL_UINT32(0u, host_flash_erase_count());

    /* The damaged slot is never reported as a record. */
    TEST_ASSERT_EQUAL_UINT32(1u, laststates_count());

    /* Flash controller left locked and the SEU lock balanced. */
    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_UINT32(host_flash_unlock_count(), host_flash_lock_count());
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());
}

/* The failure path still has to exist: when the Flash controller cannot
 * program the slot it selected, laststates_write() must report -1, leave the
 * Flash controller locked and leave the bookkeeping untouched.
 *
 * A TORN slot (reset mid-write) is NOT this case: laststates_resync() skips it
 * and the record lands in the next free slot, so logging survives (see
 * test_laststates_write_skips_a_partially_programmed_slot). The genuinely
 * unprogrammable case is a worn/stuck cell or a supply glitch that makes a
 * perfectly-erased row raise PROGERR - the host model injects exactly that via
 * host_flash_fail_program_after(), the only way to reach this branch once the
 * module refuses to write into anything that is not fully erased. */
void test_laststates_write_fails_cleanly_when_the_target_row_is_unprogrammable(void)
{
    laststates_entry_t first = make_entry(0x1111u, STATE_INIT, STATE_READY,
                                          TRIGGER_BOOT, 0x11u);
    laststates_entry_t entry = make_entry(0x7777u, STATE_READY, STATE_CRIT,
                                          TRIGGER_FAULT, 0x77u);
    int commits_before;

    /* Slot 0 is filled normally; the next write targets slot 1 (erased). */
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&first));
    TEST_ASSERT_EQUAL_UINT32(1u, mirror()->idx);

    /* Make the very next double-word program fail, as a stuck cell would on
     * real silicon. */
    host_flash_fail_program_after(0u);

    commits_before = seu_stub_commit_count();

    TEST_ASSERT_EQUAL_INT(-1, laststates_write(&entry));

    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_UINT32(host_flash_unlock_count(), host_flash_lock_count());
    TEST_ASSERT_EQUAL_INT(commits_before, seu_stub_commit_count());  /* no advance */
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());
    TEST_ASSERT_EQUAL_UINT32(1u, mirror()->idx);   /* bookkeeping untouched */
}


/* =====================================================================
 * Pool lock: "no lock needed" and "the lock failed" are different answers
 * (Kilo #21, comment id 3740842366)
 * ===================================================================== */

/* laststates_pool_lock() used to answer 0 both when serialisation was
 * unnecessary (boot, exception context) and when it was necessary but
 * unobtainable (osMutexNew() returned NULL because the FreeRTOS heap was
 * tight, or osMutexAcquire() failed). Both writers then programmed the SHARED
 * pool with no synchronisation at all - the exact race the mutex exists to
 * prevent, re-entered silently through the front door.
 *
 * The contract now: LASTSTATES_LOCK_FAILED means REFUSE. Nothing is unlocked,
 * nothing is programmed, no bookkeeping moves, and the loss is counted so
 * ground can see the pool went unsynchronised instead of inferring it from a
 * hole in the trail. */
void test_laststates_write_refuses_the_write_when_the_pool_lock_fails(void)
{
    laststates_entry_t entry = make_entry(0x2222u, STATE_READY, STATE_CRIT,
                                          TRIGGER_FAULT, 0x5Au);
    uint32_t programs_before;
    uint32_t unlocks_before;
    uint32_t dropped_before;
    int      commits_before;

    programs_before = host_flash_program_count();
    unlocks_before  = host_flash_unlock_count();
    dropped_before  = laststates_dropped_records();
    commits_before  = seu_stub_commit_count();

    laststates_pool_lock_set_result_for_test(LASTSTATES_LOCK_FAILED);
    TEST_ASSERT_EQUAL_INT(-1, laststates_write(&entry));
    laststates_pool_lock_set_result_for_test(LASTSTATES_LOCK_NOT_NEEDED);

    /* Not one Flash cycle: the controller was never even unlocked. */
    TEST_ASSERT_EQUAL_UINT32(programs_before, host_flash_program_count());
    TEST_ASSERT_EQUAL_UINT32(unlocks_before, host_flash_unlock_count());
    TEST_ASSERT_FALSE(host_flash_is_unlocked());
    TEST_ASSERT_EQUAL_UINT32(0u, mirror()->idx);
    TEST_ASSERT_EQUAL_INT(commits_before, seu_stub_commit_count());
    TEST_ASSERT_EQUAL_INT(0, seu_stub_lock_depth());

    /* ...and the refusal is observable from the ground. */
    TEST_ASSERT_EQUAL_UINT32(dropped_before + 1u, laststates_dropped_records());

    /* The pool is still usable once serialisation is available again. */
    TEST_ASSERT_EQUAL_INT(0, laststates_write(&entry));
    TEST_ASSERT_EQUAL_UINT32(1u, mirror()->idx);
}


/* A cyclic record can cross a physical 16 KB chip boundary without crossing
 * the 64 KB ring boundary. The cyclic layer must split it into legal FRAM
 * transfers; exposing a single cross-chip request would make the driver reject
 * it and silently lose ordinary telemetry records. */
void test_cyclic_buffer_write_splits_a_record_at_a_chip_boundary(void)
{
    static uint8_t filler[4096];
    const uint8_t record[8] = { 0xD0u, 0xD1u, 0xD2u, 0xD3u,
                                0xE0u, 0xE1u, 0xE2u, 0xE3u };
    uint8_t tail[4];
    uint8_t head[4];
    uint32_t i;

    memset(filler, 0x55, sizeof(filler));
    fram_init();
    cyclic_buffer_init();

    for (i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(filler, sizeof(filler)));
    }
    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(filler, sizeof(filler) - 4u));
    TEST_ASSERT_EQUAL_UINT32(FRAM_CHIP_SIZE - 4u, cyclic_buffer_head());

    TEST_ASSERT_EQUAL_INT(0, cyclic_buffer_write(record, sizeof(record)));
    TEST_ASSERT_EQUAL_UINT32(FRAM_CHIP_SIZE + 4u, cyclic_buffer_head());
    TEST_ASSERT_EQUAL_INT(0, fram_read(FRAM_CHIP_SIZE - 4u, tail, sizeof(tail)));
    TEST_ASSERT_EQUAL_INT(0, fram_read(FRAM_CHIP_SIZE, head, sizeof(head)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(record, tail, sizeof(tail));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(record + 4u, head, sizeof(head));
}

/* The public cyclic API accepts at most one full ring per call. Larger writes
 * would otherwise require overwriting a range twice and make failure recovery
 * ambiguous, so reject them before touching FRAM or advancing the cursor. */
void test_cyclic_buffer_write_rejects_a_record_larger_than_the_ring(void)
{
    static uint8_t oversized[FRAM_TOTAL_SIZE + 1u];

    fram_init();
    cyclic_buffer_init();

    TEST_ASSERT_EQUAL_INT(-1, cyclic_buffer_write(oversized, sizeof(oversized)));
    TEST_ASSERT_EQUAL_UINT32(0u, cyclic_buffer_head());

    TEST_ASSERT_EQUAL_INT(-1, cyclic_buffer_write(NULL, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, cyclic_buffer_head());
}
