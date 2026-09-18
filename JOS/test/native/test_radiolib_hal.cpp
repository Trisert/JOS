#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "radiolib_hal.h"
#include "radio_ownership.h"

GPIO_TypeDef native_gpio_a{1U};
GPIO_TypeDef native_gpio_b{2U};
GPIO_TypeDef native_gpio_c{3U};
void *SPI1 = &native_gpio_a;
SysTick_Type native_systick{80000U, 0U};
SysTick_Type *SysTick = &native_systick;
uint32_t SystemCoreClock = 80000000U;

static const char *events[32];
static unsigned event_count;
static bool acquire_ok;
static bool dma_start_ok;
static bool dma_complete;
static bool dma_error;
static unsigned acquire_count;
static unsigned release_count;
static unsigned dma_count;
static unsigned abort_count;
static unsigned cs_low_count;
static unsigned cs_high_count;
static unsigned reset_low_count;
static unsigned reset_high_count;
static uint32_t tick;
extern "C" void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *);
extern "C" void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *);

static void reset_trace(void)
{
    event_count = 0U;
    acquire_ok = true;
    dma_start_ok = true;
    dma_complete = true;
    dma_error = false;
    acquire_count = 0U;
    release_count = 0U;
    dma_count = 0U;
    abort_count = 0U;
    cs_low_count = 0U;
    cs_high_count = 0U;
    reset_low_count = 0U;
    reset_high_count = 0U;
    tick = 0U;
}

static void event(const char *name)
{
    assert(event_count < (sizeof(events) / sizeof(events[0])));
    events[event_count++] = name;
}

static void assert_events(const char *const *expected, unsigned count)
{
    assert(event_count == count);
    for (unsigned i = 0U; i < count; ++i) {
        assert(std::strcmp(events[i], expected[i]) == 0);
    }
}

extern "C" int lora_spi_bus_acquire(uint32_t timeout_ticks)
{
    assert(timeout_ticks == RADIO_OWNERSHIP_TIMEOUT_TICKS);
    ++acquire_count;
    event("acquire");
    return acquire_ok ? 0 : -1;
}

extern "C" void lora_spi_bus_release(void)
{
    ++release_count;
    event("release");
}

extern "C" void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *) {}

extern "C" void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t pin, uint32_t value)
{
    if (pin == CS_TTC_Pin) {
        if (value == GPIO_PIN_RESET) {
            ++cs_low_count;
            event("cs-low");
        } else {
            ++cs_high_count;
            event("cs-high");
        }
    } else if (pin == LoRa_NRST_Pin) {
        if (value == GPIO_PIN_RESET) {
            ++reset_low_count;
            event("reset-low");
        } else {
            ++reset_high_count;
            event("reset-high");
        }
    }
}

extern "C" uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *, uint16_t)
{
    return GPIO_PIN_SET;
}

extern "C" int HAL_SPI_Init(SPI_HandleTypeDef *)
{
    event("spi-init");
    return HAL_OK;
}

extern "C" int HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *spi,
                                             uint8_t *out, uint8_t *in,
                                             uint16_t len)
{
    (void)out;
    (void)len;
    ++dma_count;
    event("dma-start");
    if (!dma_start_ok) {
        return HAL_ERROR;
    }
    if (dma_error) {
        event("dma-error");
        HAL_SPI_ErrorCallback(spi);
    } else if (dma_complete) {
        for (uint16_t i = 0U; i < len; ++i) {
            in[i] = 0xA5U;
        }
        event("dma-complete");
        HAL_SPI_TxRxCpltCallback(spi);
    }
    return HAL_OK;
}

extern "C" int HAL_SPI_Abort(SPI_HandleTypeDef *)
{
    ++abort_count;
    event("dma-abort");
    return HAL_OK;
}

extern "C" uint32_t HAL_RCC_GetPCLK2Freq(void) { return 80000000U; }
extern "C" uint32_t HAL_GetTick(void) { return tick++; }
extern "C" void HAL_Delay(uint32_t) {}
extern "C" void __WFI(void) { tick += 100U; }
extern "C" void __NOP(void) {}

static SPI_HandleTypeDef spi{SPI1, 1U, {SPI_BAUDRATEPRESCALER_8}};

static STM32Hal make_hal(void)
{
    STM32Hal hal(&spi);
    hal.addPin(RLIB_NSS, CS_TTC_GPIO_Port, CS_TTC_Pin);
    return hal;
}

