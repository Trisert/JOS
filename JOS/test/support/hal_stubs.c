/* ---------------------------------------------------------------------------
 * hal_stubs.c - host definitions for the CMSIS/HAL entry points that the
 *               modules under test call but that have no meaning on a PC.
 *
 * Only symbols that are *referenced* by host-compiled flight code live here:
 *
 *   App/obsw/boot_crc.c -> HAL_GetTick()        (timestamp of the CRC-fail
 *                                                LastStates record)
 *                       -> NVIC_SystemReset()   (reboot after too many
 *                                                consecutive CRC failures)
 *
 * NVIC_SystemReset() must not return on target. Returning here would let the
 * test continue past a point the flight code considers unreachable, so the
 * double either
 *
 *   - aborts the test run with a readable message (default: nothing asked for
 *     a reboot, so a reboot is a defect), or
 *   - longjmp()s straight back to the HOST_EXPECT_NVIC_RESET() call site when
 *     a test has explicitly armed the capture. That reproduces "does not
 *     return" faithfully: no statement after NVIC_SystemReset() is executed,
 *     which is exactly what the retry-budget logic in boot_crc_apply_policy()
 *     relies on.
 *
 * Refs: NASA-STD-8739.8 (test doubles must be explicit, never silent).
 * ------------------------------------------------------------------------- */
#include "main.h"        /* fakes/main.h */
#include "host_support.h"
#include "unity.h"

#include <stdint.h>

/* Monotonic millisecond tick. Deterministic (no wall clock) so a test that
 * asserts on a recorded timestamp can predict it: the counter advances by one
 * per call and is reset by host_hal_tick_reset(). */
static uint32_t host_tick;

uint32_t HAL_GetTick(void)
{
    return host_tick++;
}

void host_hal_tick_reset(void)
{
    host_tick = 0u;
}

/* ---------------------------------------------------------------------------
 * NVIC_SystemReset() capture
 * ------------------------------------------------------------------------- */
jmp_buf host_nvic_reset_jmp;

static int      nvic_reset_armed;
static uint32_t nvic_reset_requests;

void host_nvic_reset_arm(void)
{
    nvic_reset_armed = 1;
}

void host_nvic_reset_disarm(void)
{
    nvic_reset_armed = 0;
}

uint32_t host_nvic_reset_count(void)
{
    return nvic_reset_requests;
}

void host_nvic_reset_clear(void)
{
    nvic_reset_armed    = 0;
    nvic_reset_requests = 0u;
}

void NVIC_SystemReset(void)
{
    nvic_reset_requests++;

    if (nvic_reset_armed) {
        /* Armed by HOST_EXPECT_NVIC_RESET(): emulate the reboot by never
         * returning to the caller. */
        nvic_reset_armed = 0;
        longjmp(host_nvic_reset_jmp, 1);
    }

    /* On target this never returns. Failing loudly beats silently continuing
     * into code the flight software believes is unreachable. */
    TEST_FAIL_MESSAGE("NVIC_SystemReset() called: the code under test asked "
                      "for a reboot");
}

/* ---------------------------------------------------------------------------
 * GPIO doubles — PB1 deploy mux + PB2 1-wire bus (see fakes/main.h)
 * ------------------------------------------------------------------------- */

GPIO_TypeDef fake_GPIOB = {0};

/* 16 pins of port B: last init mode/pull, output latch mirror, and an input
 * override driven by the test (-1 = no override, reads follow ODR). */
static uint32_t gpio_mode[16];
static uint32_t gpio_pull[16];
static int      gpio_force[16];
/* Latch: did this pin ever see Mode==INPUT && Pull==PULLUP since reset?
 * Lets tests assert the deploy-read transient (input+pull-up) even though
 * the pin is restored to output before the call returns. */
static int      gpio_saw_in_pu[16];
static int      gpio_initialised;

static void gpio_lazy_init(void)
{
    if (!gpio_initialised) {
        for (int i = 0; i < 16; i++) {
            gpio_force[i] = -1;
        }
        gpio_initialised = 1;
    }
}

static int gpio_bit_index(uint16_t pin)
{
    for (int i = 0; i < 16; i++) {
        if (pin == (uint16_t)(1u << i)) {
            return i;
        }
    }
    return -1;
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *cfg)
{
    uint16_t mask;
    (void)port;   /* single-port (GPIOB) double */
    gpio_lazy_init();
    if (cfg == NULL) {
        return;
    }
    mask = cfg->Pin;
    for (int i = 0; i < 16; i++) {
        if ((mask & (uint16_t)(1u << i)) != 0u) {
            gpio_mode[i] = cfg->Mode;
            gpio_pull[i] = cfg->Pull;
            if ((cfg->Mode == GPIO_MODE_INPUT) && (cfg->Pull == GPIO_PULLUP)) {
                gpio_saw_in_pu[i] = 1;
            }
        }
    }
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState s)
{
    if (s == GPIO_PIN_SET) {
        port->ODR |= pin;
    } else {
        port->ODR &= (uint32_t)(~((uint32_t)pin));
    }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    int idx;
    gpio_lazy_init();
    idx = gpio_bit_index(pin);
    if ((idx >= 0) && (gpio_force[idx] >= 0)) {
        return (gpio_force[idx] != 0) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }
    return ((port->ODR & pin) != 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void host_gpio_force_input(int pin_index, int level)
{
    gpio_lazy_init();
    if ((pin_index >= 0) && (pin_index < 16)) {
        gpio_force[pin_index] = level;
    }
}

uint32_t host_gpio_last_mode(int pin_index)
{
    gpio_lazy_init();
    if ((pin_index >= 0) && (pin_index < 16)) {
        return gpio_mode[pin_index];
    }
    return 0u;
}

uint32_t host_gpio_last_pull(int pin_index)
{
    gpio_lazy_init();
    if ((pin_index >= 0) && (pin_index < 16)) {
        return gpio_pull[pin_index];
    }
    return 0u;
}

int host_gpio_odr(int pin_index)
{
    if ((pin_index >= 0) && (pin_index < 16)) {
        return ((fake_GPIOB.ODR & (1u << pin_index)) != 0u) ? 1 : 0;
    }
    return 0;
}

/* 1 if the pin was ever programmed as input+pull-up since host_gpio_reset. */
int host_gpio_saw_input_pullup(int pin_index)
{
    gpio_lazy_init();
    if ((pin_index >= 0) && (pin_index < 16)) {
        return gpio_saw_in_pu[pin_index];
    }
    return 0;
}

void host_gpio_reset(void)
{
    gpio_lazy_init();
    for (int i = 0; i < 16; i++) {
        gpio_mode[i]  = 0u;
        gpio_pull[i]  = 0u;
        gpio_force[i] = -1;
        gpio_saw_in_pu[i] = 0;
    }
    fake_GPIOB.ODR = 0u;
    fake_GPIOB.IDR = 0u;
}
