/*
 * radiohal.cpp — STM32 HAL adapter implementation for RadioLib.
 *
 * Ported from Marco-42/RedPill-T (satellite/stm32_lora/Core/Src/STM32Hal.cpp),
 * adapted for JOS. See radiohal.h for licensing + pin-mapping notes.
 *
 * SPI uses REAL DMA: HAL_SPI_TransmitReceive_DMA on SPI1 (DMA1 Channel2 RX /
 * Channel3 TX, CSELR mapping per RM0351 Table 46 — STM32L496 has no DMAMUX),
 * with IRQ-driven completion (DMA1_Channel2/3_IRQHandler in stm32l4xx_it.c,
 * SPF §3.6.4.2). spiTransfer() blocks on HAL_SPI_GetState() with a BOUNDED
 * timeout because RadioLib's HAL abstraction is synchronous — never
 * HAL_MAX_DELAY from a task. SPI1 is shared with the CLOUD MAX11128 ADC, so
 * the whole burst runs under the spi1_bus mutex (see App/comms/spi1_bus.h).
 * delayMicroseconds() is BLOCKING: only call from init / non-RTOS-hot paths
 * (SPF: radio.begin() performs reset settling).
 */

#include "radiolib_hal.h"
#include "spi1_bus.h"

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

void STM32Hal::spiBegin()
{
    /* CS handled by digitalWrite in RadioLib. Ensure the shared-bus mutex
       exists: idempotent, first client init wins. */
    spi1_bus_init();
}
void STM32Hal::spiEnd()          { /* nothing to release */ }

void STM32Hal::spiBeginTransaction()
{
    if (_spi != nullptr) {
        HAL_SPI_Init(_spi);   /* idempotent re-init is safe */
    }
}

void STM32Hal::spiEndTransaction() { /* CS de-asserted by RadioLib after xfer */ }

/* Max DMA wait per chunk: SPI1 runs at ~10 Mbit/s, so even a full 64 KiB
   chunk takes ~50 ms on the wire; 100 ms bounds the block without risking
   a false timeout, and HAL_GetTick() subtraction is wrap-safe (TIM6 tick). */
#define STM32HAL_SPI_DMA_TIMEOUT_MS 100U
#define STM32HAL_SPI_DMA_MAX_CHUNK  0xFFFFU

void STM32Hal::spiTransfer(uint8_t* out, size_t len, uint8_t* in)
{
    if (_spi == nullptr || out == nullptr || in == nullptr || len == 0U) {
        return;
    }
    /* Shared SPI1 (radio + CLOUD ADC): serialise the whole burst with a
       BOUNDED lock wait — never osWaitForever from a task. On refusal leave
       in[] untouched (same best-effort contract as a HAL timeout). */
    if (!spi1_bus_lock(SPI1_BUS_LOCK_TIMEOUT_MS)) {
        return;
    }
    /* HAL takes a uint16_t size: split oversized transfers into chunks.
       (RadioLib/SX1268 bursts are < 300 B; this is belt-and-braces.) */
    bool stream_ok = true;
    while ((len > 0U) && stream_ok) {
        uint16_t chunk = (len > STM32HAL_SPI_DMA_MAX_CHUNK)
                       ? (uint16_t)STM32HAL_SPI_DMA_MAX_CHUNK
                       : (uint16_t)len;
        /* On start failure — e.g. HAL_BUSY — stop the stream, leave the rest
           of in[] untouched. */
        if (HAL_SPI_TransmitReceive_DMA(_spi, out, in, chunk) != HAL_OK) {
            stream_ok = false;
            break;
        }
        /* Synchronous RadioLib contract: block until the DMA TC IRQ drives
           the SPI handle back to READY (see DMA1_Channel2/3_IRQHandler).
           Bounded: abort the transfer rather than park the task forever. */
        uint32_t tickstart = HAL_GetTick();
        while (HAL_SPI_GetState(_spi) != HAL_SPI_STATE_READY) {
            if ((HAL_GetTick() - tickstart) > STM32HAL_SPI_DMA_TIMEOUT_MS) {
                (void)HAL_SPI_Abort(_spi);
                stream_ok = false;
                break;
            }
        }
        if (!stream_ok) {
            break;
        }
        out += chunk;
        in  += chunk;
        len -= chunk;
    }
    spi1_bus_unlock();
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

/* Microseconds since boot. T35 moved the HAL 1 ms tick onto TIM6 and left
   SysTick to FreeRTOS, so a SysTick->LOAD/VAL remainder would measure the
   RTOS quantum — NOT HAL time — and is wrong here. TIM6 runs its counter at
   1 MHz with period 999 (see stm32l4xx_hal_timebase_tim.c), so TIM6->CNT is
   the sub-millisecond microsecond remainder. The HAL_GetTick() double-read
   closes the update-interrupt race at the ms edge. If TIM6 is not running
   yet (early init), degrade to whole milliseconds: documented, never
   fabricated sub-ms digits. RadioLib relies on micros() for reset-settling
   and preamble timing, so the unit must be correct. */
unsigned long STM32Hal::micros()
{
    uint32_t ms_before = (uint32_t)HAL_GetTick();
    uint32_t cnt = (htim6.Instance != NULL) ? htim6.Instance->CNT : 0U;
    uint32_t ms_after = (uint32_t)HAL_GetTick();
    if (ms_after != ms_before) {
        /* Tick wrapped mid-read: re-sample both so tick and remainder agree. */
        ms_before = ms_after;
        cnt = (htim6.Instance != NULL) ? htim6.Instance->CNT : 0U;
    }
    if (cnt > 999U) {
        cnt = 999U;
    }
    return (unsigned long)ms_before * 1000UL + (unsigned long)cnt;
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
