#include "cloud.h"
#include "MAX11128.h"
#include "main.h"
#include "radio_ownership.h"
#include "state_machine.h"
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

GPIO_TypeDef native_cloud_gpio_b;
SPI_HandleTypeDef hspi1;

static uint32_t fake_tick;
static uint32_t bus_wait_ticks;
static unsigned adc_reads;
static int bus_held;
static int bus_released_before_fram;
static unsigned fram_writes;
static unsigned expect_number;

static void expect(int condition)
{
    expect_number++;
    if (!condition) {
        (void)fprintf(stderr, "expect %u failed\n", expect_number);
    }
    assert(condition);
}

uint32_t xTaskGetTickCount(void)
{
    return fake_tick;
}

int lora_spi_bus_acquire(uint32_t timeout_ticks)
{
    expect(timeout_ticks == RADIO_OWNERSHIP_TIMEOUT_TICKS);
    expect(!bus_held);
    fake_tick += bus_wait_ticks;
    bus_wait_ticks = 0U;
    bus_held = 1;
    return 0;
}

void lora_spi_bus_release(void)
{
    expect(bus_held);
    bus_held = 0;
}

uint16_t MAX11128_ADC_ReadRawCH(MAX11128_t *adc, uint8_t channel)
{
    (void)adc;
    (void)channel;
    /* First 32 samples establish the baseline. Each later acquisition has
     * one breach followed by intact stripes. */
    adc_reads++;
    if (adc_reads <= (CLOUD_FACES * CLOUD_STRIPES)) {
        return 1000U;
    }
    return ((adc_reads - 33U) % (CLOUD_FACES * CLOUD_STRIPES) == 0U)
               ? 1600U
               : 1000U;
}

void MAX11128_Initialize(MAX11128_t *adc, SPI_HandleTypeDef *spi)
{
    (void)adc;
    (void)spi;
}

void MAX11128_ADC_Config(MAX11128_t *adc)
{
    (void)adc;
}

void MAX11128_ADC_Uni_Setup(MAX11128_t *adc)
{
    (void)adc;
}

int cyclic_buffer_write(const uint8_t *data, size_t len)
{
    (void)data;
    expect(len == sizeof(cloud_sample_t));
    expect(!bus_held);
    bus_released_before_fram = 1;
    fram_writes++;
    return 0;
}

void watchdog_alive_self(void) {}
int watchdog_register_task(osThreadId_t handle, uint32_t period_ms)
{
    (void)handle;
    (void)period_ms;
    return 0;
}

obw_state_t state_machine_get_state(void)
{
    return STATE_ACTIVE;
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
                         const osThreadAttr_t *attr)
{
    (void)func;
    (void)argument;
    (void)attr;
    return NULL;
}

osStatus_t osDelay(uint32_t ticks)
{
    (void)ticks;
    return osOK;
}

static void test_breach_history_and_spi_scope(void)
{
    cloud_sample_t first;
    cloud_sample_t second;

    cloud_init();

    fake_tick = 100U;
    bus_wait_ticks = 25U;
    expect(cloud_acquire(&first) == 1);
    expect(first.face[0].stripes[0].breached == 1U);
    expect(first.face[0].stripes[0].timestamp == 125U);
    expect(bus_released_before_fram == 1);
    expect(fram_writes == 1U);

    fake_tick = 200U;
    expect(cloud_acquire(&second) == 0);
    expect(second.face[0].stripes[0].breached == 1U);
    expect(second.face[0].stripes[0].timestamp == 125U);
    expect(fram_writes == 1U);
}

int main(void)
{
    test_breach_history_and_spi_scope();
    return 0;
}
