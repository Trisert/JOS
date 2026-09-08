/* deploy_sense.c — DEPLOY_SENSE / LoRa_NRST mux on PB1 (see header). */

#include "deploy_sense.h"
#include "main.h"

#ifndef HOST_UNIT_TEST
#include "stm32l4xx_hal.h"

/* Blocking microsecond spin (same technique as the RadioLib HAL).
 * Host double below: instant no-op. */
static void mux_delay_us(uint32_t us)
{
    volatile uint32_t n = (SystemCoreClock / 1000000u) * us / 4u;
    while (n-- != 0u) {
        __NOP();
    }
}
#else
static void mux_delay_us(uint32_t us) { (void)us; }
#endif

/* Idle state of the mux: output HIGH = radio NOT in reset. */
static void mux_as_output_high(void)
{
    GPIO_InitTypeDef cfg = {0};
    cfg.Pin   = DEPLOY_SENSE_Pin;
    cfg.Mode  = GPIO_MODE_OUTPUT_PP;
    cfg.Pull  = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DEPLOY_SENSE_GPIO_Port, &cfg);
    HAL_GPIO_WritePin(DEPLOY_SENSE_GPIO_Port, DEPLOY_SENSE_Pin, GPIO_PIN_SET);
}

void deploy_mux_init(void)
{
    mux_as_output_high();
}

int deploy_read_raw(void)
{
    GPIO_InitTypeDef cfg = {0};
    int level;

    cfg.Pin   = DEPLOY_SENSE_Pin;
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_PULLUP;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DEPLOY_SENSE_GPIO_Port, &cfg);
    mux_delay_us(10u);              /* pull-up settle */
    level = (HAL_GPIO_ReadPin(DEPLOY_SENSE_GPIO_Port, DEPLOY_SENSE_Pin)
             == GPIO_PIN_SET) ? 1 : 0;
    /* Restore the reset role BEFORE returning: the line must never float. */
    mux_as_output_high();
    return level;
}

int deploy_is_deployed(void)
{
    int raw = deploy_read_raw();
    if (raw < 0) {
        return raw;
    }
    return (raw == DEPLOY_DEPLOYED_LEVEL) ? 1 : 0;
}

void deploy_nrst_assert(void)
{
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_RESET);
}

void deploy_nrst_release(void)
{
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_SET);
}

void deploy_nrst_pulse(void)
{
    deploy_nrst_assert();
    mux_delay_us(200u);             /* SX1268 needs >100 us */
    deploy_nrst_release();
}
