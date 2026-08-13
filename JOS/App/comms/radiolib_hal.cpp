/*
 * radiohal.cpp — STM32 HAL adapter implementation for RadioLib.
 *
 * Ported from Marco-42/RedPill-T (satellite/stm32_lora/Core/Src/STM32Hal.cpp),
 * adapted for JOS. See radiohal.h for licensing + pin-mapping notes.
 *
 * SPI is polled via HAL_SPI_TransmitReceive (no DMA here — RadioLib's HAL
 * abstraction is synchronous). delayMicroseconds() is BLOCKING: only call from
 * init / non-RTOS-hot paths (SPF: radio.begin() performs reset settling).
 */

#include "radiolib_hal.h"

STM32Hal::STM32Hal(SPI_HandleTypeDef* spiHandle)
    : RadioLibHal(/*input*/1, /*output*/0, /*low*/0, /*high*/1, /*rising*/2, /*falling*/3),
      _spi(spiHandle)
{
    for (int i = 0; i < MAX_PINS; i++) {
        _pinMap[i].port = nullptr;
        _pinMap[i].pin  = 0;
    }
}

void STM32Hal::addPin(uint32_t pinId, GPIO_TypeDef* port, uint16_t pin)
{
    if (pinId < (uint32_t)MAX_PINS) {
        _pinMap[pinId].port = port;
        _pinMap[pinId].pin  = pin;
    }
}

Stm32Pin* STM32Hal::getStmPin(uint32_t pinId)
{
    if (pinId >= (uint32_t)MAX_PINS) {
        return nullptr;
    }
    return &_pinMap[pinId];
}

/* ----------------------------- GPIO ----------------------------- */

void STM32Hal::pinMode(uint32_t pin, uint32_t mode)
{
    Stm32Pin* p = getStmPin(pin);
    if (p == nullptr || p->port == nullptr) {
        return;
    }
    GPIO_InitTypeDef cfg = {0};
    cfg.Pin  = p->pin;
    cfg.Mode = (mode == 0U) ? GPIO_MODE_OUTPUT_PP : GPIO_MODE_INPUT;
    cfg.Pull = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(p->port, &cfg);
}

void STM32Hal::digitalWrite(uint32_t pin, uint32_t value)
{
    Stm32Pin* p = getStmPin(pin);
    if (p == nullptr || p->port == nullptr) {
        return;
    }
    HAL_GPIO_WritePin(p->port, p->pin,
                      (value == 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint32_t STM32Hal::digitalRead(uint32_t pin)
{
    Stm32Pin* p = getStmPin(pin);
    if (p == nullptr || p->port == nullptr) {
        return 0U;
    }
    return (uint32_t)HAL_GPIO_ReadPin(p->port, p->pin);
}

/* ----------------------------- SPI ------------------------------ */

void STM32Hal::spiBegin()        { /* CS handled by digitalWrite in RadioLib */ }
void STM32Hal::spiEnd()          { /* nothing to release */ }

void STM32Hal::spiBeginTransaction()
{
    if (_spi != nullptr) {
        HAL_SPI_Init(_spi);   /* idempotent re-init is safe */
    }
}

void STM32Hal::spiEndTransaction() { /* CS de-asserted by RadioLib after xfer */ }

void STM32Hal::spiTransfer(uint8_t* out, size_t len, uint8_t* in)
{
    if (_spi == nullptr || out == nullptr || in == nullptr) {
        return;
    }
    /* Polled full-duplex; RadioLib expects out[] echoed into in[]. */
    if (HAL_SPI_TransmitReceive(_spi, out, in, (uint16_t)len, HAL_MAX_DELAY) != HAL_OK) {
        /* Best-effort: leave in[] untouched on failure. */
    }
}

/* ----------------------------- Time ----------------------------- */

void STM32Hal::delay(RadioLibTime_t ms)
{
    /* Blocking. Only safe off the hot RTOS path (use osDelay in tasks). */
    HAL_Delay((uint32_t)ms);
}

void STM32Hal::delayMicroseconds(RadioLibTime_t us)
{
    /* Blocking microsecond spin — used by RadioLib during reset settling.
       The loop body (decrement + __NOP + branch) costs ~4 Cortex-M4 cycles,
       so scale the iteration count accordingly. Approximate by design: RadioLib
       only uses this for short reset/power-up settles where ±a few µs is fine.
       For cycle-accurate delays DWT should be used, but that needs the debug
       clock enabled; this is the safe default. */
    uint32_t cycles = ((SystemCoreClock / 1000000U) * (uint32_t)us) / 4U;
    while (cycles--) {
        __NOP();
    }
}

unsigned long STM32Hal::millis()  { return (unsigned long)(HAL_GetTick()); }

/* Microseconds since boot, derived from the 1 kHz SysTick: the whole-ms part
   from HAL_GetTick() and the sub-ms remainder from the current down-counter
   value. This is the REAL elapsed time, not HAL_GetTick()*1000 (which would
   be milliseconds mislabelled as microseconds). RadioLib relies on micros()
   for reset-settling and preamble timing, so the unit must be correct. */
unsigned long STM32Hal::micros()
{
    uint32_t ticks_per_us = SystemCoreClock / 1000000UL;
    // cppcheck-suppress cstyleCast  // SysTick is a CMSIS macro cast; unavoidable
    uint32_t elapsed_sub_ms = ((uint32_t)SysTick->LOAD - (uint32_t)SysTick->VAL) / ticks_per_us;
    return (unsigned long)(HAL_GetTick()) * 1000UL + (unsigned long)elapsed_sub_ms;
}

long STM32Hal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout)
{
    (void)pin; (void)state; (void)timeout;
    return -1L;   /* not used for SX1268 */
}

/* ----------------- Multiplexed reset/deploy pin ---------------- */
/* SPF pag 95: LoRa_NRST shares one OBC GPIO with DEPLOY_SENSE.     */

void STM32Hal::configureResetPin(void)
{
    GPIO_InitTypeDef cfg = {0};
    cfg.Pin   = LoRa_NRST_Pin;
    cfg.Mode  = GPIO_MODE_OUTPUT_PP;
    cfg.Pull  = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(LoRa_NRST_GPIO_Port, &cfg);
    /* Idle HIGH = NOT in reset (SX1268 reset is active-low). */
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_SET);
}

void STM32Hal::pulseReset(void)
{
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_RESET);
    /* >100 us per SX1268 datasheet; blocking is fine during init only. */
    delayMicroseconds(200);
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_SET);
    delayMicroseconds(200);
}

void STM32Hal::releaseResetPin(void)
{
    /* Leave as output HIGH so the line is never floating. DEPLOY_SENSE read
       must temporarily reconfigure this pin as input (see comms side). */
    HAL_GPIO_WritePin(LoRa_NRST_GPIO_Port, LoRa_NRST_Pin, GPIO_PIN_SET);
}
