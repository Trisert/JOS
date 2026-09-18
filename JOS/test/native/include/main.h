#ifndef JOS_NATIVE_MAIN_H
#define JOS_NATIVE_MAIN_H

#include <stdint.h>

typedef struct GPIO_TypeDef { unsigned tag; } GPIO_TypeDef;
typedef struct {
    void *Instance;
    uint32_t State;
    struct { uint32_t BaudRatePrescaler; } Init;
} SPI_HandleTypeDef;
typedef struct { uint32_t LOAD; uint32_t VAL; } SysTick_Type;

extern GPIO_TypeDef native_gpio_a;
extern GPIO_TypeDef native_gpio_b;
extern GPIO_TypeDef native_gpio_c;
extern void *SPI1;
extern SysTick_Type *SysTick;
extern uint32_t SystemCoreClock;

#define GPIOA (&native_gpio_a)
#define GPIOB (&native_gpio_b)
#define GPIOC (&native_gpio_c)
#define GPIO_PIN_0 ((uint16_t)0x0001U)
#define GPIO_PIN_1 ((uint16_t)0x0002U)
#define GPIO_PIN_4 ((uint16_t)0x0010U)
#define GPIO_PIN_SET 1U
#define GPIO_PIN_RESET 0U
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_MODE_INPUT 2U
#define GPIO_NOPULL 0U
#define GPIO_SPEED_FREQ_HIGH 3U
#define HAL_SPI_STATE_RESET 0U
#define HAL_OK 0
#define HAL_ERROR 1
#define SPI_BAUDRATEPRESCALER_2 2U
#define SPI_BAUDRATEPRESCALER_4 4U
#define SPI_BAUDRATEPRESCALER_8 8U
#define SPI_BAUDRATEPRESCALER_16 16U
#define SPI_BAUDRATEPRESCALER_32 32U
#define SPI_BAUDRATEPRESCALER_64 64U
#define SPI_BAUDRATEPRESCALER_128 128U
#define SPI_BAUDRATEPRESCALER_256 256U

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} GPIO_InitTypeDef;

extern "C" {
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, uint32_t);
uint32_t HAL_GPIO_ReadPin(GPIO_TypeDef *, uint16_t);
int HAL_SPI_Init(SPI_HandleTypeDef *);
int HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *, uint8_t *, uint8_t *, uint16_t);
int HAL_SPI_Abort(SPI_HandleTypeDef *);
uint32_t HAL_RCC_GetPCLK2Freq(void);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t);
void __WFI(void);
void __NOP(void);
}

#define LoRa_NRST_GPIO_Port GPIOB
#define LoRa_NRST_Pin GPIO_PIN_1

#endif
