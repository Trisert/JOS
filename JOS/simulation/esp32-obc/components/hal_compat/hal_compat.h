#ifndef HAL_COMPAT_H
#define HAL_COMPAT_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int HAL_StatusTypeDef;
#define HAL_OK           0
#define HAL_ERROR        (-1)
#define HAL_BUSY         (-2)
#define HAL_TIMEOUT      (-3)

typedef struct {
    void *Instance;
} SPI_HandleTypeDef;

typedef struct {
    void *Instance;
} I2C_HandleTypeDef;

typedef struct {
    void *Instance;
    uint32_t Channel;
} ADC_HandleTypeDef;

typedef struct {
    void *Instance;
} TIM_HandleTypeDef;

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} GPIO_InitTypeDef;

#define GPIO_MODE_OUTPUT_PP     1
#define GPIO_MODE_INPUT         0
#define GPIO_NOPULL             0
#define GPIO_SPEED_FREQ_LOW     0
#define GPIO_PIN_SET            1
#define GPIO_PIN_RESET          0

#define ADC_CHANNEL_4           4
#define ADC_CHANNEL_5           5
#define ADC_CHANNEL_8           8
#define ADC_CHANNEL_9           9
#define ADC_REGULAR_RANK_1      1
#define ADC_RESOLUTION_12B      12
#define ADC_DATAALIGN_RIGHT     0
#define ADC_SINGLE_ENDED        0
#define ADC_OFFSET_NONE         0

#define ADC_SAMPLETIME_2CYCLES_5   0
#define ADC_SAMPLETIME_247CYCLES_5 1

typedef struct {
    uint32_t Channel;
    uint32_t Rank;
    uint32_t SamplingTime;
    uint32_t SingleDiff;
    uint32_t OffsetNumber;
    uint32_t Offset;
} ADC_ChannelConfTypeDef;

typedef struct {
    void *Port;
    uint16_t Pin;
} GPIO_Pin_t;

#define GPIO_PIN_0   (1 << 0)
#define GPIO_PIN_1   (1 << 1)
#define GPIO_PIN_2   (1 << 2)
#define GPIO_PIN_4   (1 << 4)
#define GPIO_PIN_5   (1 << 5)
#define GPIO_PIN_10  (1 << 10)
#define GPIO_PIN_12  (1 << 12)
#define GPIO_PIN_14  (1 << 14)

#define GPIOC   ((GPIO_TypeDef *)0x01)
#define GPIOA   ((GPIO_TypeDef *)0x02)
#define GPIOB   ((GPIO_TypeDef *)0x03)
#define GPIOE   ((GPIO_TypeDef *)0x04)

typedef void GPIO_TypeDef;

void HAL_Init(void);
void HAL_Delay(uint32_t ms);

HAL_StatusTypeDef HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *cfg);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, uint8_t state);
uint8_t HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, uint8_t *data, uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *data, uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t len, uint32_t timeout);

HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t addr, uint8_t *data, uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t addr, uint8_t *data, uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t reg, uint16_t regsize, uint8_t *data, uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t addr, uint16_t reg, uint16_t regsize, uint8_t *data, uint16_t len, uint32_t timeout);

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *hadc, ADC_ChannelConfTypeDef *cfg);
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *hadc, uint32_t timeout);
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *hadc);

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_COMPAT_H */
