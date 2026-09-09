/*
 * radiohal.cpp — STM32 HAL adapter implementation for RadioLib.
 *
 * Ported from Marco-42/RedPill-T (satellite/stm32_lora/Core/Src/STM32Hal.cpp),
 * adapted for JOS. See radiohal.h for licensing + pin-mapping notes.
 *
 * SPI uses REAL DMA: HAL_SPI_TransmitReceive_DMA on SPI1 (DMA1 Channel2 RX /
 * Channel3 TX, CSELR mapping per RM0351 Table 46 — STM32L496 has no DMAMUX),
 * with IRQ-driven completion (DMA1_Channel2/3_IRQHandler in stm32l4xx_it.c).
 * spiTransfer() still blocks because RadioLib's HAL abstraction is
 * synchronous, but the wait sleeps with __WFI on a DMA-complete flag (woken
 * by the DMA IRQ or the TIM6 timebase tick) instead of busy-spinning, and
 * every chunk carries a baud-derived timeout. delayMicroseconds() is
 * BLOCKING: only call from init / non-RTOS-hot paths (SPF: radio.begin()
 * performs reset settling).
 */

#include "radiolib_hal.h"
#include "spi_dma_sched.h"

/* DMA completion state, written by the ISR callbacks below, read by the
 * waiting spiTransfer(). Plain volatile bytes: single-writer (ISR) /
 * single-reader (task) handoff, no FreeRTOS call allowed in the ISR
 * (DMA IRQs run at prio 5 = MAX_SYSCALL, but the callbacks stay HAL-free
 * by design so the ceiling never matters here). */
namespace {
volatile uint8_t s_dma_state = 0U;   /* 0 idle, 1 busy, 2 done, 3 error */
volatile uint32_t s_spi_last_error = 0U;
}

/* HAL callbacks: C linkage, SPI1 only. TX/RX complete share one flag because
 * TransmitReceive_DMA always runs both directions together. */
extern "C" void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if ((hspi != nullptr) && (hspi->Instance == SPI1)) {
        s_dma_state = 2U;
    }
}

extern "C" void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if ((hspi != nullptr) && (hspi->Instance == SPI1)) {
        s_dma_state = 3U;
    }
}

uint32_t STM32Hal::spiLastError(void)
{
    return s_spi_last_error;
}

void STM32Hal::spiClearError(void)
{
    s_spi_last_error = (uint32_t)SPI_XFER_OK;
}

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

/* spiEnd() intentionally does NOT call HAL_SPI_DeInit(): DeInit would
 * disable the DMA IRQs and tear down the handles while another transfer may
 * be queued, and the DMA1 clock would need refcounting to avoid a power
 * leak. The handles are configured once (below) and live for the whole
 * mission; the ~DMA1-clock cost is negligible next to the radio. */
void STM32Hal::spiEnd()          { /* nothing to release */ }

void STM32Hal::spiBeginTransaction()
{
    /* One-time init: MX_SPI1_Init() already configured the peripheral at
     * boot; re-running HAL_SPI_Init() (hence MspInit: HAL_DMA_Init + NVIC)
     * on every transaction is racy against an in-flight transfer (a
     * swallowed HAL_BUSY) and buys nothing. Only recover from RESET state. */
    if ((_spi != nullptr) &&
        (_spi->State == HAL_SPI_STATE_RESET)) {
        HAL_SPI_Init(_spi);
    }
}

void STM32Hal::spiEndTransaction() { /* CS de-asserted by RadioLib after xfer */ }

/* Decode the programmed BaudRatePrescaler to its divisor (RM0351 §40.4.2). */
static uint32_t spi_presc_div(uint32_t prescaler)
{
    switch (prescaler) {
    case SPI_BAUDRATEPRESCALER_2:   return 2U;
    case SPI_BAUDRATEPRESCALER_4:   return 4U;
    case SPI_BAUDRATEPRESCALER_8:   return 8U;
    case SPI_BAUDRATEPRESCALER_16:  return 16U;
    case SPI_BAUDRATEPRESCALER_32:  return 32U;
    case SPI_BAUDRATEPRESCALER_64:  return 64U;
    case SPI_BAUDRATEPRESCALER_128: return 128U;
    case SPI_BAUDRATEPRESCALER_256: return 256U;
    default:                        return 8U;   /* MX_SPI1_Init default */
    }
}

void STM32Hal::spiTransfer(uint8_t* out, size_t len, uint8_t* in)
{
    if (_spi == nullptr || out == nullptr || in == nullptr) {
        return;
    }
    if (len == 0U) {
        return;
    }

    /* Per-chunk timeout from the REAL baud (PCLK2 / prescaler), not an
     * assumed bit rate: with MX defaults (80 MHz / 8 = 10 Mbit/s) a full
     * 0xFFFF chunk needs ~52 ms wire time; margin covers IRQ latency. */
    const uint32_t pclk  = HAL_RCC_GetPCLK2Freq();
    const uint32_t div   = spi_presc_div(_spi->Init.BaudRatePrescaler);
    size_t         done  = 0U;
    bool           failed = false;

    while ((done < len) && !failed) {
        size_t   left  = len - done;
        uint32_t chunk = (left > (size_t)SPI_DMA_MAX_CHUNK)
                         ? (uint32_t)SPI_DMA_MAX_CHUNK : (uint32_t)left;
        uint32_t timeout = spi_dma_chunk_timeout_ms(chunk, pclk, div, 25U);
        uint32_t t0      = HAL_GetTick();

        s_dma_state = 1U;
        if (HAL_SPI_TransmitReceive_DMA(_spi, &out[done], &in[done],
                                        (uint16_t)chunk) != HAL_OK) {
            s_spi_last_error = (uint32_t)SPI_XFER_DMA_START;
            failed = true;
            break;
        }
        /* Sleep until the DMA IRQ (or the TIM6 tick) wakes us; the tick
         * guarantees progress even if an IRQ is ever lost. Wrap-safe:
         * unsigned subtraction survives the 49-day HAL_GetTick() rollover. */
        while (s_dma_state == 1U) {
            if ((HAL_GetTick() - t0) >= timeout) {
                break;
            }
            __WFI();
        }
        if (s_dma_state != 2U) {
            /* Timeout or DMA error (TE flag -> ErrorCallback -> state 3):
             * stop the peripheral so the next transfer starts clean, then
             * quarantine this chunk: zero-fill, never hand RadioLib a
             * half-shifted frame that could parse as valid. */
            HAL_SPI_Abort(_spi);
            for (uint32_t i = 0U; i < chunk; i++) {
                in[done + i] = 0U;
            }
            s_spi_last_error = (s_dma_state == 3U)
                               ? (uint32_t)SPI_XFER_DMA_ERROR
                               : (uint32_t)SPI_XFER_DMA_TIMEOUT;
            failed = true;
            break;
        }
        done += chunk;
    }
    s_dma_state = 0U;
    if (!failed) {
        s_spi_last_error = (uint32_t)SPI_XFER_OK;
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
    /* Blocking microsecond spin — used by RadioLib during reset settling. */
    volatile uint32_t cycles = (SystemCoreClock / 1000000U) * (uint32_t)us;
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