static void test_refused_transaction_is_quarantined(void)
{
    reset_trace();
    acquire_ok = false;
    STM32Hal hal = make_hal();
    hal.spiClearError();
    uint8_t out[3] = {1U, 2U, 3U};
    uint8_t in[3] = {0xCCU, 0xCCU, 0xCCU};

    hal.spiBeginTransaction();
    hal.digitalWrite(RLIB_NSS, 0U);
    hal.spiTransfer(out, sizeof(in), in);
    hal.digitalWrite(RLIB_NSS, 1U);
    hal.spiEndTransaction();

    assert(cs_low_count == 0U);
    assert(dma_count == 0U);
    assert(release_count == 0U);
    for (uint8_t byte : in) { assert(byte == 0U); }
    assert(hal.spiLastError() == SPI_XFER_DMA_START);
    const char *expected[] = {"acquire", "cs-high"};
    assert_events(expected, 2U);
}

static void test_success_order_and_idle_high(void)
{
    reset_trace();
    STM32Hal hal = make_hal();
    hal.spiClearError();
    hal.digitalWrite(RLIB_NSS, 1U);
    assert(cs_high_count == 1U);
    reset_trace();
    uint8_t out[2] = {1U, 2U};
    uint8_t in[2] = {0U, 0U};

    hal.spiBeginTransaction();
    hal.digitalWrite(RLIB_NSS, 0U);
    hal.spiTransfer(out, sizeof(in), in);
    hal.digitalWrite(RLIB_NSS, 1U);
    hal.spiEndTransaction();

    assert(in[0] == 0xA5U && in[1] == 0xA5U);
    assert(hal.spiLastError() == SPI_XFER_OK);
    const char *expected[] = {"acquire", "cs-low", "dma-start",
                              "dma-complete", "cs-high", "release"};
    assert_events(expected, 6U);
    assert(cs_low_count == 1U && cs_high_count == 1U);
}

static void test_reset_idle_high_is_preserved(void)
{
    reset_trace();
    STM32Hal hal = make_hal();
    hal.configureResetPin();
    hal.pulseReset();
    hal.releaseResetPin();

    const char *expected[] = {"reset-high", "reset-low", "reset-high",
                              "reset-high"};
    assert_events(expected, 4U);
    assert(reset_low_count == 1U && reset_high_count == 3U);
}

static void test_error_stays_until_explicit_clear(void)
{
    reset_trace();
    STM32Hal hal = make_hal();
    uint8_t out = 0x11U;
    uint8_t in = 0xCCU;

    dma_start_ok = false;
    hal.spiBeginTransaction();
    hal.spiTransfer(&out, 1U, &in);
    hal.spiEndTransaction();
    assert(in == 0U);
    assert(abort_count == 1U);
    assert(hal.spiLastError() == SPI_XFER_DMA_START);

    dma_start_ok = true;
    hal.spiBeginTransaction();
    hal.spiTransfer(&out, 1U, &in);
    hal.spiEndTransaction();
    assert(hal.spiLastError() == SPI_XFER_DMA_START);

    hal.spiClearError();
    assert(hal.spiLastError() == SPI_XFER_OK);
}

static void test_dma_timeout_is_quarantined(void)
{
    reset_trace();
    dma_complete = false;
    STM32Hal hal = make_hal();
    uint8_t out = 0x11U;
    uint8_t in = 0xCCU;

    hal.spiBeginTransaction();
    hal.digitalWrite(RLIB_NSS, 0U);
    hal.spiTransfer(&out, 1U, &in);
    hal.digitalWrite(RLIB_NSS, 1U);
    hal.spiEndTransaction();

    assert(in == 0U);
    assert(abort_count == 1U);
    assert(hal.spiLastError() == SPI_XFER_DMA_TIMEOUT);
    assert(release_count == 1U);
}

static void test_dma_error_is_quarantined(void)
{
    reset_trace();
    dma_error = true;
    STM32Hal hal = make_hal();
    uint8_t out = 0x22U;
    uint8_t in = 0xCCU;

    hal.spiBeginTransaction();
    hal.digitalWrite(RLIB_NSS, 0U);
    hal.spiTransfer(&out, 1U, &in);
    hal.digitalWrite(RLIB_NSS, 1U);
    hal.spiEndTransaction();

    assert(in == 0U);
    assert(abort_count == 1U);
    assert(hal.spiLastError() == SPI_XFER_DMA_ERROR);
    assert(release_count == 1U);
}

int main(void)
{
    test_refused_transaction_is_quarantined();
    test_success_order_and_idle_high();
    test_reset_idle_high_is_preserved();
    test_error_stays_until_explicit_clear();
    test_dma_timeout_is_quarantined();
    test_dma_error_is_quarantined();
    puts("real radiolib_hal: ownership, CS, DMA ordering, quarantine, sticky error PASS");
    return 0;
}
