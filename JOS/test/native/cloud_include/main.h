#ifndef JOS_NATIVE_CLOUD_MAIN_H
#define JOS_NATIVE_CLOUD_MAIN_H

#include <stdint.h>

typedef struct { uint32_t opaque; } GPIO_TypeDef;
typedef struct { uint32_t opaque; } SPI_HandleTypeDef;

extern GPIO_TypeDef native_cloud_gpio_b;
extern SPI_HandleTypeDef hspi1;

#define GPIOB (&native_cloud_gpio_b)
#define GPIO_PIN_4 ((uint16_t)0x0010U)

#endif
