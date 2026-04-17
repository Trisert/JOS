#include "hal_compat.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HAL_COMPAT";

static struct {
    struct { uint8_t state[16]; } port[5];
} gpio_state;

static uint16_t adc_values[16] = {2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
                                   2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048};

void hal_compat_set_adc_value(uint8_t channel, uint16_t mv)
{
    if (channel < 16) {
        adc_values[channel] = mv;
    }
}

uint16_t hal_compat_get_adc_raw(uint8_t channel)
{
    if (channel < 16) {
        return (uint16_t)((adc_values[channel] * 4095) / 3000);
    }
    return 0;
}

static int port_to_idx(GPIO_TypeDef *port)
{
    if (port == GPIOA) return 1;
    if (port == GPIOB) return 2;
    if (port == GPIOC) return 3;
    if (port == GPIOE) return 4;
    return 0;
}

void HAL_Init(void)
{
    memset(&gpio_state, 0, sizeof(gpio_state));
}

void HAL_Delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

HAL_StatusTypeDef HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *cfg)
{
    int pidx = port_to_idx(port);
    if (pidx == 0) return HAL_ERROR;
    for (int i = 0; i < 16; i++) {
        if (cfg->Pin & (1 << i)) {
            gpio_state.port[pidx].state[i] = cfg->Mode;
        }
    }
    return HAL_OK;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, uint8_t state)
{
    int pidx = port_to_idx(port);
    if (pidx == 0) return;
    for (int i = 0; i < 16; i++) {
        if (pin & (1 << i)) {
            gpio_state.port[pidx].state[i] = state ? 1 : 0;
        }
    }
    ESP_LOGD(TAG, "GPIO Write port=%p pin=0x%04x state=%d", port, pin, state);
}

uint8_t HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    int pidx = port_to_idx(port);
    if (pidx == 0) return 0;
    for (int i = 0; i < 16; i++) {
        if (pin & (1 << i)) {
            return gpio_state.port[pidx].state[i] & 1;
        }
    }
    return 0;
}

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)
{
    (void)hspi;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hspi;
    (void)data;
    (void)len;
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hspi;
    memset(data, 0, len);
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t len, uint32_t timeout)
{
    (void)hspi;
    (void)tx;
    memcpy(rx, tx, len);
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t addr, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hi2c;
    (void)addr;
    (void)data;
    (void)len;
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t addr, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hi2c;
    (void)addr;
    (void)data;
    (void)len;
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t reg, uint16_t regsize, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hi2c;
    (void)addr;
    (void)reg;
    (void)regsize;
    (void)data;
    (void)len;
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t reg, uint16_t regsize, uint8_t *data, uint16_t len, uint32_t timeout)
{
    (void)hi2c;
    (void)addr;
    (void)reg;
    (void)regsize;
    (void)data;
    (void)len;
    (void)timeout;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *hadc, ADC_ChannelConfTypeDef *cfg)
{
    if (hadc && cfg) {
        hadc->Channel = cfg->Channel;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *hadc, uint32_t timeout)
{
    (void)hadc;
    (void)timeout;
    return HAL_OK;
}

uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *hadc)
{
    if (hadc && hadc->Channel < 16) {
        return hal_compat_get_adc_raw(hadc->Channel);
    }
    return 0;
}

HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    return HAL_OK;
}

void Error_Handler(void)
{
    ESP_LOGE(TAG, "Error_Handler called!");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
