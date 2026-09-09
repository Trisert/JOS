/**
 * @file    test_spi_bus.c
 * @brief   Unit tests for the shared SPI1 bus mutex (App/comms/spi1_bus.c).
 *
 * The radio (SX1268) and the CLOUD MAX11128 ADC share one physical SPI1 bus
 * from different FreeRTOS tasks; this mutex is the only thing serialising
 * their frames. The RTOS calls are hand-doubled here (controllable return
 * values + call counters) instead of CMock because the contract under test
 * is behavioural over a sequence of calls (init-once, refuse-when-missing),
 * not a single-call expectation.
 *
 * Standards: ECSS-E-ST-40C §5.5, NASA-STD-8739.8.
 */

#include "unity.h"
#include "spi1_bus.h"

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Controllable doubles for the three CMSIS-RTOS2 entry points spi1_bus uses.
 * ------------------------------------------------------------------------- */

static int      dbl_new_calls;
static int      dbl_new_fail;          /* >0: next N creations return NULL */
static int      dbl_acquire_calls;
static uint32_t dbl_acquire_last_timeout;
static int      dbl_acquire_rv;        /* osStatus_t to return */
static int      dbl_release_calls;

static int dbl_dummy_mutex;

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    (void)attr;
    dbl_new_calls++;
    if (dbl_new_fail > 0) {
        dbl_new_fail--;
        return NULL;
    }
    return (osMutexId_t)&dbl_dummy_mutex;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    (void)mutex_id;
    dbl_acquire_calls++;
    dbl_acquire_last_timeout = timeout;
    return (osStatus_t)dbl_acquire_rv;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    (void)mutex_id;
    dbl_release_calls++;
    return osOK;
}

/* ---------------------------------------------------------------------------
 * Fixture. NOTE: s_spi1_mutex inside spi1_bus.c is process state shared by
 * all tests in this binary. Creation-failure tests must run before the first
 * successful init; Unity runs tests in file order, which this file relies on
 * (documented here so a future reorder does not silently void the coverage).
 * ------------------------------------------------------------------------- */

void setUp(void)
{
    dbl_new_calls            = 0;
    dbl_acquire_calls        = 0;
    dbl_acquire_last_timeout = 0U;
    dbl_acquire_rv           = osOK;
    dbl_release_calls        = 0;
    /* dbl_new_fail is intentionally NOT reset: failure-arming tests set it. */
}

void tearDown(void)
{
}

/* Lock before any successful init must refuse, never touch the RTOS. */
void test_spi_bus_lock_before_init_refuses(void)
{
    dbl_new_fail = 1;              /* keep the module uninitialised below */
    spi1_bus_init();               /* consumes the failure, mutex stays NULL */
    dbl_new_fail = 0;

    TEST_ASSERT_FALSE(spi1_bus_lock(SPI1_BUS_LOCK_TIMEOUT_MS));
    TEST_ASSERT_EQUAL(0, dbl_acquire_calls);
}

/* First successful init creates the mutex exactly once... */
void test_spi_bus_init_creates_mutex_once(void)
{
    spi1_bus_init();
    spi1_bus_init();
    TEST_ASSERT_EQUAL(1, dbl_new_calls);
}

/* ...and a lock then reaches the RTOS with the caller's timeout. */
void test_spi_bus_lock_forwards_timeout(void)
{
    TEST_ASSERT_TRUE(spi1_bus_lock(10U));
    TEST_ASSERT_EQUAL(1, dbl_acquire_calls);
    TEST_ASSERT_EQUAL_UINT32(10U, dbl_acquire_last_timeout);
}

/* A refused acquire (timeout / busy) propagates as false. */
void test_spi_bus_lock_timeout_propagates(void)
{
    dbl_acquire_rv = osErrorTimeout;
    TEST_ASSERT_FALSE(spi1_bus_lock(10U));
    TEST_ASSERT_EQUAL(1, dbl_acquire_calls);
}

/* Unlock releases exactly once per call and never fails the caller. */
void test_spi_bus_unlock_releases(void)
{
    spi1_bus_unlock();
    TEST_ASSERT_EQUAL(1, dbl_release_calls);
    spi1_bus_unlock();
    TEST_ASSERT_EQUAL(2, dbl_release_calls);
}
