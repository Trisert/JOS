/**
 * @file    test_deploy_sense.c
 * @brief   Unit tests for the DEPLOY_SENSE / LoRa_NRST mux on PB1
 *          (App/comms/deploy_sense.c, SPF v3 3.7.5.3.1 p.95).
 *
 * The PB1 GPIO is doubled by support/hal_stubs.c: HAL_GPIO_Init records the
 * last mode/pull per pin, HAL_GPIO_WritePin drives an ODR latch, and
 * host_gpio_force_input() overrides what a pin reads as input (the
 * deploy-switch level). That is exactly the surface the mux sequences, so
 * "the radio line is never left floating" is an assertion, not an inference.
 *
 * What is verified
 *   - mux init leaves PB1 as output HIGH (radio NOT in reset).
 *   - a deploy read samples the switch level AND restores output HIGH.
 *   - deployed = LOW, stowed = HIGH (DEPLOY_DEPLOYED_LEVEL).
 *   - the SX1268 reset helpers drive the same pin (assert LOW, release HIGH,
 *     pulse ends HIGH).
 */

#include "unity.h"
#include "deploy_sense.h"
#include "host_support.h"
#include "main.h"   /* fakes/main.h on the host: GPIO mode/pull constants */

#define PB1 1

void setUp(void)
{
    host_gpio_reset();
    host_gpio_force_input(PB1, -1);   /* PB1 follows ODR unless forced */
}

void tearDown(void)
{
    host_gpio_force_input(PB1, -1);
}

/* Mux init must idle HIGH as push-pull output: the SX1268 reset is
 * active-low, so anything else holds the radio in reset from boot. */
void test_mux_init_idle_high(void)
{
    deploy_mux_init();
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* A stowed switch (pull-up HIGH) reads 1, and the pin comes back as
 * output HIGH — the reset role is restored before returning. */
void test_read_stowed_restores_output_high(void)
{
    int level;

    deploy_mux_init();
    host_gpio_force_input(PB1, 1);
    level = deploy_read_raw();

    TEST_ASSERT_EQUAL(1, level);
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* A fired switch pulls PB1 to GND: reads 0 even though the idle ODR is 1. */
void test_read_deployed_switch_low(void)
{
    int level;

    deploy_mux_init();
    host_gpio_force_input(PB1, 0);
    level = deploy_read_raw();

    TEST_ASSERT_EQUAL(0, level);
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* While sampling, the pin really passes through input+pull-up (not an
 * output fighting the switch): the stub latches the transient mode even
 * though the pin is restored to output before the call returns. */
void test_read_samples_as_input_pullup(void)
{
    deploy_mux_init();
    TEST_ASSERT_EQUAL(0, host_gpio_saw_input_pullup(PB1));

    host_gpio_force_input(PB1, 0);
    TEST_ASSERT_EQUAL(0, deploy_read_raw());

    TEST_ASSERT_EQUAL(1, host_gpio_saw_input_pullup(PB1));
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
}

/* Polarity: DEPLOYED reads LOW, STOWED reads HIGH. */
void test_is_deployed_polarity(void)
{
    deploy_mux_init();

    host_gpio_force_input(PB1, 0);
    TEST_ASSERT_EQUAL(1, deploy_is_deployed());

    host_gpio_force_input(PB1, 1);
    TEST_ASSERT_EQUAL(0, deploy_is_deployed());
}

/* Reset helpers share the same pin: assert pulls LOW, release drives HIGH,
 * pulse (>100 us per SX1268 datasheet) ends HIGH. */
void test_nrst_helpers(void)
{
    deploy_mux_init();

    deploy_nrst_assert();
    TEST_ASSERT_EQUAL(0, host_gpio_odr(PB1));

    deploy_nrst_release();
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));

    deploy_nrst_assert();
    deploy_nrst_pulse();
    TEST_ASSERT_EQUAL(1, host_gpio_odr(PB1));
    TEST_ASSERT_EQUAL_UINT32(GPIO_MODE_OUTPUT_PP, host_gpio_last_mode(PB1));
}
